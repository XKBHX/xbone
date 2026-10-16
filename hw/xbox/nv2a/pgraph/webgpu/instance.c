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
#include "ui/xemu-settings.h"

#ifndef __EMSCRIPTEN__
struct WgpuBlobCache {
    char *dir;
    GMutex mutex;
};

static char *blob_cache_path(WgpuBlobCache *cache, const uint8_t *key,
                             size_t key_size)
{
    gchar *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, key, key_size);
    char *path = g_build_filename(cache->dir, sum, NULL);

    g_free(sum);
    return path;
}

/* Dawn calls these from the pipeline compiler thread. A zero valueSize asks
 * for the stored size; a later call copies that many bytes. */
static size_t blob_cache_load(size_t key_size, const uint8_t *key,
                              size_t value_size, uint8_t *value,
                              void *userdata1, void *userdata2)
{
    WgpuBlobCache *cache = userdata1;
    size_t stored = 0;

    if (!cache->dir || !key_size) {
        return 0;
    }

    g_mutex_lock(&cache->mutex);
    g_autofree char *path = blob_cache_path(cache, key, key_size);
    if (!value_size) {
        GStatBuf st;

        if (g_stat(path, &st) == 0 && st.st_size > 0) {
            stored = st.st_size;
        }
    } else {
        g_autofree char *data = NULL;
        gsize len = 0;

        if (g_file_get_contents(path, &data, &len, NULL) && len == value_size &&
            value) {
            memcpy(value, data, value_size);
            stored = value_size;
        }
    }
    g_mutex_unlock(&cache->mutex);
    return stored;
}

static void blob_cache_store(size_t key_size, const uint8_t *key,
                             size_t value_size, const uint8_t *value,
                             void *userdata1, void *userdata2)
{
    WgpuBlobCache *cache = userdata1;

    if (!cache->dir || !key_size || !value_size || !value) {
        return;
    }

    g_mutex_lock(&cache->mutex);
    g_autofree char *path = blob_cache_path(cache, key, key_size);
    if (!g_file_set_contents(path, (const char *)value, value_size, NULL)) {
        fprintf(stderr, "[WebGPU] Failed to store pipeline cache entry\n");
    }
    g_mutex_unlock(&cache->mutex);
}

static WgpuBlobCache *blob_cache_new(void)
{
    const char *base = xemu_settings_get_base_path();
    WgpuBlobCache *cache = g_new0(WgpuBlobCache, 1);

    cache->dir = g_build_filename(base, "webgpu-pipeline-cache", NULL);
    g_mutex_init(&cache->mutex);
    if (g_mkdir_with_parents(cache->dir, 0700) != 0) {
        fprintf(stderr, "[WebGPU] Pipeline cache disabled (%s)\n", cache->dir);
        g_free(cache->dir);
        cache->dir = NULL;
    } else {
        fprintf(stderr, "[WebGPU] Pipeline cache: %s\n", cache->dir);
    }
    return cache;
}

static void blob_cache_free(WgpuBlobCache *cache)
{
    if (!cache) {
        return;
    }
    g_mutex_clear(&cache->mutex);
    g_free(cache->dir);
    g_free(cache);
}
#endif

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

static void uncaptured_error_callback(WGPUDevice const *device,
                                      WGPUErrorType type,
                                      WGPUStringView message, void *userdata1,
                                      void *userdata2)
{
    fprintf(stderr, "[WebGPU] Error (%d): %.*s\n", type, (int)message.length,
            message.data);
}

static void device_lost_callback(WGPUDevice const *device,
                                 WGPUDeviceLostReason reason,
                                 WGPUStringView message, void *userdata1,
                                 void *userdata2)
{
    if (reason == WGPUDeviceLostReason_Destroyed ||
        reason == WGPUDeviceLostReason_CallbackCancelled) {
        return;
    }
    fprintf(stderr, "[WebGPU] Device lost (%d): %.*s\n", reason,
            (int)message.length, message.data);
}

/*
 * Sharing is optional. Depth32FloatStencil8 stays required. The fence
 * feature is requested with the memory feature because EndAccess rejects
 * the texture without it.
 */
static void add_shared_display_features(PGRAPHWgpuState *r, WGPUAdapter adapter,
                                        WGPUFeatureName *features,
                                        size_t *count)
{
#if defined(CONFIG_OPENGL) && !defined(__EMSCRIPTEN__)
    bool have_memory = false;
    bool have_fence = false;
    WGPUFeatureName memory_feature = 0;
    WGPUFeatureName fence_feature = 0;

#ifdef __APPLE__
    memory_feature = WGPUFeatureName_SharedTextureMemoryIOSurface;
    fence_feature = WGPUFeatureName_SharedFenceMTLSharedEvent;
    have_memory = true;
    have_fence = true;
#elif defined(__linux__)
    memory_feature = WGPUFeatureName_SharedTextureMemoryDmaBuf;
    have_memory = true;
    if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_SharedFenceSyncFD)) {
        fence_feature = WGPUFeatureName_SharedFenceSyncFD;
        have_fence = true;
    } else if (wgpuAdapterHasFeature(
                   adapter, WGPUFeatureName_SharedFenceVkSemaphoreOpaqueFD)) {
        fence_feature = WGPUFeatureName_SharedFenceVkSemaphoreOpaqueFD;
        have_fence = true;
    }
#elif defined(_WIN32)
    memory_feature = WGPUFeatureName_SharedTextureMemoryDXGISharedHandle;
    fence_feature = WGPUFeatureName_SharedFenceDXGISharedHandle;
    have_memory = true;
    have_fence = true;
#endif

    if (!have_memory || !have_fence ||
        !wgpuAdapterHasFeature(adapter, memory_feature) ||
        !wgpuAdapterHasFeature(adapter, fence_feature)) {
        return;
    }
    features[(*count)++] = memory_feature;
    features[(*count)++] = fence_feature;
    r->shared_display = true;
#else
    (void)r;
    (void)adapter;
    (void)features;
    (void)count;
#endif
}

static WGPUDevice request_device(PGRAPHWgpuState *r, Error **errp)
{
    WGPUInstance instance = r->instance;
    WGPUAdapter adapter = r->adapter;
    RequestDeviceState state = { 0 };
    WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPURequestDeviceCallbackInfo callback =
        WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    WGPUFutureWaitInfo wait_info = WGPU_FUTURE_WAIT_INFO_INIT;
    WGPUFeatureName required_features[8] = {
        WGPUFeatureName_Depth32FloatStencil8,
    };
    size_t num_required_features = 1;

    for (size_t i = 0; i < num_required_features; i++) {
        if (!wgpuAdapterHasFeature(adapter, required_features[i])) {
            error_setg(errp, "WebGPU adapter lacks required feature %d",
                       required_features[i]);
            return NULL;
        }
    }
    r->depth_clip_control =
        wgpuAdapterHasFeature(adapter, WGPUFeatureName_DepthClipControl);
    if (r->depth_clip_control) {
        required_features[num_required_features++] =
            WGPUFeatureName_DepthClipControl;
    }
    r->float32_filterable =
        wgpuAdapterHasFeature(adapter, WGPUFeatureName_Float32Filterable);
    if (r->float32_filterable) {
        required_features[num_required_features++] =
            WGPUFeatureName_Float32Filterable;
    }
    add_shared_display_features(r, adapter, required_features,
                                &num_required_features);
    descriptor.requiredFeatureCount = num_required_features;
    descriptor.requiredFeatures = required_features;
    descriptor.uncapturedErrorCallbackInfo.callback = uncaptured_error_callback;
    descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    descriptor.deviceLostCallbackInfo.callback = device_lost_callback;

#ifndef __EMSCRIPTEN__
    WGPUDawnCacheDeviceDescriptor cache =
        WGPU_DAWN_CACHE_DEVICE_DESCRIPTOR_INIT;
    if (r->blob_cache && r->blob_cache->dir) {
        cache.isolationKey = pgraph_wgpu_str("xemu-nv2a");
        cache.dawnLoadCacheDataCallbackInfo.callback = blob_cache_load;
        cache.dawnLoadCacheDataCallbackInfo.userdata1 = r->blob_cache;
        cache.dawnStoreCacheDataCallbackInfo.callback = blob_cache_store;
        cache.dawnStoreCacheDataCallbackInfo.userdata1 = r->blob_cache;
        descriptor.nextInChain = &cache.chain;
    }
#endif

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

bool pgraph_wgpu_wait(PGRAPHWgpuState *r, WGPUFuture future)
{
    WGPUFutureWaitInfo wait_info = WGPU_FUTURE_WAIT_INFO_INIT;

    wait_info.future = future;
    WGPUWaitStatus status =
        wgpuInstanceWaitAny(r->instance, 1, &wait_info, UINT64_MAX);
    if (status != WGPUWaitStatus_Success) {
        fprintf(stderr, "[WebGPU] Wait failed (%d)\n", status);
        return false;
    }
    return true;
}

typedef struct ErrorScopeState {
    WGPUErrorType type;
    char *message;
} ErrorScopeState;

static void pop_error_scope_callback(WGPUPopErrorScopeStatus status,
                                     WGPUErrorType type, WGPUStringView message,
                                     void *userdata1, void *userdata2)
{
    ErrorScopeState *state = userdata1;

    state->type = status == WGPUPopErrorScopeStatus_Success ?
                      type :
                      WGPUErrorType_Unknown;
    if (type != WGPUErrorType_NoError) {
        state->message = g_strndup(message.data, message.length);
    }
}

static WGPUShaderModule finish_shader_module(PGRAPHWgpuState *r,
                                             const char *label,
                                             WGPUShaderModule module)
{
    WGPUPopErrorScopeCallbackInfo callback =
        WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    ErrorScopeState state = { 0 };

    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = pop_error_scope_callback;
    callback.userdata1 = &state;
    pgraph_wgpu_wait(r, wgpuDevicePopErrorScope(r->device, callback));

    if (state.type != WGPUErrorType_NoError) {
        fprintf(stderr, "[WebGPU] Failed to create %s shader module: %s\n",
                label, state.message ?: "unknown error");
        g_free(state.message);
        if (module) {
            wgpuShaderModuleRelease(module);
        }
        return NULL;
    }
    return module;
}

WGPUShaderModule pgraph_wgpu_create_wgsl_module(PGRAPHWgpuState *r,
                                                const char *label,
                                                const char *code)
{
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;

    source.code = pgraph_wgpu_str(code);
    descriptor.nextInChain = &source.chain;
    descriptor.label = pgraph_wgpu_str(label);
    wgpuDevicePushErrorScope(r->device, WGPUErrorFilter_Validation);
    return finish_shader_module(
        r, label, wgpuDeviceCreateShaderModule(r->device, &descriptor));
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

#ifndef __EMSCRIPTEN__
    r->blob_cache = blob_cache_new();
#endif
    r->device = request_device(r, errp);
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
#ifndef __EMSCRIPTEN__
    blob_cache_free(r->blob_cache);
    r->blob_cache = NULL;
#endif
}
