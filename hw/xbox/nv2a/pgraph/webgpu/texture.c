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

static void write_level(PGRAPHWgpuState *r, WGPUTexture texture,
                        const WgpuColorFormatInfo *f, WGPUTextureFormat format,
                        unsigned int layer, unsigned int level,
                        unsigned int width, unsigned int height,
                        unsigned int depth, const uint8_t *src,
                        size_t src_pitch)
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
        write_level(r, texture, wf, format, 0, 0, adjusted_width,
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
                    write_level(r, texture, wf, format, layer, level, width,
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
                    write_level(r, texture, wf, format, layer, level, width,
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
                write_level(r, texture, wf, format, 0, level, width, height,
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
                write_level(r, texture, wf, format, 0, level, width, height,
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

static void release_texture_object(TextureBinding *binding)
{
    /* Only released: unsubmitted commands may still reference the texture */
    if (binding->view) {
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

    /* Render targets reach textures through guest RAM for now: write back any
     * surface this texture may read. */
    pgraph_wgpu_download_surfaces_in_range_if_dirty(pg, key.texture_vram_offset,
                                                    key.texture_length);

    uint64_t key_hash = fast_hash((const uint8_t *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *binding = container_of(node, TextureBinding, node);
    bool found = binding->texture != NULL;

    bool possibly_dirty = !found || binding->possibly_dirty;
    possibly_dirty |= check_texture_possibly_dirty(d, &key);
    r->texture_bindings[texture_idx] = binding;

    if (found) {
        if (possibly_dirty) {
            uint64_t hash = hash_texture_data(d, &key);
            binding->possibly_dirty = false;
            if (hash != binding->hash) {
                /* Queue writes run before unsubmitted commands, which may
                 * still sample the old contents, so use a new texture */
                release_texture_object(binding);
                create_texture_object(r, binding);
                upload_texture_data(pg, binding);
                binding->hash = hash;
            }
        }
        return;
    }

    memcpy(&binding->key, &key, sizeof(key));
    binding->possibly_dirty = false;
    binding->hash = hash_texture_data(d, &key);
    binding->type = get_texture_type(r, &state);

    assert(kelvin_color_format_info_map[state.color_format].bytes_per_pixel);
    assert(state.dimensionality == 2 || state.dimensionality == 3);

    create_texture_object(r, binding);
    binding->sampler =
        create_sampler(r, &key, binding->type != PSH_TEXTURE_TYPE_2D_UINT);
    upload_texture_data(pg, binding);
}

/* Guest CPU writes to texture memory don't set texture_dirty, so every enabled
 * texture is looked up again on each draw, which checks its pages. The bind
 * group is only rebuilt when a binding or its texture object changes. */
void pgraph_wgpu_bind_textures(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        TextureBinding *old_binding = r->texture_bindings[i];
        WGPUTextureView old_view = old_binding ? old_binding->view : NULL;

        if (pgraph_is_texture_enabled(pg, i)) {
            bind_texture(pg, i);
        } else {
            r->texture_bindings[i] = NULL;
        }
        pg->texture_dirty[i] = false;

        TextureBinding *binding = r->texture_bindings[i];
        if (binding != old_binding || (binding && binding->view != old_view)) {
            r->texture_bindings_changed = true;
        }
    }
}

WGPUBindGroup pgraph_wgpu_get_texture_bind_group(PGRAPHWgpuState *r,
                                                 const WgpuTextureLayout *l)
{
    if (r->texture_bind_group && !r->texture_bindings_changed &&
        r->texture_bind_group_layout == l) {
        return r->texture_bind_group;
    }
    if (r->texture_bind_group) {
        wgpuBindGroupRelease(r->texture_bind_group);
    }

    WGPUBindGroupEntry entries[2 * NV2A_MAX_TEXTURES];
    size_t num_entries = 0;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (l->types[i] == PSH_TEXTURE_TYPE_NONE) {
            continue;
        }

        TextureBinding *binding = r->texture_bindings[i];
        WGPUTextureView view = r->dummy_texture_views[l->types[i]];
        WGPUSampler sampler = r->dummy_sampler;
        if (binding && binding->type == l->types[i]) {
            view = binding->view;
            sampler = binding->sampler;
        } else if (binding) {
            NV2A_UNIMPLEMENTED("Texture %d type %d, shader expects %d", i,
                               binding->type, l->types[i]);
        }

        WGPUBindGroupEntry texture = WGPU_BIND_GROUP_ENTRY_INIT;
        texture.binding = 2 * i;
        texture.textureView = view;
        entries[num_entries++] = texture;

        WGPUBindGroupEntry sampler_entry = WGPU_BIND_GROUP_ENTRY_INIT;
        sampler_entry.binding = 2 * i + 1;
        sampler_entry.sampler = sampler;
        entries[num_entries++] = sampler_entry;
    }

    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Textures");
    desc.layout = l->bind_group_layout;
    desc.entryCount = num_entries;
    desc.entries = entries;
    r->texture_bind_group = wgpuDeviceCreateBindGroup(r->device, &desc);
    r->texture_bind_group_layout = l;
    r->texture_bindings_changed = false;
    return r->texture_bind_group;
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
    TextureBinding *binding = container_of(node, TextureBinding, node);

    release_texture_object(binding);
    if (binding->sampler) {
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
}

void pgraph_wgpu_finalize_textures(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }
    if (r->texture_bind_group) {
        wgpuBindGroupRelease(r->texture_bind_group);
        r->texture_bind_group = NULL;
    }
    r->texture_bind_group_layout = NULL;

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
}
