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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"
#include "hw/xbox/nv2a/pgraph/glsl/spirv.h"
#include "hw/xbox/nv2a/pgraph/wgsl/gen.h"
#include "hw/xbox/nv2a/pgraph/wgsl/translate.h"
#include "renderer.h"

#define VSH_UBO_BINDING 0
#define PSH_UBO_BINDING 1
#define PSH_TEX_BINDING 0

/* std140 matches glslang. WGSL uniform layout does not pad array elements or
 * mat2 columns out to 16 bytes, so a WGSL module is packed with wgsl=true. */
static size_t layout_std140(const UniformInfo *info, int count,
                            uint32_t skip_mask, WgpuUniformMember *members,
                            bool wgsl)
{
    size_t offset = 0;

    for (int i = 0; i < count; i++) {
        WgpuUniformMember *m = &members[i];
        if (skip_mask & (1 << i)) {
            m->offset = -1;
            continue;
        }

        if (wgsl) {
            int components = 1;
            int columns = 1;
            switch (info[i].type) {
            case UniformElementType_float:
            case UniformElementType_int:
            case UniformElementType_uint:
                break;
            case UniformElementType_ivec2:
            case UniformElementType_vec2:
                components = 2;
                break;
            case UniformElementType_vec3:
                components = 3;
                break;
            case UniformElementType_ivec4:
            case UniformElementType_vec4:
                components = 4;
                break;
            case UniformElementType_mat2:
                components = 2;
                columns = 2;
                break;
            default:
                g_assert_not_reached();
            }
            size_t el_align =
                components == 3 || components == 4 ? 16 : components * 4;
            size_t el_size = columns * components * 4;
            if (info[i].type == UniformElementType_mat2) {
                /* mat2x2f is two vec2 columns. An array strides by 16. */
                el_align = 8;
                el_size = 16;
            }
            size_t stride =
                info[i].count > 1 ? ROUND_UP(el_size, el_align) : el_size;
            offset = ROUND_UP(offset, el_align);
            m->offset = offset;
            m->column_bytes =
                (info[i].type == UniformElementType_mat2) ? 8 : (int)el_size;
            m->columns = (info[i].type == UniformElementType_mat2) ?
                             2 * info[i].count :
                             info[i].count;
            m->stride =
                (info[i].type == UniformElementType_mat2) ? 8 : (int)stride;
            offset += stride * info[i].count;
            continue;
        }

        int components, columns = 1;
        switch (info[i].type) {
        case UniformElementType_float:
        case UniformElementType_int:
        case UniformElementType_uint:
            components = 1;
            break;
        case UniformElementType_ivec2:
        case UniformElementType_vec2:
            components = 2;
            break;
        case UniformElementType_vec3:
            components = 3;
            break;
        case UniformElementType_ivec4:
        case UniformElementType_vec4:
            components = 4;
            break;
        case UniformElementType_mat2:
            components = 2;
            columns = 2;
            break;
        default:
            g_assert_not_reached();
        }

        /* Array elements and matrix columns are padded to vec4 */
        bool padded = info[i].count > 1 || columns > 1;
        size_t align = padded || components > 2 ? 16 : components * 4;

        offset = ROUND_UP(offset, align);
        m->offset = offset;
        m->column_bytes = components * 4;
        m->columns = columns * info[i].count;
        m->stride = padded ? 16 : m->column_bytes;
        /* Arrays and matrices occupy whole vec4 slots, padding included */
        offset += padded ? m->stride * m->columns : m->column_bytes;
    }

    return ROUND_UP(offset, 16);
}

static void pack_uniforms(uint8_t *out, const UniformInfo *info, int count,
                          const WgpuUniformMember *members, const void *values)
{
    for (int i = 0; i < count; i++) {
        const WgpuUniformMember *m = &members[i];
        if (m->offset < 0) {
            continue;
        }
        const uint8_t *in = (const uint8_t *)values + info[i].val_offs;
        for (int c = 0; c < m->columns; c++) {
            memcpy(out + m->offset + c * m->stride, in + c * m->column_bytes,
                   m->column_bytes);
        }
    }
}

static WGPUShaderModule create_spirv_module(PGRAPHWgpuState *r,
                                            glslang_stage_t stage,
                                            const char *label, MString *glsl)
{
    GByteArray *spirv = pgraph_glsl_compile_to_spirv(
        PGRAPH_GLSL_TARGET_WEBGPU, stage, mstring_get_str(glsl), false);
    WGPUShaderModule module = NULL;

    if (spirv) {
        module = pgraph_wgpu_create_spirv_module(
            r, label, (const uint32_t *)spirv->data, spirv->len / 4);
        g_byte_array_unref(spirv);
    }
    if (!module) {
        fprintf(stderr, "[WebGPU] GLSL source:\n%s\n", mstring_get_str(glsl));
    }
    return module;
}

/* Prefer WGSL. The GLSL text is still produced first and remains the oracle;
 * if translation or Tint rejects it, the SPIR-V module is the fallback. */
static WGPUShaderModule create_draw_module(PGRAPHWgpuState *r,
                                           glslang_stage_t stage,
                                           const char *label, MString *glsl,
                                           bool vertex, bool *used_wgsl)
{
    *used_wgsl = false;
    if (getenv("XEMU_WGSL") && !strcmp(getenv("XEMU_WGSL"), "0")) {
        return create_spirv_module(r, stage, label, glsl);
    }
    g_autoptr(MString) wgsl =
        pgraph_wgsl_translate(mstring_get_str(glsl), vertex);
    if (wgsl && getenv("XEMU_WGSL_DUMP")) {
        fprintf(stderr, "[WebGPU] WGSL %s:\n%s\n", label,
                mstring_get_str(wgsl));
    }
    if (wgsl) {
        WGPUShaderModule module =
            pgraph_wgpu_create_wgsl_module(r, label, mstring_get_str(wgsl));
        if (module) {
            *used_wgsl = true;
            return module;
        }
        fprintf(stderr, "[WebGPU] WGSL source for %s:\n%s\n", label,
                mstring_get_str(wgsl));
    }
    fprintf(stderr, "[WebGPU] %s is using the SPIR-V fallback\n", label);
    return create_spirv_module(r, stage, label, glsl);
}

static WgpuTextureLayout *
get_texture_layout(PGRAPHWgpuState *r,
                   const PshTextureType types[NV2A_MAX_TEXTURES])
{
    for (guint i = 0; i < r->texture_layouts->len; i++) {
        WgpuTextureLayout *l =
            g_array_index(r->texture_layouts, WgpuTextureLayout *, i);
        if (!memcmp(l->types, types, sizeof(l->types))) {
            return l;
        }
    }

    WgpuTextureLayout *l = g_new0(WgpuTextureLayout, 1);
    memcpy(l->types, types, sizeof(l->types));

    WGPUBindGroupLayoutEntry entries[2 * NV2A_MAX_TEXTURES];
    size_t num_entries = 0;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (types[i] == PSH_TEXTURE_TYPE_NONE) {
            continue;
        }
        WGPUBindGroupLayoutEntry texture = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        texture.binding = PSH_TEX_BINDING + 2 * i;
        texture.visibility = WGPUShaderStage_Fragment;
        texture.texture.sampleType = WGPUTextureSampleType_Float;
        texture.texture.viewDimension = WGPUTextureViewDimension_2D;
        switch (types[i]) {
        case PSH_TEXTURE_TYPE_2D_UINT:
            texture.texture.sampleType = WGPUTextureSampleType_Uint;
            break;
        case PSH_TEXTURE_TYPE_3D:
            texture.texture.viewDimension = WGPUTextureViewDimension_3D;
            break;
        case PSH_TEXTURE_TYPE_CUBE:
            texture.texture.viewDimension = WGPUTextureViewDimension_Cube;
            break;
        default:
            break;
        }
        entries[num_entries++] = texture;

        WGPUBindGroupLayoutEntry sampler = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        sampler.binding = PSH_TEX_BINDING + 2 * i + 1;
        sampler.visibility = WGPUShaderStage_Fragment;
        sampler.sampler.type = types[i] == PSH_TEXTURE_TYPE_2D_UINT ?
                                   WGPUSamplerBindingType_NonFiltering :
                                   WGPUSamplerBindingType_Filtering;
        entries[num_entries++] = sampler;
    }

    WGPUBindGroupLayoutDescriptor bgl_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.label = pgraph_wgpu_str("Textures");
    bgl_desc.entryCount = num_entries;
    bgl_desc.entries = entries;
    l->bind_group_layout =
        wgpuDeviceCreateBindGroupLayout(r->device, &bgl_desc);

    WGPUBindGroupLayout layouts[] = { r->uniform_bind_group_layout,
                                      l->bind_group_layout };
    WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl_desc.label = pgraph_wgpu_str("PGRAPH");
    pl_desc.bindGroupLayoutCount = ARRAY_SIZE(layouts);
    pl_desc.bindGroupLayouts = layouts;
    l->pipeline_layout = wgpuDeviceCreatePipelineLayout(r->device, &pl_desc);

    g_array_append_val(r->texture_layouts, l);
    return l;
}

static ShaderBinding *create_shader_binding(PGRAPHWgpuState *r,
                                            const ShaderState *state)
{
    ShaderBinding *b = g_new0(ShaderBinding, 1);
    b->state = *state;

    nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);

    WgpuPrimState prim = pgraph_wgpu_get_prim_state(&state->geom);
    bool expand;
    GenVshGlslOptions vsh_opts = {
        .target = PGRAPH_GLSL_TARGET_WEBGPU,
        .ubo_binding = VSH_UBO_BINDING,
        .point_quads = pgraph_wgpu_prim_topology(&prim, &expand) ==
                       WGPUPrimitiveTopology_PointList,
    };
    MString *vsh_code = pgraph_glsl_gen_vsh(&state->vsh, vsh_opts);
    b->vsh = create_draw_module(r, GLSLANG_STAGE_VERTEX, "PGRAPH vertex",
                                vsh_code, true, &b->vsh_wgsl);
    mstring_unref(vsh_code);

    GenPshGlslOptions psh_opts = {
        .target = PGRAPH_GLSL_TARGET_WEBGPU,
        .ubo_binding = PSH_UBO_BINDING,
        .tex_binding = PSH_TEX_BINDING,
    };
    MString *psh_code = pgraph_glsl_gen_psh(&state->psh, psh_opts);
    b->psh = create_draw_module(r, GLSLANG_STAGE_FRAGMENT, "PGRAPH fragment",
                                psh_code, false, &b->psh_wgsl);
    mstring_unref(psh_code);

    b->valid = b->vsh && b->psh;

    uint32_t vsh_skip =
        state->vsh.uniform_attrs ? 0 : (1 << VshUniform_inlineValue);
    b->vsh_ubo_size = layout_std140(VshUniformInfo, VshUniform__COUNT, vsh_skip,
                                    b->vsh_members, b->vsh_wgsl);
    b->psh_ubo_size = layout_std140(PshUniformInfo, PshUniform__COUNT, 0,
                                    b->psh_members, b->psh_wgsl);
    for (int i = 0; i < VshUniform__COUNT; i++) {
        b->vsh_locs[i] = b->vsh_members[i].offset < 0 ? -1 : i;
    }
    for (int i = 0; i < PshUniform__COUNT; i++) {
        b->psh_locs[i] = b->psh_members[i].offset < 0 ? -1 : i;
    }

    PshTextureType types[NV2A_MAX_TEXTURES];
    pgraph_glsl_get_psh_texture_types(&state->psh, PGRAPH_GLSL_TARGET_WEBGPU,
                                      types);
    b->texture_layout = get_texture_layout(r, types);

    return b;
}

static void free_shader_binding(gpointer data)
{
    ShaderBinding *b = data;

    if (b->vsh) {
        wgpuShaderModuleRelease(b->vsh);
    }
    if (b->psh) {
        wgpuShaderModuleRelease(b->psh);
    }
    g_free(b);
}

static guint shader_state_hash(gconstpointer key)
{
    return fast_hash(key, sizeof(ShaderState));
}

static gboolean shader_state_equal(gconstpointer a, gconstpointer b)
{
    return !memcmp(a, b, sizeof(ShaderState));
}

void pgraph_wgpu_bind_shaders(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r->shader_binding &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        return;
    }

    ShaderState state = pgraph_glsl_get_shader_state(pg);
    ShaderBinding *b = g_hash_table_lookup(r->shader_cache, &state);
    if (!b) {
        b = create_shader_binding(r, &state);
        g_hash_table_insert(r->shader_cache, &b->state, b);
    }
    r->shader_binding = b;
}

/* Mode for the generator's applyBorder(): bits 0-2 are the axes that use
 * BORDER addressing, bit 3 is set when the sampler filters linearly. */
static uint32_t get_texture_border_mode(PGRAPHState *pg, int i)
{
    if (!(pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) &
          NV_PGRAPH_TEXCTL0_0_ENABLE)) {
        return 0;
    }

    uint32_t address = pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + i * 4);
    uint32_t mode = 0;
    if (GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU) ==
        NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER) {
        mode |= 1;
    }
    if (GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV) ==
        NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER) {
        mode |= 2;
    }
    if (GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP) ==
        NV_PGRAPH_TEXADDRESS0_ADDRU_BORDER) {
        mode |= 4;
    }

    uint32_t filter = pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4);
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    if (mode && mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_wgpu_map) &&
        pgraph_texture_mag_filter_wgpu_map[mag_filter] ==
            WGPUFilterMode_Linear) {
        mode |= 8;
    }
    return mode;
}

void pgraph_wgpu_update_uniforms(PGRAPHState *pg, bool *vsh_changed,
                                 bool *psh_changed)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    ShaderBinding *b = r->shader_binding;

    nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);

    VshUniformValues vsh_values;
    memset(&vsh_values, 0, sizeof(vsh_values));
    pgraph_glsl_set_vsh_uniform_values(pg, &b->state.vsh, b->vsh_locs,
                                       &vsh_values);
    memset(r->vsh_uniform_data, 0, b->vsh_ubo_size);
    pack_uniforms(r->vsh_uniform_data, VshUniformInfo, VshUniform__COUNT,
                  b->vsh_members, &vsh_values);

    PshUniformValues psh_values;
    memset(&psh_values, 0, sizeof(psh_values));
    pgraph_glsl_set_psh_uniform_values(pg, b->psh_locs, &psh_values);
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        psh_values.texScale[i] = 1.0f;
        pgraph_argb_pack32_to_rgba_float(
            pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + i * 4),
            psh_values.texBorderColor[i]);
        psh_values.texBorderMode[i] = get_texture_border_mode(pg, i);
        psh_values.texLodBias[i] = pgraph_convert_lod_bias_to_float(
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4),
                     NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS));
    }
    memset(r->psh_uniform_data, 0, b->psh_ubo_size);
    pack_uniforms(r->psh_uniform_data, PshUniformInfo, PshUniform__COUNT,
                  b->psh_members, &psh_values);

    uint64_t vsh_hash = fast_hash(r->vsh_uniform_data, b->vsh_ubo_size);
    uint64_t psh_hash = fast_hash(r->psh_uniform_data, b->psh_ubo_size);
    *vsh_changed = !r->uniforms_valid || vsh_hash != r->uniform_hashes[0];
    *psh_changed = !r->uniforms_valid || psh_hash != r->uniform_hashes[1];
    r->uniform_hashes[0] = vsh_hash;
    r->uniform_hashes[1] = psh_hash;

    nv2a_profile_inc_counter((*vsh_changed || *psh_changed) ?
                                 NV2A_PROF_SHADER_UBO_DIRTY :
                                 NV2A_PROF_SHADER_UBO_NOTDIRTY);
}

static bool check_spirv_support(PGRAPHWgpuState *r)
{
    MString *glsl = mstring_from_str("#version 450\n"
                                     "void main() {\n"
                                     "  gl_Position = vec4(0.0);\n"
                                     "}\n");
    WGPUShaderModule module =
        create_spirv_module(r, GLSLANG_STAGE_VERTEX, "SPIR-V probe", glsl);
    mstring_unref(glsl);
    if (!module) {
        return false;
    }
    wgpuShaderModuleRelease(module);
    return true;
}

void pgraph_wgpu_init_shaders(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    pgraph_glsl_init_spirv_compiler();

    if (!check_spirv_support(r)) {
        fprintf(stderr,
                "[WebGPU] Dawn cannot ingest SPIR-V. Build Dawn with "
                "TINT_BUILD_SPV_READER=ON (see scripts/build-dawn.py).\n");
    }

    r->shader_cache = g_hash_table_new_full(
        shader_state_hash, shader_state_equal, NULL, free_shader_binding);
    r->texture_layouts = g_array_new(false, false, sizeof(WgpuTextureLayout *));

    /* The largest blocks the generator can emit */
    WgpuUniformMember vsh_members[VshUniform__COUNT];
    WgpuUniformMember psh_members[PshUniform__COUNT];
    /* Size the slot for std140. A WGSL block is the same size or smaller. */
    size_t vsh_size =
        layout_std140(VshUniformInfo, VshUniform__COUNT, 0, vsh_members, false);
    size_t psh_size =
        layout_std140(PshUniformInfo, PshUniform__COUNT, 0, psh_members, false);
    r->uniform_binding_size = ROUND_UP(MAX(vsh_size, psh_size), 256);
    r->vsh_uniform_data = g_malloc0(r->uniform_binding_size);
    r->psh_uniform_data = g_malloc0(r->uniform_binding_size);

    WGPUBindGroupLayoutEntry entries[2] = {
        WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
        WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
    };
    entries[0].binding = VSH_UBO_BINDING;
    entries[0].visibility = WGPUShaderStage_Vertex;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.hasDynamicOffset = true;
    entries[1].binding = PSH_UBO_BINDING;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.hasDynamicOffset = true;

    WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Uniforms");
    desc.entryCount = ARRAY_SIZE(entries);
    desc.entries = entries;
    r->uniform_bind_group_layout =
        wgpuDeviceCreateBindGroupLayout(r->device, &desc);
}

void pgraph_wgpu_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    r->shader_binding = NULL;
    if (r->shader_cache) {
        g_hash_table_destroy(r->shader_cache);
        r->shader_cache = NULL;
    }
    if (r->texture_layouts) {
        for (guint i = 0; i < r->texture_layouts->len; i++) {
            WgpuTextureLayout *l =
                g_array_index(r->texture_layouts, WgpuTextureLayout *, i);
            wgpuPipelineLayoutRelease(l->pipeline_layout);
            wgpuBindGroupLayoutRelease(l->bind_group_layout);
            g_free(l);
        }
        g_array_free(r->texture_layouts, true);
        r->texture_layouts = NULL;
    }
    if (r->uniform_bind_group) {
        wgpuBindGroupRelease(r->uniform_bind_group);
        r->uniform_bind_group = NULL;
    }
    if (r->uniform_bind_group_layout) {
        wgpuBindGroupLayoutRelease(r->uniform_bind_group_layout);
        r->uniform_bind_group_layout = NULL;
    }
    g_free(r->vsh_uniform_data);
    r->vsh_uniform_data = NULL;
    g_free(r->psh_uniform_data);
    r->psh_uniform_data = NULL;

    pgraph_glsl_finalize_spirv_compiler();
}
