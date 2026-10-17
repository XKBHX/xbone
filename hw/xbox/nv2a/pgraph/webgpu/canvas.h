/*
 * Browser canvas for the WebGPU display.
 *
 * The PFIFO thread owns the transferred OffscreenCanvas. Acquire returns
 * the current texture view. The caller records the PCRTC composite and the
 * UI into that view, then releases it. wgpuSurfacePresent aborts in
 * emdawnwebgpu; submitting the canvas texture is the present.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_CANVAS_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WgpuCanvas {
    WGPUSurface surface;
    WGPUTextureFormat format;
    uint32_t width;
    uint32_t height;
    int configured;
    WGPUTexture current;
    WGPUTextureView current_view;
} WgpuCanvas;

void wgpu_canvas_init(WgpuCanvas *canvas);

/* The returned view stays owned by `canvas` until wgpu_canvas_release. */
bool wgpu_canvas_acquire(WgpuCanvas *canvas, WGPUInstance instance,
                         WGPUAdapter adapter, WGPUDevice device,
                         WGPUTextureFormat *format, uint32_t *width,
                         uint32_t *height, WGPUTextureView *view);

void wgpu_canvas_release(WgpuCanvas *canvas);
void wgpu_canvas_fini(WgpuCanvas *canvas);

#ifdef __cplusplus
}
#endif

#endif
