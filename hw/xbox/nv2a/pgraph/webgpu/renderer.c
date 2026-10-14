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
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

static GPUProperties gpu_properties;

static void pgraph_wgpu_complete_sync(NV2AState *d)
{
    qatomic_set(&d->pgraph.sync_pending, false);
    qemu_event_set(&d->pgraph.sync_complete);
}

static void pgraph_wgpu_complete_flush(NV2AState *d)
{
    qatomic_set(&d->pgraph.flush_pending, false);
    qemu_event_set(&d->pgraph.flush_complete);
}

static void pgraph_wgpu_process_pending(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    if (!qatomic_read(&pg->sync_pending) && !qatomic_read(&pg->flush_pending)) {
        return;
    }

    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_mutex_lock(&pg->lock);
    if (qatomic_read(&pg->sync_pending)) {
        pgraph_wgpu_complete_sync(d);
    }
    if (qatomic_read(&pg->flush_pending)) {
        pgraph_wgpu_complete_flush(d);
    }
    qemu_mutex_unlock(&pg->lock);
    qemu_mutex_lock(&d->pfifo.lock);
}

static void pgraph_wgpu_init(NV2AState *d, Error **errp)
{
    PGRAPHState *pg = &d->pgraph;

    pg->wgpu_renderer_state = g_new0(PGRAPHWgpuState, 1);
    if (!pgraph_wgpu_init_instance(pg, errp)) {
        pgraph_wgpu_finalize_instance(pg);
        g_clear_pointer(&pg->wgpu_renderer_state, g_free);
    }
}

static void pgraph_wgpu_finalize(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    if (!pg->wgpu_renderer_state) {
        return;
    }
    pgraph_wgpu_finalize_instance(pg);
    g_clear_pointer(&pg->wgpu_renderer_state, g_free);
}

static void pgraph_wgpu_clear_report_value(NV2AState *d)
{
}

static void pgraph_wgpu_clear_surface(NV2AState *d, uint32_t parameter)
{
}

static void pgraph_wgpu_draw_begin(NV2AState *d)
{
}

static void pgraph_wgpu_draw_end(NV2AState *d)
{
}

static void pgraph_wgpu_flip_stall(NV2AState *d)
{
}

static void pgraph_wgpu_flush_draw(NV2AState *d)
{
}

static void pgraph_wgpu_get_report(NV2AState *d, uint32_t parameter)
{
    pgraph_write_zpass_pixel_cnt_report(d, parameter, 0);
}

static void pgraph_wgpu_image_blit(NV2AState *d)
{
}

static void pgraph_wgpu_pre_savevm_trigger(NV2AState *d)
{
}

static void pgraph_wgpu_pre_savevm_wait(NV2AState *d)
{
}

static void pgraph_wgpu_pre_shutdown_trigger(NV2AState *d)
{
}

static void pgraph_wgpu_pre_shutdown_wait(NV2AState *d)
{
}

static void pgraph_wgpu_process_pending_reports(NV2AState *d)
{
}

static void pgraph_wgpu_surface_update(NV2AState *d, bool upload,
                                       bool color_write, bool zeta_write)
{
}

static void pgraph_wgpu_set_surface_scale_factor(NV2AState *d,
                                                 unsigned int scale)
{
    d->pgraph.surface_scale_factor = scale;
}

static unsigned int pgraph_wgpu_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor;
}

static int pgraph_wgpu_get_framebuffer_surface(NV2AState *d)
{
    return 0;
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
