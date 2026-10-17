#pragma once

struct SDL_Window;

#ifdef CONFIG_WEBGPU
bool xemu_wgpu_ui_init(SDL_Window *window);
void xemu_wgpu_ui_shutdown(void);
bool xemu_wgpu_ui_active(void);
void xemu_wgpu_ui_new_frame(void);
void xemu_wgpu_ui_render(void);
void xemu_wgpu_ui_rebuild_fonts(void);
#else
static inline bool xemu_wgpu_ui_init(SDL_Window *) { return false; }
static inline void xemu_wgpu_ui_shutdown(void) {}
static inline bool xemu_wgpu_ui_active(void) { return false; }
static inline void xemu_wgpu_ui_new_frame(void) {}
static inline void xemu_wgpu_ui_render(void) {}
static inline void xemu_wgpu_ui_rebuild_fonts(void) {}
#endif
