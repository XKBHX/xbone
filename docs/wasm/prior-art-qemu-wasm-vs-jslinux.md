# Prior art: qemu-wasm-sample vs. JSLinux

This document compares two ways of running an x86 PC in a browser, and says
what each means for an xemu WASM build. The guest CPU options it informs are
in [cpu-realtime.md](cpu-realtime.md).

- **qemu-wasm-sample** by Kohei Tokunaga
  ([ktock/qemu-wasm-sample](https://github.com/ktock/qemu-wasm-sample)). A
  build and demo harness for upstream QEMU compiled with Emscripten, plus a
  WebAssembly TCG backend patch (maintained in `ktock/qemu-wasm#21` and sent
  upstream as the
  [v4 series](https://lists.nongnu.org/archive/html/qemu-devel/2026-01/msg04985.html)).
- **JSLinux** by Fabrice Bellard
  ([Windows 2000 demo](https://bellard.org/jslinux/vm.html?url=win2k.cfg&mem=192&graphic=1&w=1024&h=768),
  [technical notes](https://bellard.org/jslinux/tech.html),
  [FAQ](https://bellard.org/jslinux/faq.html)). A purpose-built PC emulator
  based on TinyEMU, written in C and compiled to WASM with Emscripten.

Statements below are sourced from those pages. Where a point is an inference
rather than something the authors state, it says so.

## 1. At a glance

| Aspect | qemu-wasm-sample | JSLinux |
| --- | --- | --- |
| Code base | Upstream QEMU (`x86_64-softmmu`), essentially unmodified apart from the TCG backend | TinyEMU core plus a custom x86 CPU emulator. The x86 core came from the 2011 JS version, then asm.js, then C again |
| Source availability | Open (QEMU plus patch; sample under Apache-2.0) | Only the RISC-V TinyEMU is published. The x86 emulator source isn't |
| CPU execution | TCI interpreter, plus a **JIT that emits wasm modules** for hot TBs | A tuned emulator in C compiled to WASM. The notes describe no dynamic code generation |
| Stated speed | No numbers in the cover letter or README | About 100 MIPS on a 2017 desktop with Firefox, with a `vmtime` benchmark tool |
| Guest CPU model | Whatever QEMU's x86 target provides | Its own 64-bit x86: bit-exact x87, PAE/NX, SVM with nested paging, SSE through AVX-512, APX. No segment limit checks and no debug registers |
| Devices | QEMU's full device models | Minimal: PIC, PIT, RTC, PCI, UART, VirtIO (console, 9P, net, block, input), simple framebuffer, and optional IDE, PS/2 and dummy VGA for Windows |
| Threads | Emscripten pthreads (`SharedArrayBuffer`). Needs COOP/COEP headers | Not documented. The site works as a plain page, which suggests no cross-origin isolation (inference) |
| Blocking and coroutines | Asyncify (QEMU coroutines, blocking helpers) | Not applicable. A small emulator loop can yield to the browser by design (inference) |
| Disk images | `file_packager.py`: preloaded into Emscripten's in-memory FS before boot | Fetched by URL from a `.cfg` VM description. TinyEMU supports HTTP-backed block devices and 9P remote filesystems |
| UI | Serial console through `xterm-pty` (`-nographic`) | Custom JS terminal, or a framebuffer GUI (`graphic=1`) |
| Networking | None in the sample | VirtIO net over a websocket VPN (capped at 40 kB/s) |
| Build target | wasm32 (emsdk 3.1.50) on `main`. wasm64, or wasm64 with a 32-bit address limit (emsdk 4.0.23), on `tcgdev64` | WASM via Emscripten |

## 2. CPU execution: JIT-to-wasm vs. a tuned emulator

**qemu-wasm** keeps QEMU's normal pipeline: the guest x86 front end
translates to TCG IR, then something executes it.

- Cold code runs on a forked **TCI**, which interprets the IR.
- Hot TBs are compiled by the new backend into **wasm bytecode**, wrapped
  in a `WebAssembly.Module`, instantiated, and called through the function
  table.
- The tiering exists because each instantiation is expensive and browsers
  cap the number of live instances (`MAX_INSTANCES`, lowered in v3 for
  Firefox).
- The generated code includes an inline soft-TLB lookup.

The upside is QEMU's full correctness and device coverage. The downsides
come from QEMU's architecture:

- IR is generated even for cold code.
- Calls cross module boundaries.
- Asyncify instrumentation.
- A multi-threaded runtime that needs cross-origin isolation.

**JSLinux** goes the other way: a smaller emulator designed for this
environment. Bellard rewrote the hand-written asm.js core as C and let
Emscripten compile it, and reports the result is faster than the asm.js
version after careful tuning. The notes mention no JIT, so the model looks
like a tuned emulator loop compiled ahead of time to WASM (inference). That
avoids every runtime cost of generating code in the browser:

- no module compiles
- no instance limits
- no table growth

The cost is that its speed is capped by an interpreter-style design.
Bellard reports about 100 MIPS (2017 hardware and browser).

**Contrast.** JSLinux shows how far a single, well-tuned emulator compiled to
WASM can go with a lean device model. qemu-wasm shows that QEMU's
general-purpose JIT can be retargeted to emit wasm at runtime. In principle
that has a higher ceiling for hot loops, but it pays heavy per-block setup
costs that native JITs don't.

## 3. System design: full QEMU vs. minimal PC

- **Device model.** qemu-wasm inherits all of QEMU's machine models. JSLinux
  emulates only what its guests need. Linux uses VirtIO almost exclusively.
  Windows 2000 ran first under QEMU inside the emulated Linux, using the
  emulator's SVM support. The current version instead adds the few legacy
  devices Windows needs: IDE, PS/2, and a dummy VGA plus a simple
  framebuffer.
- **Runtime model.** qemu-wasm needs Emscripten pthreads (vCPU, iothreads,
  RCU), `PROXY_TO_PTHREAD` and Asyncify. JSLinux's small design avoids that
  machinery as far as the published material shows.
- **Storage.** qemu-wasm-sample preloads images into memory, which is simple
  but costs download time and RAM in proportion to image size. JSLinux
  describes VMs by URL (`url=win2k.cfg`) and uses TinyEMU's remote block and
  filesystem support. Large disks don't have to be downloaded before boot.

## 4. What this means for xemu

1. **The CPU path has to be qemu-wasm's.** xemu is QEMU: its Xbox machine,
   NV2A, MCPX and the TCG x86 front end all depend on QEMU. The wasm TCG
   backend is the only one of the two approaches xemu can adopt directly. The
   JSLinux x86 core isn't available, and replacing QEMU's CPU would mean
   leaving the xemu codebase.
2. **JSLinux's number calibrates expectations.** About 100 MIPS from a tuned
   WASM emulator (2017) is well below what a 733 MHz Pentium III executes at
   full speed. A Pentium III retires around one instruction per clock in
   typical code, so that's hundreds of MIPS; this is an estimate. An
   interpreter-only design, even a very good one, is therefore unlikely to
   reach Xbox real time. That supports making a wasm JIT (the qemu-wasm
   route) the priority in [cpu-realtime.md](cpu-realtime.md), rather than
   tuning TCI.
3. **Adopt JSLinux's system-level ideas:**
   - **Lazy, chunked disk access** over HTTP or OPFS, instead of
     `file_packager` preloading. An Xbox HDD image is about 8 GB, and a game
     disc image is several GB, so preloading isn't viable.
   - **Lean devices for the browser profile.** Disable QEMU subsystems the
     Xbox machine doesn't need, to cut code size, Asyncify instrumentation
     and threads.
   - **Benchmark built in.** Ship a `vmtime`-style measurement, in line with
     the measurement plan in [cpu-realtime.md](cpu-realtime.md).
4. **Adopt qemu-wasm's runtime conventions:** COOP/COEP hosting for
   pthreads, Asyncify (or JSPI) for coroutines, and the emsdk version that
   the backend needs.
