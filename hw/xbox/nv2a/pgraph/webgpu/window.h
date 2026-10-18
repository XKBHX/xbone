/*
 * Native window surface for the WebGPU display.
 *
 * The UI thread publishes the platform handle and the pixel size. The PFIFO
 * thread creates the Dawn surface and is the only thread that uses it.
 * Describe configures the surface and reports its format. Acquire returns
 * the current texture view. The caller records the gamma blit and ImGui
 * into one command buffer, submits, then calls present. That present is
 * wgpuSurfacePresent, which the browser build does not use.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_WINDOW_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include <webgpu/webgpu.h>

/* Same values as NV2A_WGPU_WINDOW_* in nv2a.h. */
#define WGPU_WINDOW_METAL 1
#define WGPU_WINDOW_HWND 2
#define WGPU_WINDOW_XLIB 3
#define WGPU_WINDOW_WAYLAND 4

typedef struct WgpuWindowTarget {
    int kind;
    void *primary;
    void *secondary;
    uint64_t x_window;
    uint32_t width;
    uint32_t height;
    int vsync;
} WgpuWindowTarget;

typedef struct WgpuWindow {
    WGPUSurface surface;
    WGPUTextureFormat format;
    uint32_t width;
    uint32_t height;
    int vsync;
    int configured;
    int kind;
    void *primary;
    void *secondary;
    uint64_t x_window;
    WGPUTexture current;
    WGPUTextureView current_view;
} WgpuWindow;

void wgpu_window_init(WgpuWindow *window);
void wgpu_window_set_target(const WgpuWindowTarget *target);
bool wgpu_window_active(void);

/* Does not call getCurrentTexture. */
bool wgpu_window_describe(WgpuWindow *window, WGPUInstance instance,
                          WGPUAdapter adapter, WGPUDevice device,
                          WGPUTextureFormat *format, uint32_t *width,
                          uint32_t *height);

/* The returned view stays owned by `window` until wgpu_window_release. */
bool wgpu_window_acquire(WgpuWindow *window, WGPUInstance instance,
                         WGPUAdapter adapter, WGPUDevice device,
                         WGPUTextureFormat *format, uint32_t *width,
                         uint32_t *height, WGPUTextureView *view);

bool wgpu_window_present(WgpuWindow *window);
void wgpu_window_release(WgpuWindow *window);
void wgpu_window_fini(WgpuWindow *window);

#endif
