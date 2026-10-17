/*
 * Browser spike for phase 5 of docs/wasm/webgpu-renderer-plan.md.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * main() stands in for xemu under -sPROXY_TO_PTHREAD (it runs on a worker).
 * It creates one more pthread, the stand-in for the PFIFO thread, and
 * transfers #canvas to that pthread. The pthread creates an emdawnwebgpu
 * device with the same WaitAny path as hw/xbox/nv2a/pgraph/webgpu/instance.c,
 * configures the OffscreenCanvas, and clears it.
 *
 * Build (emsdk 4.0.10 or newer, so --use-port=emdawnwebgpu exists):
 *
 *   em++ tests/wasm/webgpu-offscreen-spike/main.cc \
 *     -o build/wasm/webgpu-offscreen-spike/index.html \
 *     --shell-file tests/wasm/webgpu-offscreen-spike/shell.html \
 *     --use-port=emdawnwebgpu:cpp_bindings=false \
 *     -pthread -sPROXY_TO_PTHREAD=1 -sOFFSCREENCANVAS_SUPPORT=1 \
 *     -sASYNCIFY=1 -sPTHREAD_POOL_SIZE=2 \
 *     -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='$GL' \
 *     -sEXIT_RUNTIME=0
 *
 * Serve the output directory with COOP: same-origin and
 * COEP: require-corp (emrun does this). wgpuSurfacePresent aborts in
 * emdawnwebgpu; the clear is presented by submitting the canvas texture.
 */

#include <emscripten.h>
#include <emscripten/threading.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <webgpu/webgpu.h>

#define CANVAS_WIDTH 640
#define CANVAS_HEIGHT 480
/* Distinct from the page background so a failed present stays dark red. */
#define CLEAR_R 0.15
#define CLEAR_G 0.75
#define CLEAR_B 0.30

static char g_report[4096];
static int g_ok;

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

static void report_view(const char *label, WGPUStringView view)
{
    char line[256];
    size_t length = 0;

    if (!view.data) {
        snprintf(line, sizeof(line), "%s=(null)\n", label);
        report_append(line);
        return;
    }
    length = view.length == WGPU_STRLEN ? strlen(view.data) : view.length;
    if (length > 180) {
        length = 180;
    }
    snprintf(line, sizeof(line), "%s=%.*s\n", label, (int)length, view.data);
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

static void on_uncaptured(WGPUDevice const *device, WGPUErrorType type,
                          WGPUStringView message, void *userdata1,
                          void *userdata2)
{
    (void)device;
    (void)userdata1;
    (void)userdata2;
    fprintf(stderr, "uncaptured error %d: %.*s\n", type,
            message.data ? (int)(message.length == WGPU_STRLEN ?
                                     strlen(message.data) :
                                     message.length) :
                           0,
            message.data ? message.data : "");
}

static int wait_future(WGPUInstance instance, WGPUFuture future,
                       const char *what)
{
    WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
    WGPUWaitStatus status;

    info.future = future;
    status = wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX);
    if (status != WGPUWaitStatus_Success || !info.completed) {
        report_append(what);
        report_fmt(" wait failed (%d)\n", status);
        return 0;
    }
    return 1;
}

static void note_feature(WGPUAdapter adapter, const char *name,
                         WGPUFeatureName feature)
{
    report_append(name);
    report_append(wgpuAdapterHasFeature(adapter, feature) ? "=yes " : "=no ");
}

static int draw_and_read(WGPUInstance instance, WGPUDevice device,
                         WGPUQueue queue, WGPUSurface surface,
                         WGPUTextureFormat format)
{
    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
    WGPUTextureView view;
    WGPUCommandEncoderDescriptor encoder_desc =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    WGPUCommandBufferDescriptor buffer_desc =
        WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    WGPUCommandEncoder encoder;
    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    WGPURenderPassEncoder render;
    WGPUCommandBuffer commands;
    WGPUBufferDescriptor readback_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUBuffer readback;
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUExtent3D extent = WGPU_EXTENT_3D_INIT;
    WGPUBufferMapCallbackInfo map_info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    WaitState mapped = { 0 };
    const uint8_t *pixels;
    int r, g, b, a;
    int expect_r = (int)(CLEAR_R * 255.0 + 0.5);
    int expect_g = (int)(CLEAR_G * 255.0 + 0.5);
    int expect_b = (int)(CLEAR_B * 255.0 + 0.5);

    wgpuSurfaceGetCurrentTexture(surface, &current);
    if (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        current.status !=
            WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        report_fmt("getCurrentTexture status %d\n", current.status);
        return 0;
    }

    view = wgpuTextureCreateView(current.texture, NULL);
    encoder = wgpuDeviceCreateCommandEncoder(device, &encoder_desc);
    color.view = view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue.r = CLEAR_R;
    color.clearValue.g = CLEAR_G;
    color.clearValue.b = CLEAR_B;
    color.clearValue.a = 1.0;
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;
    render = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
    wgpuRenderPassEncoderEnd(render);

    readback_desc.size = 256;
    readback_desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readback = wgpuDeviceCreateBuffer(device, &readback_desc);
    src.texture = current.texture;
    dst.buffer = readback;
    dst.layout.bytesPerRow = 256;
    dst.layout.rowsPerImage = 1;
    extent.width = 1;
    extent.height = 1;
    extent.depthOrArrayLayers = 1;
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);

    commands = wgpuCommandEncoderFinish(encoder, &buffer_desc);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuRenderPassEncoderRelease(render);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(view);

    map_info.mode = WGPUCallbackMode_WaitAnyOnly;
    map_info.callback = on_map;
    map_info.userdata1 = &mapped;
    if (!wait_future(instance,
                     wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, 256,
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

    pixels = (const uint8_t *)wgpuBufferGetConstMappedRange(readback, 0, 256);
    if (!pixels) {
        report_append("mapped range is null\n");
        return 0;
    }
    if (format == WGPUTextureFormat_BGRA8Unorm) {
        b = pixels[0];
        g = pixels[1];
        r = pixels[2];
        a = pixels[3];
    } else {
        r = pixels[0];
        g = pixels[1];
        b = pixels[2];
        a = pixels[3];
    }
    report_fmt("pixel r=%d", r);
    report_fmt(" g=%d", g);
    report_fmt(" b=%d", b);
    report_fmt(" a=%d\n", a);
    wgpuBufferUnmap(readback);
    wgpuBufferRelease(readback);
    wgpuTextureRelease(current.texture);
    if (r != expect_r || g != expect_g || b != expect_b || a != 255) {
        report_append("pixel does not match the clear color\n");
        return 0;
    }
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
    WGPUFeatureName required[4];
    size_t required_count = 0;
    WaitState adapter_state = { 0 };
    WaitState device_state = { 0 };
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_source =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    WGPUSurface surface;
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    WGPUTextureFormat format;
    int in_worker;

    in_worker = EM_ASM_INT({ return typeof document === 'undefined' ? 1 : 0; });
    report_fmt("worker=%d\n", in_worker);
    if (!in_worker) {
        report_append("device thread is the page thread\n");
        return 0;
    }

    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = instance_features;
    instance = wgpuCreateInstance(&instance_desc);
    if (!instance) {
        report_append(
            "wgpuCreateInstance failed (TimedWaitAny needs Asyncify)\n");
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
        report_append("adapter request failed: ");
        report_append(adapter_state.message);
        report_append("\n");
        return 0;
    }
    adapter = (WGPUAdapter)adapter_state.object;
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        report_view("vendor", info.vendor);
        report_view("architecture", info.architecture);
        report_view("device", info.device);
        report_view("description", info.description);
        wgpuAdapterInfoFreeMembers(info);
    }
    note_feature(adapter, "depth32float-stencil8",
                 WGPUFeatureName_Depth32FloatStencil8);
    note_feature(adapter, "texture-compression-bc",
                 WGPUFeatureName_TextureCompressionBC);
    note_feature(adapter, "clip-distances", WGPUFeatureName_ClipDistances);
    note_feature(adapter, "depth-clip-control",
                 WGPUFeatureName_DepthClipControl);
    note_feature(adapter, "float32-filterable",
                 WGPUFeatureName_Float32Filterable);
    report_append("\n");

    if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_Depth32FloatStencil8)) {
        required[required_count++] = WGPUFeatureName_Depth32FloatStencil8;
    }
    device_desc.requiredFeatureCount = required_count;
    device_desc.requiredFeatures = required;
    device_desc.uncapturedErrorCallbackInfo.callback = on_uncaptured;
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
    if (!queue) {
        report_append("queue is null\n");
        return 0;
    }

    canvas_source.selector = sv("#canvas");
    surface_desc.nextInChain = &canvas_source.chain;
    surface_desc.label = sv("spike");
    surface = wgpuInstanceCreateSurface(instance, &surface_desc);
    if (!surface) {
        report_append("wgpuInstanceCreateSurface failed\n");
        return 0;
    }
    if (wgpuSurfaceGetCapabilities(surface, adapter, &caps) !=
            WGPUStatus_Success ||
        caps.formatCount == 0) {
        report_append("surface has no formats\n");
        return 0;
    }
    format = caps.formats[0];
    if (format == WGPUTextureFormat_BGRA8Unorm) {
        report_append("canvas format=bgra8unorm\n");
    } else if (format == WGPUTextureFormat_RGBA8Unorm) {
        report_append("canvas format=rgba8unorm\n");
    } else {
        report_fmt("canvas format=%d\n", format);
    }
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    config.device = device;
    config.format = format;
    config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    config.width = CANVAS_WIDTH;
    config.height = CANVAS_HEIGHT;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &config);
    report_append("surface configured\n");

    if (!draw_and_read(instance, device, queue, surface, format)) {
        return 0;
    }
    report_append("ok\n");
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
                status.textContent = $1 ? 'spike ok' : 'spike failed';
            }
        },
        g_report, g_ok);
    emscripten_exit_with_live_runtime();
    return 0;
}
