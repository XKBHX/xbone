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

#ifndef HW_XBOX_NV2A_PGRAPH_WEBGPU_PRIM_H
#define HW_XBOX_NV2A_PGRAPH_WEBGPU_PRIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <webgpu/webgpu.h>

/* Host topology for an NV2A primitive (enum ShaderPrimitiveMode). If
 * *expand is set, the primitive must be rewritten into a list with
 * pgraph_wgpu_prim_expand(). */
WGPUPrimitiveTopology pgraph_wgpu_prim_topology(int primitive_mode,
                                                bool *expand);

/* Upper bound on the indices pgraph_wgpu_prim_expand() emits for count
 * vertices. */
size_t pgraph_wgpu_prim_max_indices(int primitive_mode, size_t count);

/* Expands count vertices into list indices. Vertex i is in[i] if in is not
 * NULL, otherwise base + i. Returns the number of indices written. */
size_t pgraph_wgpu_prim_expand(int primitive_mode, const uint32_t *in,
                               uint32_t base, size_t count, uint32_t *out);

#endif
