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
#include "qemu/queue.h"
#include "qemu/thread.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/surface.h"

#include <webgpu/webgpu.h>

#include "constants.h"

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
                                                     hwaddr start,
                                                     hwaddr size);
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
