/*
 * Browser present check for phase 5 of docs/wasm/webgpu-renderer-plan.md.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * main() stands in for xemu under -sPROXY_TO_PTHREAD. It transfers
 * #canvas to one pthread, the stand-in for the PFIFO thread. That
 * thread uses hw/xbox/nv2a/pgraph/webgpu/canvas.c to bind the canvas,
 * samples a stand-in PCRTC texture into the current texture, then
 * draws a UI quad on top. wgpuSurfacePresent is not called.
 * The report also times eight write-then-map waits. Link with -sJSPI=1
 * instead of -sASYNCIFY=1 to time JavaScript Promise Integration.
 *
 * Build (emsdk 4.0.10 or newer):
 *
 *   em++ tests/wasm/webgpu-canvas-present/main.cc \
 *     hw/xbox/nv2a/pgraph/webgpu/canvas.c \
 *     -o build/wasm/webgpu-canvas-present/index.html \
 *     --shell-file tests/wasm/webgpu-canvas-present/shell.html \
 *     --use-port=emdawnwebgpu:cpp_bindings=false \
 *     -pthread -sPROXY_TO_PTHREAD=1 -sOFFSCREENCANVAS_SUPPORT=1 \
 *     -sASYNCIFY=1 -sPTHREAD_POOL_SIZE=2 \
 *     -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='$GL' \
 *     -sEXIT_RUNTIME=0 \
 *     -I hw/xbox/nv2a/pgraph/webgpu
 *
 * Serve with COOP: same-origin and COEP: require-corp (emrun does this).
 */

#include "canvas.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <webgpu/webgpu.h>

#define CANVAS_W 640
#define CANVAS_H 480
/* Green scanout, red UI corner. The page background is dark red. */
#define SCAN_R 0
#define SCAN_G 255
#define SCAN_B 0
#define UI_R 255
#define UI_G 0
#define UI_B 0

static const char k_composite_wgsl[] =
    "@group(0) @binding(0) var frame_tex: texture_2d<f32>;\n"
    "@group(0) @binding(1) var frame_samp: sampler;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) "
    "vec4f {\n"
    "    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    let size = vec2f(textureDimensions(frame_tex));\n"
    "    return textureSample(frame_tex, frame_samp, pos.xy / "
    "vec2f(640.0, 480.0));\n"
    "}\n";

static const char k_ui_wgsl[] =
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) "
    "vec4f {\n"
    "    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4f {\n"
    "    return vec4f(1.0, 0.0, 0.0, 1.0);\n"
    "}\n";

static char g_report[4096];
static int g_ok;
static int g_suspends;
static double g_suspend_ms;

static void report_append(const char *text)
{
    size_t used = strlen(g_report);
    size_t room = sizeof(g_report) - used;

    if (room > 1) {
        snprintf(g_report + used, room, "%s", text);
    }
}

static void report_fmt(const char *fmt, int value)
{
    char line[128];

    snprintf(line, sizeof(line), fmt, value);
    report_append(line);
}

static WGPUStringView sv(const char *text)
{
    WGPUStringView view = WGPU_STRING_VIEW_INIT;

    view.data = text;
    view.length = WGPU_STRLEN;
    return view;
}

typedef struct WaitState {
    int status;
    void *object;
    char message[256];
} WaitState;

static void copy_message(char *dst, size_t dst_size, WGPUStringView message)
{
    size_t length = 0;

    if (!message.data || dst_size == 0) {
        if (dst_size) {
            dst[0] = '\0';
        }
        return;
    }
    length = message.length == WGPU_STRLEN ? strlen(message.data) :
                                            message.length;
    if (length >= dst_size) {
        length = dst_size - 1;
    }
    memcpy(dst, message.data, length);
    dst[length] = '\0';
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView message, void *userdata1, void *userdata2)
{
    WaitState *state = (WaitState *)userdata1;

    (void)userdata2;
    state->status = status;
    state->object = adapter;
    copy_message(state->message, sizeof(state->message), message);
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView message, void *userdata1, void *userdata2)
{
    WaitState *state = (WaitState *)userdata1;

    (void)userdata2;
    state->status = status;
    state->object = device;
    copy_message(state->message, sizeof(state->message), message);
}

static void on_map(WGPUMapAsyncStatus status, WGPUStringView message,
                   void *userdata1, void *userdata2)
{
    WaitState *state = (WaitState *)userdata1;

    (void)userdata2;
    state->status = status;
    copy_message(state->message, sizeof(state->message), message);
}

static int wait_future(WGPUInstance instance, WGPUFuture future,
                       const char *what)
{
    WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
    WGPUWaitStatus status;
    double start_ms;

    info.future = future;
    status = wgpuInstanceWaitAny(instance, 1, &info, 0);
    if (status == WGPUWaitStatus_Success && info.completed) {
        return 1;
    }
    start_ms = emscripten_get_now();
    info.completed = false;
    status = wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX);
    g_suspend_ms += emscripten_get_now() - start_ms;
    g_suspends++;
    if (status != WGPUWaitStatus_Success || !info.completed) {
        report_append(what);
        report_fmt(" wait failed (%d)\n", status);
        return 0;
    }
    return 1;
}

static WGPUShaderModule make_module(WGPUDevice device, const char *code)
{
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;

    source.code = sv(code);
    desc.nextInChain = &source.chain;
    return wgpuDeviceCreateShaderModule(device, &desc);
}

static WGPURenderPipeline make_pipeline(WGPUDevice device, WGPUShaderModule module,
                                        WGPUTextureFormat format,
                                        WGPUPipelineLayout layout)
{
    WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;

    color.format = format;
    color.writeMask = WGPUColorWriteMask_All;
    fragment.module = module;
    fragment.entryPoint = sv("fs");
    fragment.targetCount = 1;
    fragment.targets = &color;
    desc.layout = layout;
    desc.vertex.module = module;
    desc.vertex.entryPoint = sv("vs");
    desc.fragment = &fragment;
    return wgpuDeviceCreateRenderPipeline(device, &desc);
}

static int map_pixel(WGPUInstance instance, WGPUBuffer buffer,
                     WGPUTextureFormat format, int *r, int *g, int *b, int *a)
{
    WGPUBufferMapCallbackInfo map_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    WaitState mapped = { 0 };
    const uint8_t *pixels;

    map_info.mode = WGPUCallbackMode_WaitAnyOnly;
    map_info.callback = on_map;
    map_info.userdata1 = &mapped;
    if (!wait_future(instance,
                     wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, 256,
                                        map_info),
                     "map")) {
        return 0;
    }
    if (mapped.status != WGPUMapAsyncStatus_Success) {
        report_append("map failed: ");
        report_append(mapped.message);
        report_append("\n");
        return 0;
    }
    pixels = (const uint8_t *)wgpuBufferGetConstMappedRange(buffer, 0, 256);
    if (!pixels) {
        report_append("mapped range is null\n");
        return 0;
    }
    if (format == WGPUTextureFormat_BGRA8Unorm) {
        *b = pixels[0];
        *g = pixels[1];
        *r = pixels[2];
        *a = pixels[3];
    } else {
        *r = pixels[0];
        *g = pixels[1];
        *b = pixels[2];
        *a = pixels[3];
    }
    wgpuBufferUnmap(buffer);
    return 1;
}

static int present_frame(WGPUInstance instance, WGPUAdapter adapter,
                         WGPUDevice device, WGPUQueue queue)
{
    WgpuCanvas canvas;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
    WGPUTextureView canvas_view = NULL;
    WGPUTextureDescriptor tex_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUTexture scanout;
    WGPUTextureView scanout_view;
    WGPUSamplerDescriptor samp_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    WGPUSampler sampler;
    WGPUBindGroupLayoutEntry entries[2];
    WGPUBindGroupLayoutDescriptor bgl_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    WGPUBindGroupLayout bgl;
    WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUPipelineLayout composite_layout;
    WGPUShaderModule composite_module;
    WGPUShaderModule ui_module;
    WGPURenderPipeline composite_pipe;
    WGPURenderPipeline ui_pipe;
    WGPUBindGroupEntry bind_entries[2];
    WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    WGPUBindGroup bind;
    WGPUCommandEncoderDescriptor encoder_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPUCommandEncoder encoder;
    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    WGPURenderPassEncoder render;
    WGPUCommandBufferDescriptor buffer_desc =
        WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandBuffer commands;
    WGPUTexelCopyTextureInfo dst_tex = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    WGPUExtent3D extent = { 4, 4, 1 };
    uint8_t pixels[256 * 4];
    WGPUBuffer center_buf;
    WGPUBuffer corner_buf;
    int cr, cg, cb, ca;
    int ur, ug, ub, ua;
    int in_worker;

    in_worker = EM_ASM_INT({ return typeof document === 'undefined' ? 1 : 0; });
    report_fmt("worker=%d\n", in_worker);
    if (!in_worker) {
        report_append("device thread is the page thread\n");
        return 0;
    }

    wgpu_canvas_init(&canvas);
    if (!wgpu_canvas_acquire(&canvas, instance, adapter, device, &format,
                             &width, &height, &canvas_view)) {
        report_append("canvas acquire failed\n");
        return 0;
    }
    report_fmt("canvas %d", (int)width);
    report_fmt("x%d ", (int)height);
    if (format == WGPUTextureFormat_BGRA8Unorm) {
        report_append("format=bgra8unorm\n");
    } else if (format == WGPUTextureFormat_RGBA8Unorm) {
        report_append("format=rgba8unorm\n");
    } else {
        report_fmt("format=%d\n", format);
    }
    if (width != CANVAS_W || height != CANVAS_H) {
        report_append("unexpected canvas size\n");
        wgpu_canvas_fini(&canvas);
        return 0;
    }

    tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
    tex_desc.size = extent;
    scanout = wgpuDeviceCreateTexture(device, &tex_desc);
    scanout_view = wgpuTextureCreateView(scanout, NULL);
    memset(pixels, 0, sizeof(pixels));
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint8_t *p = pixels + y * 256 + x * 4;
            p[0] = SCAN_R;
            p[1] = SCAN_G;
            p[2] = SCAN_B;
            p[3] = 255;
        }
    }
    dst_tex.texture = scanout;
    layout.bytesPerRow = 256;
    layout.rowsPerImage = 4;
    wgpuQueueWriteTexture(queue, &dst_tex, pixels, sizeof(pixels), &layout,
                          &extent);

    samp_desc.magFilter = WGPUFilterMode_Nearest;
    samp_desc.minFilter = WGPUFilterMode_Nearest;
    sampler = wgpuDeviceCreateSampler(device, &samp_desc);

    entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    bgl_desc.entryCount = 2;
    bgl_desc.entries = entries;
    bgl = wgpuDeviceCreateBindGroupLayout(device, &bgl_desc);
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts = &bgl;
    composite_layout = wgpuDeviceCreatePipelineLayout(device, &pl_desc);

    composite_module = make_module(device, k_composite_wgsl);
    ui_module = make_module(device, k_ui_wgsl);
    composite_pipe =
        make_pipeline(device, composite_module, format, composite_layout);
    ui_pipe = make_pipeline(device, ui_module, format, NULL);
    bind_entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    bind_entries[0].binding = 0;
    bind_entries[0].textureView = scanout_view;
    bind_entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    bind_entries[1].binding = 1;
    bind_entries[1].sampler = sampler;
    bg_desc.layout = bgl;
    bg_desc.entryCount = 2;
    bg_desc.entries = bind_entries;
    bind = wgpuDeviceCreateBindGroup(device, &bg_desc);

    encoder = wgpuDeviceCreateCommandEncoder(device, &encoder_desc);
    color.view = canvas_view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = (WGPUColor){ 0, 0, 0, 1 };
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;
    render = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
    wgpuRenderPassEncoderSetPipeline(render, composite_pipe);
    wgpuRenderPassEncoderSetBindGroup(render, 0, bind, 0, NULL);
    wgpuRenderPassEncoderDraw(render, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(render);
    wgpuRenderPassEncoderRelease(render);

    color.loadOp = WGPULoadOp_Load;
    render = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
    wgpuRenderPassEncoderSetPipeline(render, ui_pipe);
    wgpuRenderPassEncoderSetScissorRect(render, 0, 0, 64, 64);
    wgpuRenderPassEncoderDraw(render, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(render);
    wgpuRenderPassEncoderRelease(render);

    center_buf = NULL;
    corner_buf = NULL;
    {
        WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        WGPUExtent3D one = { 1, 1, 1 };

        buffer_desc.size = 256;
        buffer_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        center_buf = wgpuDeviceCreateBuffer(device, &buffer_desc);
        corner_buf = wgpuDeviceCreateBuffer(device, &buffer_desc);
        src.texture = canvas.current;
        dst.layout.bytesPerRow = 256;
        dst.layout.rowsPerImage = 1;
        src.origin.x = CANVAS_W / 2;
        src.origin.y = CANVAS_H / 2;
        dst.buffer = center_buf;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &one);
        src.origin.x = 8;
        src.origin.y = 8;
        dst.buffer = corner_buf;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &one);
    }

    commands = wgpuCommandEncoderFinish(encoder, &buffer_desc);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);

    if (!map_pixel(instance, center_buf, format, &cr, &cg, &cb, &ca) ||
        !map_pixel(instance, corner_buf, format, &ur, &ug, &ub, &ua)) {
        wgpu_canvas_fini(&canvas);
        return 0;
    }
    report_fmt("center r=%d", cr);
    report_fmt(" g=%d", cg);
    report_fmt(" b=%d", cb);
    report_fmt(" a=%d\n", ca);
    report_fmt("corner r=%d", ur);
    report_fmt(" g=%d", ug);
    report_fmt(" b=%d", ub);
    report_fmt(" a=%d\n", ua);

    /* Eight write-then-map cycles while the device is still current.
     * Each pending map suspends once. The write size is a multiple of 4. */
    {
        WGPUBufferDescriptor copy_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        WGPUBuffer copy_buf;
        int suspends_before = g_suspends;
        double ms_before = g_suspend_ms;
        uint8_t bytes[4] = { 7, 7, 7, 7 };

        copy_desc.size = 256;
        copy_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        copy_buf = wgpuDeviceCreateBuffer(device, &copy_desc);
        if (!copy_buf) {
            report_append("copy buffer create failed\n");
            wgpu_canvas_fini(&canvas);
            return 0;
        }
        for (int i = 0; i < 8; i++) {
            WGPUBufferMapCallbackInfo map_info =
                WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            WaitState mapped = { 0 };

            wgpuQueueWriteBuffer(queue, copy_buf, 0, bytes, sizeof(bytes));
            map_info.mode = WGPUCallbackMode_WaitAnyOnly;
            map_info.callback = on_map;
            map_info.userdata1 = &mapped;
            if (!wait_future(instance,
                             wgpuBufferMapAsync(copy_buf, WGPUMapMode_Read, 0,
                                                256, map_info),
                             "copy map")) {
                wgpuBufferRelease(copy_buf);
                wgpu_canvas_fini(&canvas);
                return 0;
            }
            wgpuBufferUnmap(copy_buf);
        }
        wgpuBufferRelease(copy_buf);
        report_fmt("asyncify=%d\n", emscripten_has_asyncify());
        report_fmt("copy_suspends=%d\n", g_suspends - suspends_before);
        report_fmt("copy_ms=%d\n", (int)(g_suspend_ms - ms_before + 0.5));
        report_fmt("suspends=%d\n", g_suspends);
        report_fmt("suspend_ms=%d\n", (int)(g_suspend_ms + 0.5));
    }

    wgpuBufferRelease(center_buf);
    wgpuBufferRelease(corner_buf);
    wgpuBindGroupRelease(bind);
    wgpuRenderPipelineRelease(composite_pipe);
    wgpuRenderPipelineRelease(ui_pipe);
    wgpuShaderModuleRelease(composite_module);
    wgpuShaderModuleRelease(ui_module);
    wgpuPipelineLayoutRelease(composite_layout);
    wgpuBindGroupLayoutRelease(bgl);
    wgpuSamplerRelease(sampler);
    wgpuTextureViewRelease(scanout_view);
    wgpuTextureRelease(scanout);
    /* Release the current texture. Do not call wgpuSurfacePresent. */
    wgpu_canvas_fini(&canvas);

    if (cr != SCAN_R || cg != SCAN_G || cb != SCAN_B || ca != 255) {
        report_append("center pixel is not the scanout\n");
        return 0;
    }
    if (ur != UI_R || ug != UI_G || ub != UI_B || ua != 255) {
        report_append("corner pixel is not the UI quad\n");
        return 0;
    }
    report_append("ok\n");
    return 1;
}

static int run_on_pfifo(void)
{
    WGPUInstanceFeatureName instance_features[] = {
        WGPUInstanceFeatureName_TimedWaitAny,
    };
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    WGPUInstance instance;
    WGPURequestAdapterOptions adapter_options =
        WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPURequestAdapterCallbackInfo adapter_cb =
        WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPURequestDeviceCallbackInfo device_cb =
        WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    WaitState adapter_state = { 0 };
    WaitState device_state = { 0 };
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;

    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = instance_features;
    instance = wgpuCreateInstance(&instance_desc);
    if (!instance) {
        report_append("wgpuCreateInstance failed\n");
        return 0;
    }

    adapter_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    adapter_cb.callback = on_adapter;
    adapter_cb.userdata1 = &adapter_state;
    if (!wait_future(instance,
                     wgpuInstanceRequestAdapter(instance, &adapter_options,
                                                adapter_cb),
                     "adapter")) {
        return 0;
    }
    if (adapter_state.status != WGPURequestAdapterStatus_Success ||
        !adapter_state.object) {
        report_append("adapter request failed\n");
        return 0;
    }
    adapter = (WGPUAdapter)adapter_state.object;

    device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    device_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    device_cb.callback = on_device;
    device_cb.userdata1 = &device_state;
    if (!wait_future(instance,
                     wgpuAdapterRequestDevice(adapter, &device_desc, device_cb),
                     "device")) {
        return 0;
    }
    if (device_state.status != WGPURequestDeviceStatus_Success ||
        !device_state.object) {
        report_append("device request failed: ");
        report_append(device_state.message);
        report_append("\n");
        return 0;
    }
    device = (WGPUDevice)device_state.object;
    queue = wgpuDeviceGetQueue(device);
    if (!present_frame(instance, adapter, device, queue)) {
        return 0;
    }
    return 1;
}

static void *pfifo_thread(void *arg)
{
    (void)arg;
    g_ok = run_on_pfifo();
    if (!g_ok && g_report[0] == '\0') {
        report_append("failed\n");
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_attr_t attr;
    pthread_t thread;
    int created;

    (void)argc;
    (void)argv;
    pthread_attr_init(&attr);
    if (emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas") != 0) {
        printf("settransferredcanvases failed\n");
        return 1;
    }
    created = pthread_create(&thread, &attr, pfifo_thread, NULL);
    pthread_attr_destroy(&attr);
    if (created != 0) {
        printf("pthread_create failed (%d)\n", created);
        return 1;
    }
    pthread_join(thread, NULL);
    printf("%s", g_report);
    MAIN_THREAD_EM_ASM(
        {
            var text = UTF8ToString($0);
            var result = document.getElementById('spike-result');
            if (result) {
                result.textContent = text;
            }
            window.spikeOk = $1;
            var status = document.getElementById('status');
            if (status) {
                status.textContent = $1 ? 'present ok' : 'present failed';
            }
        },
        g_report, g_ok);
    emscripten_exit_with_live_runtime();
    return g_ok ? 0 : 1;
}
