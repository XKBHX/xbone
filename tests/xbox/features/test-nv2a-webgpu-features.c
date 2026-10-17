/*
 * Test NV2A WebGPU optional-feature selection
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/webgpu/features.h"

static void test_feature_disabled(void)
{
    g_assert_false(
        pgraph_wgpu_feature_disabled(NULL, WGPU_FEATURE_DEPTH32FLOAT_STENCIL8));
    g_assert_false(
        pgraph_wgpu_feature_disabled("", WGPU_FEATURE_DEPTH32FLOAT_STENCIL8));
    g_assert_true(
        pgraph_wgpu_feature_disabled(WGPU_FEATURE_DEPTH32FLOAT_STENCIL8,
                                     WGPU_FEATURE_DEPTH32FLOAT_STENCIL8));
    g_assert_true(pgraph_wgpu_feature_disabled(
        "texture-compression-bc, depth32float-stencil8",
        WGPU_FEATURE_DEPTH32FLOAT_STENCIL8));
    g_assert_true(pgraph_wgpu_feature_disabled(
        "clip-distances,depth-clip-control", WGPU_FEATURE_DEPTH_CLIP_CONTROL));
    g_assert_false(pgraph_wgpu_feature_disabled(
        WGPU_FEATURE_DEPTH32FLOAT_STENCIL8, WGPU_FEATURE_DEPTH_CLIP_CONTROL));
    g_assert_false(pgraph_wgpu_feature_disabled(
        "depth32float-stencil", WGPU_FEATURE_DEPTH32FLOAT_STENCIL8));
}

static void test_zeta_backing(void)
{
    g_assert_cmpint(pgraph_wgpu_zeta_backing(true, true), ==,
                    WGPU_ZETA_BACKING_DEPTH32_STENCIL8);
    g_assert_cmpint(pgraph_wgpu_zeta_backing(true, false), ==,
                    WGPU_ZETA_BACKING_DEPTH32_STENCIL8);
    g_assert_cmpint(pgraph_wgpu_zeta_backing(false, true), ==,
                    WGPU_ZETA_BACKING_DEPTH32);
    g_assert_cmpint(pgraph_wgpu_zeta_backing(false, false), ==,
                    WGPU_ZETA_BACKING_DEPTH24PLUS_STENCIL8);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/webgpu/features/disabled", test_feature_disabled);
    g_test_add_func("/nv2a/webgpu/features/zeta-backing", test_zeta_backing);
    return g_test_run();
}
