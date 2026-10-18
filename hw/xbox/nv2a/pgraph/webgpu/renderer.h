/*
 * Geforce NV2A PGRAPH WebGPU Renderer
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/lru.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <webgpu/webgpu.h>

#include "constants.h"
#include "prim.h"

typedef struct WgpuUniformMember {
    int offset; // -1 if the member is not in the block
    int column_bytes;
    int columns;
    int stride;
} WgpuUniformMember;

typedef struct WgpuTextureLayout {
    PshTextureType types[NV2A_MAX_TEXTURES];
    WGPUBindGroupLayout bind_group_layout;
    WGPUPipelineLayout pipeline_layout;
} WgpuTextureLayout;

typedef struct ShaderBinding {
    ShaderState state;
    bool valid;
    WGPUShaderModule vsh;
    WGPUShaderModule psh;
    VshUniformLocs vsh_locs;
    PshUniformLocs psh_locs;
    WgpuUniformMember vsh_members[VshUniform__COUNT];
    WgpuUniformMember psh_members[PshUniform__COUNT];
    size_t vsh_ubo_size;
    size_t psh_ubo_size;
    WgpuTextureLayout *texture_layout;
} ShaderBinding;

typedef struct WgpuVertexAttribute {
    uint8_t location;
    uint8_t offset;
    WGPUVertexFormat format;
    /* Guest stride when this attribute is fetched from the RAM mirror. */
    uint32_t stride;
} WgpuVertexAttribute;

typedef struct WgpuVertexLayout {
    uint32_t stride;
    uint32_t num_attributes;
    /* Each attribute is its own vertex buffer into the guest RAM mirror. */
    bool from_ram;
    WgpuVertexAttribute attributes[NV2A_VERTEXSHADER_ATTRIBUTES];
} WgpuVertexLayout;

typedef struct WgpuVertexSource {
    const uint8_t *data; // element 0
    size_t stride;
    unsigned int format;
    unsigned int count;
    hwaddr ram_addr; // ~(hwaddr)0 when the bytes are not in guest RAM
} WgpuVertexSource;

#define WGPU_TEXTURE_BIND_CACHE 64

typedef struct WgpuTextureBindKey {
    const WgpuTextureLayout *layout;
    WGPUTextureView views[NV2A_MAX_TEXTURES];
    WGPUSampler samplers[NV2A_MAX_TEXTURES];
} WgpuTextureBindKey;

typedef struct WgpuTextureBindCacheEntry {
    WgpuTextureBindKey key;
    WGPUBindGroup group;
    uint64_t stamp;
    bool used;
} WgpuTextureBindCacheEntry;

typedef struct PipelineKey {
    const ShaderBinding *shader;
    WGPUTextureFormat color_format;
    WGPUTextureFormat zeta_format;
    WGPUPrimitiveTopology topology;
    WgpuVertexLayout vertex_layout;
    uint32_t regs[9];
} PipelineKey;

enum WgpuStreamBufferIndex {
    WGPU_STREAM_UNIFORM,
    WGPU_STREAM_INDEX,
    WGPU_STREAM_VERTEX,
    WGPU_STREAM__COUNT,
};

typedef struct WgpuStreamBuffer {
    const char *label;
    WGPUBufferUsage usage;
    WGPUBuffer buffer;
    uint8_t *data;
    size_t size;
    size_t used;
} WgpuStreamBuffer;

typedef struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    MemAccessCallback *access_cb;

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;

    BasicSurfaceFormatInfo fmt;
    WgpuSurfaceFormatInfo host_fmt;

    WGPUTexture texture;
    WGPUTextureView view;
    /* Readable depth for a depth24plus-stencil8 zeta. NULL otherwise. */
    WGPUTexture depth_copy;
    WGPUTextureView depth_copy_view;

    bool initialized;
} SurfaceBinding;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    uint32_t filter;
    uint32_t address;
    uint32_t max_anisotropy;
    /* 1, or the surface scale when this texture is a scaled render target. */
    uint32_t scale;
} TextureKey;

typedef struct TextureBinding {
    LruNode node;
    TextureKey key;
    PshTextureType type;
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUSampler sampler;
    uint64_t hash;
    int draw_time;
    bool possibly_dirty;
} TextureBinding;

typedef struct ClearPipelineKey {
    WGPUTextureFormat color_format;
    WGPUTextureFormat zeta_format;
    WGPUColorWriteMask write_mask;
    bool depth_write;
    bool stencil_write;
} ClearPipelineKey;

typedef struct ClearPipeline {
    ClearPipelineKey key;
    WGPURenderPipeline pipeline;
} ClearPipeline;

typedef struct WgpuDisplayState {
    WGPUShaderModule module;
    WGPUBindGroupLayout bind_group_layout;
    WGPURenderPipeline pipeline;
    WGPUTextureFormat format;
    WGPUSampler frame_sampler;
    WGPUSampler pvideo_sampler;
    WGPUBuffer uniform;

    WGPUTexture image;
    WGPUTextureView image_view;
    unsigned int width;
    unsigned int height;
    /* The UI pass samples this image, so it is rgba8unorm with
     * TextureBinding and is not the GL-shared IOSurface. */
    bool sampleable;
    bool image_sampleable;
    bool presented;

    WGPUTexture pvideo_image;
    WGPUTextureView pvideo_view;
    unsigned int pvideo_width;
    unsigned int pvideo_height;

    WGPUBindGroup bind_group;

    WGPUBuffer readback;
    uint64_t readback_size;
    uint8_t *pixels;
    size_t pixels_size;
    bool pixels_valid;

    /*
     * Native builds can render the composite into memory that GL samples
     * directly. The readback above is the fallback when that import fails.
     * The browser presents the composite and the UI into the canvas.
     */
#ifndef __EMSCRIPTEN__
    WGPUSharedTextureMemory shared_memory;
    bool shared;
    bool shared_initialized;
    bool shared_access;
    int shared_gl_failed;
    uint32_t shared_serial;
#ifdef __APPLE__
    void *io_surface;
#endif
#ifdef __linux__
    int drm_fd;
    uint32_t dumb_handle;
    int prime_fd;
    uint64_t prime_size;
#endif
#ifdef _WIN32
    void *d3d_device;
    void *d3d_context;
    void *d3d_texture;
    void *shared_handle;
#endif
#endif

#ifdef CONFIG_OPENGL
    GLuint gl_texture;
    GLuint gl_memory;
    unsigned int gl_width;
    unsigned int gl_height;
    uint32_t gl_serial;
#endif
} WgpuDisplayState;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
} QueryReport;

typedef struct WgpuBlobCache WgpuBlobCache;

typedef struct PGRAPHWgpuState {
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WgpuBlobCache *blob_cache;

    WGPUCommandEncoder encoder;
    unsigned int encoder_start_time;
    WGPURenderPassEncoder render_pass;
    bool framebuffer_dirty;
    uint32_t submit_count;

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete;

    /* Occlusion queries. WebGPU begins and ends a query inside one render
     * pass, so a Vulkan query that spans several passes becomes several
     * queries here. Their sample counts are summed. */
    WGPUQuerySet query_set;
    WGPUBuffer query_resolve;
    WGPUBuffer query_readback;
    QSIMPLEQ_HEAD(, QueryReport) report_queue;
    unsigned int num_queries_in_flight;
    unsigned int max_queries_in_flight;
    bool new_query_needed;
    bool query_in_flight;
    bool resolve_queries;
    bool occlusion_collect_pending;
    bool occlusion_readback_pending;
    uint64_t occlusion_readback_bytes;
    uint32_t zpass_pixel_count_result;
    /* True once a query returns more than one sample. A 0 or 1 result is
     * the boolean occlusion query the spec allows, and must not be divided
     * by the surface scale. */
    bool zpass_pixel_count_is_samples;

    WGPUBuffer readback_buffer;
    uint64_t readback_buffer_size;

    WGPUShaderModule clear_module;
    GArray *clear_pipelines; // ClearPipeline

    WGPUShaderModule depth_upload_module;
    WGPUBindGroupLayout depth_upload_bind_group_layout;
    WGPUPipelineLayout depth_upload_pipeline_layout;
    WGPUTextureFormat depth_upload_formats[2];
    WGPURenderPipeline depth_upload_pipelines[2];

    /* Stencil upload when the zeta format cannot be a copy destination. */
    WGPUShaderModule stencil_upload_module;
    WGPUBindGroupLayout stencil_upload_layout;
    WGPUPipelineLayout stencil_upload_pipeline_layout;
    WGPURenderPipeline stencil_upload_pipeline;
    WGPUBuffer stencil_upload_refs;

    /* Pack/unpack kernels, plus the Z16 surface-to-texture kernel and the
     * depth24plus depth-copy pack. */
    WGPUShaderModule surface_compute_modules[10];
    WGPUBindGroupLayout surface_compute_layouts[10];
    WGPUPipelineLayout surface_compute_pipeline_layouts[10];
    WGPUComputePipeline surface_compute_pipelines[10];
    WGPUBuffer surface_compute_uniform;
    WGPUBuffer surface_compute_storage[3];
    uint64_t surface_compute_storage_size[3];
    WGPUBuffer surface_compute_readback;
    uint64_t surface_compute_readback_size;

    /* Expands packed texels on upload. Byte-identical copies do not use it. */
    WGPUShaderModule texture_decode_module;
    WGPUBindGroupLayout texture_decode_layout;
    WGPUPipelineLayout texture_decode_pipeline_layout;
    WGPUComputePipeline texture_decode_pipeline;
    WGPUBuffer texture_decode_uniform;
    WGPUBuffer texture_decode_src;
    uint64_t texture_decode_src_size;
    WGPUBuffer texture_decode_dst;
    uint64_t texture_decode_dst_size;

    bool depth32float_stencil8;
    bool texture_compression_bc;
    /* Queried only. User clip stays a fragment discard either way. */
    bool clip_distances;
    bool depth_clip_control;
    bool float32_filterable;
    /* Display target can be imported into GL. Browser builds leave this off. */
    bool shared_display;

    WgpuStreamBuffer stream_buffers[WGPU_STREAM__COUNT];

    /* Guest RAM mirror used as vertex buffers when the formats fit. */
    WGPUBuffer vertex_ram;
    uint64_t vertex_ram_bytes;
    unsigned long *vertex_ram_valid;
    bool vertex_ram_drawn;
    bool vertex_ram_eligible;

    /* The UI thread posts a frame. The PFIFO thread records it. */
    bool ui_pending;
    QemuEvent ui_complete;
    uint32_t max_vertex_buffers;
    uint32_t max_vertex_buffer_array_stride;
    uint64_t vertex_ram_offset[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint64_t vertex_ram_size[NV2A_VERTEXSHADER_ATTRIBUTES];

    GHashTable *shader_cache; // ShaderState -> ShaderBinding
    ShaderBinding *shader_binding;
    GArray *texture_layouts; // WgpuTextureLayout *
    WGPUBindGroupLayout uniform_bind_group_layout;
    WGPUBindGroup uniform_bind_group;
    WGPUBuffer uniform_bind_group_buffer;
    size_t uniform_binding_size;
    uint8_t *vsh_uniform_data;
    uint8_t *psh_uniform_data;
    uint64_t uniform_hashes[2];
    bool uniforms_valid;
    uint32_t uniform_offsets[2];

    WGPUTexture dummy_textures[PSH_TEXTURE_TYPE_CUBE + 1];
    WGPUTextureView dummy_texture_views[PSH_TEXTURE_TYPE_CUBE + 1];
    WGPUSampler dummy_sampler;

    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];
    WgpuTextureBindCacheEntry texture_bind_cache[WGPU_TEXTURE_BIND_CACHE];
    uint64_t texture_bind_stamp;

    GHashTable *pipeline_cache; // PipelineKey -> WGPURenderPipeline
    WGPUTexture scratch_zeta;
    WGPUTextureView scratch_zeta_view;
    /* Stand-in color targets when a fallback shader writes @location(1)
     * and the real surface is missing or not sampleable. */
    WGPUTexture fallback_color;
    WGPUTextureView fallback_color_view;
    uint32_t fallback_color_width;
    uint32_t fallback_color_height;
    WGPUTexture fallback_depth;
    WGPUTextureView fallback_depth_view;
    uint32_t fallback_depth_width;
    uint32_t fallback_depth_height;
    WGPUShaderModule depth_copy_clear_module;
    WGPURenderPipeline depth_copy_clear_pipeline;

    WgpuVertexLayout vertex_layout;
    WgpuVertexSource vertex_sources[NV2A_VERTEXSHADER_ATTRIBUTES];

    WgpuDisplayState display;
} PGRAPHWgpuState;

typedef enum WgpuFinishReason {
    WGPU_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    WGPU_FINISH_REASON_SURFACE_CREATE,
    WGPU_FINISH_REASON_SURFACE_DOWN,
    WGPU_FINISH_REASON_NEED_BUFFER_SPACE,
    WGPU_FINISH_REASON_FRAMEBUFFER_DIRTY,
    WGPU_FINISH_REASON_PRESENTING,
    WGPU_FINISH_REASON_FLIP_STALL,
    WGPU_FINISH_REASON_FLUSH,
    WGPU_FINISH_REASON_STALLED,
} WgpuFinishReason;

static inline WGPUStringView pgraph_wgpu_str(const char *s)
{
    return (WGPUStringView){ .data = s, .length = WGPU_STRLEN };
}

// instance.c
bool pgraph_wgpu_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_wgpu_finalize_instance(PGRAPHState *pg);
bool pgraph_wgpu_wait(PGRAPHWgpuState *r, WGPUFuture future);
WGPUShaderModule pgraph_wgpu_create_wgsl_module(PGRAPHWgpuState *r,
                                                const char *label,
                                                const char *code);

// buffer.c
void pgraph_wgpu_init_buffers(PGRAPHState *pg);
void pgraph_wgpu_finalize_buffers(PGRAPHState *pg);
bool pgraph_wgpu_stream_reserve(PGRAPHState *pg, int index, size_t size);
size_t pgraph_wgpu_stream_alloc(PGRAPHWgpuState *r, int index, size_t size,
                                size_t align, void **ptr);
void pgraph_wgpu_flush_streams(PGRAPHWgpuState *r);
void pgraph_wgpu_vertex_ram_invalidate(PGRAPHState *pg, hwaddr addr,
                                       hwaddr size);

// shaders.c
void pgraph_wgpu_init_shaders(PGRAPHState *pg);
void pgraph_wgpu_finalize_shaders(PGRAPHState *pg);
void pgraph_wgpu_bind_shaders(PGRAPHState *pg);
void pgraph_wgpu_update_uniforms(PGRAPHState *pg, bool *vsh_changed,
                                 bool *psh_changed);

// texture.c
void pgraph_wgpu_init_textures(PGRAPHState *pg);
void pgraph_wgpu_finalize_textures(PGRAPHState *pg);
void pgraph_wgpu_bind_textures(NV2AState *d);
void pgraph_wgpu_release_texture_bind_groups(PGRAPHWgpuState *r);
WGPUBindGroup pgraph_wgpu_get_texture_bind_group(PGRAPHWgpuState *r,
                                                 const WgpuTextureLayout *l);
void pgraph_wgpu_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                              hwaddr size);

// vertex.c
bool pgraph_wgpu_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                        unsigned int max_element,
                                        bool inline_data,
                                        unsigned int inline_stride,
                                        unsigned int provoking_element);
void pgraph_wgpu_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_wgpu_pack_vertices(PGRAPHWgpuState *r, uint8_t *out,
                               unsigned int first, unsigned int count);
/* Point lists that are rewritten stay packed. Otherwise a fitting layout
 * fetches from the RAM mirror. */
bool pgraph_wgpu_vertex_ram_prepare(PGRAPHState *pg, unsigned int first,
                                    unsigned int count, bool reorder);

// command.c
WGPUCommandEncoder pgraph_wgpu_ensure_encoder(PGRAPHState *pg);
void pgraph_wgpu_end_render_pass(PGRAPHWgpuState *r);
void pgraph_wgpu_finish(PGRAPHState *pg, WgpuFinishReason why, bool wait);
void pgraph_wgpu_wait_idle(PGRAPHWgpuState *r);
const void *pgraph_wgpu_read_buffer(PGRAPHWgpuState *r, WGPUBuffer buffer,
                                    uint64_t size);

// surface.c
void pgraph_wgpu_init_surfaces(PGRAPHState *pg);
WGPURenderPipeline pgraph_wgpu_depth_upload_pipeline(PGRAPHWgpuState *r,
                                                     WGPUTextureFormat format);
void pgraph_wgpu_finalize_surfaces(PGRAPHState *pg);
void pgraph_wgpu_surface_flush(NV2AState *d);
void pgraph_wgpu_process_pending_downloads(NV2AState *d);
void pgraph_wgpu_surface_download_if_dirty(NV2AState *d,
                                           SurfaceBinding *surface);
SurfaceBinding *pgraph_wgpu_surface_get(NV2AState *d, hwaddr addr);
SurfaceBinding *pgraph_wgpu_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_wgpu_wait_for_surface_download(SurfaceBinding *surface);
void pgraph_wgpu_download_dirty_surfaces(NV2AState *d);
typedef struct WgpuMemoryRange {
    hwaddr addr;
    hwaddr size;
} WgpuMemoryRange;

void pgraph_wgpu_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                     hwaddr start, hwaddr size);
/* One readback for every draw_dirty surface that overlaps any range. */
void pgraph_wgpu_download_surfaces_covering(PGRAPHState *pg,
                                            const WgpuMemoryRange *ranges,
                                            int count);
void pgraph_wgpu_vertex_ram_upload(PGRAPHState *pg,
                                   const WgpuMemoryRange *ranges, int count);
void pgraph_wgpu_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                     bool force);
void pgraph_wgpu_surface_update(NV2AState *d, bool upload, bool color_write,
                                bool zeta_write);
void pgraph_wgpu_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_wgpu_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_wgpu_get_surface_scale_factor(NV2AState *d);
void pgraph_wgpu_reload_surface_scale_factor(PGRAPHState *pg);

// reports.c
void pgraph_wgpu_init_reports(PGRAPHState *pg);
void pgraph_wgpu_finalize_reports(PGRAPHState *pg);
void pgraph_wgpu_clear_report_value(NV2AState *d);
void pgraph_wgpu_get_report(NV2AState *d, uint32_t parameter);
void pgraph_wgpu_process_pending_reports(NV2AState *d);
/* Finish an open query before ending the pass that owns it. */
void pgraph_wgpu_end_occlusion_query(PGRAPHWgpuState *r);
/* Make room for another query before this draw allocates vertex data. */
void pgraph_wgpu_reserve_occlusion_query(PGRAPHState *pg);
/* Begin the query for draws about to be recorded in the open pass. */
void pgraph_wgpu_prepare_occlusion_query(PGRAPHState *pg);
/* Record resolve + copy when reports or a full query set must be read. */
void pgraph_wgpu_occlusion_before_submit(PGRAPHState *pg);
void pgraph_wgpu_occlusion_after_submit(PGRAPHState *pg);

// surface-compute.c
void pgraph_wgpu_init_surface_compute(PGRAPHWgpuState *r);
void pgraph_wgpu_finalize_surface_compute(PGRAPHWgpuState *r);
/* Guest-layout rows, `pitch` bytes apart. Swizzle stays with the caller. */
void pgraph_wgpu_pack_converted_surface(PGRAPHState *pg,
                                        SurfaceBinding *surface, uint8_t *dst,
                                        unsigned int pitch);
/* Byte size of a converted download, and the GPU row stride. */
void pgraph_wgpu_converted_download_layout(const SurfaceBinding *surface,
                                           uint32_t *stride, uint64_t *bytes);
/* Pack into `dst` at `offset` and submit. Does not map. */
void pgraph_wgpu_record_converted_download(PGRAPHState *pg,
                                           SurfaceBinding *surface,
                                           WGPUBuffer dst, uint64_t offset);
/* Keep the pack storage buffer large enough for every download in a batch. */
void pgraph_wgpu_reserve_pack_output(PGRAPHWgpuState *r, uint64_t size);
void pgraph_wgpu_unpack_converted_surface(PGRAPHState *pg,
                                          SurfaceBinding *surface,
                                          const uint8_t *src,
                                          unsigned int pitch);
/* Copy a zeta surface into a colour texture. Z24 becomes packed r32uint.
 * Z16 becomes rgba32float with `channel_modes` (2 bits per channel).
 */
void pgraph_wgpu_copy_zeta_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                      WGPUTexture dst,
                                      WGPUTextureFormat dst_format,
                                      uint32_t channel_modes);

// draw.c
void pgraph_wgpu_init_pipelines(PGRAPHState *pg);
void pgraph_wgpu_finalize_pipelines(PGRAPHState *pg);
void pgraph_wgpu_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_wgpu_draw_begin(NV2AState *d);
void pgraph_wgpu_draw_end(NV2AState *d);
void pgraph_wgpu_flush_draw(NV2AState *d);
WgpuPrimState pgraph_wgpu_get_prim_state(const GeomState *geom);

// blit.c
void pgraph_wgpu_image_blit(NV2AState *d);

// display.c
void pgraph_wgpu_init_display(PGRAPHState *pg);
void pgraph_wgpu_finalize_display(PGRAPHState *pg);
void pgraph_wgpu_render_display(PGRAPHState *pg);
#ifdef __EMSCRIPTEN__
/* Configures #canvas and reports its format. Does not call
 * getCurrentTexture, so a later wait does not expire a canvas texture. */
bool pgraph_wgpu_display_canvas_describe(PGRAPHWgpuState *r,
                                         WGPUTextureFormat *format, int *width,
                                         int *height);
/* The view is owned by the display until pgraph_wgpu_display_canvas_release.
 * Submit the passes that use it before any suspending wait.
 * wgpuSurfacePresent is not called. */
bool pgraph_wgpu_display_canvas_acquire(PGRAPHWgpuState *r,
                                        WGPUTextureView *view,
                                        WGPUTextureFormat *format, int *width,
                                        int *height);
void pgraph_wgpu_display_canvas_release(PGRAPHWgpuState *r);
#else
/* The UI thread publishes the SDL window. These run on the PFIFO thread.
 * present calls wgpuSurfacePresent after the gamma blit and ImGui submit. */
bool pgraph_wgpu_display_window_active(void);
bool pgraph_wgpu_display_window_describe(PGRAPHWgpuState *r,
                                         WGPUTextureFormat *format, int *width,
                                         int *height);
bool pgraph_wgpu_display_window_acquire(PGRAPHWgpuState *r,
                                        WGPUTextureView *view,
                                        WGPUTextureFormat *format, int *width,
                                        int *height);
bool pgraph_wgpu_display_window_present(PGRAPHWgpuState *r);
void pgraph_wgpu_display_window_release(PGRAPHWgpuState *r);
#endif
void pgraph_wgpu_copy_adapter_text(char *vendor, size_t vendor_size,
                                   char *device, size_t device_size,
                                   char *description, size_t description_size);
#ifdef CONFIG_OPENGL
GLuint pgraph_wgpu_present_display(PGRAPHWgpuState *r);
#endif

#endif
