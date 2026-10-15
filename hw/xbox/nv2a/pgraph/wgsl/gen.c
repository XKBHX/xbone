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

#include "qemu/osdep.h"
#include "gen.h"
#include "translate.h"

MString *pgraph_wgsl_gen_vsh(const VshState *state, GenVshGlslOptions opts)
{
    g_autoptr(MString) glsl = pgraph_glsl_gen_vsh(state, opts);

    return pgraph_wgsl_translate(mstring_get_str(glsl), true);
}

MString *pgraph_wgsl_gen_psh(const PshState *state, GenPshGlslOptions opts)
{
    g_autoptr(MString) glsl = pgraph_glsl_gen_psh(state, opts);

    return pgraph_wgsl_translate(mstring_get_str(glsl), false);
}
