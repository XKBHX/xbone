// HUD decals for the WebGPU UI.
//
// fs_mask is the controller and memory-unit shader. fs_logo is the SDF
// animation from xemu-logo.frag. fs_gamma is the framebuffer blit, including
// the DAC palette. The vertex shader matches the GLSL decal shader: params.z
// flips the texture v, and params.w flips clip y so an FBO sampled with the
// existing GL texture coordinates keeps v = 0 at the bottom.

struct Uniforms {
    scale_offset: vec4f,
    tex_scale_offset: vec4f,
    color_primary: vec4f,
    color_secondary: vec4f,
    color_fill: vec4f,
    params: vec4f,
    palette: array<vec4u, 64>,
}

struct VsIn {
    @location(0) pos: vec2f,
    @location(1) uv: vec2f,
}

struct VsOut {
    @builtin(position) clip: vec4f,
    @location(0) uv: vec2f,
    @location(1) gl_uv: vec2f,
}

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(1) @binding(0) var tex: texture_2d<f32>;
@group(1) @binding(1) var samp: sampler;

const TEXT_POS = vec4f(0.01, 0.0, 0.98, 0.125);
const PI: f32 = 3.14159265359;
const LINE_WIDTH: f32 = 0.175;
const DURATION: f32 = 1.25;
const PAUSE: f32 = 6.0;
const NUM_PARTICLES: i32 = 35;
const NUM_SPOTLIGHTS: i32 = 5;

@vertex
fn vs(in: VsIn) -> VsOut {
    var t = in.uv;
    if (u.params.z != 0.0) {
        t.y = 1.0 - t.y;
    }
    let ndc = in.pos * u.scale_offset.xy + u.scale_offset.zw;
    var clip_y = ndc.y;
    if (u.params.w != 0.0) {
        clip_y = -ndc.y;
    }
    var out: VsOut;
    out.clip = vec4f(ndc.x, clip_y, 0.0, 1.0);
    out.uv = t * u.tex_scale_offset.xy + u.tex_scale_offset.zw;
    out.gl_uv = ndc * vec2f(0.5, 0.5) + vec2f(0.5, 0.5);
    return out;
}

@fragment
fn fs_mask(in: VsOut) -> @location(0) vec4f {
    let t = textureSample(tex, samp, in.uv);
    var out = u.color_fill;
    out = vec4f(
        out.rgb + mix(u.color_secondary.rgb, u.color_primary.rgb, t.r),
        out.a + t.a - t.b);
    return out;
}

fn modulo(x: f32, y: f32) -> f32 {
    return x - y * floor(x / y);
}

fn random(co: f32) -> f32 {
    return fract(abs(sin(co * 12.989)) * 43758.545);
}

fn median(r: f32, g: f32, b: f32) -> f32 {
    return max(min(r, g), min(max(r, g), b));
}

fn qua_impulse(k: f32, x: f32) -> f32 {
    return 2.0 * sqrt(k) * x / (1.0 + k * x * x);
}

fn current_time() -> f32 {
    return modulo(u.params.x, DURATION + PAUSE) / DURATION;
}

fn get_box(uv: vec2f, x: f32, width: f32) -> f32 {
    let lhs = sign(clamp(x - uv.x, 0.0, 1.0));
    let rhs = sign(clamp(x - uv.x + width, 0.0, 1.0));
    return rhs - lhs;
}

fn sweeping_line_pos() -> f32 {
    return current_time() - LINE_WIDTH + TEXT_POS.x;
}

fn sweeping_line(uv: vec2f) -> f32 {
    return get_box(uv, sweeping_line_pos(), LINE_WIDTH);
}

fn gradients(uv: vec2f) -> f32 {
    let t = current_time();
    let l_s = abs(cos(t * PI * 2.0));
    let pos = t - uv.x + TEXT_POS.x;
    let left = l_s * smoothstep(0.0, 1.0,
        0.5 - abs(pos - LINE_WIDTH) * (20.0 + 80.0 * (1.0 - l_s)));
    let r_s = abs(sin(t * PI * 2.0));
    let right = r_s * smoothstep(0.0, 1.0,
        0.5 - abs(pos) * (20.0 + 80.0 * (1.0 - r_s)));
    let gradient_y = smoothstep(0.55, 1.0, 1.0 - abs(0.5 - uv.y));
    return (left + right) * gradient_y;
}

fn spotlight_pos(i: i32) -> vec2f {
    let t = current_time();
    let fi = f32(i);
    let initial = TEXT_POS.zw * vec2f(
        fi / f32(NUM_SPOTLIGHTS - 1),
        sign(random(fi + 62.0) - 0.6) * 2.0);
    let velocity = vec2f(
        sign(random(fi + 63.0) - 0.5) * 0.7 * (0.3 + 0.6 * random(fi + 100.0)),
        -sign(initial.y) * 0.8 * (0.1 + 0.9 * random(fi + 62.0)));
    return initial + velocity * t + vec2f(TEXT_POS.x, 0.5);
}

fn spotlights(uv: vec2f) -> f32 {
    let t = current_time();
    let right = smoothstep(0.3, 0.7,
        0.8 - 8.0 * abs(t - uv.x + TEXT_POS.x + 0.05));
    var c = 0.0;
    for (var j = 0; j < NUM_SPOTLIGHTS; j = j + 1) {
        let d = distance(uv, spotlight_pos(j));
        c = c + (1.0 - smoothstep(0.04, 0.07835, d));
    }
    return 0.6 * right + 0.4 * c;
}

fn particle_initial(i: i32) -> vec2f {
    let fi = f32(i);
    return TEXT_POS.zw * vec2f(
        fi / f32(NUM_PARTICLES - 1),
        sign(random(fi) - 0.2));
}

fn prob(p: f32, i: i32) -> f32 {
    return sign(clamp(random(f32(i * 30)) - (1.0 - p), 0.0, 1.0));
}

fn particle_lifespan(i: i32) -> f32 {
    return 1.0 + 1.25 * exp(-10.0 * random(f32(i * 30))) + 0.5 * prob(0.3, i);
}

fn particle_time(i: i32) -> f32 {
    return current_time() - particle_initial(i).x;
}

fn particle_alive(i: i32) -> f32 {
    return clamp(sign(particle_time(i)), 0.0, 1.0);
}

fn particle_intensity(i: i32) -> f32 {
    return particle_alive(i) *
        clamp(particle_lifespan(i) - particle_time(i), 0.0, 1.0);
}

fn particle_pos(i: i32) -> vec2f {
    let pt = particle_time(i);
    let fi = f32(i);
    let impulse = qua_impulse(20.0, pt * 0.25 + 0.05 + 0.4 * random(fi + 30.0));
    let initial = particle_initial(i);
    let velocity = vec2f(
        0.4 * impulse * sign(random(fi + 66.0) - 0.1) *
            (0.3 + 0.6 * random(fi + 100.0)),
        0.8 * impulse * sign(initial.y) * (0.1 + 0.9 * random(fi + 62.0)));
    return initial + particle_alive(i) * velocity * pt + vec2f(TEXT_POS.x, 0.5);
}

fn particles(uv: vec2f) -> f32 {
    var c = 0.0;
    for (var j = 0; j < NUM_PARTICLES; j = j + 1) {
        let d = distance(uv, particle_pos(j));
        c = c + (1.0 - smoothstep(0.004, 0.00835, d)) * particle_intensity(j);
    }
    return c;
}

@fragment
fn fs_logo(in: VsOut) -> @location(0) vec4f {
    // gl_uv matches gl_FragCoord / 512 for the 512x512 logo target.
    var uv = in.gl_uv;
    let scale = 1.4;
    uv = uv - 0.5 * (1.0 - 1.0 / scale);
    uv = uv * scale;
    let pos = uv;

    // GL samples this SDF with a transparent border. Clamp-to-edge would
    // repeat the edge texel, so out-of-range coordinates contribute black.
    let in_bounds = pos.x >= 0.0 && pos.x <= 1.0 &&
        pos.y >= 0.0 && pos.y <= 1.0;
    let sampled = textureSample(tex, samp, clamp(pos, vec2f(0.0), vec2f(1.0))).rgb;
    let msd = select(vec3f(0.0), sampled, in_bounds);
    let sd = median(msd.r, msd.g, msd.b);
    let screen_px = 6.0 * (sd - 0.5);
    let opacity = clamp(screen_px + 0.5, 0.0, 1.0);
    let fg = u.color_primary;
    let bg = u.color_fill;
    let fill_color = mix(bg, fg, opacity);
    var outline = clamp(screen_px + 1.6, 0.0, 1.0);
    outline = outline - clamp(screen_px - 1.6, 0.0, 1.0);
    outline = smoothstep(0.5, 1.0, outline);

    let line_color = mix(bg, fg, outline);
    var out = mix(fill_color, line_color, sweeping_line(uv));
    let mask_rhs = clamp(sign(uv.x - LINE_WIDTH - sweeping_line_pos()), 0.0, 1.0);
    out = out + fill_color * mask_rhs * spotlights(uv);
    out = out + mix(vec4f(0.0), fg, particles(uv));
    out = out + 2.0 * fg * get_box(uv, TEXT_POS.x, TEXT_POS.z) * gradients(uv);
    return out;
}

fn palette_at(i: u32) -> u32 {
    let v = u.palette[i >> 2u];
    let c = i & 3u;
    if (c == 0u) {
        return v.x;
    }
    if (c == 1u) {
        return v.y;
    }
    if (c == 2u) {
        return v.z;
    }
    return v.w;
}

fn gamma_ch(ch: i32, col: f32) -> f32 {
    let idx = u32(clamp(col, 0.0, 1.0) * 255.0);
    let bits = extractBits(palette_at(idx), u32(ch * 8), 8u);
    return f32(bits) / 255.0;
}

@fragment
fn fs_gamma(in: VsOut) -> @location(0) vec4f {
    let col = textureSample(tex, samp, in.uv);
    return vec4f(gamma_ch(0, col.r), gamma_ch(1, col.g), gamma_ch(2, col.b), col.a);
}
