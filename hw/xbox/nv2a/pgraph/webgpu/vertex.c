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

#include "qemu/osdep.h"
#include "renderer.h"

/*
 * WebGPU has no 3-component 8/16-bit or scaled-integer formats, and a
 * vertex stride must be a multiple of 4. Attributes that fit a core format
 * are fetched from a guest RAM mirror. Anything else is packed on the CPU
 * into one interleaved buffer, which is also how indexed point lists are
 * drawn: instance data cannot be indexed.
 */

static void add_attribute(PGRAPHWgpuState *r, int location, unsigned int format,
                          unsigned int count, const uint8_t *data,
                          size_t stride)
{
    WgpuVertexLayout *layout = &r->vertex_layout;
    WgpuVertexAttribute *a = &layout->attributes[layout->num_attributes];
    WgpuVertexSource *src = &r->vertex_sources[layout->num_attributes];
    unsigned int size;

    switch (format) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        assert(count >= 1 && count <= 4);
        a->format = count == 1 ? WGPUVertexFormat_Unorm8 :
                    count == 2 ? WGPUVertexFormat_Unorm8x2 :
                                 WGPUVertexFormat_Unorm8x4;
        size = 4;
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
        assert(count >= 1 && count <= 4);
        a->format = count == 1 ? WGPUVertexFormat_Snorm16 :
                    count == 2 ? WGPUVertexFormat_Snorm16x2 :
                                 WGPUVertexFormat_Snorm16x4;
        size = count <= 2 ? 4 : 8;
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
        assert(count >= 1 && count <= 4);
        a->format = WGPUVertexFormat_Float32 + count - 1;
        size = 4 * count;
        break;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
        /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
        assert(count == 1);
        a->format = WGPUVertexFormat_Sint32;
        size = 4;
        break;
    default:
        fprintf(stderr, "Unknown vertex type: 0x%x\n", format);
        assert(!"Unknown vertex type");
        return;
    }

    a->location = location;
    a->offset = layout->stride;
    layout->stride += size;
    layout->num_attributes++;

    src->data = data;
    src->stride = stride;
    src->format = format;
    src->count = count;
    src->ram_addr = ~(hwaddr)0;
}

static void reset_attributes(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pg->compressed_attrs = 0;
    pg->uniform_attrs = 0;
    pg->swizzle_attrs = 0;
    r->vertex_ram_eligible = false;
    memset(&r->vertex_layout, 0, sizeof(r->vertex_layout));
}

static uint32_t vertex_format_bytes(WGPUVertexFormat format)
{
    switch (format) {
    case WGPUVertexFormat_Unorm8:
        return 1;
    case WGPUVertexFormat_Unorm8x2:
        return 2;
    case WGPUVertexFormat_Unorm8x4:
        return 4;
    case WGPUVertexFormat_Snorm16:
        return 2;
    case WGPUVertexFormat_Snorm16x2:
        return 4;
    case WGPUVertexFormat_Snorm16x4:
        return 8;
    case WGPUVertexFormat_Float32:
    case WGPUVertexFormat_Sint32:
        return 4;
    case WGPUVertexFormat_Float32x2:
        return 8;
    case WGPUVertexFormat_Float32x3:
        return 12;
    case WGPUVertexFormat_Float32x4:
        return 16;
    default:
        return 0;
    }
}

static bool attribute_fits_ram(const PGRAPHWgpuState *r,
                               const WgpuVertexAttribute *attr,
                               const WgpuVertexSource *src)
{
    uint32_t bytes = vertex_format_bytes(attr->format);

    if (src->ram_addr == ~(hwaddr)0 || !bytes || !src->stride) {
        return false;
    }
    if (src->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K) {
        return false;
    }
    if ((src->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D ||
         src->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL ||
         src->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1) &&
        src->count == 3) {
        return false;
    }
    if ((src->stride % 4) != 0 || (src->ram_addr % 4) != 0) {
        return false;
    }
    if (src->stride < bytes ||
        src->stride > r->max_vertex_buffer_array_stride) {
        return false;
    }
    return true;
}

static int compare_ranges(const void *a, const void *b)
{
    const WgpuMemoryRange *ra = a;
    const WgpuMemoryRange *rb = b;

    if (ra->addr < rb->addr) {
        return -1;
    }
    if (ra->addr > rb->addr) {
        return 1;
    }
    return 0;
}

/* Page-align, then merge overlapping ranges so one readback covers them. */
static int merge_vertex_ranges(WgpuMemoryRange *ranges, int count)
{
    int merged;

    if (count <= 0) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        hwaddr end = ranges[i].addr + ranges[i].size;

        ranges[i].addr &= TARGET_PAGE_MASK;
        end = ROUND_UP(end, TARGET_PAGE_SIZE);
        ranges[i].size = end - ranges[i].addr;
    }
    qsort(ranges, count, sizeof(ranges[0]), compare_ranges);

    merged = 1;
    for (int i = 1; i < count; i++) {
        WgpuMemoryRange *prev = &ranges[merged - 1];
        hwaddr prev_end = prev->addr + prev->size;
        hwaddr end = ranges[i].addr + ranges[i].size;

        if (ranges[i].addr <= prev_end) {
            prev->size = MAX(prev_end, end) - prev->addr;
        } else {
            ranges[merged++] = ranges[i];
        }
    }
    return merged;
}

bool pgraph_wgpu_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                        unsigned int max_element,
                                        bool inline_data,
                                        unsigned int inline_stride,
                                        unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    unsigned int num_elements = max_element - min_element + 1;

    reset_attributes(pg);

    if (!inline_data) {
        WgpuMemoryRange ranges[NV2A_VERTEXSHADER_ATTRIBUTES];
        int nranges = 0;

        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            hwaddr dma_len;
            uint8_t *attr_data;
            hwaddr start;

            if (!attr->count) {
                continue;
            }
            attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            start = attr_data + attr->offset - d->vram_ptr +
                    min_element * attr->stride;
            ranges[nranges].addr = start;
            ranges[nranges].size = num_elements * MAX(attr->stride, 1);
            nranges++;
        }
        nranges = merge_vertex_ranges(ranges, nranges);
        /* A draw_dirty surface under these vertices is read back once.
         * DIRTY_MEMORY_NV2A is a CPU write. The RAM mirror uploads those
         * pages when a draw fetches them. Semaphore release downloads
         * before the semaphore store. */
        pgraph_wgpu_download_surfaces_covering(pg, ranges, nranges);
    }

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (!attr->count) {
            pg->uniform_attrs |= 1 << i;
            continue;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);

        const uint8_t *data;
        size_t stride;
        if (inline_data) {
            data =
                (const uint8_t *)pg->inline_array + attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            data = attr_data + attr->offset;
            stride = attr->stride;
        }

        hwaddr ram_addr = ~(hwaddr)0;
        if (!inline_data && data >= d->vram_ptr &&
            (size_t)(data - d->vram_ptr) < memory_region_size(d->vram)) {
            ram_addr = data - d->vram_ptr;
        }

        if (!stride) {
            /* Stride of 0 indicates that only the first element should be
             * used. */
            pg->uniform_attrs |= 1 << i;
            pgraph_update_inline_value(attr, data);
            continue;
        }

        pgraph_update_inline_value(attr, data + stride * provoking_element);

        add_attribute(r, i, attr->format, attr->count, data, stride);
        r->vertex_sources[r->vertex_layout.num_attributes - 1].ram_addr =
            ram_addr;

        if (attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP) {
            pg->compressed_attrs |= 1 << i;
        }
        if (attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D) {
            pg->swizzle_attrs |= 1 << i;
        }
    }

    r->vertex_ram_eligible =
        r->vertex_ram && r->vertex_layout.num_attributes > 0 &&
        r->vertex_layout.num_attributes <= r->max_vertex_buffers;
    for (uint32_t i = 0; i < r->vertex_layout.num_attributes; i++) {
        if (!attribute_fits_ram(r, &r->vertex_layout.attributes[i],
                                &r->vertex_sources[i])) {
            r->vertex_ram_eligible = false;
            break;
        }
    }

    return r->vertex_layout.num_attributes > 0;
}

bool pgraph_wgpu_vertex_ram_prepare(PGRAPHState *pg, unsigned int first,
                                    unsigned int count, bool reorder)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuMemoryRange ranges[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint32_t n = r->vertex_layout.num_attributes;

    if (reorder || !r->vertex_ram_eligible || !count) {
        return false;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint64_t stride = r->vertex_sources[i].stride;
        uint64_t off = r->vertex_sources[i].ram_addr + (uint64_t)first * stride;
        uint64_t bytes = (uint64_t)count * stride;

        if (off > r->vertex_ram_bytes || bytes > r->vertex_ram_bytes - off) {
            return false;
        }
        ranges[i].addr = off;
        ranges[i].size = bytes;
    }

    for (uint32_t i = 0; i < n; i++) {
        r->vertex_layout.attributes[i].offset = 0;
        r->vertex_layout.attributes[i].stride = r->vertex_sources[i].stride;
        r->vertex_ram_offset[i] = ranges[i].addr;
        r->vertex_ram_size[i] = ranges[i].size;
    }
    r->vertex_layout.from_ram = true;
    pgraph_wgpu_vertex_ram_upload(pg, ranges, n);
    return true;
}

void pgraph_wgpu_bind_vertex_attributes_inline(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    reset_attributes(pg);

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->inline_buffer_populated) {
            add_attribute(r, i, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4,
                          (const uint8_t *)attr->inline_buffer,
                          4 * sizeof(float));
            memcpy(attr->inline_value,
                   attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                   sizeof(attr->inline_value));
        } else {
            pg->uniform_attrs |= 1 << i;
        }
    }
}

void pgraph_wgpu_pack_vertices(PGRAPHWgpuState *r, uint8_t *out,
                               unsigned int first, unsigned int count)
{
    const WgpuVertexLayout *layout = &r->vertex_layout;

    for (uint32_t a = 0; a < layout->num_attributes; a++) {
        const WgpuVertexSource *src = &r->vertex_sources[a];
        const uint8_t *in = src->data + first * src->stride;
        uint8_t *dst = out + layout->attributes[a].offset;

        for (unsigned int v = 0; v < count; v++) {
            switch (src->format) {
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
                memcpy(dst, in, src->count);
                if (src->count == 3) {
                    dst[3] = 0xff;
                }
                break;
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
                memcpy(dst, in, 2 * src->count);
                if (src->count == 3) {
                    int16_t one = INT16_MAX;
                    memcpy(dst + 6, &one, sizeof(one));
                }
                break;
            case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
                for (unsigned int c = 0; c < src->count; c++) {
                    int16_t s;
                    memcpy(&s, in + 2 * c, sizeof(s));
                    float f = s;
                    memcpy(dst + 4 * c, &f, sizeof(f));
                }
                break;
            default:
                memcpy(dst, in, 4 * src->count);
                break;
            }
            in += src->stride;
            dst += layout->stride;
        }
    }
}
