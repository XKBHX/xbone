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

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_CONSTANTS_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_CONSTANTS_H

#include "hw/xbox/nv2a/nv2a_regs.h"
#include <webgpu/webgpu.h>

typedef enum WgpuSurfaceConversion {
    WGPU_SURFACE_CONV_NONE,
    WGPU_SURFACE_CONV_R5G6B5,
    WGPU_SURFACE_CONV_X1R5G5B5,
    WGPU_SURFACE_CONV_Z16,
    WGPU_SURFACE_CONV_Z24S8,
} WgpuSurfaceConversion;

typedef struct BasicSurfaceFormatInfo {
    unsigned int bytes_per_pixel;
} BasicSurfaceFormatInfo;

typedef struct WgpuSurfaceFormatInfo {
    unsigned int host_bytes_per_pixel;
    WGPUTextureFormat format;
    WgpuSurfaceConversion conversion;
    bool depth;
} WgpuSurfaceFormatInfo;

static const BasicSurfaceFormatInfo kelvin_surface_color_format_map[] = {
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5] = { 2 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5] = { 2 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8] = { 4 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8] = { 4 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_B8] = { 1 },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8] = { 2 },
};

/* WebGPU cannot render to packed 16-bit formats, so those surfaces live in
 * rgba8unorm and are converted on the CPU during upload and download.
 */
static const WgpuSurfaceFormatInfo kelvin_surface_color_format_wgpu_map[] = {
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5] = {
        4, WGPUTextureFormat_RGBA8Unorm, WGPU_SURFACE_CONV_X1R5G5B5, false,
    },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5] = {
        4, WGPUTextureFormat_RGBA8Unorm, WGPU_SURFACE_CONV_R5G6B5, false,
    },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8] = {
        4, WGPUTextureFormat_BGRA8Unorm, WGPU_SURFACE_CONV_NONE, false,
    },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8] = {
        4, WGPUTextureFormat_BGRA8Unorm, WGPU_SURFACE_CONV_NONE, false,
    },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_B8] = {
        1, WGPUTextureFormat_R8Unorm, WGPU_SURFACE_CONV_NONE, false,
    },
    [NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8] = {
        2, WGPUTextureFormat_RG8Unorm, WGPU_SURFACE_CONV_NONE, false,
    },
};

static const BasicSurfaceFormatInfo kelvin_surface_zeta_format_map[] = {
    [NV097_SET_SURFACE_FORMAT_ZETA_Z16] = { 2 },
    [NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] = { 4 },
};

/* Depth formats can't be copied to from buffers, and depth24plus can't be
 * copied from either, so both zeta formats use depth32float-stencil8.
 */
static const WgpuSurfaceFormatInfo kelvin_surface_zeta_format_wgpu_map[] = {
    [NV097_SET_SURFACE_FORMAT_ZETA_Z16] = {
        5, WGPUTextureFormat_Depth32FloatStencil8, WGPU_SURFACE_CONV_Z16, true,
    },
    [NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] = {
        5, WGPUTextureFormat_Depth32FloatStencil8, WGPU_SURFACE_CONV_Z24S8,
        true,
    },
};

#endif
