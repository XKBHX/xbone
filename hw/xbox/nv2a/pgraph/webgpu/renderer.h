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

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_RENDERER_H

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"

#include <webgpu/webgpu.h>

typedef struct PGRAPHWgpuState {
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
} PGRAPHWgpuState;

bool pgraph_wgpu_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_wgpu_finalize_instance(PGRAPHState *pg);

#endif
