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
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

static const char clear_wgsl[] =
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f "
    "{\n"
    "    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4f {\n"
    "    return vec4f(1.0);\n"
    "}\n";

void pgraph_wgpu_init_pipelines(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    r->clear_module = pgraph_wgpu_create_wgsl_module(r, "Clear", clear_wgsl);
    r->clear_pipelines = g_array_new(false, true, sizeof(ClearPipeline));
}

void pgraph_wgpu_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

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

void pgraph_wgpu_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    bool depth_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) &
                        NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    pgraph_wgpu_surface_update(d, true, true, depth_test || stencil_test);
}

void pgraph_wgpu_draw_end(NV2AState *d)
{
    /* Geometry is not rendered yet, so surfaces are not marked dirty. */
}

void pgraph_wgpu_flush_draw(NV2AState *d)
{
}
