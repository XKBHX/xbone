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

/*
 * WebGPU cannot render to packed 16-bit colour, or copy a depth aspect
 * to or from a buffer as Z16/Z24S8. These kernels convert on the GPU.
 * width_in/width_out carry the same nearest scale as vk/surface-compute.c
 * (scale is 1 until the surface scale factor is implemented). Swizzle
 * stays on the CPU, as it does in Vulkan.
 *
 * Z24 packing matches vk/surface-compute.c for D32_SFLOAT_S8 and the
 * fragment shader: pack truncates d * 0xffffff, unpack divides by 2^24
 * and adds one ULP. 16-bit colour matches the previous CPU expand/reduce.
 */

#define PARAMS               \
    "struct Params {\n"      \
    "    width_in: u32,\n"   \
    "    height_in: u32,\n"  \
    "    width_out: u32,\n"  \
    "    height_out: u32,\n" \
    "    src_stride: u32,\n" \
    "    dst_stride: u32,\n" \
    "    aux_stride: u32,\n" \
    "    pad: u32,\n"        \
    "}\n"

#define FUNCS                                                            \
    "fn pack_scale() -> u32 {\n"                                         \
    "    return max(params.width_in / max(params.width_out, 1u), 1u);\n" \
    "}\n"                                                                \
    "fn unpack_scale() -> u32 {\n"                                       \
    "    return max(params.width_out / max(params.width_in, 1u), 1u);\n" \
    "}\n"                                                                \
    "fn byte_of(v: f32) -> u32 { return u32(v * 255.0 + 0.5); }\n"       \
    "fn reduce_bits(b: u32, maxv: u32) -> u32 {\n"                       \
    "    return (b * maxv + 127u) / 255u;\n"                             \
    "}\n"                                                                \
    "fn expand_bits(v: u32, bits: u32) -> u32 {\n"                       \
    "    let maxv = (1u << bits) - 1u;\n"                                \
    "    return (v * 255u + maxv / 2u) / maxv;\n"                        \
    "}\n"

#define LOAD                                                                 \
    "fn load_u16(byte_off: u32) -> u32 {\n"                                  \
    "    let word = byte_off / 4u;\n"                                        \
    "    let shift = (byte_off % 4u) * 8u;\n"                                \
    "    if (shift <= 16u) { return (src[word] >> shift) & 0xffffu; }\n"     \
    "    return ((src[word] >> 24u) | (src[word + 1u] << 8u)) & 0xffffu;\n"  \
    "}\n"                                                                    \
    "fn load_u32(byte_off: u32) -> u32 {\n"                                  \
    "    let word = byte_off / 4u;\n"                                        \
    "    let shift = (byte_off % 4u) * 8u;\n"                                \
    "    if (shift == 0u) { return src[word]; }\n"                           \
    "    return (src[word] >> shift) | (src[word + 1u] << (32u - shift));\n" \
    "}\n"

#define MAIN_ROW                                            \
    "@compute @workgroup_size(64)\n"                        \
    "fn main(@builtin(global_invocation_id) id: vec3u) {\n" \
    "    let y_out = id.x;\n"                               \
    "    if (y_out >= params.height_out) { return; }\n"

enum {
    COMPUTE_PACK_Z24,
    COMPUTE_UNPACK_Z24,
    COMPUTE_PACK_Z16,
    COMPUTE_UNPACK_Z16,
    COMPUTE_PACK_R5G6B5,
    COMPUTE_UNPACK_R5G6B5,
    COMPUTE_PACK_X1R5G5B5,
    COMPUTE_UNPACK_X1R5G5B5,
    COMPUTE__COUNT,
};

typedef struct SurfaceComputeParams {
    uint32_t width_in;
    uint32_t height_in;
    uint32_t width_out;
    uint32_t height_out;
    uint32_t src_stride;
    uint32_t dst_stride;
    uint32_t aux_stride;
    uint32_t pad;
} SurfaceComputeParams;

static const char pack_z24_wgsl[] = PARAMS
    "@group(0) @binding(0) var depth_tex: texture_depth_2d;\n"
    "@group(0) @binding(1) var stencil_tex: texture_2d<u32>;\n"
    "@group(0) @binding(2) var<storage, read_write> dst: array<u32>;\n"
    "@group(0) @binding(3) var<uniform> params: Params;\n" FUNCS MAIN_ROW
    "    let scale = pack_scale();\n"
    "    let y = y_out * scale;\n"
    "    let stride = params.dst_stride / 4u;\n"
    "    for (var x_out = 0u; x_out < params.width_out; x_out++) {\n"
    "        let x = x_out * scale;\n"
    "        let d = textureLoad(depth_tex, vec2i(i32(x), i32(y)), 0);\n"
    "        let depth_value = u32(i32(d * 16777215.0));\n"
    "        let s = textureLoad(stencil_tex, vec2i(i32(x), i32(y)), 0).r;\n"
    "        dst[y_out * stride + x_out] = (depth_value << 8u) | (s & 0xffu);\n"
    "    }\n"
    "}\n";

static const char unpack_z24_wgsl[] = PARAMS
    "@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
    "@group(0) @binding(1) var<storage, read_write> depth_out: array<f32>;\n"
    "@group(0) @binding(2) var<storage, read_write> stencil_out: array<u32>;\n"
    "@group(0) @binding(3) var<uniform> params: Params;\n" FUNCS LOAD MAIN_ROW
    "    let scale = unpack_scale();\n"
    "    let y_in = y_out / scale;\n"
    "    let depth_stride = params.dst_stride / 4u;\n"
    "    var stencil_word = 0u;\n"
    "    for (var x_out = 0u; x_out < params.width_out; x_out++) {\n"
    "        let x_in = x_out / scale;\n"
    "        let v = load_u32(y_in * params.src_stride + x_in * 4u);\n"
    "        let z24 = v >> 8u;\n"
    "        var bits = bitcast<u32>(f32(z24) / 16777216.0);\n"
    "        bits += 1u;\n"
    "        depth_out[y_out * depth_stride + x_out] = bitcast<f32>(bits);\n"
    "        stencil_word |= (v & 0xffu) << ((x_out % 4u) * 8u);\n"
    "        if (x_out % 4u == 3u || x_out + 1u == params.width_out) {\n"
    "            let byte_off = y_out * params.aux_stride + (x_out & ~3u);\n"
    "            stencil_out[byte_off / 4u] = stencil_word;\n"
    "            stencil_word = 0u;\n"
    "        }\n"
    "    }\n"
    "}\n";

static const char pack_z16_wgsl[] = PARAMS
    "@group(0) @binding(0) var depth_tex: texture_depth_2d;\n"
    "@group(0) @binding(1) var<storage, read_write> dst: array<u32>;\n"
    "@group(0) @binding(3) var<uniform> params: Params;\n" FUNCS MAIN_ROW
    "    let scale = pack_scale();\n"
    "    let y = y_out * scale;\n"
    "    let stride = params.dst_stride / 4u;\n"
    "    var word = 0u;\n"
    "    for (var x_out = 0u; x_out < params.width_out; x_out++) {\n"
    "        let x = x_out * scale;\n"
    "        let d = clamp(textureLoad(depth_tex, vec2i(i32(x), i32(y)), 0),\n"
    "                      0.0, 1.0);\n"
    "        let z = u32(d * 65535.0 + 0.5);\n"
    "        if (x_out % 2u == 0u) {\n"
    "            word = z;\n"
    "            if (x_out + 1u == params.width_out) {\n"
    "                dst[y_out * stride + x_out / 2u] = word;\n"
    "            }\n"
    "        } else {\n"
    "            dst[y_out * stride + x_out / 2u] = word | (z << 16u);\n"
    "        }\n"
    "    }\n"
    "}\n";

static const char unpack_z16_wgsl[] = PARAMS
    "@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
    "@group(0) @binding(1) var<storage, read_write> depth_out: array<f32>;\n"
    "@group(0) @binding(3) var<uniform> params: Params;\n" FUNCS LOAD MAIN_ROW
    "    let scale = unpack_scale();\n"
    "    let y_in = y_out / scale;\n"
    "    let stride = params.dst_stride / 4u;\n"
    "    for (var x_out = 0u; x_out < params.width_out; x_out++) {\n"
    "        let x_in = x_out / scale;\n"
    "        let z = load_u16(y_in * params.src_stride + x_in * 2u);\n"
    "        depth_out[y_out * stride + x_out] = f32(z) / 65535.0;\n"
    "    }\n"
    "}\n";

#define PACK_COLOR(expr)                                                    \
    PARAMS                                                                  \
    "@group(0) @binding(0) var color_tex: texture_2d<f32>;\n"               \
    "@group(0) @binding(1) var<storage, read_write> dst: array<u32>;\n"     \
    "@group(0) @binding(3) var<uniform> params: Params;\n" FUNCS MAIN_ROW   \
    "    let scale = pack_scale();\n"                                       \
    "    let y = y_out * scale;\n"                                          \
    "    let stride = params.dst_stride / 4u;\n"                            \
    "    var word = 0u;\n"                                                  \
    "    for (var x_out = 0u; x_out < params.width_out; x_out++) {\n"       \
    "        let x = x_out * scale;\n"                                      \
    "        let c = textureLoad(color_tex, vec2i(i32(x), i32(y)), 0);\n"   \
    "        let pix = " expr ";\n"                                         \
    "        if (x_out % 2u == 0u) {\n"                                     \
    "            word = pix;\n"                                             \
    "            if (x_out + 1u == params.width_out) {\n"                   \
    "                dst[y_out * stride + x_out / 2u] = word;\n"            \
    "            }\n"                                                       \
    "        } else {\n"                                                    \
    "            dst[y_out * stride + x_out / 2u] = word | (pix << 16u);\n" \
    "        }\n"                                                           \
    "    }\n"                                                               \
    "}\n"

static const char pack_r5g6b5_wgsl[] =
    PACK_COLOR("(reduce_bits(byte_of(c.r), 31u) << 11u) |"
               "(reduce_bits(byte_of(c.g), 63u) << 5u) |"
               "reduce_bits(byte_of(c.b), 31u)");

static const char pack_x1r5g5b5_wgsl[] =
    PACK_COLOR("select(0u, 0x8000u, byte_of(c.a) >= 128u) |"
               "(reduce_bits(byte_of(c.r), 31u) << 10u) |"
               "(reduce_bits(byte_of(c.g), 31u) << 5u) |"
               "reduce_bits(byte_of(c.b), 31u)");

#define UNPACK_COLOR(expr)                                                     \
    PARAMS                                                                     \
    "@group(0) @binding(0) var<storage, read> src: array<u32>;\n"              \
    "@group(0) @binding(1) var<storage, read_write> dst: array<u32>;\n"        \
    "@group(0) @binding(3) var<uniform> params: Params;\n" FUNCS LOAD MAIN_ROW \
    "    let scale = unpack_scale();\n"                                        \
    "    let y_in = y_out / scale;\n"                                          \
    "    let stride = params.dst_stride / 4u;\n"                               \
    "    for (var x_out = 0u; x_out < params.width_out; x_out++) {\n"          \
    "        let x_in = x_out / scale;\n"                                      \
    "        let pix = load_u16(y_in * params.src_stride + x_in * 2u);\n"      \
    "        dst[y_out * stride + x_out] = " expr ";\n"                        \
    "    }\n"                                                                  \
    "}\n"

static const char unpack_r5g6b5_wgsl[] =
    UNPACK_COLOR("expand_bits((pix >> 11u) & 31u, 5u) |"
                 "(expand_bits((pix >> 5u) & 63u, 6u) << 8u) |"
                 "(expand_bits(pix & 31u, 5u) << 16u) |"
                 "(255u << 24u)");

static const char unpack_x1r5g5b5_wgsl[] =
    UNPACK_COLOR("expand_bits((pix >> 10u) & 31u, 5u) |"
                 "(expand_bits((pix >> 5u) & 31u, 5u) << 8u) |"
                 "(expand_bits(pix & 31u, 5u) << 16u) |"
                 "(select(0u, 255u, (pix & 0x8000u) != 0u) << 24u)");

static const char *const compute_shader_source[COMPUTE__COUNT] = {
    [COMPUTE_PACK_Z24] = pack_z24_wgsl,
    [COMPUTE_UNPACK_Z24] = unpack_z24_wgsl,
    [COMPUTE_PACK_Z16] = pack_z16_wgsl,
    [COMPUTE_UNPACK_Z16] = unpack_z16_wgsl,
    [COMPUTE_PACK_R5G6B5] = pack_r5g6b5_wgsl,
    [COMPUTE_UNPACK_R5G6B5] = unpack_r5g6b5_wgsl,
    [COMPUTE_PACK_X1R5G5B5] = pack_x1r5g5b5_wgsl,
    [COMPUTE_UNPACK_X1R5G5B5] = unpack_x1r5g5b5_wgsl,
};

static int compute_op(const SurfaceBinding *surface, bool pack)
{
    switch (surface->host_fmt.conversion) {
    case WGPU_SURFACE_CONV_Z24S8:
        return pack ? COMPUTE_PACK_Z24 : COMPUTE_UNPACK_Z24;
    case WGPU_SURFACE_CONV_Z16:
        return pack ? COMPUTE_PACK_Z16 : COMPUTE_UNPACK_Z16;
    case WGPU_SURFACE_CONV_R5G6B5:
        return pack ? COMPUTE_PACK_R5G6B5 : COMPUTE_UNPACK_R5G6B5;
    case WGPU_SURFACE_CONV_X1R5G5B5:
        return pack ? COMPUTE_PACK_X1R5G5B5 : COMPUTE_UNPACK_X1R5G5B5;
    default:
        assert(!"Surface conversion has no compute shader");
        return 0;
    }
}

static WGPUBindGroupLayoutEntry buffer_entry(uint32_t binding,
                                             WGPUBufferBindingType type)
{
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Compute;
    entry.buffer.type = type;
    return entry;
}

static WGPUBindGroupLayoutEntry texture_entry(uint32_t binding,
                                              WGPUTextureSampleType sample)
{
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Compute;
    entry.texture.sampleType = sample;
    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    return entry;
}

static void fill_layout_entries(int op, WGPUBindGroupLayoutEntry *entries,
                                uint32_t *count)
{
    uint32_t n = 0;

    switch (op) {
    case COMPUTE_PACK_Z24:
        entries[n++] = texture_entry(0, WGPUTextureSampleType_Depth);
        entries[n++] = texture_entry(1, WGPUTextureSampleType_Uint);
        entries[n++] = buffer_entry(2, WGPUBufferBindingType_Storage);
        break;
    case COMPUTE_UNPACK_Z24:
        entries[n++] = buffer_entry(0, WGPUBufferBindingType_ReadOnlyStorage);
        entries[n++] = buffer_entry(1, WGPUBufferBindingType_Storage);
        entries[n++] = buffer_entry(2, WGPUBufferBindingType_Storage);
        break;
    case COMPUTE_PACK_Z16:
    case COMPUTE_PACK_R5G6B5:
    case COMPUTE_PACK_X1R5G5B5:
        entries[n++] = texture_entry(0, op == COMPUTE_PACK_Z16 ?
                                            WGPUTextureSampleType_Depth :
                                            WGPUTextureSampleType_Float);
        entries[n++] = buffer_entry(1, WGPUBufferBindingType_Storage);
        break;
    case COMPUTE_UNPACK_Z16:
        entries[n++] = buffer_entry(0, WGPUBufferBindingType_ReadOnlyStorage);
        entries[n++] = buffer_entry(1, WGPUBufferBindingType_Storage);
        break;
    case COMPUTE_UNPACK_R5G6B5:
    case COMPUTE_UNPACK_X1R5G5B5:
        entries[n++] = buffer_entry(0, WGPUBufferBindingType_ReadOnlyStorage);
        entries[n++] = buffer_entry(1, WGPUBufferBindingType_Storage);
        break;
    default:
        assert(false);
    }
    entries[n++] = buffer_entry(3, WGPUBufferBindingType_Uniform);
    *count = n;
}

void pgraph_wgpu_init_surface_compute(PGRAPHWgpuState *r)
{
    G_STATIC_ASSERT(
        COMPUTE__COUNT ==
        ARRAY_SIZE(((PGRAPHWgpuState *)NULL)->surface_compute_pipelines));
    for (int op = 0; op < COMPUTE__COUNT; op++) {
        r->surface_compute_modules[op] = pgraph_wgpu_create_wgsl_module(
            r, "Surface compute", compute_shader_source[op]);

        WGPUBindGroupLayoutEntry entries[4];
        uint32_t entry_count = 0;
        fill_layout_entries(op, entries, &entry_count);

        WGPUBindGroupLayoutDescriptor bgl_desc =
            WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bgl_desc.entryCount = entry_count;
        bgl_desc.entries = entries;
        r->surface_compute_layouts[op] =
            wgpuDeviceCreateBindGroupLayout(r->device, &bgl_desc);

        WGPUPipelineLayoutDescriptor layout_desc =
            WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        layout_desc.bindGroupLayoutCount = 1;
        layout_desc.bindGroupLayouts = &r->surface_compute_layouts[op];
        r->surface_compute_pipeline_layouts[op] =
            wgpuDeviceCreatePipelineLayout(r->device, &layout_desc);

        WGPUComputePipelineDescriptor desc =
            WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        desc.label = pgraph_wgpu_str("Surface compute");
        desc.layout = r->surface_compute_pipeline_layouts[op];
        desc.compute.module = r->surface_compute_modules[op];
        desc.compute.entryPoint = pgraph_wgpu_str("main");
        r->surface_compute_pipelines[op] =
            wgpuDeviceCreateComputePipeline(r->device, &desc);
    }

    WGPUBufferDescriptor uniform_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    uniform_desc.label = pgraph_wgpu_str("Surface compute params");
    uniform_desc.size = sizeof(SurfaceComputeParams);
    uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    r->surface_compute_uniform =
        wgpuDeviceCreateBuffer(r->device, &uniform_desc);
}

void pgraph_wgpu_finalize_surface_compute(PGRAPHWgpuState *r)
{
    for (int op = 0; op < COMPUTE__COUNT; op++) {
        if (r->surface_compute_pipelines[op]) {
            wgpuComputePipelineRelease(r->surface_compute_pipelines[op]);
            r->surface_compute_pipelines[op] = NULL;
        }
        if (r->surface_compute_pipeline_layouts[op]) {
            wgpuPipelineLayoutRelease(r->surface_compute_pipeline_layouts[op]);
            r->surface_compute_pipeline_layouts[op] = NULL;
        }
        if (r->surface_compute_layouts[op]) {
            wgpuBindGroupLayoutRelease(r->surface_compute_layouts[op]);
            r->surface_compute_layouts[op] = NULL;
        }
        if (r->surface_compute_modules[op]) {
            wgpuShaderModuleRelease(r->surface_compute_modules[op]);
            r->surface_compute_modules[op] = NULL;
        }
    }
    if (r->surface_compute_uniform) {
        wgpuBufferRelease(r->surface_compute_uniform);
        r->surface_compute_uniform = NULL;
    }
    for (int i = 0; i < 3; i++) {
        if (r->surface_compute_storage[i]) {
            wgpuBufferRelease(r->surface_compute_storage[i]);
            r->surface_compute_storage[i] = NULL;
            r->surface_compute_storage_size[i] = 0;
        }
    }
    if (r->surface_compute_readback) {
        wgpuBufferRelease(r->surface_compute_readback);
        r->surface_compute_readback = NULL;
        r->surface_compute_readback_size = 0;
    }
}

static WGPUBuffer ensure_buffer(PGRAPHWgpuState *r, int index, uint64_t size,
                                WGPUBufferUsage usage, const char *label)
{
    size = ROUND_UP(size, 4);
    if (r->surface_compute_storage[index] &&
        r->surface_compute_storage_size[index] >= size) {
        return r->surface_compute_storage[index];
    }
    if (r->surface_compute_storage[index]) {
        wgpuBufferRelease(r->surface_compute_storage[index]);
    }

    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str(label);
    desc.size = ROUND_UP(size, 1024 * 1024);
    desc.usage = usage;
    r->surface_compute_storage[index] =
        wgpuDeviceCreateBuffer(r->device, &desc);
    r->surface_compute_storage_size[index] = desc.size;
    return r->surface_compute_storage[index];
}

static WGPUBuffer ensure_readback(PGRAPHWgpuState *r, uint64_t size)
{
    if (r->surface_compute_readback &&
        r->surface_compute_readback_size >= size) {
        return r->surface_compute_readback;
    }
    if (r->surface_compute_readback) {
        wgpuBufferRelease(r->surface_compute_readback);
    }

    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Surface compute readback");
    desc.size = ROUND_UP(size, 1024 * 1024);
    desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    r->surface_compute_readback = wgpuDeviceCreateBuffer(r->device, &desc);
    r->surface_compute_readback_size = desc.size;
    return r->surface_compute_readback;
}

static WGPUTextureView aspect_view(WGPUTexture texture,
                                   WGPUTextureFormat format,
                                   WGPUTextureAspect aspect)
{
    WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;

    desc.format = format;
    desc.dimension = WGPUTextureViewDimension_2D;
    desc.aspect = aspect;
    desc.usage = WGPUTextureUsage_TextureBinding;
    return wgpuTextureCreateView(texture, &desc);
}

static void dispatch_compute(PGRAPHState *pg, int op,
                             const SurfaceComputeParams *params,
                             const WGPUBindGroupEntry *extra, uint32_t nextra,
                             WgpuFinishReason why)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;

    /* Queue writes land before the next submit. Flush anything still in the
     * encoder first, so it runs with the params it recorded against.
     */
    pgraph_wgpu_finish(pg, why, false);

    wgpuQueueWriteBuffer(r->queue, r->surface_compute_uniform, 0, params,
                         sizeof(*params));

    WGPUBindGroupEntry entries[4];
    memcpy(entries, extra, nextra * sizeof(entries[0]));
    entries[nextra] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[nextra].binding = 3;
    entries[nextra].buffer = r->surface_compute_uniform;
    entries[nextra].size = sizeof(*params);

    WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg_desc.layout = r->surface_compute_layouts[op];
    bg_desc.entryCount = nextra + 1;
    bg_desc.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(r->device, &bg_desc);

    pgraph_wgpu_end_render_pass(r);
    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    WGPUComputePassDescriptor pass_desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    pass_desc.label = pgraph_wgpu_str("Surface compute");
    WGPUComputePassEncoder pass =
        wgpuCommandEncoderBeginComputePass(encoder, &pass_desc);
    wgpuComputePassEncoderSetPipeline(pass, r->surface_compute_pipelines[op]);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (params->height_out + 63) / 64, 1, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

static void scaled_size(PGRAPHState *pg, const SurfaceBinding *surface,
                        unsigned int *width, unsigned int *height)
{
    *width = surface->width;
    *height = surface->height;
    pgraph_apply_scaling_factor(pg, width, height);
}

static void copy_storage_to_texture(WGPUCommandEncoder encoder,
                                    WGPUBuffer buffer, uint32_t bytes_per_row,
                                    WGPUTexture texture,
                                    WGPUTextureAspect aspect, uint32_t width,
                                    uint32_t height)
{
    WGPUTexelCopyBufferInfo src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    src.buffer = buffer;
    src.layout.bytesPerRow = bytes_per_row;
    src.layout.rowsPerImage = height;

    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = texture;
    dst.aspect = aspect;

    WGPUExtent3D extent = { width, height, 1 };
    wgpuCommandEncoderCopyBufferToTexture(encoder, &src, &dst, &extent);
}

void pgraph_wgpu_pack_converted_surface(PGRAPHState *pg,
                                        SurfaceBinding *surface, uint8_t *dst,
                                        unsigned int pitch)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    int op = compute_op(surface, true);
    unsigned int scaled_w, scaled_h;
    scaled_size(pg, surface, &scaled_w, &scaled_h);

    uint32_t bpp = surface->fmt.bytes_per_pixel;
    uint32_t dst_stride = ROUND_UP(surface->width * bpp, 4);
    uint64_t dst_bytes = (uint64_t)dst_stride * surface->height;

    SurfaceComputeParams params = {
        .width_in = scaled_w,
        .height_in = scaled_h,
        .width_out = surface->width,
        .height_out = surface->height,
        .dst_stride = dst_stride,
    };

    WGPUBuffer out = ensure_buffer(
        r, 1, dst_bytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
        "Surface pack output");
    WGPUBindGroupEntry entries[4];
    uint32_t nentries = 0;
    WGPUTextureView depth_view = NULL;
    WGPUTextureView stencil_view = NULL;
    WGPUTextureView color_view = NULL;

    if (op == COMPUTE_PACK_Z24 || op == COMPUTE_PACK_Z16) {
        depth_view =
            aspect_view(surface->texture, WGPUTextureFormat_Depth32Float,
                        WGPUTextureAspect_DepthOnly);
        entries[nentries] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[nentries].binding = 0;
        entries[nentries].textureView = depth_view;
        nentries++;
    }
    if (op == COMPUTE_PACK_Z24) {
        stencil_view = aspect_view(surface->texture, WGPUTextureFormat_Stencil8,
                                   WGPUTextureAspect_StencilOnly);
        entries[nentries] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[nentries].binding = 1;
        entries[nentries].textureView = stencil_view;
        nentries++;
    }
    if (op == COMPUTE_PACK_R5G6B5 || op == COMPUTE_PACK_X1R5G5B5) {
        color_view = wgpuTextureCreateView(surface->texture, NULL);
        entries[nentries] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[nentries].binding = 0;
        entries[nentries].textureView = color_view;
        nentries++;
    }

    entries[nentries] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[nentries].binding = op == COMPUTE_PACK_Z24 ? 2 : 1;
    entries[nentries].buffer = out;
    entries[nentries].size = ROUND_UP(dst_bytes, 4);
    nentries++;

    dispatch_compute(pg, op, &params, entries, nentries,
                     WGPU_FINISH_REASON_SURFACE_DOWN);

    WGPUBuffer readback = ensure_readback(r, dst_bytes);
    wgpuCommandEncoderCopyBufferToBuffer(pgraph_wgpu_ensure_encoder(pg), out, 0,
                                         readback, 0, ROUND_UP(dst_bytes, 4));
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_DOWN, false);

    const uint8_t *mapped = pgraph_wgpu_read_buffer(r, readback, dst_bytes);
    if (mapped) {
        for (unsigned int y = 0; y < surface->height; y++) {
            memcpy(dst + (size_t)y * pitch, mapped + (uint64_t)y * dst_stride,
                   (size_t)surface->width * bpp);
        }
        wgpuBufferUnmap(readback);
    }

    if (depth_view) {
        wgpuTextureViewRelease(depth_view);
    }
    if (stencil_view) {
        wgpuTextureViewRelease(stencil_view);
    }
    if (color_view) {
        wgpuTextureViewRelease(color_view);
    }
}

static void queue_write_source(PGRAPHWgpuState *r, WGPUBuffer buffer,
                               const uint8_t *src, uint64_t bytes)
{
    uint64_t padded = ROUND_UP(bytes, 4);
    g_autofree uint8_t *tmp = g_malloc0(padded);
    memcpy(tmp, src, bytes);
    wgpuQueueWriteBuffer(r->queue, buffer, 0, tmp, padded);
}

static void draw_uploaded_depth(PGRAPHState *pg, SurfaceBinding *surface,
                                WGPUTexture staging)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WGPUTextureView staging_view = wgpuTextureCreateView(staging, NULL);

    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = 0;
    entry.textureView = staging_view;
    WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg_desc.layout = r->depth_upload_bind_group_layout;
    bg_desc.entryCount = 1;
    bg_desc.entries = &entry;
    WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(r->device, &bg_desc);

    WGPURenderPassDepthStencilAttachment ds =
        WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    ds.view = surface->view;
    ds.depthLoadOp = WGPULoadOp_Load;
    ds.depthStoreOp = WGPUStoreOp_Store;
    ds.stencilLoadOp = WGPULoadOp_Load;
    ds.stencilStoreOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.label = pgraph_wgpu_str("Depth upload");
    pass_desc.depthStencilAttachment = &ds;

    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    WGPURenderPassEncoder pass =
        wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    wgpuRenderPassEncoderSetPipeline(pass, r->depth_upload_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(bind_group);
    wgpuTextureViewRelease(staging_view);
}

void pgraph_wgpu_unpack_converted_surface(PGRAPHState *pg,
                                          SurfaceBinding *surface,
                                          const uint8_t *src,
                                          unsigned int pitch)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    int op = compute_op(surface, false);
    unsigned int scaled_w, scaled_h;
    scaled_size(pg, surface, &scaled_w, &scaled_h);

    uint64_t src_bytes = (uint64_t)pitch * surface->height;
    WGPUBuffer source = ensure_buffer(
        r, 0, src_bytes + 4, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
        "Surface unpack source");
    queue_write_source(r, source, src, src_bytes);

    bool depth = surface->host_fmt.depth;
    uint32_t dst_stride = ROUND_UP(scaled_w * 4, 256);
    uint32_t stencil_stride =
        depth && op == COMPUTE_UNPACK_Z24 ? ROUND_UP(scaled_w, 256) : 0;
    uint64_t dst_bytes = (uint64_t)dst_stride * scaled_h;

    SurfaceComputeParams params = {
        .width_in = surface->width,
        .height_in = surface->height,
        .width_out = scaled_w,
        .height_out = scaled_h,
        .src_stride = pitch,
        .dst_stride = dst_stride,
        .aux_stride = stencil_stride,
    };

    WGPUBuffer unpacked = ensure_buffer(
        r, 1, dst_bytes, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
        "Surface unpack output");
    WGPUBuffer stencil = NULL;
    if (stencil_stride) {
        stencil =
            ensure_buffer(r, 2, (uint64_t)stencil_stride * scaled_h,
                          WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
                          "Surface unpack stencil");
    }

    WGPUBindGroupEntry entries[3];
    uint32_t nentries = 0;
    entries[nentries] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[nentries].binding = 0;
    entries[nentries].buffer = source;
    entries[nentries].size = ROUND_UP(src_bytes + 4, 4);
    nentries++;
    entries[nentries] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[nentries].binding = 1;
    entries[nentries].buffer = unpacked;
    entries[nentries].size = dst_bytes;
    nentries++;
    if (stencil) {
        entries[nentries] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[nentries].binding = 2;
        entries[nentries].buffer = stencil;
        entries[nentries].size = (uint64_t)stencil_stride * scaled_h;
        nentries++;
    }

    dispatch_compute(pg, op, &params, entries, nentries,
                     WGPU_FINISH_REASON_SURFACE_CREATE);

    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    if (depth) {
        WGPUTextureDescriptor staging_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        staging_desc.label = pgraph_wgpu_str("Depth upload staging");
        staging_desc.usage =
            WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        staging_desc.dimension = WGPUTextureDimension_2D;
        staging_desc.size = (WGPUExtent3D){ scaled_w, scaled_h, 1 };
        staging_desc.format = WGPUTextureFormat_R32Float;
        WGPUTexture staging = wgpuDeviceCreateTexture(r->device, &staging_desc);

        copy_storage_to_texture(encoder, unpacked, dst_stride, staging,
                                WGPUTextureAspect_All, scaled_w, scaled_h);
        if (stencil) {
            copy_storage_to_texture(
                encoder, stencil, stencil_stride, surface->texture,
                WGPUTextureAspect_StencilOnly, scaled_w, scaled_h);
        }
        draw_uploaded_depth(pg, surface, staging);
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_SURFACE_CREATE, false);
        wgpuTextureRelease(staging);
    } else {
        copy_storage_to_texture(encoder, unpacked, dst_stride, surface->texture,
                                WGPUTextureAspect_All, scaled_w, scaled_h);
    }
}
