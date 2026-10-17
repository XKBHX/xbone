#pragma once

#include <stdint.h>
#include <vector>

class Fbo;

enum WgpuArt {
    WGPU_ART_DUKE = 0,
    WGPU_ART_S,
    WGPU_ART_XMU,
    WGPU_ART_LOGO,
    WGPU_ART_COUNT,
};

enum DecalBlend {
    DECAL_BLEND_REPLACE = 0,
    DECAL_BLEND_UNDER,
    DECAL_BLEND_ALPHA,
};

#ifdef CONFIG_WEBGPU
bool wgpu_helpers_active(void);
bool wgpu_helpers_init(void);
void wgpu_helpers_shutdown(void);
void wgpu_helpers_fbo_create(Fbo *fbo);
void wgpu_helpers_fbo_destroy(Fbo *fbo);
void wgpu_helpers_fbo_target(Fbo *fbo);
void wgpu_helpers_fbo_restore(Fbo *fbo);
uint64_t wgpu_helpers_fbo_texture(Fbo *fbo);
void wgpu_helpers_begin(int art, bool logo, float time_sec);
void wgpu_helpers_blend(int blend);
void wgpu_helpers_target_size(float *w, float *h);
void wgpu_helpers_art_size(float *w, float *h);
void wgpu_helpers_draw(const float scale_offset[4],
                       const float tex_scale_offset[4], uint32_t primary,
                       uint32_t secondary, uint32_t fill);
bool wgpu_helpers_render_framebuffer(uint32_t tex, int width, int height,
                                     bool flip, const float scale[2]);
bool wgpu_helpers_framebuffer_png(uint32_t tex, bool flip, int width,
                                  int height, std::vector<uint8_t> &png);
uint64_t wgpu_helpers_icon(int *width, int *height);
#else
static inline bool wgpu_helpers_active(void)
{
    return false;
}
static inline bool wgpu_helpers_init(void)
{
    return false;
}
static inline void wgpu_helpers_shutdown(void)
{
}
static inline void wgpu_helpers_fbo_create(Fbo *)
{
}
static inline void wgpu_helpers_fbo_destroy(Fbo *)
{
}
static inline void wgpu_helpers_fbo_target(Fbo *)
{
}
static inline void wgpu_helpers_fbo_restore(Fbo *)
{
}
static inline uint64_t wgpu_helpers_fbo_texture(Fbo *)
{
    return 0;
}
static inline void wgpu_helpers_begin(int, bool, float)
{
}
static inline void wgpu_helpers_blend(int)
{
}
static inline void wgpu_helpers_target_size(float *w, float *h)
{
    *w = 0;
    *h = 0;
}
static inline void wgpu_helpers_art_size(float *w, float *h)
{
    *w = 1;
    *h = 1;
}
static inline void wgpu_helpers_draw(const float[4], const float[4], uint32_t,
                                     uint32_t, uint32_t)
{
}
static inline bool wgpu_helpers_render_framebuffer(uint32_t, int, int, bool,
                                                   const float[2])
{
    return false;
}
static inline bool wgpu_helpers_framebuffer_png(uint32_t, bool, int, int,
                                                std::vector<uint8_t> &)
{
    return false;
}
static inline uint64_t wgpu_helpers_icon(int *width, int *height)
{
    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
    return 0;
}
#endif
