# WebAssembly and WebGPU documentation

This directory holds all documentation related to running xemu on
WebAssembly (WASM) and to rendering Xbox graphics through WebGPU.

| Document | Purpose |
| --- | --- |
| [xbox-graphics-architecture.md](xbox-graphics-architecture.md) | How the original Xbox GPU (NV2A) works and how xemu emulates it today (OpenGL, Vulkan, Null renderers). |
| [webgpu-renderer-plan.md](webgpu-renderer-plan.md) | Plan for adding a WebGPU rendering path and a WebGPU UI, first natively (prebuilt Dawn) and then in the browser (Emscripten). |
| [prior-art-qemu-wasm-vs-jslinux.md](prior-art-qemu-wasm-vs-jslinux.md) | Comparison of QEMU-on-wasm (qemu-wasm-sample) with Fabrice Bellard's JSLinux, and what xemu should take from each. |
| [cpu-realtime.md](cpu-realtime.md) | How to get the guest x86 CPU to real time under WebAssembly (a wasm TCG backend, JIT improvements, measurement plan). |
| [tcg-wasm-backend.md](tcg-wasm-backend.md) | Design of xemu's native WebAssembly TCG backend (`tcg/wasm`), which replaces TCI on `wasm32`. |

These files are plain Markdown and are intentionally not part of the Sphinx
manual under `docs/` (which only builds `.rst`).

## Current WASM status in this tree

The tree inherits upstream QEMU's Emscripten host support and adds a native
WebAssembly TCG backend. When `display.renderer` is `WEBGPU`, the UI
thread records Dear ImGui, the logo, controller and memory-unit art, the
icon, the scaled gamma blit, snapshot thumbnails, and the VGA fallback.
The PFIFO thread plays that recording on the NV2A Dawn device and samples
the display composite directly. Natively the combined image is read back
into the OpenGL window. In the browser the same passes are recorded into
the page canvas. That page has to be cross-origin isolated, or the build's
pthreads never start. `wasm/serve.py` sends the headers; see the phase 5
browser step 5 notes in
[webgpu-renderer-plan.md](webgpu-renderer-plan.md#implementation-status).
A WebGPU NV2A renderer runs natively (Dawn); see the status
table in
[webgpu-renderer-plan.md](webgpu-renderer-plan.md#implementation-status).

What exists:

- `configure` maps the Emscripten toolchain to `host_os=emscripten`,
  `cpu=wasm32`; `meson.build` lists both as supported.
- `configs/meson/emscripten.txt` sets link flags: `-pthread`,
  `-sASYNCIFY=1`, `-sASYNCIFY_STACK_SIZE=65536`, `-sPROXY_TO_PTHREAD=1`,
  `-sOFFSCREENCANVAS_SUPPORT=1`, `-sASYNCIFY_IMPORTS=ffi_call_js,emwgpuWaitAny`,
  `-sTOTAL_MEMORY=2GB`, `-sWASM_BIGINT`, `-sEXPORT_ES6=1`, and others.
  `OFFSCREENCANVAS_SUPPORT` pulls in the GL library so Emscripten can
  transfer `#canvas`. The WebGPU present does not draw with WebGL.
- `os-wasm.c` and `include/system/os-wasm.h` are Emscripten replacements for
  `os-posix.c`. `util/coroutine-wasm.c` implements coroutines on Asyncify.
- `tests/docker/dockerfiles/emsdk-wasm32-cross.docker` builds the
  cross-compiled dependencies (zlib, libffi, glib, pixman) with emsdk 3.1.50.
- `tcg/wasm` is a native TCG backend that compiles each translation block to
  a WebAssembly module, with the soft-TLB fast path inline. `meson.build`
  selects it on `wasm32`; `--enable-tcg-interpreter` still selects TCI. See
  [tcg-wasm-backend.md](tcg-wasm-backend.md).
- `hw/xbox/nv2a/pgraph/webgpu` is a native WebGPU renderer
  (`-Dwebgpu=enabled`). On Emscripten it builds against the `emdawnwebgpu`
  port and is the only renderer offered. Natively it has the surface
  cache, clears, image blits, the draw path, textures, primitive assembly
  (polygon modes and the flat-shading provoking vertex), point size and
  point sprites, surface pack and unpack, copies of a render target into
  a texture, the PCRTC/PVIDEO display composite, ZPASS occlusion queries,
  and the surface scale factor. The composite is shared into a GL texture
  when the platform can import it (an IOSurface on macOS, a dma-buf on
  Linux, a DXGI shared handle on Windows). Otherwise it is read back. The
  WebGPU UI does not use that GL texture for an accelerated frame. It
  samples a separate `rgba8unorm` composite on the NV2A device. Every
  shader module is WGSL. Draw shaders are translated from the GLSL
  generator. Pack, unpack, clears,
  depth upload, texture upload, and the display composite are written in
  WGSL. The WebGPU
  backend does not link glslang. A shader the translator or Tint rejects
  is not drawn. Dawn's compiled pipelines are stored in
  `webgpu-pipeline-cache` under the xemu base path. A draw waits for its
  pipeline. Blend color and the stencil reference are dynamic state, so
  they do not create another pipeline. Surface readbacks are coalesced:
  several dirty surfaces are copied in one submit and mapped once, and
  only a surface a draw has dirtied is read back. Texture bind groups are
  cached by the views and samplers they name. A guest-RAM vertex buffer is
  uploaded in 64 KiB blocks and used when every attribute fits a core
  vertex format. Other attributes are still packed on the CPU. Packed
  texture texels are expanded by a compute shader. Palette, YUV, and
  unswizzle stay on the CPU. DXT is uploaded as BC1, BC2, or BC3 when the
  device has `texture-compression-bc` and the texture is 2D or a cube
  whose base size is a multiple of 4. Otherwise DXT is decompressed on
  the CPU. `depth32float-stencil8`, `texture-compression-bc`, and
  `depth-clip-control` are optional. `XEMU_WEBGPU_DISABLE_FEATURES` forces
  the fallback. See the phase 5 browser step 4 notes in
  [webgpu-renderer-plan.md](webgpu-renderer-plan.md#implementation-status).
- `tests/wasm/webgpu-offscreen-spike` is a standalone emdawnwebgpu program,
  built with emsdk 6.0.12. A pthread other than the page thread creates a
  device and clears a transferred `OffscreenCanvas`. It is not an xemu
  build. `tests/wasm/webgpu-canvas-present` links the same `canvas.c` the
  renderer uses and draws a stand-in scanout plus a UI quad into that
  canvas. A pending `wgpuInstanceWaitAny` suspends through Asyncify.
  The same present test linked with JSPI suspends that way instead.
  xemu stays on Asyncify because its coroutine fibers use Asyncify
  stacks. See the phase 5 browser notes in
  [webgpu-renderer-plan.md](webgpu-renderer-plan.md#implementation-status).
- `wasm/serve.py` serves the browser launcher, `wasm/index.html`, with
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`. Those headers are what
  enable `SharedArrayBuffer` and pthreads. The server also sends
  `Cross-Origin-Resource-Policy: cross-origin`, and it sets `.js` to
  `text/javascript` and `.wasm` to `application/wasm`. The launcher will
  not start unless the page is a secure context and `crossOriginIsolated`.
  `python3 -m http.server` leaves `SharedArrayBuffer` disabled.
  `python3 wasm/serve.py --check` checks the headers without a browser.

`build/wasm/xemu/qemu-system-i386.js` links with the local emsdk 6.0.12,
WebGPU enabled, and OpenGL disabled. The browser UI still includes epoxy,
so the link pulls in `libepoxy` and Emscripten's `-lGL`. The Rust DSP JIT
has no wasm32 archive, so this build uses the C interpreter. Guest CPU
performance is not yet real time for a 733 MHz Pentium III. The wasm
backend removes the interpreter, but still compiles one module per TB and
returns to C between TBs. See [cpu-realtime.md](cpu-realtime.md).
