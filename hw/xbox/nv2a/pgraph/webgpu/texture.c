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
#include "qemu/bswap.h"
#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "renderer.h"

#define TEXTURE_CACHE_SIZE 1024

/* Stages without a texture, or whose texture doesn't match the type the shader
 * declares, sample a 1x1 placeholder of the declared type. */

static void create_dummy_texture(PGRAPHWgpuState *r, PshTextureType type)
{
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    uint32_t texel = 0;

    desc.label = pgraph_wgpu_str("Dummy texture");
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.size = (WGPUExtent3D){ 1, 1, 1 };
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    view_desc.dimension = WGPUTextureViewDimension_2D;

    switch (type) {
    case PSH_TEXTURE_TYPE_2D_UINT:
        desc.format = WGPUTextureFormat_R32Uint;
        break;
    case PSH_TEXTURE_TYPE_3D:
        desc.dimension = WGPUTextureDimension_3D;
        view_desc.dimension = WGPUTextureViewDimension_3D;
        break;
    case PSH_TEXTURE_TYPE_CUBE:
        desc.size.depthOrArrayLayers = 6;
        view_desc.dimension = WGPUTextureViewDimension_Cube;
        break;
    default:
        break;
    }

    WGPUTexture texture = wgpuDeviceCreateTexture(r->device, &desc);
    for (uint32_t layer = 0; layer < desc.size.depthOrArrayLayers; layer++) {
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        WGPUExtent3D extent = { 1, 1, 1 };
        dst.texture = texture;
        dst.origin.z = layer;
        layout.bytesPerRow = sizeof(texel);
        wgpuQueueWriteTexture(r->queue, &dst, &texel, sizeof(texel), &layout,
                              &extent);
    }

    r->dummy_textures[type] = texture;
    r->dummy_texture_views[type] = wgpuTextureCreateView(texture, &view_desc);
}

static WGPUTextureFormat get_host_format(PGRAPHWgpuState *r,
                                         const WgpuColorFormatInfo *f)
{
    switch (f->decode) {
    case WGPU_TEX_DECODE_COPY:
        return f->format;
    case WGPU_TEX_DECODE_RGB8_SNORM:
        return WGPUTextureFormat_RGBA8Snorm;
    case WGPU_TEX_DECODE_R16:
        /* Norm16 formats are only filterable with a Dawn-specific feature */
        return r->float32_filterable ? WGPUTextureFormat_RGBA32Float :
                                       WGPUTextureFormat_RGBA16Float;
    default:
        return WGPUTextureFormat_RGBA8Unorm;
    }
}

static unsigned int get_host_texel_size(WGPUTextureFormat format)
{
    switch (format) {
    case WGPUTextureFormat_RGBA16Float:
        return 8;
    case WGPUTextureFormat_RGBA32Float:
        return 16;
    default:
        return 4;
    }
}

static unsigned int get_decode_texel_size(WgpuTextureDecode decode)
{
    switch (decode) {
    case WGPU_TEX_DECODE_R8:
        return 1;
    case WGPU_TEX_DECODE_RG8:
    case WGPU_TEX_DECODE_A1R5G5B5:
    case WGPU_TEX_DECODE_R5G6B5:
    case WGPU_TEX_DECODE_A4R4G4B4:
    case WGPU_TEX_DECODE_R16:
        return 2;
    case WGPU_TEX_DECODE_RGB8_SNORM:
        return 3;
    default:
        return 4;
    }
}

/* One dispatch covers the default maxComputeWorkgroupsPerDimension. A
 * larger image uses another 256-byte uniform slot so queue writes of the
 * params do not alias inside one submit. */
#define TEXTURE_DECODE_ROWS_PER_DISPATCH (65535u * 64u)
#define TEXTURE_DECODE_UNIFORM_SLOTS 16u
#define TEXTURE_DECODE_UNIFORM_STRIDE 256u

/* Matches decode_texels. Mode and swizzle numbers are the C enums. Bit
 * expansion replicates bits; it is not the surface packer's rounding. */
static const char texture_decode_wgsl[] =
    "struct Params {\n"
    "    width: u32,\n"
    "    rows: u32,\n"
    "    src_stride: u32,\n"
    "    dst_stride: u32,\n"
    "    src_bpp: u32,\n"
    "    decode: u32,\n"
    "    swizzle: u32,\n"
    "    out_kind: u32,\n"
    "    row_base: u32,\n"
    "    pad0: u32,\n"
    "    pad1: u32,\n"
    "    pad2: u32,\n"
    "}\n"
    "struct Texel {\n"
    "    c0: u32,\n"
    "    c1: u32,\n"
    "    c2: u32,\n"
    "    c3: u32,\n"
    "    one: u32,\n"
    "}\n"
    "@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
    "@group(0) @binding(1) var<storage, read_write> dst: array<u32>;\n"
    "@group(0) @binding(2) var<uniform> params: Params;\n"
    "fn load_byte(byte_off: u32) -> u32 {\n"
    "    let word = byte_off / 4u;\n"
    "    let shift = (byte_off % 4u) * 8u;\n"
    "    return (src[word] >> shift) & 0xffu;\n"
    "}\n"
    "fn load_u16(byte_off: u32) -> u32 {\n"
    "    return load_byte(byte_off) | (load_byte(byte_off + 1u) << 8u);\n"
    "}\n"
    "fn expand_bits(v: u32, bits: u32) -> u32 {\n"
    "    return (v << (8u - bits)) | (v >> (2u * bits - 8u));\n"
    "}\n"
    "fn unorm_to_half(value: f32) -> u32 {\n"
    "    if (value <= 0.0) { return 0u; }\n"
    "    if (value >= 1.0) { return 0x3c00u; }\n"
    "    let parts = frexp(value);\n"
    "    var exponent = parts.exp + 14;\n"
    "    if (exponent <= 0) {\n"
    "        return u32(i32(round(value * 16777216.0)));\n"
    "    }\n"
    "    var mantissa = u32(i32(round((2.0 * parts.fract - 1.0) * 1024.0)));\n"
    "    if (mantissa == 1024u) {\n"
    "        mantissa = 0u;\n"
    "        exponent = exponent + 1;\n"
    "    }\n"
    "    return (u32(exponent) << 10u) | mantissa;\n"
    "}\n"
    "fn decode_texel(byte_off: u32) -> Texel {\n"
    "    var t: Texel;\n"
    "    t.one = 0xffu;\n"
    "    let mode = params.decode;\n"
    "    if (mode == 1u) {\n"
    "        t.c0 = load_byte(byte_off);\n"
    "        t.c3 = 0xffu;\n"
    "    } else if (mode == 2u) {\n"
    "        t.c0 = load_byte(byte_off);\n"
    "        t.c1 = load_byte(byte_off + 1u);\n"
    "        t.c3 = 0xffu;\n"
    "    } else if (mode == 3u) {\n"
    "        let v = load_u16(byte_off);\n"
    "        t.c0 = expand_bits((v >> 10u) & 0x1fu, 5u);\n"
    "        t.c1 = expand_bits((v >> 5u) & 0x1fu, 5u);\n"
    "        t.c2 = expand_bits(v & 0x1fu, 5u);\n"
    "        if ((v & 0x8000u) != 0u) { t.c3 = 0xffu; }\n"
    "    } else if (mode == 4u) {\n"
    "        let v = load_u16(byte_off);\n"
    "        t.c0 = expand_bits(v >> 11u, 5u);\n"
    "        t.c1 = expand_bits((v >> 5u) & 0x3fu, 6u);\n"
    "        t.c2 = expand_bits(v & 0x1fu, 5u);\n"
    "        t.c3 = 0xffu;\n"
    "    } else if (mode == 5u) {\n"
    "        let v = load_u16(byte_off);\n"
    "        t.c0 = ((v >> 8u) & 0xfu) * 0x11u;\n"
    "        t.c1 = ((v >> 4u) & 0xfu) * 0x11u;\n"
    "        t.c2 = (v & 0xfu) * 0x11u;\n"
    "        t.c3 = (v >> 12u) * 0x11u;\n"
    "    } else if (mode == 6u) {\n"
    "        t.c0 = load_byte(byte_off + 2u);\n"
    "        t.c1 = load_byte(byte_off + 1u);\n"
    "        t.c2 = load_byte(byte_off);\n"
    "        t.c3 = load_byte(byte_off + 3u);\n"
    "    } else if (mode == 7u) {\n"
    "        t.c0 = load_byte(byte_off);\n"
    "        t.c1 = load_byte(byte_off + 1u);\n"
    "        t.c2 = load_byte(byte_off + 2u);\n"
    "        t.c3 = load_byte(byte_off + 3u);\n"
    "    } else if (mode == 8u) {\n"
    "        t.one = 0x7fu;\n"
    "        t.c0 = load_byte(byte_off);\n"
    "        t.c1 = load_byte(byte_off + 1u);\n"
    "        t.c2 = load_byte(byte_off + 2u);\n"
    "        t.c3 = t.one;\n"
    "    } else if (mode == 9u) {\n"
    "        t.one = 0xffffu;\n"
    "        t.c0 = load_u16(byte_off);\n"
    "        t.c3 = t.one;\n"
    "    }\n"
    "    return t;\n"
    "}\n"
    "fn channel(t: Texel, index: u32) -> u32 {\n"
    "    switch index {\n"
    "        case 0u: { return t.c0; }\n"
    "        case 1u: { return t.c1; }\n"
    "        case 2u: { return t.c2; }\n"
    "        default: { return t.c3; }\n"
    "    }\n"
    "}\n"
    "fn swizzle_channel(t: Texel, index: u32) -> u32 {\n"
    "    let swz = (params.swizzle >> (index * 8u)) & 0xffu;\n"
    "    switch swz {\n"
    "        case 0u: { return channel(t, index); }\n"
    "        case 1u: { return t.c0; }\n"
    "        case 2u: { return t.c1; }\n"
    "        case 3u: { return t.c2; }\n"
    "        case 4u: { return t.c3; }\n"
    "        case 5u: { return 0u; }\n"
    "        default: { return t.one; }\n"
    "    }\n"
    "}\n"
    "fn store_texel(byte_off: u32, t: Texel) {\n"
    "    let o0 = swizzle_channel(t, 0u);\n"
    "    let o1 = swizzle_channel(t, 1u);\n"
    "    let o2 = swizzle_channel(t, 2u);\n"
    "    let o3 = swizzle_channel(t, 3u);\n"
    "    let word = byte_off / 4u;\n"
    "    let denom = f32(t.one);\n"
    "    if (params.out_kind == 0u) {\n"
    "        dst[word] = (o0 & 0xffu) | ((o1 & 0xffu) << 8u) |\n"
    "            ((o2 & 0xffu) << 16u) | ((o3 & 0xffu) << 24u);\n"
    "    } else if (params.out_kind == 1u) {\n"
    "        let h0 = unorm_to_half(f32(o0) / denom);\n"
    "        let h1 = unorm_to_half(f32(o1) / denom);\n"
    "        let h2 = unorm_to_half(f32(o2) / denom);\n"
    "        let h3 = unorm_to_half(f32(o3) / denom);\n"
    "        dst[word] = h0 | (h1 << 16u);\n"
    "        dst[word + 1u] = h2 | (h3 << 16u);\n"
    "    } else {\n"
    "        dst[word] = bitcast<u32>(f32(o0) / denom);\n"
    "        dst[word + 1u] = bitcast<u32>(f32(o1) / denom);\n"
    "        dst[word + 2u] = bitcast<u32>(f32(o2) / denom);\n"
    "        dst[word + 3u] = bitcast<u32>(f32(o3) / denom);\n"
    "    }\n"
    "}\n"
    "@compute @workgroup_size(64)\n"
    "fn main(@builtin(global_invocation_id) id: vec3u) {\n"
    "    let y = params.row_base + id.x;\n"
    "    if (y >= params.rows) { return; }\n"
    "    var bpp: u32 = 4u;\n"
    "    if (params.out_kind == 1u) { bpp = 8u; }\n"
    "    else if (params.out_kind == 2u) { bpp = 16u; }\n"
    "    for (var x: u32 = 0u; x < params.width; x++) {\n"
    "        let src_off = y * params.src_stride + x * params.src_bpp;\n"
    "        let dst_off = y * params.dst_stride + x * bpp;\n"
    "        store_texel(dst_off, decode_texel(src_off));\n"
    "    }\n"
    "}\n";

typedef struct TextureDecodeParams {
    uint32_t width;
    uint32_t rows;
    uint32_t src_stride;
    uint32_t dst_stride;
    uint32_t src_bpp;
    uint32_t decode;
    uint32_t swizzle;
    uint32_t out_kind;
    uint32_t row_base;
    uint32_t pad[3];
} TextureDecodeParams;

static uint32_t expand_bits(uint32_t v, unsigned int bits)
{
    return (v << (8 - bits)) | (v >> (2 * bits - 8));
}

/* Converts a value in [0, 1] to IEEE half precision */
static uint16_t unorm_to_half(float f)
{
    if (f <= 0.0f) {
        return 0;
    }
    if (f >= 1.0f) {
        return 0x3c00;
    }

    int e;
    float m = frexpf(f, &e);
    int exponent = e - 1 + 15;
    if (exponent <= 0) {
        return lrintf(f * 16777216.0f);
    }
    uint32_t mantissa = lrintf((2.0f * m - 1.0f) * 1024.0f);
    if (mantissa == 1024) {
        mantissa = 0;
        exponent++;
    }
    return (exponent << 10) | mantissa;
}

static void decode_texels(const WgpuColorFormatInfo *f,
                          WGPUTextureFormat format, const uint8_t *src,
                          unsigned int count, uint8_t *dst)
{
    if (f->decode == WGPU_TEX_DECODE_COPY) {
        memcpy(dst, src, count * get_host_texel_size(format));
        return;
    }

    unsigned int src_size = get_decode_texel_size(f->decode);

    for (unsigned int i = 0; i < count; i++, src += src_size) {
        uint32_t c[4], one = 0xff;
        uint32_t v = src_size == 2 ? lduw_le_p(src) : 0;

        switch (f->decode) {
        case WGPU_TEX_DECODE_R8:
            c[0] = src[0], c[1] = 0, c[2] = 0, c[3] = 0xff;
            break;
        case WGPU_TEX_DECODE_RG8:
            c[0] = src[0], c[1] = src[1], c[2] = 0, c[3] = 0xff;
            break;
        case WGPU_TEX_DECODE_A1R5G5B5:
            c[0] = expand_bits((v >> 10) & 0x1f, 5);
            c[1] = expand_bits((v >> 5) & 0x1f, 5);
            c[2] = expand_bits(v & 0x1f, 5);
            c[3] = (v & 0x8000) ? 0xff : 0;
            break;
        case WGPU_TEX_DECODE_R5G6B5:
            c[0] = expand_bits(v >> 11, 5);
            c[1] = expand_bits((v >> 5) & 0x3f, 6);
            c[2] = expand_bits(v & 0x1f, 5);
            c[3] = 0xff;
            break;
        case WGPU_TEX_DECODE_A4R4G4B4:
            c[0] = ((v >> 8) & 0xf) * 0x11;
            c[1] = ((v >> 4) & 0xf) * 0x11;
            c[2] = (v & 0xf) * 0x11;
            c[3] = (v >> 12) * 0x11;
            break;
        case WGPU_TEX_DECODE_BGRA8:
            c[0] = src[2], c[1] = src[1], c[2] = src[0], c[3] = src[3];
            break;
        case WGPU_TEX_DECODE_RGBA8:
            c[0] = src[0], c[1] = src[1], c[2] = src[2], c[3] = src[3];
            break;
        case WGPU_TEX_DECODE_RGB8_SNORM:
            one = 0x7f;
            c[0] = src[0], c[1] = src[1], c[2] = src[2], c[3] = one;
            break;
        case WGPU_TEX_DECODE_R16:
            one = 0xffff;
            c[0] = v, c[1] = 0, c[2] = 0, c[3] = one;
            break;
        default:
            g_assert_not_reached();
        }

        uint32_t out[4];
        for (int j = 0; j < 4; j++) {
            switch (f->swizzle[j]) {
            case WGPU_SWZ_ID:
                out[j] = c[j];
                break;
            case WGPU_SWZ_ZERO:
                out[j] = 0;
                break;
            case WGPU_SWZ_ONE:
                out[j] = one;
                break;
            default:
                out[j] = c[f->swizzle[j] - WGPU_SWZ_R];
                break;
            }
        }

        if (format == WGPUTextureFormat_RGBA32Float) {
            for (int j = 0; j < 4; j++) {
                float value = out[j] / (float)one;
                memcpy(dst, &value, sizeof(value));
                dst += sizeof(value);
            }
        } else if (format == WGPUTextureFormat_RGBA16Float) {
            for (int j = 0; j < 4; j++) {
                stw_he_p(dst, unorm_to_half(out[j] / (float)one));
                dst += 2;
            }
        } else {
            for (int j = 0; j < 4; j++) {
                *dst++ = out[j];
            }
        }
    }
}

static void write_level_cpu(PGRAPHWgpuState *r, WGPUTexture texture,
                            const WgpuColorFormatInfo *f,
                            WGPUTextureFormat format, unsigned int layer,
                            unsigned int level, unsigned int width,
                            unsigned int height, unsigned int depth,
                            const uint8_t *src, size_t src_pitch)
{
    size_t dst_pitch = width * get_host_texel_size(format);
    size_t size = dst_pitch * height * depth;
    g_autofree uint8_t *decoded = NULL;
    const uint8_t *data = src;

    if (f->decode != WGPU_TEX_DECODE_COPY || src_pitch != dst_pitch) {
        decoded = g_malloc(size);
        for (unsigned int row = 0; row < height * depth; row++) {
            decode_texels(f, format, src + row * src_pitch, width,
                          decoded + row * dst_pitch);
        }
        data = decoded;
    }

    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = texture;
    dst.mipLevel = level;
    dst.origin.z = layer;

    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = dst_pitch;
    layout.rowsPerImage = height;

    WGPUExtent3D extent = { width, height, depth };
    wgpuQueueWriteTexture(r->queue, &dst, data, size, &layout, &extent);
}

static WGPUBindGroupLayoutEntry
texture_decode_layout_entry(uint32_t binding, WGPUBufferBindingType type)
{
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Compute;
    entry.buffer.type = type;
    return entry;
}

static bool texture_decode_fits(PGRAPHWgpuState *r, uint64_t bytes)
{
    WGPULimits limits = WGPU_LIMITS_INIT;
    uint64_t max_binding = (uint64_t)128 * 1024 * 1024;
    uint64_t max_buffer = (uint64_t)256 * 1024 * 1024;

    if (wgpuDeviceGetLimits(r->device, &limits) == WGPUStatus_Success) {
        if (limits.maxStorageBufferBindingSize) {
            max_binding = limits.maxStorageBufferBindingSize;
        }
        if (limits.maxBufferSize) {
            max_buffer = limits.maxBufferSize;
        }
    }
    return bytes <= max_binding && ROUND_UP(bytes, 1024 * 1024) <= max_buffer;
}

static WGPUBuffer ensure_texture_decode_buffer(PGRAPHWgpuState *r, bool source,
                                               uint64_t size, const char *label)
{
    WGPUBuffer *slot = source ? &r->texture_decode_src : &r->texture_decode_dst;
    uint64_t *capacity =
        source ? &r->texture_decode_src_size : &r->texture_decode_dst_size;
    WGPUBufferUsage usage =
        source ? WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst :
                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;

    size = ROUND_UP(size, 4);
    if (*slot && *capacity >= size) {
        return *slot;
    }
    if (*slot) {
        wgpuBufferRelease(*slot);
        *slot = NULL;
    }

    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str(label);
    desc.size = ROUND_UP(size, 1024 * 1024);
    desc.usage = usage;
    *slot = wgpuDeviceCreateBuffer(r->device, &desc);
    *capacity = desc.size;
    return *slot;
}

static uint32_t pack_texture_swizzle(const uint8_t swizzle[4])
{
    return (uint32_t)swizzle[0] | ((uint32_t)swizzle[1] << 8) |
           ((uint32_t)swizzle[2] << 16) | ((uint32_t)swizzle[3] << 24);
}

static uint32_t texture_decode_out_kind(WGPUTextureFormat format)
{
    switch (format) {
    case WGPUTextureFormat_RGBA16Float:
        return 1;
    case WGPUTextureFormat_RGBA32Float:
        return 2;
    default:
        return 0;
    }
}

/* Returns false when this level stays on write_level_cpu. A queue write is
 * visible to every command in the next submit, so the open encoder is
 * submitted before the source buffer is reused. */
static bool write_level_compute(PGRAPHState *pg, WGPUTexture texture,
                                const WgpuColorFormatInfo *f,
                                WGPUTextureFormat format, unsigned int layer,
                                unsigned int level, unsigned int width,
                                unsigned int height, unsigned int depth,
                                const uint8_t *src, size_t src_pitch)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    uint32_t src_bpp = get_decode_texel_size(f->decode);
    uint32_t texel = get_host_texel_size(format);
    uint64_t row_bytes = (uint64_t)width * texel;
    uint64_t dst_stride = ROUND_UP(row_bytes, 256);
    uint64_t rows64;
    uint32_t rows, nchunks, chunk;
    uint64_t src_bytes, src_upload, dst_bytes;
    g_autofree uint8_t *padded = NULL;
    const void *upload;
    WGPUBuffer src_buffer, dst_buffer;
    WGPUBindGroup groups[TEXTURE_DECODE_UNIFORM_SLOTS];
    TextureDecodeParams params;

    if (f->decode == WGPU_TEX_DECODE_COPY || !r->texture_decode_pipeline ||
        !width || !height || !depth || !src_pitch || src_pitch > UINT32_MAX ||
        dst_stride > UINT32_MAX || dst_stride == 0) {
        return false;
    }
    if (height > UINT32_MAX / depth) {
        return false;
    }
    rows64 = (uint64_t)height * depth;
    if (rows64 > (uint64_t)TEXTURE_DECODE_UNIFORM_SLOTS *
                     TEXTURE_DECODE_ROWS_PER_DISPATCH) {
        return false;
    }
    rows = (uint32_t)rows64;
    if (src_pitch > UINT64_MAX / rows || dst_stride > UINT64_MAX / rows) {
        return false;
    }
    src_bytes = src_pitch * rows;
    src_upload = ROUND_UP(src_bytes, 4);
    dst_bytes = dst_stride * rows;
    if (!src_upload || !texture_decode_fits(r, src_upload) ||
        !texture_decode_fits(r, dst_bytes)) {
        return false;
    }

    /* The previous submit may still be reading these buffers. Releasing a
     * buffer the queue is using is safe; overwriting one an open encoder
     * has already recorded is not. */
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_FLUSH, false);

    src_buffer = ensure_texture_decode_buffer(r, true, src_upload,
                                              "Texture decode source");
    dst_buffer = ensure_texture_decode_buffer(r, false, dst_bytes,
                                              "Texture decode output");
    if (!src_buffer || !dst_buffer) {
        return false;
    }

    upload = src;
    if (src_upload != src_bytes) {
        padded = g_malloc0(src_upload);
        memcpy(padded, src, src_bytes);
        upload = padded;
    }
    wgpuQueueWriteBuffer(r->queue, src_buffer, 0, upload, src_upload);

    params = (TextureDecodeParams){
        .width = width,
        .rows = rows,
        .src_stride = src_pitch,
        .dst_stride = dst_stride,
        .src_bpp = src_bpp,
        .decode = f->decode,
        .swizzle = pack_texture_swizzle(f->swizzle),
        .out_kind = texture_decode_out_kind(format),
    };
    nchunks = (rows + TEXTURE_DECODE_ROWS_PER_DISPATCH - 1) /
              TEXTURE_DECODE_ROWS_PER_DISPATCH;
    for (chunk = 0; chunk < nchunks; chunk++) {
        WGPUBindGroupEntry entries[3];
        WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;

        params.row_base = chunk * TEXTURE_DECODE_ROWS_PER_DISPATCH;
        wgpuQueueWriteBuffer(r->queue, r->texture_decode_uniform,
                             (uint64_t)chunk * TEXTURE_DECODE_UNIFORM_STRIDE,
                             &params, sizeof(params));

        entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[0].binding = 0;
        entries[0].buffer = src_buffer;
        entries[0].size = src_upload;
        entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[1].binding = 1;
        entries[1].buffer = dst_buffer;
        entries[1].size = dst_bytes;
        entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[2].binding = 2;
        entries[2].buffer = r->texture_decode_uniform;
        entries[2].offset = (uint64_t)chunk * TEXTURE_DECODE_UNIFORM_STRIDE;
        entries[2].size = sizeof(params);

        bg_desc.layout = r->texture_decode_layout;
        bg_desc.entryCount = 3;
        bg_desc.entries = entries;
        groups[chunk] = wgpuDeviceCreateBindGroup(r->device, &bg_desc);
    }

    pgraph_wgpu_end_render_pass(r);
    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    WGPUComputePassDescriptor pass_desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    pass_desc.label = pgraph_wgpu_str("Texture decode");
    WGPUComputePassEncoder pass =
        wgpuCommandEncoderBeginComputePass(encoder, &pass_desc);
    wgpuComputePassEncoderSetPipeline(pass, r->texture_decode_pipeline);
    for (chunk = 0; chunk < nchunks; chunk++) {
        uint32_t batch = MIN(rows - chunk * TEXTURE_DECODE_ROWS_PER_DISPATCH,
                             TEXTURE_DECODE_ROWS_PER_DISPATCH);

        wgpuComputePassEncoderSetBindGroup(pass, 0, groups[chunk], 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(pass, (batch + 63) / 64, 1, 1);
        wgpuBindGroupRelease(groups[chunk]);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    WGPUTexelCopyBufferInfo copy_src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copy_src.buffer = dst_buffer;
    copy_src.layout.bytesPerRow = dst_stride;
    copy_src.layout.rowsPerImage = height;

    WGPUTexelCopyTextureInfo copy_dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copy_dst.texture = texture;
    copy_dst.mipLevel = level;
    copy_dst.origin.z = layer;

    WGPUExtent3D extent = { width, height, depth };
    wgpuCommandEncoderCopyBufferToTexture(encoder, &copy_src, &copy_dst,
                                          &extent);
    return true;
}

static void write_level(PGRAPHState *pg, WGPUTexture texture,
                        const WgpuColorFormatInfo *f, WGPUTextureFormat format,
                        unsigned int layer, unsigned int level,
                        unsigned int width, unsigned int height,
                        unsigned int depth, const uint8_t *src,
                        size_t src_pitch)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (write_level_compute(pg, texture, f, format, layer, level, width, height,
                            depth, src, src_pitch)) {
        return;
    }
    write_level_cpu(r, texture, f, format, layer, level, width, height, depth,
                    src, src_pitch);
}

static void init_texture_decode(PGRAPHWgpuState *r)
{
    WGPUBindGroupLayoutEntry entries[3];
    WGPUBindGroupLayoutDescriptor bgl_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayoutDescriptor layout_desc =
        WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    WGPUBufferDescriptor uniform_desc = WGPU_BUFFER_DESCRIPTOR_INIT;

    G_STATIC_ASSERT(sizeof(TextureDecodeParams) == 48);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_R8 == 1);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_RG8 == 2);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_A1R5G5B5 == 3);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_R5G6B5 == 4);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_A4R4G4B4 == 5);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_BGRA8 == 6);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_RGBA8 == 7);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_RGB8_SNORM == 8);
    G_STATIC_ASSERT(WGPU_TEX_DECODE_R16 == 9);
    G_STATIC_ASSERT(WGPU_SWZ_ID == 0);
    G_STATIC_ASSERT(WGPU_SWZ_R == 1);
    G_STATIC_ASSERT(WGPU_SWZ_G == 2);
    G_STATIC_ASSERT(WGPU_SWZ_B == 3);
    G_STATIC_ASSERT(WGPU_SWZ_A == 4);
    G_STATIC_ASSERT(WGPU_SWZ_ZERO == 5);
    G_STATIC_ASSERT(WGPU_SWZ_ONE == 6);

    r->texture_decode_module = pgraph_wgpu_create_wgsl_module(
        r, "Texture decode", texture_decode_wgsl);
    if (!r->texture_decode_module) {
        return;
    }

    entries[0] =
        texture_decode_layout_entry(0, WGPUBufferBindingType_ReadOnlyStorage);
    entries[1] = texture_decode_layout_entry(1, WGPUBufferBindingType_Storage);
    entries[2] = texture_decode_layout_entry(2, WGPUBufferBindingType_Uniform);
    bgl_desc.entryCount = 3;
    bgl_desc.entries = entries;
    r->texture_decode_layout =
        wgpuDeviceCreateBindGroupLayout(r->device, &bgl_desc);

    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &r->texture_decode_layout;
    r->texture_decode_pipeline_layout =
        wgpuDeviceCreatePipelineLayout(r->device, &layout_desc);

    desc.label = pgraph_wgpu_str("Texture decode");
    desc.layout = r->texture_decode_pipeline_layout;
    desc.compute.module = r->texture_decode_module;
    desc.compute.entryPoint = pgraph_wgpu_str("main");
    r->texture_decode_pipeline =
        wgpuDeviceCreateComputePipeline(r->device, &desc);
    if (!r->texture_decode_pipeline) {
        return;
    }

    uniform_desc.label = pgraph_wgpu_str("Texture decode params");
    uniform_desc.size =
        TEXTURE_DECODE_UNIFORM_SLOTS * TEXTURE_DECODE_UNIFORM_STRIDE;
    uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    r->texture_decode_uniform =
        wgpuDeviceCreateBuffer(r->device, &uniform_desc);
}

static void finalize_texture_decode(PGRAPHWgpuState *r)
{
    if (r->texture_decode_pipeline) {
        wgpuComputePipelineRelease(r->texture_decode_pipeline);
        r->texture_decode_pipeline = NULL;
    }
    if (r->texture_decode_pipeline_layout) {
        wgpuPipelineLayoutRelease(r->texture_decode_pipeline_layout);
        r->texture_decode_pipeline_layout = NULL;
    }
    if (r->texture_decode_layout) {
        wgpuBindGroupLayoutRelease(r->texture_decode_layout);
        r->texture_decode_layout = NULL;
    }
    if (r->texture_decode_module) {
        wgpuShaderModuleRelease(r->texture_decode_module);
        r->texture_decode_module = NULL;
    }
    if (r->texture_decode_uniform) {
        wgpuBufferRelease(r->texture_decode_uniform);
        r->texture_decode_uniform = NULL;
    }
    if (r->texture_decode_src) {
        wgpuBufferRelease(r->texture_decode_src);
        r->texture_decode_src = NULL;
        r->texture_decode_src_size = 0;
    }
    if (r->texture_decode_dst) {
        wgpuBufferRelease(r->texture_decode_dst);
        r->texture_decode_dst = NULL;
        r->texture_decode_dst_size = 0;
    }
}

static enum S3TC_DECOMPRESS_FORMAT
kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        g_assert_not_reached();
    }
}

static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size =
        s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8 :
                                                                           16;
    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }
        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

/* Textures with a border are stored at (at least) double the reported size */
static void get_texture_size(const TextureShape *s, unsigned int *width,
                             unsigned int *height, unsigned int *depth)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s->color_format];

    *width = s->width;
    *height = s->height;
    *depth = s->depth;
    if (!f.linear && s->border) {
        *width = MAX(16, *width * 2);
        *height = MAX(16, *height * 2);
        *depth = MAX(16, *depth * 2);
    }
}

static void upload_texture_data(PGRAPHState *pg, TextureBinding *binding)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    const TextureShape s = binding->key.state;
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    const WgpuColorFormatInfo *wf =
        &kelvin_color_format_wgpu_map[s.color_format];
    WGPUTextureFormat format = get_host_format(r, wf);
    unsigned int decode_size = get_decode_texel_size(wf->decode);
    WGPUTexture texture = binding->texture;

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);

    const uint8_t *texture_data =
        d->vram_ptr + binding->key.texture_vram_offset;
    const uint8_t *palette_data =
        d->vram_ptr + binding->key.palette_vram_offset;

    unsigned int adjusted_width, adjusted_height, adjusted_depth;
    get_texture_size(&s, &adjusted_width, &adjusted_height, &adjusted_depth);

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 &&
               "Can't handle strides unaligned to pixels");
        g_autofree uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data, palette_data, adjusted_width, adjusted_height, 1,
            s.pitch, 0, NULL);
        write_level(pg, texture, wf, format, 0, 0, adjusted_width,
                    adjusted_height, 1, converted ?: texture_data,
                    converted ? adjusted_width * decode_size : s.pitch);
        return;
    }

    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size =
        s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ? 8 :
                                                                           16;

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        int num_layers = s.cubemap ? 6 : 1;

        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            const uint8_t *src = texture_data + layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                width = MAX(width, 1);
                height = MAX(height, 1);

                if (is_compressed) {
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;
                    g_autofree uint8_t *converted = s3tc_decompress_2d(
                        kelvin_format_to_s3tc_format(s.color_format), src,
                        width, height);
                    write_level(pg, texture, wf, format, layer, level, width,
                                height, 1, converted, width * 4);
                    src +=
                        physical_width / 4 * physical_height / 4 * block_size;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;
                    g_autofree uint8_t *unswizzled = g_malloc(height * pitch);
                    unswizzle_rect(src, width, height, unswizzled, pitch,
                                   f.bytes_per_pixel);
                    g_autofree uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data, width, height, 1, pitch, 0,
                        NULL);
                    write_level(pg, texture, wf, format, layer, level, width,
                                height, 1, converted ?: unswizzled,
                                converted ? width * decode_size : pitch);
                    src += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;
        const uint8_t *src = texture_data;

        for (int level = 0; level < s.levels; level++) {
            width = MAX(width, 1);
            height = MAX(height, 1);
            depth = MAX(depth, 1);

            if (is_compressed) {
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                g_autofree uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format), src, width,
                    height, depth);
                write_level(pg, texture, wf, format, 0, level, width, height,
                            depth, converted, width * 4);
                src += physical_width / 4 * physical_height / 4 * depth *
                       block_size;
            } else {
                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;
                g_autofree uint8_t *unswizzled = g_malloc(slice_pitch * depth);
                unswizzle_box(src, width, height, depth, unswizzled, row_pitch,
                              slice_pitch, f.bytes_per_pixel);
                g_autofree uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data, width, height, depth,
                    row_pitch, slice_pitch, NULL);
                write_level(pg, texture, wf, format, 0, level, width, height,
                            depth, converted ?: unswizzled,
                            converted ? width * decode_size : row_pitch);
                src += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }
}

static void create_texture_object(PGRAPHWgpuState *r, TextureBinding *binding)
{
    const TextureShape *s = &binding->key.state;
    BasicColorFormatInfo f = kelvin_color_format_info_map[s->color_format];
    const WgpuColorFormatInfo *wf =
        &kelvin_color_format_wgpu_map[s->color_format];
    unsigned int width, height, depth;

    get_texture_size(s, &width, &height, &depth);
    /* A surface copy is stored at the scaled size. The factor is 1 for now. */
    if (binding->key.scale > 1) {
        width *= binding->key.scale;
        height *= binding->key.scale;
    }

    g_autofree gchar *label =
        g_strdup_printf("Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
                        binding->key.texture_vram_offset, s->color_format,
                        s->width, s->height, s->depth, s->levels);

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str(label);
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.format = get_host_format(r, wf);
    desc.mipLevelCount = f.linear ? 1 : s->levels;
    desc.size = (WGPUExtent3D){ width, height, 1 };

    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    if (s->dimensionality == 3) {
        desc.dimension = WGPUTextureDimension_3D;
        desc.size.depthOrArrayLayers = depth;
        view_desc.dimension = WGPUTextureViewDimension_3D;
    } else if (s->cubemap) {
        desc.size.depthOrArrayLayers = 6;
        view_desc.dimension = WGPUTextureViewDimension_Cube;
    } else {
        view_desc.dimension = WGPUTextureViewDimension_2D;
    }

    binding->texture = wgpuDeviceCreateTexture(r->device, &desc);
    binding->view = wgpuTextureCreateView(binding->texture, &view_desc);
}

static void purge_texture_bind_groups(PGRAPHWgpuState *r, WGPUTextureView view,
                                      WGPUSampler sampler)
{
    for (int i = 0; i < WGPU_TEXTURE_BIND_CACHE; i++) {
        WgpuTextureBindCacheEntry *entry = &r->texture_bind_cache[i];
        bool drop = false;

        if (!entry->used) {
            continue;
        }
        for (int s = 0; s < NV2A_MAX_TEXTURES; s++) {
            if ((view && entry->key.views[s] == view) ||
                (sampler && entry->key.samplers[s] == sampler)) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            continue;
        }
        wgpuBindGroupRelease(entry->group);
        entry->group = NULL;
        entry->used = false;
    }
}

static void release_texture_object(PGRAPHWgpuState *r, TextureBinding *binding)
{
    /* Only released: unsubmitted commands may still reference the texture.
     * Bind groups that still name the view have to go first. */
    if (binding->view) {
        purge_texture_bind_groups(r, binding->view, NULL);
        wgpuTextureViewRelease(binding->view);
        binding->view = NULL;
    }
    if (binding->texture) {
        wgpuTextureRelease(binding->texture);
        binding->texture = NULL;
    }
}

static WGPUSampler create_sampler(PGRAPHWgpuState *r, const TextureKey *key,
                                  bool filterable)
{
    const TextureShape *s = &key->state;
    BasicColorFormatInfo f = kelvin_color_format_info_map[s->color_format];
    uint32_t filter = key->filter;
    uint32_t address = key->address;

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_wgpu_map));
    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_wgpu_map));

    unsigned int levels = f.linear ? 1 : s->levels;
    bool mipmap_en =
        !f.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);
    bool mipmap_nearest =
        levels == 1 || min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    desc.addressModeU = pgraph_texture_addr_wgpu_map[GET_MASK(
        address, NV_PGRAPH_TEXADDRESS0_ADDRU)];
    desc.addressModeV = pgraph_texture_addr_wgpu_map[GET_MASK(
        address, NV_PGRAPH_TEXADDRESS0_ADDRV)];
    desc.addressModeW = s->dimensionality > 2 ?
                            pgraph_texture_addr_wgpu_map[GET_MASK(
                                address, NV_PGRAPH_TEXADDRESS0_ADDRP)] :
                            WGPUAddressMode_ClampToEdge;
    if (filterable) {
        desc.magFilter = pgraph_texture_mag_filter_wgpu_map[mag_filter];
        desc.minFilter = pgraph_texture_min_filter_wgpu_map[min_filter];
        desc.mipmapFilter = mipmap_nearest ? WGPUMipmapFilterMode_Nearest :
                                             WGPUMipmapFilterMode_Linear;
    }
    desc.lodMinClamp = mipmap_en ? MIN(s->min_mipmap_level, levels - 1) : 0;
    desc.lodMaxClamp = mipmap_en ? MIN(s->max_mipmap_level, levels - 1) : 0;
    desc.lodMaxClamp = MAX(desc.lodMinClamp, desc.lodMaxClamp);

    /* Anisotropy is only allowed when every filter is linear */
    if (key->max_anisotropy > 1 && desc.magFilter == WGPUFilterMode_Linear &&
        desc.minFilter == WGPUFilterMode_Linear &&
        desc.mipmapFilter == WGPUMipmapFilterMode_Linear) {
        desc.maxAnisotropy = MIN(key->max_anisotropy, 16);
    }

    return wgpuDeviceCreateSampler(r->device, &desc);
}

static PshTextureType get_texture_type(PGRAPHWgpuState *r,
                                       const TextureShape *s)
{
    if (s->dimensionality == 3) {
        return PSH_TEXTURE_TYPE_3D;
    }
    if (s->cubemap) {
        return PSH_TEXTURE_TYPE_CUBE;
    }
    if (get_host_format(r, &kelvin_color_format_wgpu_map[s->color_format]) ==
        WGPUTextureFormat_R32Uint) {
        return PSH_TEXTURE_TYPE_2D_UINT;
    }
    return PSH_TEXTURE_TYPE_2D;
}

typedef struct PossiblyDirtyRange {
    hwaddr addr, end;
} PossiblyDirtyRange;

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node,
                                                 void *opaque)
{
    PossiblyDirtyRange *test = opaque;
    TextureBinding *binding = container_of(node, TextureBinding, node);

    if (binding->possibly_dirty) {
        return;
    }

    uintptr_t k_tex_addr = binding->key.texture_vram_offset;
    uintptr_t k_tex_end = k_tex_addr + binding->key.texture_length - 1;
    bool overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

    if (binding->key.palette_length > 0) {
        uintptr_t k_pal_addr = binding->key.palette_vram_offset;
        uintptr_t k_pal_end = k_pal_addr + binding->key.palette_length - 1;
        overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
    }

    binding->possibly_dirty |= overlapping;
}

void pgraph_wgpu_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                              hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    PossiblyDirtyRange test = { .addr = addr, .end = end };
    lru_visit_active(&d->pgraph.wgpu_renderer_state->texture_cache,
                     mark_textures_possibly_dirty_visitor, &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

/* Checks whether any of the pages spanned by the texture or palette are dirty.
 * Other textures on those pages are marked, because the dirty bits are
 * cleared here. */
static bool check_texture_possibly_dirty(NV2AState *d, const TextureKey *key)
{
    bool possibly_dirty = false;

    if (check_texture_dirty(d, key->texture_vram_offset, key->texture_length)) {
        possibly_dirty = true;
        pgraph_wgpu_mark_textures_possibly_dirty(d, key->texture_vram_offset,
                                                 key->texture_length);
    }
    if (key->palette_length &&
        check_texture_dirty(d, key->palette_vram_offset, key->palette_length)) {
        possibly_dirty = true;
        pgraph_wgpu_mark_textures_possibly_dirty(d, key->palette_vram_offset,
                                                 key->palette_length);
    }
    return possibly_dirty;
}

static uint64_t hash_texture_data(NV2AState *d, const TextureKey *key)
{
    uint64_t hash =
        fast_hash(d->vram_ptr + key->texture_vram_offset, key->texture_length);
    if (key->palette_length) {
        hash ^= fast_hash(d->vram_ptr + key->palette_vram_offset,
                          key->palette_length);
    }
    return hash;
}

/* 0 is 0, 1 is 1, 2 is the 16-bit depth value. Matches decode_texels for R16.
 */
static uint32_t r16_channel_mode(const WgpuColorFormatInfo *wf, int channel)
{
    switch (wf->swizzle[channel]) {
    case WGPU_SWZ_ZERO:
    case WGPU_SWZ_G:
    case WGPU_SWZ_B:
        return 0;
    case WGPU_SWZ_ONE:
    case WGPU_SWZ_A:
        return 1;
    case WGPU_SWZ_R:
        return 2;
    case WGPU_SWZ_ID:
        if (channel == 0) {
            return 2;
        }
        return channel == 3 ? 1 : 0;
    default:
        return 0;
    }
}

static uint32_t z16_channel_modes(const WgpuColorFormatInfo *wf)
{
    uint32_t modes = 0;

    for (int i = 0; i < 4; i++) {
        modes |= r16_channel_mode(wf, i) << (i * 2);
    }
    return modes;
}

/*
 * Copy on the GPU only when the texture texels are the surface image.
 * Anything that still needs a decode (16-bit colour, X8 alpha, B8, G8B8,
 * a mismatched depth format) keeps the RAM download.
 */
static bool surface_to_texture_compatible(PGRAPHWgpuState *r,
                                          const SurfaceBinding *surface,
                                          const TextureShape *shape)
{
    const WgpuColorFormatInfo *wf;

    if (shape->dimensionality != 2 || shape->cubemap || shape->levels > 1 ||
        shape->border || surface->width != shape->width ||
        surface->height != shape->height ||
        (!surface->swizzle && surface->pitch != shape->pitch) ||
        shape->color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
        return false;
    }

    wf = &kelvin_color_format_wgpu_map[shape->color_format];
    if (!surface->color) {
        if (surface->host_fmt.conversion == WGPU_SURFACE_CONV_Z24S8 &&
            wf->decode == WGPU_TEX_DECODE_COPY &&
            wf->format == WGPUTextureFormat_R32Uint) {
            return true;
        }
        return surface->host_fmt.conversion == WGPU_SURFACE_CONV_Z16 &&
               wf->decode == WGPU_TEX_DECODE_R16 &&
               get_host_format(r, wf) == WGPUTextureFormat_RGBA32Float;
    }

    return surface->host_fmt.conversion == WGPU_SURFACE_CONV_NONE &&
           wf->decode == WGPU_TEX_DECODE_COPY &&
           wf->format == surface->host_fmt.format;
}

static void copy_color_surface_to_texture(PGRAPHState *pg,
                                          SurfaceBinding *surface,
                                          TextureBinding *texture)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    unsigned int width = surface->width;
    unsigned int height = surface->height;

    pgraph_apply_scaling_factor(pg, &width, &height);
    pgraph_wgpu_end_render_pass(r);

    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUExtent3D extent = { width, height, 1 };

    src.texture = surface->texture;
    dst.texture = texture->texture;
    wgpuCommandEncoderCopyTextureToTexture(pgraph_wgpu_ensure_encoder(pg), &src,
                                           &dst, &extent);
}

static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);
    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    if (surface->color) {
        copy_color_surface_to_texture(pg, surface, texture);
    } else {
        PGRAPHWgpuState *r = pg->wgpu_renderer_state;
        const WgpuColorFormatInfo *wf =
            &kelvin_color_format_wgpu_map[texture->key.state.color_format];
        uint32_t modes = 0;

        if (surface->host_fmt.conversion == WGPU_SURFACE_CONV_Z16) {
            modes = z16_channel_modes(wf);
        }
        pgraph_wgpu_copy_zeta_to_texture(pg, surface, texture->texture,
                                         get_host_format(r, wf), modes);
    }
    texture->draw_time = surface->draw_time;
}

static void bind_texture(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx);

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    key.texture_length = pgraph_get_texture_length(pg, &state);
    if (state.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8) {
        size_t palette_length;
        key.palette_vram_offset = pgraph_get_texture_palette_phys_addr_length(
            pg, texture_idx, &palette_length);
        key.palette_length = palette_length;
    }
    key.filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    key.address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    key.max_anisotropy =
        1 << (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + texture_idx * 4),
                       NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY));
    key.scale = 1;

    SurfaceBinding *surface =
        pgraph_wgpu_surface_get(d, key.texture_vram_offset);
    bool surface_to_texture = false;
    bool surface_uploaded = false;
    if (surface && state.levels == 1) {
        surface_to_texture = surface_to_texture_compatible(r, surface, &state);
        if (!surface_to_texture && surface->color) {
            trace_nv2a_pgraph_surface_texture_compat_failed(
                surface->shape.color_format, state.color_format);
        }
        if (surface_to_texture && surface->upload_pending) {
            pgraph_wgpu_upload_surface_data(d, surface, false);
            surface_uploaded = true;
        }
    }
    if (!surface_to_texture) {
        pgraph_wgpu_download_surfaces_in_range_if_dirty(
            pg, key.texture_vram_offset, key.texture_length);
    }
    if (surface_to_texture && pg->surface_scale_factor > 1) {
        key.scale = pg->surface_scale_factor;
    }

    uint64_t key_hash = fast_hash((const uint8_t *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *binding = container_of(node, TextureBinding, node);
    bool found = binding->texture != NULL;

    bool possibly_dirty = !found || binding->possibly_dirty;
    if (!surface_to_texture) {
        possibly_dirty |= check_texture_possibly_dirty(d, &key);
    }
    r->texture_bindings[texture_idx] = binding;

    if (found) {
        if (surface_to_texture) {
            if (surface_uploaded || surface->draw_time != binding->draw_time) {
                copy_surface_to_texture(pg, surface, binding);
            }
        } else if (possibly_dirty) {
            uint64_t hash = hash_texture_data(d, &key);
            binding->possibly_dirty = false;
            if (hash != binding->hash) {
                /* Queue writes run before unsubmitted commands, which may
                 * still sample the old contents, so use a new texture */
                release_texture_object(r, binding);
                create_texture_object(r, binding);
                upload_texture_data(pg, binding);
                binding->hash = hash;
            }
        }
        return;
    }

    memcpy(&binding->key, &key, sizeof(key));
    binding->possibly_dirty = false;
    binding->type = get_texture_type(r, &state);

    assert(kelvin_color_format_info_map[state.color_format].bytes_per_pixel);
    assert(state.dimensionality == 2 || state.dimensionality == 3);

    create_texture_object(r, binding);
    binding->sampler =
        create_sampler(r, &key, binding->type != PSH_TEXTURE_TYPE_2D_UINT);
    if (surface_to_texture) {
        binding->hash = 0;
        copy_surface_to_texture(pg, surface, binding);
    } else {
        binding->hash = hash_texture_data(d, &key);
        binding->draw_time = 0;
        upload_texture_data(pg, binding);
    }
}

/* Guest CPU writes to texture memory don't set texture_dirty, so every enabled
 * texture is looked up again on each draw, which checks its pages. The bind
 * group for a texture and sampler set is reused from the cache. */
void pgraph_wgpu_bind_textures(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (pgraph_is_texture_enabled(pg, i)) {
            bind_texture(pg, i);
        } else {
            r->texture_bindings[i] = NULL;
        }
        pg->texture_dirty[i] = false;
    }
}

static void texture_bind_key(PGRAPHWgpuState *r, const WgpuTextureLayout *l,
                             WgpuTextureBindKey *key)
{
    memset(key, 0, sizeof(*key));
    key->layout = l;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (l->types[i] == PSH_TEXTURE_TYPE_NONE) {
            continue;
        }
        TextureBinding *binding = r->texture_bindings[i];
        key->views[i] = r->dummy_texture_views[l->types[i]];
        key->samplers[i] = r->dummy_sampler;
        if (binding && binding->type == l->types[i]) {
            key->views[i] = binding->view;
            key->samplers[i] = binding->sampler;
        }
    }
}

static WGPUBindGroup create_texture_bind_group(PGRAPHWgpuState *r,
                                               const WgpuTextureLayout *l,
                                               const WgpuTextureBindKey *key)
{
    WGPUBindGroupEntry entries[2 * NV2A_MAX_TEXTURES];
    size_t num_entries = 0;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (l->types[i] == PSH_TEXTURE_TYPE_NONE) {
            continue;
        }
        if (r->texture_bindings[i] &&
            r->texture_bindings[i]->type != l->types[i]) {
            NV2A_UNIMPLEMENTED("Texture %d type %d, shader expects %d", i,
                               r->texture_bindings[i]->type, l->types[i]);
        }

        WGPUBindGroupEntry texture = WGPU_BIND_GROUP_ENTRY_INIT;
        texture.binding = 2 * i;
        texture.textureView = key->views[i];
        entries[num_entries++] = texture;

        WGPUBindGroupEntry sampler_entry = WGPU_BIND_GROUP_ENTRY_INIT;
        sampler_entry.binding = 2 * i + 1;
        sampler_entry.sampler = key->samplers[i];
        entries[num_entries++] = sampler_entry;
    }

    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Textures");
    desc.layout = l->bind_group_layout;
    desc.entryCount = num_entries;
    desc.entries = entries;
    return wgpuDeviceCreateBindGroup(r->device, &desc);
}

void pgraph_wgpu_release_texture_bind_groups(PGRAPHWgpuState *r)
{
    for (int i = 0; i < WGPU_TEXTURE_BIND_CACHE; i++) {
        WgpuTextureBindCacheEntry *entry = &r->texture_bind_cache[i];

        if (!entry->used) {
            continue;
        }
        wgpuBindGroupRelease(entry->group);
        entry->group = NULL;
        entry->used = false;
    }
}

WGPUBindGroup pgraph_wgpu_get_texture_bind_group(PGRAPHWgpuState *r,
                                                 const WgpuTextureLayout *l)
{
    WgpuTextureBindKey key;
    WgpuTextureBindCacheEntry *free_slot = NULL;
    WgpuTextureBindCacheEntry *oldest = NULL;

    texture_bind_key(r, l, &key);
    for (int i = 0; i < WGPU_TEXTURE_BIND_CACHE; i++) {
        WgpuTextureBindCacheEntry *entry = &r->texture_bind_cache[i];

        if (!entry->used) {
            if (!free_slot) {
                free_slot = entry;
            }
            continue;
        }
        if (!memcmp(&entry->key, &key, sizeof(key))) {
            entry->stamp = ++r->texture_bind_stamp;
            return entry->group;
        }
        if (!oldest || entry->stamp < oldest->stamp) {
            oldest = entry;
        }
    }

    WgpuTextureBindCacheEntry *slot = free_slot;
    if (!slot) {
        slot = oldest;
        wgpuBindGroupRelease(slot->group);
        slot->group = NULL;
        slot->used = false;
    }

    slot->key = key;
    slot->group = create_texture_bind_group(r, l, &key);
    slot->stamp = ++r->texture_bind_stamp;
    slot->used = true;
    return slot->group;
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    TextureBinding *binding = container_of(node, TextureBinding, node);

    binding->texture = NULL;
    binding->view = NULL;
    binding->sampler = NULL;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHWgpuState *r = container_of(lru, PGRAPHWgpuState, texture_cache);
    TextureBinding *binding = container_of(node, TextureBinding, node);

    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == binding) {
            return false;
        }
    }
    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHWgpuState *r = container_of(lru, PGRAPHWgpuState, texture_cache);
    TextureBinding *binding = container_of(node, TextureBinding, node);

    release_texture_object(r, binding);
    if (binding->sampler) {
        purge_texture_bind_groups(r, NULL, binding->sampler);
        wgpuSamplerRelease(binding->sampler);
        binding->sampler = NULL;
    }
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node,
                                        const void *key)
{
    TextureBinding *binding = container_of(node, TextureBinding, node);
    return memcmp(&binding->key, key, sizeof(TextureKey));
}

void pgraph_wgpu_init_textures(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = PSH_TEXTURE_TYPE_2D; i <= PSH_TEXTURE_TYPE_CUBE; i++) {
        create_dummy_texture(r, i);
    }

    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Dummy sampler");
    r->dummy_sampler = wgpuDeviceCreateSampler(r->device, &desc);

    lru_init(&r->texture_cache);
    r->texture_cache_entries =
        g_malloc0_n(TEXTURE_CACHE_SIZE, sizeof(TextureBinding));
    for (int i = 0; i < TEXTURE_CACHE_SIZE; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;

    init_texture_decode(r);
}

void pgraph_wgpu_finalize_textures(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }
    pgraph_wgpu_release_texture_bind_groups(r);

    if (r->texture_cache_entries) {
        lru_flush(&r->texture_cache);
        g_free(r->texture_cache_entries);
        r->texture_cache_entries = NULL;
    }

    for (int i = 0; i < ARRAY_SIZE(r->dummy_textures); i++) {
        if (r->dummy_texture_views[i]) {
            wgpuTextureViewRelease(r->dummy_texture_views[i]);
            r->dummy_texture_views[i] = NULL;
        }
        if (r->dummy_textures[i]) {
            wgpuTextureRelease(r->dummy_textures[i]);
            r->dummy_textures[i] = NULL;
        }
    }
    if (r->dummy_sampler) {
        wgpuSamplerRelease(r->dummy_sampler);
        r->dummy_sampler = NULL;
    }
    finalize_texture_decode(r);
}
