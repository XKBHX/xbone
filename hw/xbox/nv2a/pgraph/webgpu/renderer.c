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

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"
#include "ui/xui/wgpu-ui-pfifo.h"

static GPUProperties gpu_properties;

static void pgraph_wgpu_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_FLUSH, false);
    pgraph_wgpu_surface_flush(d);
    pgraph_wgpu_mark_textures_possibly_dirty(d, 0, memory_region_size(d->vram));
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        pg->texture_dirty[i] = true;
    }

    qatomic_set(&pg->flush_pending, false);
    qemu_event_set(&pg->flush_complete);
}

static void pgraph_wgpu_sync(NV2AState *d)
{
    pgraph_wgpu_render_display(&d->pgraph);
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_wgpu_process_pending(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (qatomic_read(&r->downloads_pending) ||
        qatomic_read(&r->download_dirty_surfaces_pending) ||
        qatomic_read(&pg->sync_pending) || qatomic_read(&pg->flush_pending) ||
        qatomic_read(&r->ui_pending)) {
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_mutex_lock(&pg->lock);
        if (qatomic_read(&r->downloads_pending)) {
            pgraph_wgpu_process_pending_downloads(d);
        }
        if (qatomic_read(&r->download_dirty_surfaces_pending)) {
            pgraph_wgpu_download_dirty_surfaces(d);
        }
        if (qatomic_read(&pg->sync_pending)) {
            pgraph_wgpu_sync(d);
        }
        if (qatomic_read(&pg->flush_pending)) {
            pgraph_wgpu_flush(d);
        }
        if (qatomic_read(&r->ui_pending)) {
            WGPUTextureView display = NULL;
            int display_width = 0;
            int display_height = 0;

            WGPUTextureView canvas = NULL;
            int canvas_width = 0;
            int canvas_height = 0;
            WGPUTextureFormat canvas_format = WGPUTextureFormat_Undefined;

            if (xemu_wgpu_ui_needs_display()) {
                r->display.sampleable = true;
                pgraph_wgpu_render_display(pg);
                if (r->display.presented && r->display.image_view) {
                    display = r->display.image_view;
                    display_width = r->display.width;
                    display_height = r->display.height;
                }
                r->display.sampleable = false;
            }
#ifdef __EMSCRIPTEN__
            /*
             * The composite above stays sampleable. The UI gamma blit
             * writes it into this canvas texture, and ImGui follows.
             */
            if (!pgraph_wgpu_display_canvas_acquire(r, &canvas, &canvas_format,
                                                    &canvas_width,
                                                    &canvas_height)) {
                canvas = NULL;
            }
#endif
            xemu_wgpu_ui_execute(r->instance, r->device, r->queue, display,
                                 display_width, display_height, canvas,
                                 canvas_width, canvas_height, canvas_format);
#ifdef __EMSCRIPTEN__
            pgraph_wgpu_display_canvas_release(r);
#endif
            qatomic_set(&r->ui_pending, false);
            qemu_event_set(&r->ui_complete);
        }
        qemu_mutex_unlock(&pg->lock);
        qemu_mutex_lock(&d->pfifo.lock);
    }
}

static void pgraph_wgpu_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->wgpu_renderer_state = g_new0(PGRAPHWgpuState, 1);
    qemu_event_init(&pg->wgpu_renderer_state->ui_complete, false);
    if (!pgraph_wgpu_init_instance(pg, errp)) {
        qemu_event_destroy(&pg->wgpu_renderer_state->ui_complete);
        pgraph_wgpu_finalize_instance(pg);
        g_clear_pointer(&pg->wgpu_renderer_state, g_free);
        return;
    }

    pgraph_wgpu_init_reports(pg);
    pgraph_wgpu_init_buffers(pg);
    pgraph_wgpu_init_textures(pg);
    pgraph_wgpu_init_shaders(pg);
    pgraph_wgpu_init_surfaces(pg);
    pgraph_wgpu_init_pipelines(pg);
    pgraph_wgpu_init_display(pg);
}

static void pgraph_wgpu_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    if (!pg->wgpu_renderer_state) {
        return;
    }
    pgraph_wgpu_finalize_display(pg);
    pgraph_wgpu_finalize_pipelines(pg);
    pgraph_wgpu_finalize_shaders(pg);
    pgraph_wgpu_finalize_textures(pg);
    pgraph_wgpu_finalize_surfaces(pg);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_FLUSH, true);
    pgraph_wgpu_finalize_reports(pg);
    pgraph_wgpu_finalize_buffers(pg);
    pgraph_wgpu_finalize_instance(pg);
    qemu_event_destroy(&pg->wgpu_renderer_state->ui_complete);
    g_clear_pointer(&pg->wgpu_renderer_state, g_free);
}

static void pgraph_wgpu_flip_stall(NV2AState *d)
{
    pgraph_wgpu_finish(&d->pgraph, WGPU_FINISH_REASON_FLIP_STALL, true);
    xemu_wgpu_wait_frame();
}

static void pgraph_wgpu_pre_savevm_trigger(NV2AState *d)
{
    PGRAPHWgpuState *r = d->pgraph.wgpu_renderer_state;

    qatomic_set(&r->download_dirty_surfaces_pending, true);
    qemu_event_reset(&r->dirty_surfaces_download_complete);
}

static void pgraph_wgpu_pre_savevm_wait(NV2AState *d)
{
    qemu_event_wait(
        &d->pgraph.wgpu_renderer_state->dirty_surfaces_download_complete);
}

static void pgraph_wgpu_pre_shutdown_trigger(NV2AState *d)
{
}

static void pgraph_wgpu_pre_shutdown_wait(NV2AState *d)
{
}

static int pgraph_wgpu_get_framebuffer_surface(NV2AState *d)
{
#ifdef __EMSCRIPTEN__
    /* The canvas present in process_pending is the browser framebuffer. */
    (void)d;
    return 0;
#else
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    qemu_mutex_lock(&d->pfifo.lock);

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);
    hwaddr addr = d->pcrtc.start + vga_display_params.line_offset;

    SurfaceBinding *surface = pgraph_wgpu_surface_get_within(d, addr);
    if (surface == NULL || !surface->color) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }

    surface->frame_time = pg->frame_time;

#ifndef CONFIG_OPENGL
    /* No UI GL context. The VGA path reads the surface back from VRAM. */
    qemu_mutex_unlock(&d->pfifo.lock);
    pgraph_wgpu_wait_for_surface_download(surface);
    return 0;
#else
    qemu_event_reset(&pg->sync_complete);
    qatomic_set(&pg->sync_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&pg->sync_complete);

    GLuint tex = pgraph_wgpu_present_display(r);
    if (tex) {
        return tex;
    }

    qemu_mutex_lock(&d->pfifo.lock);
    surface = pgraph_wgpu_surface_get_within(d, addr);
    qemu_mutex_unlock(&d->pfifo.lock);
    if (surface && surface->color) {
        pgraph_wgpu_wait_for_surface_download(surface);
    }
    return 0;
#endif
#endif
}

static GPUProperties *pgraph_wgpu_get_gpu_properties(void)
{
    return &gpu_properties;
}

static PGRAPHRenderer pgraph_wgpu_renderer = {
    .type = CONFIG_DISPLAY_RENDERER_WEBGPU,
    .name = "WebGPU",
    .ops = {
        .init = pgraph_wgpu_init,
        .finalize = pgraph_wgpu_finalize,
        .clear_report_value = pgraph_wgpu_clear_report_value,
        .clear_surface = pgraph_wgpu_clear_surface,
        .draw_begin = pgraph_wgpu_draw_begin,
        .draw_end = pgraph_wgpu_draw_end,
        .flip_stall = pgraph_wgpu_flip_stall,
        .flush_draw = pgraph_wgpu_flush_draw,
        .get_report = pgraph_wgpu_get_report,
        .image_blit = pgraph_wgpu_image_blit,
        .pre_savevm_trigger = pgraph_wgpu_pre_savevm_trigger,
        .pre_savevm_wait = pgraph_wgpu_pre_savevm_wait,
        .pre_shutdown_trigger = pgraph_wgpu_pre_shutdown_trigger,
        .pre_shutdown_wait = pgraph_wgpu_pre_shutdown_wait,
        .process_pending = pgraph_wgpu_process_pending,
        .process_pending_reports = pgraph_wgpu_process_pending_reports,
        .surface_update = pgraph_wgpu_surface_update,
        .set_surface_scale_factor = pgraph_wgpu_set_surface_scale_factor,
        .get_surface_scale_factor = pgraph_wgpu_get_surface_scale_factor,
        .get_framebuffer_surface = pgraph_wgpu_get_framebuffer_surface,
        .get_gpu_properties = pgraph_wgpu_get_gpu_properties,
    },
};

static void __attribute__((constructor)) register_renderer(void)
{
    pgraph_renderer_register(&pgraph_wgpu_renderer);
}

int nv2a_wgpu_display_size(int *width, int *height)
{
    NV2AState *d = g_nv2a;
    PGRAPHState *pg;
    VGADisplayParams params;
    SurfaceBinding *surface;
    unsigned int w = 0;
    unsigned int h = 0;

    if (!d || !d->pgraph.wgpu_renderer_state) {
        return 0;
    }
    pg = &d->pgraph;
    qemu_mutex_lock(&d->pfifo.lock);
    d->vga.get_params(&d->vga, &params);
    surface =
        pgraph_wgpu_surface_get_within(d, d->pcrtc.start + params.line_offset);
    if (!surface || !surface->color || !surface->width || !surface->height) {
        qemu_mutex_unlock(&d->pfifo.lock);
        return 0;
    }
    d->vga.get_resolution(&d->vga, (int *)&w, (int *)&h);
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        h *= 2;
    }
    pgraph_apply_scaling_factor(pg, &w, &h);
    qemu_mutex_unlock(&d->pfifo.lock);
    if (!w || !h) {
        return 0;
    }
    if (width) {
        *width = (int)w;
    }
    if (height) {
        *height = (int)h;
    }
    return 1;
}

bool nv2a_wgpu_submit_ui(void)
{
    NV2AState *d = g_nv2a;
    PGRAPHWgpuState *r;
    bool locked;

    if (!d || !d->pgraph.wgpu_renderer_state) {
        return false;
    }
    r = d->pgraph.wgpu_renderer_state;
    /*
     * Font rebuild and thumbnail readback run while the UI holds the BQL.
     * The PFIFO thread can need that lock inside the pusher, so drop it
     * for the wait. The frame present itself is already outside the BQL.
     */
    locked = bql_locked();
    if (locked) {
        bql_unlock();
    }
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_event_reset(&r->ui_complete);
    qatomic_set(&r->ui_pending, true);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&r->ui_complete);
    if (locked) {
        bql_lock();
    }
    return true;
}

void nv2a_wgpu_copy_adapter(char *vendor, size_t vendor_size, char *name,
                            size_t name_size, char *description,
                            size_t description_size)
{
    if (!g_nv2a || !g_nv2a->pgraph.wgpu_renderer_state) {
        if (vendor && vendor_size) {
            vendor[0] = '\0';
        }
        if (name && name_size) {
            name[0] = '\0';
        }
        if (description && description_size) {
            description[0] = '\0';
        }
        return;
    }
    pgraph_wgpu_copy_adapter_text(vendor, vendor_size, name, name_size,
                                  description, description_size);
}
