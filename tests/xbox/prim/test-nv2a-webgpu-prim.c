/*
 * Test NV2A WebGPU primitive assembly
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
#include "hw/xbox/nv2a/pgraph/webgpu/prim.h"

#define MAX_OUT 256

typedef struct PrimCase {
    const char *name;
    WgpuPrimState state;
    size_t count;
    WGPUPrimitiveTopology topology;
    bool expand;
    size_t num_expected;
    uint32_t expected[64];
} PrimCase;

#define SMOOTH(mode, poly) { mode, poly, false, true }
#define FLAT_FIRST(mode) { mode, POLY_MODE_FILL, true, true }
#define FLAT_LAST(mode) { mode, POLY_MODE_FILL, true, false }

#define TRI_LIST WGPUPrimitiveTopology_TriangleList
#define LINE_LIST WGPUPrimitiveTopology_LineList
#define POINT_LIST WGPUPrimitiveTopology_PointList

static const PrimCase cases[] = {
    /* Fill, smooth shading */
    { "fill/fan",
      SMOOTH(PRIM_TYPE_TRIANGLE_FAN, POLY_MODE_FILL),
      5,
      TRI_LIST,
      true,
      9,
      { 0, 1, 2, 0, 2, 3, 0, 3, 4 } },
    { "fill/polygon",
      SMOOTH(PRIM_TYPE_POLYGON, POLY_MODE_FILL),
      4,
      TRI_LIST,
      true,
      6,
      { 0, 1, 2, 0, 2, 3 } },
    { "fill/quads",
      SMOOTH(PRIM_TYPE_QUADS, POLY_MODE_FILL),
      9,
      TRI_LIST,
      true,
      12,
      { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 } },
    { "fill/quad-strip",
      SMOOTH(PRIM_TYPE_QUAD_STRIP, POLY_MODE_FILL),
      6,
      TRI_LIST,
      true,
      12,
      { 0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5 } },
    { "fill/line-loop",
      SMOOTH(PRIM_TYPE_LINE_LOOP, POLY_MODE_FILL),
      3,
      LINE_LIST,
      true,
      6,
      { 0, 1, 1, 2, 2, 0 } },
    { "fill/line-loop-2",
      SMOOTH(PRIM_TYPE_LINE_LOOP, POLY_MODE_FILL),
      2,
      LINE_LIST,
      true,
      2,
      { 0, 1 } },
    { "fill/strip", SMOOTH(PRIM_TYPE_TRIANGLE_STRIP, POLY_MODE_FILL), 5,
      WGPUPrimitiveTopology_TriangleStrip, false },
    { "fill/line-strip", SMOOTH(PRIM_TYPE_LINE_STRIP, POLY_MODE_FILL), 5,
      WGPUPrimitiveTopology_LineStrip, false },
    { "fill/triangles", SMOOTH(PRIM_TYPE_TRIANGLES, POLY_MODE_FILL), 6,
      TRI_LIST, false },
    { "fill/points", SMOOTH(PRIM_TYPE_POINTS, POLY_MODE_LINE), 6, POINT_LIST,
      false },
    { "fill/lines-ignore-poly-mode", SMOOTH(PRIM_TYPE_LINES, POLY_MODE_POINT),
      4, LINE_LIST, false },

    /* Flat shading, first vertex provoking */
    { "flat-first/triangles", FLAT_FIRST(PRIM_TYPE_TRIANGLES), 6, TRI_LIST,
      false },
    { "flat-first/lines", FLAT_FIRST(PRIM_TYPE_LINES), 4, LINE_LIST, false },
    { "flat-first/strip",
      FLAT_FIRST(PRIM_TYPE_TRIANGLE_STRIP),
      5,
      TRI_LIST,
      true,
      9,
      { 0, 1, 2, 1, 3, 2, 2, 3, 4 } },
    { "flat-first/fan",
      FLAT_FIRST(PRIM_TYPE_TRIANGLE_FAN),
      4,
      TRI_LIST,
      true,
      6,
      { 1, 2, 0, 2, 3, 0 } },
    { "flat-first/polygon",
      FLAT_FIRST(PRIM_TYPE_POLYGON),
      4,
      TRI_LIST,
      true,
      6,
      { 0, 1, 2, 0, 2, 3 } },
    { "flat-first/quads",
      FLAT_FIRST(PRIM_TYPE_QUADS),
      4,
      TRI_LIST,
      true,
      6,
      { 3, 1, 2, 3, 0, 1 } },
    { "flat-first/quad-strip",
      FLAT_FIRST(PRIM_TYPE_QUAD_STRIP),
      4,
      TRI_LIST,
      true,
      6,
      { 3, 0, 1, 3, 2, 0 } },

    /* Flat shading, last vertex provoking */
    { "flat-last/triangles",
      FLAT_LAST(PRIM_TYPE_TRIANGLES),
      6,
      TRI_LIST,
      true,
      6,
      { 2, 0, 1, 5, 3, 4 } },
    { "flat-last/strip",
      FLAT_LAST(PRIM_TYPE_TRIANGLE_STRIP),
      5,
      TRI_LIST,
      true,
      9,
      { 2, 0, 1, 3, 2, 1, 4, 2, 3 } },
    { "flat-last/fan",
      FLAT_LAST(PRIM_TYPE_TRIANGLE_FAN),
      4,
      TRI_LIST,
      true,
      6,
      { 2, 0, 1, 3, 0, 2 } },
    { "flat-last/polygon",
      FLAT_LAST(PRIM_TYPE_POLYGON),
      4,
      TRI_LIST,
      true,
      6,
      { 0, 1, 2, 0, 2, 3 } },
    { "flat-last/quads",
      FLAT_LAST(PRIM_TYPE_QUADS),
      4,
      TRI_LIST,
      true,
      6,
      { 3, 1, 2, 3, 0, 1 } },
    { "flat-last/lines",
      FLAT_LAST(PRIM_TYPE_LINES),
      5,
      LINE_LIST,
      true,
      4,
      { 1, 0, 3, 2 } },
    { "flat-last/line-strip",
      FLAT_LAST(PRIM_TYPE_LINE_STRIP),
      3,
      LINE_LIST,
      true,
      4,
      { 1, 0, 2, 1 } },
    { "flat-last/line-loop",
      FLAT_LAST(PRIM_TYPE_LINE_LOOP),
      3,
      LINE_LIST,
      true,
      6,
      { 1, 0, 2, 1, 0, 2 } },

    /* Line polygon mode */
    { "line/triangles",
      SMOOTH(PRIM_TYPE_TRIANGLES, POLY_MODE_LINE),
      3,
      LINE_LIST,
      true,
      6,
      { 0, 1, 1, 2, 2, 0 } },
    { "line/strip",
      SMOOTH(PRIM_TYPE_TRIANGLE_STRIP, POLY_MODE_LINE),
      4,
      LINE_LIST,
      true,
      12,
      { 0, 1, 1, 2, 2, 0, 1, 3, 3, 2, 2, 1 } },
    { "line/fan",
      SMOOTH(PRIM_TYPE_TRIANGLE_FAN, POLY_MODE_LINE),
      4,
      LINE_LIST,
      true,
      12,
      { 0, 1, 1, 2, 2, 0, 0, 2, 2, 3, 3, 0 } },
    { "line/quads",
      SMOOTH(PRIM_TYPE_QUADS, POLY_MODE_LINE),
      4,
      LINE_LIST,
      true,
      8,
      { 0, 1, 1, 2, 2, 3, 3, 0 } },
    { "line/quad-strip",
      SMOOTH(PRIM_TYPE_QUAD_STRIP, POLY_MODE_LINE),
      4,
      LINE_LIST,
      true,
      8,
      { 0, 1, 1, 3, 3, 2, 2, 0 } },
    { "line/polygon",
      SMOOTH(PRIM_TYPE_POLYGON, POLY_MODE_LINE),
      4,
      LINE_LIST,
      true,
      8,
      { 0, 1, 1, 2, 2, 3, 3, 0 } },

    /* Point polygon mode */
    { "point/triangles",
      SMOOTH(PRIM_TYPE_TRIANGLES, POLY_MODE_POINT),
      3,
      POINT_LIST,
      true,
      3,
      { 0, 1, 2 } },
    { "point/fan",
      SMOOTH(PRIM_TYPE_TRIANGLE_FAN, POLY_MODE_POINT),
      4,
      POINT_LIST,
      true,
      6,
      { 0, 1, 2, 0, 2, 3 } },
    { "point/quads",
      SMOOTH(PRIM_TYPE_QUADS, POLY_MODE_POINT),
      4,
      POINT_LIST,
      true,
      4,
      { 0, 1, 2, 3 } },
    { "point/quad-strip",
      SMOOTH(PRIM_TYPE_QUAD_STRIP, POLY_MODE_POINT),
      6,
      POINT_LIST,
      true,
      8,
      { 0, 1, 2, 3, 2, 3, 4, 5 } },
    { "point/polygon",
      SMOOTH(PRIM_TYPE_POLYGON, POLY_MODE_POINT),
      4,
      POINT_LIST,
      true,
      4,
      { 0, 1, 2, 3 } },

    /* Too few vertices */
    { "short/fan", SMOOTH(PRIM_TYPE_TRIANGLE_FAN, POLY_MODE_FILL), 2, TRI_LIST,
      true, 0 },
    { "short/quads", SMOOTH(PRIM_TYPE_QUADS, POLY_MODE_FILL), 3, TRI_LIST, true,
      0 },
    { "short/line-loop", SMOOTH(PRIM_TYPE_LINE_LOOP, POLY_MODE_FILL), 1,
      LINE_LIST, true, 0 },
    { "short/polygon-line", SMOOTH(PRIM_TYPE_POLYGON, POLY_MODE_LINE), 2,
      LINE_LIST, true, 0 },
};

static void test_case(const void *opaque)
{
    const PrimCase *c = opaque;
    bool expand;

    g_assert_cmpint(pgraph_wgpu_prim_topology(&c->state, &expand), ==,
                    c->topology);
    g_assert_cmpint(expand, ==, c->expand);
    if (!expand) {
        return;
    }

    size_t max = pgraph_wgpu_prim_max_indices(&c->state, c->count);
    g_assert_cmpuint(max, <=, MAX_OUT);

    uint32_t out[MAX_OUT];
    size_t n = pgraph_wgpu_prim_expand(&c->state, NULL, 0, c->count, out);
    g_assert_cmpuint(n, <=, max);
    g_assert_cmpmem(out, n * sizeof(uint32_t), c->expected,
                    c->num_expected * sizeof(uint32_t));

    /* Index lists map through in[] and ignore base */
    uint32_t in[MAX_OUT];
    for (size_t i = 0; i < c->count; i++) {
        in[i] = 100 + 2 * i;
    }
    n = pgraph_wgpu_prim_expand(&c->state, in, 7, c->count, out);
    g_assert_cmpuint(n, ==, c->num_expected);
    for (size_t i = 0; i < n; i++) {
        g_assert_cmpuint(out[i], ==, 100 + 2 * c->expected[i]);
    }

    /* Arrays add base */
    n = pgraph_wgpu_prim_expand(&c->state, NULL, 40, c->count, out);
    g_assert_cmpuint(n, ==, c->num_expected);
    for (size_t i = 0; i < n; i++) {
        g_assert_cmpuint(out[i], ==, 40 + c->expected[i]);
    }
}

/* Signed area of the triangle in a 2D grid with vertex i at (i / 2, i % 2),
 * which lays out quad strips and triangle strips as a ribbon. */
static int ribbon_area(uint32_t a, uint32_t b, uint32_t c)
{
    int ax = a / 2, ay = a % 2, bx = b / 2, by = b % 2, cx = c / 2, cy = c % 2;
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* Every triangle of a strip or quad strip has the winding of the first one,
 * whatever the shading mode. */
static void test_ribbon_winding(void)
{
    static const int modes[] = { PRIM_TYPE_TRIANGLE_STRIP,
                                 PRIM_TYPE_QUAD_STRIP };
    static const WgpuPrimState shading[] = {
        { 0, POLY_MODE_FILL, true, true },
        { 0, POLY_MODE_FILL, true, false },
        { 0, POLY_MODE_FILL, false, true },
    };

    for (int m = 0; m < ARRAY_SIZE(modes); m++) {
        for (int s = 0; s < ARRAY_SIZE(shading); s++) {
            WgpuPrimState st = shading[s];
            st.primitive_mode = modes[m];
            uint32_t out[MAX_OUT];
            size_t n = pgraph_wgpu_prim_expand(&st, NULL, 0, 10, out);
            g_assert_cmpuint(n, >, 0);
            g_assert_cmpuint(n % 3, ==, 0);
            int sign = ribbon_area(out[0], out[1], out[2]) > 0 ? 1 : -1;
            for (size_t i = 0; i < n; i += 3) {
                int area = ribbon_area(out[i], out[i + 1], out[i + 2]);
                g_assert_cmpint(area * sign, >, 0);
            }
        }
    }
}

/* max_indices bounds expand for every mode and count */
static void test_max_indices(void)
{
    for (int mode = PRIM_TYPE_POINTS; mode <= PRIM_TYPE_POLYGON; mode++) {
        for (int poly = POLY_MODE_FILL; poly <= POLY_MODE_LINE; poly++) {
            for (int flat = 0; flat < 2; flat++) {
                for (int first = 0; first < 2; first++) {
                    WgpuPrimState st = { mode, poly, flat, first };
                    bool expand;
                    pgraph_wgpu_prim_topology(&st, &expand);
                    if (!expand) {
                        continue;
                    }
                    for (size_t count = 0; count < 24; count++) {
                        uint32_t out[MAX_OUT];
                        size_t max = pgraph_wgpu_prim_max_indices(&st, count);
                        g_assert_cmpuint(max, <=, MAX_OUT);
                        size_t n =
                            pgraph_wgpu_prim_expand(&st, NULL, 0, count, out);
                        g_assert_cmpuint(n, <=, max);
                    }
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *path =
            g_strdup_printf("/nv2a/webgpu/prim/%s", cases[i].name);
        g_test_add_data_func(path, &cases[i], test_case);
    }
    g_test_add_func("/nv2a/webgpu/prim/ribbon-winding", test_ribbon_winding);
    g_test_add_func("/nv2a/webgpu/prim/max-indices", test_max_indices);

    return g_test_run();
}
