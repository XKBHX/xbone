# What is left

The renderer and browser-present work in
[webgpu-renderer-plan.md](webgpu-renderer-plan.md) is implemented. Phases 0
through 5 are marked done in that plan's
[implementation status](webgpu-renderer-plan.md#implementation-status).
What remains is proving the renderer, finishing the browser host, and making
the guest CPU fast enough to play. The phase 5 exit says browser performance
is reported, not gated, until the CPU work in
[cpu-realtime.md](cpu-realtime.md) lands.

What already runs is summarized in [README.md](README.md).

## Renderer exit checks

Each phase's implementation steps are done. The exit check written for that
phase has not been run.

| Phase | Exit still open |
| --- | --- |
| 1. Native bring-up | nxdk sample programs, and the basic `nxdk_pgraph_tests` cases (clears, simple geometry, texture formats), compared with Vulkan. The dashboard matches Vulkan on macOS arm64. |
| 2. Feature completeness | Full `nxdk_pgraph_tests` parity with Vulkan, within an agreed per-test tolerance. A curated set of commercial titles boots and renders. Save states round-trip. The PVIDEO overlay was not compared. Polygon point mode was not shown on screen. |
| 4. Performance | Frame times within about 20% of Vulkan on the same machine for the curated titles. Frame times have not been compared. |
| 5. WebGPU UI and browser | The native window presents on macOS arm64 (`bgra8unorm`, no validation errors) and was not compared by eye. Linux and Windows were not booted, for the window or for the dma-buf and DXGI display share. Feature fallbacks forced with `XEMU_WEBGPU_DISABLE_FEATURES` were not booted. |

The testing plan in
[webgpu-renderer-plan.md](webgpu-renderer-plan.md#6-testing-strategy) is
still open as well. Golden-image CI on SwiftShader is described there and
is not running. Windows CI still needs a MinGW build of Dawn, because the
official release is MSVC and the Windows build cross-compiles with MinGW.
`display.webgpu.validation` matches `display.vulkan.validation_layers`.
It is off by default. When it is set, Dawn's backend validation level is
Full (Metal shader validation, Vulkan validation layers, or the D3D debug
layer) and the `skip_validation` device toggle is disabled. In the browser
the flag logs that the browser validates commands and that Dawn backend
layers are not available. On macOS arm64, a boot of the pgraph comparison
disc with the flag set logged `Backend validation enabled` and
`Presenting to the window (2560x1920, bgra8unorm)`, and recorded no
validation error. The window picture was not compared by eye.

A Chromium boot of the bundled disc, on macOS arm64, draws an nxdk pgraph
test frame and the menu bar on the same canvas. That log also records two
defects:

- Chromium rejects a store into a vector swizzle (`fragColor.xyz =`,
  `oPos.xy *=`, `oPos.xyzw =`). Dawn's Tint accepts those stores, so a
  native boot did not show this. The translator now writes the whole
  vector. `tests/xbox/wgsl` covers those three assignments. A Chromium
  boot of this translator has not been run. A shader the translator or
  the browser still rejects is skipped, and the test frame is drawn
  anyway.
- A ZPASS report still cannot return the pixel count `16448`. Dawn's
  Metal backend records `MTLVisibilityResultModeBoolean`, so a passing
  occlusion query comes back as `1`. That 0-or-1 result is no longer
  divided by `surface_scale` squared, so a pass stays `1` when the scale
  is greater than 1. The WebGPU spec only requires an occlusion query to
  tell zero from non-zero. A backend that returns a real sample count is
  still divided, as in Vulkan.

Documented accuracy gaps that are still the behavior of the renderer:

- In line and point polygon modes, index rewriting gives each edge or
  point the flat attributes of its own first vertex. The geometry shaders
  give every edge and point the flat attributes of its triangle's
  provoking vertex. When the front and back polygon modes differ, the
  front mode is used and `NV2A_UNIMPLEMENTED` is logged. Line and point
  modes do no face culling.
- Point quads follow triangle coverage. Fractional sizes below about 2
  pixels can cover different pixels than Vulkan's point rasterization.
  A point whose centre is outside the viewport but whose quad overlaps it
  is partly drawn. Vulkan discards that point by the centre.
- `clip-distances` is detected and left off. Clip planes stay a fragment
  discard. Without `depth-clip-control`, the vertex shader clamps clip Z.
- On the `depth24plus-stencil8` fallback, stencil readback is 0. Z16 falls
  back to `depth32float`, and Z24S8 falls back to `depth24plus-stencil8`
  plus an `r32float` depth copy, when `depth32float-stencil8` is missing.
- Palette, YUV, and unswizzle stay on the CPU. `R6G5B5` is expanded to
  3-byte snorm on the CPU, then the texture-upload kernel writes
  `rgba8snorm`. Attributes WebGPU cannot fetch, and inline vertices, stay
  on the CPU packer.
- `TitleInfo` in `ui/xui/scene-components.cc` is still compiled out
  (`#if 0`).

## Browser host

These sit outside the renderer. They are the phase 5 prerequisites that
are still open, plus what the current browser link actually does.

- The browser link does not include `libepoxy`. OpenGL and gloffscreen
  are left out of that build. The UI includes `GLES3/gl3.h` there, and
  desktop-only GL entry points that the WebGPU path does not call are
  empty inline stubs so the fallback still compiles. Emscripten's
  `-lGL` stays, because `OFFSCREENCANVAS_SUPPORT` needs it to transfer
  `#canvas`. The link also sets `-sMAX_WEBGL_VERSION=2` so the GLES3
  calls that remain in the UI (`glUniform1ui`) resolve. The WebGPU
  present does not draw with WebGL.
- The launcher copies a user-supplied MCPX, BIOS, HDD, EEPROM, and disc
  into Emscripten's in-memory filesystem before boot. The same files are
  stored in the origin private file system, so the next visit can start
  without choosing them again. Guest writes to the HDD and EEPROM are
  copied back when xemu exits. The page also copies them when it is
  hidden, when the tab closes, and once a minute while xemu is running.
  A crash can still drop writes made after the last copy. The images
  still reside in memory while xemu runs.
- `tests/docker/dockerfiles/emsdk-wasm32-cross.docker` is still emsdk
  3.1.50. The local browser build uses emsdk 6.0.12. Xbox dependencies
  (SDL3, libslirp, and others) are not in that image. `emdawnwebgpu` needs
  the newer SDK.
- The Rust DSP JIT has no wasm32 archive, so the browser build uses the C
  interpreter.

`build/wasm/xemu/qemu-system-i386.js` does link, with WebGPU enabled and
OpenGL disabled. [tcg-wasm-backend.md](tcg-wasm-backend.md) records an
earlier validation, run on upstream QEMU because xemu could not link for
the web then. That sentence is history. The current link is the one in
[README.md](README.md).

## Guest CPU

Real time for a 733 MHz Pentium III is a separate project. `tcg/wasm`
replaces TCI on `wasm32`. It still compiles one module per translation
block and returns to C between blocks. Guest CPU performance is not real
time. The order below is the one in
[cpu-realtime.md](cpu-realtime.md).

1. **Measure it first.** Boot to the dashboard, timed to a fixed screen.
   Run a deterministic nxdk CPU benchmark that prints iterations per
   second. Run one or two commercial titles from a fixed save state and
   record frames per second over 60 seconds. Compare native TCG, native
   TCI, the wasm TCG backend, and each later improvement on its own.
   The exit is 100% of real hardware on the benchmark, and the chosen
   titles holding their native frame rate on a stated reference machine.
   Even with the items below done, real time should be expected first for
   the dashboard and lighter titles on high-end desktops.
2. **Improve the wasm JIT** ([cpu-realtime.md](cpu-realtime.md) §2.2).
   Compile hot regions of several translation blocks into one module, in
   a background worker, and keep running cold code on TCI or the baseline
   tier until that module is ready. Inside the module, `goto_tb` becomes
   a direct branch. Invalidate whole regions when the code buffer is
   flushed. The soft-TLB fast path is already inlined in `tcg/wasm`.
3. **Cheap wins** ([cpu-realtime.md](cpu-realtime.md) §2.3). Narrow
   Asyncify with `ASYNCIFY_ONLY` or `ASYNCIFY_REMOVE`, limited to the
   coroutine and blocking-wait paths. JSPI was timed on the present test
   and was not faster on those short waits. The xemu link stays on
   Asyncify because coroutine fibers use Asyncify stacks. That narrowing
   has not been done. The release build is still `-O2` without LTO.
   Emscripten runs `wasm-opt` as part of an `-O2` link. The emulator link
   pre-creates `(navigator.hardwareConcurrency||4)+8` workers.
   `c_thread_count` and `cpp_thread_count` are 0 so Meson does not append
   its own pool of 4 after that expression. The launcher writes
   `display.quality.surface_scale` 1, `audio.use_dsp` false, and
   `audio.vp.num_workers` at one per core, capped at 16.
4. **Ops the backend does not emit.** TCG expands these generically:
   `extract2`, `divs2` and `divu2`, 64-bit `mulu2`, `muls2`, `muluh` and
   `mulsh`, 128-bit guest memory ops, 64-bit guest addresses, and xemu's
   native x87 FPU ops. WebAssembly `f64` is a later fit for the FPU ops.
   `mb` emits nothing. That is correct for one vCPU. Real multi-vCPU
   support would need WebAssembly atomics.

High-level emulation of XDK libraries, and ahead-of-time translation of
XBE code to wasm, are research directions in
[cpu-realtime.md](cpu-realtime.md) §2.4. They are not the next step.
Streaming a native xemu instance to the browser is the fallback in §2.5
if in-browser real time is a hard requirement before that work lands.
