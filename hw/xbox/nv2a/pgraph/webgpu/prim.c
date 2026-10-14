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
#include "hw/xbox/nv2a/pgraph/vsh_regs.h"
#include "prim.h"

/*
 * WebGPU has no fans, quads, polygons or line loops. Those primitives are
 * rewritten into lists with the same winding as the GL definitions.
 * Polygon modes other than fill, and the NV2A provoking vertex for flat
 * shading, are not handled yet.
 */

WGPUPrimitiveTopology pgraph_wgpu_prim_topology(int primitive_mode,
                                                bool *expand)
{
    *expand = false;

    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return WGPUPrimitiveTopology_PointList;
    case PRIM_TYPE_LINES:
        return WGPUPrimitiveTopology_LineList;
    case PRIM_TYPE_LINE_STRIP:
        return WGPUPrimitiveTopology_LineStrip;
    case PRIM_TYPE_TRIANGLES:
        return WGPUPrimitiveTopology_TriangleList;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return WGPUPrimitiveTopology_TriangleStrip;
    case PRIM_TYPE_LINE_LOOP:
        *expand = true;
        return WGPUPrimitiveTopology_LineList;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
        *expand = true;
        return WGPUPrimitiveTopology_TriangleList;
    default:
        assert(!"Invalid primitive_mode");
        return WGPUPrimitiveTopology_Undefined;
    }
}

size_t pgraph_wgpu_prim_max_indices(int primitive_mode, size_t count)
{
    switch (primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
        return 2 * count;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
    case PRIM_TYPE_QUAD_STRIP:
        return 3 * count;
    case PRIM_TYPE_QUADS:
        return 6 * (count / 4);
    default:
        return count;
    }
}

size_t pgraph_wgpu_prim_expand(int primitive_mode, const uint32_t *in,
                               uint32_t base, size_t count, uint32_t *out)
{
#define V(i) (in ? in[i] : base + (uint32_t)(i))
    size_t n = 0;

    switch (primitive_mode) {
    case PRIM_TYPE_LINE_LOOP:
        if (count < 2) {
            break;
        }
        for (size_t i = 0; i < count; i++) {
            out[n++] = V(i);
            out[n++] = V((i + 1) % count);
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_POLYGON:
        for (size_t i = 1; i + 1 < count; i++) {
            out[n++] = V(0);
            out[n++] = V(i);
            out[n++] = V(i + 1);
        }
        break;
    case PRIM_TYPE_QUADS:
        for (size_t i = 0; i + 3 < count; i += 4) {
            out[n++] = V(i);
            out[n++] = V(i + 1);
            out[n++] = V(i + 2);
            out[n++] = V(i);
            out[n++] = V(i + 2);
            out[n++] = V(i + 3);
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (size_t i = 0; i + 3 < count; i += 2) {
            out[n++] = V(i);
            out[n++] = V(i + 1);
            out[n++] = V(i + 3);
            out[n++] = V(i);
            out[n++] = V(i + 3);
            out[n++] = V(i + 2);
        }
        break;
    default:
        assert(!"Primitive does not need expansion");
        break;
    }
#undef V

    return n;
}
