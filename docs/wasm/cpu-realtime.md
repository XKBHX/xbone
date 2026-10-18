# Running the guest CPU in real time under WebAssembly

The Xbox CPU is a single-core, 32-bit x86 Pentium III-class chip at
733 MHz, with MMX and SSE. Natively, xemu runs it with QEMU's TCG JIT, which
translates x86 into host machine code. Under Emscripten, `tcg/wasm` replaces
TCI unless the build is configured with `--enable-tcg-interpreter`. Guest
speed is still short of real time. This document explains where the time
goes, lists the options in order of payoff, and gives a measurement plan so
each option is judged by numbers. What is left is summarized in
[remaining-work.md](remaining-work.md).

## 1. Where the time goes today

1. **TCI interprets every TCG op,** when the build uses
   `--enable-tcg-interpreter`. The front end still translates guest x86
   into TCG IR, but instead of emitting host code, `tcg/tci.c` decodes and
   dispatches each IR op in a C loop. That costs a decode and an indirect
   branch per op, plus register-file traffic through memory. TCI is
   commonly an order of magnitude or more slower than a native TCG backend.
   The default wasm32 build uses `tcg/wasm` instead. Its remaining cost is
   one module per translation block and a return to C between blocks. See
   [tcg-wasm-backend.md](tcg-wasm-backend.md).
2. **Software MMU on every memory access.** Every guest load and store goes
   through the soft-TLB. Native backends inline the fast path. TCI pays a
   helper-style lookup for each access. `tcg/wasm` inlines that fast path.
3. **WebAssembly can't jump into code it generates.** A wasm module can't
   create and run new code inside itself. Generated code has to become a new
   `WebAssembly.Module` that JavaScript compiles and instantiates, so each
   compiled block costs a lot to bring up.
4. **Asyncify.** `configs/meson/emscripten.txt` links with `-sASYNCIFY=1`
   (used by `util/coroutine-wasm.c`). Asyncify instruments every function
   that might be on the stack during an unwind. That grows the code and slows
   ordinary execution on those paths.
5. **Other CPU-heavy subsystems.** These are smaller, but they compete for
   cores:
   - The NV2A path on the PFIFO thread: texture unswizzle and conversion,
     plus surface readbacks.
   - MCPX audio voice processing (`mcpx.apu_thread` and `mcpx.voice_worker`,
     sized by `audio.vp.num_workers`). The DSP56300 interpreter
     (`hw/xbox/mcpx/apu/dsp`) only runs when `audio.use_dsp` is enabled,
     which is off by default.

Hardware virtualization (KVM, HVF, WHPX) isn't available inside a browser, so
everything below is about making translation faster.

## 2. Options, in order of expected payoff

### 2.1 A WebAssembly TCG backend (highest payoff)

**Status: done in this tree.** xemu now has its own wasm32 backend in
`tcg/wasm`, described in [tcg-wasm-backend.md](tcg-wasm-backend.md). It
builds with the existing emsdk 3.1.50 image and needs no wasm64. It emits one
module per TB, inlines the soft-TLB fast path, keeps TCG registers in
WebAssembly locals, and cooperates with Asyncify so that coroutine switches
inside helpers still work. The upstream series below is kept for reference,
because its tiering ideas carry over to section 2.2.

Kohei Tokunaga's series
["wasm: Add Wasm TCG backend based on wasm64" (v4, January 2026)](https://lists.nongnu.org/archive/html/qemu-devel/2026-01/msg04985.html)
adds a real TCG backend that emits WebAssembly. As of this writing it has not
been merged upstream. Its design:

- **Tiered execution.** TBs (translation blocks) start on a forked TCI.
  Frequently executed TBs are compiled into wasm and instantiated through
  `WebAssembly.Module` / `WebAssembly.instantiate`. This limits compile
  overhead and stays under browsers' limit on the number of instances
  (`MAX_INSTANCES`).
- **wasm64 build** (`--cpu=wasm64`), with
  `--enable-wasm64-32bit-address-limit` for engines that only run wasm32.
- An inline TLB lookup in generated code, and coroutine switching from
  helpers.
- Requires emsdk 4.0.23. A sample setup is at
  [ktock/qemu-wasm-sample (tcgdev64)](https://github.com/ktock/qemu-wasm-sample/tree/tcgdev64).

What porting it into xemu would have involved (the reasons for writing a
wasm32 backend instead):

1. **Rebase friction.** The tree is QEMU `10.2.0` and still has 32-bit host
   support (`tcg/tci/tcg-target-reg-bits.h`). v4 assumes that support has been
   removed upstream. Either backport the 32-bit host removal, or port the
   series onto the v3 base.
2. **Build changes.**
   - Bump `tests/docker/dockerfiles/emsdk-wasm32-cross.docker` from emsdk
     3.1.50 to 4.0.23 or later.
   - Add a wasm64 cross file next to `configs/meson/emscripten.txt`.
   - Relax the `wasm32` requires-TCI check in `meson.build`.
3. **Guest target.** xemu builds `i386-softmmu`, and the Xbox is a 32-bit
   guest, so the backend's 64-bit guest support isn't needed. Benchmark both
   the full wasm64 build and the 32-bit-address-limit mode, because engines
   can add bounds-check overhead for Memory64.

### 2.2 Improve the wasm JIT for Xbox workloads (work on top of `tcg/wasm`)

- **Compile in the background.** Hand hot TB bytes to a dedicated compile
  worker that calls the asynchronous `WebAssembly.compile`. Post the compiled
  `WebAssembly.Module` back to the vCPU worker, which instantiates it against
  the shared memory. Modules can be posted between workers. The vCPU keeps
  running on TCI meanwhile, so compilation never stalls emulation.
- **Compile regions, not single TBs.** Put a hot loop or function made of
  several TBs into one module. Inside the module, use a `loop` + `br_table`
  dispatcher, so `goto_tb` between TBs in the region becomes a direct
  branch. Only region exits go back to the C dispatcher. This cuts per-module
  overhead and the instance count, and recovers most of the TB chaining that
  native backends get.
- **Specialize the soft-TLB fast path.** Xbox RAM is 64 MiB (128 MiB on
  Chihiro or a devkit) and most hot accesses hit RAM, not MMIO. Emit the
  inline TLB compare plus a direct load or store on the shared wasm memory,
  and keep helpers only for misses and I/O.
- **Code-cache flushes.** Self-modifying code and TB invalidation need to
  drop region modules cheaply. Track the guest pages each region covers and
  invalidate whole regions.

### 2.3 Cheap wins that help any backend

- **Replace Asyncify with JSPI** (`-sJSPI`) where the browser supports it,
  or narrow Asyncify with `ASYNCIFY_ONLY`/`ASYNCIFY_REMOVE` lists limited to
  the coroutine and blocking-wait paths. Measure both.
- **Optimize the release build.** Build with `-O3` and LTO (`-flto`) and run
  `wasm-opt` on the result. Make sure link-time optimization flags reach the
  final link. The v4 series includes a meson fix for exactly this.
- **Keep the vCPU worker dedicated.** `-sPROXY_TO_PTHREAD` is already set.
  Check that the PFIFO, APU and voice-worker threads land on their own
  workers. Use a fixed Emscripten pthread pool (`-sPTHREAD_POOL_SIZE`) so
  thread creation doesn't block, and keep the BQL hold times short on the
  vCPU path.
- **Lighter defaults for the browser build.** Leave `audio.use_dsp`
  off, cap `audio.vp.num_workers` to the available cores, and keep
  `display.quality.surface_scale = 1`.

### 2.4 Research directions (large, uncertain)

- **High-level emulation of XDK libraries.** Xbox titles statically link
  Microsoft's XDK (D3D8, DirectSound, XAPI). Identifying those functions by
  signature, as Cxbx-Reloaded does, and replacing hot ones with native or wasm
  implementations would remove a large share of guest instructions. This
  moves away from xemu's low-level-emulation accuracy model.
- **Ahead-of-time recompilation of XBE code to wasm,** using a static
  translator with a JIT fallback for indirect branches and self-modifying
  code. This could pay off for specific titles, but it's a separate project.

### 2.5 If in-browser real time is a hard requirement

Run native xemu on a server and stream video and input to the browser with
WebRTC. This gets real time today on suitable hardware, at the cost of
infrastructure and latency, and doesn't depend on any of the work above.

## 3. Measurement plan (do this first)

Every option above should be judged against the same workloads and metrics.

1. **Workloads.**
   - Boot to the dashboard, timed to a fixed screen.
   - A deterministic nxdk CPU benchmark XBE that prints iterations per
     second.
   - One or two commercial titles at a fixed save state, measuring
     frames per second over 60 seconds.
2. **Metrics.**
   - Guest speed as a percentage of real hardware (benchmark iterations per
     second against a known real-Xbox number).
   - Frames per second from the HUD.
   - TCG statistics from the `info jit` monitor command.
3. **Native baselines.** These need no browser.
   - Native xemu with the default TCG backend.
   - Native xemu configured with `--enable-tcg-interpreter`.

   The ratio between the two is the interpreter penalty on this exact tree.
4. **Browser runs.** Run the same workloads in Chrome with the wasm TCI
   build, then with the wasm TCG backend from 2.1, then with each
   improvement from 2.2 and 2.3 on its own.
5. **Exit criterion for "real time".** The CPU benchmark reaches 100% of
   real hardware, and the chosen titles hold their native frame rate on a
   stated reference machine.

## 4. Realistic expectations

Native xemu already needs a fast modern desktop CPU to run many titles at
full speed with a native JIT. A wasm JIT adds overhead on top of that:
compile and instantiate costs, structured control flow, bounds-checked
memory, and no direct use of host flags. So even with 2.1 to 2.3 done, real
time should be expected first for the dashboard and lighter titles on
high-end desktops, not across the library. The measurement plan exists to
replace that expectation with numbers early, before large investments in
2.2 or 2.4.
