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

/*
 * Per-batch streams are staged on the CPU and written with wgpuQueueWriteBuffer
 * immediately before the batch is submitted, so the writes are ordered after
 * all previously submitted work that may still read the old contents.
 */

static void create_stream_buffer(PGRAPHWgpuState *r, WgpuStreamBuffer *b,
                                 size_t size)
{
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;

    desc.label = pgraph_wgpu_str(b->label);
    desc.usage = b->usage | WGPUBufferUsage_CopyDst;
    desc.size = size;
    b->buffer = wgpuDeviceCreateBuffer(r->device, &desc);
    b->data = g_realloc(b->data, size);
    b->size = size;
    b->used = 0;
}

void pgraph_wgpu_init_buffers(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    static const struct {
        const char *label;
        WGPUBufferUsage usage;
        size_t size;
    } streams[WGPU_STREAM__COUNT] = {
        [WGPU_STREAM_UNIFORM] = { "Uniform stream", WGPUBufferUsage_Uniform,
                                  4 * MiB },
        [WGPU_STREAM_INDEX] = { "Index stream", WGPUBufferUsage_Index,
                                4 * MiB },
        [WGPU_STREAM_VERTEX] = { "Vertex stream", WGPUBufferUsage_Vertex,
                                 16 * MiB },
    };

    for (int i = 0; i < WGPU_STREAM__COUNT; i++) {
        WgpuStreamBuffer *b = &r->stream_buffers[i];
        b->label = streams[i].label;
        b->usage = streams[i].usage;
        create_stream_buffer(r, b, streams[i].size);
    }
}

void pgraph_wgpu_finalize_buffers(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < WGPU_STREAM__COUNT; i++) {
        WgpuStreamBuffer *b = &r->stream_buffers[i];
        if (b->buffer) {
            wgpuBufferRelease(b->buffer);
            b->buffer = NULL;
        }
        g_free(b->data);
        b->data = NULL;
    }
}

/* Alignment slack covered by every reservation */
#define STREAM_ALIGN_SLACK 256

bool pgraph_wgpu_stream_reserve(PGRAPHState *pg, int index, size_t size)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuStreamBuffer *b = &r->stream_buffers[index];

    size += STREAM_ALIGN_SLACK;
    if (b->used + size <= b->size) {
        return false;
    }

    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_NEED_BUFFER_SPACE, false);

    if (size > b->size) {
        /* The old buffer stays alive until the submitted work is done */
        wgpuBufferRelease(b->buffer);
        create_stream_buffer(r, b, pow2ceil(size));
    }
    return true;
}

size_t pgraph_wgpu_stream_alloc(PGRAPHWgpuState *r, int index, size_t size,
                                size_t align, void **ptr)
{
    WgpuStreamBuffer *b = &r->stream_buffers[index];

    assert(align <= STREAM_ALIGN_SLACK);
    size_t offset = ROUND_UP(b->used, align);
    assert(offset + size <= b->size);
    b->used = offset + size;
    *ptr = b->data + offset;
    return offset;
}

void pgraph_wgpu_flush_streams(PGRAPHWgpuState *r)
{
    for (int i = 0; i < WGPU_STREAM__COUNT; i++) {
        WgpuStreamBuffer *b = &r->stream_buffers[i];
        if (b->used) {
            wgpuQueueWriteBuffer(r->queue, b->buffer, 0, b->data,
                                 ROUND_UP(b->used, 4));
            b->used = 0;
        }
    }
    r->uniforms_valid = false;
}
