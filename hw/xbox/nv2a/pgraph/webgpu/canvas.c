/*
 * Browser canvas for the WebGPU display.
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "canvas.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <stdio.h>
#include <string.h>

static WGPUStringView canvas_str(const char *text)
{
    WGPUStringView view = WGPU_STRING_VIEW_INIT;

    view.data = text;
    view.length = WGPU_STRLEN;
    return view;
}

void wgpu_canvas_init(WgpuCanvas *canvas)
{
    memset(canvas, 0, sizeof(*canvas));
}

static void release_current(WgpuCanvas *canvas)
{
    if (canvas->current_view) {
        wgpuTextureViewRelease(canvas->current_view);
        canvas->current_view = NULL;
    }
    if (canvas->current) {
        wgpuTextureRelease(canvas->current);
        canvas->current = NULL;
    }
}

static bool canvas_size(int *width, int *height)
{
    int w = 0;
    int h = 0;

    if (emscripten_get_canvas_element_size("#canvas", &w, &h) ==
            EMSCRIPTEN_RESULT_SUCCESS &&
        w > 0 && h > 0) {
        *width = w;
        *height = h;
        return true;
    }

    w = 0;
    h = 0;
    /* clang-format splits === inside EM_ASM, which is not valid JS. */
    /* clang-format off */
    EM_ASM(
        {
            var c = (typeof findCanvasEventTarget === 'function') ?
                findCanvasEventTarget('#canvas') : null;
            if (c) {
                setValue($0, c.width | 0, 'i32');
                setValue($1, c.height | 0, 'i32');
            }
        },
        &w, &h);
    /* clang-format on */
    if (w <= 0 || h <= 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

static bool ensure_surface(WgpuCanvas *canvas, WGPUInstance instance,
                           WGPUAdapter adapter, WGPUDevice device, int width,
                           int height)
{
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    WGPUTextureFormat format;

    if (!canvas->surface) {
        source.selector = canvas_str("#canvas");
        surface_desc.nextInChain = &source.chain;
        surface_desc.label = canvas_str("display");
        canvas->surface = wgpuInstanceCreateSurface(instance, &surface_desc);
        if (!canvas->surface) {
            fprintf(stderr, "[WebGPU] Canvas surface creation failed\n");
            return false;
        }
    }

    if (canvas->configured && canvas->width == (uint32_t)width &&
        canvas->height == (uint32_t)height) {
        return true;
    }

    if (wgpuSurfaceGetCapabilities(canvas->surface, adapter, &caps) !=
            WGPUStatus_Success ||
        caps.formatCount == 0) {
        fprintf(stderr, "[WebGPU] Canvas surface has no formats\n");
        return false;
    }
    format = caps.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    config.device = device;
    config.format = format;
    config.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    config.width = width;
    config.height = height;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(canvas->surface, &config);
    canvas->format = format;
    canvas->width = width;
    canvas->height = height;
    canvas->configured = 1;
    return true;
}

bool wgpu_canvas_describe(WgpuCanvas *canvas, WGPUInstance instance,
                          WGPUAdapter adapter, WGPUDevice device,
                          WGPUTextureFormat *format, uint32_t *width,
                          uint32_t *height)
{
    int w = 0;
    int h = 0;

    if (!instance || !adapter || !device || !canvas_size(&w, &h)) {
        return false;
    }
    if (!ensure_surface(canvas, instance, adapter, device, w, h)) {
        return false;
    }
    if (format) {
        *format = canvas->format;
    }
    if (width) {
        *width = canvas->width;
    }
    if (height) {
        *height = canvas->height;
    }
    return true;
}

bool wgpu_canvas_acquire(WgpuCanvas *canvas, WGPUInstance instance,
                         WGPUAdapter adapter, WGPUDevice device,
                         WGPUTextureFormat *format, uint32_t *width,
                         uint32_t *height, WGPUTextureView *view)
{
    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
    int w = 0;
    int h = 0;

    release_current(canvas);
    if (view) {
        *view = NULL;
    }
    if (!instance || !adapter || !device || !canvas_size(&w, &h)) {
        return false;
    }
    if (!ensure_surface(canvas, instance, adapter, device, w, h)) {
        return false;
    }

    wgpuSurfaceGetCurrentTexture(canvas->surface, &current);
    if ((current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
         current.status !=
             WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) ||
        !current.texture) {
        fprintf(stderr, "[WebGPU] Canvas getCurrentTexture status %d\n",
                current.status);
        return false;
    }

    canvas->current = current.texture;
    canvas->current_view = wgpuTextureCreateView(current.texture, NULL);
    if (!canvas->current_view) {
        release_current(canvas);
        return false;
    }
    if (format) {
        *format = canvas->format;
    }
    if (width) {
        *width = canvas->width;
    }
    if (height) {
        *height = canvas->height;
    }
    if (view) {
        *view = canvas->current_view;
    }
    return true;
}

void wgpu_canvas_release(WgpuCanvas *canvas)
{
    release_current(canvas);
}

void wgpu_canvas_fini(WgpuCanvas *canvas)
{
    release_current(canvas);
    if (canvas->surface) {
        wgpuSurfaceRelease(canvas->surface);
        canvas->surface = NULL;
    }
    canvas->configured = 0;
    canvas->width = 0;
    canvas->height = 0;
}
