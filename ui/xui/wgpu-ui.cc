/*
 * Dear ImGui draw lists for the NV2A WebGPU device.
 *
 * The UI thread builds ImDrawData and HUD commands. It posts a copy to the
 * PFIFO thread, which records the game composite and the UI pass into one
 * image. In the browser, and on a native Dawn surface, the gamma blit and
 * ImGui are one submit into the current texture. That texture is acquired
 * after any suspending wait, because the browser expires it when the
 * worker's JavaScript task ends. Native then calls wgpuSurfacePresent.
 * Without a surface the image is read back into the OpenGL window.
 * Texture ids in the draw lists are handles into the PFIFO-side texture
 * table.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "common.hh"
#include "wgpu-helpers.hh"
#include "wgpu-ui.hh"
#include "wgpu-ui-pfifo.h"

#include <imgui_impl_wgpu.h>
#include <webgpu/webgpu.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

struct GlSlot {
    GLuint name;
    int width;
    int height;
    uint64_t id;
    bool used;
};

static bool g_active;
static bool g_backend;
static std::atomic<int> g_job;
static uint64_t g_font_handle;
static WGPUTextureView g_font_view;
static bool g_fonts_dirty;
static WGPUInstance g_instance;
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
static std::vector<GlSlot> g_gl_slots;
static ImDrawData g_copied;
static std::vector<ImDrawList *> g_copied_lists;
static std::vector<uint8_t> g_present;
static int g_present_w;
static int g_present_h;
static uint32_t g_present_stride;
static bool g_present_blend;
static bool g_present_ok;
static WGPUTextureView g_canvas_view;
static WGPUTextureFormat g_target_format = WGPUTextureFormat_RGBA8Unorm;
static WGPUTextureFormat g_imgui_format = WGPUTextureFormat_Undefined;
static std::atomic<bool> g_canvas_presented;
static std::mutex g_submit_lock;
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

static bool wait_future(WGPUFuture future)
{
    return xemu_wgpu_wait(g_instance, future);
}

static uint32_t align_row(uint32_t row_bytes)
{
    return (row_bytes + 255u) & ~255u;
}

static void release_gl_images(void)
{
    for (GlSlot &slot : g_gl_slots) {
        if (slot.id) {
            wgpu_helpers_release_image(slot.id);
        }
    }
    g_gl_slots.clear();
}

static bool create_overlay_gl(void)
{
    static const char *vs_src = "#version 150 core\n"
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
    GLuint vs;
    GLuint fs;
    GLint ok = 0;
    char log[512] = { 0 };
    GLsizei len = 0;

#ifdef __EMSCRIPTEN__
    /* The browser presents into the canvas. There is no GL context to
     * compile an overlay into, and epoxy would dlopen libEGL.so.1. */
    return true;
#else
    /* A native Dawn surface presents the window. There is no GL context. */
    if (!SDL_GL_GetCurrentContext()) {
        return true;
    }
#endif
    vs = glCreateShader(GL_VERTEX_SHADER);
    fs = glCreateShader(GL_FRAGMENT_SHADER);
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
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
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

static bool read_gl_rgba(GLuint name, int width, int height,
                         std::vector<uint8_t> &tight)
{
    tight.resize((size_t)width * (size_t)height * 4u);
    glBindTexture(GL_TEXTURE_2D, name);
    while (glGetError() != GL_NO_ERROR) {
    }
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, tight.data());
    return glGetError() == GL_NO_ERROR;
}

static ImTextureID gl_name_to_handle(GLuint name)
{
    GLint width = 0;
    GLint height = 0;
    std::vector<uint8_t> tight;
    uint64_t id;

    glBindTexture(GL_TEXTURE_2D, name);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    if (width <= 0 || height <= 0 ||
        !read_gl_rgba(name, width, height, tight)) {
        return 0;
    }
    for (size_t i = 0; i < g_gl_slots.size(); i++) {
        GlSlot &slot = g_gl_slots[i];

        if (slot.name != name) {
            continue;
        }
        if (slot.width == width && slot.height == height &&
            wgpu_helpers_update_image(slot.id, tight.data(), width, height)) {
            slot.used = true;
            return (ImTextureID)slot.id;
        }
        wgpu_helpers_release_image(slot.id);
        g_gl_slots.erase(g_gl_slots.begin() + i);
        break;
    }
    id = wgpu_helpers_create_image(tight.data(), width, height);
    if (!id) {
        return 0;
    }
    g_gl_slots.push_back(GlSlot{ name, width, height, id, true });
    return (ImTextureID)id;
}

static void rewrite_gl_textures(ImDrawData *draw_data)
{
    std::vector<GlSlot> kept;

    for (GlSlot &slot : g_gl_slots) {
        slot.used = false;
    }
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        ImDrawList *draw_list = draw_data->CmdLists[n];
        for (int i = 0; i < draw_list->CmdBuffer.Size; i++) {
            ImDrawCmd *cmd = &draw_list->CmdBuffer[i];

            if (cmd->UserCallback || cmd->TextureId == 0 ||
                is_ui_texture(cmd->TextureId)) {
                continue;
            }
            cmd->TextureId = gl_name_to_handle((GLuint)cmd->TextureId);
        }
    }
    for (GlSlot &slot : g_gl_slots) {
        if (slot.used) {
            kept.push_back(slot);
            continue;
        }
        wgpu_helpers_release_image(slot.id);
    }
    g_gl_slots.swap(kept);
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
    (void)window;
    if (!create_overlay_gl()) {
        fprintf(stderr, "[WebGPU] UI overlay init failed\n");
        return false;
    }
    g_active = true;
    fprintf(stderr, "[WebGPU] UI draw lists are posted to the PFIFO device\n");
    return true;
}

static void release_ui_gpu(void)
{
    if (g_backend && ImGui::GetCurrentContext() &&
        ImGui::GetIO().BackendRendererUserData) {
        ImGui_ImplWGPU_Shutdown();
    }
    g_backend = false;
    g_font_view = NULL;
    g_font_handle = 0;
    g_fonts_dirty = false;
    release_gl_images();
    wgpu_helpers_gpu_shutdown();
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
}

void xemu_wgpu_ui_shutdown(void)
{
    g_active = false;
    g_scene_drawn = false;
    g_ui_textures.clear();
    if (g_backend) {
        xemu_wgpu_ui_submit(XEMU_WGPU_UI_JOB_SHUTDOWN);
    }
    destroy_overlay_gl();
    g_queue = NULL;
    g_device = NULL;
    g_instance = NULL;
}

void xemu_wgpu_ui_new_frame(void)
{
    if (!g_active || g_backend) {
        return;
    }
    xemu_wgpu_ui_submit(XEMU_WGPU_UI_JOB_INIT);
}

void xemu_wgpu_ui_rebuild_fonts(void)
{
    if (!g_active) {
        return;
    }
    g_fonts_dirty = true;
    if (g_backend) {
        xemu_wgpu_ui_submit(XEMU_WGPU_UI_JOB_FONTS);
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
    if (!g_active || width <= 0 || height <= 0) {
        return NULL;
    }
    if (g_canvas_view) {
        return g_canvas_view;
    }
    if (!ensure_target(width, height)) {
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
    nv2a_wgpu_copy_adapter(g_vendor_text, sizeof(g_vendor_text), g_adapter_text,
                           sizeof(g_adapter_text), g_driver_text,
                           sizeof(g_driver_text));
    if (!g_vendor_text[0]) {
        snprintf(g_vendor_text, sizeof(g_vendor_text), "Dawn");
    }
    if (!g_adapter_text[0]) {
        snprintf(g_adapter_text, sizeof(g_adapter_text), "WebGPU");
    }
    if (!g_driver_text[0]) {
        snprintf(g_driver_text, sizeof(g_driver_text), "WebGPU");
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

/* Above any GL texture name, and outside the helper handle allocator. */
static const uint64_t kFontHandle = 0xF000000000000001ull;

static void free_copied_draw(void)
{
    for (ImDrawList *list : g_copied_lists) {
        IM_DELETE(list);
    }
    g_copied_lists.clear();
    g_copied.Clear();
}

static void copy_draw_data(const ImDrawData *src)
{
    free_copied_draw();
    g_copied.Valid = src->Valid;
    g_copied.TotalIdxCount = src->TotalIdxCount;
    g_copied.TotalVtxCount = src->TotalVtxCount;
    g_copied.DisplayPos = src->DisplayPos;
    g_copied.DisplaySize = src->DisplaySize;
    g_copied.FramebufferScale = src->FramebufferScale;
    g_copied.OwnerViewport = src->OwnerViewport;
    for (int n = 0; n < src->CmdListsCount; n++) {
        const ImDrawList *from = src->CmdLists[n];
        ImDrawList *list = IM_NEW(ImDrawList)(from->_Data);

        list->CmdBuffer = from->CmdBuffer;
        list->IdxBuffer = from->IdxBuffer;
        list->VtxBuffer = from->VtxBuffer;
        list->Flags = from->Flags;
        g_copied_lists.push_back(list);
        g_copied.CmdLists.push_back(list);
    }
    g_copied.CmdListsCount = g_copied.CmdLists.Size;
    rewrite_gl_textures(&g_copied);
}

static void adopt_font(void)
{
    ImFontAtlas *fonts = ImGui::GetIO().Fonts;
    WGPUTextureView view = (WGPUTextureView)(uintptr_t)fonts->TexID;

    if (!view || (uint64_t)(uintptr_t)view == kFontHandle) {
        return;
    }
    g_font_view = view;
    if (!g_font_handle) {
        g_font_handle = kFontHandle;
        xemu_wgpu_ui_register_texture(g_font_handle);
    }
    fonts->SetTexID((ImTextureID)g_font_handle);
}

static bool init_imgui(void)
{
    ImGui_ImplWGPU_InitInfo info = {};

    info.Device = g_device;
    info.NumFramesInFlight = 1;
    info.RenderTargetFormat = g_target_format;
    info.DepthStencilFormat = WGPUTextureFormat_Undefined;
    if (!ImGui_ImplWGPU_Init(&info)) {
        fprintf(stderr, "[WebGPU] UI ImGui backend init failed\n");
        return false;
    }
    ImGui_ImplWGPU_NewFrame();
    adopt_font();
    g_imgui_format = g_target_format;
    return true;
}

static bool init_backend(void)
{
    if (!init_imgui()) {
        return false;
    }
    if (!create_white_texture() || !wgpu_helpers_gpu_init()) {
        fprintf(stderr, "[WebGPU] UI device objects init failed\n");
        if (g_white_view) {
            wgpuTextureViewRelease(g_white_view);
            g_white_view = NULL;
        }
        if (g_white) {
            wgpuTextureRelease(g_white);
            g_white = NULL;
        }
        g_white_id = 0;
        ImGui_ImplWGPU_Shutdown();
        g_imgui_format = WGPUTextureFormat_Undefined;
        return false;
    }
    g_fonts_dirty = false;
    g_backend = true;
    return true;
}

static bool ensure_backend(void)
{
    if (!g_backend) {
        return init_backend();
    }
    if (g_imgui_format == g_target_format) {
        return true;
    }
    ImGui_ImplWGPU_Shutdown();
    if (!init_imgui()) {
        g_backend = false;
        return false;
    }
    return true;
}

static WGPUTextureView resolve_draw_view(ImTextureID id)
{
    WGPUTextureView view = NULL;

    if (id == 0) {
        return g_white_view;
    }
    if ((uint64_t)id == g_font_handle) {
        return g_font_view ? g_font_view : g_white_view;
    }
    view = wgpu_helpers_resolve((uint64_t)id);
    return view ? view : g_white_view;
}

static void translate_draw_textures(ImDrawData *draw_data)
{
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        ImDrawList *draw_list = draw_data->CmdLists[n];

        for (int i = 0; i < draw_list->CmdBuffer.Size; i++) {
            ImDrawCmd *cmd = &draw_list->CmdBuffer[i];

            if (cmd->UserCallback) {
                continue;
            }
            cmd->TextureId =
                (ImTextureID)(uintptr_t)resolve_draw_view(cmd->TextureId);
        }
    }
}

static void record_imgui(bool scene)
{
    ImDrawData *draw_data = &g_copied;
    int width;
    int height;
    bool same_target;
    size_t bytes;
    WGPUCommandEncoder encoder = NULL;

    bool to_canvas = g_canvas_view != NULL;

    g_present_ok = false;
    if (!draw_data->Valid) {
        wgpu_helpers_submit_pending();
        return;
    }
    width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    same_target = to_canvas || (g_target && g_target_width == width &&
                                g_target_height == height);
    if (width <= 0 || height <= 0) {
        wgpu_helpers_submit_pending();
        return;
    }
    if (!to_canvas && !ensure_target(width, height)) {
        wgpu_helpers_submit_pending();
        return;
    }
    if (!same_target) {
        scene = false;
    }
    translate_draw_textures(draw_data);

    WGPUCommandEncoderDescriptor encoder_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    if (to_canvas) {
        /* Gamma is already recorded here, so ImGui joins that submit. */
        encoder = wgpu_helpers_take_encoder();
    } else {
        wgpu_helpers_submit_pending();
    }
    if (!encoder) {
        encoder_desc.label = wgpu_label("ImGui");
        encoder = wgpuDeviceCreateCommandEncoder(g_device, &encoder_desc);
    }
    if (!encoder) {
        return;
    }

    /* The canvas has no pixel readback. An error-scope wait would suspend
     * the worker and expire the canvas texture. Uncaptured errors still log. */
    if (!to_canvas) {
        wgpuDevicePushErrorScope(g_device, WGPUErrorFilter_Validation);
    }

    color.view = to_canvas ? g_canvas_view : g_target_view;
    color.loadOp = scene ? WGPULoadOp_Load : WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{ 0, 0, 0, 0 };
    pass_desc.label = wgpu_label("ImGui");
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;

    WGPURenderPassEncoder pass =
        wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    if (pass) {
        ImGui_ImplWGPU_RenderDrawData(draw_data, pass);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    if (!to_canvas && encoder) {
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        WGPUExtent3D extent = { (uint32_t)width, (uint32_t)height, 1 };
        src.texture = g_target;
        src.aspect = WGPUTextureAspect_All;
        dst.buffer = g_readback;
        dst.layout.bytesPerRow = g_target_stride;
        dst.layout.rowsPerImage = height;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
    }

    WGPUCommandBufferDescriptor command_desc =
        WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer commands = NULL;
    if (encoder) {
        commands = wgpuCommandEncoderFinish(encoder, &command_desc);
        wgpuCommandEncoderRelease(encoder);
    }
    if (commands) {
        wgpuQueueSubmit(g_queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
        if (to_canvas) {
            g_canvas_presented.store(true, std::memory_order_release);
        }
    }

    if (!to_canvas) {
        MapState map_state = {};
        WGPUBufferMapCallbackInfo map_cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        bytes = (size_t)g_target_stride * (size_t)height;
        map_cb.mode = WGPUCallbackMode_WaitAnyOnly;
        map_cb.callback = buffer_map_callback;
        map_cb.userdata1 = &map_state;
        xemu_wgpu_note_download();
        if (wait_future(wgpuBufferMapAsync(g_readback, WGPUMapMode_Read, 0,
                                           bytes, map_cb)) &&
            map_state.status == WGPUMapAsyncStatus_Success) {
            const void *pixels =
                wgpuBufferGetConstMappedRange(g_readback, 0, bytes);
            if (pixels) {
                g_present.assign((const uint8_t *)pixels,
                                 (const uint8_t *)pixels + bytes);
                g_present_w = width;
                g_present_h = height;
                g_present_stride = g_target_stride;
                g_present_blend = !scene;
                g_present_ok = true;
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
            fprintf(stderr, "[WebGPU] UI validation: %s\n",
                    error_state.message);
        }
    }
}

bool xemu_wgpu_ui_submit(int job)
{
    std::lock_guard<std::mutex> lock(g_submit_lock);
    bool ok;

    g_job.store(job, std::memory_order_release);
    ok = nv2a_wgpu_submit_ui();
    g_job.store(0, std::memory_order_release);
    return ok;
}

extern "C" bool xemu_wgpu_ui_needs_display(void)
{
    return wgpu_helpers_needs_display();
}

extern "C" void xemu_wgpu_ui_execute(WGPUInstance instance, WGPUDevice device,
                                     WGPUQueue queue, WGPUTextureView display,
                                     int display_width, int display_height,
                                     const XemuWgpuCanvasOps *canvas)
{
    static bool logged;
    int job = g_job.load(std::memory_order_acquire);
    bool canvas_view = false;

    g_instance = instance;
    g_device = device;
    g_queue = queue;
    g_canvas_view = NULL;
    g_canvas_presented.store(false, std::memory_order_release);
    wgpu_helpers_set_no_suspend(false);
    if (job == XEMU_WGPU_UI_JOB_SHUTDOWN) {
        release_ui_gpu();
    } else {
        WGPUTextureFormat canvas_format = WGPUTextureFormat_Undefined;

        /* Format only. getCurrentTexture waits until the passes that can
         * suspend have finished. */
        if (canvas && canvas->describe &&
            canvas->describe(canvas->opaque, NULL, NULL, &canvas_format) &&
            canvas_format != WGPUTextureFormat_Undefined) {
            g_target_format = canvas_format;
        }
        if (!ensure_backend()) {
            /* The NV2A device is created on the PFIFO thread after UI init.
             * The next frame retries. */
        } else {
            if (!logged) {
                fprintf(stderr,
                        "[WebGPU] UI pass is recorded on the PFIFO device\n");
                logged = true;
            }
            wgpu_helpers_set_window_format(g_target_format);
            if (g_fonts_dirty) {
                ImGui_ImplWGPU_CreateDeviceObjects();
                adopt_font();
                g_fonts_dirty = false;
            }
            if (job == XEMU_WGPU_UI_JOB_FRAME) {
                bool scene;

                wgpu_helpers_replay_offscreen();
                wgpu_helpers_prepare_window();
                if (canvas && canvas->acquire) {
                    WGPUTextureView view = NULL;
                    int width = 0;
                    int height = 0;
                    WGPUTextureFormat format = WGPUTextureFormat_Undefined;

                    wgpu_helpers_set_no_suspend(true);
                    if (canvas->acquire(canvas->opaque, &view, &width, &height,
                                        &format) &&
                        view && width > 0 && height > 0 &&
                        format == g_target_format) {
                        g_canvas_view = view;
                        canvas_view = true;
                    } else {
                        wgpu_helpers_set_no_suspend(false);
                    }
                }
                scene = wgpu_helpers_replay_window(display, display_width,
                                                   display_height, canvas_view);
                record_imgui(scene);
                if (canvas_view && canvas->present &&
                    !canvas->present(canvas->opaque)) {
                    fprintf(stderr, "[WebGPU] Window present failed\n");
                }
                wgpu_helpers_set_no_suspend(false);
                wgpu_helpers_retire();
            } else if (job == XEMU_WGPU_UI_JOB_PNG) {
                wgpu_helpers_run_png(display);
            }
        }
    }
    wgpu_helpers_set_no_suspend(false);
    g_canvas_view = NULL;
    g_instance = NULL;
    g_device = NULL;
    g_queue = NULL;
}

void xemu_wgpu_ui_render(void)
{
    ImDrawData *draw_data = ImGui::GetDrawData();

    g_scene_drawn = false;
    g_present_ok = false;
    if (!g_active || !draw_data) {
        return;
    }
    copy_draw_data(draw_data);
    if (!xemu_wgpu_ui_submit(XEMU_WGPU_UI_JOB_FRAME)) {
        free_copied_draw();
        return;
    }
    if (!g_canvas_presented.load(std::memory_order_acquire) && g_present_ok &&
        SDL_GL_GetCurrentContext()) {
        draw_overlay(g_present.data(), g_present_w, g_present_h,
                     g_present_stride, g_present_blend);
    }
    free_copied_draw();
}
