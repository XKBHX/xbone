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
#include "qapi/error.h"
#include "renderer.h"

typedef struct RequestAdapterState {
    WGPURequestAdapterStatus status;
    WGPUAdapter adapter;
    char *message;
} RequestAdapterState;

typedef struct RequestDeviceState {
    WGPURequestDeviceStatus status;
    WGPUDevice device;
    char *message;
} RequestDeviceState;

static char *pgraph_wgpu_string_dup(WGPUStringView string)
{
    if (!string.data) {
        return g_strdup("");
    }
    return g_strndup(string.data, string.length);
}

static void request_adapter_callback(WGPURequestAdapterStatus status,
                                     WGPUAdapter adapter,
                                     WGPUStringView message, void *userdata1,
                                     void *userdata2)
{
    RequestAdapterState *state = userdata1;

    state->status = status;
    state->adapter = adapter;
    state->message = pgraph_wgpu_string_dup(message);
}

static void request_device_callback(WGPURequestDeviceStatus status,
                                    WGPUDevice device, WGPUStringView message,
                                    void *userdata1, void *userdata2)
{
    RequestDeviceState *state = userdata1;

    state->status = status;
    state->device = device;
    state->message = pgraph_wgpu_string_dup(message);
}

static WGPUAdapter request_adapter(WGPUInstance instance, Error **errp)
{
    RequestAdapterState state = { 0 };
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPURequestAdapterCallbackInfo callback =
        WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    WGPUFutureWaitInfo wait_info = WGPU_FUTURE_WAIT_INFO_INIT;

    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = request_adapter_callback;
    callback.userdata1 = &state;
    wait_info.future = wgpuInstanceRequestAdapter(instance, &options, callback);
    WGPUWaitStatus wait_status =
        wgpuInstanceWaitAny(instance, 1, &wait_info, UINT64_MAX);

    if (wait_status != WGPUWaitStatus_Success) {
        error_setg(errp, "Waiting for WebGPU adapter failed (%d)", wait_status);
    } else if (state.status != WGPURequestAdapterStatus_Success) {
        error_setg(errp, "WebGPU adapter request failed: %s", state.message);
    }
    g_free(state.message);
    return state.adapter;
}

static WGPUDevice request_device(WGPUInstance instance, WGPUAdapter adapter,
                                 Error **errp)
{
    RequestDeviceState state = { 0 };
    WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPURequestDeviceCallbackInfo callback =
        WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    WGPUFutureWaitInfo wait_info = WGPU_FUTURE_WAIT_INFO_INIT;

    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = request_device_callback;
    callback.userdata1 = &state;
    wait_info.future = wgpuAdapterRequestDevice(adapter, &descriptor, callback);
    WGPUWaitStatus wait_status =
        wgpuInstanceWaitAny(instance, 1, &wait_info, UINT64_MAX);

    if (wait_status != WGPUWaitStatus_Success) {
        error_setg(errp, "Waiting for WebGPU device failed (%d)", wait_status);
    } else if (state.status != WGPURequestDeviceStatus_Success) {
        error_setg(errp, "WebGPU device request failed: %s", state.message);
    }
    g_free(state.message);
    return state.device;
}

static void log_adapter(WGPUAdapter adapter)
{
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    WGPUSupportedFeatures features = WGPU_SUPPORTED_FEATURES_INIT;

    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        fprintf(stderr, "[WebGPU] Adapter: %.*s (%.*s), backend %d\n",
                (int)info.device.length, info.device.data,
                (int)info.description.length, info.description.data,
                info.backendType);
        wgpuAdapterInfoFreeMembers(info);
    }

    wgpuAdapterGetFeatures(adapter, &features);
    fprintf(stderr, "[WebGPU] Features (%zu):", features.featureCount);
    for (size_t i = 0; i < features.featureCount; i++) {
        fprintf(stderr, " %d", features.features[i]);
    }
    fprintf(stderr, "\n");
    wgpuSupportedFeaturesFreeMembers(features);
}

bool pgraph_wgpu_init_instance(PGRAPHState *pg, Error **errp)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WGPUInstanceDescriptor descriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
    WGPUInstanceFeatureName instance_features[] = {
        WGPUInstanceFeatureName_TimedWaitAny,
    };

    descriptor.requiredFeatureCount = ARRAY_SIZE(instance_features);
    descriptor.requiredFeatures = instance_features;
    r->instance = wgpuCreateInstance(&descriptor);
    if (!r->instance) {
        error_setg(errp, "Failed to create WebGPU instance");
        return false;
    }

    r->adapter = request_adapter(r->instance, errp);
    if (*errp) {
        return false;
    }
    log_adapter(r->adapter);

    r->device = request_device(r->instance, r->adapter, errp);
    if (*errp) {
        return false;
    }

    r->queue = wgpuDeviceGetQueue(r->device);
    if (!r->queue) {
        error_setg(errp, "Failed to get WebGPU device queue");
        return false;
    }
    return true;
}

void pgraph_wgpu_finalize_instance(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r->queue) {
        wgpuQueueRelease(r->queue);
    }
    if (r->device) {
        wgpuDeviceRelease(r->device);
    }
    if (r->adapter) {
        wgpuAdapterRelease(r->adapter);
    }
    if (r->instance) {
        wgpuInstanceRelease(r->instance);
    }
}
