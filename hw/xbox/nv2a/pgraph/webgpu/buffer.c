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
#include "qemu/bitmap.h"
#include "renderer.h"

/* Dirty vertex pages are uploaded in blocks of this size, so several
 * nearby pages share one wgpuQueueWriteBuffer. */
#define VERTEX_RAM_GRAIN (64 * 1024)

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

    WGPULimits limits = WGPU_LIMITS_INIT;
    r->max_vertex_buffers = 8;
    r->max_vertex_buffer_array_stride = 2048;
    uint64_t max_buffer = (uint64_t)256 * MiB;
    if (wgpuDeviceGetLimits(r->device, &limits) == WGPUStatus_Success) {
        if (limits.maxVertexBuffers) {
            r->max_vertex_buffers = limits.maxVertexBuffers;
        }
        if (limits.maxVertexBufferArrayStride) {
            r->max_vertex_buffer_array_stride =
                limits.maxVertexBufferArrayStride;
        }
        if (limits.maxBufferSize) {
            max_buffer = limits.maxBufferSize;
        }
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    uint64_t vram = memory_region_size(d->vram);
    if (!vram || vram > max_buffer) {
        return;
    }

    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Vertex RAM");
    desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    desc.size = vram;
    r->vertex_ram = wgpuDeviceCreateBuffer(r->device, &desc);
    if (!r->vertex_ram) {
        return;
    }
    r->vertex_ram_bytes = vram;
    r->vertex_ram_valid = bitmap_new(vram / TARGET_PAGE_SIZE);
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
    if (r->vertex_ram) {
        wgpuBufferRelease(r->vertex_ram);
        r->vertex_ram = NULL;
    }
    g_free(r->vertex_ram_valid);
    r->vertex_ram_valid = NULL;
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

void pgraph_wgpu_vertex_ram_invalidate(PGRAPHState *pg, hwaddr addr,
                                       hwaddr size)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    size_t pages;

    if (!r->vertex_ram_valid || !size || addr >= r->vertex_ram_bytes) {
        return;
    }
    if (size > r->vertex_ram_bytes - addr) {
        size = r->vertex_ram_bytes - addr;
    }

    pages = r->vertex_ram_bytes / TARGET_PAGE_SIZE;
    size_t start = addr / TARGET_PAGE_SIZE;
    size_t end = (addr + size + TARGET_PAGE_SIZE - 1) / TARGET_PAGE_SIZE;
    if (end > pages) {
        end = pages;
    }
    if (end > start) {
        bitmap_clear(r->vertex_ram_valid, start, end - start);
    }
}

typedef struct RamSpan {
    uint64_t begin;
    uint64_t end;
} RamSpan;

static int compare_spans(const void *a, const void *b)
{
    const RamSpan *sa = a;
    const RamSpan *sb = b;

    if (sa->begin < sb->begin) {
        return -1;
    }
    if (sa->begin > sb->begin) {
        return 1;
    }
    return 0;
}

void pgraph_wgpu_vertex_ram_upload(PGRAPHState *pg,
                                   const WgpuMemoryRange *ranges, int count)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    g_autoptr(GArray) spans = g_array_new(false, false, sizeof(RamSpan));
    size_t pages;

    if (!r->vertex_ram || count <= 0) {
        return;
    }
    pages = r->vertex_ram_bytes / TARGET_PAGE_SIZE;

    for (int i = 0; i < count; i++) {
        hwaddr addr = ranges[i].addr;
        hwaddr size = ranges[i].size;

        if (!size || addr >= r->vertex_ram_bytes) {
            continue;
        }
        if (size > r->vertex_ram_bytes - addr) {
            size = r->vertex_ram_bytes - addr;
        }
        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A)) {
            pgraph_wgpu_vertex_ram_invalidate(pg, addr, size);
        }

        size_t page = addr / TARGET_PAGE_SIZE;
        size_t end = (addr + size + TARGET_PAGE_SIZE - 1) / TARGET_PAGE_SIZE;
        if (end > pages) {
            end = pages;
        }
        while (page < end) {
            size_t invalid = find_next_zero_bit(r->vertex_ram_valid, end, page);
            size_t next;
            uint64_t begin, span_end;
            RamSpan span;

            if (invalid >= end) {
                break;
            }
            next = find_next_bit(r->vertex_ram_valid, end, invalid);
            begin = ((uint64_t)invalid * TARGET_PAGE_SIZE) &
                    ~(uint64_t)(VERTEX_RAM_GRAIN - 1);
            span_end =
                ROUND_UP((uint64_t)next * TARGET_PAGE_SIZE, VERTEX_RAM_GRAIN);
            if (span_end > r->vertex_ram_bytes) {
                span_end = r->vertex_ram_bytes;
            }
            span.begin = begin;
            span.end = span_end;
            g_array_append_val(spans, span);
            page = next;
        }
    }

    if (!spans->len) {
        return;
    }
    qsort(spans->data, spans->len, sizeof(RamSpan), compare_spans);

    uint32_t merged = 1;
    for (guint i = 1; i < spans->len; i++) {
        RamSpan *prev = &g_array_index(spans, RamSpan, merged - 1);
        RamSpan *span = &g_array_index(spans, RamSpan, i);

        if (span->begin <= prev->end) {
            prev->end = MAX(prev->end, span->end);
        } else {
            g_array_index(spans, RamSpan, merged) = *span;
            merged++;
        }
    }

    /* A draw already recorded against this buffer must be submitted before
     * the write, or it would sample the new bytes. */
    if (r->vertex_ram_drawn) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_VERTEX_BUFFER_DIRTY, false);
        r->vertex_ram_drawn = false;
    }

    for (uint32_t i = 0; i < merged; i++) {
        RamSpan *span = &g_array_index(spans, RamSpan, i);
        uint64_t size = span->end - span->begin;
        size_t start_page, end_page;

        if (!size) {
            continue;
        }
        wgpuQueueWriteBuffer(r->queue, r->vertex_ram, span->begin,
                             d->vram_ptr + span->begin, size);
        start_page = span->begin / TARGET_PAGE_SIZE;
        end_page = span->end / TARGET_PAGE_SIZE;
        if (end_page > start_page) {
            bitmap_set(r->vertex_ram_valid, start_page, end_page - start_page);
        }
    }
}
