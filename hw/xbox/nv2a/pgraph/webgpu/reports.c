/*
 * Geforce NV2A PGRAPH WebGPU Renderer
 *
 * Copyright (c) 2026 xemu Project
 *
 * Based on the Vulkan implementation:
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "renderer.h"

/*
 * ZPASS_PIXEL_CNT. WebGPU occlusion queries count samples that pass the
 * depth and stencil tests, but the spec only requires zero versus non-zero.
 * Dawn's native backends generally return the real count. A binary result
 * is left as-is and then divided by the surface scale, which is wrong when
 * the scale is greater than 1. That gap is documented in the plan.
 *
 * A query has to begin and end inside one render pass. Ending the pass ends
 * the query, and the next counted draw starts another. The sums match a
 * Vulkan query that stayed open across those passes.
 */

void pgraph_wgpu_init_reports(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
    r->max_queries_in_flight = 1024;
    r->num_queries_in_flight = 0;
    r->new_query_needed = false;
    r->query_in_flight = false;
    r->resolve_queries = false;
    r->occlusion_collect_pending = false;
    r->occlusion_readback_pending = false;
    r->zpass_pixel_count_result = 0;

    WGPUQuerySetDescriptor query_desc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
    query_desc.label = pgraph_wgpu_str("ZPASS");
    query_desc.type = WGPUQueryType_Occlusion;
    query_desc.count = r->max_queries_in_flight;
    r->query_set = wgpuDeviceCreateQuerySet(r->device, &query_desc);

    uint64_t bytes = (uint64_t)r->max_queries_in_flight * sizeof(uint64_t);
    WGPUBufferDescriptor resolve_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    resolve_desc.label = pgraph_wgpu_str("ZPASS resolve");
    resolve_desc.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
    resolve_desc.size = bytes;
    r->query_resolve = wgpuDeviceCreateBuffer(r->device, &resolve_desc);

    WGPUBufferDescriptor readback_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    readback_desc.label = pgraph_wgpu_str("ZPASS readback");
    readback_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    readback_desc.size = bytes;
    r->query_readback = wgpuDeviceCreateBuffer(r->device, &readback_desc);
}

void pgraph_wgpu_finalize_reports(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    if (r->query_readback) {
        wgpuBufferRelease(r->query_readback);
        r->query_readback = NULL;
    }
    if (r->query_resolve) {
        wgpuBufferRelease(r->query_resolve);
        r->query_resolve = NULL;
    }
    if (r->query_set) {
        wgpuQuerySetRelease(r->query_set);
        r->query_set = NULL;
    }
}

void pgraph_wgpu_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    QueryReport *report = g_malloc(sizeof(QueryReport));
    report->clear = true;
    report->parameter = 0;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

void pgraph_wgpu_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    uint8_t type = GET_MASK(parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    QueryReport *report = g_malloc(sizeof(QueryReport));
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->num_queries_in_flight;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->new_query_needed = true;
}

void pgraph_wgpu_end_occlusion_query(PGRAPHWgpuState *r)
{
    if (!r->query_in_flight) {
        return;
    }
    wgpuRenderPassEncoderEndOcclusionQuery(r->render_pass);
    r->query_in_flight = false;
}

void pgraph_wgpu_reserve_occlusion_query(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (pg->clearing || !pg->zpass_pixel_count_enable) {
        return;
    }

    /* The open query already has a slot. A report boundary needs the next
     * slot, and so does a draw with no query open. */
    bool needs_new = !r->query_in_flight || r->new_query_needed;
    if (needs_new && r->num_queries_in_flight >= r->max_queries_in_flight) {
        r->resolve_queries = true;
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_STALLED, true);
    }
}

void pgraph_wgpu_prepare_occlusion_query(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!r->render_pass) {
        return;
    }

    if (pg->clearing || !pg->zpass_pixel_count_enable) {
        pgraph_wgpu_end_occlusion_query(r);
        return;
    }

    if (r->new_query_needed && r->query_in_flight) {
        pgraph_wgpu_end_occlusion_query(r);
    }
    if (r->query_in_flight) {
        return;
    }

    assert(r->num_queries_in_flight < r->max_queries_in_flight);
    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    wgpuRenderPassEncoderBeginOcclusionQuery(r->render_pass,
                                             r->num_queries_in_flight);
    r->num_queries_in_flight++;
    r->query_in_flight = true;
    r->new_query_needed = false;
}

void pgraph_wgpu_occlusion_before_submit(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!r->resolve_queries && QSIMPLEQ_EMPTY(&r->report_queue)) {
        return;
    }

    r->occlusion_collect_pending = true;
    r->occlusion_readback_pending = false;
    r->occlusion_readback_bytes = 0;
    if (!r->num_queries_in_flight) {
        return;
    }

    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    uint64_t bytes = (uint64_t)r->num_queries_in_flight * sizeof(uint64_t);
    wgpuCommandEncoderResolveQuerySet(encoder, r->query_set, 0,
                                      r->num_queries_in_flight,
                                      r->query_resolve, 0);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, r->query_resolve, 0,
                                         r->query_readback, 0, bytes);
    r->occlusion_readback_bytes = bytes;
    r->occlusion_readback_pending = true;
}

static void collect_reports(NV2AState *d, const uint64_t *query_results)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    int num_results_counted = 0;
    const int result_divisor =
        pg->surface_scale_factor * pg->surface_scale_factor;

    QueryReport *report;
    while ((report = QSIMPLEQ_FIRST(&r->report_queue)) != NULL) {
        assert(report->query_count >= (unsigned)num_results_counted);
        assert(report->query_count <= r->num_queries_in_flight);

        while (num_results_counted < (int)report->query_count) {
            r->zpass_pixel_count_result += query_results[num_results_counted++];
        }

        if (report->clear) {
            r->zpass_pixel_count_result = 0;
        } else {
            pgraph_write_zpass_pixel_cnt_report(d, report->parameter,
                                                r->zpass_pixel_count_result /
                                                    result_divisor);
        }

        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    while (num_results_counted < (int)r->num_queries_in_flight) {
        r->zpass_pixel_count_result += query_results[num_results_counted++];
    }

    r->num_queries_in_flight = 0;
    r->resolve_queries = false;
    r->occlusion_collect_pending = false;
    r->occlusion_readback_pending = false;
}

void pgraph_wgpu_occlusion_after_submit(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    NV2AState *d = container_of(pg, NV2AState, pgraph);

    if (!r->occlusion_collect_pending) {
        return;
    }

    if (r->occlusion_readback_pending) {
        const void *mapped = pgraph_wgpu_read_buffer(
            r, r->query_readback, r->occlusion_readback_bytes);
        if (!mapped) {
            /* Leave the queries and the queue so the next submit retries. */
            return;
        }
        collect_reports(d, mapped);
        wgpuBufferUnmap(r->query_readback);
        return;
    }

    assert(r->num_queries_in_flight == 0);
    collect_reports(d, NULL);
}

void pgraph_wgpu_process_pending_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    uint32_t *dma_get = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    uint32_t *dma_put = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];

    if (*dma_get == *dma_put && r->encoder &&
        !QSIMPLEQ_EMPTY(&r->report_queue)) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_STALLED, true);
    }
}
