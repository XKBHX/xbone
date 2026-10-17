/*
 * WebGPU logo, controller art, and framebuffer blit.
 *
 * These draw on the UI-thread device from wgpu-ui.cc. ImGui samples the
 * resulting views directly. The game image is still a GL texture, so the
 * framebuffer blit copies it with glGetTexImage until the UI pass moves to
 * the PFIFO device.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.hh"
#include "gl-helpers.hh"
#include "wgpu-helpers.hh"
#include "wgpu-ui.hh"

#include "ui/shader/ui-widgets-wgsl.h"

#include "data/controller_mask.png.h"
#include "data/controller_mask_s.png.h"
#include "data/logo_sdf.png.h"
#include "data/xemu_64x64.png.h"
#include "data/xmu_mask.png.h"

#include <fpng.h>
#include "stb_image.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

struct WidgetUniform {
    float scale_offset[4];
    float tex_scale_offset[4];
    float color_primary[4];
    float color_secondary[4];
    float color_fill[4];
    float params[4];
    uint32_t palette[256];
};

static_assert(offsetof(WidgetUniform, palette) == 96,
              "palette must match the WGSL uniform");
static_assert(sizeof(WidgetUniform) == 1120, "WGSL uniform size");

struct ArtImage {
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUBindGroup group;
    int width;
    int height;
    uint64_t id;
};

struct FboImage {
    Fbo *owner;
    WGPUTexture texture;
    WGPUTextureView view;
    uint64_t id;
};

struct SourceImage {
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUBindGroup linear;
    WGPUBindGroup nearest;
    int width;
    int height;
};

static bool g_ready;
static WGPUShaderModule g_module;
static WGPUBindGroupLayout g_uniform_layout;
static WGPUBindGroupLayout g_texture_layout;
static WGPUPipelineLayout g_pipeline_layout;
static WGPURenderPipeline g_mask_pipe[3];
static WGPURenderPipeline g_logo_pipe;
static WGPURenderPipeline g_gamma_pipe;
static WGPUSampler g_sampler_linear;
static WGPUSampler g_sampler_nearest;
static WGPUBuffer g_uniform_buffer;
static WGPUBindGroup g_uniform_group;
static WGPUBuffer g_vertices;
static WGPUBuffer g_indices;
static uint32_t g_uniform_align = 256;
static uint32_t g_uniform_stride;
static uint32_t g_uniform_capacity;
static uint32_t g_uniform_cursor;
static ArtImage g_art[WGPU_ART_COUNT];
static ArtImage g_icon;
static SourceImage g_source;
static std::vector<FboImage> g_fbos;

static WGPUCommandEncoder g_encoder;
static WGPURenderPassEncoder g_pass;
static std::vector<uint8_t> g_pass_uniforms;
static uint32_t g_pass_base;
static int g_target_w;
static int g_target_h;
static int g_art_index;
static int g_blend;
static bool g_logo;
static bool g_clip_flip;
static float g_time;

static WGPUTexture g_png_texture;
static WGPUTextureView g_png_view;
static WGPUBuffer g_png_readback;
static int g_png_w;
static int g_png_h;

static const float kVerts[] = {
    -1.f, -1.f, 0.f, 0.f, -1.f, 1.f,  0.f, 1.f,
    1.f,  1.f,  1.f, 1.f, 1.f,  -1.f, 1.f, 0.f,
};
static const uint32_t kIndices[] = { 0, 1, 2, 0, 2, 3 };

static WGPUStringView wgpu_label(const char *text)
{
    return WGPUStringView{ text, WGPU_STRLEN };
}

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void copy_message(char *dst, size_t dst_size, WGPUStringView message)
{
    size_t length = 0;

    if (message.data) {
        length = message.length == WGPU_STRLEN ? strlen(message.data) :
                                                 message.length;
    }
    if (length >= dst_size) {
        length = dst_size - 1;
    }
    if (length) {
        memcpy(dst, message.data, length);
    }
    dst[length] = '\0';
}

static bool wait_future(WGPUFuture future)
{
    WGPUInstance instance = xemu_wgpu_ui_instance();
    WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;

    if (!instance) {
        return false;
    }
    info.future = future;
    return wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX) ==
           WGPUWaitStatus_Success;
}

struct IdleState {
    WGPUQueueWorkDoneStatus status;
};

static void work_done_callback(WGPUQueueWorkDoneStatus status,
                               WGPUStringView message, void *userdata1,
                               void *userdata2)
{
    (void)message;
    (void)userdata2;
    ((IdleState *)userdata1)->status = status;
}

static bool wait_idle(void)
{
    IdleState state = {};
    WGPUQueueWorkDoneCallbackInfo callback =
        WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    WGPUQueue queue = xemu_wgpu_ui_queue();

    if (!queue) {
        return false;
    }
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = work_done_callback;
    callback.userdata1 = &state;
    return wait_future(wgpuQueueOnSubmittedWorkDone(queue, callback)) &&
           state.status == WGPUQueueWorkDoneStatus_Success;
}

struct ErrorState {
    WGPUErrorType type;
    char message[512];
};

static void pop_error_callback(WGPUPopErrorScopeStatus status,
                               WGPUErrorType type, WGPUStringView message,
                               void *userdata1, void *userdata2)
{
    (void)userdata2;
    ErrorState *state = (ErrorState *)userdata1;

    state->type = status == WGPUPopErrorScopeStatus_Success ?
                      type :
                      WGPUErrorType_Unknown;
    if (state->type != WGPUErrorType_NoError) {
        copy_message(state->message, sizeof(state->message), message);
    }
}

static bool pop_errors(const char *what)
{
    ErrorState state = {};
    WGPUPopErrorScopeCallbackInfo callback =
        WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;

    state.type = WGPUErrorType_NoError;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = pop_error_callback;
    callback.userdata1 = &state;
    if (!wait_future(
            wgpuDevicePopErrorScope(xemu_wgpu_ui_device(), callback)) ||
        state.type != WGPUErrorType_NoError) {
        fprintf(stderr, "[WebGPU] UI %s: %s\n", what,
                state.message[0] ? state.message : "validation error");
        return false;
    }
    return true;
}

static void release_art(ArtImage *art)
{
    if (art->id) {
        xemu_wgpu_ui_unregister_texture(art->id);
        art->id = 0;
    }
    if (art->group) {
        wgpuBindGroupRelease(art->group);
        art->group = NULL;
    }
    if (art->view) {
        wgpuTextureViewRelease(art->view);
        art->view = NULL;
    }
    if (art->texture) {
        wgpuTextureRelease(art->texture);
        art->texture = NULL;
    }
    art->width = 0;
    art->height = 0;
}

static WGPUTexture make_texture(const char *label, WGPUTextureUsage usage,
                                int width, int height)
{
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;

    desc.label = wgpu_label(label);
    desc.usage = usage;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = { (uint32_t)width, (uint32_t)height, 1 };
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    return wgpuDeviceCreateTexture(xemu_wgpu_ui_device(), &desc);
}

static WGPUTextureView make_view(WGPUTexture texture)
{
    WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;

    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.dimension = WGPUTextureViewDimension_2D;
    desc.mipLevelCount = 1;
    desc.arrayLayerCount = 1;
    desc.aspect = WGPUTextureAspect_All;
    return wgpuTextureCreateView(texture, &desc);
}

static WGPUBindGroup make_texture_group(WGPUTextureView view,
                                        WGPUSampler sampler)
{
    WGPUBindGroupEntry entries[2];
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;

    entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].textureView = view;
    entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].sampler = sampler;
    desc.layout = g_texture_layout;
    desc.entryCount = 2;
    desc.entries = entries;
    return wgpuDeviceCreateBindGroup(xemu_wgpu_ui_device(), &desc);
}

static uint32_t row_stride(int width)
{
    return align_up((uint32_t)width * 4u, 256u);
}

static void upload_rgba(WGPUTexture texture, const uint8_t *tight, int width,
                        int height)
{
    uint32_t stride = row_stride(width);
    std::vector<uint8_t> pixels((size_t)stride * (size_t)height);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    WGPUExtent3D size = { (uint32_t)width, (uint32_t)height, 1 };

    for (int y = 0; y < height; y++) {
        memcpy(pixels.data() + (size_t)y * stride,
               tight + (size_t)y * (size_t)width * 4u, (size_t)width * 4u);
    }
    dst.texture = texture;
    dst.aspect = WGPUTextureAspect_All;
    layout.bytesPerRow = stride;
    layout.rowsPerImage = height;
    wgpuQueueWriteTexture(xemu_wgpu_ui_queue(), &dst, pixels.data(),
                          pixels.size(), &layout, &size);
}

static bool load_art(ArtImage *art, const unsigned char *buf, unsigned size,
                     bool flip, bool publish)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *data;

    stbi_set_flip_vertically_on_load(flip);
    data = stbi_load_from_memory(buf, size, &width, &height, &channels, 4);
    if (!data || width <= 0 || height <= 0) {
        return false;
    }
    art->width = width;
    art->height = height;
    art->texture = make_texture(
        "UI art", WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        width, height);
    if (art->texture) {
        art->view = make_view(art->texture);
    }
    if (art->view) {
        art->group = make_texture_group(art->view, g_sampler_linear);
        upload_rgba(art->texture, data, width, height);
    }
    stbi_image_free(data);
    if (!art->group) {
        release_art(art);
        return false;
    }
    if (publish) {
        art->id = (uint64_t)(uintptr_t)art->view;
        xemu_wgpu_ui_register_texture(art->id);
    }
    return true;
}

static void set_blend(WGPUBlendState *blend, int mode)
{
    blend->color.operation = WGPUBlendOperation_Add;
    blend->alpha.operation = WGPUBlendOperation_Add;
    if (mode == DECAL_BLEND_UNDER) {
        blend->color.srcFactor = WGPUBlendFactor_OneMinusDstAlpha;
        blend->color.dstFactor = WGPUBlendFactor_One;
    } else if (mode == DECAL_BLEND_ALPHA) {
        blend->color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend->color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    } else {
        blend->color.srcFactor = WGPUBlendFactor_One;
        blend->color.dstFactor = WGPUBlendFactor_Zero;
    }
    blend->alpha.srcFactor = blend->color.srcFactor;
    blend->alpha.dstFactor = blend->color.dstFactor;
}

static WGPURenderPipeline make_pipeline(const char *entry, int blend)
{
    WGPUBlendState blend_state = WGPU_BLEND_STATE_INIT;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    WGPUVertexAttribute attributes[2];
    WGPUVertexBufferLayout vertex_buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;

    set_blend(&blend_state, blend);
    target.format = WGPUTextureFormat_RGBA8Unorm;
    target.blend = &blend_state;
    target.writeMask = WGPUColorWriteMask_All;

    attributes[0] = WGPU_VERTEX_ATTRIBUTE_INIT;
    attributes[0].format = WGPUVertexFormat_Float32x2;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1] = WGPU_VERTEX_ATTRIBUTE_INIT;
    attributes[1].format = WGPUVertexFormat_Float32x2;
    attributes[1].offset = 8;
    attributes[1].shaderLocation = 1;
    vertex_buffer.arrayStride = 16;
    vertex_buffer.stepMode = WGPUVertexStepMode_Vertex;
    vertex_buffer.attributeCount = 2;
    vertex_buffer.attributes = attributes;

    desc.label = wgpu_label(entry);
    desc.layout = g_pipeline_layout;
    desc.vertex.module = g_module;
    desc.vertex.entryPoint = wgpu_label("vs");
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vertex_buffer;
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.cullMode = WGPUCullMode_None;
    desc.multisample.count = 1;
    desc.multisample.mask = 0xFFFFFFFFu;
    fragment.module = g_module;
    fragment.entryPoint = wgpu_label(entry);
    fragment.targetCount = 1;
    fragment.targets = &target;
    desc.fragment = &fragment;
    return wgpuDeviceCreateRenderPipeline(xemu_wgpu_ui_device(), &desc);
}

static WGPUBuffer make_buffer(const char *label, WGPUBufferUsage usage,
                              const void *data, size_t size)
{
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUBuffer buffer;

    desc.label = wgpu_label(label);
    desc.usage = usage | WGPUBufferUsage_CopyDst;
    desc.size = size;
    buffer = wgpuDeviceCreateBuffer(xemu_wgpu_ui_device(), &desc);
    if (buffer && data && size) {
        wgpuQueueWriteBuffer(xemu_wgpu_ui_queue(), buffer, 0, data, size);
    }
    return buffer;
}

static WGPUSampler make_sampler(WGPUFilterMode filter)
{
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;

    desc.addressModeU = WGPUAddressMode_ClampToEdge;
    desc.addressModeV = WGPUAddressMode_ClampToEdge;
    desc.addressModeW = WGPUAddressMode_ClampToEdge;
    desc.magFilter = filter;
    desc.minFilter = filter;
    desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    desc.maxAnisotropy = 1;
    return wgpuDeviceCreateSampler(xemu_wgpu_ui_device(), &desc);
}

static bool create_pipelines(void)
{
    WGPUDevice device = xemu_wgpu_ui_device();
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    WGPUBindGroupLayoutEntry uniform_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    WGPUBindGroupLayoutEntry texture_entries[2];
    WGPUBindGroupLayoutDescriptor uniform_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUBindGroupLayoutDescriptor texture_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUBindGroupLayout layouts[2];
    WGPUPipelineLayoutDescriptor layout_desc =
        WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPULimits limits = WGPU_LIMITS_INIT;
    bool ok;

    if (wgpuDeviceGetLimits(device, &limits) == WGPUStatus_Success &&
        limits.minUniformBufferOffsetAlignment) {
        g_uniform_align = limits.minUniformBufferOffsetAlignment;
    }
    g_uniform_stride = align_up(sizeof(WidgetUniform), g_uniform_align);
    g_uniform_capacity = g_uniform_stride * 1024u;

    source.code = wgpu_label(ui_widgets_wgsl_src);
    module_desc.nextInChain = &source.chain;
    module_desc.label = wgpu_label("UI widgets");
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    g_module = wgpuDeviceCreateShaderModule(device, &module_desc);

    uniform_entry.binding = 0;
    uniform_entry.visibility =
        WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    uniform_entry.buffer.type = WGPUBufferBindingType_Uniform;
    uniform_entry.buffer.hasDynamicOffset = true;
    uniform_entry.buffer.minBindingSize = sizeof(WidgetUniform);
    uniform_desc.entryCount = 1;
    uniform_desc.entries = &uniform_entry;
    g_uniform_layout = wgpuDeviceCreateBindGroupLayout(device, &uniform_desc);

    texture_entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    texture_entries[0].binding = 0;
    texture_entries[0].visibility = WGPUShaderStage_Fragment;
    texture_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    texture_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    texture_entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    texture_entries[1].binding = 1;
    texture_entries[1].visibility = WGPUShaderStage_Fragment;
    texture_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    texture_desc.entryCount = 2;
    texture_desc.entries = texture_entries;
    g_texture_layout = wgpuDeviceCreateBindGroupLayout(device, &texture_desc);

    layouts[0] = g_uniform_layout;
    layouts[1] = g_texture_layout;
    layout_desc.bindGroupLayoutCount = 2;
    layout_desc.bindGroupLayouts = layouts;
    g_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_desc);

    g_mask_pipe[DECAL_BLEND_REPLACE] =
        make_pipeline("fs_mask", DECAL_BLEND_REPLACE);
    g_mask_pipe[DECAL_BLEND_UNDER] =
        make_pipeline("fs_mask", DECAL_BLEND_UNDER);
    g_mask_pipe[DECAL_BLEND_ALPHA] =
        make_pipeline("fs_mask", DECAL_BLEND_ALPHA);
    g_logo_pipe = make_pipeline("fs_logo", DECAL_BLEND_REPLACE);
    g_gamma_pipe = make_pipeline("fs_gamma", DECAL_BLEND_REPLACE);
    ok = pop_errors("widget shaders");
    return ok && g_module && g_uniform_layout && g_texture_layout &&
           g_pipeline_layout && g_mask_pipe[0] && g_mask_pipe[1] &&
           g_mask_pipe[2] && g_logo_pipe && g_gamma_pipe;
}

static bool reserve_uniforms(void)
{
    uint32_t need = g_uniform_stride * 64u;

    if (g_uniform_cursor > g_uniform_capacity ||
        g_uniform_capacity - g_uniform_cursor < need) {
        if (!wait_idle()) {
            return false;
        }
        g_uniform_cursor = 0;
    }
    g_pass_base = g_uniform_cursor;
    g_pass_uniforms.clear();
    return true;
}

static bool append_uniform(const WidgetUniform &uniform, uint32_t *offset)
{
    uint32_t local = (uint32_t)g_pass_uniforms.size();

    if (g_pass_base + local + g_uniform_stride > g_uniform_capacity) {
        fprintf(stderr, "[WebGPU] UI uniform buffer is full\n");
        return false;
    }
    g_pass_uniforms.resize(local + g_uniform_stride, 0);
    memcpy(g_pass_uniforms.data() + local, &uniform, sizeof(uniform));
    *offset = g_pass_base + local;
    return true;
}

static void end_render_pass(void)
{
    if (!g_pass) {
        return;
    }
    if (!g_pass_uniforms.empty()) {
        wgpuQueueWriteBuffer(xemu_wgpu_ui_queue(), g_uniform_buffer,
                             g_pass_base, g_pass_uniforms.data(),
                             g_pass_uniforms.size());
        g_uniform_cursor = g_pass_base + (uint32_t)g_pass_uniforms.size();
        g_pass_uniforms.clear();
    }
    wgpuRenderPassEncoderEnd(g_pass);
    wgpuRenderPassEncoderRelease(g_pass);
    g_pass = NULL;
}

static void submit_encoder(void)
{
    WGPUCommandBufferDescriptor desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer commands;

    if (!g_encoder) {
        return;
    }
    commands = wgpuCommandEncoderFinish(g_encoder, &desc);
    wgpuCommandEncoderRelease(g_encoder);
    g_encoder = NULL;
    if (commands) {
        WGPUQueue queue = xemu_wgpu_ui_queue();
        wgpuQueueSubmit(queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
    }
}

static bool begin_pass(WGPUTextureView view, float clear_alpha)
{
    WGPUCommandEncoderDescriptor encoder_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;

    if (g_pass) {
        end_render_pass();
        submit_encoder();
    }
    if (!reserve_uniforms()) {
        return false;
    }
    encoder_desc.label = wgpu_label("UI decal");
    g_encoder =
        wgpuDeviceCreateCommandEncoder(xemu_wgpu_ui_device(), &encoder_desc);
    if (!g_encoder) {
        return false;
    }
    color.view = view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{ 0, 0, 0, (double)clear_alpha };
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;
    g_pass = wgpuCommandEncoderBeginRenderPass(g_encoder, &pass_desc);
    if (!g_pass) {
        submit_encoder();
        return false;
    }
    wgpuRenderPassEncoderSetVertexBuffer(g_pass, 0, g_vertices, 0,
                                         sizeof(kVerts));
    wgpuRenderPassEncoderSetIndexBuffer(
        g_pass, g_indices, WGPUIndexFormat_Uint32, 0, sizeof(kIndices));
    return true;
}

static void unpack_color(uint32_t color, float out[4])
{
    out[0] = (float)((color >> 24) & 0xff) / 255.0f;
    out[1] = (float)((color >> 16) & 0xff) / 255.0f;
    out[2] = (float)((color >> 8) & 0xff) / 255.0f;
    out[3] = (float)(color & 0xff) / 255.0f;
}

static void fill_palette(WidgetUniform *uniform)
{
    const uint8_t *palette = nv2a_get_dac_palette();

    for (int i = 0; i < 256; i++) {
        uniform->palette[i] =
            (uint32_t)((palette[i * 3 + 2] << 16) | (palette[i * 3 + 1] << 8) |
                       palette[i * 3]);
    }
}

static void draw_quad(WGPURenderPipeline pipeline, WGPUBindGroup textures,
                      const WidgetUniform &uniform)
{
    uint32_t offset = 0;

    if (!g_pass || !pipeline || !textures ||
        !append_uniform(uniform, &offset)) {
        return;
    }
    wgpuRenderPassEncoderSetPipeline(g_pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(g_pass, 0, g_uniform_group, 1, &offset);
    wgpuRenderPassEncoderSetBindGroup(g_pass, 1, textures, 0, nullptr);
    wgpuRenderPassEncoderDrawIndexed(g_pass, 6, 1, 0, 0, 0);
}

static bool read_gl_rgba(uint32_t name, std::vector<uint8_t> *pixels,
                         int *width, int *height)
{
    GLint w = 0;
    GLint h = 0;
    GLint pack_align = 4;
    GLint pack_row = 0;
    GLenum error;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, name);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0) {
        return false;
    }
    pixels->resize((size_t)w * (size_t)h * 4u);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    while (glGetError() != GL_NO_ERROR) {
    }
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels->data());
    error = glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
    glPixelStorei(GL_PACK_ROW_LENGTH, pack_row);
    if (error != GL_NO_ERROR) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

static void release_source(void)
{
    if (g_source.linear) {
        wgpuBindGroupRelease(g_source.linear);
        g_source.linear = NULL;
    }
    if (g_source.nearest) {
        wgpuBindGroupRelease(g_source.nearest);
        g_source.nearest = NULL;
    }
    if (g_source.view) {
        wgpuTextureViewRelease(g_source.view);
        g_source.view = NULL;
    }
    if (g_source.texture) {
        wgpuTextureRelease(g_source.texture);
        g_source.texture = NULL;
    }
    g_source.width = 0;
    g_source.height = 0;
}

static WGPUBindGroup ensure_source(uint32_t name, bool linear)
{
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;

    if (!read_gl_rgba(name, &pixels, &width, &height)) {
        return NULL;
    }
    if (!g_source.texture || g_source.width != width ||
        g_source.height != height) {
        release_source();
        g_source.texture = make_texture("UI framebuffer",
                                        WGPUTextureUsage_TextureBinding |
                                            WGPUTextureUsage_CopyDst,
                                        width, height);
        if (!g_source.texture) {
            return NULL;
        }
        g_source.view = make_view(g_source.texture);
        g_source.linear = make_texture_group(g_source.view, g_sampler_linear);
        g_source.nearest = make_texture_group(g_source.view, g_sampler_nearest);
        g_source.width = width;
        g_source.height = height;
    }
    if (!g_source.linear || !g_source.nearest) {
        return NULL;
    }
    upload_rgba(g_source.texture, pixels.data(), width, height);
    return linear ? g_source.linear : g_source.nearest;
}

static FboImage *find_fbo(Fbo *fbo)
{
    for (FboImage &image : g_fbos) {
        if (image.owner == fbo) {
            return &image;
        }
    }
    return NULL;
}

bool wgpu_helpers_active(void)
{
    return g_ready;
}

bool wgpu_helpers_init(void)
{
    WGPUBindGroupEntry uniform_entry = WGPU_BIND_GROUP_ENTRY_INIT;
    WGPUBindGroupDescriptor uniform_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUDevice device = xemu_wgpu_ui_device();

    if (g_ready) {
        return true;
    }
    if (!device || !create_pipelines()) {
        wgpu_helpers_shutdown();
        return false;
    }
    g_sampler_linear = make_sampler(WGPUFilterMode_Linear);
    g_sampler_nearest = make_sampler(WGPUFilterMode_Nearest);
    g_uniform_buffer = make_buffer("UI uniforms", WGPUBufferUsage_Uniform, NULL,
                                   g_uniform_capacity);
    g_vertices =
        make_buffer("UI quad", WGPUBufferUsage_Vertex, kVerts, sizeof(kVerts));
    g_indices = make_buffer("UI quad indices", WGPUBufferUsage_Index, kIndices,
                            sizeof(kIndices));
    if (!g_sampler_linear || !g_sampler_nearest || !g_uniform_buffer ||
        !g_vertices || !g_indices) {
        wgpu_helpers_shutdown();
        return false;
    }
    uniform_entry.binding = 0;
    uniform_entry.buffer = g_uniform_buffer;
    uniform_entry.offset = 0;
    uniform_entry.size = sizeof(WidgetUniform);
    uniform_desc.layout = g_uniform_layout;
    uniform_desc.entryCount = 1;
    uniform_desc.entries = &uniform_entry;
    g_uniform_group = wgpuDeviceCreateBindGroup(device, &uniform_desc);
    if (!g_uniform_group ||
        !load_art(&g_art[WGPU_ART_DUKE], controller_mask_data,
                  controller_mask_size, true, false) ||
        !load_art(&g_art[WGPU_ART_S], controller_mask_s_data,
                  controller_mask_s_size, true, false) ||
        !load_art(&g_art[WGPU_ART_XMU], xmu_mask_data, xmu_mask_size, true,
                  false) ||
        !load_art(&g_art[WGPU_ART_LOGO], logo_sdf_data, logo_sdf_size, true,
                  false) ||
        !load_art(&g_icon, xemu_64x64_data, xemu_64x64_size, false, true)) {
        wgpu_helpers_shutdown();
        return false;
    }
    g_ready = true;
    fprintf(stderr, "[WebGPU] UI shaders: WGSL\n");
    return true;
}

void wgpu_helpers_shutdown(void)
{
    g_ready = false;
    if (g_pass || g_encoder) {
        end_render_pass();
        submit_encoder();
    }
    for (FboImage &image : g_fbos) {
        if (image.id) {
            xemu_wgpu_ui_unregister_texture(image.id);
        }
        if (image.view) {
            wgpuTextureViewRelease(image.view);
        }
        if (image.texture) {
            wgpuTextureRelease(image.texture);
        }
    }
    g_fbos.clear();
    for (int i = 0; i < WGPU_ART_COUNT; i++) {
        release_art(&g_art[i]);
    }
    release_art(&g_icon);
    release_source();
    if (g_png_readback) {
        wgpuBufferRelease(g_png_readback);
        g_png_readback = NULL;
    }
    if (g_png_view) {
        wgpuTextureViewRelease(g_png_view);
        g_png_view = NULL;
    }
    if (g_png_texture) {
        wgpuTextureRelease(g_png_texture);
        g_png_texture = NULL;
    }
    g_png_w = 0;
    g_png_h = 0;
    for (int i = 0; i < 3; i++) {
        if (g_mask_pipe[i]) {
            wgpuRenderPipelineRelease(g_mask_pipe[i]);
            g_mask_pipe[i] = NULL;
        }
    }
    if (g_logo_pipe) {
        wgpuRenderPipelineRelease(g_logo_pipe);
        g_logo_pipe = NULL;
    }
    if (g_gamma_pipe) {
        wgpuRenderPipelineRelease(g_gamma_pipe);
        g_gamma_pipe = NULL;
    }
    if (g_uniform_group) {
        wgpuBindGroupRelease(g_uniform_group);
        g_uniform_group = NULL;
    }
    if (g_pipeline_layout) {
        wgpuPipelineLayoutRelease(g_pipeline_layout);
        g_pipeline_layout = NULL;
    }
    if (g_uniform_layout) {
        wgpuBindGroupLayoutRelease(g_uniform_layout);
        g_uniform_layout = NULL;
    }
    if (g_texture_layout) {
        wgpuBindGroupLayoutRelease(g_texture_layout);
        g_texture_layout = NULL;
    }
    if (g_uniform_buffer) {
        wgpuBufferRelease(g_uniform_buffer);
        g_uniform_buffer = NULL;
    }
    if (g_vertices) {
        wgpuBufferRelease(g_vertices);
        g_vertices = NULL;
    }
    if (g_indices) {
        wgpuBufferRelease(g_indices);
        g_indices = NULL;
    }
    if (g_sampler_linear) {
        wgpuSamplerRelease(g_sampler_linear);
        g_sampler_linear = NULL;
    }
    if (g_sampler_nearest) {
        wgpuSamplerRelease(g_sampler_nearest);
        g_sampler_nearest = NULL;
    }
    if (g_module) {
        wgpuShaderModuleRelease(g_module);
        g_module = NULL;
    }
    g_uniform_cursor = 0;
    g_pass_uniforms.clear();
}

void wgpu_helpers_fbo_create(Fbo *fbo)
{
    FboImage image = {};

    if (!g_ready || !fbo || fbo->w <= 0 || fbo->h <= 0) {
        return;
    }
    image.owner = fbo;
    image.texture = make_texture("UI fbo",
                                 WGPUTextureUsage_TextureBinding |
                                     WGPUTextureUsage_RenderAttachment,
                                 fbo->w, fbo->h);
    if (!image.texture) {
        return;
    }
    image.view = make_view(image.texture);
    if (!image.view) {
        wgpuTextureRelease(image.texture);
        return;
    }
    image.id = (uint64_t)(uintptr_t)image.view;
    xemu_wgpu_ui_register_texture(image.id);
    g_fbos.push_back(image);
}

void wgpu_helpers_fbo_destroy(Fbo *fbo)
{
    for (size_t i = 0; i < g_fbos.size(); i++) {
        if (g_fbos[i].owner != fbo) {
            continue;
        }
        if (g_fbos[i].id) {
            xemu_wgpu_ui_unregister_texture(g_fbos[i].id);
        }
        if (g_fbos[i].view) {
            wgpuTextureViewRelease(g_fbos[i].view);
        }
        if (g_fbos[i].texture) {
            wgpuTextureRelease(g_fbos[i].texture);
        }
        g_fbos.erase(g_fbos.begin() + i);
        return;
    }
}

void wgpu_helpers_fbo_target(Fbo *fbo)
{
    FboImage *image = find_fbo(fbo);

    if (!image || !image->view) {
        return;
    }
    g_target_w = fbo->w;
    g_target_h = fbo->h;
    g_clip_flip = true;
    begin_pass(image->view, 0);
}

void wgpu_helpers_fbo_restore(Fbo *fbo)
{
    (void)fbo;
    if (!g_pass) {
        return;
    }
    end_render_pass();
    submit_encoder();
}

uint64_t wgpu_helpers_fbo_texture(Fbo *fbo)
{
    FboImage *image = find_fbo(fbo);

    return image ? image->id : 0;
}

void wgpu_helpers_begin(int art, bool logo, float time_sec)
{
    g_art_index = art;
    g_logo = logo;
    g_time = time_sec;
    g_blend = DECAL_BLEND_REPLACE;
}

void wgpu_helpers_blend(int blend)
{
    g_blend = blend;
}

void wgpu_helpers_target_size(float *w, float *h)
{
    *w = (float)g_target_w;
    *h = (float)g_target_h;
}

void wgpu_helpers_art_size(float *w, float *h)
{
    if (g_art_index < 0 || g_art_index >= WGPU_ART_COUNT ||
        g_art[g_art_index].width <= 0) {
        *w = 1;
        *h = 1;
        return;
    }
    *w = (float)g_art[g_art_index].width;
    *h = (float)g_art[g_art_index].height;
}

void wgpu_helpers_draw(const float scale_offset[4],
                       const float tex_scale_offset[4], uint32_t primary,
                       uint32_t secondary, uint32_t fill)
{
    WidgetUniform uniform = {};
    WGPURenderPipeline pipeline;
    WGPUBindGroup textures;

    if (g_art_index < 0 || g_art_index >= WGPU_ART_COUNT) {
        return;
    }
    memcpy(uniform.scale_offset, scale_offset, sizeof(uniform.scale_offset));
    memcpy(uniform.tex_scale_offset, tex_scale_offset,
           sizeof(uniform.tex_scale_offset));
    unpack_color(primary, uniform.color_primary);
    unpack_color(secondary, uniform.color_secondary);
    unpack_color(fill, uniform.color_fill);
    uniform.params[0] = g_time;
    uniform.params[3] = g_clip_flip ? 1.0f : 0.0f;
    if (g_blend < 0 || g_blend > DECAL_BLEND_ALPHA) {
        g_blend = DECAL_BLEND_REPLACE;
    }
    pipeline = g_logo ? g_logo_pipe : g_mask_pipe[g_blend];
    textures = g_art[g_art_index].group;
    draw_quad(pipeline, textures, uniform);
}

static void draw_gamma(WGPUBindGroup source, const float scale[2], bool flip)
{
    WidgetUniform uniform = {};

    uniform.scale_offset[0] = scale[0];
    uniform.scale_offset[1] = scale[1];
    uniform.tex_scale_offset[0] = 1.0f;
    uniform.tex_scale_offset[1] = 1.0f;
    uniform.params[2] = flip ? 1.0f : 0.0f;
    fill_palette(&uniform);
    draw_quad(g_gamma_pipe, source, uniform);
}

bool wgpu_helpers_render_framebuffer(uint32_t tex, int width, int height,
                                     bool flip, const float scale[2])
{
    WGPUTextureView target;
    bool linear;
    bool screen_off;
    WGPUBindGroup source = NULL;

    if (!g_ready || width <= 0 || height <= 0) {
        return false;
    }
    target = xemu_wgpu_ui_prepare_target(width, height);
    if (!target) {
        return false;
    }
    screen_off = nv2a_get_screen_off();
    linear = g_config.display.filtering != CONFIG_DISPLAY_FILTERING_NEAREST;
    if (!screen_off) {
        source = ensure_source(tex, linear);
        if (!source) {
            return false;
        }
    }
    g_clip_flip = false;
    if (!begin_pass(target, 1)) {
        return false;
    }
    if (source) {
        draw_gamma(source, scale, flip);
    }
    end_render_pass();
    submit_encoder();
    xemu_wgpu_ui_mark_scene();
    return true;
}

struct MapState {
    WGPUMapAsyncStatus status;
};

static void buffer_map_callback(WGPUMapAsyncStatus status,
                                WGPUStringView message, void *userdata1,
                                void *userdata2)
{
    (void)userdata2;
    MapState *state = (MapState *)userdata1;

    state->status = status;
    if (status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "[WebGPU] UI image readback failed (%d): %.*s\n",
                status, (int)message.length, message.data ? message.data : "");
    }
}

static bool ensure_png_target(int width, int height)
{
    uint32_t stride = row_stride(width);
    size_t bytes = (size_t)stride * (size_t)height;
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;

    if (g_png_texture && g_png_w == width && g_png_h == height) {
        return true;
    }
    if (g_png_readback) {
        wgpuBufferRelease(g_png_readback);
        g_png_readback = NULL;
    }
    if (g_png_view) {
        wgpuTextureViewRelease(g_png_view);
        g_png_view = NULL;
    }
    if (g_png_texture) {
        wgpuTextureRelease(g_png_texture);
        g_png_texture = NULL;
    }
    g_png_texture = make_texture(
        "UI png", WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
        width, height);
    if (!g_png_texture) {
        return false;
    }
    g_png_view = make_view(g_png_texture);
    desc.label = wgpu_label("UI png readback");
    desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    desc.size = bytes;
    g_png_readback = wgpuDeviceCreateBuffer(xemu_wgpu_ui_device(), &desc);
    if (!g_png_view || !g_png_readback) {
        return false;
    }
    g_png_w = width;
    g_png_h = height;
    return true;
}

bool wgpu_helpers_framebuffer_png(uint32_t tex, bool flip, int width,
                                  int height, std::vector<uint8_t> &png)
{
    bool linear;
    WGPUBindGroup source;
    float scale[2] = { 1.0f, 1.0f };
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUExtent3D extent;
    MapState map_state = {};
    WGPUBufferMapCallbackInfo map_callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    uint32_t stride;
    size_t bytes;
    std::vector<uint8_t> rgb;

    if (!g_ready || width <= 0 || height <= 0 ||
        !ensure_png_target(width, height)) {
        return false;
    }
    linear = g_config.display.filtering != CONFIG_DISPLAY_FILTERING_NEAREST;
    source = ensure_source(tex, linear);
    if (!source || !begin_pass(g_png_view, 1)) {
        return false;
    }
    if (!nv2a_get_screen_off()) {
        draw_gamma(source, scale, flip);
    }
    end_render_pass();
    stride = row_stride(width);
    bytes = (size_t)stride * (size_t)height;
    src.texture = g_png_texture;
    src.aspect = WGPUTextureAspect_All;
    dst.buffer = g_png_readback;
    dst.layout.bytesPerRow = stride;
    dst.layout.rowsPerImage = height;
    extent = { (uint32_t)width, (uint32_t)height, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(g_encoder, &src, &dst, &extent);
    submit_encoder();

    map_callback.mode = WGPUCallbackMode_WaitAnyOnly;
    map_callback.callback = buffer_map_callback;
    map_callback.userdata1 = &map_state;
    if (!wait_future(wgpuBufferMapAsync(g_png_readback, WGPUMapMode_Read, 0,
                                        bytes, map_callback)) ||
        map_state.status != WGPUMapAsyncStatus_Success) {
        return false;
    }
    const void *mapped =
        wgpuBufferGetConstMappedRange(g_png_readback, 0, bytes);
    if (!mapped) {
        wgpuBufferUnmap(g_png_readback);
        return false;
    }
    rgb.resize((size_t)width * (size_t)height * 3u);
    for (int y = 0; y < height; y++) {
        const uint8_t *row = (const uint8_t *)mapped + (size_t)y * stride;
        uint8_t *out = rgb.data() + (size_t)y * (size_t)width * 3u;
        for (int x = 0; x < width; x++) {
            out[x * 3 + 0] = row[x * 4 + 0];
            out[x * 3 + 1] = row[x * 4 + 1];
            out[x * 3 + 2] = row[x * 4 + 2];
        }
    }
    wgpuBufferUnmap(g_png_readback);
    return fpng::fpng_encode_image_to_memory(rgb.data(), width, height, 3, png);
}

uint64_t wgpu_helpers_icon(int *width, int *height)
{
    if (width) {
        *width = g_icon.width;
    }
    if (height) {
        *height = g_icon.height;
    }
    return g_icon.id;
}
