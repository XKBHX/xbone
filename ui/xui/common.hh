//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once

#include <SDL3/SDL.h>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
/* The WebGPU UI does not call these. The desktop GL fallback in this file
 * still has to compile, and WebGL has no names for them. */
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_WIDTH
#define GL_TEXTURE_WIDTH 0x1000
#endif
#ifndef GL_TEXTURE_HEIGHT
#define GL_TEXTURE_HEIGHT 0x1001
#endif
#ifndef GL_PACK_IMAGE_HEIGHT
#define GL_PACK_IMAGE_HEIGHT 0x806C
#endif
static inline void glBindFragDataLocation(GLuint program, GLuint color,
                                          const GLchar *name)
{
    (void)program;
    (void)color;
    (void)name;
}
static inline void glGetTexLevelParameteriv(GLenum target, GLint level,
                                            GLenum pname, GLint *params)
{
    (void)target;
    (void)level;
    (void)pname;
    if (params) {
        *params = 0;
    }
}
static inline void glGetTexImage(GLenum target, GLint level, GLenum format,
                                 GLenum type, void *pixels)
{
    (void)target;
    (void)level;
    (void)format;
    (void)type;
    (void)pixels;
}
#else
#include <epoxy/gl.h>
#endif
#include "ui/xemu-settings.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <misc/cpp/imgui_stdlib.h>
#include <stb_image.h>

#include "qemu/osdep.h"

extern "C" {
// Include necessary QEMU headers
#include "qapi/error.h"
#include "system/runstate.h"
#include "hw/xbox/mcpx/apu/apu_debug.h"
#include "hw/xbox/nv2a/debug.h"
#include "hw/xbox/nv2a/nv2a.h"

#undef typename
#undef atomic_fetch_add
#undef atomic_fetch_and
#undef atomic_fetch_xor
#undef atomic_fetch_or
#undef atomic_fetch_sub
}

extern bool g_screenshot_pending;
extern float g_main_menu_height;
