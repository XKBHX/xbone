/*
 * Native window surface for the WebGPU display.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "window.h"

#include <stdio.h>

static WgpuWindowTarget g_target;
static QemuMutex g_target_lock;
static int g_target_lock_ready;

static void ensure_lock(void)
{
    if (g_target_lock_ready) {
        return;
    }
    qemu_mutex_init(&g_target_lock);
    g_target_lock_ready = 1;
}

static WgpuWindowTarget copy_target(void)
{
    WgpuWindowTarget target = { 0 };

    ensure_lock();
    qemu_mutex_lock(&g_target_lock);
    target = g_target;
    qemu_mutex_unlock(&g_target_lock);
    return target;
}

void wgpu_window_set_target(const WgpuWindowTarget *target)
{
    ensure_lock();
    qemu_mutex_lock(&g_target_lock);
    g_target = *target;
    qemu_mutex_unlock(&g_target_lock);
}

bool wgpu_window_active(void)
{
    return copy_target().kind != 0;
}

void wgpu_window_init(WgpuWindow *window)
{
    memset(window, 0, sizeof(*window));
}

static void release_current(WgpuWindow *window)
{
    if (window->current_view) {
        wgpuTextureViewRelease(window->current_view);
        window->current_view = NULL;
    }
    if (window->current) {
        wgpuTextureRelease(window->current);
        window->current = NULL;
    }
}

static WGPUStringView window_str(const char *text)
{
    WGPUStringView view = WGPU_STRING_VIEW_INIT;

    view.data = text;
    view.length = WGPU_STRLEN;
    return view;
}

static bool same_target(const WgpuWindow *window,
                        const WgpuWindowTarget *target)
{
    return window->kind == target->kind && window->primary == target->primary &&
           window->secondary == target->secondary &&
           window->x_window == target->x_window;
}

static bool create_surface(WgpuWindow *window, WGPUInstance instance,
                           const WgpuWindowTarget *target)
{
    WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;

    surface_desc.label = window_str("window");
    switch (target->kind) {
    case WGPU_WINDOW_METAL: {
        WGPUSurfaceSourceMetalLayer source =
            WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;

        source.layer = target->primary;
        surface_desc.nextInChain = &source.chain;
        window->surface = wgpuInstanceCreateSurface(instance, &surface_desc);
        break;
    }
    case WGPU_WINDOW_HWND: {
        WGPUSurfaceSourceWindowsHWND source =
            WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;

        source.hinstance = target->primary;
        source.hwnd = target->secondary;
        surface_desc.nextInChain = &source.chain;
        window->surface = wgpuInstanceCreateSurface(instance, &surface_desc);
        break;
    }
    case WGPU_WINDOW_XLIB: {
        WGPUSurfaceSourceXlibWindow source =
            WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;

        source.display = target->primary;
        source.window = target->x_window;
        surface_desc.nextInChain = &source.chain;
        window->surface = wgpuInstanceCreateSurface(instance, &surface_desc);
        break;
    }
    case WGPU_WINDOW_WAYLAND: {
        WGPUSurfaceSourceWaylandSurface source =
            WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;

        source.display = target->primary;
        source.surface = target->secondary;
        surface_desc.nextInChain = &source.chain;
        window->surface = wgpuInstanceCreateSurface(instance, &surface_desc);
        break;
    }
    default:
        return false;
    }
    if (!window->surface) {
        fprintf(stderr, "[WebGPU] Window surface creation failed\n");
        return false;
    }
    window->kind = target->kind;
    window->primary = target->primary;
    window->secondary = target->secondary;
    window->x_window = target->x_window;
    window->configured = 0;
    return true;
}

static bool mode_supported(const WGPUSurfaceCapabilities *caps,
                           WGPUPresentMode mode)
{
    for (size_t i = 0; i < caps->presentModeCount; i++) {
        if (caps->presentModes[i] == mode) {
            return true;
        }
    }
    return false;
}

static WGPUTextureFormat choose_format(const WGPUSurfaceCapabilities *caps)
{
    for (size_t i = 0; i < caps->formatCount; i++) {
        WGPUTextureFormat format = caps->formats[i];

        if (format == WGPUTextureFormat_BGRA8Unorm ||
            format == WGPUTextureFormat_RGBA8Unorm) {
            return format;
        }
    }
    if (caps->formatCount == 0) {
        return WGPUTextureFormat_Undefined;
    }
    return caps->formats[0];
}

static WGPUPresentMode choose_present_mode(const WGPUSurfaceCapabilities *caps,
                                           int vsync)
{
    WGPUPresentMode preferred =
        vsync ? WGPUPresentMode_Fifo : WGPUPresentMode_Immediate;

    if (mode_supported(caps, preferred)) {
        return preferred;
    }
    if (vsync && mode_supported(caps, WGPUPresentMode_Mailbox)) {
        return WGPUPresentMode_Mailbox;
    }
    if (!vsync && mode_supported(caps, WGPUPresentMode_Fifo)) {
        return WGPUPresentMode_Fifo;
    }
    if (caps->presentModeCount == 0) {
        return preferred;
    }
    return caps->presentModes[0];
}

static WGPUCompositeAlphaMode choose_alpha(const WGPUSurfaceCapabilities *caps)
{
    for (size_t i = 0; i < caps->alphaModeCount; i++) {
        if (caps->alphaModes[i] == WGPUCompositeAlphaMode_Opaque) {
            return WGPUCompositeAlphaMode_Opaque;
        }
    }
    if (caps->alphaModeCount == 0) {
        return WGPUCompositeAlphaMode_Opaque;
    }
    return caps->alphaModes[0];
}

static bool ensure_surface(WgpuWindow *window, WGPUInstance instance,
                           WGPUAdapter adapter, WGPUDevice device,
                           const WgpuWindowTarget *target)
{
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    WGPUTextureFormat format;
    WGPUTextureUsage usage = WGPUTextureUsage_RenderAttachment;

    if (target->kind == 0 || target->width == 0 || target->height == 0) {
        return false;
    }
    if (window->surface && !same_target(window, target)) {
        release_current(window);
        wgpuSurfaceRelease(window->surface);
        window->surface = NULL;
        window->configured = 0;
    }
    if (!window->surface && !create_surface(window, instance, target)) {
        return false;
    }
    if (window->configured && window->width == target->width &&
        window->height == target->height && window->vsync == target->vsync) {
        return true;
    }
    if (wgpuSurfaceGetCapabilities(window->surface, adapter, &caps) !=
            WGPUStatus_Success ||
        caps.formatCount == 0) {
        fprintf(stderr, "[WebGPU] Window surface has no formats\n");
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        return false;
    }
    format = choose_format(&caps);
    if (caps.usages & WGPUTextureUsage_CopySrc) {
        usage |= WGPUTextureUsage_CopySrc;
    }
    config.device = device;
    config.format = format;
    config.usage = usage;
    config.width = target->width;
    config.height = target->height;
    config.alphaMode = choose_alpha(&caps);
    config.presentMode = choose_present_mode(&caps, target->vsync);
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    wgpuSurfaceConfigure(window->surface, &config);
    window->format = format;
    window->width = target->width;
    window->height = target->height;
    window->vsync = target->vsync;
    window->configured = 1;
    return true;
}

bool wgpu_window_describe(WgpuWindow *window, WGPUInstance instance,
                          WGPUAdapter adapter, WGPUDevice device,
                          WGPUTextureFormat *format, uint32_t *width,
                          uint32_t *height)
{
    WgpuWindowTarget target = copy_target();

    if (!instance || !adapter || !device ||
        !ensure_surface(window, instance, adapter, device, &target)) {
        return false;
    }
    if (format) {
        *format = window->format;
    }
    if (width) {
        *width = window->width;
    }
    if (height) {
        *height = window->height;
    }
    return true;
}

static bool current_ok(WGPUSurfaceGetCurrentTextureStatus status)
{
    return status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
           status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
}

static const char *format_name(WGPUTextureFormat format)
{
    switch (format) {
    case WGPUTextureFormat_BGRA8Unorm:
        return "bgra8unorm";
    case WGPUTextureFormat_RGBA8Unorm:
        return "rgba8unorm";
    default:
        return "other";
    }
}

bool wgpu_window_acquire(WgpuWindow *window, WGPUInstance instance,
                         WGPUAdapter adapter, WGPUDevice device,
                         WGPUTextureFormat *format, uint32_t *width,
                         uint32_t *height, WGPUTextureView *view)
{
    static int logged;
    WgpuWindowTarget target = copy_target();
    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;

    release_current(window);
    if (view) {
        *view = NULL;
    }
    if (!instance || !adapter || !device ||
        !ensure_surface(window, instance, adapter, device, &target)) {
        return false;
    }

    wgpuSurfaceGetCurrentTexture(window->surface, &current);
    if (!current_ok(current.status)) {
        WGPUSurfaceTexture retry = WGPU_SURFACE_TEXTURE_INIT;

        if (current.texture) {
            wgpuTextureRelease(current.texture);
            current.texture = NULL;
        }
        window->configured = 0;
        if (!ensure_surface(window, instance, adapter, device, &target)) {
            return false;
        }
        wgpuSurfaceGetCurrentTexture(window->surface, &retry);
        current = retry;
    }
    if (!current_ok(current.status) || !current.texture) {
        fprintf(stderr, "[WebGPU] Window getCurrentTexture status %d\n",
                current.status);
        if (current.texture) {
            wgpuTextureRelease(current.texture);
        }
        return false;
    }

    window->current = current.texture;
    window->current_view = wgpuTextureCreateView(current.texture, NULL);
    if (!window->current_view) {
        release_current(window);
        return false;
    }
    if (format) {
        *format = window->format;
    }
    if (width) {
        *width = window->width;
    }
    if (height) {
        *height = window->height;
    }
    if (view) {
        *view = window->current_view;
    }
    if (!logged) {
        fprintf(stderr, "[WebGPU] Presenting to the window (%ux%u, %s)\n",
                window->width, window->height, format_name(window->format));
        logged = 1;
    }
    return true;
}

bool wgpu_window_present(WgpuWindow *window)
{
    WGPUStatus status;

    if (!window->surface || !window->current) {
        return false;
    }
    status = wgpuSurfacePresent(window->surface);
    if (status != WGPUStatus_Success) {
        fprintf(stderr, "[WebGPU] Window present status %d\n", (int)status);
        return false;
    }
    return true;
}

void wgpu_window_release(WgpuWindow *window)
{
    release_current(window);
}

void wgpu_window_fini(WgpuWindow *window)
{
    release_current(window);
    if (window->surface) {
        wgpuSurfaceRelease(window->surface);
        window->surface = NULL;
    }
    window->configured = 0;
    window->width = 0;
    window->height = 0;
}
