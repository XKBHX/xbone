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

#ifndef HW_XBOX_NV2A_PGRAPH_WGSL_TRANSLATE_H
#define HW_XBOX_NV2A_PGRAPH_WGSL_TRANSLATE_H

#include "qemu/osdep.h"
#include "qemu/mstring.h"

/* Translate WebGPU-targeted GLSL from the existing generator into WGSL.
 * Returns NULL if the GLSL uses something this translator does not cover.
 */
MString *pgraph_wgsl_translate(const char *glsl, bool vertex);

#endif
