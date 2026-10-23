/*
 * Test NV2A WebGPU GLSL-to-WGSL translation
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/wgsl/translate.h"

static char *translate(const char *glsl, bool vertex)
{
    g_autoptr(MString) wgsl = pgraph_wgsl_translate(glsl, vertex);
    if (!wgsl) {
        return NULL;
    }
    return g_strdup(mstring_get_str(wgsl));
}

static void assert_has(const char *wgsl, const char *needle)
{
    if (!strstr(wgsl, needle)) {
        g_test_message("missing '%s' in:\n%s", needle, wgsl);
        g_test_fail();
    }
}

static void assert_lacks(const char *wgsl, const char *needle)
{
    if (strstr(wgsl, needle)) {
        g_test_message("unexpected '%s' in:\n%s", needle, wgsl);
        g_test_fail();
    }
}

static void test_vertex(void)
{
    const char *glsl =
        "#version 450\n"
        "bool xIsNan(float x) {\n"
        "  return (floatBitsToUint(x) & 0x7FFFFFFFu) > 0x7F800000u;\n"
        "}\n"
        "#define isnan xIsNan\n"
        "#define INFINITY uintBitsToFloat(0x7F800000u + uint(gl_VertexIndex == "
        "-1))\n"
        "#ifndef INFINITY\n"
        "#define INFINITY (1.0 / 0.0)\n"
        "#endif\n"
        "layout(set = 0, binding = 0, std140) uniform VshUniforms {\n"
        "  vec4 c[4];\n"
        "  vec2 surfaceSize;\n"
        "};\n"
        "#define fogPlane c[3]\n"
        "#define normal v2.xyz\n"
        "float clampAwayZeroInf(float t) {\n"
        "  if (t > 0.0 || floatBitsToUint(t) == 0) {\n"
        "    t = clamp(t, uintBitsToFloat(0x1F800000), "
        "uintBitsToFloat(0x5F800000));\n"
        "  } else {\n"
        "    t = clamp(t, uintBitsToFloat(0xDF800000), "
        "uintBitsToFloat(0x9F800000));\n"
        "  }\n"
        "  return t;\n"
        "}\n"
        "vec4 _in(float v) { return vec4(v); }\n"
        "vec4 _in(vec4 v) { return v.xyzw; }\n"
        "#define MOV(dest, mask, src) dest.mask = _MOV(_in(src)).mask\n"
        "vec4 _MOV(vec4 src) { return src; }\n"
        "vec4 _RSQ(float src) {\n"
        "  if (src == 0.0) { return vec4(INFINITY); }\n"
        "  return vec4(inversesqrt(abs(src)));\n"
        "}\n"
        "vec4 oPos = vec4(0.0,0.0,0.0,1.0);\n"
        "layout(location = 0) out vec4 vtxD0;\n"
        "layout(location = 1) noperspective out vec4 vtxPos0;\n"
        "layout(location = 0) in vec4 v0;\n"
        "layout(location = 2) in vec4 v2;\n"
        "void main() {\n"
        "  MOV(oPos, xyzw, v0);\n"
        "  oPos += fogPlane;\n"
        "  oPos.xyz += normal;\n"
        "  oPos.w = clampAwayZeroInf(oPos.w);\n"
        "  vec4 r = _RSQ(oPos.x);\n"
        "  int ptIndex = gl_VertexIndex;\n"
        "  vec2 ptCorner = vec2(ptIndex & 1, ptIndex >> 1);\n"
        "  vtxD0 = oPos;\n"
        "  vtxPos0 = oPos;\n"
        "  gl_Position = vec4(oPos.x, -oPos.y, oPos.z, oPos.w);\n"
        "}\n";

    g_autofree char *wgsl = translate(glsl, true);
    g_assert_nonnull(wgsl);
    assert_has(wgsl, "@vertex");
    assert_has(wgsl, "fn vs(");
    assert_has(wgsl, "@location(0) v0: vec4f");
    assert_has(wgsl, "@builtin(vertex_index) vertex_index: u32");
    assert_has(wgsl, "@builtin(position) position: vec4f");
    assert_has(wgsl, "@location(1) @interpolate(linear) vtxPos0: vec4f");
    assert_has(wgsl, "struct VshUniforms");
    assert_has(wgsl, "c: array<vec4f, 4>");
    assert_has(wgsl, "@group(0) @binding(0) var<uniform> u_blk: VshUniforms");
    assert_has(wgsl, "u_blk.c[3]");
    assert_has(wgsl, "v2.xyz");
    assert_has(wgsl, "_MOV(_in_vec4f(v0)).xyzw");
    assert_has(wgsl,
               "oPos = vec4f(_swz_new.x, _swz_new.y, _swz_new.z, _swz_new.w)");
    assert_has(wgsl, "let _swz_new = (oPos.xyz + v2.xyz)");
    assert_has(wgsl,
               "oPos = vec4f(_swz_new.x, _swz_new.y, _swz_new.z, oPos.w)");
    assert_has(wgsl, "oPos = vec4f(oPos.x, oPos.y, oPos.z, _swz_new)");
    assert_lacks(wgsl, "oPos.xyzw =");
    assert_lacks(wgsl, "oPos.xyz +=");
    assert_lacks(wgsl, "oPos.w =");
    assert_has(wgsl, "fn _in(");
    assert_has(wgsl, "fn _in_vec4f(");
    assert_has(wgsl, "0xDF800000u");
    assert_has(wgsl, "inverseSqrt");
    assert_has(wgsl, "i32(vertex_index)");
    assert_has(wgsl, "var<private> gl_VertexIndex: i32");
    assert_has(wgsl, "(-oPos.y)");
    assert_has(wgsl, "gl_Position =");
    assert_has(wgsl, "_out.position = gl_Position");
    assert_has(wgsl, "bitcast<u32>");
    assert_lacks(wgsl, "1.0 / 0.0");
    assert_lacks(wgsl, "#version");
    assert_lacks(wgsl, "diagnostic(");
}

static void test_fragment(void)
{
    const char *glsl =
        "#version 450\n"
        "#define INFINITY uintBitsToFloat(0x7F800000u + uint(gl_VertexIndex == "
        "-1))\n"
        "#define isnan xIsNan\n"
        "bool xIsNan(float x) {\n"
        "  return (floatBitsToUint(x) & 0x7FFFFFFFu) > 0x7F800000u;\n"
        "}\n"
        "bvec4 xIsNan(vec4 x) {\n"
        "  return greaterThan(floatBitsToUint(x) & 0x7FFFFFFFu, "
        "uvec4(0x7F800000u));\n"
        "}\n"
        "vec4 NaNToOne(vec4 src) {\n"
        "  return mix(src, vec4(1.0), isnan(src));\n"
        "}\n"
        "bool same(vec4 a, vec4 b) { return a == b; }\n"
        "layout(location = 0) flat in vec4 vtxD0;\n"
        "layout(location = 1) noperspective in vec4 vtxPos0;\n"
        "layout(location = 0) out vec4 fragColor;\n"
        "layout(set = 0, binding = 1, std140) uniform PshUniforms {\n"
        "  int alphaRef;\n"
        "  mat2 bumpMat[4];\n"
        "  ivec2 surfaceScale;\n"
        "  float texLodBias[4];\n"
        "  vec4 clipRegion[8];\n"
        "};\n"
        "#define c0_0 consts[0]\n"
        "vec4 _LIT(vec4 src) {\n"
        "  vec4 s = src;\n"
        "  vec4 t = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "  t.z = (s.x > 0.0) ? exp2(s.w * log2(s.y)) : 0.0;\n"
        "  return t;\n"
        "}\n"
        "const float[9] gaussian3x3 = float[9](\n"
        "  1.0/16.0, 2.0/16.0, 1.0/16.0,\n"
        "  2.0/16.0, 4.0/16.0, 2.0/16.0,\n"
        "  1.0/16.0, 2.0/16.0, 1.0/16.0);\n"
        "precise float kahan(vec2 a, vec2 b) {\n"
        "  precise float cd = a.y*b.x;\n"
        "  return cd;\n"
        "}\n"
        "layout(set = 1, binding = 0) uniform texture2D texSamp0_tex;\n"
        "layout(set = 1, binding = 1) uniform sampler texSamp0_smp;\n"
        "#define texSamp0 sampler2D(texSamp0_tex, texSamp0_smp)\n"
        "layout(set = 1, binding = 2) uniform utexture2D texSamp1_tex;\n"
        "layout(set = 1, binding = 3) uniform sampler texSamp1_smp;\n"
        "#define texSamp1 usampler2D(texSamp1_tex, texSamp1_smp)\n"
        "layout(set = 1, binding = 4) uniform texture3D texSamp2_tex;\n"
        "layout(set = 1, binding = 5) uniform sampler texSamp2_smp;\n"
        "#define texSamp2 sampler3D(texSamp2_tex, texSamp2_smp)\n"
        "void main() {\n"
        "  float zPlane = vtxPos0.x;\n"
        "  vec2 zGrad = vec2(dFdx(zPlane), dFdy(zPlane)) * "
        "vec2(surfaceScale);\n"
        "  vec2 coord = gl_FragCoord.xy - 0.5;\n"
        "  vec2 scaled = gl_FragCoord.xy / surfaceScale;\n"
        "  for (int i = 0; i < 9; i++) {\n"
        "    zPlane += gaussian3x3[i];\n"
        "  }\n"
        "  bool outside = any(bvec4(\n"
        "      lessThan(coord, vec2(0.0)),\n"
        "      greaterThanEqual(coord, vec2(1.0))));\n"
        "  if (!outside) { discard; }\n"
        "  vec4 pT0 = vtxD0;\n"
        "  vec4 t0 = texture(texSamp0, pT0.xy, texLodBias[0]);\n"
        "  vec4 t1 = textureProj(texSamp0, pT0.xyw);\n"
        "  vec4 t2 = textureProj(texSamp2, pT0);\n"
        "  ivec2 sz = textureSize(texSamp0, 0);\n"
        "  uvec4 raw = texelFetch(texSamp1, clamp(ivec2(0), ivec2(0), sz - 1), "
        "0);\n"
        "  vec2 bm = bumpMat[0] * vec2(1.0, 0.0);\n"
        "  fragColor = NaNToOne(t0 + t1 + t2 + vec4(raw) + vec4(bm, 0.0, "
        "1.0));\n"
        "  fragColor += _LIT(vtxD0);\n"
        "  zPlane *= 255.0;\n"
        "  coord /= surfaceScale;\n"
        "  if (same(fragColor, vtxD0)) { discard; }\n"
        "  precise float zvalue = 1.0 / zPlane;\n"
        "  if (isnan(zvalue)) {\n"
        "    zvalue = uintBitsToFloat(0x7F7FFFFFu);\n"
        "  }\n"
        "  gl_FragDepth = zvalue;\n"
        "}\n";

    g_autofree char *wgsl = translate(glsl, false);
    g_assert_nonnull(wgsl);
    assert_has(wgsl, "diagnostic(off, derivative_uniformity);");
    assert_has(wgsl, "@fragment");
    assert_has(wgsl, "fn fs(");
    assert_has(wgsl, "@location(0) @interpolate(flat) vtxD0: vec4f");
    assert_has(wgsl, "@location(1) @interpolate(linear) vtxPos0: vec4f");
    assert_has(wgsl, "@builtin(position) frag_coord: vec4f");
    assert_has(wgsl, "@location(0) fragColor: vec4f");
    assert_has(wgsl, "@builtin(frag_depth) frag_depth: f32");
    assert_has(wgsl, "bumpMat: array<mat2x2f, 4>");
    assert_has(wgsl, "surfaceScale: vec2i");
    assert_has(wgsl, "texLodBias: array<f32, 4>");
    assert_has(wgsl, "clipRegion: array<vec4f, 8>");
    assert_has(wgsl, "@group(1) @binding(0) var texSamp0_tex: texture_2d<f32>");
    assert_has(wgsl, "@group(1) @binding(4) var texSamp2_tex: texture_3d<f32>");
    assert_has(wgsl, "@group(1) @binding(2) var texSamp1_tex: texture_2d<u32>");
    assert_has(wgsl, "textureSampleBias(texSamp0_tex, texSamp0_smp");
    assert_has(wgsl, "textureSample(texSamp0_tex, texSamp0_smp");
    assert_has(wgsl, "textureSample(texSamp2_tex, texSamp2_smp");
    assert_has(wgsl, "vec2i(textureDimensions(texSamp0_tex))");
    assert_has(wgsl, "textureLoad(texSamp1_tex");
    assert_has(wgsl, "dpdx(zPlane)");
    assert_has(wgsl, "dpdy(zPlane)");
    assert_has(wgsl, "vec2f(u_blk.surfaceScale)");
    assert_has(wgsl, "const gaussian3x3 = array<f32, 9>");
    assert_has(wgsl, "for (var i: i32 = 0;");
    assert_has(wgsl, "select(");
    assert_has(wgsl, "bitcast<vec4u>");
    assert_has(wgsl, "all((");
    assert_has(wgsl, "discard;");
    assert_has(wgsl, "a.y * b.x");
    assert_has(wgsl, "zPlane *= 255.0");
    assert_has(wgsl, "coord /= ");
    assert_has(wgsl, "var<private> gl_FragDepth: f32");
    assert_has(wgsl, "_out.frag_depth = gl_FragDepth");
    assert_lacks(wgsl, "vertex_index");
    assert_lacks(wgsl, "gl_FragCoord");
    assert_lacks(wgsl, "sampler2D");
    assert_lacks(wgsl, "texture(");
    assert_lacks(wgsl, "dFdx");
}

static void test_rejects_switch(void)
{
    const char *glsl = "void main() { switch (1) { default: break; } }\n";
    g_autofree char *wgsl = translate(glsl, true);
    g_assert_null(wgsl);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/webgpu/wgsl/vertex", test_vertex);
    g_test_add_func("/nv2a/webgpu/wgsl/fragment", test_fragment);
    g_test_add_func("/nv2a/webgpu/wgsl/reject-switch", test_rejects_switch);
    return g_test_run();
}
