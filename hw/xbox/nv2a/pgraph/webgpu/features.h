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

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_FEATURES_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_FEATURES_H

#define WGPU_FEATURE_DEPTH32FLOAT_STENCIL8 "depth32float-stencil8"
#define WGPU_FEATURE_TEXTURE_COMPRESSION_BC "texture-compression-bc"
#define WGPU_FEATURE_CLIP_DISTANCES "clip-distances"
#define WGPU_FEATURE_DEPTH_CLIP_CONTROL "depth-clip-control"

/* Comma-separated names. Spaces around commas are ignored. */
bool pgraph_wgpu_feature_disabled(const char *list, const char *name);

typedef enum WgpuZetaBacking {
    /* Sampleable depth and stencil. Requires depth32float-stencil8. */
    WGPU_ZETA_BACKING_DEPTH32_STENCIL8,
    /* Z16 without that feature. depth32float is sampleable. */
    WGPU_ZETA_BACKING_DEPTH32,
    /* Z24S8 without that feature. Depth is copied to an r32float target. */
    WGPU_ZETA_BACKING_DEPTH24PLUS_STENCIL8,
} WgpuZetaBacking;

WgpuZetaBacking pgraph_wgpu_zeta_backing(bool depth32float_stencil8, bool z16);

#endif
