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
- `hw/xbox/nv2a/pgraph/webgpu` is an early WebGPU renderer
  (`-Dwebgpu=enabled`). On Emscripten it builds against the `emdawnwebgpu`
  port and is the only renderer offered. Natively it has the surface
  cache, clears, image blits, the draw path, textures and the
  download-to-RAM display path, and it renders the dashboard. It still
  compiles shaders through SPIR-V, so it can't draw under Emscripten until
  the WGSL generator exists.

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
