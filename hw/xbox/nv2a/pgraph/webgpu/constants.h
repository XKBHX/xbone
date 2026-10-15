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
 * rgba8unorm and are packed and unpacked by the compute kernels.
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

/* WebGPU has no constant-alpha factors. They use the blend constant, which is
 * set to the constant alpha splatted across all channels. */
static const WGPUBlendFactor pgraph_blend_factor_wgpu_map[] = {
    WGPUBlendFactor_Zero,
    WGPUBlendFactor_One,
    WGPUBlendFactor_Src,
    WGPUBlendFactor_OneMinusSrc,
    WGPUBlendFactor_SrcAlpha,
    WGPUBlendFactor_OneMinusSrcAlpha,
    WGPUBlendFactor_DstAlpha,
    WGPUBlendFactor_OneMinusDstAlpha,
    WGPUBlendFactor_Dst,
    WGPUBlendFactor_OneMinusDst,
    WGPUBlendFactor_SrcAlphaSaturated,
    WGPUBlendFactor_Zero,
    WGPUBlendFactor_Constant,
    WGPUBlendFactor_OneMinusConstant,
    WGPUBlendFactor_Constant,
    WGPUBlendFactor_OneMinusConstant,
};

#define PGRAPH_BLEND_FACTOR_CONSTANT_ALPHA 14
#define PGRAPH_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA 15

static const WGPUBlendOperation pgraph_blend_equation_wgpu_map[] = {
    WGPUBlendOperation_Subtract, WGPUBlendOperation_ReverseSubtract,
    WGPUBlendOperation_Add,      WGPUBlendOperation_Min,
    WGPUBlendOperation_Max,      WGPUBlendOperation_ReverseSubtract,
    WGPUBlendOperation_Add,
};

/* WebGPU cannot cull both faces; those draws are skipped instead. */
static const WGPUCullMode pgraph_cull_face_wgpu_map[] = {
    WGPUCullMode_None,
    WGPUCullMode_Front,
    WGPUCullMode_Back,
    WGPUCullMode_None,
};

static const WGPUCompareFunction pgraph_depth_func_wgpu_map[] = {
    WGPUCompareFunction_Never,        WGPUCompareFunction_Less,
    WGPUCompareFunction_Equal,        WGPUCompareFunction_LessEqual,
    WGPUCompareFunction_Greater,      WGPUCompareFunction_NotEqual,
    WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Always,
};

static const WGPUCompareFunction pgraph_stencil_func_wgpu_map[] = {
    WGPUCompareFunction_Never,        WGPUCompareFunction_Less,
    WGPUCompareFunction_Equal,        WGPUCompareFunction_LessEqual,
    WGPUCompareFunction_Greater,      WGPUCompareFunction_NotEqual,
    WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Always,
};

static const WGPUStencilOperation pgraph_stencil_op_wgpu_map[] = {
    WGPUStencilOperation_Keep,           WGPUStencilOperation_Keep,
    WGPUStencilOperation_Zero,           WGPUStencilOperation_Replace,
    WGPUStencilOperation_IncrementClamp, WGPUStencilOperation_DecrementClamp,
    WGPUStencilOperation_Invert,         WGPUStencilOperation_IncrementWrap,
    WGPUStencilOperation_DecrementWrap,
};

static const WGPUFilterMode pgraph_texture_min_filter_wgpu_map[] = {
    WGPUFilterMode_Nearest, WGPUFilterMode_Nearest, WGPUFilterMode_Linear,
    WGPUFilterMode_Nearest, WGPUFilterMode_Linear,  WGPUFilterMode_Nearest,
    WGPUFilterMode_Linear,  WGPUFilterMode_Linear,
};

static const WGPUFilterMode pgraph_texture_mag_filter_wgpu_map[] = {
    WGPUFilterMode_Nearest, WGPUFilterMode_Nearest, WGPUFilterMode_Linear,
    WGPUFilterMode_Nearest, WGPUFilterMode_Linear, /* FIXME: Convolution */
};

/* WebGPU has no clamp-to-border, so BORDER and CLAMP_OGL clamp to edge */
static const WGPUAddressMode pgraph_texture_addr_wgpu_map[] = {
    WGPUAddressMode_Repeat,       WGPUAddressMode_Repeat,
    WGPUAddressMode_MirrorRepeat, WGPUAddressMode_ClampToEdge,
    WGPUAddressMode_ClampToEdge,  WGPUAddressMode_ClampToEdge,
};

/* Layout of the texel data after the generic decoders in pgraph/texture.c
 * (palette, DXT, YUV, R6G5B5) have run. */
typedef enum WgpuTextureDecode {
    WGPU_TEX_DECODE_COPY, // Already in the host format
    WGPU_TEX_DECODE_R8,
    WGPU_TEX_DECODE_RG8,
    WGPU_TEX_DECODE_A1R5G5B5,
    WGPU_TEX_DECODE_R5G6B5,
    WGPU_TEX_DECODE_A4R4G4B4,
    WGPU_TEX_DECODE_BGRA8,
    WGPU_TEX_DECODE_RGBA8,
    WGPU_TEX_DECODE_RGB8_SNORM,
    WGPU_TEX_DECODE_R16,
} WgpuTextureDecode;

/* WebGPU core has no texture view swizzle, so the component mapping (the
 * VkComponentMapping in vk/constants.h) is applied while decoding. */
typedef enum WgpuSwizzle {
    WGPU_SWZ_ID,
    WGPU_SWZ_R,
    WGPU_SWZ_G,
    WGPU_SWZ_B,
    WGPU_SWZ_A,
    WGPU_SWZ_ZERO,
    WGPU_SWZ_ONE,
} WgpuSwizzle;

typedef struct WgpuColorFormatInfo {
    WgpuTextureDecode decode;
    WGPUTextureFormat format; // For WGPU_TEX_DECODE_COPY
    uint8_t swizzle[4];
} WgpuColorFormatInfo;

#define ID WGPU_SWZ_ID
#define R WGPU_SWZ_R
#define G WGPU_SWZ_G
#define B WGPU_SWZ_B
#define A WGPU_SWZ_A
#define ZERO WGPU_SWZ_ZERO
#define ONE WGPU_SWZ_ONE
#define COPY(fmt) { WGPU_TEX_DECODE_COPY, WGPUTextureFormat_##fmt }
#define DECODE(dec, ...)                                    \
    {                                                       \
        WGPU_TEX_DECODE_##dec, WGPUTextureFormat_Undefined, \
        {                                                   \
            __VA_ARGS__                                     \
        }                                                   \
    }

static const WgpuColorFormatInfo kelvin_color_format_wgpu_map[66] = {
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_Y8] = DECODE(R8, R, R, R, ONE),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_AY8] = DECODE(R8, R, R, R, R),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A1R5G5B5] = DECODE(A1R5G5B5),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X1R5G5B5] =
        DECODE(A1R5G5B5, ID, ID, ID, ONE),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4] = DECODE(A4R4G4B4),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5] = DECODE(R5G6B5),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8] = COPY(BGRA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8] =
        DECODE(BGRA8, ID, ID, ID, ONE),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8] = COPY(BGRA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5] = COPY(RGBA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8] = COPY(RGBA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8] = COPY(RGBA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5] = DECODE(A1R5G5B5),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5] = DECODE(R5G6B5),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8] = COPY(BGRA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8] = DECODE(R8, R, R, R, ONE),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_G8B8] = DECODE(RG8, R, G, R, G),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8] = DECODE(R8, ONE, ONE, ONE, R),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8Y8] = DECODE(RG8, R, R, R, G),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_AY8] = DECODE(R8, R, R, R, R),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5] =
        DECODE(A1R5G5B5, ID, ID, ID, ONE),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A4R4G4B4] = DECODE(A4R4G4B4),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8] =
        DECODE(BGRA8, ID, ID, ID, ONE),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8] = DECODE(R8, ONE, ONE, ONE, R),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8Y8] = DECODE(RG8, R, R, R, G),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R6G5B5] = DECODE(RGB8_SNORM),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_G8B8] = DECODE(RG8, R, G, R, G),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8B8] = DECODE(RG8, G, R, R, G),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_CR8YB8CB8YA8] = COPY(RGBA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LC_IMAGE_YB8CR8YA8CB8] = COPY(RGBA8Unorm),

    /* As in Vulkan, the pixel shader reads extra information from the depth
     * texture channels: R is the depth value, G is 0 for 16-bit and 1 for
     * 24-bit, B is 0 for fixed and 1 for float. The X8Y24 formats are sampled
     * as uint, and the shader extracts the 24-bit value itself. */
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED] =
        DECODE(R16, R, ZERO, ZERO, ZERO),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED] =
        COPY(R32Uint),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FLOAT] =
        COPY(R32Uint),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED] =
        DECODE(R16, R, ZERO, ZERO, ZERO),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT] =
        DECODE(R16, R, ZERO, ONE, ZERO),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16] = DECODE(R16, R, R, R, ONE),

    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8] = COPY(RGBA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_B8G8R8A8] = DECODE(RGBA8, G, B, A, R),
    [NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R8G8B8A8] = DECODE(RGBA8, A, B, G, R),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8] = COPY(RGBA8Unorm),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_B8G8R8A8] =
        DECODE(RGBA8, G, B, A, R),
    [NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8] =
        DECODE(RGBA8, A, B, G, R),
};

#undef ID
#undef R
#undef G
#undef B
#undef A
#undef ZERO
#undef ONE
#undef COPY
#undef DECODE

#endif
