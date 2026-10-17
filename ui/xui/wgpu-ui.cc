/*
 * Dear ImGui on a UI-thread WebGPU device.
 *
 * The NV2A device stays on the PFIFO thread. This device exists so ImGui can
 * record on the thread that builds the draw lists. The pass is drawn into an
 * rgba8unorm target, read back, and blended over the OpenGL framebuffer.
 * Step 4 of the WebGPU UI plan moves the pass onto the PFIFO device.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.hh"
#include "wgpu-ui.hh"

#include <imgui_impl_wgpu.h>
#include <webgpu/webgpu.h>

#include <cstdint>
#include <cstring>
#include <vector>

struct GlImage {
    GLuint name;
    int width;
    int height;
    WGPUTexture texture;
    WGPUTextureView view;
    bool used;
};

static bool g_active;
static WGPUInstance g_instance;
static WGPUAdapter g_adapter;
static WGPUDevice g_device;
static WGPUQueue g_queue;
static char g_vendor_text[256];
static char g_adapter_text[256];
static char g_driver_text[512];
static WGPUTexture g_target;
static WGPUTextureView g_target_view;
static WGPUBuffer g_readback;
static int g_target_width;
static int g_target_height;
static uint32_t g_target_stride;
static WGPUTexture g_white;
static WGPUTextureView g_white_view;
static ImTextureID g_white_id;
static std::vector<GlImage> g_gl_images;
static std::vector<uint64_t> g_ui_textures;

static bool is_ui_texture(ImTextureID id);
static bool g_scene_drawn;

static GLuint g_overlay_program;
static GLuint g_overlay_vao;
static GLuint g_overlay_vbo;
static GLuint g_overlay_tex;

static WGPUStringView wgpu_label(const char *text)
{
    return WGPUStringView{ text, WGPU_STRLEN };
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

struct AdapterRequest {
    WGPURequestAdapterStatus status;
    WGPUAdapter adapter;
    char message[256];
};

struct DeviceRequest {
    WGPURequestDeviceStatus status;
    WGPUDevice device;
    char message[256];
};

static void request_adapter_callback(WGPURequestAdapterStatus status,
                                     WGPUAdapter adapter,
                                     WGPUStringView message, void *userdata1,
                                     void *userdata2)
{
    (void)userdata2;
    AdapterRequest *request = (AdapterRequest *)userdata1;

    request->status = status;
    request->adapter = adapter;
    copy_message(request->message, sizeof(request->message), message);
}

static void request_device_callback(WGPURequestDeviceStatus status,
                                    WGPUDevice device, WGPUStringView message,
                                    void *userdata1, void *userdata2)
{
    (void)userdata2;
    DeviceRequest *request = (DeviceRequest *)userdata1;

    request->status = status;
    request->device = device;
    copy_message(request->message, sizeof(request->message), message);
}

static void uncaptured_error_callback(WGPUDevice const *device,
                                      WGPUErrorType type,
                                      WGPUStringView message, void *userdata1,
                                      void *userdata2)
{
    (void)device;
    (void)userdata1;
    (void)userdata2;
    fprintf(stderr, "[WebGPU] UI error (%d): %.*s\n", type, (int)message.length,
            message.data ? message.data : "");
}

static void device_lost_callback(WGPUDevice const *device,
                                 WGPUDeviceLostReason reason,
                                 WGPUStringView message, void *userdata1,
                                 void *userdata2)
{
    (void)device;
    (void)userdata1;
    (void)userdata2;
    if (reason == WGPUDeviceLostReason_Destroyed ||
        reason == WGPUDeviceLostReason_CallbackCancelled) {
        return;
    }
    fprintf(stderr, "[WebGPU] UI device lost (%d): %.*s\n", reason,
            (int)message.length, message.data ? message.data : "");
}

static bool wait_future(WGPUFuture future)
{
    WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;

    info.future = future;
    return wgpuInstanceWaitAny(g_instance, 1, &info, UINT64_MAX) ==
           WGPUWaitStatus_Success;
}

static uint32_t align_row(uint32_t row_bytes)
{
    return (row_bytes + 255u) & ~255u;
}

static const char *backend_name(WGPUBackendType type)
{
    switch (type) {
    case WGPUBackendType_D3D11:
        return "D3D11";
    case WGPUBackendType_D3D12:
        return "D3D12";
    case WGPUBackendType_Metal:
        return "Metal";
    case WGPUBackendType_Vulkan:
        return "Vulkan";
    case WGPUBackendType_OpenGL:
        return "OpenGL";
    case WGPUBackendType_OpenGLES:
        return "OpenGL ES";
    case WGPUBackendType_Null:
        return "Null";
    case WGPUBackendType_WebGPU:
        return "WebGPU";
    default:
        return "WebGPU";
    }
}

static void store_renderer(WGPUAdapter adapter)
{
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;

    g_vendor_text[0] = '\0';
    g_adapter_text[0] = '\0';
    g_driver_text[0] = '\0';
    if (wgpuAdapterGetInfo(adapter, &info) != WGPUStatus_Success) {
        snprintf(g_vendor_text, sizeof(g_vendor_text), "Dawn");
        snprintf(g_adapter_text, sizeof(g_adapter_text), "WebGPU");
        snprintf(g_driver_text, sizeof(g_driver_text), "WebGPU");
        return;
    }
    copy_message(g_vendor_text, sizeof(g_vendor_text), info.vendor);
    copy_message(g_adapter_text, sizeof(g_adapter_text), info.device);
    copy_message(g_driver_text, sizeof(g_driver_text), info.description);
    if (!g_vendor_text[0]) {
        snprintf(g_vendor_text, sizeof(g_vendor_text), "%s",
                 backend_name(info.backendType));
    }
    if (!g_adapter_text[0]) {
        snprintf(g_adapter_text, sizeof(g_adapter_text), "%s",
                 g_driver_text[0] ? g_driver_text : "WebGPU");
    }
    if (!g_driver_text[0]) {
        snprintf(g_driver_text, sizeof(g_driver_text), "%s",
                 backend_name(info.backendType));
    }
    wgpuAdapterInfoFreeMembers(info);
}

static void release_gl_images(void)
{
    for (GlImage &image : g_gl_images) {
        if (image.view) {
            wgpuTextureViewRelease(image.view);
        }
        if (image.texture) {
            wgpuTextureRelease(image.texture);
        }
    }
    g_gl_images.clear();
}

static bool create_overlay_gl(void)
{
    static const char *vs_src =
        "#version 150 core\n"
        "in vec2 a_pos;\n"
        "in vec2 a_uv;\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "    v_uv = a_uv;\n"
        "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_src =
        "#version 150 core\n"
        "in vec2 v_uv;\n"
        "out vec4 frag;\n"
        "uniform sampler2D u_tex;\n"
        "void main() { frag = texture(u_tex, v_uv); }\n";
    /* Top of the window samples row 0. WebGPU and glTexImage2D both treat
     * the first uploaded row as v = 0, and v = 0 is the bottom of a GL quad
     * whose uv.y grows upward, so the top vertices use uv.y = 0. */
    static const float verts[] = {
        -1.f, -1.f, 0.f, 1.f, 1.f, -1.f, 1.f, 1.f,
        -1.f, 1.f,  0.f, 0.f, 1.f, 1.f,  1.f, 0.f,
    };
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    GLint ok = 0;
    char log[512] = { 0 };
    GLsizei len = 0;

    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof(log), &len, log);
        fprintf(stderr, "[WebGPU] UI overlay vertex shader failed: %s\n", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof(log), &len, log);
        fprintf(stderr, "[WebGPU] UI overlay fragment shader failed: %s\n",
                log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    g_overlay_program = glCreateProgram();
    glAttachShader(g_overlay_program, vs);
    glAttachShader(g_overlay_program, fs);
    /* GLSL 150 has no layout(location) on inputs. */
    glBindAttribLocation(g_overlay_program, 0, "a_pos");
    glBindAttribLocation(g_overlay_program, 1, "a_uv");
    glLinkProgram(g_overlay_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(g_overlay_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "[WebGPU] UI overlay program failed to link\n");
        glDeleteProgram(g_overlay_program);
        g_overlay_program = 0;
        return false;
    }

    glGenVertexArrays(1, &g_overlay_vao);
    glGenBuffers(1, &g_overlay_vbo);
    glBindVertexArray(g_overlay_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_overlay_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                          (void *)(sizeof(float) * 2));
    glBindVertexArray(0);
    glGenTextures(1, &g_overlay_tex);
    return true;
}

static void destroy_overlay_gl(void)
{
    if (!SDL_GL_GetCurrentContext()) {
        return;
    }
    if (g_overlay_tex) {
        glDeleteTextures(1, &g_overlay_tex);
        g_overlay_tex = 0;
    }
    if (g_overlay_vbo) {
        glDeleteBuffers(1, &g_overlay_vbo);
        g_overlay_vbo = 0;
    }
    if (g_overlay_vao) {
        glDeleteVertexArrays(1, &g_overlay_vao);
        g_overlay_vao = 0;
    }
    if (g_overlay_program) {
        glDeleteProgram(g_overlay_program);
        g_overlay_program = 0;
    }
}

static bool create_white_texture(void)
{
    const uint8_t pixel[4] = { 255, 255, 255, 255 };
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    WGPUExtent3D size = { 1, 1, 1 };
    uint8_t row[256] = { 0 };

    desc.label = wgpu_label("ImGui white");
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = size;
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    g_white = wgpuDeviceCreateTexture(g_device, &desc);
    if (!g_white) {
        return false;
    }

    view_desc.format = WGPUTextureFormat_RGBA8Unorm;
    view_desc.dimension = WGPUTextureViewDimension_2D;
    view_desc.mipLevelCount = 1;
    view_desc.arrayLayerCount = 1;
    view_desc.aspect = WGPUTextureAspect_All;
    g_white_view = wgpuTextureCreateView(g_white, &view_desc);
    g_white_id = (ImTextureID)(uintptr_t)g_white_view;

    memcpy(row, pixel, sizeof(pixel));
    dst.texture = g_white;
    dst.aspect = WGPUTextureAspect_All;
    layout.bytesPerRow = 256;
    layout.rowsPerImage = 1;
    wgpuQueueWriteTexture(g_queue, &dst, row, sizeof(row), &layout, &size);
    return g_white_view != NULL;
}

static void destroy_target(void)
{
    if (g_readback) {
        wgpuBufferRelease(g_readback);
        g_readback = NULL;
    }
    if (g_target_view) {
        wgpuTextureViewRelease(g_target_view);
        g_target_view = NULL;
    }
    if (g_target) {
        wgpuTextureRelease(g_target);
        g_target = NULL;
    }
    g_target_width = 0;
    g_target_height = 0;
    g_target_stride = 0;
}

static bool ensure_target(int width, int height)
{
    uint32_t stride = align_row((uint32_t)width * 4u);
    size_t bytes = (size_t)stride * (size_t)height;

    if (g_target && g_target_width == width && g_target_height == height) {
        return true;
    }
    destroy_target();

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;

    desc.label = wgpu_label("ImGui target");
    desc.usage =
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = { (uint32_t)width, (uint32_t)height, 1 };
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;
    g_target = wgpuDeviceCreateTexture(g_device, &desc);
    if (!g_target) {
        return false;
    }

    view_desc.format = WGPUTextureFormat_RGBA8Unorm;
    view_desc.dimension = WGPUTextureViewDimension_2D;
    view_desc.mipLevelCount = 1;
    view_desc.arrayLayerCount = 1;
    view_desc.aspect = WGPUTextureAspect_All;
    g_target_view = wgpuTextureCreateView(g_target, &view_desc);

    buffer_desc.label = wgpu_label("ImGui readback");
    buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    buffer_desc.size = bytes;
    g_readback = wgpuDeviceCreateBuffer(g_device, &buffer_desc);
    if (!g_target_view || !g_readback) {
        destroy_target();
        return false;
    }
    g_target_width = width;
    g_target_height = height;
    g_target_stride = stride;
    return true;
}

static bool copy_gl_texture(GLuint name, int width, int height,
                             WGPUTexture texture)
{
    uint32_t stride = align_row((uint32_t)width * 4u);
    std::vector<uint8_t> tight((size_t)width * (size_t)height * 4u);
    std::vector<uint8_t> pixels((size_t)stride * (size_t)height);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    WGPUExtent3D size = { (uint32_t)width, (uint32_t)height, 1 };

    glBindTexture(GL_TEXTURE_2D, name);
    while (glGetError() != GL_NO_ERROR) {
    }
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, tight.data());
    if (glGetError() != GL_NO_ERROR) {
        return false;
    }
    for (int y = 0; y < height; y++) {
        memcpy(pixels.data() + (size_t)y * stride,
               tight.data() + (size_t)y * (size_t)width * 4u, (size_t)width * 4u);
    }
    dst.texture = texture;
    dst.aspect = WGPUTextureAspect_All;
    layout.bytesPerRow = stride;
    layout.rowsPerImage = height;
    wgpuQueueWriteTexture(g_queue, &dst, pixels.data(), pixels.size(), &layout,
                          &size);
    return true;
}

static ImTextureID upload_gl_texture(GLuint name)
{
    for (size_t i = 0; i < g_gl_images.size(); i++) {
        GlImage &image = g_gl_images[i];
        GLint width = 0;
        GLint height = 0;

        if (image.name != name) {
            continue;
        }
        if (image.used) {
            return (ImTextureID)(uintptr_t)image.view;
        }
        glBindTexture(GL_TEXTURE_2D, name);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        if (width == image.width && height == image.height &&
            copy_gl_texture(name, width, height, image.texture)) {
            image.used = true;
            return (ImTextureID)(uintptr_t)image.view;
        }
        wgpuTextureViewRelease(image.view);
        wgpuTextureRelease(image.texture);
        g_gl_images.erase(g_gl_images.begin() + i);
        break;
    }

    GLint width = 0;
    GLint height = 0;
    glBindTexture(GL_TEXTURE_2D, name);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    if (width <= 0 || height <= 0) {
        return g_white_id;
    }

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    WGPUExtent3D size = { (uint32_t)width, (uint32_t)height, 1 };

    desc.label = wgpu_label("ImGui GL image");
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = size;
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.mipLevelCount = 1;
    desc.sampleCount = 1;

    GlImage image = {};
    image.name = name;
    image.width = width;
    image.height = height;
    image.used = true;
    image.texture = wgpuDeviceCreateTexture(g_device, &desc);
    if (!image.texture) {
        return g_white_id;
    }
    view_desc.format = WGPUTextureFormat_RGBA8Unorm;
    view_desc.dimension = WGPUTextureViewDimension_2D;
    view_desc.mipLevelCount = 1;
    view_desc.arrayLayerCount = 1;
    view_desc.aspect = WGPUTextureAspect_All;
    image.view = wgpuTextureCreateView(image.texture, &view_desc);
    if (!image.view || !copy_gl_texture(name, width, height, image.texture)) {
        if (image.view) {
            wgpuTextureViewRelease(image.view);
        }
        wgpuTextureRelease(image.texture);
        return g_white_id;
    }
    ImTextureID id = (ImTextureID)(uintptr_t)image.view;
    g_gl_images.push_back(image);
    return id;
}

static void bind_draw_textures(ImDrawData *draw_data)
{
    ImTextureID font = ImGui::GetIO().Fonts->TexID;

    for (GlImage &image : g_gl_images) {
        image.used = false;
    }
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        ImDrawList *draw_list = draw_data->CmdLists[n];
        for (int i = 0; i < draw_list->CmdBuffer.Size; i++) {
            ImDrawCmd *cmd = &draw_list->CmdBuffer[i];
            if (cmd->UserCallback) {
                continue;
            }
            if (font && cmd->TextureId == font) {
                continue;
            }
            if (cmd->TextureId == 0) {
                cmd->TextureId = g_white_id;
                continue;
            }
            if (is_ui_texture(cmd->TextureId)) {
                continue;
            }
            cmd->TextureId = upload_gl_texture((GLuint)cmd->TextureId);
        }
    }

    std::vector<GlImage> kept;
    for (GlImage &image : g_gl_images) {
        if (image.used) {
            kept.push_back(image);
            continue;
        }
        /* Drop bind groups that still name this view on the next font rebuild.
         * Release the texture now so a recycled GL name cannot sample it. */
        wgpuTextureViewRelease(image.view);
        wgpuTextureRelease(image.texture);
    }
    g_gl_images.swap(kept);
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
        fprintf(stderr, "[WebGPU] UI readback failed (%d): %.*s\n", status,
                (int)message.length, message.data ? message.data : "");
    }
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

static bool is_ui_texture(ImTextureID id)
{
    uint64_t key = (uint64_t)id;

    for (uint64_t stored : g_ui_textures) {
        if (stored == key) {
            return true;
        }
    }
    return false;
}

static void draw_overlay(const void *pixels, int width, int height,
                         uint32_t stride, bool blend_over)
{
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean cull = glIsEnabled(GL_CULL_FACE);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint blend_src = GL_SRC_ALPHA;
    GLint blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    GLint unpack_row = 0;

    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);

    glBindTexture(GL_TEXTURE_2D, g_overlay_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(stride / 4u));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    if (blend_over) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    glViewport(0, 0, width, height);
    glUseProgram(g_overlay_program);
    glBindVertexArray(g_overlay_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);

    glBlendFunc(blend_src, blend_dst);
    if (blend) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (depth) {
        glEnable(GL_DEPTH_TEST);
    }
    if (cull) {
        glEnable(GL_CULL_FACE);
    }
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
}

bool xemu_wgpu_ui_active(void)
{
    return g_active;
}

bool xemu_wgpu_ui_init(SDL_Window *window)
{
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    WGPUInstanceFeatureName features[] = {
        WGPUInstanceFeatureName_TimedWaitAny,
    };
    WGPURequestAdapterOptions adapter_options =
        WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPURequestAdapterCallbackInfo adapter_cb =
        WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPURequestDeviceCallbackInfo device_cb =
        WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    AdapterRequest adapter_request = {};
    DeviceRequest device_request = {};

    (void)window;
    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = features;
    g_instance = wgpuCreateInstance(&instance_desc);
    if (!g_instance) {
        fprintf(stderr, "[WebGPU] UI instance creation failed\n");
        return false;
    }

    adapter_options.powerPreference = WGPUPowerPreference_HighPerformance;
    adapter_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    adapter_cb.callback = request_adapter_callback;
    adapter_cb.userdata1 = &adapter_request;
    if (!wait_future(wgpuInstanceRequestAdapter(g_instance, &adapter_options,
                                                adapter_cb)) ||
        adapter_request.status != WGPURequestAdapterStatus_Success) {
        fprintf(stderr, "[WebGPU] UI adapter request failed: %s\n",
                adapter_request.message);
        xemu_wgpu_ui_shutdown();
        return false;
    }
    g_adapter = adapter_request.adapter;

    device_desc.uncapturedErrorCallbackInfo.callback =
        uncaptured_error_callback;
    device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    device_desc.deviceLostCallbackInfo.callback = device_lost_callback;
    device_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    device_cb.callback = request_device_callback;
    device_cb.userdata1 = &device_request;
    if (!wait_future(
            wgpuAdapterRequestDevice(g_adapter, &device_desc,
                                              device_cb)) ||
        device_request.status != WGPURequestDeviceStatus_Success) {
        fprintf(stderr, "[WebGPU] UI device request failed: %s\n",
                device_request.message);
        xemu_wgpu_ui_shutdown();
        return false;
    }
    g_device = device_request.device;
    g_queue = wgpuDeviceGetQueue(g_device);

    ImGui_ImplWGPU_InitInfo imgui_info;
    imgui_info.Device = g_device;
    imgui_info.NumFramesInFlight = 1;
    imgui_info.RenderTargetFormat = WGPUTextureFormat_RGBA8Unorm;
    imgui_info.DepthStencilFormat = WGPUTextureFormat_Undefined;
    if (!ImGui_ImplWGPU_Init(&imgui_info) || !create_white_texture() ||
        !create_overlay_gl()) {
        fprintf(stderr, "[WebGPU] UI ImGui backend init failed\n");
        xemu_wgpu_ui_shutdown();
        return false;
    }

    store_renderer(g_adapter);
    fprintf(stderr, "[WebGPU] UI device: %s (%s)\n", g_adapter_text,
            g_driver_text);
    g_active = true;
    return true;
}

void xemu_wgpu_ui_shutdown(void)
{
    g_active = false;
    g_scene_drawn = false;
    g_ui_textures.clear();
    if (ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendRendererUserData) {
        ImGui_ImplWGPU_Shutdown();
    }
    release_gl_images();
    destroy_target();
    if (g_white_view) {
        wgpuTextureViewRelease(g_white_view);
        g_white_view = NULL;
    }
    if (g_white) {
        wgpuTextureRelease(g_white);
        g_white = NULL;
    }
    g_white_id = 0;
    destroy_overlay_gl();
    if (g_queue) {
        wgpuQueueRelease(g_queue);
        g_queue = NULL;
    }
    if (g_device) {
        wgpuDeviceRelease(g_device);
        g_device = NULL;
    }
    if (g_adapter) {
        wgpuAdapterRelease(g_adapter);
        g_adapter = NULL;
    }
    if (g_instance) {
        wgpuInstanceRelease(g_instance);
        g_instance = NULL;
    }
}

void xemu_wgpu_ui_new_frame(void)
{
    ImGui_ImplWGPU_NewFrame();
}

void xemu_wgpu_ui_rebuild_fonts(void)
{
    if (g_active) {
        ImGui_ImplWGPU_CreateDeviceObjects();
    }
}

WGPUDevice xemu_wgpu_ui_device(void)
{
    return g_device;
}

WGPUQueue xemu_wgpu_ui_queue(void)
{
    return g_queue;
}

WGPUInstance xemu_wgpu_ui_instance(void)
{
    return g_instance;
}

WGPUTextureView xemu_wgpu_ui_prepare_target(int width, int height)
{
    if (!g_active || width <= 0 || height <= 0 ||
        !ensure_target(width, height)) {
        return NULL;
    }
    return g_target_view;
}

void xemu_wgpu_ui_mark_scene(void)
{
    g_scene_drawn = true;
}

void xemu_wgpu_ui_register_texture(uint64_t id)
{
    if (id) {
        g_ui_textures.push_back(id);
    }
}

bool xemu_wgpu_ui_renderer(const char **vendor, const char **adapter,
                           const char **driver, const char **shader)
{
    if (!g_active) {
        return false;
    }
    if (vendor) {
        *vendor = g_vendor_text;
    }
    if (adapter) {
        *adapter = g_adapter_text;
    }
    if (driver) {
        *driver = g_driver_text;
    }
    if (shader) {
        *shader = "WGSL";
    }
    return true;
}

void xemu_wgpu_ui_unregister_texture(uint64_t id)
{
    for (size_t i = 0; i < g_ui_textures.size(); i++) {
        if (g_ui_textures[i] == id) {
            g_ui_textures.erase(g_ui_textures.begin() + i);
            return;
        }
    }
}

void xemu_wgpu_ui_render(void)
{
    ImDrawData *draw_data = ImGui::GetDrawData();
    int width;
    int height;
    bool scene = g_scene_drawn;
    bool same_target;

    g_scene_drawn = false;

    if (!g_active || !draw_data) {
        return;
    }
    width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    same_target =
        g_target && g_target_width == width && g_target_height == height;
    if (width <= 0 || height <= 0 || !ensure_target(width, height)) {
        return;
    }
    if (!same_target) {
        scene = false;
    }

    wgpuDevicePushErrorScope(g_device, WGPUErrorFilter_Validation);
    bind_draw_textures(draw_data);

    WGPUCommandEncoderDescriptor encoder_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    encoder_desc.label = wgpu_label("ImGui");
    WGPUCommandEncoder encoder =
        wgpuDeviceCreateCommandEncoder(g_device, &encoder_desc);

    color.view = g_target_view;
    color.loadOp = scene ? WGPULoadOp_Load : WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{ 0, 0, 0, 0 };
    pass_desc.label = wgpu_label("ImGui");
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;

    WGPURenderPassEncoder pass =
        wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    ImGui_ImplWGPU_RenderDrawData(draw_data, pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUExtent3D extent = { (uint32_t)width, (uint32_t)height, 1 };
    src.texture = g_target;
    src.aspect = WGPUTextureAspect_All;
    dst.buffer = g_readback;
    dst.layout.bytesPerRow = g_target_stride;
    dst.layout.rowsPerImage = height;
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);

    WGPUCommandBufferDescriptor command_desc =
        WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, &command_desc);
    wgpuCommandEncoderRelease(encoder);
    wgpuQueueSubmit(g_queue, 1, &commands);
    wgpuCommandBufferRelease(commands);

    MapState map_state = {};
    WGPUBufferMapCallbackInfo map_cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    size_t bytes = (size_t)g_target_stride * (size_t)height;
    map_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    map_cb.callback = buffer_map_callback;
    map_cb.userdata1 = &map_state;
    if (wait_future(wgpuBufferMapAsync(g_readback, WGPUMapMode_Read, 0, bytes,
                                       map_cb)) &&
        map_state.status == WGPUMapAsyncStatus_Success) {
        const void *pixels =
            wgpuBufferGetConstMappedRange(g_readback, 0, bytes);
        if (pixels) {
            draw_overlay(pixels, width, height, g_target_stride, !scene);
        }
        wgpuBufferUnmap(g_readback);
    }

    ErrorState error_state = {};
    WGPUPopErrorScopeCallbackInfo pop_cb =
        WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    pop_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    pop_cb.callback = pop_error_callback;
    pop_cb.userdata1 = &error_state;
    if (wait_future(wgpuDevicePopErrorScope(g_device, pop_cb)) &&
        error_state.type != WGPUErrorType_NoError) {
        fprintf(stderr, "[WebGPU] UI validation: %s\n", error_state.message);
    }
}
