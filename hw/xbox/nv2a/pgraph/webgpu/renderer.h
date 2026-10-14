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
} WgpuVertexAttribute;

typedef struct WgpuVertexLayout {
    uint32_t stride;
    uint32_t num_attributes;
    WgpuVertexAttribute attributes[NV2A_VERTEXSHADER_ATTRIBUTES];
} WgpuVertexLayout;

typedef struct WgpuVertexSource {
    const uint8_t *data; // element 0
    size_t stride;
    unsigned int format;
    unsigned int count;
} WgpuVertexSource;

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
} TextureKey;

typedef struct TextureBinding {
    LruNode node;
    TextureKey key;
    PshTextureType type;
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUSampler sampler;
    uint64_t hash;
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

typedef struct PGRAPHWgpuState {
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;

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

    WGPUBuffer readback_buffer;
    uint64_t readback_buffer_size;

    WGPUShaderModule clear_module;
    GArray *clear_pipelines; // ClearPipeline

    WGPUShaderModule depth_upload_module;
    WGPUBindGroupLayout depth_upload_bind_group_layout;
    WGPURenderPipeline depth_upload_pipeline;

    bool depth_clip_control;
    bool float32_filterable;

    WgpuStreamBuffer stream_buffers[WGPU_STREAM__COUNT];

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
    bool texture_bindings_changed;
    WGPUBindGroup texture_bind_group;
    const WgpuTextureLayout *texture_bind_group_layout;

    GHashTable *pipeline_cache; // PipelineKey -> WGPURenderPipeline
    WGPUTexture scratch_zeta;
    WGPUTextureView scratch_zeta_view;

    WgpuVertexLayout vertex_layout;
    WgpuVertexSource vertex_sources[NV2A_VERTEXSHADER_ATTRIBUTES];
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
WGPUShaderModule pgraph_wgpu_create_spirv_module(PGRAPHWgpuState *r,
                                                 const char *label,
                                                 const uint32_t *code,
                                                 size_t num_words);

// buffer.c
void pgraph_wgpu_init_buffers(PGRAPHState *pg);
void pgraph_wgpu_finalize_buffers(PGRAPHState *pg);
bool pgraph_wgpu_stream_reserve(PGRAPHState *pg, int index, size_t size);
size_t pgraph_wgpu_stream_alloc(PGRAPHWgpuState *r, int index, size_t size,
                                size_t align, void **ptr);
void pgraph_wgpu_flush_streams(PGRAPHWgpuState *r);

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

// command.c
WGPUCommandEncoder pgraph_wgpu_ensure_encoder(PGRAPHState *pg);
void pgraph_wgpu_end_render_pass(PGRAPHWgpuState *r);
void pgraph_wgpu_finish(PGRAPHState *pg, WgpuFinishReason why, bool wait);
void pgraph_wgpu_wait_idle(PGRAPHWgpuState *r);
const void *pgraph_wgpu_read_buffer(PGRAPHWgpuState *r, WGPUBuffer buffer,
                                    uint64_t size);

// surface.c
void pgraph_wgpu_init_surfaces(PGRAPHState *pg);
void pgraph_wgpu_finalize_surfaces(PGRAPHState *pg);
void pgraph_wgpu_surface_flush(NV2AState *d);
void pgraph_wgpu_process_pending_downloads(NV2AState *d);
void pgraph_wgpu_surface_download_if_dirty(NV2AState *d,
                                           SurfaceBinding *surface);
SurfaceBinding *pgraph_wgpu_surface_get(NV2AState *d, hwaddr addr);
SurfaceBinding *pgraph_wgpu_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_wgpu_wait_for_surface_download(SurfaceBinding *surface);
void pgraph_wgpu_download_dirty_surfaces(NV2AState *d);
void pgraph_wgpu_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                     hwaddr start, hwaddr size);
void pgraph_wgpu_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                     bool force);
void pgraph_wgpu_surface_update(NV2AState *d, bool upload, bool color_write,
                                bool zeta_write);
void pgraph_wgpu_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);

// draw.c
void pgraph_wgpu_init_pipelines(PGRAPHState *pg);
void pgraph_wgpu_finalize_pipelines(PGRAPHState *pg);
void pgraph_wgpu_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_wgpu_draw_begin(NV2AState *d);
void pgraph_wgpu_draw_end(NV2AState *d);
void pgraph_wgpu_flush_draw(NV2AState *d);

// blit.c
void pgraph_wgpu_image_blit(NV2AState *d);

#endif
