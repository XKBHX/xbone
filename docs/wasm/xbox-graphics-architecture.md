# How Xbox graphics works (and how xemu emulates it)

This document describes the original Xbox GPU (NVIDIA NV2A) and how this
repository emulates it. It covers what a new rendering backend has to
reproduce. All paths are relative to the repository root.

## 1. The hardware in one page

The NV2A is an NVIDIA GPU from the GeForce 3 / NV20 generation, built into
the Xbox chipset. A few properties drive almost every emulation decision:

- **Unified memory.** The Xbox has 64 MiB of RAM (128 MiB on Chihiro and
  devkits) shared by CPU and GPU. There is no separate VRAM. Render targets,
  textures, vertex buffers and push buffers all live in guest RAM at physical
  addresses. The CPU can read or write any of them at any time.
- **Command submission via push buffers.** The CPU writes command streams
  ("push buffers") into RAM and advances a `DMA_PUT` pointer. The GPU's
  **PFIFO** engine fetches commands and dispatches each *method* (register
  write) to the object bound on a *subchannel*.
- **Objects and classes.** Each subchannel is bound to an object instance in
  **RAMIN** (instance memory), found by handle through the **RAMHT** hash
  table. The class decides how methods are interpreted. xemu handles:

  | Class | Name | Role |
  | --- | --- | --- |
  | `0x97` | Kelvin (`NV097`) | The 3D engine: about 200 methods covering all render state, shaders, and draws. |
  | `0x62` | Context Surfaces 2D (`NV062`) | Source/destination description for 2D blits. |
  | `0x9F` | Image Blit (`NV09F`) | Rectangle copies between surfaces. |
  | `0x12` | Beta (`NV012`) | Blend factor for the blitter. |
  | `0x44` | Context Pattern (`NV044`) | Pattern color for 2D operations. |

- **Programmable but pre-DX9 pipeline.**
  - *Vertex*: either a fixed-function transform and lighting (T&L) unit
    (8 lights, fog, texgen, skinning, point params) or a **vertex program**
    of up to 136 instructions run on a 4-wide VLIW unit with 192 constant
    registers. This is the shader model that became D3D8's `vs.1.1`.
  - *Pixel*: **no general pixel shader**. Instead there are 4 *texture shader*
    stages that pick how each texture is addressed (projective, cube, bump
    environment map, dot-product, dependent reads, and so on), followed by 8
    **register combiner** stages plus a final combiner. This is NVIDIA's
    register combiner model, exposed as D3D8 `ps.1.x`.
  - *Fixed back end*: alpha test, depth/stencil, blending (including the
    signed add and reverse-subtract variants), color mask, fog, dithering,
    up to 8 window-clip rectangles, and multisample modes (2x horizontal,
    4x square).
- **Primitive types beyond modern APIs**: points, lines, line loops/strips,
  triangles, strips, fans, **quads, quad strips, and polygons**, plus
  polygon modes (fill, line, point) set separately for front and back faces,
  and a selectable provoking vertex.
- **Surface formats**: color `X1R5G5B5`, `R5G6B5`, `X8R8G8B8`, `A8R8G8B8`,
  `B8`, `G8B8`. Depth/stencil `Z16` and `Z24S8`, each in fixed or float
  form. Surfaces can be linear (pitched) or **swizzled** (Morton order).
- **Textures**: 2D, 3D (volume) and cube maps. Swizzled or linear. Formats
  include 16-bit packed formats, 8-bit paletted (`I8`), DXT1/3/5, luminance
  and alpha formats, and depth formats. Address modes are wrap, mirror,
  clamp-to-edge, border, and "clamp OGL".
- **Reports and semaphores**: occlusion queries ("ZPASS pixel count") are
  written back to RAM, and semaphore writes are used for CPU/GPU sync.
- **Display.** The **PCRTC** scans out a framebuffer address and raises
  VBLANK. The **PRAMDAC** handles video timing. **PVIDEO** is a hardware
  YUV overlay used by video playback. The external video encoder (Conexant,
  Focus or Xcalibur) is emulated on SMBus (`hw/xbox/smbus_*.c`).

## 2. Code map

```text
hw/xbox/
  xbox.c, xbox_pci.c        machine + chipset wiring
  nv2a/
    nv2a.c                  PCI device, MMIO block table, memory setup, VGA hooks
    nv2a_int.h, nv2a_regs.h device state, register and method definitions
    pfifo.c                 push-buffer fetch (pusher) + method dispatch (puller), RAMHT
    pmc.c ptimer.c pfb.c    misc engine blocks
    pcrtc.c pramdac.c       scanout / timing
    pvideo.c                overlay registers
    prm*.c user.c           VGA-compat and user-channel windows
    pgraph/
      pgraph.c / pgraph.h   PGRAPH engine: method handlers, register file, renderer selection
      methods.h.inc         Kelvin method table
      vertex.c              inline-array / inline-element / draw-arrays bookkeeping
      texture.c, swizzle.c, s3tc.c   format-agnostic texture decode helpers
      surface.h, texture.h  shape descriptors shared by renderers
      rdi.c                 RDI (internal RAM) access: vertex program, constants, etc.
      glsl/                 SHARED shader generator (NV2A state -> GLSL source)
      gl/                   OpenGL renderer
      vk/                   Vulkan renderer
      null/                 Null renderer (no output; reference for the minimal op set)
ui/xemu.c, ui/xui/          SDL3 + OpenGL + Dear ImGui front end
config_spec.yml             user config; display.renderer = NULL | OPENGL | VULKAN
```

## 3. Runtime flow

### 3.1 Memory and threads

`nv2a_init_memory()` in `hw/xbox/nv2a/nv2a.c` sets up memory:

- `d->vram` *is* guest system RAM (UMA). It is also exposed as PCI BAR 1.
- RAMIN is a separate 1 MiB RAM region mapped at MMIO `+0x700000`.
- Dirty logging is enabled on RAM with two NV2A-specific clients:
  `DIRTY_MEMORY_NV2A` (surfaces and vertex data) and `DIRTY_MEMORY_NV2A_TEX`
  (textures), defined in `include/exec/ramlist.h`. Renderers call
  `memory_region_test_and_clear_dirty()` to learn that the CPU touched GPU
  data, and `memory_region_set_client_dirty()` after the GPU writes to RAM.
- It starts **`nv2a.pfifo_thread`**. All PGRAPH work and all renderer API
  calls happen on this thread. The renderer's `init` is called from
  `pfifo_thread()` through `pgraph_init_thread()`.

Three execution contexts interact:

| Context | Does | Locks |
| --- | --- | --- |
| vCPU thread (TCG) | MMIO writes to PFIFO (`DMA_PUT`), RAM writes | BQL |
| PFIFO thread | fetches and dispatches methods, drives the renderer | `pfifo.lock`, `pgraph.lock` |
| Main/UI thread | composites the frame and ImGui, swaps | `pgraph.renderer_lock` while it holds the frame |

### 3.2 Command processing

`pfifo_thread()` (`pfifo.c`) loops:

1. `pgraph_process_pending()`: runs work other threads requested (flush,
   display sync, surface downloads), dispatched to `ops.process_pending`.
2. `pfifo_run_pusher()`: maps the DMA push buffer with `nv_dma_map()` and
   decodes method headers (incrementing, non-incrementing, jumps, calls).
3. `pfifo_run_puller()`: resolves `SET_OBJECT` handles through RAMHT, then
   calls `pgraph_method()`.
4. `pgraph_process_pending_reports()`: finishes occlusion query results.
5. Waits on `fifo_cond` until it is kicked again.

`pgraph_method()` (`pgraph.c`) loads the object context into the
`CTX_SWITCH*` registers, then branches on the graphics class. The 2D classes
are handled inline. `NV_IMAGE_BLIT` calls `ops.image_blit` when `SIZE` is
written. Kelvin methods go through a generated table
(`pgraph_kelvin_methods[]`, from `methods.h.inc`). Most handlers just
**write the PGRAPH register file** (`pgraph_reg_w`, which also sets a dirty
bit per register) or one of the state arrays in `PGRAPHState`:
vertex program (`program_data`), constants (`vsh_constants`), lighting
(`ltctxa`, `ltctxb`, `ltc1`), vertex attributes, and inline buffers.

### 3.3 The draw lifecycle

These are the points where renderer ops are called from `pgraph.c`:

| Guest action | Method(s) | Renderer op |
| --- | --- | --- |
| Change render target / DMA / pitch / format | `SET_CONTEXT_DMA_COLOR/ZETA`, `SET_SURFACE_*` | `surface_update(upload, color_write, zeta_write)` |
| Start primitive | `SET_BEGIN_END(prim)` | `draw_begin` |
| Feed vertices | `DRAW_ARRAYS`, `ARRAY_ELEMENT16/32`, `INLINE_ARRAY`, `SET_VERTEX_DATA*` | accumulate in `PGRAPHState` (`vertex.c`), with `flush_draw` when inline buffers fill up |
| End primitive | `SET_BEGIN_END(END)` | `draw_end`: the actual draw |
| Clear | `CLEAR_SURFACE` | `clear_surface` |
| 2D copy | `NV09F_SIZE` | `image_blit` |
| Occlusion | `CLEAR_REPORT_VALUE`, `GET_REPORT` | `clear_report_value`, `get_report` |
| Semaphore / flip | `BACK_END_WRITE_SEMAPHORE_RELEASE`, `FLIP_STALL` | `surface_update` (write back), `flip_stall` |

At `draw_end`, a backend:

1. Calls `pgraph_glsl_get_shader_state(pg)` to turn the relevant registers
   into a `ShaderState` key (`VshState` + `GeomState` + `PshState`).
2. Looks up or generates shaders (`pgraph_glsl_check_shader_state_dirty`),
   with LRU caches.
3. Builds the fixed-function key: blend, depth/stencil, cull, polygon offset,
   color mask, topology, vertex layout. Then binds or creates a pipeline.
4. Binds textures: decodes from RAM with `pgraph_get_texture_shape`,
   `pgraph_convert_texture_data`, `unswizzle_*` and `s3tc_decompress_*`, and
   caches them by content hash and RAM range (re-checked through
   `DIRTY_MEMORY_NV2A_TEX`).
5. Binds vertex data. Arrays in guest RAM are read directly from a GPU
   buffer that mirrors RAM (Vulkan: `pgraph_vk_update_vertex_ram_buffer`).
   Inline data is uploaded into streaming buffers. Formats the host can't
   consume, such as the packed `CMP` normals, are converted on the CPU
   (`VertexAttribute.needs_conversion`).
6. Issues the draw into the currently bound **surface** and marks it
   `draw_dirty`.

### 3.4 The surface cache (the hardest part)

Because render targets live in guest RAM, each backend keeps a **surface
cache** (`gl/surface.c`, `vk/surface.c`). Each entry maps a
`(RAM address, shape)` pair to a host render target.

- **Upload**: when a surface is bound and RAM under it is dirty (the CPU wrote
  it), RAM is converted (unswizzled if needed) and uploaded.
- **Download**: when the guest might read the result, the host image is read
  back, converted to the NV2A format, swizzled if needed, and written into
  RAM. That covers CPU reads, using the surface as a texture, display, save
  states, and semaphores. Vulkan does depth/stencil packing on the GPU with
  compute shaders (`vk/surface-compute.c`: D24S8 or D32F+S8 to and from
  `Z24S8`).
- **Surface as texture**: when a texture's address matches a cached surface,
  the backend samples the host image directly and skips the round trip
  through RAM.
- **Resolution scaling**: `surface_scale_factor` renders internally at N×.
  RAM still holds native-resolution data.
- **Deferred downloads**: Vulkan batches readbacks
  (`downloads_pending`, `download_dirty_surfaces_pending`) and does them in
  `process_pending`, so the PFIFO thread doesn't stall on every draw.
  WebGPU does the same: dirty surfaces in one batch share one submit and
  one `wgpuBufferMapAsync`.

### 3.5 Shader generation (`hw/xbox/nv2a/pgraph/glsl/`)

Both GPU backends share one generator that writes **GLSL source text**
with `MString` appends.

- `vsh.c`: vertex shader wrapper, fog, clipping, viewport, and Z/W
  perspective handling.
  - `vsh-ff.c`: fixed-function T&L (lighting, texgen, skinning) emitted as
    GLSL.
  - `vsh-prog.c`: translates NV2A vertex program microcode to GLSL.
- `psh.c`: translates texture shader stages and register combiners into a
  fragment shader. That includes alpha test, color key, signed formats,
  border and clamp handling, window clipping, and fog.
- `geom.c`: an optional **geometry shader**. `pgraph_glsl_need_geom()` turns
  it on for quads, quad strips, polygons, and for non-fill polygon modes.
  It also fixes flat-shading provoking-vertex differences and triangle
  winding (`tri_rot0/1` from `GPUProperties`).
- `common.c`: uniform declaration helpers. Uniform blocks are described with
  X-macros, so a backend gets a name, type, count and offset table.

Each generator takes an options struct with a single `bool vulkan` flag
(`GenVshGlslOptions`, `GenGeomGlslOptions`, `GenPshGlslOptions`) that
switches binding syntax and conventions.

Backend use:

- **OpenGL**: compiles the GLSL directly.
- **Vulkan**: compiles the GLSL to SPIR-V with **glslang** (`vk/glsl.c`) and
  reflects uniform layouts with **SPIRV-Reflect**. It requires the device
  features `geometryShader`, `fillModeNonSolid`, `depthClamp` and
  `shaderClipDistance` (see `vk/instance.c`). Quads are submitted as
  `LINE_LIST_WITH_ADJACENCY` and expanded in the geometry shader
  (`vk/draw.c`).

### 3.6 Display

- On flip, the guest points the PCRTC at a new framebuffer address.
- The UI thread (`gl_render_frame()` in `ui/xemu.c`) calls
  `nv2a_get_framebuffer_surface()`, which returns an **OpenGL texture name**
  (`int`).
  - **OpenGL backend**: returns its display texture.
  - **Vulkan backend**: asks the PFIFO thread to render the display image,
    applying the PCRTC/PVIDEO composite in `vk/display.c`. The image is
    exported with `VK_KHR_external_memory` and imported into GL with
    `glImportMemoryFdEXT`/`Win32HandleEXT`. **Without external memory**
    (`HAVE_EXTERNAL_MEMORY == 0`), it downloads the surface into guest RAM and
    returns `0`.
  - A return value of `0` makes the UI use the **VGA fallback**. It uploads
    the scanout region of guest RAM as a GL texture
    (`xb_surface_gl_create_texture`).
- The UI then draws the frame plus ImGui (HUD, menus, snapshots) with
  OpenGL and calls `nv2a_release_framebuffer_surface()`.

### 3.7 The renderer interface

`PGRAPHRenderer` in `pgraph/pgraph.h` is the full contract. Renderers
register themselves from a constructor:

```c
static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_xxx_renderer);
}
```

They are selected by `g_config.display.renderer`. `nv2a_context_init()` calls
every registered renderer's `early_context_init` on the main thread. `init`
runs later on the PFIFO thread. The Null renderer (`pgraph/null/renderer.c`)
is the minimal working example: it only has to acknowledge `sync_pending`
and `flush_pending` and write zero for reports.

Other details a backend has to respect:

- `process_pending` is entered with `pfifo.lock` held. It must drop that lock
  and take `pgraph.lock` before doing work, then restore the locks.
- `pre_savevm_trigger`/`pre_savevm_wait`: every dirty surface must be in RAM
  before a snapshot is taken.
- `set_surface_scale_factor`/`get_surface_scale_factor` are called from the
  UI with `renderer_lock` held.
- `get_gpu_properties`: host-specific quirks found at startup (geometry
  shader winding).
