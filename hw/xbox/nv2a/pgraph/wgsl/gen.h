/*
 * Geforce NV2A PGRAPH WGSL Shader Generator
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_WGSL_GEN_H
#define HW_XBOX_NV2A_PGRAPH_WGSL_GEN_H

#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"

/*
 * Emit WGSL for the same VshState / PshState the GLSL generator uses.
 * The GLSL text remains the source of the shading rules; this translates
 * the WebGPU-targeted GLSL into WGSL so the browser path does not need
 * SPIR-V or glslang.
 */
MString *pgraph_wgsl_gen_vsh(const VshState *state, GenVshGlslOptions opts);
MString *pgraph_wgsl_gen_psh(const PshState *state, GenPshGlslOptions opts);

#endif
