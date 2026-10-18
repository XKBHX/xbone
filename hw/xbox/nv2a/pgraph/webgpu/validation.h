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

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_VALIDATION_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_VALIDATION_H

#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dawn backend validation (Metal shader validation, Vulkan layers, or the
 * D3D debug layer). The descriptor's required features are kept. */
WGPUInstance
pgraph_wgpu_create_validated_instance(const WGPUInstanceDescriptor *descriptor);

#ifdef __cplusplus
}
#endif

#endif
