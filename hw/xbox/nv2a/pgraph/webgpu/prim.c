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
 * WebGPU has no geometry stage and no fans, quads, polygons or line loops.
 * Those primitives, and the line and point polygon modes, are rewritten into
 * lists that follow the geometry shaders in glsl/geom.c.
 *
 * Flat shaded outputs take the first vertex of each host primitive, so with
 * flat shading every triangle is rotated (keeping its winding) and every line
 * reversed as needed to put the NV2A provoking vertex first. Quads take
 * attributes from their fourth vertex, which the split along the 0-2 diagonal
 * leaves out of the first triangle, so flat quads are split along the other
 * diagonal.
 *
 * In line and point polygon modes the geometry shaders give every edge and
 * point the flat attributes of its triangle's provoking vertex. Index
 * rewriting cannot express that, so those use their own first vertex.
 */

static bool is_face(int primitive_mode)
{
    switch (primitive_mode) {
    case PRIM_TYPE_TRIANGLES:
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
        return true;
    default:
        return false;
    }
}

static bool last_provoking(const WgpuPrimState *s)
{
    return s->flat && !s->first_vertex_is_provoking;
}

WGPUPrimitiveTopology pgraph_wgpu_prim_topology(const WgpuPrimState *s,
                                                bool *expand)
{
    *expand = true;

    if (is_face(s->primitive_mode)) {
        switch (s->polygon_mode) {
        case POLY_MODE_POINT:
            return WGPUPrimitiveTopology_PointList;
        case POLY_MODE_LINE:
            return WGPUPrimitiveTopology_LineList;
        default:
            break;
        }
    }

    switch (s->primitive_mode) {
    case PRIM_TYPE_POINTS:
        *expand = false;
        return WGPUPrimitiveTopology_PointList;
    case PRIM_TYPE_LINES:
        *expand = last_provoking(s);
        return WGPUPrimitiveTopology_LineList;
    case PRIM_TYPE_LINE_STRIP:
        if (last_provoking(s)) {
            return WGPUPrimitiveTopology_LineList;
        }
        *expand = false;
        return WGPUPrimitiveTopology_LineStrip;
    case PRIM_TYPE_LINE_LOOP:
        return WGPUPrimitiveTopology_LineList;
    case PRIM_TYPE_TRIANGLES:
        *expand = last_provoking(s);
        return WGPUPrimitiveTopology_TriangleList;
    case PRIM_TYPE_TRIANGLE_STRIP:
        if (s->flat) {
            return WGPUPrimitiveTopology_TriangleList;
        }
        *expand = false;
        return WGPUPrimitiveTopology_TriangleStrip;
    case PRIM_TYPE_TRIANGLE_FAN:
    case PRIM_TYPE_QUADS:
    case PRIM_TYPE_QUAD_STRIP:
    case PRIM_TYPE_POLYGON:
        return WGPUPrimitiveTopology_TriangleList;
    default:
        assert(!"Invalid primitive_mode");
        *expand = false;
        return WGPUPrimitiveTopology_Undefined;
    }
}

size_t pgraph_wgpu_prim_max_indices(const WgpuPrimState *s, size_t count)
{
    size_t tris = 0, quads = 0;

    switch (s->primitive_mode) {
    case PRIM_TYPE_POINTS:
        return count;
    case PRIM_TYPE_LINES:
        return count & ~(size_t)1;
    case PRIM_TYPE_LINE_STRIP:
    case PRIM_TYPE_LINE_LOOP:
        return 2 * count;
    case PRIM_TYPE_TRIANGLES:
        tris = count / 3;
        break;
    case PRIM_TYPE_TRIANGLE_STRIP:
    case PRIM_TYPE_TRIANGLE_FAN:
        tris = count > 2 ? count - 2 : 0;
        break;
    case PRIM_TYPE_QUADS:
        quads = count / 4;
        break;
    case PRIM_TYPE_QUAD_STRIP:
        quads = count > 2 ? (count - 2) / 2 : 0;
        break;
    case PRIM_TYPE_POLYGON:
        if (s->polygon_mode == POLY_MODE_LINE) {
            return 2 * count;
        } else if (s->polygon_mode == POLY_MODE_POINT) {
            return count;
        }
        tris = count > 2 ? count - 2 : 0;
        break;
    default:
        return 0;
    }

    switch (s->polygon_mode) {
    case POLY_MODE_LINE:
        return 6 * tris + 8 * quads;
    case POLY_MODE_POINT:
        return 3 * tris + 4 * quads;
    default:
        return 3 * tris + 6 * quads;
    }
}

typedef struct Emitter {
    const WgpuPrimState *s;
    uint32_t *out;
    size_t n;
} Emitter;

static void emit(Emitter *e, uint32_t v)
{
    e->out[e->n++] = v;
}

static void emit_line(Emitter *e, uint32_t a, uint32_t b)
{
    if (last_provoking(e->s)) {
        emit(e, b);
        emit(e, a);
    } else {
        emit(e, a);
        emit(e, b);
    }
}

static void emit_edge(Emitter *e, uint32_t a, uint32_t b)
{
    emit(e, a);
    emit(e, b);
}

/* Emits triangle (a, b, c), where provoking (0-2) selects its provoking
 * vertex. */
static void emit_tri(Emitter *e, uint32_t a, uint32_t b, uint32_t c,
                     int provoking)
{
    uint32_t v[3] = { a, b, c };

    switch (e->s->polygon_mode) {
    case POLY_MODE_LINE:
        emit_edge(e, a, b);
        emit_edge(e, b, c);
        emit_edge(e, c, a);
        break;
    case POLY_MODE_POINT:
        emit(e, a);
        emit(e, b);
        emit(e, c);
        break;
    default:
        if (!e->s->flat) {
            provoking = 0;
        }
        for (int i = 0; i < 3; i++) {
            emit(e, v[(provoking + i) % 3]);
        }
        break;
    }
}

/* Emits quad (a, b, c, d) of a quad list. */
static void emit_quad(Emitter *e, uint32_t a, uint32_t b, uint32_t c,
                      uint32_t d)
{
    switch (e->s->polygon_mode) {
    case POLY_MODE_LINE:
        emit_edge(e, a, b);
        emit_edge(e, b, c);
        emit_edge(e, c, d);
        emit_edge(e, d, a);
        break;
    case POLY_MODE_POINT:
        emit(e, a);
        emit(e, b);
        emit(e, c);
        emit(e, d);
        break;
    default:
        if (e->s->flat) {
            emit_tri(e, b, c, d, 2);
            emit_tri(e, d, a, b, 0);
        } else {
            emit_tri(e, a, b, c, 0);
            emit_tri(e, a, c, d, 0);
        }
        break;
    }
}

/* Emits quad (a, b, c, d) of a quad strip, whose outline is a, b, d, c. */
static void emit_strip_quad(Emitter *e, uint32_t a, uint32_t b, uint32_t c,
                            uint32_t d)
{
    switch (e->s->polygon_mode) {
    case POLY_MODE_LINE:
        emit_edge(e, a, b);
        emit_edge(e, b, d);
        emit_edge(e, d, c);
        emit_edge(e, c, a);
        break;
    case POLY_MODE_POINT:
        emit(e, a);
        emit(e, b);
        emit(e, c);
        emit(e, d);
        break;
    default:
        if (e->s->flat) {
            emit_tri(e, a, b, d, 2);
            emit_tri(e, a, d, c, 1);
        } else {
            emit_tri(e, a, b, c, 0);
            emit_tri(e, c, b, d, 0);
        }
        break;
    }
}

size_t pgraph_wgpu_prim_expand(const WgpuPrimState *s, const uint32_t *in,
                               uint32_t base, size_t count, uint32_t *out)
{
#define V(i) (in ? in[i] : base + (uint32_t)(i))
    Emitter e = { .s = s, .out = out, .n = 0 };
    bool first = s->first_vertex_is_provoking;

    switch (s->primitive_mode) {
    case PRIM_TYPE_LINES:
        for (size_t i = 0; i + 1 < count; i += 2) {
            emit_line(&e, V(i), V(i + 1));
        }
        break;
    case PRIM_TYPE_LINE_STRIP:
        for (size_t i = 0; i + 1 < count; i++) {
            emit_line(&e, V(i), V(i + 1));
        }
        break;
    case PRIM_TYPE_LINE_LOOP:
        for (size_t i = 0; i + 1 < count; i++) {
            emit_line(&e, V(i), V(i + 1));
        }
        if (count > 2) {
            emit_line(&e, V(count - 1), V(0));
        }
        break;
    case PRIM_TYPE_TRIANGLES:
        for (size_t i = 0; i + 2 < count; i += 3) {
            emit_tri(&e, V(i), V(i + 1), V(i + 2), first ? 0 : 2);
        }
        break;
    case PRIM_TYPE_TRIANGLE_STRIP:
        for (size_t i = 0; i + 2 < count; i++) {
            if (i & 1) {
                emit_tri(&e, V(i), V(i + 2), V(i + 1), first ? 0 : 1);
            } else {
                emit_tri(&e, V(i), V(i + 1), V(i + 2), first ? 0 : 2);
            }
        }
        break;
    case PRIM_TYPE_TRIANGLE_FAN:
        for (size_t i = 1; i + 1 < count; i++) {
            emit_tri(&e, V(0), V(i), V(i + 1), first ? 1 : 2);
        }
        break;
    case PRIM_TYPE_QUADS:
        for (size_t i = 0; i + 3 < count; i += 4) {
            emit_quad(&e, V(i), V(i + 1), V(i + 2), V(i + 3));
        }
        break;
    case PRIM_TYPE_QUAD_STRIP:
        for (size_t i = 0; i + 3 < count; i += 2) {
            emit_strip_quad(&e, V(i), V(i + 1), V(i + 2), V(i + 3));
        }
        break;
    case PRIM_TYPE_POLYGON:
        if (s->polygon_mode == POLY_MODE_LINE) {
            for (size_t i = 0; count > 2 && i < count; i++) {
                emit_edge(&e, V(i), V((i + 1) % count));
            }
        } else if (s->polygon_mode == POLY_MODE_POINT) {
            for (size_t i = 0; i < count; i++) {
                emit(&e, V(i));
            }
        } else {
            for (size_t i = 1; i + 1 < count; i++) {
                emit_tri(&e, V(0), V(i), V(i + 1), 0);
            }
        }
        break;
    default:
        assert(!"Primitive does not need expansion");
        break;
    }
#undef V

    return e.n;
}
