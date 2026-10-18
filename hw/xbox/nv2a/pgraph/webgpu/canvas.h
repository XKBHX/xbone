/*
 * Browser canvas for the WebGPU display.
 *
 * The PFIFO thread owns the transferred OffscreenCanvas. Describe configures
 * the surface and reports its format without calling getCurrentTexture.
 * Acquire returns the current texture view. Call it only after any
 * suspending wait: the browser expires that texture when the worker's
 * JavaScript task ends. The caller records every canvas pass into one
 * command buffer and submits once, then releases the view. wgpuSurfacePresent
 * aborts in emdawnwebgpu; that submit is the present.
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

/* Configures #canvas when its size changes. Does not call getCurrentTexture. */
bool wgpu_canvas_describe(WgpuCanvas *canvas, WGPUInstance instance,
                          WGPUAdapter adapter, WGPUDevice device,
                          WGPUTextureFormat *format, uint32_t *width,
                          uint32_t *height);

/* The returned view stays owned by `canvas` until wgpu_canvas_release.
 * A wait between this and the submit expires the texture. */
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
