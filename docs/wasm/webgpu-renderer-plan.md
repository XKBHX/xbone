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
| Primitives that need geometry shaders | CPU index rewriting, plus instanced quads for point sprites | WebGPU has no geometry stage, no quads, no fans, and no polygon mode. |
| Display (native) | Composite on the GPU, read the image back, and upload it to a GL texture on the UI thread | There is no shared-memory import yet (phase 4). The UI already displays a returned GL texture, so it needs no changes. Without OpenGL, fall back to the VGA read of guest RAM. |
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
| Point sprites / point size > 1 | 1-pixel points only | **Instanced quads** (phase 2 step 2). Draw a 4-vertex triangle strip per instance, one instance per point, with the packed vertex buffer stepped per instance. The vertex shader expands the point in clip space. This replaced the original vertex-pulling plan because it needs no storage buffers. |
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
hw/xbox/nv2a/pgraph/wgsl/   WGSL translation of the WebGPU-targeted GLSL vertex and pixel shaders
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
| `get_framebuffer_surface` | native: composite, read back, return a GL texture. No color surface, or no OpenGL: download and return `0`. Browser canvas present is phase 5 |
| `set/get_surface_scale_factor` | `surface.c` |
| `get_gpu_properties` | `instance.c` (constant winding, since there is no geometry shader) |

## 5. Phased plan

Each phase ends with a demonstrable exit criterion.

### Implementation status

| Phase | Status |
| --- | --- |
| 0. Groundwork | Done. Natively on macOS arm64 (Dawn Metal backend, Apple M1 Pro), `display.renderer = "WEBGPU"` creates a device and boots with a black screen and no asserts. Not yet run on Linux or Windows. |
| 1. Native bring-up | All six steps are done, plus the NV09F image blit. On macOS arm64 the dashboard renders fully textured with no WebGPU validation errors, and matches the Vulkan renderer side by side. The exit check against nxdk sample programs and the basic nxdk_pgraph_tests has not been run yet. Step 4 originally needed a Dawn built with the SPIR-V reader. Phase 3 step 3 removed that requirement. |
| 2. Feature completeness | Steps 1–7 are done. Step 1 is primitive assembly, with host unit tests in `tests/xbox/prim`. Step 2 is point size and point sprites. Step 3 is textures (border colour, LOD bias, `x8y24` sampling, CPU texture updates, and the format table checked against hardware). Step 4 is the WGSL pack and unpack kernels for 16-bit colour, `Z16` and `Z24S8`. Step 5 copies a compatible render target into a texture on the GPU. Step 6 is the PCRTC/PVIDEO composite, read back into a GL texture for the existing UI. The NV09F image blit was already in phase 1. Step 7 is ZPASS occlusion queries, the surface scale factor, and the snapshot download. NV2A semaphore release already downloads dirty surfaces before the semaphore is written. The phase 2 exit check (full nxdk_pgraph_tests parity, commercial titles, and a save-state round trip) has not been run. |
| 3. WGSL generator | Steps 1–3 are done. Step 1 translates draw shaders from the WebGPU-targeted GLSL. Step 2 did not need new shaders: the pack and unpack kernels and the display composite were already WGSL in phase 2. Step 3 creates every shader module with `WGPUShaderSourceWGSL`. The WebGPU backend no longer links glslang and does not request `ShaderSourceSPIRV`. glslang remains for Vulkan. A translation or Tint failure skips that draw. |
| 4. Performance | Step 1 keeps Dawn's blob cache under the xemu base path in `webgpu-pipeline-cache`. A draw waits for its pipeline, so compilation does not drop geometry. Blend color and the stencil reference stay dynamic and are not part of the pipeline key. Steps 2–5 are not started. Frame times have not been compared with Vulkan. |
| 5. WebGPU UI and browser | Not started. |

Phase 0 notes:

- Dawn is pinned to `v20261013.193955` in `scripts/download-dawn.py`
  (SHA-256 per archive). CI installs it on Linux x86_64 and macOS
  x86_64/arm64 and builds with `--enable-webgpu`. Meson finds Dawn
  through `pkg-config` (`dawn`), then CMake (`Dawn`, target
  `dawn::webgpu_dawn`), then `webgpu_dawn`. The official release only
  ships CMake files, so `download-dawn.py` also writes a `dawn.pc`; CI
  uses it through `PKG_CONFIG_PATH`, because `build.sh` configures Linux
  and macOS as cross builds, where Meson's CMake probing doesn't work
  reliably. When Dawn comes from CMake on macOS, `meson.build` adds the
  `-framework` entries Meson drops from Dawn's link interface.
- The prebuilt macOS archives are built without Tint's SPIR-V reader
  (`TINT_BUILD_SPV_READER` follows `DAWN_ENABLE_VULKAN`), which the
  GLSL → SPIR-V shader path needs. `scripts/build-dawn.py` builds the same
  pinned revision from source with the reader enabled and installs it with
  the same layout and `dawn.pc`, for example
  `scripts/build-dawn.py --source build/dawn-src --output build/dawn-sdk-spv`,
  then `meson configure -Dcmake_prefix_path=$PWD/build/dawn-sdk-spv` (or put
  its `lib/pkgconfig` on `PKG_CONFIG_PATH`). A Release build takes about
  15 minutes on an M1 Pro.
- `webgpu/instance.c` creates the instance with `TimedWaitAny`, requests the
  adapter and device with `WGPUCallbackMode_WaitAnyOnly` futures, and logs
  the adapter and its features with a `[WebGPU]` prefix.
- The generator option is now `PgraphGlslTarget target`
  (`PGRAPH_GLSL_TARGET_GL`, `_VULKAN`, `_WEBGPU`). For WebGPU, uniform
  blocks use `layout(set = 0, binding = N)`. Push constants are opt-in
  (`use_push_constants_for_uniform_attrs`) and the generator never emits
  `gl_ClipDistance`, so neither needs extra gating. Texture samplers still
  use `layout(binding = N)` with the implicit set 0; the phase 1 bind-group
  layout decides whether textures move to their own set.
- glslang lives in `glsl/spirv.c` (`pgraph_glsl_compile_to_spirv`) and is
  linked for Vulkan. Meson looks for it when Vulkan is found. The WebGPU
  backend no longer compiles SPIR-V (phase 3 step 3). The WebGPU target of
  the GLSL generator still emits the Vulkan 1.1 style that the translator
  accepts; Vulkan keeps Vulkan 1.3 / SPIR-V 1.6.
- The Emscripten configure step still stops at
  `subprojects/dsp56300/meson.build` (`Unsupported host OS: emscripten`).
  This is outside the renderer and tracked with the phase 5 prerequisites.

Phase 1 notes (steps 1, 2, 3 and 6):

- `webgpu/command.c` keeps one open `WGPUCommandEncoder` per batch, plus
  at most one render pass. `pgraph_wgpu_finish(reason, wait)` ends the pass,
  submits, and can wait on `wgpuQueueOnSubmittedWorkDone`. `flip_stall`
  waits; every other reason only submits. `wgpuQueueWriteTexture` runs
  before any commands that haven't been submitted yet, so every upload
  submits the open encoder first. Surface textures are only released, never
  destroyed, because an unsubmitted encoder may still reference them.
- The device now requires `depth32float-stencil8` (the adapter's feature
  list is checked first). Uncaptured errors and device loss are logged with
  the `[WebGPU]` prefix.
- `webgpu/surface.c` is a port of `vk/surface.c`: the same binding,
  eviction, expiry and CPU-access-callback logic. Format conversion runs on
  the CPU for now:
  - `A8R8G8B8`/`X8R8G8B8` use `bgra8unorm`, `B8` uses `r8unorm` and `G8B8`
    uses `rg8unorm`. These are byte-identical copies.
  - `R5G6B5` and `X1R5G5B5` are expanded to `rgba8unorm`. On download they
    are rounded back down, and alpha becomes the X1 bit at ≥ 128, as with
    Vulkan's `A1R5G5B5`.
  - `Z24S8` and `Z16` both use `depth32float-stencil8`. Z24 uses the same
    conversion as `vk/surface-compute.c` (pack `int(d * 0xffffff)`; unpack
    `z / 2^24` plus one ULP). Z16 uses `z / 65535` and round-to-nearest.
  - Downloads copy the colour, or the depth and stencil aspects separately,
    into one growable `MapRead` buffer (rows aligned to 256 bytes), then
    wait with `wgpuInstanceWaitAny`.
  - Depth aspects can't be copy destinations, so a depth upload writes an
    `r32float` staging texture and draws a fullscreen triangle that outputs
    `@builtin(frag_depth)`. Stencil is written directly through a
    stencil-aspect `wgpuQueueWriteTexture`.
  - Phase 2 step 4 moves this conversion into the compute pack/unpack
    pass. Phase 4 item 4 is separate: it is the texture upload conversion.
- Clears always use a fullscreen-triangle draw: blend constant colour, the
  channel write mask, the viewport depth range set to the clear depth, and
  stencil `Replace` with the reference value. The scissor is the clear
  rectangle. Pipelines are cached by format, write mask and depth/stencil
  write. A load-op clear for full-surface clears is a later optimisation.
- `blit.c` is the CPU-side NV09F blit from `vk/blit.c`, unchanged.
  `get_framebuffer_surface` waits for the scanout surface to download and
  returns `0`, so the UI shows guest RAM through the VGA path.
  Resolution scaling is fixed at 1×.
- **Shader-path finding.** The prebuilt Dawn macOS
  archives for `v20261013.193955` are built without Tint's SPIR-V reader.
  The library contains the string "Tint SPIR-V reader is not enabled", and
  the bundled `tint` CLI rejects SPIR-V input. The Linux archive does include
  the reader. So the GLSL → SPIR-V → `WGPUShaderSourceSPIRV` bring-up path
  can't run on macOS with the pinned prebuilt. The alternatives are
  building Dawn from source with `TINT_BUILD_SPV_READER` for development, or
  moving the phase 3 WGSL generator ahead of the draw path. Step 4 takes the
  first option and keeps the WGSL generator in phase 3.

Phase 1 notes (step 4, draw path):

- Step 4 compiled shaders through SPIR-V, so it needed a Dawn with Tint's
  SPIR-V reader. `scripts/build-dawn.py` builds one at the pinned revision.
  Modules were created with Dawn's `allowNonUniformDerivatives`, because
  GLSL allows implicit derivatives in non-uniform control flow. Phase 3
  step 3 removed that path: the renderer no longer probes SPIR-V at init
  and does not request `ShaderSourceSPIRV`. Fragment WGSL turns the same
  allowance into `diagnostic(off, derivative_uniformity)`.
- Tint rejects some SPIR-V that Vulkan accepts, so the WebGPU target of the
  generator also:
  - replaces `isnan`/`isinf` with bit-pattern tests, because WGSL can't
    represent NaN;
  - derives `INFINITY` from `gl_VertexIndex`, because WGSL can't represent
    infinite constants and the SPIR-V optimizer would fold `1.0 / 0.0`;
  - flips `gl_Position.y`, because WebGPU NDC is y-up.
- Uniforms: one bind group (group 0) holds the vertex and pixel uniform
  blocks, bound at dynamic offsets into a per-batch uniform stream. The
  std140 offsets are computed from the generator's X-macro tables rather
  than from SPIR-V reflection, and values are hashed so that unchanged
  blocks aren't uploaded again. Textures are in group 1. Each combination of
  sampler types gets its own bind group layout and pipeline layout, and
  until step 5 every stage is bound to a 1×1 zeroed dummy texture.
- Streams: the uniform, index and vertex streams are staged on the CPU and
  written with `wgpuQueueWriteBuffer` right before each submit. A stream that
  runs out of space finishes the batch and grows to the next power of two.
- **No guest-RAM vertex mirror yet.** WebGPU allows at most 8 vertex buffers
  and has no 3-component 8/16-bit or scaled formats. So each draw packs its
  active attributes on the CPU into one interleaved buffer:
  - `UB×3` becomes `unorm8x4`, and `S1×3` becomes `snorm16x4`;
  - `S32K` is converted to float;
  - `CMP` stays `sint32` and is decoded in the shader.

  Surfaces that overlap the vertex data are downloaded first. The VRAM
  mirror with dirty tracking moves to phase 4.
- Phase 2 primitive expansion was pulled forward, because WebGPU has no
  fans, quads or polygons. `prim.c` turns fans, polygons, quads, quad
  strips and line loops into index lists. Triangle and line lists, strips,
  and points draw directly.
- Pipelines are cached on the shader binding, attachment formats, topology,
  vertex layout and the raw register words that feed fixed-function state.
  WebGPU requires a depth attachment whenever the fragment shader writes
  depth, so a draw with no zeta surface uses a scratch `depth24plus`
  attachment (compare Always, no writes).
- Known gaps: flat-shading provoking vertex and polygon modes (both handled
  since phase 2 step 1), point sprites
  and point size (handled since phase 2 step 2), `x8y24` uint sampling
  (handled since phase 2 step 3), and culling of both faces (those draws
  are skipped). Constant-alpha blend factors use the blend constant
  with alpha in every channel. Emscripten can't use this path until the
  phase 3 WGSL generator exists.

Phase 1 notes (step 5, textures):

- `webgpu/texture.c` is a port of `vk/texture.c`. It has the same
  1024-entry LRU cache, keyed on the texture shape, address, length,
  palette, filter, address mode and anisotropy. It uses the same
  `DIRTY_MEMORY_NV2A_TEX` page tracking with content hashing. The flush op
  marks every cached texture as possibly dirty, as in Vulkan.
- Decoding runs on the CPU. The shared decoders in `pgraph/texture.c`
  handle palette, YUV and `R6G5B5`, `s3tc_decompress_*` handles DXT1/3/5,
  and `unswizzle_rect`/`unswizzle_box` handle swizzled 2D, cube and 3D
  textures. Then `kelvin_color_format_wgpu_map` turns every format into a
  core WebGPU format:
  - `A8R8G8B8`, `I8`, `A8B8G8R8`, DXT and YUV are copied unchanged into
    `bgra8unorm` or `rgba8unorm`. The X8Y24 depth formats are copied
    unchanged into `r32uint`.
  - Every other colour format (the 16-bit packed formats, `Y8`, `A8`,
    `G8B8`, `R8B8` and the reordered 32-bit formats) is expanded to
    `rgba8unorm`. `R6G5B5` becomes `rgba8snorm`.
  - Core WebGPU has no view swizzle, so each format's component mapping
    (the `VkComponentMapping` in `vk/constants.h`) is applied while
    decoding. This includes the depth flag channels that the shadow-map
    code reads.
  - `Y16` and the Y16 depth formats become `rgba32float` when
    `float32-filterable` is available (requested at device creation), and
    `rgba16float` otherwise. Norm16 formats aren't used, because Dawn only
    makes them filterable with a Dawn-specific feature.
- Uploads use `wgpuQueueWriteTexture`, one call per layer and mip level.
  Queue writes run before commands that haven't been submitted yet, so when
  a texture's contents change it gets a new `WGPUTexture`. The old one is
  only released, never written. Draws don't stall.
- Each cache entry owns its sampler. Address modes map directly, except
  `BORDER` and `CLAMP_OGL`, which clamp to edge. Anisotropy is only
  enabled when every filter is linear, as WebGPU requires. X8Y24 textures
  use a non-filtering sampler.
- The texture bind group (group 1) is rebuilt only when the bound textures
  or the shader's texture layout change. A stage is bound to the dummy of
  the declared type when its texture is disabled, or when its type
  (2D, 2D uint, 3D, cube) doesn't match what the shader declares.
- A compatible render target is copied into the texture on the GPU (phase 2
  step 5). Any other dirty surface in the texture's range is still downloaded
  first, as Vulkan does for incompatible surfaces.
- Border textures are allocated at the doubled border size that the shader's
  border adjustment expects.
- Not yet handled: border colour and LOD bias (both handled since phase 2
  step 3), the signed-channel filter bits (also unimplemented in Vulkan)
  and convolution filters. They belong to phase 2 step 3. Phase 2 step 3
  checked the formats, depth, cube and 3D paths on screen.

Phase 2 notes (step 1, primitive assembly):

- `prim.c` takes a `WgpuPrimState` (primitive mode, polygon mode, flat
  shading, provoking vertex) built from the shader binding's `GeomState`.
  It returns the host topology and, when needed, rewrites the draw into an
  index list. It follows the geometry shaders in `glsl/geom.c`:
  - Fill mode splits quads along the 0-2 diagonal and quad strip quads into
    `(0, 1, 2)` and `(2, 1, 3)`, as Vulkan does. Fans and polygons share
    vertex 0.
  - Line mode emits the three edges of every triangle (strips and fans
    included), the four outer edges of every quad, and the closed outline
    of a polygon. Point mode emits every triangle's or quad's vertices,
    and every polygon vertex once.
  - Line loops of two vertices draw one segment.
- With flat shading, WebGPU takes the first vertex of each primitive. So
  every triangle is rotated, keeping its winding, to put the NV2A
  provoking vertex first. Strips and fans use the same provoking vertex as
  the geometry shader. With last-vertex provoking, lines are reversed.
  Flat-shaded triangle strips are always converted to lists, so the
  host's rule for odd strip triangles never matters.
- Quads and quad strips take flat attributes from their fourth vertex. The
  first triangle of a 0-2 split doesn't contain that vertex, so flat quads
  are split along the 1-3 diagonal instead, and flat quad strips along
  0-3. Interpolated attributes such as texture coordinates can differ
  from Vulkan for non-planar quads, or quads that aren't parallelograms.
- Differences from Vulkan and the geometry shaders:
  - Polygon line mode closes the outline. Vulkan draws an open line strip
    and marks it `FIXME`.
  - Polygon point mode works; the geometry shader generator asserts.
  - In line and point modes, the geometry shaders give every edge and
    point the flat attributes of its triangle's provoking vertex. Index
    rewriting can't express this, so each edge or point uses its own
    first vertex.
  - Line and point modes do no face culling, which matches the geometry
    shaders' TODO. When the front and back polygon modes differ, the front
    mode is used and `NV2A_UNIMPLEMENTED` is logged.

Phase 2 notes (step 2, point size and point sprites):

- WebGPU only rasterizes 1-pixel points, so every point, whether from
  `PRIM_TYPE_POINTS` or polygon point mode, is an instance of a 4-vertex
  triangle strip. The packed vertex buffer is stepped per instance, and
  the vertex shader (`GenVshGlslOptions.point_quads`) moves each corner by
  half of `oPts.x` (at least 1) pixels in clip space. The point size math
  is the generator's existing code, the same that feeds `gl_PointSize` for
  Vulkan. Point quads are never culled.
- Instance data can't go through an index buffer, so indexed point draws
  and polygon point mode pack their vertices on the CPU in draw order.
  Non-indexed draws use `firstInstance` for each range.
- WebGPU has no `gl_PointCoord`, so the WebGPU target adds a `vtxPtCoord`
  varying, which the pixel shader uses for texture 3 when point sprites
  are on. Point quads write their corner, `(0, 0)` at the top left. Other
  primitives write their own texture 3 coordinates. On hardware, point
  sprite mode leaves non-point primitives textured normally: the "Quad"
  column of nxdk_pgraph_tests `Point sprite/AlphaMask_FF` in the hardware
  golden results shows this. Vulkan reads `gl_PointCoord` there, which is
  undefined for non-points, and draws a dark square.
- The original plan used vertex pulling from a storage buffer. Instancing
  gives the same result with fixed-function vertex fetch, and also works
  where vertex-stage storage buffers are limited.
- Checked with the nxdk_pgraph_tests release `v2026-10-10_21-27-19` on
  macOS arm64:
  - The `Point params` and `Point size` suites (including the 64-pixel
    `LargestPointSize` cases) match Vulkan side by side.
  - The `Point sprite` `AlphaMask_FF`, `AlphaMask_VS` and `AlphaTest` cases
    match Vulkan, and on the Quad column they match the hardware golden
    image rather than Vulkan.
  - No WebGPU validation errors.
- Known differences:
  - Fractional sizes below about 2 pixels can cover slightly different
    pixels than Vulkan's point rasterization, because quads follow
    triangle coverage rules.
  - Points whose centre is outside the viewport but whose quad overlaps it
    are partly drawn; Vulkan discards them by the centre.
  - `PolygonMode_Point` didn't run with the release ISO and config used
    here, so polygon point mode hasn't been checked on screen.
- Odd vertex formats and strides need nothing more for now. The CPU
  packer from phase 1 already converts every format and stride. Reading
  vertices straight from a guest RAM mirror is phase 4.

Phase 2 notes (step 3, textures):

- WebGPU samplers have no border colour, so `BORDER` addressing still uses
  a clamp-to-edge sampler, and the pixel shader mixes in the border colour.
  The WebGPU target adds two pixel shader uniforms, `texBorderColor` (from
  `NV_PGRAPH_BORDERCOLOR0`) and `texBorderMode`. The mode holds one bit for
  each of U, V and P that uses `BORDER`, and one bit for a linear
  magnification filter. The GL and Vulkan targets don't declare either
  uniform.
- After each 2D and 3D sample, `applyBorder()` computes how much of the
  sample lies inside the texture on each border axis, and mixes the
  clamp-to-edge texel with the border colour by that amount:
  - With linear filtering the coverage on each axis is
    `clamp(u * size + 0.5, 0, 1) * clamp((1 - u) * size + 0.5, 0, 1)`.
    For bilinear filtering of the base level this gives exactly the weight
    of the texels inside the texture.
  - With nearest filtering the coverage is 1 inside `[0, 1)` and 0 outside.
  - Minified samples use the same base-level coverage, so mipmapped
    textures blend into the border slightly differently from hardware.
- Textures whose border source is the texture itself already sample their
  own border texels, so they skip the mix. So do cube maps and `x8y24`
  depth textures.
- `CLAMP_OGL` stays clamp-to-edge, as in Vulkan.
- WebGPU samplers have no LOD bias either. The WebGPU target adds a
  `texLodBias` pixel shader uniform, from
  `NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS`, and passes it as the bias
  argument of every implicit-LOD `texture()` and `textureProj()` call. Tint
  turns these into `textureSampleBias`, which clamps the bias to
  [-16, 15.99]; the register's range is [-16, 15.996]. `x8y24` textures get
  no bias, because WGSL only allows a bias on float textures.
- Checked with the nxdk_pgraph_tests release `v2026-10-10_21-27-19` on
  macOS arm64, with no WebGPU validation errors:
  - `Texture border/2D` matches the hardware golden image.
  - `Texture border color/TextureBorderColor` matches the hardware golden
    image, including the translucent border alpha. Vulkan draws the
    border opaque for some formats.
  - The rest of the `Texture border` suite (3D and cube textures with
    texture borders) renders as before.
- The LOD bias change was checked on the same release, with no WebGPU
  validation errors. `Bump map`, `Texture cubemap`, `Texture 3D as 2D`
  (apart from the CPU-update case described below) and part of
  `Texture shadow comparator` render the same as Vulkan. These
  cover the projective, cube map, bump map, dot product and shadow sampling
  paths. Two problems limited testing:
  - The `Texture LOD Bias` suite doesn't run from this release ISO on
    either renderer, so the bias values themselves haven't been compared
    with hardware.
  - `Bump env lum` and `Bump map` stop partway with an abort on the
    unimplemented texture colour format `0x33` in `pgraph/texture.c`, which
    every renderer shares.
- Textures are looked up again on every draw. Guest CPU writes to texture
  memory set the page dirty bits but not `texture_dirty`, so binding only
  after state changes kept stale texels. `Texture 3D as 2D/Volumetric_as_2D`
  showed this. Vulkan rechecks on every draw because its dirty check also
  fires for disabled stages. A lookup whose binding and texture object are
  unchanged doesn't rebuild the bind group. With this, the `Texture CPU
  Update` suite and `Volumetric_as_2D` match the hardware golden images.
- WGSL has no `textureSample` for integer textures, so Tint rejected every
  shader that sampled an `x8y24` depth texture through `textureProj` on a
  `usampler2D`, and those draws were skipped. The WebGPU target now uses
  `texelFetch` on the texel that a nearest-filtered sample would return,
  clamped to the edge. Integer textures can't be filtered, so a nearest
  texel is the only result a sample can give. Vulkan keeps the guest's
  address modes, so `BORDER`, wrap and mirror addressing can differ from
  Vulkan.
- Format table check. These suites were captured on both renderers and
  scored against the hardware golden images: `Texture format`,
  `Texture DXT`, `Texture palette`, `Texture 2D as cubemap`,
  `Volume texture`, `Texture signed component tests`,
  `Texture projective 2D`, `Texture 3D as 2D`, `TextureWrapMode`,
  `Texture anisotropy`, `Texture CPU Update`, and the 24-bit and some 16-bit
  `Texture shadow comparator` cases. That is about 180 tests. After the two
  fixes above, WebGPU is as close to hardware as Vulkan in every one of them.
  Problems shared by every renderer:
  - The shared texture code doesn't define nxdk's `LU_IMAGE_R8B8` (`0x16`),
    swizzled `X8_Y24` and `Y16_FLOAT` depth (`0x2A`, `0x2B`, `0x2D`),
    `SZ_Y16` (`0x32`), the `YB16YA16` formats (`0x33`, `0x36`) and
    `A4V6YB6A4U6YA6` (`0x34`), and aborts on them. Swizzled 3D `R6G5B5`
    hits an assertion in `pgraph_convert_texture_data`. Those test cases
    were skipped.
  - The signed-channel bits are unimplemented, so `Texture signed component
    tests` differ from hardware on both renderers.
  - In `Texture projective 2D/ExplicitQ_ZeroCorner_*`, where q is zero at
    one corner, the `BORDER` row differs from hardware on both renderers,
    in different ways.

Phase 2 notes (step 4, surface pack and unpack):

- `R5G6B5`, `X1R5G5B5`, `Z16` and `Z24S8` still render through `rgba8unorm`
  and `depth32float-stencil8`. Upload and download used to convert on the
  CPU. `surface-compute.c` does that conversion in WGSL.
- Each kernel runs one thread per output row, so packed 16-bit pairs and
  stencil bytes are written by a single thread. Pack samples the colour
  texture, or the depth texture plus a stencil-aspect `texture_2d<u32>`,
  and writes guest pixels into a storage buffer. That buffer is copied to
  a map-readable buffer. Unpack writes `r32float` depth, then the existing
  fullscreen triangle stores it with `@builtin(frag_depth)`. Stencil is
  copied in as bytes. Colour is copied in as `rgba8unorm`. Copy rows are
  aligned to 256 bytes. `A8R8G8B8`, `X8R8G8B8`, `B8` and `G8B8` stay on the
  direct copy path.
- Z24 matches `vk/surface-compute.c` for `D32_SFLOAT_S8`: pack is
  `int(d * 0xffffff)`, unpack is `z / 2^24` plus one ULP. Z16 is
  `z / 65535`, rounded to nearest on the way back. 16-bit colour uses the
  same expand and reduce as the old CPU path. The X1 bit is alpha ≥ 128,
  which is what Vulkan's `A1R5G5B5` stores. Vulkan's format comment still
  says that bit should be forced to zero. Hardware clears leave it clear,
  so both renderers differ from hardware in the same way.
- The kernels take `width_in` and `width_out` and scale the same way as
  Vulkan. The scale factor is still 1 until step 7.
- Swizzle stays on the CPU (`swizzle_rect` / `unswizzle_rect`), as in
  Vulkan.
- Checked with nxdk_pgraph_tests release `v2026-10-10_21-27-19` on macOS
  arm64. Frames that show the same test match Vulkan, with no validation
  errors on these paths. `Surface format` `R5G6B5` matches the hardware
  golden. `X1R5G5B5` matches Vulkan. Fixed-point `Z16` and `Z24` depth
  tests match Vulkan and sit close to the hardware goldens.
- `Color zeta overlap` hits the `buffer_dirty` assert while evicting the
  bound colour surface. The OpenGL backend asserts in the same place.
- Float-Z depth tests submit a viewport depth outside `[0, 1]`. Dawn
  rejects the command buffer, so those draws do not appear. That check is
  outside the pack and unpack path.
- The surface tables still have no entry for `X1R5G5B5_O1R5G5B5` (`0x2`),
  `X8R8G8B8_O8R8G8B8` (`0x5`), or either `X1A7R8G8B8` format (`0x6`,
  `0x7`). Every renderer aborts on them.

Phase 2 notes (step 5, surface as texture):

- `bind_texture` looks up a surface at the texture's address. A
  single-level 2D texture with the same width and height, and the same
  pitch unless the surface is swizzled, can be filled from that surface
  image. The texture object stays in the cache and is recopied when
  `draw_time` changes, or after guest writes are uploaded.
- The copy runs when the GPU texels are the surface image. `A8R8G8B8` and
  `X8R8G8B8` surfaces copy into an `A8R8G8B8` texture with
  `copyTextureToTexture` (`bgra8unorm` to `bgra8unorm`). `Z24S8` sampled as
  `X8Y24` reuses the Z24 pack kernel and `copyBufferToTexture` into
  `r32uint`, with no readback. The packed word is `(z24 << 8) | stencil`,
  which the shader shifts by 8. `Z16` sampled as a Y16 texture quantizes
  to a 16-bit unorm and writes `rgba32float` with the same channel swizzle
  as the RAM decode (`R` is the depth, `G` is 0, `B` is 0 for fixed and 1
  for float). That path needs `float32-filterable`. On a device without it
  the Y16 texture is `rgba16float` and the bind stays on the RAM path.
- 16-bit colour, `B8`, `G8B8`, and `X8R8G8B8` textures (alpha forced to 1)
  stay on the RAM path. So do mipmaps, cubemaps, 3D textures, border
  textures, indexed textures, and a linear pitch that does not match.
  `Z24` sampled as Y16 stays on the RAM path as well: the texel sizes
  differ.
- When the surface scale factor is greater than 1 the texture is created
  at the scaled size, matching Vulkan. The factor is still 1.
- Checked with nxdk_pgraph_tests release `v2026-10-10_21-27-19` on macOS
  arm64. `TexFmt_A8R8G8B8` (swizzled and linear), `RenderTextureLoop`,
  `RenderTextureClearLoop`, and shadow comparator
  `2P16_10.00-20.00_15.0000_LT` match Vulkan. Frames that show the same
  test are identical, and there are no WebGPU validation errors. The copy
  runs for those render targets. `R5G6B5` and `X8R8G8B8` log a
  compatibility failure and stay on the RAM path.

Phase 2 notes (step 6, display composite):

- The NV09F image blit is the CPU path from phase 1. `display.c` is the
  PCRTC composite from `vk/display.c`, written in WGSL. The scanout surface
  is sampled into an `rgba8unorm` image at the VGA size. Interlace doubles
  the height, then the surface scale is applied. The scale is still 1.
  `line_offset` crops the source the same way Vulkan does.
- The shader flips Y the same way `vk/display.c` does. WebGPU stores row 0
  at the top, and `glTexImage2D` treats that row as the bottom. The UI
  samples the returned texture with flip disabled, so the picture is upright.
- PVIDEO buffer 0, colour format `LE_CR8YB8CB8YA8` only, is converted with
  `convert_yuy2_to_rgb` and drawn over the framebuffer. The overlay rectangle,
  colour key, and `DS_DX` / `DT_DY` scale match Vulkan, including treating
  `SIZE_IN == 0xFFFFFFFF` as off. Other colour formats still assert, as in
  the OpenGL and Vulkan renderers. Dirty surfaces that overlap the overlay
  are downloaded before the upload, so a surface used as the video source is
  not stale.
- The overlay sample is taken in uniform control flow. WGSL rejects
  `textureSample` under the per-pixel clip test.
- There is no GL memory import yet. That is phase 4. `get_framebuffer_surface`
  asks the PFIFO thread to render the composite and read it back, then
  uploads the pixels with `glTexImage2D` on the UI thread, which already has
  the GL context current. The UI is unchanged. If there is no colour surface,
  the readback fails, or the build has no OpenGL, it still returns 0 and the
  VGA path reads guest RAM.
- The GL texture name belongs to the UI context. Shutdown retires it and the
  next present on that context deletes it.
- Checked by compiling the display shader with Tint and the renderer against
  the SPIR-V Dawn SDK. The native check build has no Vulkan renderer, so
  `renderer = 'VULKAN'` falls back to OpenGL. On the ZPass frames below the
  framebuffer is upright and the rectangles, line, and point match that
  OpenGL picture. The filtered PVIDEO cases did not appear before the test
  XBE rebooted, so the overlay itself was not compared.

Phase 2 notes (step 7, reports and surface scale):

- `reports.c` follows `vk/reports.c`. `GET_REPORT` and `CLEAR_REPORT_VALUE`
  queue a watermark of how many occlusion queries have been started.
  `ZPASS_PIXEL_CNT` is the only report type. The count is divided by
  `surface_scale_factor` squared, as in Vulkan.
- WebGPU occlusion queries begin and end inside one render pass, and the
  pass must name the query set. Ending the pass ends the query. The next
  counted draw starts another, and the sample counts are added together, so
  a Vulkan query that stayed open across several passes has the same total.
  Clears and draws with ZPASS disabled close the query without counting.
- Results are resolved when a report is queued or the 1024-query set is
  full, then read back with `wgpuBufferMapAsync`. Vulkan reads its query
  pool from `finish` as well. `BACK_END_WRITE_SEMAPHORE_RELEASE` calls
  `surface_update`, which downloads dirty surfaces before the CPU stores
  the semaphore. With TCG that download waits for a later guest read, as in
  Vulkan. `pre_savevm_*` was already the Vulkan sequence: download every
  dirty surface, then wait. `pre_shutdown_*` stays empty, as in Vulkan.
- The WebGPU spec only requires an occlusion query to tell zero from
  non-zero. Dawn's Metal backend records
  `MTLVisibilityResultModeBoolean`, so a passing query comes back as 1.
  That value is stored as returned. Dividing it by the scale factor
  squared is wrong when the scale is greater than 1.
- The scale factor comes from `display.quality.surface_scale`. Changing it
  downloads dirty surfaces at the old factor, then flushes and reloads, as
  in Vulkan. Surfaces, viewports, scissors, the display target, and
  surface-to-texture copies were already multiplied by the factor. The pack
  and unpack kernels already scale. Direct copies (`A8R8G8B8`, `X8R8G8B8`,
  `B8`, `G8B8`) now take the top-left texel of each block on the way down
  and repeat it on the way up, which is what those kernels do. WebGPU lines
  stay one pixel wide, so a scaled frame's lines are thinner than Vulkan's.
- Compared on screen with OpenGL, because this native check build does not
  register Vulkan and a `VULKAN` config switches to OpenGL. nxdk_pgraph_tests
  release `v2026-10-10_21-27-19`, scale 1, macOS arm64 (M1 Pro). Semaphores
  `0xABC` and `0xCBA` pass on both. The pictures match: the same rectangles
  on `ZPass`, the same one-pixel line on `ZPassLineWidth-0x0001` and
  `0x0008`, and the same point on `ZPassPointSize-0x0008` and `0x0040`.
  OpenGL reports `40960` and `49152` for the two `ZPass` queries, `16448`
  for both line widths and for point size `0x0040`, and `16385` for point
  size `0x0008`, and those tests pass. WebGPU reports `1` for each of those
  except the second `ZPass` query, which is `0`, and those tests fail.
  There were no WebGPU validation errors. `surface_scale` 2 and a save
  state round trip were not run. The suite then failed to load
  `e:/nxdk_pgraph_tests/nxdk_pgraph_tests_config.json` and rebooted, so the
  PVIDEO cases in the same disc config were not shown.

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
2. Point sprites and point size (instanced quads), plus odd formats and
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

Phase 3 notes (step 1, vertex and pixel shaders):

- `pgraph/wgsl/` translates the GLSL produced by `pgraph_glsl_gen_vsh` and
  `pgraph_glsl_gen_psh` for `PGRAPH_GLSL_TARGET_WEBGPU`. `pgraph_wgsl_gen_vsh`
  and `pgraph_wgsl_gen_psh` take the same `VshState` and `PshState` as the
  GLSL generators. The GLSL text stays the source of the shading rules.
- A WGSL draw module uses entry points `vs` and `fs`. `XEMU_WGSL_DUMP`
  prints the translated source. Step 3 removed the SPIR-V fallback, so a
  shader the translator or Tint rejects is not drawn.
- WGSL has no overloads, so a later `xIsNan`, `_in`, or `normN` is renamed
  from its parameter types. Parameters are copied to locals, because a WGSL
  parameter cannot be assigned. `gl_VertexIndex` is a private variable set
  from `@builtin(vertex_index)`, because a helper cannot see an entry-point
  builtin. `if` and loop bodies are emitted with braces.
- When the module is WGSL, the uniform block is packed in WGSL layout.
  Scalar and `vec2` arrays are not padded to 16 bytes, and a `mat2` column
  is 8 bytes. The binding slot is still sized for std140, which is the same
  size or larger.
- `tests/xbox/wgsl` translates a vertex shader and a pixel shader without a
  device. On macOS arm64, booting nxdk_pgraph_tests compiled every draw
  shader as WGSL, with no Tint error. The same boot with `XEMU_WGSL=0`,
  before step 3 removed that switch, was compared on screen for
  `ZPassLineWidth-0x0001`, `ZPassLineWidth-0x0008`, and
  `ZPassPointSize-0x0008`. The rectangle, the line, and the point match.
  Both paths report the boolean occlusion count `1`, which is the Dawn
  limit from phase 2 step 7, not a WGSL difference.

Phase 3 notes (step 2, pack, unpack, and display):

- No new shaders. `surface-compute.c` and `display.c` were written as WGSL
  in phase 2 steps 4 and 6, and the clear and depth-upload shaders in
  `draw.c` and `surface.c` were already WGSL as well. They are created with
  `pgraph_wgpu_create_wgsl_module`. The checks from those phase 2 notes
  still apply.

Phase 3 notes (step 3, WGSL only):

- Draw shaders are still produced as GLSL by `pgraph_glsl_gen_vsh` and
  `pgraph_glsl_gen_psh`, then translated. The module passed to Dawn is
  WGSL, with entry points `vs` and `fs`. There is no SPIR-V fallback.
  `XEMU_WGSL=0` is gone. `XEMU_WGSL_DUMP` still prints the WGSL.
- If translation or Tint rejects a draw shader, that binding is invalid and
  the draw is skipped. Pack, unpack, clear, depth upload, and the display
  composite are fixed WGSL strings and are not on that path.
- `pgraph_wgpu_create_spirv_module` is removed. The instance requests
  `TimedWaitAny` only, not `ShaderSourceSPIRV`. Fragment shaders still emit
  `diagnostic(off, derivative_uniformity)`, which is what the SPIR-V option
  `allowNonUniformDerivatives` used to allow.
- The WebGPU objects no longer link glslang. Meson looks up glslang only
  when Vulkan is found. `glsl/spirv.c` stays for the Vulkan renderer.
- A native Dawn build no longer needs `TINT_BUILD_SPV_READER` for this
  backend. On macOS arm64 the instance comes up without that feature, and
  a boot of nxdk_pgraph_tests compiled its draw shaders with no Tint error.
  `tests/xbox/wgsl` still passes.

**Exit**: the WebGPU backend runs natively with WGSL only and keeps phase 2
parity.

### Phase 4: performance and native display fast path (medium)

1. Persistent pipeline cache through Dawn's blob cache. The draw waits for
   that pipeline. Blend color and the stencil reference stay dynamic, so a
   change there does not compile another pipeline.
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

Phase 4 notes (step 1, pipeline cache):

- Draw pipelines are created with `wgpuDeviceCreateRenderPipeline` before
  the draw is recorded. Skipping a draw while a pipeline compiled dropped
  boot-animation geometry, because a fade changes `NV_PGRAPH_BLENDCOLOR`
  on draws that are not repeated. That path was removed.
- `NV_PGRAPH_BLENDCOLOR` and `NV_PGRAPH_CONTROL_1_STENCIL_REF` are applied
  with `wgpuRenderPassEncoderSetBlendConstant` and
  `wgpuRenderPassEncoderSetStencilReference`. They are cleared in the
  pipeline key so a fade or a new stencil reference reuses the pipeline.
- The device descriptor chains `WGPUDawnCacheDeviceDescriptor`. Load and
  store callbacks keep one file per key under
  `<xemu base path>/webgpu-pipeline-cache`, named by the SHA-256 of Dawn's
  key. Dawn may call them from a compiler thread, so the file access is
  locked, and a store uses `g_file_set_contents` so a partial file cannot
  fail Dawn's blob hash check. The browser build has no Dawn blob cache.
  The in-memory map remains the `PipelineKey` hash table.
- On macOS arm64, a boot of nxdk_pgraph_tests wrote the cache (102 files
  on that run) and a second boot loaded it with no Dawn hash failure.

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
- **Differential shader testing** in phase 3: `tests/xbox/wgsl` checks the
  translator without a device. Before the SPIR-V path was removed, the same
  guest frame was drawn both ways and compared on screen.
- **Validation.** Run Dawn with backend validation and the WebGPU error
  scopes (`wgpuDevicePushErrorScope`) enabled in debug builds. A
  `display.webgpu.validation` config flag would mirror
  `display.vulkan.validation_layers`.
- **CI.** Download a pinned prebuilt Dawn release (version and SHA-256 kept
  in `scripts/download-dawn.py`, with archives for Linux x86_64 and macOS
  x86_64/arm64). Build the native WebGPU backend against it. Windows builds
  cross-compile with MinGW, which can't link the MSVC-built official Dawn
  release, so Windows needs a MinGW Dawn build before it can join. On
  Linux, run headless on SwiftShader through Dawn's Vulkan backend, with
  unit tests plus a small
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
