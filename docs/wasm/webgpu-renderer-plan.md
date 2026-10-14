# Plan: an alternate NV2A rendering path using WebGPU

Read [xbox-graphics-architecture.md](xbox-graphics-architecture.md) first.
This plan assumes you know the PFIFO/PGRAPH flow, the surface cache, and the
shared GLSL generator.

## 1. Goals and non-goals

Goals:

1. Add a fourth renderer, `WEBGPU`, next to Null, OpenGL and Vulkan. It
   plugs into the existing `PGRAPHRenderer` ops table and needs no changes
   to `pgraph.c` method handling.
2. Write it once against the standard C API (`webgpu.h`) so the same code
   runs:
   - **natively**, through [Dawn](https://dawn.googlesource.com/dawn), on
     Vulkan, D3D12 and Metal. This is the development, debugging and CI
     target.
   - **in the browser**, through Emscripten's `emdawnwebgpu` port, as part of
     a WASM build of xemu.
3. Reach visual parity with the Vulkan renderer on the
   [nxdk_pgraph_tests](https://github.com/abaire/nxdk_pgraph_tests) suite.

Non-goals for this plan:

- Making the guest CPU fast enough in the browser. Under WASM, xemu runs x86
  code through the TCG interpreter (TCI); see [README.md](README.md). That is
  a separate project. A WebGPU renderer is worth having natively even if
  browser performance stays poor.
- Replacing the OpenGL or Vulkan renderers.

## 2. Key design decisions

| Decision | Choice | Why |
| --- | --- | --- |
| API surface | Standard `webgpu.h` only. Dawn-native extensions are allowed only behind `#ifndef __EMSCRIPTEN__` | One codebase for both targets. |
| Native implementation | Dawn | Dawn ships the Emscripten port (`emdawnwebgpu`), the WGSL compiler (Tint), and native SPIR-V ingestion, which helps bring-up. |
| Threading | The device is created and used **only on the PFIFO thread**, like the Vulkan backend | Matches the existing locking model. In the browser, WebGPU objects can't move between workers, so this is mandatory there. |
| Shader language | **Bring-up only**: reuse the GLSL generator, compile to SPIR-V with the existing glslang code, and feed SPIR-V to Dawn natively. **Then a native WGSL generator** (phase 3), built before the performance work | Gets pixels quickly while keeping the long-term path free of translation layers. The browser only accepts WGSL, and no Tint has to ship inside the WASM binary. |
| Primitives that need geometry shaders | CPU index rewriting, plus vertex pulling for point sprites | WebGPU has no geometry stage, no quads, no fans, and no polygon mode. |
| Display (native) | Start with the existing "download to RAM, return `0`" fallback | This is exactly what Vulkan does without external memory (`vk/renderer.c`). The UI needs no changes. |
| Display and UI (browser) | **The UI moves to WebGPU.** One device and one canvas (`OffscreenCanvas`), both owned by the PFIFO worker. ImGui draw data is built on the UI thread and handed to the PFIFO thread, which records the UI pass after the game frame | Avoids WebGPU↔WebGL interop, which browsers don't offer, and the game frame never leaves the GPU. WebGPU objects can't cross workers, so the UI pass has to be recorded on the device's thread. |
| Native CI dependency | **Prebuilt Dawn**, pinned by version and checksum | Building Dawn from source makes CI much slower. A pinned prebuilt keeps builds reproducible. |

## 3. WebGPU gap analysis

This section lists each place where NV2A emulation needs something that
WebGPU's core spec doesn't provide, and the strategy for each. "Optional
feature" means it must be requested at device creation and may be missing.
Each one needs a fallback.

### 3.1 Primitive assembly (replaces `glsl/geom.c`)

| NV2A need | WebGPU | Strategy |
| --- | --- | --- |
| Quads, quad strips | none | Rewrite indices to a triangle list on the CPU. For non-indexed draws, synthesize an index buffer. |
| Polygons, triangle fans | no fan topology | Triangle-list fan expansion. |
| Line loops | none | Line strip plus a closing index. |
| Polygon mode line/point | fill only | Expand triangles into edge or point index lists. Culling then has to happen on the CPU or in a vertex-pulling pass. Wireframe is rare, so start CPU-only. The current generator already asserts that front mode equals back mode. |
| Provoking vertex (flat shading) | WGSL `@interpolate(flat)` uses the **first** vertex | During list expansion, rotate each triangle's indices so the NV2A provoking vertex comes first. Strips with last-vertex provoking get converted to lists. |
| Winding fix-ups (`tri_rot0/1`) | — | Apply them in the same index rewrite. |
| Strip primitive restart | always on for indexed strips | Use `uint32` indices for strips, so `0xFFFF` is never treated as a restart. Or convert strips to lists. |
| Point sprites / point size > 1 | 1-pixel points only | **Vertex pulling.** Draw 6 vertices per instance, one instance per point. The vertex shader reads attributes from a storage buffer, then expands the point in clip space. Particles are common in games, so this needs to land in phase 2. |
| Wide lines | 1-pixel lines only | Accept the difference (the Vulkan backend treats `wideLines` as optional in `vk/instance.c`). |

Implement this as a standalone, unit-testable module
(`webgpu/prim.c`) with no GPU dependency. It maps
`(primitive_mode, polygon_mode, provoking, index source)` to
`(WGPUPrimitiveTopology, index buffer)`.

### 3.2 Vertex fetch

| NV2A need | WebGPU | Strategy |
| --- | --- | --- |
| `UB_D3D` (BGRA bytes) | `unorm8x4-bgra` | Direct. |
| `UB_OGL`, `S1`, `S32K`, `F` with 2 or 4 components | covered by core formats | Direct. |
| 3-component 8/16-bit, `CMP` (11:11:10) | no 3-component 8/16-bit formats | CPU conversion through the existing `needs_conversion` path. Later, decode in a vertex-pulling shader. |
| Odd strides or offsets | stride must be a multiple of 4; offsets aligned to `min(4, size)` | CPU repack, or fall back to vertex pulling. |
| Arrays read from guest RAM | — | One `GPUBuffer` mirrors guest RAM (64 or 128 MiB). Upload dirty ranges with `wgpuQueueWriteBuffer`, driven by `DIRTY_MEMORY_NV2A`, like `pgraph_vk_update_vertex_ram_buffer`. Check `maxBufferSize`, whose default is 256 MiB. |

### 3.3 Textures and samplers

| NV2A need | WebGPU | Strategy |
| --- | --- | --- |
| `R5G6B5`, `A1R5G5B5`, `A4R4G4B4` | no packed 16-bit formats | Expand to `rgba8unorm` on upload. Start on the CPU in `pgraph_convert_texture_data`, then move to a compute shader. |
| `R8G8B8_SNORM` | no 3-component formats | `rgba8snorm`. |
| `R16_UNORM` | optional feature | Use `r16unorm` when available. Otherwise use `r16uint` and normalize in the shader, or convert to `r32float`. |
| Component swizzle (`VkComponentMapping` in `vk/constants.h`) | no view swizzle in core | Bake the swizzle into the fragment shader (the shader key already depends on texture state), or apply it during upload conversion. |
| Paletted `I8`, DXT1/3/5 | BC formats need an optional feature | Palette: decode on upload, as the existing backends do. DXT: use `texture-compression-bc` when available, otherwise `s3tc_decompress_*`. Browsers on mobile GPUs often lack BC. |
| `BORDER` and `CLAMP_OGL` address modes | no clamp-to-border | Use a clamp-to-edge sampler, and do the coordinate test and border-color select in the generated shader. |
| LOD bias in the sampler | not in the sampler | `textureSampleBias` in the shader. |
| Sampling in non-uniform control flow | WGSL rejects implicit-derivative sampling there | Hoist the samples, use `textureSampleGrad`, or emit `diagnostic(off, derivative_uniformity)`. |
| 3D textures, cube maps, anisotropy | supported | Direct. |

### 3.4 Render targets and the surface cache

| NV2A need | WebGPU | Strategy |
| --- | --- | --- |
| 16-bit color surfaces | can't render to 565/1555 | Render to `rgba8unorm`. On download, pack in a compute pass that writes a storage buffer, then copy it to a map-readable buffer. |
| `B8`, `G8B8` surfaces | `r8unorm`, `rg8unorm` | Direct, with swizzle handling. |
| `Z24S8` / `Z16` read back to RAM | `depth24plus*` depth can't be copied | Prefer `depth32float-stencil8` (optional feature, widely available), which mirrors Vulkan's `D32_SFLOAT_S8` path. Pack in compute: `textureLoad` on `texture_depth_2d`, and read stencil through a stencil-aspect view as `texture_2d<u32>`. This is a WGSL port of `vk/surface-compute.c`. Fallback: `depth24plus-stencil8` plus a depth-to-color copy pass. |
| Upload of depth from RAM | `depth32float` can't be a buffer copy destination | Use a fullscreen draw that writes `@builtin(frag_depth)`, with stencil written through a stencil-reference loop or a stencil8 copy. |
| Swizzled surfaces | — | Reuse `swizzle_box`/`unswizzle_box` on the CPU, or later port them to compute. |
| Resolution scaling | — | Same model as Vulkan: allocate at N×, then scale down in the pack pass. |
| Anti-aliasing modes | — | Already supersampled by `pgraph_apply_anti_aliasing_factor`. No host MSAA needed. |

### 3.5 Fixed-function state

| NV2A need | WebGPU | Strategy |
| --- | --- | --- |
| Blend factors and equations | supported. The signed variants don't exist | Map them the way `pgraph_blend_equation_vk_map` does (approximated). |
| Logic op | none | Not needed. Vulkan only uses `COPY`. |
| Depth clamp | optional `depth-clip-control` (`unclippedDepth`) | Fallback: clamp in the shader and write `frag_depth`. |
| User clip distances | optional `clip-distances` | Fallback: `discard` in the fragment shader. Window clipping already works this way. |
| Polygon offset | `depthBias`, `depthBiasSlopeScale` and `depthBiasClamp` are pipeline state | Include them in the pipeline key. |
| Push constants | none in core | A uniform ring buffer with dynamic offsets. |
| Dynamic state | only viewport, scissor, blend constant and stencil reference are dynamic | Everything else goes into `PipelineKey`, as Vulkan already does. Use `wgpuDeviceCreateRenderPipelineAsync` and an LRU cache. |

### 3.6 Reports and synchronization

| NV2A need | WebGPU | Strategy |
| --- | --- | --- |
| ZPASS pixel counts | occlusion query sets. The spec only guarantees zero vs. non-zero | Use `WGPUQueryType_Occlusion`. Native Dawn backends generally report real sample counts; verify this per backend in the phase 2 tests. Document the gap for implementations where results are binary. |
| Synchronous surface download (CPU reads, semaphores, save states) | `wgpuBufferMapAsync` only | Natively: `wgpuInstanceWaitAny` with a timeout. Browser: the wait needs Asyncify (already enabled by `configs/meson/emscripten.txt`) or JSPI to yield to the worker's event loop. Batch and defer downloads like `downloads_pending` in Vulkan, so each frame pays for at most a few waits. |

## 4. Proposed code layout

```text
hw/xbox/nv2a/pgraph/webgpu/
  meson.build
  renderer.c / renderer.h   PGRAPHRenderer ops table, PGRAPHWgpuState
  instance.c                adapter and device creation, feature/limit probing, GPUProperties
  command.c                 encoder and pass lifecycle, submit, finish(reason)
  buffer.c                  ring buffers (uniform, index, inline vertex), guest-RAM mirror
  prim.c                    primitive expansion (no GPU dependency; unit tested)
  vertex.c                  vertex layouts, CPU conversion and repack
  texture.c, constants.h    NV2A format to WGPUTextureFormat table, upload conversion, cache
  sampler.c                 sampler cache plus border/clamp shader flags
  surface.c                 surface cache: bind, upload, download, scale
  surface-compute.c         WGSL pack/unpack kernels (color 16-bit, Z24S8, Z16)
  shaders.c                 ShaderState-to-module cache, bind group layouts, uniform packing
  draw.c                    PipelineKey, pipeline cache, draw_begin/draw_end, clear
  blit.c                    NV09F image blit
  reports.c                 occlusion queries, report write-back
  display.c                 PCRTC/PVIDEO composite, presentation (readback or canvas)
  debug.c                   labels, debug groups, validation hooks
hw/xbox/nv2a/pgraph/wgsl/   (phase B) WGSL emitter for vsh/psh, sharing state extraction with glsl/
```

How the `PGRAPHRenderer` ops map to the Vulkan implementation they model:

| Op | WebGPU implementation (modelled on) |
| --- | --- |
| `early_context_init` | none (no main-thread context needed) |
| `init` / `finalize` | `instance.c`, plus `init_*` for each module (`vk/renderer.c`) |
| `surface_update` | `surface.c` (`pgraph_vk_surface_update`) |
| `draw_begin` / `draw_end` / `flush_draw` / `clear_surface` | `draw.c` (`vk/draw.c`) |
| `image_blit` | `blit.c` (`vk/blit.c`) |
| `clear_report_value` / `get_report` / `process_pending_reports` | `reports.c` |
| `flip_stall` | `command.c` finish + submit |
| `process_pending` | deferred downloads, display sync and flush (`pgraph_vk_process_pending`) |
| `pre_savevm_*` / `pre_shutdown_*` | download every dirty surface and signal the `QemuEvent` |
| `get_framebuffer_surface` | native phase 1: download and return `0`. Browser: present, return `0` |
| `set/get_surface_scale_factor` | `surface.c` |
| `get_gpu_properties` | `instance.c` (constant winding, since there is no geometry shader) |

## 5. Phased plan

Each phase ends with a demonstrable exit criterion.

### Phase 0: groundwork and refactors (small, landable on its own)

1. **Config and build plumbing.**
   - `config_spec.yml`: add `WEBGPU` to `display.renderer.values`.
   - `meson_options.txt`: add `option('webgpu', type: 'feature', value: 'auto')`.
   - `meson.build`: find a **prebuilt Dawn** natively through `pkg-config`
     or `cmake` (`dependency('dawn')` with a `webgpu_dawn` fallback name), and
     point it at a pinned release archive in CI. On Emscripten, add
     `--use-port=emdawnwebgpu` to compile and link arguments. Set
     `CONFIG_WEBGPU`.
   - `hw/xbox/nv2a/pgraph/meson.build`: `subdir('webgpu')`.
   - `pgraph.h`: forward-declare `PGRAPHWgpuState` and add it to the
     renderer-state union.
   - `get_default_renderer()`: lowest priority natively, and the only choice
     on Emscripten.
   - Add Dawn's license file under `licenses/`.
2. **Skeleton renderer.** Copy `null/renderer.c`, create a device, and log the
   adapter and features. **Exit**: `display.renderer = "WEBGPU"` boots the
   dashboard with a black screen and no asserts.
3. **Generalize generator options.** Replace `bool vulkan` in
   `GenVshGlslOptions`, `GenGeomGlslOptions` and `GenPshGlslOptions` with an
   enum target `{ GL, VULKAN, WEBGPU }`. The WebGPU GLSL variant must not use
   push constants, must use explicit `set`/`binding` numbers that match the
   WebGPU bind-group plan, must not emit a geometry stage, and must gate
   `gl_ClipDistance` on the feature being present.
4. **Factor out glslang** from `vk/glsl.c` into a backend-neutral helper, so
   WebGPU can reuse GLSL-to-SPIR-V without linking Vulkan.

### Phase 1: native bring-up with Dawn (medium)

1. Command lifecycle: one encoder per batch, and a render pass per surface
   binding. `finish(reason)` ends the pass, submits, and optionally waits.
2. Surface cache with **CPU-side conversion only** at first. Support
   `A8R8G8B8`/`X8R8G8B8` color and `Z24S8` through `depth32float-stencil8`.
   Implement upload, download and dirty tracking, mirroring `vk/surface.c`.
3. `clear_surface`, using load-op clear when possible and a clear draw
   otherwise.
4. Draw path for triangles and triangle strips: shader state, then GLSL, then
   SPIR-V, then `WGPUShaderSourceSPIRV` (Dawn-native). Add the pipeline cache,
   uniform ring buffer, guest-RAM vertex mirror, and inline vertex buffers.
5. Textures: linear and swizzled `A8R8G8B8`, `X8R8G8B8`, DXT and paletted
   formats through the existing CPU decoders.
6. Display: download to RAM and return `0` from `get_framebuffer_surface`.

**Exit**: the dashboard and nxdk sample programs render correctly. Basic
nxdk_pgraph_tests (clears, simple geometry, texture formats) match Vulkan.

### Phase 2: feature completeness (large)

1. `prim.c`: quads, quad strips, polygons, fans, line loops, polygon modes,
   provoking-vertex rotation and winding. Write host-side unit tests first,
   with no GPU (see section 6).
2. Vertex pulling for point sprites and point size, plus odd formats and
   strides.
3. The full texture format table, including swizzle-in-shader, border and
   clamp emulation, LOD bias, `R16_UNORM` and `R8G8B8_SNORM` fallbacks, and
   cube and 3D textures.
4. 16-bit color surfaces, `Z16`, swizzled surfaces, and the compute pack and
   unpack kernels (port `vk/surface-compute.c` to WGSL).
5. Surface-as-texture without a RAM round trip.
6. `image_blit`, PVIDEO overlay, and the PCRTC display composite
   (`vk/display.c`).
7. Occlusion reports, semaphores, `pre_savevm_*` (snapshots must work), and
   surface scale factor.

**Exit**: nxdk_pgraph_tests parity with Vulkan within an agreed per-test
tolerance. A curated set of commercial titles boots and renders. Save states
round-trip.

### Phase 3: WGSL generator (large; comes before performance work)

1. Add `pgraph/wgsl/` emitters for the vertex shader (fixed-function and
   program) and pixel shader (texture shaders and combiners). They consume the
   same `VshState`/`PshState` keys. Keep the GLSL path as a **differential
   oracle**: render the same test with both and compare images.
2. Write the surface pack/unpack compute kernels and the display composite
   directly in WGSL.
3. Use `WGPUShaderSourceWGSL` everywhere, and remove the SPIR-V and glslang
   dependency from the WebGPU backend.

**Exit**: the WebGPU backend runs natively with WGSL only and keeps phase 2
parity.

### Phase 4: performance and native display fast path (medium)

1. Asynchronous pipeline creation, with a placeholder or skipped draw while a
   pipeline compiles. Dawn's blob cache gives a persistent pipeline cache.
2. Fewer stalls: deferred and coalesced downloads, and readbacks only when
   `DIRTY_MEMORY_NV2A` or a semaphore actually requires them.
3. Bind group caching keyed by texture and sampler set. Coarser
   `wgpuQueueWriteBuffer` ranges for the RAM mirror.
4. Move upload conversion to compute shaders.
5. Native display without OpenGL interop: once the WebGPU UI from phase 5
   also runs natively, the game frame and UI share one device and the
   readback fallback is only needed for the OpenGL UI. Until then, export the
   display texture with Dawn `SharedTextureMemory` (dmabuf / opaque FD, D3D
   shared handle, IOSurface) and import it into GL with the same
   `glImportMemory*EXT` calls `vk/display.c` uses.

**Exit**: frame times within about 20% of Vulkan on the same machine for the
curated titles.

### Phase 5: WebGPU UI and browser target (large; depends on the wider WASM port)

Prerequisites outside the renderer, tracked separately:

- Make `epoxy`/OpenGL optional in `meson.build` when the WebGPU UI is
  selected.
- Provide a file system for user-supplied BIOS, MCPX and HDD images (OPFS or
  `FS`).
- Add Xbox dependencies (SDL3, libslirp, and others) to
  `tests/docker/dockerfiles/emsdk-wasm32-cross.docker`, and bump emsdk past
  the pinned 3.1.50. `emdawnwebgpu` and the wasm TCG backend
  ([cpu-realtime.md](cpu-realtime.md)) both need emsdk 4.x.

UI port to WebGPU (build it natively first, then reuse it in the browser):

1. Switch ImGui from its OpenGL3 backend to `imgui_impl_wgpu`. The imgui
   wrap points at the `xemu-project/imgui` fork, so the WebGPU backend must
   be enabled in that fork's meson options.
2. Port `ui/xui/gl-helpers.cc` to WebGPU. It makes about 145 direct OpenGL
   calls for the logo, controller art, display scaling and framebuffer blit.
   Also port its shaders in `ui/shader/*`, rewritten in WGSL.
3. Port the remaining direct GL users: `ui/xemu.c` (`gl_render_frame`,
   the VGA fallback upload), `ui/xemu-snapshots.c`, `ui/xemu-thumbnail.cc`,
   `ui/xui/main-menu.cc`, `ui/xui/scene-components.cc` and
   `ui/xui/compat.cc`. Snapshot thumbnails come from a WebGPU readback.
4. Threading: the UI thread still runs ImGui logic and builds `ImDrawData`.
   It posts a deep copy (vertex/index buffers, draw commands, texture IDs) to
   the PFIFO thread with a `ui_render_pending` flag, handled in
   `process_pending` like `sync_pending`. The PFIFO thread records the game
   composite and the UI pass into the same canvas texture and presents.
   Texture IDs in the UI become handles into a PFIFO-side texture table,
   because WebGPU objects can't be shared across threads in the browser.

Browser renderer work:

1. **Spike first.** Confirm that `emdawnwebgpu` can create and use a device
   from an Emscripten pthread (the PFIFO worker), and that a transferred
   `OffscreenCanvas` can be configured as a WebGPU canvas context in that
   worker.
2. Presentation: in `display.c`, render the PCRTC composite into the canvas's
   current texture, followed by the UI pass. `get_framebuffer_surface` is
   no longer used on this path.
3. Blocking waits: route `wgpuInstanceWaitAny` through Asyncify, or JSPI where
   available. Measure the cost, and reduce the number of synchronous
   downloads per frame.
4. Feature detection and fallbacks for browsers that lack
   `depth32float-stencil8`, `texture-compression-bc`, `clip-distances` or
   `depth-clip-control`.
5. Hosting requirements: cross-origin isolation (`COOP: same-origin`,
   `COEP: require-corp`) for `SharedArrayBuffer` and pthreads.

**Exit**: natively, the xemu UI runs entirely on WebGPU with no OpenGL. In
the browser, an nxdk test XBE renders in Chrome with the game and UI on one
WebGPU canvas. Performance is reported, not gated, until the CPU work in
[cpu-realtime.md](cpu-realtime.md) lands.

## 6. Testing strategy

- **Host unit tests (no GPU)** for `prim.c` and format conversion: build the
  expanded index list from known inputs and compare it with expected
  triangles, provoking vertex included. They go next to the existing
  `tests/xbox/` tests.
- **nxdk_pgraph_tests golden images.** Run each test with the Vulkan and
  WebGPU backends and diff the images with a per-test threshold. Track a
  parity dashboard by test category: texture formats, combiners, fog, point
  sprites, and so on.
- **Differential shader testing** in phase 3: compare GLSL→SPIR-V output
  with native WGSL output on the same `ShaderState`.
- **Validation.** Run Dawn with backend validation and the WebGPU error
  scopes (`wgpuDevicePushErrorScope`) enabled in debug builds. A
  `display.webgpu.validation` config flag would mirror
  `display.vulkan.validation_layers`.
- **CI.** Download a pinned prebuilt Dawn release (version and SHA-256 kept
  in the CI config, with matching archives for Linux, Windows and macOS).
  Build the native WebGPU backend against it. On Linux, run headless on
  SwiftShader through Dawn's Vulkan backend, with unit tests plus a small
  golden-image subset. Bumping Dawn is a deliberate PR that updates the pin
  and reruns the golden images.

## 7. Risks and open questions

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Guest CPU speed under TCI in the browser | Browser build unplayable regardless of the GPU path | See [cpu-realtime.md](cpu-realtime.md). Justify native WebGPU on its own merits: portability, an easier-to-debug API, and an OpenGL-free UI. |
| WebGPU UI port scope | Many direct GL call sites outside ImGui | Port and ship it natively first, behind a config switch, before depending on it in the browser. |
| Prebuilt Dawn ABI drift | `webgpu.h` changes between Dawn releases | Pin one release. Isolate Dawn-specific calls in `instance.c` and `display.c`. |
| Synchronous readbacks in the browser | Large Asyncify overhead, frame hitches | JSPI, batched downloads, and avoiding readback for display. |
| No geometry shaders | Point-sprite and wireframe correctness | `prim.c` plus vertex pulling. Wireframe culling is a known limitation at first. |
| Pipeline compile stutter (pipelines are immutable) | Hitching on first use | Async creation, persistent cache, and pre-warming from a recorded key list. |
| Optional features missing on some adapters | Formats, depth readback, clip | Every optional feature has a core-only fallback (section 3). Test the fallbacks explicitly by disabling the features. |
| Occlusion counts binary on some browsers | Lens flares and visibility effects differ | Accept and document. Compare against Vulkan to quantify the impact. |

Decisions made:

1. **Build the WGSL generator**, as phase 3, before the performance work.
   Tint will not be shipped inside the WASM binary.
2. **Move the UI to WebGPU** (ImGui `imgui_impl_wgpu` plus the xui GL
   helpers). The UI pass is recorded on the PFIFO thread, in the same device
   and canvas as the game frame.
3. **Use prebuilt Dawn in CI**, pinned by version and checksum.
