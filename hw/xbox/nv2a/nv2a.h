/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2020-2021 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_NV2A_H
#define HW_NV2A_H

#include <stdbool.h>
#include <stddef.h>

void nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void nv2a_context_init(void);
int nv2a_get_framebuffer_surface(void);
void nv2a_release_framebuffer_surface(void);
void nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int nv2a_get_surface_scale_factor(void);
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);
/* WebGPU UI. display_size is the composited guest image. Returns 0 when
 * there is no color surface. submit_ui blocks until the PFIFO thread has
 * run the posted UI job. */
int nv2a_wgpu_display_size(int *width, int *height);
bool nv2a_wgpu_submit_ui(void);
/* Native window present. The UI thread publishes the platform handle and
 * the pixel size before posting a frame. kind is one of the values below.
 * primary/secondary are the Metal layer, Win32 HINSTANCE and HWND, the X11
 * Display, or the Wayland display and surface. x_window is the X11 window. */
#define NV2A_WGPU_WINDOW_METAL 1
#define NV2A_WGPU_WINDOW_HWND 2
#define NV2A_WGPU_WINDOW_XLIB 3
#define NV2A_WGPU_WINDOW_WAYLAND 4
void nv2a_wgpu_set_window_target(int kind, void *primary, void *secondary,
                                 uint64_t x_window, uint32_t width,
                                 uint32_t height, int vsync);
void nv2a_wgpu_copy_adapter(char *vendor, size_t vendor_size, char *name,
                            size_t name_size, char *description,
                            size_t description_size);

#endif
