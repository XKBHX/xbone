/*
 * Geforce NV2A PGRAPH GLSL to SPIR-V Compiler
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_XBOX_NV2A_PGRAPH_GLSL_SPIRV_H
#define HW_XBOX_NV2A_PGRAPH_GLSL_SPIRV_H

#include "qemu/osdep.h"
#include "common.h"

#include <glslang/Include/glslang_c_interface.h>

void pgraph_glsl_init_spirv_compiler(void);
void pgraph_glsl_finalize_spirv_compiler(void);
GByteArray *pgraph_glsl_compile_to_spirv(PgraphGlslTarget target,
                                         glslang_stage_t stage,
                                         const char *glsl_source, bool debug);

#endif
