/*
 * Geforce NV2A PGRAPH WebGPU Renderer
 *
 * Copyright (c) 2026 xemu Project
 *
 * Based on Vulkan implementation:
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "ui/xemu-settings.h"
#include "renderer.h"

static const int num_invalid_surfaces_to_keep = 10;
static const int max_surface_frame_time_delta = 5;

void pgraph_wgpu_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    /* Download at the old factor, then flush reloads the new one. */
    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(
        &d->pgraph.wgpu_renderer_state->dirty_surfaces_download_complete);
    qatomic_set(&d->pgraph.wgpu_renderer_state->download_dirty_surfaces_pending,
                true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(
        &d->pgraph.wgpu_renderer_state->dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_wgpu_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor;
}

void pgraph_wgpu_reload_surface_scale_factor(PGRAPHState *pg)
{
    int factor = g_config.display.quality.surface_scale;
    pg->surface_scale_factor = MAX(factor, 1);
}

/* Nearest sample of the top-left texel in each scale x scale block. That is
 * what the pack kernels do. A pixel-center blit is implementation-defined
 * when the sample lands on a texel edge. */
static void downsample_nearest(uint8_t *dst, unsigned int dst_pitch,
                               const uint8_t *src, uint32_t src_stride,
                               unsigned int width, unsigned int height,
                               unsigned int scale, unsigned int bpp)
{
    if (scale <= 1) {
        for (unsigned int y = 0; y < height; y++) {
            memcpy(dst + (size_t)y * dst_pitch, src + (uint64_t)y * src_stride,
                   (size_t)width * bpp);
        }
        return;
    }

    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *src_row = src + (uint64_t)(y * scale) * src_stride;
        uint8_t *dst_row = dst + (size_t)y * dst_pitch;
        for (unsigned int x = 0; x < width; x++) {
            memcpy(dst_row + (size_t)x * bpp,
                   src_row + (size_t)(x * scale) * bpp, bpp);
        }
    }
}

static void upsample_nearest(uint8_t *dst, uint32_t dst_stride,
                             const uint8_t *src, unsigned int src_pitch,
                             unsigned int width, unsigned int height,
                             unsigned int scale, unsigned int bpp)
{
    unsigned int scaled_h = height * scale;

    for (unsigned int y = 0; y < scaled_h; y++) {
        const uint8_t *src_row = src + (size_t)(y / scale) * src_pitch;
        uint8_t *dst_row = dst + (uint64_t)y * dst_stride;
        unsigned int scaled_w = width * scale;
        for (unsigned int x = 0; x < scaled_w; x++) {
            memcpy(dst_row + (size_t)x * bpp,
                   src_row + (size_t)(x / scale) * bpp, bpp);
        }
    }
}

static const char depth_upload_wgsl[] =
    "@group(0) @binding(0) var depth_tex: texture_2d<f32>;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f "
    "{\n"
    "    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @builtin(frag_depth) "
    "f32 {\n"
    "    return textureLoad(depth_tex, vec2i(pos.xy), 0).r;\n"
    "}\n";

static void get_surface_dimensions(PGRAPHState const *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

static bool framebuffer_dirty(PGRAPHState const *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed ||
        (!pg->surface_shape.color_format && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

static bool check_surface_overlaps_range(const SurfaceBinding *surface,
                                         hwaddr range_start, hwaddr range_len)
{
    hwaddr surface_end = surface->vram_addr + surface->size;
    hwaddr range_end = range_start + range_len;
    return !(surface->vram_addr >= range_end || range_start >= surface_end);
}

static void download_surface_list(NV2AState *d, SurfaceBinding **surfaces,
                                  int count);

void pgraph_wgpu_download_surfaces_covering(PGRAPHState *pg,
                                            const WgpuMemoryRange *ranges,
                                            int count)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    g_autoptr(GPtrArray) list = g_ptr_array_new();

    if (!count) {
        return;
    }

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (!surface->draw_dirty || !surface->width || !surface->height) {
            continue;
        }
        for (int i = 0; i < count; i++) {
            if (!ranges[i].size) {
                continue;
            }
            if (check_surface_overlaps_range(surface, ranges[i].addr,
                                             ranges[i].size)) {
                g_ptr_array_add(list, surface);
                break;
            }
        }
    }

    download_surface_list(container_of(pg, NV2AState, pgraph),
                          (SurfaceBinding **)list->pdata, list->len);
}

void pgraph_wgpu_download_surfaces_in_range_if_dirty(PGRAPHState *pg,
                                                     hwaddr start, hwaddr size)
{
    WgpuMemoryRange range = { .addr = start, .size = size };

    pgraph_wgpu_download_surfaces_covering(pg, &range, 1);
}

static void ensure_readback_buffer(PGRAPHWgpuState *r, uint64_t size)
{
    if (r->readback_buffer && r->readback_buffer_size >= size) {
        return;
    }
    if (r->readback_buffer) {
        wgpuBufferRelease(r->readback_buffer);
    }
    WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.label = pgraph_wgpu_str("Surface readback");
    descriptor.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    descriptor.size = ROUND_UP(size, 1024 * 1024);
    r->readback_buffer = wgpuDeviceCreateBuffer(r->device, &descriptor);
    r->readback_buffer_size = descriptor.size;
}

static void copy_aspect_to_buffer(WGPUCommandEncoder encoder,
                                  SurfaceBinding *surface,
                                  WGPUTextureAspect aspect, WGPUBuffer buffer,
                                  uint64_t offset, uint32_t bytes_per_row,
                                  unsigned int width, unsigned int height)
{
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = surface->texture;
    src.aspect = aspect;

    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buffer;
    dst.layout.offset = offset;
    dst.layout.bytesPerRow = bytes_per_row;
    dst.layout.rowsPerImage = height;

    WGPUExtent3D extent = { width, height, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
}

typedef struct SurfaceDownload {
    SurfaceBinding *surface;
    uint8_t *pixels;
    bool converted;
    uint64_t offset;
    uint64_t gpu_bytes;
    uint32_t src_pitch;
    unsigned int scale;
} SurfaceDownload;

/* Copy every surface, then map the readback once. A converted surface is
 * submitted on its own because the pack kernel reuses one storage buffer,
 * but it lands in the same readback so the CPU still waits once. */
static void download_surface_list(NV2AState *d, SurfaceBinding **surfaces,
                                  int count)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    g_autoptr(GArray) jobs = g_array_new(false, true, sizeof(SurfaceDownload));
    uint64_t cursor = 0;
    uint64_t max_pack = 0;

    for (int i = 0; i < count; i++) {
        SurfaceBinding *surface = surfaces[i];
        bool seen = false;

        if (!surface || !surface->width || !surface->height) {
            continue;
        }
        for (guint j = 0; j < jobs->len; j++) {
            if (g_array_index(jobs, SurfaceDownload, j).surface == surface) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        SurfaceDownload job = {
            .surface = surface,
            .pixels = d->vram_ptr + surface->vram_addr,
            .converted = surface->host_fmt.conversion != WGPU_SURFACE_CONV_NONE,
        };
        if (job.converted) {
            uint32_t stride;
            uint64_t bytes;
            pgraph_wgpu_converted_download_layout(surface, &stride, &bytes);
            job.src_pitch = stride;
            /* The pack copy rounds the byte count up to 4. */
            job.gpu_bytes = ROUND_UP(bytes, 256);
            max_pack = MAX(max_pack, bytes);
        } else {
            unsigned int scale = pg->surface_scale_factor;
            unsigned int scaled_w = surface->width * scale;
            uint32_t bpp = surface->host_fmt.host_bytes_per_pixel;

            job.scale = scale;
            job.src_pitch = ROUND_UP(scaled_w * bpp, 256);
            job.gpu_bytes = (uint64_t)job.src_pitch * surface->height * scale;
        }
        cursor = ROUND_UP(cursor, 256);
        job.offset = cursor;
        cursor += job.gpu_bytes;
        g_array_append_val(jobs, job);

        nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
        trace_nv2a_pgraph_surface_download(
            surface->color ? "COLOR" : "ZETA", surface->swizzle ? "sz" : "lin",
            surface->vram_addr, surface->width, surface->height, surface->pitch,
            surface->fmt.bytes_per_pixel);
    }

    if (!jobs->len || !cursor) {
        return;
    }

    /* Draws that produced these surfaces have to be submitted first. */
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN, false);
    if (max_pack) {
        pgraph_wgpu_reserve_pack_output(r, max_pack);
    }
    ensure_readback_buffer(r, cursor);

    bool recorded_direct = false;
    for (guint i = 0; i < jobs->len; i++) {
        SurfaceDownload *job = &g_array_index(jobs, SurfaceDownload, i);
        SurfaceBinding *surface = job->surface;

        if (job->converted) {
            continue;
        }
        copy_aspect_to_buffer(
            pgraph_wgpu_ensure_encoder(pg), surface, WGPUTextureAspect_All,
            r->readback_buffer, job->offset, job->src_pitch,
            surface->width * job->scale, surface->height * job->scale);
        recorded_direct = true;
    }

    for (guint i = 0; i < jobs->len; i++) {
        SurfaceDownload *job = &g_array_index(jobs, SurfaceDownload, i);

        if (!job->converted) {
            continue;
        }
        /* Submits any direct copies still in the encoder, then this pack. */
        pgraph_wgpu_record_converted_download(pg, job->surface,
                                              r->readback_buffer, job->offset);
        recorded_direct = false;
    }

    if (recorded_direct) {
        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN, false);
    }

    const uint8_t *mapped =
        pgraph_wgpu_read_buffer(r, r->readback_buffer, cursor);
    if (mapped) {
        for (guint i = 0; i < jobs->len; i++) {
            SurfaceDownload *job = &g_array_index(jobs, SurfaceDownload, i);
            SurfaceBinding *surface = job->surface;
            uint8_t *out = job->pixels;
            g_autofree uint8_t *swizzle_buf = NULL;

            if (surface->swizzle) {
                swizzle_buf = g_malloc(surface->size);
                out = swizzle_buf;
            }
            if (job->converted) {
                uint32_t bpp = surface->fmt.bytes_per_pixel;
                for (unsigned int y = 0; y < surface->height; y++) {
                    memcpy(out + (size_t)y * surface->pitch,
                           mapped + job->offset + (uint64_t)y * job->src_pitch,
                           (size_t)surface->width * bpp);
                }
            } else {
                downsample_nearest(out, surface->pitch, mapped + job->offset,
                                   job->src_pitch, surface->width,
                                   surface->height, job->scale,
                                   surface->host_fmt.host_bytes_per_pixel);
            }
            if (surface->swizzle) {
                swizzle_rect(swizzle_buf, surface->width, surface->height,
                             job->pixels, surface->pitch,
                             surface->fmt.bytes_per_pixel);
                nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
            }
        }
        wgpuBufferUnmap(r->readback_buffer);
    }

    for (guint i = 0; i < jobs->len; i++) {
        SurfaceBinding *surface =
            g_array_index(jobs, SurfaceDownload, i).surface;
        hwaddr bytes = surface->pitch * surface->height;

        memory_region_set_client_dirty(d->vram, surface->vram_addr, bytes,
                                       DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, surface->vram_addr, bytes,
                                       DIRTY_MEMORY_NV2A_TEX);
        surface->download_pending = false;
        surface->draw_dirty = false;
    }
}

static void download_surface(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force) || !surface->width ||
        !surface->height) {
        return;
    }

    download_surface_list(d, &surface, 1);
}

void pgraph_wgpu_wait_for_surface_download(SurfaceBinding *surface)
{
    NV2AState *d = g_nv2a;
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    if (qatomic_read(&surface->draw_dirty)) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&surface->download_pending, true);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&r->downloads_complete);
    }
}

void pgraph_wgpu_process_pending_downloads(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    g_autoptr(GPtrArray) list = g_ptr_array_new();
    SurfaceBinding *surface;

    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->download_pending) {
            g_ptr_array_add(list, surface);
        }
    }
    download_surface_list(d, (SurfaceBinding **)list->pdata, list->len);

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

void pgraph_wgpu_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    g_autoptr(GPtrArray) list = g_ptr_array_new();
    SurfaceBinding *surface;

    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->draw_dirty) {
            g_ptr_array_add(list, surface);
        }
    }
    download_surface_list(d, (SurfaceBinding **)list->pdata, list->len);

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    NV2AState *d = (NV2AState *)opaque;
    qemu_mutex_lock(&d->pgraph.lock);

    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;
    bool wait_for_downloads = false;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (!check_surface_overlaps_range(surface, addr, len)) {
            continue;
        }

        hwaddr offset = addr - surface->vram_addr;

        if (write) {
            trace_nv2a_pgraph_surface_cpu_write(surface->vram_addr, offset);
        } else {
            trace_nv2a_pgraph_surface_cpu_read(surface->vram_addr, offset);
        }

        if (surface->draw_dirty) {
            surface->download_pending = true;
            wait_for_downloads = true;
        }

        if (write) {
            surface->upload_pending = true;
        }
    }

    qemu_mutex_unlock(&d->pgraph.lock);

    if (wait_for_downloads) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&r->downloads_complete);
        qatomic_set(&r->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&r->downloads_complete);
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (tcg_enabled()) {
        if (surface->width && surface->height) {
            surface->access_cb = mem_access_callback_insert(
                qemu_get_cpu(0), d->vram, surface->vram_addr, surface->size,
                &surface_access_callback, d);
        } else {
            surface->access_cb = NULL;
        }
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding const *surface)
{
    if (tcg_enabled()) {
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
    }
}

static void bind_surface(PGRAPHWgpuState *r, SurfaceBinding *surface)
{
    if (surface->color) {
        r->color_binding = surface;
    } else {
        r->zeta_binding = surface;
    }

    r->framebuffer_dirty = true;
}

static void unbind_surface(NV2AState *d, bool color)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    if (color) {
        if (r->color_binding) {
            r->color_binding = NULL;
            r->framebuffer_dirty = true;
        }
    } else {
        if (r->zeta_binding) {
            r->zeta_binding = NULL;
            r->framebuffer_dirty = true;
        }
    }
}

static void invalidate_surface(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    if (surface == r->color_binding) {
        assert(d->pgraph.surface_color.buffer_dirty);
        unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        assert(d->pgraph.surface_zeta.buffer_dirty);
        unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    QTAILQ_INSERT_HEAD(&r->invalid_surfaces, surface, entry);
}

static bool check_surfaces_overlap(const SurfaceBinding *surface,
                                   const SurfaceBinding *other_surface)
{
    return check_surface_overlaps_range(surface, other_surface->vram_addr,
                                        other_surface->size);
}

static void invalidate_overlapping_surfaces(NV2AState *d,
                                            SurfaceBinding const *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    g_autoptr(GPtrArray) list = g_ptr_array_new();
    SurfaceBinding *other_surface, *next_surface;

    QTAILQ_FOREACH (other_surface, &r->surfaces, entry) {
        if (check_surfaces_overlap(surface, other_surface) &&
            other_surface->draw_dirty) {
            g_ptr_array_add(list, other_surface);
        }
    }
    download_surface_list(d, (SurfaceBinding **)list->pdata, list->len);

    QTAILQ_FOREACH_SAFE (other_surface, &r->surfaces, entry, next_surface) {
        if (check_surfaces_overlap(surface, other_surface)) {
            trace_nv2a_pgraph_surface_evict_overlapping(
                other_surface->vram_addr, other_surface->width,
                other_surface->height, other_surface->pitch);
            invalidate_surface(d, other_surface);
        }
    }
}

static void surface_put(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    assert(pgraph_wgpu_surface_get(d, surface->vram_addr) == NULL);

    invalidate_overlapping_surfaces(d, surface);
    register_cpu_access_callback(d, surface);

    QTAILQ_INSERT_HEAD(&r->surfaces, surface, entry);
}

SurfaceBinding *pgraph_wgpu_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->vram_addr == addr) {
            return surface;
        }
    }

    return NULL;
}

SurfaceBinding *pgraph_wgpu_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (addr >= surface->vram_addr &&
            addr < (surface->vram_addr + surface->size)) {
            return surface;
        }
    }

    return NULL;
}

static void create_surface_texture(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    unsigned int width = surface->width ? surface->width : 1;
    unsigned int height = surface->height ? surface->height : 1;
    pgraph_apply_scaling_factor(pg, &width, &height);

    assert(!surface->texture);

    g_autofree gchar *label =
        g_strdup_printf("Surface %" HWADDR_PRIx "h fmt:%s,%02xh %dx%d",
                        surface->vram_addr, surface->color ? "Color" : "Zeta",
                        surface->color ? surface->shape.color_format :
                                         surface->shape.zeta_format,
                        surface->width, surface->height);

    WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = pgraph_wgpu_str(label);
    descriptor.usage = WGPUTextureUsage_RenderAttachment |
                       WGPUTextureUsage_TextureBinding |
                       WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = (WGPUExtent3D){ width, height, 1 };
    descriptor.format = surface->host_fmt.format;
    surface->texture = wgpuDeviceCreateTexture(r->device, &descriptor);
    surface->view = wgpuTextureCreateView(surface->texture, NULL);

    nv2a_profile_inc_counter(NV2A_PROF_SURF_CREATE);
}

static void migrate_surface_texture(SurfaceBinding *dst, SurfaceBinding *src)
{
    dst->texture = src->texture;
    dst->view = src->view;
    src->texture = NULL;
    src->view = NULL;
}

static void destroy_surface_texture(SurfaceBinding *surface)
{
    if (surface->view) {
        wgpuTextureViewRelease(surface->view);
        surface->view = NULL;
    }
    if (surface->texture) {
        wgpuTextureRelease(surface->texture);
        surface->texture = NULL;
    }
}

static bool check_invalid_surface_is_compatibile(SurfaceBinding *surface,
                                                 SurfaceBinding *target)
{
    return surface->host_fmt.format == target->host_fmt.format &&
           surface->width == target->width && surface->height == target->height;
}

static SurfaceBinding *
get_any_compatible_invalid_surface(PGRAPHWgpuState *r, SurfaceBinding *target)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE (surface, &r->invalid_surfaces, entry, next) {
        if (check_invalid_surface_is_compatibile(surface, target)) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            return surface;
        }
    }

    return NULL;
}

static void prune_invalid_surfaces(PGRAPHWgpuState *r, int keep)
{
    int num_surfaces = 0;

    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE (surface, &r->invalid_surfaces, entry, next) {
        num_surfaces += 1;
        if (num_surfaces > keep) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            destroy_surface_texture(surface);
            g_free(surface);
        }
    }
}

static void expire_old_surfaces(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    g_autoptr(GPtrArray) list = g_ptr_array_new();
    SurfaceBinding *s, *next;

    QTAILQ_FOREACH (s, &r->surfaces, entry) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= max_surface_frame_time_delta && s->draw_dirty) {
            g_ptr_array_add(list, s);
        }
    }
    download_surface_list(d, (SurfaceBinding **)list->pdata, list->len);

    QTAILQ_FOREACH_SAFE (s, &r->surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        if (last_used >= max_surface_frame_time_delta) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            invalidate_surface(d, s);
        }
    }
}

static bool check_surface_compatibility(SurfaceBinding const *s1,
                                        SurfaceBinding const *s2, bool strict)
{
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->host_fmt.format == s2->host_fmt.format) &&
        (s1->host_fmt.conversion == s2->host_fmt.conversion) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

void pgraph_wgpu_surface_download_if_dirty(NV2AState *d,
                                           SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        download_surface(d, surface, true);
    }
}

static void write_texture(PGRAPHWgpuState *r, WGPUTexture texture,
                          WGPUTextureAspect aspect, const void *data,
                          size_t size, uint32_t bytes_per_row,
                          unsigned int width, unsigned int height)
{
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = texture;
    dst.aspect = aspect;

    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = bytes_per_row;
    layout.rowsPerImage = height;

    WGPUExtent3D extent = { width, height, 1 };
    wgpuQueueWriteTexture(r->queue, &dst, data, size, &layout, &extent);
}

void pgraph_wgpu_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                     bool force)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!(surface->upload_pending || force)) {
        return;
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);

    /* Queue writes execute before any not-yet-submitted commands. */
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_CREATE, false);

    trace_nv2a_pgraph_surface_upload(
        surface->color ? "COLOR" : "ZETA", surface->swizzle ? "sz" : "lin",
        surface->vram_addr, surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    surface->upload_pending = false;
    surface->draw_time = pg->draw_time;

    if (!surface->width || !surface->height) {
        surface->initialized = true;
        return;
    }

    uint8_t *buf = d->vram_ptr + surface->vram_addr;
    g_autofree uint8_t *swizzle_buf = NULL;
    unsigned int pitch = surface->pitch;

    if (surface->swizzle) {
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        unswizzle_rect(buf, surface->width, surface->height, swizzle_buf,
                       surface->pitch, surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        buf = swizzle_buf;
    }

    if (surface->host_fmt.depth ||
        surface->host_fmt.conversion != WGPU_SURFACE_CONV_NONE) {
        pgraph_wgpu_unpack_converted_surface(pg, surface, buf, pitch);
    } else {
        unsigned int scale = pg->surface_scale_factor;
        unsigned int bpp = surface->fmt.bytes_per_pixel;
        if (scale <= 1) {
            uint32_t row_size = surface->width * bpp;
            size_t size = (size_t)row_size * surface->height;
            g_autofree uint8_t *host_buf = g_malloc(size);
            for (unsigned int y = 0; y < surface->height; y++) {
                memcpy(host_buf + (size_t)y * row_size, buf + (size_t)y * pitch,
                       row_size);
            }
            write_texture(r, surface->texture, WGPUTextureAspect_All, host_buf,
                          size, row_size, surface->width, surface->height);
        } else {
            unsigned int scaled_w = surface->width * scale;
            unsigned int scaled_h = surface->height * scale;
            uint32_t row_size = ROUND_UP(scaled_w * bpp, 256);
            size_t size = (size_t)row_size * scaled_h;
            g_autofree uint8_t *host_buf = g_malloc(size);
            upsample_nearest(host_buf, row_size, buf, pitch, surface->width,
                             surface->height, scale, bpp);
            write_texture(r, surface->texture, WGPUTextureAspect_All, host_buf,
                          size, row_size, scaled_w, scaled_h);
        }
    }

    surface->initialized = true;
}

static void compare_surfaces(SurfaceBinding const *a, SurfaceBinding const *b)
{
#define DO_CMP(fld)                                                        \
    if (a->fld != b->fld)                                                  \
        trace_nv2a_pgraph_surface_compare_mismatch(#fld, (long int)a->fld, \
                                                   (long int)b->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(host_fmt.format)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
#undef DO_CMP
}

static void populate_surface_binding_target_sized(NV2AState *d, bool color,
                                                  unsigned int width,
                                                  unsigned int height,
                                                  SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    BasicSurfaceFormatInfo fmt;
    WgpuSurfaceFormatInfo host_fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_wgpu_map));
        fmt = kelvin_surface_color_format_map[pg->surface_shape.color_format];
        host_fmt = kelvin_surface_color_format_wgpu_map[pg->surface_shape
                                                            .color_format];
        if (host_fmt.host_bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        assert(pg->surface_shape.zeta_format != 0);
        assert(pg->surface_shape.zeta_format <
               ARRAY_SIZE(kelvin_surface_zeta_format_wgpu_map));
        fmt = kelvin_surface_zeta_format_map[pg->surface_shape.zeta_format];
        host_fmt =
            kelvin_surface_zeta_format_wgpu_map[pg->surface_shape.zeta_format];
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    target->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    target->fmt = fmt;
    target->host_fmt = host_fmt;
    target->color = color;
    target->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    target->vram_addr = dma.address + surface->offset;
    target->width = width;
    target->height = height;
    target->pitch = surface->pitch;
    target->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    target->upload_pending = true;
    target->download_pending = false;
    target->draw_dirty = false;
    target->dma_addr = dma.address;
    target->dma_len = dma.limit;
    target->frame_time = pg->frame_time;
    target->draw_time = pg->draw_time;
    target->cleared = false;

    target->initialized = false;
}

static void populate_surface_binding_target(NV2AState *d, bool color,
                                            SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        get_surface_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        /* Surface dimensions come from the clip rectangle, so include the
         * surface offset as well.
         */
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_target_sized(d, color, width, height, target);
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    SurfaceBinding target;
    memset(&target, 0, sizeof(target));
    populate_surface_binding_target(d, color, &target);

    Surface *pg_surface = color ? &pg->surface_color : &pg->surface_zeta;

    bool mem_dirty = !tcg_enabled() && memory_region_test_and_clear_dirty(
                                           d->vram, target.vram_addr,
                                           target.size, DIRTY_MEMORY_NV2A);

    SurfaceBinding *current_binding =
        color ? r->color_binding : r->zeta_binding;

    if (!current_binding ||
        (upload && (pg_surface->buffer_dirty || mem_dirty))) {
        pgraph_wgpu_end_render_pass(r);

        unbind_surface(d, color);

        SurfaceBinding *surface = pgraph_wgpu_surface_get(d, target.vram_addr);
        if (surface != NULL) {
            SurfaceBinding *other =
                (color ? r->zeta_binding : r->color_binding);
            if (surface == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", target.vram_addr,
            target.swizzle ? "sz" : "ln", pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x, pg->surface_shape.clip_width,
            pg->surface_shape.clip_y, pg->surface_shape.clip_height);

        bool should_create = true;

        if (surface != NULL) {
            bool is_compatible =
                check_surface_compatibility(surface, &target, false);

            void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                             const char *layout, uint32_t anti_aliasing,
                             uint32_t clip_x, uint32_t clip_width,
                             uint32_t clip_y, uint32_t clip_height,
                             uint32_t pitch) =
                surface->color ? trace_nv2a_pgraph_surface_match_color :
                                 trace_nv2a_pgraph_surface_match_zeta;

            trace_fn(surface->vram_addr, surface->width, surface->height,
                     surface->swizzle ? "sz" : "ln",
                     surface->shape.anti_aliasing, surface->shape.clip_x,
                     surface->shape.clip_width, surface->shape.clip_y,
                     surface->shape.clip_height, surface->pitch);

            assert(!(target.swizzle && pg->clearing));

            if (is_compatible && color &&
                !check_surface_compatibility(surface, &target, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_target_sized(
                    d, !color, surface->width, surface->height, &zeta_entry);
                hwaddr color_end = surface->vram_addr + surface->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= surface->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

            if (is_compatible && !color && r->color_binding) {
                is_compatible &= (surface->width == r->color_binding->width) &&
                                 (surface->height == r->color_binding->height);
            }

            if (is_compatible) {
                pg->surface_binding_dim.width = surface->width;
                pg->surface_binding_dim.clip_x = surface->shape.clip_x;
                pg->surface_binding_dim.clip_width = surface->shape.clip_width;
                pg->surface_binding_dim.height = surface->height;
                pg->surface_binding_dim.clip_y = surface->shape.clip_y;
                pg->surface_binding_dim.clip_height =
                    surface->shape.clip_height;
                surface->upload_pending |= mem_dirty;
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else {
                trace_nv2a_pgraph_surface_evict_reason("incompatible",
                                                       surface->vram_addr);
                compare_surfaces(surface, &target);
                pgraph_wgpu_surface_download_if_dirty(d, surface);
                invalidate_surface(d, surface);
            }
        }

        if (should_create) {
            surface = get_any_compatible_invalid_surface(r, &target);
            if (surface) {
                migrate_surface_texture(&target, surface);
            } else {
                surface = g_malloc(sizeof(SurfaceBinding));
                create_surface_texture(pg, &target);
            }

            *surface = target;
            surface_put(d, surface);

            pg->surface_binding_dim.width = target.width;
            pg->surface_binding_dim.clip_x = target.shape.clip_x;
            pg->surface_binding_dim.clip_width = target.shape.clip_width;
            pg->surface_binding_dim.height = target.height;
            pg->surface_binding_dim.clip_y = target.shape.clip_y;
            pg->surface_binding_dim.clip_height = target.shape.clip_height;

            if (color && r->zeta_binding &&
                (r->zeta_binding->width != target.width ||
                 r->zeta_binding->height != target.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

        void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                         const char *layout, uint32_t anti_aliasing,
                         uint32_t clip_x, uint32_t clip_width, uint32_t clip_y,
                         uint32_t clip_height, uint32_t pitch) =
            color ? (should_create ? trace_nv2a_pgraph_surface_create_color :
                                     trace_nv2a_pgraph_surface_hit_color) :
                    (should_create ? trace_nv2a_pgraph_surface_create_zeta :
                                     trace_nv2a_pgraph_surface_hit_zeta);
        trace_fn(surface->vram_addr, surface->width, surface->height,
                 surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                 surface->shape.clip_x, surface->shape.clip_width,
                 surface->shape.clip_y, surface->shape.clip_height,
                 surface->pitch);

        bind_surface(r, surface);
        pg_surface->buffer_dirty = false;
    }

    if (!upload && pg_surface->draw_dirty) {
        if (!tcg_enabled()) {
            /* Cannot monitor for reads/writes; flush now */
            download_surface(d, color ? r->color_binding : r->zeta_binding,
                             true);
        }

        pg_surface->write_enabled_cache = false;
        pg_surface->draw_dirty = false;
    }
}

void pgraph_wgpu_surface_update(NV2AState *d, bool upload, bool color_write,
                                bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write =
        color_write && (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);
        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache) &&
            pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache) &&
            pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    if (upload) {
        pg->draw_time++;
    }

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    if (r->color_binding) {
        r->color_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_wgpu_upload_surface_data(d, r->color_binding, false);
            r->color_binding->draw_time = pg->draw_time;
            r->color_binding->swizzle = swizzle;
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_wgpu_upload_surface_data(d, r->zeta_binding, false);
            r->zeta_binding->draw_time = pg->draw_time;
            r->zeta_binding->swizzle = swizzle;
        }
    }

    if (r->color_binding && r->zeta_binding) {
        assert(r->color_binding->width == r->zeta_binding->width);
        assert(r->color_binding->height == r->zeta_binding->height);
    }

    expire_old_surfaces(d);
    prune_invalid_surfaces(r, num_invalid_surfaces_to_keep);
}

void pgraph_wgpu_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

static void create_depth_upload_pipeline(PGRAPHWgpuState *r)
{
    r->depth_upload_module =
        pgraph_wgpu_create_wgsl_module(r, "Depth upload", depth_upload_wgsl);

    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor bgl_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.entryCount = 1;
    bgl_desc.entries = &entry;
    r->depth_upload_bind_group_layout =
        wgpuDeviceCreateBindGroupLayout(r->device, &bgl_desc);

    WGPUPipelineLayoutDescriptor layout_desc =
        WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &r->depth_upload_bind_group_layout;
    WGPUPipelineLayout layout =
        wgpuDeviceCreatePipelineLayout(r->device, &layout_desc);

    WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth_stencil.format = WGPUTextureFormat_Depth32FloatStencil8;
    depth_stencil.depthWriteEnabled = WGPUOptionalBool_True;
    depth_stencil.depthCompare = WGPUCompareFunction_Always;
    depth_stencil.stencilReadMask = 0;
    depth_stencil.stencilWriteMask = 0;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = r->depth_upload_module;
    fragment.entryPoint = pgraph_wgpu_str("fs");

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Depth upload");
    desc.layout = layout;
    desc.vertex.module = r->depth_upload_module;
    desc.vertex.entryPoint = pgraph_wgpu_str("vs");
    desc.depthStencil = &depth_stencil;
    desc.fragment = &fragment;
    r->depth_upload_pipeline = wgpuDeviceCreateRenderPipeline(r->device, &desc);

    wgpuPipelineLayoutRelease(layout);
}

void pgraph_wgpu_init_surfaces(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    QTAILQ_INIT(&r->surfaces);
    QTAILQ_INIT(&r->invalid_surfaces);

    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    qemu_event_init(&r->dirty_surfaces_download_complete, false);

    r->color_binding = NULL;
    r->zeta_binding = NULL;
    r->framebuffer_dirty = true;

    create_depth_upload_pipeline(r);
    pgraph_wgpu_init_surface_compute(r);
    pgraph_wgpu_reload_surface_scale_factor(pg);
}

void pgraph_wgpu_finalize_surfaces(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pgraph_wgpu_surface_flush(container_of(pg, NV2AState, pgraph));

    if (r->depth_upload_pipeline) {
        wgpuRenderPipelineRelease(r->depth_upload_pipeline);
        r->depth_upload_pipeline = NULL;
    }
    if (r->depth_upload_bind_group_layout) {
        wgpuBindGroupLayoutRelease(r->depth_upload_bind_group_layout);
        r->depth_upload_bind_group_layout = NULL;
    }
    if (r->depth_upload_module) {
        wgpuShaderModuleRelease(r->depth_upload_module);
        r->depth_upload_module = NULL;
    }
    pgraph_wgpu_finalize_surface_compute(r);
    if (r->readback_buffer) {
        wgpuBufferRelease(r->readback_buffer);
        r->readback_buffer = NULL;
        r->readback_buffer_size = 0;
    }
    qemu_event_destroy(&r->downloads_complete);
    qemu_event_destroy(&r->dirty_surfaces_download_complete);
}

void pgraph_wgpu_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    /* Clear last surface shape to force recreation of buffers at next draw */
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    unbind_surface(d, true);
    unbind_surface(d, false);

    g_autoptr(GPtrArray) list = g_ptr_array_new();
    SurfaceBinding *s, *next;

    QTAILQ_FOREACH (s, &r->surfaces, entry) {
        if (s->draw_dirty) {
            g_ptr_array_add(list, s);
        }
    }
    download_surface_list(d, (SurfaceBinding **)list->pdata, list->len);

    QTAILQ_FOREACH_SAFE (s, &r->surfaces, entry, next) {
        invalidate_surface(d, s);
    }
    prune_invalid_surfaces(r, 0);
    pgraph_wgpu_reload_surface_scale_factor(pg);
}
