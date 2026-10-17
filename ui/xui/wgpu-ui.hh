#pragma once

#include <stdint.h>

struct SDL_Window;

#ifdef CONFIG_WEBGPU
#include <webgpu/webgpu.h>
enum {
    XEMU_WGPU_UI_JOB_INIT = 1,
    XEMU_WGPU_UI_JOB_FONTS,
    XEMU_WGPU_UI_JOB_FRAME,
    XEMU_WGPU_UI_JOB_PNG,
    XEMU_WGPU_UI_JOB_SHUTDOWN,
};

bool xemu_wgpu_ui_init(SDL_Window *window);
void xemu_wgpu_ui_shutdown(void);
bool xemu_wgpu_ui_active(void);
void xemu_wgpu_ui_new_frame(void);
void xemu_wgpu_ui_render(void);
bool xemu_wgpu_ui_submit(int job);
void xemu_wgpu_ui_rebuild_fonts(void);
WGPUDevice xemu_wgpu_ui_device(void);
WGPUQueue xemu_wgpu_ui_queue(void);
WGPUInstance xemu_wgpu_ui_instance(void);
WGPUTextureView xemu_wgpu_ui_prepare_target(int width, int height);
void xemu_wgpu_ui_mark_scene(void);
void xemu_wgpu_ui_register_texture(uint64_t id);
void xemu_wgpu_ui_unregister_texture(uint64_t id);
bool xemu_wgpu_ui_renderer(const char **vendor, const char **adapter,
                           const char **driver, const char **shader);
#else
static inline bool xemu_wgpu_ui_init(SDL_Window *)
{
    return false;
}
static inline void xemu_wgpu_ui_shutdown(void)
{
}
static inline bool xemu_wgpu_ui_active(void)
{
    return false;
}
static inline void xemu_wgpu_ui_new_frame(void)
{
}
static inline void xemu_wgpu_ui_render(void)
{
}
static inline void xemu_wgpu_ui_rebuild_fonts(void)
{
}
static inline bool xemu_wgpu_ui_renderer(const char **, const char **,
                                         const char **, const char **)
{
    return false;
}
#endif
