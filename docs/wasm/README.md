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
WebAssembly TCG backend. The xemu UI has not been ported yet. A WebGPU NV2A
renderer is in progress natively (Dawn); see the status table in
[webgpu-renderer-plan.md](webgpu-renderer-plan.md#implementation-status).

What exists:

- `configure` maps the Emscripten toolchain to `host_os=emscripten`,
  `cpu=wasm32`; `meson.build` lists both as supported.
- `configs/meson/emscripten.txt` sets link flags: `-pthread`,
  `-sASYNCIFY=1`, `-sPROXY_TO_PTHREAD=1`, `-sTOTAL_MEMORY=2GB`,
  `-sWASM_BIGINT`, `-sEXPORT_ES6=1`, and others.
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
  and the surface scale factor. The composite is read back and uploaded to
  a GL texture for the existing UI. Every shader module is WGSL. Draw
  shaders are translated from the GLSL generator. Pack, unpack, clears,
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
  texture texels are expanded by a compute shader; palette, YUV, DXT and
  unswizzle stay on the CPU.

What blocks an xemu WASM build today:

- `epoxy` (OpenGL loader) is declared `required: true` in `meson.build`. The
  xemu UI (`ui/xemu.c`, `ui/xui/`) is SDL3 + OpenGL + Dear ImGui (OpenGL3
  backend) and is linked unconditionally.
- Both GPU renderers (`hw/xbox/nv2a/pgraph/gl`, `.../vk`) need desktop
  OpenGL. The Vulkan renderer still shows its frames through OpenGL by
  importing them with `GL_EXT_memory_object`.
- `subprojects/dsp56300` rejects `host_os=emscripten` at configure time.
- Guest CPU performance is not yet real time for a 733 MHz Pentium III. The
  wasm backend removes the interpreter, but still compiles one module per TB
  and returns to C between TBs. See [cpu-realtime.md](cpu-realtime.md).
