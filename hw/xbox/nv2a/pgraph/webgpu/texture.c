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
#include "renderer.h"

/* Texture uploads are not implemented yet, so every stage samples a 1x1
 * placeholder of the type the shader declares. */

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

void pgraph_wgpu_init_textures(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    for (int i = PSH_TEXTURE_TYPE_2D; i <= PSH_TEXTURE_TYPE_CUBE; i++) {
        create_dummy_texture(r, i);
    }

    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Dummy sampler");
    r->dummy_sampler = wgpuDeviceCreateSampler(r->device, &desc);
}

void pgraph_wgpu_finalize_textures(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

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

WGPUBindGroup pgraph_wgpu_create_dummy_texture_bind_group(
    PGRAPHWgpuState *r, WGPUBindGroupLayout layout,
    const PshTextureType types[NV2A_MAX_TEXTURES])
{
    WGPUBindGroupEntry entries[2 * NV2A_MAX_TEXTURES];
    size_t num_entries = 0;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (types[i] == PSH_TEXTURE_TYPE_NONE) {
            continue;
        }
        WGPUBindGroupEntry texture = WGPU_BIND_GROUP_ENTRY_INIT;
        texture.binding = 2 * i;
        texture.textureView = r->dummy_texture_views[types[i]];
        entries[num_entries++] = texture;

        WGPUBindGroupEntry sampler = WGPU_BIND_GROUP_ENTRY_INIT;
        sampler.binding = 2 * i + 1;
        sampler.sampler = r->dummy_sampler;
        entries[num_entries++] = sampler;
    }

    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Dummy textures");
    desc.layout = layout;
    desc.entryCount = num_entries;
    desc.entries = entries;
    return wgpuDeviceCreateBindGroup(r->device, &desc);
}
