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
#include "hw/xbox/nv2a/nv2a_int.h"
#include "prim.h"
#include "renderer.h"

#define SCRATCH_ZETA_FORMAT WGPUTextureFormat_Depth24Plus

static const char clear_wgsl[] =
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f "
    "{\n"
    "    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4f {\n"
    "    return vec4f(1.0);\n"
    "}\n";

static guint pipeline_key_hash(gconstpointer key)
{
    return fast_hash(key, sizeof(PipelineKey));
}

static gboolean pipeline_key_equal(gconstpointer a, gconstpointer b)
{
    return !memcmp(a, b, sizeof(PipelineKey));
}

static void release_pipeline(gpointer data)
{
    wgpuRenderPipelineRelease(data);
}

void pgraph_wgpu_init_pipelines(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    r->clear_module = pgraph_wgpu_create_wgsl_module(r, "Clear", clear_wgsl);
    r->clear_pipelines = g_array_new(false, true, sizeof(ClearPipeline));
    r->pipeline_cache = g_hash_table_new_full(
        pipeline_key_hash, pipeline_key_equal, g_free, release_pipeline);
}

void pgraph_wgpu_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r->pipeline_cache) {
        g_hash_table_destroy(r->pipeline_cache);
        r->pipeline_cache = NULL;
    }
    if (r->scratch_zeta) {
        wgpuTextureViewRelease(r->scratch_zeta_view);
        wgpuTextureRelease(r->scratch_zeta);
        r->scratch_zeta_view = NULL;
        r->scratch_zeta = NULL;
    }
    if (r->clear_pipelines) {
        for (guint i = 0; i < r->clear_pipelines->len; i++) {
            wgpuRenderPipelineRelease(
                g_array_index(r->clear_pipelines, ClearPipeline, i).pipeline);
        }
        g_array_free(r->clear_pipelines, true);
        r->clear_pipelines = NULL;
    }
    if (r->clear_module) {
        wgpuShaderModuleRelease(r->clear_module);
        r->clear_module = NULL;
    }
}

static WGPURenderPipeline create_clear_pipeline(PGRAPHWgpuState *r,
                                                const ClearPipelineKey *key)
{
    WGPUBlendState blend = {
        .color = { .operation = WGPUBlendOperation_Add,
                   .srcFactor = WGPUBlendFactor_Constant,
                   .dstFactor = WGPUBlendFactor_Zero },
        .alpha = { .operation = WGPUBlendOperation_Add,
                   .srcFactor = WGPUBlendFactor_Constant,
                   .dstFactor = WGPUBlendFactor_Zero },
    };
    WGPUColorTargetState color_target = WGPU_COLOR_TARGET_STATE_INIT;
    color_target.format = key->color_format;
    color_target.blend = &blend;
    color_target.writeMask = key->write_mask;

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = r->clear_module;
    fragment.entryPoint = pgraph_wgpu_str("fs");
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    WGPUStencilFaceState stencil_face = WGPU_STENCIL_FACE_STATE_INIT;
    stencil_face.compare = WGPUCompareFunction_Always;
    stencil_face.failOp = WGPUStencilOperation_Replace;
    stencil_face.depthFailOp = WGPUStencilOperation_Replace;
    stencil_face.passOp = WGPUStencilOperation_Replace;

    WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth_stencil.format = key->zeta_format;
    depth_stencil.depthWriteEnabled =
        key->depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    depth_stencil.depthCompare = WGPUCompareFunction_Always;
    depth_stencil.stencilFront = stencil_face;
    depth_stencil.stencilBack = stencil_face;
    depth_stencil.stencilReadMask = 0xff;
    depth_stencil.stencilWriteMask = key->stencil_write ? 0xff : 0;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Clear");
    desc.vertex.module = r->clear_module;
    desc.vertex.entryPoint = pgraph_wgpu_str("vs");
    if (key->color_format != WGPUTextureFormat_Undefined) {
        desc.fragment = &fragment;
    }
    if (key->zeta_format != WGPUTextureFormat_Undefined) {
        desc.depthStencil = &depth_stencil;
    }
    return wgpuDeviceCreateRenderPipeline(r->device, &desc);
}

static WGPURenderPipeline get_clear_pipeline(PGRAPHWgpuState *r,
                                             const ClearPipelineKey *key)
{
    for (guint i = 0; i < r->clear_pipelines->len; i++) {
        ClearPipeline *p = &g_array_index(r->clear_pipelines, ClearPipeline, i);
        if (!memcmp(&p->key, key, sizeof(*key))) {
            return p->pipeline;
        }
    }

    ClearPipeline p = { .key = *key,
                        .pipeline = create_clear_pipeline(r, key) };
    g_array_append_val(r->clear_pipelines, p);
    return p.pipeline;
}

void pgraph_wgpu_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;

    pgraph_wgpu_surface_update(d, true, write_color, write_zeta);

    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = xmax - xmin + 1;
    unsigned int scissor_height = ymax - ymin + 1;

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    unsigned int surface_width = binding->width,
                 surface_height = binding->height;
    pgraph_apply_scaling_factor(pg, &surface_width, &surface_height);
    scissor_width = MIN(scissor_width, surface_width - xmin);
    scissor_height = MIN(scissor_height, surface_height - ymin);

    SurfaceBinding *color = write_color ? r->color_binding : NULL;
    SurfaceBinding *zeta = write_zeta ? r->zeta_binding : NULL;
    if (!color && !zeta) {
        pg->clearing = false;
        return;
    }

    ClearPipelineKey key;
    memset(&key, 0, sizeof(key));
    if (color) {
        key.color_format = color->host_fmt.format;
        if (parameter & NV097_CLEAR_SURFACE_R) {
            key.write_mask |= WGPUColorWriteMask_Red;
        }
        if (parameter & NV097_CLEAR_SURFACE_G) {
            key.write_mask |= WGPUColorWriteMask_Green;
        }
        if (parameter & NV097_CLEAR_SURFACE_B) {
            key.write_mask |= WGPUColorWriteMask_Blue;
        }
        if (parameter & NV097_CLEAR_SURFACE_A) {
            key.write_mask |= WGPUColorWriteMask_Alpha;
        }
    }
    if (zeta) {
        key.zeta_format = zeta->host_fmt.format;
        key.depth_write = parameter & NV097_CLEAR_SURFACE_Z;
        key.stencil_write = parameter & NV097_CLEAR_SURFACE_STENCIL;
    }
    WGPURenderPipeline pipeline = get_clear_pipeline(r, &key);

    WGPURenderPassColorAttachment color_attachment =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDepthStencilAttachment ds_attachment =
        WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.label = pgraph_wgpu_str("Clear");
    if (color) {
        color_attachment.view = color->view;
        color_attachment.loadOp = WGPULoadOp_Load;
        color_attachment.storeOp = WGPUStoreOp_Store;
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color_attachment;
    }
    if (zeta) {
        ds_attachment.view = zeta->view;
        ds_attachment.depthLoadOp = WGPULoadOp_Load;
        ds_attachment.depthStoreOp = WGPUStoreOp_Store;
        ds_attachment.stencilLoadOp = WGPULoadOp_Load;
        ds_attachment.stencilStoreOp = WGPUStoreOp_Store;
        pass_desc.depthStencilAttachment = &ds_attachment;
    }

    float rgba[4] = { 0 };
    float depth = 1.0f;
    int stencil = 0;
    if (color) {
        pgraph_get_clear_color(pg, rgba);
    }
    if (zeta) {
        pgraph_get_clear_depth_stencil_value(pg, &depth, &stencil);
    }

    pgraph_wgpu_end_render_pass(r);
    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    WGPURenderPassEncoder pass =
        wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetViewport(pass, 0, 0, surface_width, surface_height,
                                     depth, depth);
    wgpuRenderPassEncoderSetScissorRect(pass, xmin, ymin, scissor_width,
                                        scissor_height);
    WGPUColor blend_constant = { rgba[0], rgba[1], rgba[2], rgba[3] };
    wgpuRenderPassEncoderSetBlendConstant(pass, &blend_constant);
    wgpuRenderPassEncoderSetStencilReference(pass, stencil & 0xff);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    pg->clearing = false;

    pgraph_wgpu_set_surface_dirty(pg, write_color, write_zeta);
}

static bool color_write_requested(PGRAPHState *pg)
{
    return pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
           (NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE |
            NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE |
            NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE |
            NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE);
}

static bool depth_or_stencil_test(PGRAPHState *pg)
{
    return (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
            NV_PGRAPH_CONTROL_0_ZENABLE) ||
           (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
            NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE);
}

void pgraph_wgpu_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    pgraph_wgpu_surface_update(d, true, true, depth_or_stencil_test(pg));
}

void pgraph_wgpu_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    bool color_write = color_write_requested(pg);
    bool zeta_test = depth_or_stencil_test(pg);
    if (!(color_write || zeta_test)) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        return;
    }

    pgraph_wgpu_flush_draw(d);

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_wgpu_set_surface_dirty(pg, color_write, zeta_test);
}

static const unsigned int pipeline_regs[] = {
    NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
    NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
    NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
};

static void init_pipeline_key(PGRAPHState *pg, WGPUPrimitiveTopology topology,
                              PipelineKey *key)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    QEMU_BUILD_BUG_ON(ARRAY_SIZE(pipeline_regs) != ARRAY_SIZE(key->regs));

    memset(key, 0, sizeof(*key));
    key->shader = r->shader_binding;
    key->color_format = r->color_binding ? r->color_binding->host_fmt.format :
                                           WGPUTextureFormat_Undefined;
    key->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.format :
                                         WGPUTextureFormat_Undefined;
    key->topology = topology;
    memcpy(&key->vertex_layout, &r->vertex_layout, sizeof(r->vertex_layout));
    for (int i = 0; i < ARRAY_SIZE(pipeline_regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, pipeline_regs[i]);
    }
}

static WGPURenderPipeline create_draw_pipeline(PGRAPHState *pg,
                                               const PipelineKey *key)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    const ShaderBinding *b = key->shader;

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

    const WgpuVertexLayout *vl = &key->vertex_layout;
    WGPUVertexAttribute attributes[NV2A_VERTEXSHADER_ATTRIBUTES];
    for (uint32_t i = 0; i < vl->num_attributes; i++) {
        attributes[i] = (WGPUVertexAttribute)WGPU_VERTEX_ATTRIBUTE_INIT;
        attributes[i].format = vl->attributes[i].format;
        attributes[i].offset = vl->attributes[i].offset;
        attributes[i].shaderLocation = vl->attributes[i].location;
    }
    WGPUVertexBufferLayout vertex_buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vertex_buffer.stepMode = WGPUVertexStepMode_Vertex;
    vertex_buffer.arrayStride = vl->stride;
    vertex_buffer.attributeCount = vl->num_attributes;
    vertex_buffer.attributes = attributes;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("PGRAPH draw");
    desc.layout = b->texture_layout->pipeline_layout;
    desc.vertex.module = b->vsh;
    desc.vertex.bufferCount = vl->num_attributes ? 1 : 0;
    desc.vertex.buffers = &vertex_buffer;

    uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);
    desc.primitive.topology = key->topology;
    desc.primitive.frontFace = (setupraster & NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                                   WGPUFrontFace_CCW :
                                   WGPUFrontFace_CW;
    desc.primitive.cullMode = WGPUCullMode_None;
    if (setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face =
            GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_wgpu_map));
        desc.primitive.cullMode = pgraph_cull_face_wgpu_map[cull_face];
    }
    desc.primitive.unclippedDepth = r->depth_clip_control;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    uint32_t control_1 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1);
    uint32_t control_2 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE;
    bool stencil_test = control_1 & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth_stencil.format = key->zeta_format;
    /* As in Vulkan, depth writes only happen when the depth test is on */
    depth_stencil.depthWriteEnabled = (depth_test && depth_write) ?
                                          WGPUOptionalBool_True :
                                          WGPUOptionalBool_False;
    depth_stencil.depthCompare = WGPUCompareFunction_Always;
    if (depth_test) {
        uint32_t depth_func = GET_MASK(control_0, NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_wgpu_map));
        depth_stencil.depthCompare = pgraph_depth_func_wgpu_map[depth_func];
    }

    WGPUStencilFaceState stencil_face = WGPU_STENCIL_FACE_STATE_INIT;
    stencil_face.compare = WGPUCompareFunction_Always;
    stencil_face.failOp = WGPUStencilOperation_Keep;
    stencil_face.depthFailOp = WGPUStencilOperation_Keep;
    stencil_face.passOp = WGPUStencilOperation_Keep;
    depth_stencil.stencilReadMask = 0xff;
    depth_stencil.stencilWriteMask = 0;
    if (stencil_test) {
        uint32_t stencil_func =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t op_fail =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass =
            GET_MASK(control_2, NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_wgpu_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_wgpu_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_wgpu_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_wgpu_map));

        stencil_face.compare = pgraph_stencil_func_wgpu_map[stencil_func];
        stencil_face.failOp = pgraph_stencil_op_wgpu_map[op_fail];
        stencil_face.depthFailOp = pgraph_stencil_op_wgpu_map[op_zfail];
        stencil_face.passOp = pgraph_stencil_op_wgpu_map[op_zpass];
        depth_stencil.stencilReadMask =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        depth_stencil.stencilWriteMask =
            GET_MASK(control_1, NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
    }
    depth_stencil.stencilFront = stencil_face;
    depth_stencil.stencilBack = stencil_face;
    WGPUDepthStencilState scratch_depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    if (key->zeta_format != WGPUTextureFormat_Undefined) {
        desc.depthStencil = &depth_stencil;
    } else {
        /* The fragment shader always writes depth, which WebGPU only allows
         * with a depth attachment, so draw against the scratch zeta */
        scratch_depth_stencil.format = SCRATCH_ZETA_FORMAT;
        scratch_depth_stencil.depthWriteEnabled = WGPUOptionalBool_False;
        scratch_depth_stencil.depthCompare = WGPUCompareFunction_Always;
        desc.depthStencil = &scratch_depth_stencil;
    }
    if (key->topology == WGPUPrimitiveTopology_TriangleStrip ||
        key->topology == WGPUPrimitiveTopology_LineStrip) {
        desc.primitive.stripIndexFormat = WGPUIndexFormat_Uint32;
    }

    WGPUColorTargetState color_target = WGPU_COLOR_TARGET_STATE_INIT;
    color_target.format = key->color_format;
    color_target.writeMask = WGPUColorWriteMask_None;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE) {
        color_target.writeMask |= WGPUColorWriteMask_Red;
    }
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE) {
        color_target.writeMask |= WGPUColorWriteMask_Green;
    }
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE) {
        color_target.writeMask |= WGPUColorWriteMask_Blue;
    }
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE) {
        color_target.writeMask |= WGPUColorWriteMask_Alpha;
    }

    WGPUBlendState blend;
    uint32_t blend_reg = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    if (blend_reg & NV_PGRAPH_BLEND_EN) {
        uint32_t sfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_DFACTOR);
        uint32_t equation = GET_MASK(blend_reg, NV_PGRAPH_BLEND_EQN);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_wgpu_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_wgpu_map));
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_wgpu_map));

        WGPUBlendComponent component = WGPU_BLEND_COMPONENT_INIT;
        component.operation = pgraph_blend_equation_wgpu_map[equation];
        component.srcFactor = pgraph_blend_factor_wgpu_map[sfactor];
        component.dstFactor = pgraph_blend_factor_wgpu_map[dfactor];
        if (component.operation == WGPUBlendOperation_Min ||
            component.operation == WGPUBlendOperation_Max) {
            /* Factors are ignored by min/max but WebGPU requires One */
            component.srcFactor = WGPUBlendFactor_One;
            component.dstFactor = WGPUBlendFactor_One;
        }
        blend.color = component;
        blend.alpha = component;
        color_target.blend = &blend;
    }

    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = b->psh;
    if (key->color_format != WGPUTextureFormat_Undefined) {
        fragment.targetCount = 1;
        fragment.targets = &color_target;
    }
    desc.fragment = &fragment;

    return wgpuDeviceCreateRenderPipeline(r->device, &desc);
}

static WGPURenderPipeline get_draw_pipeline(PGRAPHState *pg,
                                            WGPUPrimitiveTopology topology)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    PipelineKey key;

    init_pipeline_key(pg, topology, &key);
    WGPURenderPipeline pipeline = g_hash_table_lookup(r->pipeline_cache, &key);
    if (pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);
        return pipeline;
    }

    pipeline = create_draw_pipeline(pg, &key);
    g_hash_table_insert(r->pipeline_cache, g_memdup2(&key, sizeof(key)),
                        pipeline);
    return pipeline;
}

static WGPUTextureView get_scratch_zeta_view(PGRAPHWgpuState *r,
                                             unsigned int width,
                                             unsigned int height)
{
    if (r->scratch_zeta && wgpuTextureGetWidth(r->scratch_zeta) == width &&
        wgpuTextureGetHeight(r->scratch_zeta) == height) {
        return r->scratch_zeta_view;
    }
    if (r->scratch_zeta) {
        wgpuTextureViewRelease(r->scratch_zeta_view);
        wgpuTextureRelease(r->scratch_zeta);
    }

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Scratch zeta");
    desc.usage = WGPUTextureUsage_RenderAttachment;
    desc.size = (WGPUExtent3D){ width, height, 1 };
    desc.format = SCRATCH_ZETA_FORMAT;
    r->scratch_zeta = wgpuDeviceCreateTexture(r->device, &desc);
    r->scratch_zeta_view = wgpuTextureCreateView(r->scratch_zeta, NULL);
    return r->scratch_zeta_view;
}

static void ensure_draw_render_pass(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (r->render_pass && !r->framebuffer_dirty) {
        return;
    }
    pgraph_wgpu_end_render_pass(r);

    WGPURenderPassColorAttachment color_attachment =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDepthStencilAttachment ds_attachment =
        WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("PGRAPH draw");
    if (r->color_binding) {
        color_attachment.view = r->color_binding->view;
        color_attachment.loadOp = WGPULoadOp_Load;
        color_attachment.storeOp = WGPUStoreOp_Store;
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &color_attachment;
    }
    if (r->zeta_binding) {
        ds_attachment.view = r->zeta_binding->view;
        ds_attachment.depthLoadOp = WGPULoadOp_Load;
        ds_attachment.depthStoreOp = WGPUStoreOp_Store;
        ds_attachment.stencilLoadOp = WGPULoadOp_Load;
        ds_attachment.stencilStoreOp = WGPUStoreOp_Store;
        desc.depthStencilAttachment = &ds_attachment;
    } else {
        WGPUTexture color = r->color_binding->texture;
        ds_attachment.view = get_scratch_zeta_view(
            r, wgpuTextureGetWidth(color), wgpuTextureGetHeight(color));
        ds_attachment.depthLoadOp = WGPULoadOp_Clear;
        ds_attachment.depthStoreOp = WGPUStoreOp_Discard;
        ds_attachment.depthClearValue = 1.0f;
        desc.depthStencilAttachment = &ds_attachment;
    }

    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    r->render_pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    r->framebuffer_dirty = false;
}

static void ensure_uniform_bind_group(PGRAPHWgpuState *r)
{
    WGPUBuffer buffer = r->stream_buffers[WGPU_STREAM_UNIFORM].buffer;

    if (r->uniform_bind_group && r->uniform_bind_group_buffer == buffer) {
        return;
    }
    if (r->uniform_bind_group) {
        wgpuBindGroupRelease(r->uniform_bind_group);
    }

    WGPUBindGroupEntry entries[2] = {
        WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT,
    };
    for (int i = 0; i < ARRAY_SIZE(entries); i++) {
        entries[i].binding = i;
        entries[i].buffer = buffer;
        entries[i].size = r->uniform_binding_size;
    }
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Uniforms");
    desc.layout = r->uniform_bind_group_layout;
    desc.entryCount = ARRAY_SIZE(entries);
    desc.entries = entries;
    r->uniform_bind_group = wgpuDeviceCreateBindGroup(r->device, &desc);
    r->uniform_bind_group_buffer = buffer;
}

static void upload_uniforms(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    ShaderBinding *b = r->shader_binding;
    bool vsh_changed, psh_changed;
    void *ptr;

    pgraph_wgpu_update_uniforms(pg, &vsh_changed, &psh_changed);
    if (vsh_changed) {
        r->uniform_offsets[0] = pgraph_wgpu_stream_alloc(
            r, WGPU_STREAM_UNIFORM, r->uniform_binding_size, 256, &ptr);
        memcpy(ptr, r->vsh_uniform_data, b->vsh_ubo_size);
    }
    if (psh_changed) {
        r->uniform_offsets[1] = pgraph_wgpu_stream_alloc(
            r, WGPU_STREAM_UNIFORM, r->uniform_binding_size, 256, &ptr);
        memcpy(ptr, r->psh_uniform_data, b->psh_ubo_size);
    }
    r->uniforms_valid = true;
    ensure_uniform_bind_group(r);
}

static void set_draw_state(PGRAPHState *pg, WGPURenderPipeline pipeline,
                           size_t vertex_offset, size_t vertex_size)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WGPURenderPassEncoder pass = r->render_pass;
    SurfaceBinding *binding = r->color_binding ?: r->zeta_binding;

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, r->uniform_bind_group, 2,
                                      r->uniform_offsets);
    wgpuRenderPassEncoderSetBindGroup(
        pass, 1, r->shader_binding->texture_layout->dummy_bind_group, 0, NULL);
    if (vertex_size) {
        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 0, r->stream_buffers[WGPU_STREAM_VERTEX].buffer,
            vertex_offset, vertex_size);
    }

    unsigned int target_width = binding->width, target_height = binding->height;
    pgraph_apply_scaling_factor(pg, &target_width, &target_height);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    wgpuRenderPassEncoderSetViewport(pass, 0, 0, MIN(vp_width, target_width),
                                     MIN(vp_height, target_height), 0.0f, 1.0f);

    /* Surface clip */
    /* FIXME: Consider moving to PSH w/ window clip */
    unsigned int xmin = pg->surface_shape.clip_x,
                 ymin = pg->surface_shape.clip_y;
    unsigned int scissor_width = pg->surface_shape.clip_width,
                 scissor_height = pg->surface_shape.clip_height;
    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);
    /* WebGPU requires the scissor rect to be inside the attachments */
    xmin = MIN(xmin, target_width);
    ymin = MIN(ymin, target_height);
    scissor_width = MIN(scissor_width, target_width - xmin);
    scissor_height = MIN(scissor_height, target_height - ymin);
    wgpuRenderPassEncoderSetScissorRect(pass, xmin, ymin, scissor_width,
                                        scissor_height);

    float blend_constant[4];
    pgraph_argb_pack32_to_rgba_float(pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR),
                                     blend_constant);
    uint32_t blend_reg = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
    uint32_t sfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_SFACTOR);
    uint32_t dfactor = GET_MASK(blend_reg, NV_PGRAPH_BLEND_DFACTOR);
    if (sfactor == PGRAPH_BLEND_FACTOR_CONSTANT_ALPHA ||
        sfactor == PGRAPH_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA ||
        dfactor == PGRAPH_BLEND_FACTOR_CONSTANT_ALPHA ||
        dfactor == PGRAPH_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA) {
        blend_constant[0] = blend_constant[1] = blend_constant[2] =
            blend_constant[3];
    }
    WGPUColor color = { blend_constant[0], blend_constant[1], blend_constant[2],
                        blend_constant[3] };
    wgpuRenderPassEncoderSetBlendConstant(pass, &color);

    wgpuRenderPassEncoderSetStencilReference(
        pass, GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                       NV_PGRAPH_CONTROL_1_STENCIL_REF));
}

static bool culls_all_triangles(PGRAPHState *pg, WGPUPrimitiveTopology topology)
{
    uint32_t setupraster = pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER);

    return (topology == WGPUPrimitiveTopology_TriangleList ||
            topology == WGPUPrimitiveTopology_TriangleStrip) &&
           (setupraster & NV_PGRAPH_SETUPRASTER_CULLENABLE) &&
           GET_MASK(setupraster, NV_PGRAPH_SETUPRASTER_CULLCTRL) == 3;
}

void pgraph_wgpu_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return;
    }

    /* Vertices [first, first + num_vertices) are packed for the draw */
    unsigned int first = 0, num_vertices = 0;
    const uint32_t *elements = NULL;
    size_t num_elements = 0;

    if (pg->draw_arrays_length) {
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);
        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        first = pg->draw_arrays_min_start;
        num_vertices = pg->draw_arrays_max_count - first;
        pgraph_wgpu_bind_vertex_attributes(d, first,
                                           pg->draw_arrays_max_count - 1, false,
                                           0, pg->draw_arrays_max_count - 1);
    } else if (pg->inline_elements_length) {
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        first = min_element;
        num_vertices = max_element - min_element + 1;
        elements = pg->inline_elements;
        num_elements = pg->inline_elements_length;
        pgraph_wgpu_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            pg->inline_elements[pg->inline_elements_length - 1]);
    } else if (pg->inline_buffer_length) {
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        num_vertices = pg->inline_buffer_length;
        pgraph_wgpu_bind_vertex_attributes_inline(d);
    } else if (pg->inline_array_length) {
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        num_vertices = pg->inline_array_length * 4 / vertex_size;
        pgraph_wgpu_bind_vertex_attributes(d, 0, num_vertices - 1, true,
                                           vertex_size, num_vertices - 1);
    } else {
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
        return;
    }

    pgraph_wgpu_bind_shaders(pg);
    ShaderBinding *b = r->shader_binding;
    if (!b->valid) {
        return;
    }

    int primitive_mode = b->state.geom.primitive_mode;
    bool expand;
    WGPUPrimitiveTopology topology =
        pgraph_wgpu_prim_topology(primitive_mode, &expand);
    if (culls_all_triangles(pg, topology)) {
        return;
    }

    size_t max_indices = 0;
    if (expand && pg->draw_arrays_length) {
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            max_indices += pgraph_wgpu_prim_max_indices(
                primitive_mode, pg->draw_arrays_count[i]);
        }
    } else if (expand) {
        max_indices = pgraph_wgpu_prim_max_indices(
            primitive_mode, elements ? num_elements : num_vertices);
    } else if (elements) {
        max_indices = num_elements;
    }

    size_t vertex_size = (size_t)num_vertices * r->vertex_layout.stride;
    pgraph_wgpu_stream_reserve(pg, WGPU_STREAM_VERTEX, vertex_size);
    pgraph_wgpu_stream_reserve(pg, WGPU_STREAM_INDEX,
                               max_indices * sizeof(uint32_t));
    pgraph_wgpu_stream_reserve(pg, WGPU_STREAM_UNIFORM,
                               2 * (r->uniform_binding_size + 256));

    WGPURenderPipeline pipeline = get_draw_pipeline(pg, topology);

    upload_uniforms(pg);

    size_t vertex_offset = 0;
    if (vertex_size) {
        void *ptr;
        vertex_offset = pgraph_wgpu_stream_alloc(r, WGPU_STREAM_VERTEX,
                                                 vertex_size, 4, &ptr);
        pgraph_wgpu_pack_vertices(r, ptr, first, num_vertices);
    }
    if (pg->inline_buffer_length) {
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            pg->vertex_attributes[i].inline_buffer_populated = false;
        }
    }

    size_t index_offset = 0, num_indices = 0;
    if (max_indices) {
        uint32_t *indices;
        index_offset = pgraph_wgpu_stream_alloc(r, WGPU_STREAM_INDEX,
                                                max_indices * sizeof(uint32_t),
                                                4, (void **)&indices);
        if (expand && pg->draw_arrays_length) {
            for (int i = 0; i < pg->draw_arrays_length; i++) {
                num_indices += pgraph_wgpu_prim_expand(
                    primitive_mode, NULL, pg->draw_arrays_start[i] - first,
                    pg->draw_arrays_count[i], indices + num_indices);
            }
        } else if (expand) {
            num_indices = pgraph_wgpu_prim_expand(
                primitive_mode, elements, 0,
                elements ? num_elements : num_vertices, indices);
        } else {
            memcpy(indices, elements, num_elements * sizeof(uint32_t));
            num_indices = num_elements;
        }
    }

    ensure_draw_render_pass(pg);
    set_draw_state(pg, pipeline, vertex_offset, vertex_size);

    WGPURenderPassEncoder pass = r->render_pass;
    if (max_indices) {
        if (!num_indices) {
            return;
        }
        wgpuRenderPassEncoderSetIndexBuffer(
            pass, r->stream_buffers[WGPU_STREAM_INDEX].buffer,
            WGPUIndexFormat_Uint32, index_offset,
            num_indices * sizeof(uint32_t));
        /* Inline element indices are absolute, the packed vertices start at
         * first */
        int32_t base_vertex = elements ? -(int32_t)first : 0;
        wgpuRenderPassEncoderDrawIndexed(pass, num_indices, 1, 0, base_vertex,
                                         0);
    } else if (pg->draw_arrays_length) {
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            wgpuRenderPassEncoderDraw(pass, pg->draw_arrays_count[i], 1,
                                      pg->draw_arrays_start[i] - first, 0);
        }
    } else {
        wgpuRenderPassEncoderDraw(pass, num_vertices, 1, 0, 0);
    }
}
