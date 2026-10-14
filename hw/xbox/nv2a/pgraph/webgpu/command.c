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
#include "renderer.h"

static const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [WGPU_FINISH_REASON_VERTEX_BUFFER_DIRTY] =
        NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [WGPU_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [WGPU_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [WGPU_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [WGPU_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [WGPU_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [WGPU_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [WGPU_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [WGPU_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
};

WGPUCommandEncoder pgraph_wgpu_ensure_encoder(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!r->encoder) {
        WGPUCommandEncoderDescriptor descriptor =
            WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
        descriptor.label = pgraph_wgpu_str("PGRAPH");
        r->encoder = wgpuDeviceCreateCommandEncoder(r->device, &descriptor);
        r->encoder_start_time = pg->draw_time;
    }
    return r->encoder;
}

void pgraph_wgpu_end_render_pass(PGRAPHWgpuState *r)
{
    if (r->render_pass) {
        wgpuRenderPassEncoderEnd(r->render_pass);
        wgpuRenderPassEncoderRelease(r->render_pass);
        r->render_pass = NULL;
    }
}

static void queue_work_done_callback(WGPUQueueWorkDoneStatus status,
                                     WGPUStringView message, void *userdata1,
                                     void *userdata2)
{
    if (status != WGPUQueueWorkDoneStatus_Success) {
        fprintf(stderr, "[WebGPU] Queue work done failed (%d): %.*s\n", status,
                (int)message.length, message.data);
    }
}

void pgraph_wgpu_wait_idle(PGRAPHWgpuState *r)
{
    WGPUQueueWorkDoneCallbackInfo callback =
        WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;

    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = queue_work_done_callback;
    pgraph_wgpu_wait(r, wgpuQueueOnSubmittedWorkDone(r->queue, callback));
}

void pgraph_wgpu_finish(PGRAPHState *pg, WgpuFinishReason why, bool wait)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r->encoder) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[why]);

        pgraph_wgpu_end_render_pass(r);

        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(r->encoder, NULL);
        wgpuCommandEncoderRelease(r->encoder);
        r->encoder = NULL;

        nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
        wgpuQueueSubmit(r->queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
        r->submit_count++;
    }

    if (wait) {
        pgraph_wgpu_wait_idle(r);
    }
}

typedef struct MapState {
    WGPUMapAsyncStatus status;
} MapState;

static void buffer_map_callback(WGPUMapAsyncStatus status,
                                WGPUStringView message, void *userdata1,
                                void *userdata2)
{
    MapState *state = userdata1;

    state->status = status;
    if (status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "[WebGPU] Buffer map failed (%d): %.*s\n", status,
                (int)message.length, message.data);
    }
}

const void *pgraph_wgpu_read_buffer(PGRAPHWgpuState *r, WGPUBuffer buffer,
                                    uint64_t size)
{
    MapState state = { 0 };
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;

    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = buffer_map_callback;
    callback.userdata1 = &state;
    WGPUFuture future =
        wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size, callback);
    if (!pgraph_wgpu_wait(r, future) ||
        state.status != WGPUMapAsyncStatus_Success) {
        return NULL;
    }
    return wgpuBufferGetConstMappedRange(buffer, 0, size);
}
