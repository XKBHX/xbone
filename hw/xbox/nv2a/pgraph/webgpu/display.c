/*
 * Geforce NV2A PGRAPH WebGPU Renderer
 *
 * Copyright (c) 2026 xemu Project
 *
 * Based on the Vulkan implementation:
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

#include <math.h>

/* std140-style layout matching the WGSL DisplayParams struct. vec3f is
 * aligned to 16, so pvideo_color_key starts at byte 64 and the struct ends
 * at 80. */
typedef struct DisplayParams {
    float line_offset;
    uint32_t pvideo_enable;
    float display_size[2];
    float pvideo_in_pos[2];
    uint32_t pvideo_color_key_enable;
    uint32_t pad0;
    float pvideo_pos[4];
    float pvideo_scale[4];
    float pvideo_color_key[3];
    uint32_t pad1;
} DisplayParams;

_Static_assert(sizeof(DisplayParams) == 80, "DisplayParams size");
_Static_assert(offsetof(DisplayParams, display_size) == 8, "display_size");
_Static_assert(offsetof(DisplayParams, pvideo_in_pos) == 16, "in_pos");
_Static_assert(offsetof(DisplayParams, pvideo_pos) == 32, "pvideo_pos");
_Static_assert(offsetof(DisplayParams, pvideo_scale) == 48, "pvideo_scale");
_Static_assert(offsetof(DisplayParams, pvideo_color_key) == 64, "color_key");

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

/* Same composite as vk/display.c, including the Y flip. WebGPU stores row 0
 * at the top. glTexImage2D treats that row as the bottom, and the UI samples
 * the texture with flip disabled, so the flip makes the picture upright.
 * The overlay sample stays in uniform control flow: WGSL rejects
 * textureSample under a per-pixel branch. */
static const char display_wgsl[] =
    "struct DisplayParams {\n"
    "    line_offset: f32,\n"
    "    pvideo_enable: u32,\n"
    "    display_size: vec2f,\n"
    "    pvideo_in_pos: vec2f,\n"
    "    pvideo_color_key_enable: u32,\n"
    "    pad0: u32,\n"
    "    pvideo_pos: vec4f,\n"
    "    pvideo_scale: vec4f,\n"
    "    pvideo_color_key: vec3f,\n"
    "}\n"
    "@group(0) @binding(0) var frame_tex: texture_2d<f32>;\n"
    "@group(0) @binding(1) var frame_samp: sampler;\n"
    "@group(0) @binding(2) var pvideo_tex: texture_2d<f32>;\n"
    "@group(0) @binding(3) var pvideo_samp: sampler;\n"
    "@group(0) @binding(4) var<uniform> params: DisplayParams;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) "
    "vec4f {\n"
    "    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    let display_size = params.display_size;\n"
    "    var tex_coord = pos.xy / display_size;\n"
    "    let frame_size = vec2f(textureDimensions(frame_tex));\n"
    "    let rel = display_size.y / frame_size.y / params.line_offset;\n"
    "    tex_coord.y = 1.0 + rel * (tex_coord.y - 1.0);\n"
    "    tex_coord.y = 1.0 - tex_coord.y;\n"
    "    var color = textureSample(frame_tex, frame_samp, tex_coord);\n"
    "    if (params.pvideo_enable != 0u) {\n"
    "        let screen_coord = vec2f(pos.x, display_size.y - pos.y) *\n"
    "            params.pvideo_scale.z;\n"
    "        let output_min = params.pvideo_pos.xy;\n"
    "        let output_max = params.pvideo_pos.xy + params.pvideo_pos.zw;\n"
    "        let out_xy = screen_coord - output_min;\n"
    "        let in_st = (params.pvideo_in_pos + out_xy *\n"
    "            params.pvideo_scale.xy) /\n"
    "            vec2f(textureDimensions(pvideo_tex));\n"
    "        let overlay = textureSample(pvideo_tex, pvideo_samp, in_st);\n"
    "        let clipped = screen_coord.x < output_min.x ||\n"
    "            screen_coord.y < output_min.y ||\n"
    "            screen_coord.x > output_max.x ||\n"
    "            screen_coord.y > output_max.y;\n"
    "        let key_ok = params.pvideo_color_key_enable == 0u ||\n"
    "            all(color.rgb == params.pvideo_color_key);\n"
    "        if (!clipped && key_ok) {\n"
    "            color = overlay;\n"
    "        }\n"
    "    }\n"
    "    return color;\n"
    "}\n";

#ifdef CONFIG_OPENGL
/* The GL name belongs to the UI context, which is current in present and
 * not necessarily in finalize. A name retired at shutdown is deleted the
 * next time that context presents. */
static GLuint retired_gl_texture;

static void retire_gl_texture(GLuint tex)
{
    if (tex) {
        retired_gl_texture = tex;
    }
}
#endif

static void release_bind_group(WgpuDisplayState *disp)
{
    if (disp->bind_group) {
        wgpuBindGroupRelease(disp->bind_group);
        disp->bind_group = NULL;
    }
}

static void destroy_texture(WGPUTexture *texture, WGPUTextureView *view)
{
    if (*view) {
        wgpuTextureViewRelease(*view);
        *view = NULL;
    }
    if (*texture) {
        wgpuTextureRelease(*texture);
        *texture = NULL;
    }
}

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);

    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void convert_cr8yb8cb8ya8(uint8_t *data_out, const uint8_t *data_in,
                                 unsigned int width, unsigned int height,
                                 unsigned int pitch)
{
    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *line = &data_in[y * pitch];
        uint32_t row_offset = y * width;

        for (unsigned int x = 0; x < width; x++) {
            uint8_t *pixel = &data_out[(row_offset + x) * 4];

            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
}

static PvideoState get_pvideo_state(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PvideoState state = { 0 };

    /* SIZE_IN of 0xFFFFFFFF is not what hardware uses to hide the overlay,
     * but games write it while the contents are not valid. Treat it as off,
     * matching the OpenGL and Vulkan renderers. */
    state.enabled =
        (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) &&
        d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    if (!state.enabled) {
        return state;
    }

    state.base = d->pvideo.regs[NV_PVIDEO_BASE];
    state.limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    state.offset = d->pvideo.regs[NV_PVIDEO_OFFSET];
    state.pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    state.format =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);

    /* Other color formats are not implemented in any renderer. */
    assert(state.format == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8);

    state.in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    state.in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);
    state.out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    state.out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);
    if (!state.out_width || !state.out_height) {
        state.enabled = false;
        return state;
    }

    state.in_s =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_S);
    state.in_t =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_T);

    uint32_t ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    uint32_t dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    state.scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(ds_dx, state.out_width);
    state.scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(dt_dy, state.out_height);

    /* Hardware caps SIZE_IN to SIZE_OUT instead of scaling. SIZE_IN can also
     * be left huge while the overlay is coming up or down. */
    if (state.in_width > state.out_width) {
        state.in_width = floorf((float)state.out_width * state.scale_x + 0.5f);
    }
    if (state.in_height > state.out_height) {
        state.in_height =
            floorf((float)state.out_height * state.scale_y + 0.5f);
    }

    state.out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    state.out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);
    state.color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);
    state.color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    if (!state.in_width || !state.in_height || !state.out_width ||
        !state.out_height || !state.pitch) {
        state.enabled = false;
        return state;
    }

    assert(state.offset + state.pitch * state.in_height <= state.limit);
    hwaddr end =
        state.base + state.offset + (hwaddr)state.pitch * state.in_height;
    assert(end <= memory_region_size(d->vram));
    return state;
}

static WGPUSampler create_sampler(PGRAPHWgpuState *r, const char *label,
                                  WGPUFilterMode mag, WGPUFilterMode min)
{
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;

    desc.label = pgraph_wgpu_str(label);
    desc.addressModeU = WGPUAddressMode_Repeat;
    desc.addressModeV = WGPUAddressMode_Repeat;
    desc.addressModeW = WGPUAddressMode_Repeat;
    desc.magFilter = mag;
    desc.minFilter = min;
    desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    return wgpuDeviceCreateSampler(r->device, &desc);
}

static void ensure_texture(PGRAPHWgpuState *r, WGPUTexture *texture,
                           WGPUTextureView *view, unsigned int *cur_w,
                           unsigned int *cur_h, unsigned int width,
                           unsigned int height, WGPUTextureUsage usage,
                           const char *label)
{
    if (*texture && *cur_w == width && *cur_h == height) {
        return;
    }

    /* The previous frame's bind group may still hold this view. */
    release_bind_group(&r->display);
    destroy_texture(texture, view);

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str(label);
    desc.usage = usage;
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.size = (WGPUExtent3D){ width, height, 1 };
    *texture = wgpuDeviceCreateTexture(r->device, &desc);
    *view = wgpuTextureCreateView(*texture, NULL);
    *cur_w = width;
    *cur_h = height;
}

static void upload_pvideo_image(PGRAPHState *pg, const PvideoState *state)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;
    unsigned int width = state->in_width;
    unsigned int height = state->in_height;
    uint32_t packed_row = width * 4;
    uint32_t row_bytes = ROUND_UP(packed_row, 256);

    ensure_texture(r, &disp->pvideo_image, &disp->pvideo_view,
                   &disp->pvideo_width, &disp->pvideo_height, width, height,
                   WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                   "PVIDEO");

    g_autofree uint8_t *packed = g_malloc(packed_row * height);
    g_autofree uint8_t *rows = g_malloc0((size_t)row_bytes * height);
    convert_cr8yb8cb8ya8(packed, d->vram_ptr + state->base + state->offset,
                         width, height, state->pitch);
    for (unsigned int y = 0; y < height; y++) {
        memcpy(rows + (size_t)y * row_bytes, packed + (size_t)y * packed_row,
               packed_row);
    }

    /* Queue writes run before commands that have not been submitted yet. */
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);

    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    WGPUExtent3D extent = { width, height, 1 };
    dst.texture = disp->pvideo_image;
    layout.bytesPerRow = row_bytes;
    layout.rowsPerImage = height;
    wgpuQueueWriteTexture(r->queue, &dst, rows, (size_t)row_bytes * height,
                          &layout, &extent);
}

static void ensure_readback(PGRAPHWgpuState *r, uint64_t size)
{
    WgpuDisplayState *disp = &r->display;

    if (disp->readback && disp->readback_size >= size) {
        return;
    }
    if (disp->readback) {
        wgpuBufferRelease(disp->readback);
    }

    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Display readback");
    desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    desc.size = ROUND_UP(size, 1024 * 1024);
    disp->readback = wgpuDeviceCreateBuffer(r->device, &desc);
    disp->readback_size = desc.size;
}

static bool readback_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;
    uint32_t packed_row = disp->width * 4;
    uint32_t src_row = ROUND_UP(packed_row, 256);
    uint64_t src_size = (uint64_t)src_row * disp->height;
    size_t packed_size = (size_t)packed_row * disp->height;

    ensure_readback(r, src_size);

    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUExtent3D extent = { disp->width, disp->height, 1 };
    src.texture = disp->image;
    dst.buffer = disp->readback;
    dst.layout.bytesPerRow = src_row;
    dst.layout.rowsPerImage = disp->height;
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);

    const uint8_t *mapped =
        pgraph_wgpu_read_buffer(r, disp->readback, src_size);
    if (!mapped) {
        return false;
    }

    if (disp->pixels_size != packed_size) {
        disp->pixels = g_realloc(disp->pixels, packed_size);
        disp->pixels_size = packed_size;
    }
    for (unsigned int y = 0; y < disp->height; y++) {
        memcpy(disp->pixels + (size_t)y * packed_row,
               mapped + (size_t)y * src_row, packed_row);
    }
    wgpuBufferUnmap(disp->readback);
    return true;
}

static void update_bind_group(PGRAPHWgpuState *r, WGPUTextureView frame)
{
    WgpuDisplayState *disp = &r->display;
    WGPUTextureView pvideo = disp->pvideo_view ?
                                 disp->pvideo_view :
                                 r->dummy_texture_views[PSH_TEXTURE_TYPE_2D];
    WGPUBindGroupEntry entries[5];

    release_bind_group(disp);

    entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].textureView = frame;

    entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].sampler = disp->frame_sampler;

    entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[2].binding = 2;
    entries[2].textureView = pvideo;

    entries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[3].binding = 3;
    entries[3].sampler = disp->pvideo_sampler;

    entries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[4].binding = 4;
    entries[4].buffer = disp->uniform;
    entries[4].size = sizeof(DisplayParams);

    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Display");
    desc.layout = disp->bind_group_layout;
    desc.entryCount = ARRAY_SIZE(entries);
    desc.entries = entries;
    disp->bind_group = wgpuDeviceCreateBindGroup(r->device, &desc);
}

static void record_display_pass(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;

    pgraph_wgpu_end_render_pass(r);
    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);

    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = disp->image_view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = (WGPUColor){ 0, 0, 0, 1 };

    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.label = pgraph_wgpu_str("Display");
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;

    WGPURenderPassEncoder pass =
        wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    wgpuRenderPassEncoderSetPipeline(pass, disp->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, disp->bind_group, 0, NULL);
    wgpuRenderPassEncoderSetViewport(pass, 0, 0, disp->width, disp->height, 0,
                                     1);
    wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, disp->width, disp->height);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

static WGPUBindGroupLayoutEntry texture_entry(uint32_t binding)
{
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.texture.sampleType = WGPUTextureSampleType_Float;
    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    return entry;
}

static WGPUBindGroupLayoutEntry sampler_entry(uint32_t binding)
{
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.sampler.type = WGPUSamplerBindingType_Filtering;
    return entry;
}

void pgraph_wgpu_init_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;
    WGPUBindGroupLayoutEntry entries[5];

    disp->module = pgraph_wgpu_create_wgsl_module(r, "Display", display_wgsl);

    entries[0] = texture_entry(0);
    entries[1] = sampler_entry(1);
    entries[2] = texture_entry(2);
    entries[3] = sampler_entry(3);
    entries[4] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[4].binding = 4;
    entries[4].visibility = WGPUShaderStage_Fragment;
    entries[4].buffer.type = WGPUBufferBindingType_Uniform;
    entries[4].buffer.minBindingSize = sizeof(DisplayParams);

    WGPUBindGroupLayoutDescriptor bgl_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.label = pgraph_wgpu_str("Display");
    bgl_desc.entryCount = ARRAY_SIZE(entries);
    bgl_desc.entries = entries;
    disp->bind_group_layout =
        wgpuDeviceCreateBindGroupLayout(r->device, &bgl_desc);

    WGPUPipelineLayoutDescriptor layout_desc =
        WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &disp->bind_group_layout;
    WGPUPipelineLayout layout =
        wgpuDeviceCreatePipelineLayout(r->device, &layout_desc);

    WGPUColorTargetState color_target = WGPU_COLOR_TARGET_STATE_INIT;
    color_target.format = WGPUTextureFormat_RGBA8Unorm;
    color_target.writeMask = WGPUColorWriteMask_Red | WGPUColorWriteMask_Green |
                             WGPUColorWriteMask_Blue | WGPUColorWriteMask_Alpha;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = disp->module;
    fragment.entryPoint = pgraph_wgpu_str("fs");
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Display");
    desc.layout = layout;
    desc.vertex.module = disp->module;
    desc.vertex.entryPoint = pgraph_wgpu_str("vs");
    desc.fragment = &fragment;
    disp->pipeline = wgpuDeviceCreateRenderPipeline(r->device, &desc);
    wgpuPipelineLayoutRelease(layout);

    disp->frame_sampler = create_sampler(
        r, "Display frame", WGPUFilterMode_Nearest, WGPUFilterMode_Nearest);
    disp->pvideo_sampler = create_sampler(
        r, "Display PVIDEO", WGPUFilterMode_Linear, WGPUFilterMode_Nearest);

    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = pgraph_wgpu_str("Display uniforms");
    buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    buffer_desc.size = sizeof(DisplayParams);
    disp->uniform = wgpuDeviceCreateBuffer(r->device, &buffer_desc);
}

void pgraph_wgpu_finalize_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;

    release_bind_group(disp);
    destroy_texture(&disp->image, &disp->image_view);
    destroy_texture(&disp->pvideo_image, &disp->pvideo_view);
    disp->width = 0;
    disp->height = 0;
    disp->pvideo_width = 0;
    disp->pvideo_height = 0;

    if (disp->readback) {
        wgpuBufferRelease(disp->readback);
        disp->readback = NULL;
        disp->readback_size = 0;
    }
    g_free(disp->pixels);
    disp->pixels = NULL;
    disp->pixels_size = 0;
    disp->pixels_valid = false;

    if (disp->uniform) {
        wgpuBufferRelease(disp->uniform);
        disp->uniform = NULL;
    }
    if (disp->frame_sampler) {
        wgpuSamplerRelease(disp->frame_sampler);
        disp->frame_sampler = NULL;
    }
    if (disp->pvideo_sampler) {
        wgpuSamplerRelease(disp->pvideo_sampler);
        disp->pvideo_sampler = NULL;
    }
    if (disp->pipeline) {
        wgpuRenderPipelineRelease(disp->pipeline);
        disp->pipeline = NULL;
    }
    if (disp->bind_group_layout) {
        wgpuBindGroupLayoutRelease(disp->bind_group_layout);
        disp->bind_group_layout = NULL;
    }
    if (disp->module) {
        wgpuShaderModuleRelease(disp->module);
        disp->module = NULL;
    }

#ifdef CONFIG_OPENGL
    retire_gl_texture(disp->gl_texture);
    disp->gl_texture = 0;
    disp->gl_width = 0;
    disp->gl_height = 0;
#endif
}

void pgraph_wgpu_render_display(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;

    disp->pixels_valid = false;
    if (!disp->pipeline) {
        return;
    }

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_wgpu_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height || !surface->view) {
        return;
    }

    unsigned int width = 0, height = 0;
    d->vga.get_resolution(&d->vga, (int *)&width, (int *)&height);
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }
    pgraph_apply_scaling_factor(pg, &width, &height);
    if (!width || !height) {
        return;
    }

    if (r->encoder && surface->draw_time >= r->encoder_start_time) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);
    }
    pgraph_wgpu_upload_surface_data(d, surface, !tcg_enabled());

    PvideoState pvideo = get_pvideo_state(pg);
    if (pvideo.enabled) {
        pgraph_wgpu_download_surfaces_in_range_if_dirty(
            pg, pvideo.base + pvideo.offset,
            (hwaddr)pvideo.pitch * pvideo.in_height);
        upload_pvideo_image(pg, &pvideo);
    }

    ensure_texture(r, &disp->image, &disp->image_view, &disp->width,
                   &disp->height, width, height,
                   WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
                   "Display");

    DisplayParams params = { 0 };
    int line_offset = vga_display_params.line_offset ?
                          surface->pitch / vga_display_params.line_offset :
                          1;
    params.line_offset = line_offset ? line_offset : 1;
    params.display_size[0] = width;
    params.display_size[1] = height;
    params.pvideo_enable = pvideo.enabled;
    if (pvideo.enabled) {
        params.pvideo_color_key_enable = pvideo.color_key_enabled;
        params.pvideo_color_key[0] =
            GET_MASK(pvideo.color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0f;
        params.pvideo_color_key[1] =
            GET_MASK(pvideo.color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0f;
        params.pvideo_color_key[2] =
            GET_MASK(pvideo.color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0f;
        params.pvideo_in_pos[0] = pvideo.in_s / 16.f;
        params.pvideo_in_pos[1] = pvideo.in_t / 8.f;
        params.pvideo_pos[0] = pvideo.out_x;
        params.pvideo_pos[1] = pvideo.out_y;
        params.pvideo_pos[2] = pvideo.out_width;
        params.pvideo_pos[3] = pvideo.out_height;
        params.pvideo_scale[0] = pvideo.scale_x;
        params.pvideo_scale[1] = pvideo.scale_y;
        unsigned int scale = MAX(pg->surface_scale_factor, 1);
        params.pvideo_scale[2] = 1.0f / scale;
        params.pvideo_scale[3] = 1.0f;
    }

    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);
    wgpuQueueWriteBuffer(r->queue, disp->uniform, 0, &params, sizeof(params));
    update_bind_group(r, surface->view);
    record_display_pass(pg);
    disp->pixels_valid = readback_display(pg);
}

#ifdef CONFIG_OPENGL
GLuint pgraph_wgpu_present_display(PGRAPHWgpuState *r)
{
    WgpuDisplayState *disp = &r->display;

    if (retired_gl_texture) {
        glDeleteTextures(1, &retired_gl_texture);
        retired_gl_texture = 0;
    }
    if (!disp->pixels_valid || !disp->pixels || !disp->width || !disp->height) {
        return 0;
    }

    if (!disp->gl_texture) {
        glGenTextures(1, &disp->gl_texture);
    }
    glBindTexture(GL_TEXTURE_2D, disp->gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (disp->gl_width != disp->width || disp->gl_height != disp->height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, disp->width, disp->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, disp->pixels);
        disp->gl_width = disp->width;
        disp->gl_height = disp->height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, disp->width, disp->height,
                        GL_RGBA, GL_UNSIGNED_BYTE, disp->pixels);
    }
    return disp->gl_texture;
}
#endif
