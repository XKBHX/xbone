/*
 * xemu User Interface
 *
 * Copyright (C) 2020-2022 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdint>
#include <fpng.h>
#include <vector>

#include "xemu-snapshots.h"
#include "xui/gl-helpers.hh"
#include "xui/wgpu-helpers.hh"
#include "xui/wgpu-ui.hh"

static uint32_t display_tex = 0;
static bool display_flip = false;

void xemu_snapshots_set_framebuffer_texture(uint32_t tex, bool flip)
{
    display_tex = tex;
    display_flip = flip;
}

static bool decode_thumbnail_png(const void *buf, size_t size,
                                 std::vector<uint8_t> &rgba, int *width,
                                 int *height)
{
    std::vector<uint8_t> pixels;
    unsigned int decoded_width, decoded_height, channels;

    if (fpng::fpng_decode_memory(buf, size, pixels, decoded_width,
                                 decoded_height, channels,
                                 3) != fpng::FPNG_DECODE_SUCCESS ||
        decoded_width == 0 || decoded_height == 0) {
        return false;
    }
    rgba.resize((size_t)decoded_width * (size_t)decoded_height * 4u);
    for (size_t i = 0; i < (size_t)decoded_width * (size_t)decoded_height;
         i++) {
        rgba[i * 4u + 0] = pixels[i * 3u + 0];
        rgba[i * 4u + 1] = pixels[i * 3u + 1];
        rgba[i * 4u + 2] = pixels[i * 3u + 2];
        rgba[i * 4u + 3] = 255;
    }
    *width = (int)decoded_width;
    *height = (int)decoded_height;
    return true;
}

static uint64_t load_png_to_gl(const void *buf, size_t size, int *width,
                               int *height)
{
    std::vector<uint8_t> rgba;
    GLuint tex = 0;

    if (!decode_thumbnail_png(buf, size, rgba, width, height)) {
        return 0;
    }
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, *width, *height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());
    return tex;
}

uint64_t xemu_snapshots_create_thumbnail(const void *buf, size_t size,
                                         int *width, int *height)
{
    int decoded_width = 0;
    int decoded_height = 0;
    std::vector<uint8_t> rgba;
    uint64_t id = 0;

    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
    if (xemu_wgpu_ui_active()) {
        if (!decode_thumbnail_png(buf, size, rgba, &decoded_width,
                                  &decoded_height)) {
            return 0;
        }
        id = wgpu_helpers_create_image(rgba.data(), decoded_width,
                                       decoded_height);
    } else {
        id = load_png_to_gl(buf, size, &decoded_width, &decoded_height);
    }
    if (!id) {
        return 0;
    }
    if (width) {
        *width = decoded_width;
    }
    if (height) {
        *height = decoded_height;
    }
    return id;
}

void xemu_snapshots_destroy_thumbnail(uint64_t thumbnail)
{
    GLuint tex;

    if (!thumbnail) {
        return;
    }
    if (xemu_wgpu_ui_active()) {
        wgpu_helpers_release_image(thumbnail);
        return;
    }
    tex = (GLuint)thumbnail;
    glDeleteTextures(1, &tex);
}

void *xemu_snapshots_create_framebuffer_thumbnail_png(size_t *size)
{
    /*
     * Avoids crashing if a snapshot is made on a thread with no GL context.
     * Normally this is not an issue, but it is better to fail safe than assert
     * here.
     * FIXME: Allow for dispatching a thumbnail request to the UI thread to
     * remove this altogether.
     *
     * A WebGPU VGA frame has no GL texture. Its pixels are already on the UI
     * device, so the PNG readback does not need a GL context. An accelerated
     * frame is still copied from the GL texture until the UI pass moves.
     */
    if (wgpu_helpers_active()) {
        if (!display_tex && !wgpu_helpers_has_guest_frame()) {
            return NULL;
        }
        if (display_tex && !SDL_GL_GetCurrentContext()) {
            return NULL;
        }
    } else if (!SDL_GL_GetCurrentContext() || display_tex == 0) {
        return NULL;
    }

    std::vector<uint8_t> png;
    if (!RenderFramebufferToPng(display_tex, display_flip, png,
                                2 * XEMU_SNAPSHOT_THUMBNAIL_WIDTH,
                                2 * XEMU_SNAPSHOT_THUMBNAIL_HEIGHT)) {
        return NULL;
    }

    void *buf = g_malloc(png.size());
    memcpy(buf, png.data(), png.size());
    *size = png.size();
    return buf;
}
