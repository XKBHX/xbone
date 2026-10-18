/*
 * Geforce NV2A PGRAPH WebGPU Renderer
 *
 * Copyright (c) 2026 xemu Project
 *
 * Based on the Vulkan implementation:
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

#include <math.h>

#ifdef __EMSCRIPTEN__
#include "canvas.h"

static WgpuCanvas display_canvas;
#else
#include "window.h"

static WgpuWindow display_window;
#endif

#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>
#endif

#if defined(CONFIG_OPENGL) && !defined(__EMSCRIPTEN__)
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/CGLIOSurface.h>
/* kCVPixelFormatType_32BGRA. Dawn maps it to bgra8unorm. CGL reads
 * GL_BGRA + UNSIGNED_INT_8_8_8_8_REV as B, G, R, A, so an rgba8unorm
 * surface (R, G, B, A) swaps red and blue. */
#define DISPLAY_IOSURFACE_BGRA 0x42475241u
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#ifndef GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#endif
#ifndef GL_LINEAR_TILING_EXT
#define GL_LINEAR_TILING_EXT 0x9585
#endif
#elif defined(_WIN32)
#include <d3d11.h>
#ifndef GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT
#define GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT 0x958C
#endif
#ifndef GL_OPTIMAL_TILING_EXT
#define GL_OPTIMAL_TILING_EXT 0x9584
#endif
#endif
#endif

/* std140-style layout matching the WGSL DisplayParams struct. vec3f is
 * aligned to 16, so pvideo_color_key starts at byte 64 and the struct ends
 * at 80. */
typedef struct DisplayParams {
    float line_offset;
    uint32_t pvideo_enable;
    float display_size[2];
    float pvideo_in_pos[2];
    uint32_t pvideo_color_key_enable;
    uint32_t pad0;
    float pvideo_pos[4];
    float pvideo_scale[4];
    float pvideo_color_key[3];
    uint32_t pad1;
} DisplayParams;

_Static_assert(sizeof(DisplayParams) == 80, "DisplayParams size");
_Static_assert(offsetof(DisplayParams, display_size) == 8, "display_size");
_Static_assert(offsetof(DisplayParams, pvideo_in_pos) == 16, "in_pos");
_Static_assert(offsetof(DisplayParams, pvideo_pos) == 32, "pvideo_pos");
_Static_assert(offsetof(DisplayParams, pvideo_scale) == 48, "pvideo_scale");
_Static_assert(offsetof(DisplayParams, pvideo_color_key) == 64, "color_key");

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

/* Same composite as vk/display.c, including the Y flip. WebGPU stores row 0
 * at the top. glTexImage2D treats that row as the bottom. The shared path
 * blits an IOSurface into the same 2D texture, and that blit matches the
 * readback byte for byte, so the flip keeps the picture upright. The UI
 * samples with flip disabled.
 * The overlay sample stays in uniform control flow: WGSL rejects
 * textureSample under a per-pixel branch. */
static const char display_wgsl[] =
    "struct DisplayParams {\n"
    "    line_offset: f32,\n"
    "    pvideo_enable: u32,\n"
    "    display_size: vec2f,\n"
    "    pvideo_in_pos: vec2f,\n"
    "    pvideo_color_key_enable: u32,\n"
    "    pad0: u32,\n"
    "    pvideo_pos: vec4f,\n"
    "    pvideo_scale: vec4f,\n"
    "    pvideo_color_key: vec3f,\n"
    "}\n"
    "@group(0) @binding(0) var frame_tex: texture_2d<f32>;\n"
    "@group(0) @binding(1) var frame_samp: sampler;\n"
    "@group(0) @binding(2) var pvideo_tex: texture_2d<f32>;\n"
    "@group(0) @binding(3) var pvideo_samp: sampler;\n"
    "@group(0) @binding(4) var<uniform> params: DisplayParams;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) "
    "vec4f {\n"
    "    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "    let display_size = params.display_size;\n"
    "    var tex_coord = pos.xy / display_size;\n"
    "    let frame_size = vec2f(textureDimensions(frame_tex));\n"
    "    let rel = display_size.y / frame_size.y / params.line_offset;\n"
    "    tex_coord.y = 1.0 + rel * (tex_coord.y - 1.0);\n"
    "    tex_coord.y = 1.0 - tex_coord.y;\n"
    "    var color = textureSample(frame_tex, frame_samp, tex_coord);\n"
    "    if (params.pvideo_enable != 0u) {\n"
    "        let screen_coord = vec2f(pos.x, display_size.y - pos.y) *\n"
    "            params.pvideo_scale.z;\n"
    "        let output_min = params.pvideo_pos.xy;\n"
    "        let output_max = params.pvideo_pos.xy + params.pvideo_pos.zw;\n"
    "        let out_xy = screen_coord - output_min;\n"
    "        let in_st = (params.pvideo_in_pos + out_xy *\n"
    "            params.pvideo_scale.xy) /\n"
    "            vec2f(textureDimensions(pvideo_tex));\n"
    "        let overlay = textureSample(pvideo_tex, pvideo_samp, in_st);\n"
    "        let clipped = screen_coord.x < output_min.x ||\n"
    "            screen_coord.y < output_min.y ||\n"
    "            screen_coord.x > output_max.x ||\n"
    "            screen_coord.y > output_max.y;\n"
    "        let key_ok = params.pvideo_color_key_enable == 0u ||\n"
    "            all(color.rgb == params.pvideo_color_key);\n"
    "        if (!clipped && key_ok) {\n"
    "            color = overlay;\n"
    "        }\n"
    "    }\n"
    "    return color;\n"
    "}\n";

#ifdef CONFIG_OPENGL
/* The GL name belongs to the UI context, which is current in present and
 * not necessarily in finalize. A name retired at shutdown is deleted the
 * next time that context presents. */
static GLuint retired_gl_texture;
static GLuint retired_gl_memory;

#ifdef __APPLE__
/* CGLTexImageIOSurface2D only accepts GL_TEXTURE_RECTANGLE. The UI samples
 * a 2D texture, so the rectangle is blitted into gl_texture. */
static GLuint iosurface_rect;
static GLuint iosurface_read_fbo;
static GLuint iosurface_draw_fbo;

static void delete_iosurface_gl(void)
{
    if (iosurface_rect) {
        glDeleteTextures(1, &iosurface_rect);
        iosurface_rect = 0;
    }
    if (iosurface_read_fbo) {
        glDeleteFramebuffers(1, &iosurface_read_fbo);
        iosurface_read_fbo = 0;
    }
    if (iosurface_draw_fbo) {
        glDeleteFramebuffers(1, &iosurface_draw_fbo);
        iosurface_draw_fbo = 0;
    }
}
#endif

static void retire_gl_objects(GLuint tex, GLuint mem)
{
    if (tex) {
        retired_gl_texture = tex;
    }
    if (mem) {
        retired_gl_memory = mem;
    }
}

static void delete_retired_gl_objects(void)
{
    if (retired_gl_texture) {
        glDeleteTextures(1, &retired_gl_texture);
        retired_gl_texture = 0;
#ifdef __APPLE__
        delete_iosurface_gl();
#endif
    }
#if defined(__linux__) || defined(_WIN32)
    if (retired_gl_memory) {
        glDeleteMemoryObjectsEXT(1, &retired_gl_memory);
        retired_gl_memory = 0;
    }
#else
    retired_gl_memory = 0;
#endif
}
#endif

static void release_bind_group(WgpuDisplayState *disp)
{
    if (disp->bind_group) {
        wgpuBindGroupRelease(disp->bind_group);
        disp->bind_group = NULL;
    }
}

static void destroy_texture(WGPUTexture *texture, WGPUTextureView *view)
{
    if (*view) {
        wgpuTextureViewRelease(*view);
        *view = NULL;
    }
    if (*texture) {
        wgpuTextureRelease(*texture);
        *texture = NULL;
    }
}

#ifndef __EMSCRIPTEN__
static void log_shared_failure(const char *why)
{
    static int logged;

    if (logged) {
        return;
    }
    logged = 1;
    fprintf(stderr,
            "[WebGPU] Display share unavailable (%s); reading the frame "
            "back\n",
            why);
}
#endif

#ifndef __EMSCRIPTEN__
#ifdef __linux__
struct display_drm_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct display_drm_destroy_dumb {
    uint32_t handle;
};

struct display_drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};

#define DISPLAY_DRM_IOCTL_CREATE_DUMB \
    _IOWR('d', 0xB2, struct display_drm_create_dumb)
#define DISPLAY_DRM_IOCTL_DESTROY_DUMB \
    _IOWR('d', 0xB3, struct display_drm_destroy_dumb)
#define DISPLAY_DRM_IOCTL_PRIME_HANDLE_TO_FD \
    _IOWR('d', 0x2d, struct display_drm_prime_handle)
/* DRM_FORMAT_ABGR8888: little-endian R, G, B, A. Dawn's rgba8unorm. */
#define DISPLAY_DRM_FORMAT_ABGR8888 0x34324241u
#endif

static bool end_shared_access(PGRAPHWgpuState *r)
{
    WgpuDisplayState *disp = &r->display;
    WGPUSharedTextureMemoryEndAccessState end =
        WGPU_SHARED_TEXTURE_MEMORY_END_ACCESS_STATE_INIT;
    bool ok;

    if (!disp->shared_access) {
        return true;
    }
    ok = wgpuSharedTextureMemoryEndAccess(disp->shared_memory, disp->image,
                                          &end) == WGPUStatus_Success;
    disp->shared_access = false;
    if (ok) {
        disp->shared_initialized = end.initialized;
    }
    /* Frees the fence list and releases each fence. */
    wgpuSharedTextureMemoryEndAccessStateFreeMembers(end);
    return ok;
}

static void destroy_shared_display(PGRAPHWgpuState *r)
{
    WgpuDisplayState *disp = &r->display;

    if (disp->shared_access) {
        end_shared_access(r);
    }
    destroy_texture(&disp->image, &disp->image_view);
    disp->image_sampleable = false;
    if (disp->shared_memory) {
        wgpuSharedTextureMemoryRelease(disp->shared_memory);
        disp->shared_memory = NULL;
    }
#ifdef __APPLE__
    if (disp->io_surface) {
        CFRelease((IOSurfaceRef)disp->io_surface);
        disp->io_surface = NULL;
    }
#endif
#ifdef __linux__
    if (disp->prime_fd >= 0) {
        close(disp->prime_fd);
        disp->prime_fd = -1;
    }
    if (disp->drm_fd >= 0 && disp->dumb_handle) {
        struct display_drm_destroy_dumb destroy = {
            .handle = disp->dumb_handle,
        };
        ioctl(disp->drm_fd, DISPLAY_DRM_IOCTL_DESTROY_DUMB, &destroy);
        disp->dumb_handle = 0;
    }
#endif
#ifdef _WIN32
    if (disp->d3d_texture) {
        ID3D11Texture2D *texture = disp->d3d_texture;

        texture->lpVtbl->Release(texture);
        disp->d3d_texture = NULL;
        disp->shared_handle = NULL;
    }
#endif
    disp->shared = false;
    disp->shared_initialized = false;
    disp->width = 0;
    disp->height = 0;
}

static void finalize_shared_os(WgpuDisplayState *disp)
{
#ifdef __linux__
    if (disp->drm_fd >= 0) {
        close(disp->drm_fd);
        disp->drm_fd = -1;
    }
#endif
#ifdef _WIN32
    if (disp->d3d_context) {
        ID3D11DeviceContext *context = disp->d3d_context;

        context->lpVtbl->Release(context);
        disp->d3d_context = NULL;
    }
    if (disp->d3d_device) {
        ID3D11Device *device = disp->d3d_device;

        device->lpVtbl->Release(device);
        disp->d3d_device = NULL;
    }
#endif
    (void)disp;
}

static void ensure_display_pipeline(PGRAPHWgpuState *r,
                                    WGPUTextureFormat format);

static bool adopt_shared_texture(PGRAPHWgpuState *r,
                                 WGPUSharedTextureMemory memory,
                                 unsigned int width, unsigned int height)
{
    WgpuDisplayState *disp = &r->display;
    WGPUSharedTextureMemoryProperties props =
        WGPU_SHARED_TEXTURE_MEMORY_PROPERTIES_INIT;
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    WGPUTexture texture;
    WGPUTextureView view;
    WGPUTextureUsage need =
        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;

    if (!memory ||
        wgpuSharedTextureMemoryGetProperties(memory, &props) !=
            WGPUStatus_Success ||
        (props.format != WGPUTextureFormat_RGBA8Unorm &&
         props.format != WGPUTextureFormat_BGRA8Unorm) ||
        props.size.width != width || props.size.height != height ||
        (props.usage & need) != need) {
        return false;
    }

    desc.label = pgraph_wgpu_str("Display");
    desc.usage = need;
    desc.dimension = WGPUTextureDimension_2D;
    desc.format = props.format;
    desc.size = props.size;
    texture = wgpuSharedTextureMemoryCreateTexture(memory, &desc);
    if (!texture) {
        return false;
    }
    view = wgpuTextureCreateView(texture, NULL);
    if (!view) {
        wgpuTextureRelease(texture);
        return false;
    }

    destroy_shared_display(r);
    disp->shared_memory = memory;
    disp->image = texture;
    disp->image_view = view;
    disp->width = width;
    disp->height = height;
    disp->shared = true;
    disp->image_sampleable = false;
    disp->shared_initialized = false;
    disp->shared_serial++;
    ensure_display_pipeline(r, props.format);
    return true;
}

#ifdef __APPLE__
static void iosurface_dict_set_int(CFMutableDictionaryRef dict, CFStringRef key,
                                   int value)
{
    CFNumberRef number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);

    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

static bool create_shared_display(PGRAPHWgpuState *r, unsigned int width,
                                  unsigned int height)
{
    int row_bytes =
        (int)IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (size_t)width * 4);
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef surface;
    WGPUSharedTextureMemoryIOSurfaceDescriptor ios =
        WGPU_SHARED_TEXTURE_MEMORY_IO_SURFACE_DESCRIPTOR_INIT;
    WGPUSharedTextureMemoryDescriptor mem_desc =
        WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
    WGPUSharedTextureMemory memory;

    iosurface_dict_set_int(props, kIOSurfaceWidth, (int)width);
    iosurface_dict_set_int(props, kIOSurfaceHeight, (int)height);
    iosurface_dict_set_int(props, kIOSurfaceBytesPerElement, 4);
    iosurface_dict_set_int(props, kIOSurfaceBytesPerRow, row_bytes);
    iosurface_dict_set_int(props, kIOSurfacePixelFormat,
                           (int)DISPLAY_IOSURFACE_BGRA);
    surface = IOSurfaceCreate(props);
    CFRelease(props);
    if (!surface) {
        return false;
    }

    ios.ioSurface = surface;
    ios.allowStorageBinding = WGPU_FALSE;
    mem_desc.label = pgraph_wgpu_str("Display shared");
    mem_desc.nextInChain = &ios.chain;
    memory = wgpuDeviceImportSharedTextureMemory(r->device, &mem_desc);
    if (!memory || !adopt_shared_texture(r, memory, width, height)) {
        if (memory) {
            wgpuSharedTextureMemoryRelease(memory);
        }
        CFRelease(surface);
        return false;
    }
    r->display.io_surface = surface;
    return true;
}
#elif defined(__linux__)
static bool create_shared_display(PGRAPHWgpuState *r, unsigned int width,
                                  unsigned int height)
{
    WgpuDisplayState *disp = &r->display;
    struct display_drm_create_dumb create = {
        .width = width,
        .height = height,
        .bpp = 32,
    };
    struct display_drm_prime_handle prime = { 0 };
    WGPUSharedTextureMemoryDmaBufPlane plane =
        WGPU_SHARED_TEXTURE_MEMORY_DMA_BUF_PLANE_INIT;
    WGPUSharedTextureMemoryDmaBufDescriptor dma =
        WGPU_SHARED_TEXTURE_MEMORY_DMA_BUF_DESCRIPTOR_INIT;
    WGPUSharedTextureMemoryDescriptor mem_desc =
        WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
    WGPUSharedTextureMemory memory = NULL;

    if (disp->drm_fd < 0) {
        disp->drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    }
    if (disp->drm_fd < 0 ||
        ioctl(disp->drm_fd, DISPLAY_DRM_IOCTL_CREATE_DUMB, &create) != 0 ||
        !create.handle || create.pitch != width * 4) {
        if (disp->drm_fd >= 0 && create.handle) {
            struct display_drm_destroy_dumb destroy = { .handle =
                                                            create.handle };
            ioctl(disp->drm_fd, DISPLAY_DRM_IOCTL_DESTROY_DUMB, &destroy);
        }
        return false;
    }

    prime.handle = create.handle;
    prime.flags = O_CLOEXEC | O_RDWR;
    if (ioctl(disp->drm_fd, DISPLAY_DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) !=
            0 ||
        prime.fd < 0) {
        struct display_drm_destroy_dumb destroy = { .handle = create.handle };
        ioctl(disp->drm_fd, DISPLAY_DRM_IOCTL_DESTROY_DUMB, &destroy);
        return false;
    }

    plane.fd = prime.fd;
    plane.stride = create.pitch;
    dma.size = (WGPUExtent3D){ width, height, 1 };
    dma.drmFormat = DISPLAY_DRM_FORMAT_ABGR8888;
    dma.planeCount = 1;
    dma.planes = &plane;
    mem_desc.label = pgraph_wgpu_str("Display shared");
    mem_desc.nextInChain = &dma.chain;
    memory = wgpuDeviceImportSharedTextureMemory(r->device, &mem_desc);
    if (!memory || !adopt_shared_texture(r, memory, width, height)) {
        if (memory) {
            wgpuSharedTextureMemoryRelease(memory);
        }
        close(prime.fd);
        struct display_drm_destroy_dumb destroy = { .handle = create.handle };
        ioctl(disp->drm_fd, DISPLAY_DRM_IOCTL_DESTROY_DUMB, &destroy);
        return false;
    }
    /* Dawn duplicated the fd. This one is closed with the dumb buffer. */
    disp->prime_fd = prime.fd;
    disp->dumb_handle = create.handle;
    disp->prime_size = create.size;
    return true;
}
#elif defined(_WIN32)
static const IID display_iid_dxgi_resource = { 0x035f3ab4,
                                               0x482e,
                                               0x4e50,
                                               { 0xb4, 0x1f, 0x8a, 0x7f, 0x8b,
                                                 0xd8, 0x96, 0x0b } };

typedef HRESULT(WINAPI *DisplayD3D11CreateDeviceFn)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static bool ensure_d3d11_device(WgpuDisplayState *disp)
{
    HMODULE library;
    DisplayD3D11CreateDeviceFn create_device;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr;

    if (disp->d3d_device) {
        return true;
    }
    library = LoadLibraryA("d3d11.dll");
    if (!library) {
        return false;
    }
    create_device = (DisplayD3D11CreateDeviceFn)GetProcAddress(
        library, "D3D11CreateDevice");
    if (!create_device) {
        return false;
    }
    hr = create_device(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                       D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        return false;
    }
    disp->d3d_device = device;
    disp->d3d_context = context;
    return true;
}

static bool create_shared_display(PGRAPHWgpuState *r, unsigned int width,
                                  unsigned int height)
{
    WgpuDisplayState *disp = &r->display;
    ID3D11Device *device;
    ID3D11Texture2D *texture = NULL;
    IDXGIResource *resource = NULL;
    HANDLE handle = NULL;
    D3D11_TEXTURE2D_DESC desc = { 0 };
    WGPUSharedTextureMemoryDXGISharedHandleDescriptor dxgi =
        WGPU_SHARED_TEXTURE_MEMORY_DXGI_SHARED_HANDLE_DESCRIPTOR_INIT;
    WGPUSharedTextureMemoryDescriptor mem_desc =
        WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
    WGPUSharedTextureMemory memory = NULL;
    HRESULT hr;

    if (!ensure_d3d11_device(disp)) {
        return false;
    }
    device = disp->d3d_device;
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    hr = device->lpVtbl->CreateTexture2D(device, &desc, NULL, &texture);
    if (FAILED(hr)) {
        return false;
    }
    hr = texture->lpVtbl->QueryInterface(texture, &display_iid_dxgi_resource,
                                         (void **)&resource);
    if (FAILED(hr)) {
        texture->lpVtbl->Release(texture);
        return false;
    }
    hr = resource->lpVtbl->GetSharedHandle(resource, &handle);
    resource->lpVtbl->Release(resource);
    if (FAILED(hr) || !handle) {
        texture->lpVtbl->Release(texture);
        return false;
    }

    dxgi.handle = handle;
    dxgi.useKeyedMutex = WGPU_FALSE;
    mem_desc.label = pgraph_wgpu_str("Display shared");
    mem_desc.nextInChain = &dxgi.chain;
    memory = wgpuDeviceImportSharedTextureMemory(r->device, &mem_desc);
    if (!memory || !adopt_shared_texture(r, memory, width, height)) {
        if (memory) {
            wgpuSharedTextureMemoryRelease(memory);
        }
        texture->lpVtbl->Release(texture);
        return false;
    }
    /* The texture owns the handle. Do not CloseHandle it. */
    disp->d3d_texture = texture;
    disp->shared_handle = handle;
    return true;
}
#else
static bool create_shared_display(PGRAPHWgpuState *r, unsigned int width,
                                  unsigned int height)
{
    (void)r;
    (void)width;
    (void)height;
    return false;
}
#endif

static bool begin_shared_access(PGRAPHWgpuState *r)
{
    WgpuDisplayState *disp = &r->display;
    WGPUSharedTextureMemoryBeginAccessDescriptor begin =
        WGPU_SHARED_TEXTURE_MEMORY_BEGIN_ACCESS_DESCRIPTOR_INIT;

    begin.initialized = disp->shared_initialized ? WGPU_TRUE : WGPU_FALSE;
    if (wgpuSharedTextureMemoryBeginAccess(disp->shared_memory, disp->image,
                                           &begin) != WGPUStatus_Success) {
        return false;
    }
    disp->shared_access = true;
    return true;
}
#endif

static float pvideo_calculate_scale(unsigned int din_dout,
                                    unsigned int output_size)
{
    float calculated_in = din_dout * (output_size - 1);

    calculated_in = floorf(calculated_in / (1 << 20) + 0.5f);
    return (calculated_in + 1.0f) / output_size;
}

static void convert_cr8yb8cb8ya8(uint8_t *data_out, const uint8_t *data_in,
                                 unsigned int width, unsigned int height,
                                 unsigned int pitch)
{
    for (unsigned int y = 0; y < height; y++) {
        const uint8_t *line = &data_in[y * pitch];
        uint32_t row_offset = y * width;

        for (unsigned int x = 0; x < width; x++) {
            uint8_t *pixel = &data_out[(row_offset + x) * 4];

            convert_yuy2_to_rgb(line, x, &pixel[0], &pixel[1], &pixel[2]);
            pixel[3] = 255;
        }
    }
}

static PvideoState get_pvideo_state(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PvideoState state = { 0 };

    /* SIZE_IN of 0xFFFFFFFF is not what hardware uses to hide the overlay,
     * but games write it while the contents are not valid. Treat it as off,
     * matching the OpenGL and Vulkan renderers. */
    state.enabled =
        (d->pvideo.regs[NV_PVIDEO_BUFFER] & NV_PVIDEO_BUFFER_0_USE) &&
        d->pvideo.regs[NV_PVIDEO_SIZE_IN] != 0xFFFFFFFF;
    if (!state.enabled) {
        return state;
    }

    state.base = d->pvideo.regs[NV_PVIDEO_BASE];
    state.limit = d->pvideo.regs[NV_PVIDEO_LIMIT];
    state.offset = d->pvideo.regs[NV_PVIDEO_OFFSET];
    state.pitch =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_PITCH);
    state.format =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_COLOR);

    /* Other color formats are not implemented in any renderer. */
    assert(state.format == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8);

    state.in_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_WIDTH);
    state.in_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_IN], NV_PVIDEO_SIZE_IN_HEIGHT);
    state.out_width =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_WIDTH);
    state.out_height =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_SIZE_OUT], NV_PVIDEO_SIZE_OUT_HEIGHT);
    if (!state.out_width || !state.out_height) {
        state.enabled = false;
        return state;
    }

    state.in_s =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_S);
    state.in_t =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_IN], NV_PVIDEO_POINT_IN_T);

    uint32_t ds_dx = d->pvideo.regs[NV_PVIDEO_DS_DX];
    uint32_t dt_dy = d->pvideo.regs[NV_PVIDEO_DT_DY];
    state.scale_x = ds_dx == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(ds_dx, state.out_width);
    state.scale_y = dt_dy == NV_PVIDEO_DIN_DOUT_UNITY ?
                        1.0f :
                        pvideo_calculate_scale(dt_dy, state.out_height);

    /* Hardware caps SIZE_IN to SIZE_OUT instead of scaling. SIZE_IN can also
     * be left huge while the overlay is coming up or down. */
    if (state.in_width > state.out_width) {
        state.in_width = floorf((float)state.out_width * state.scale_x + 0.5f);
    }
    if (state.in_height > state.out_height) {
        state.in_height =
            floorf((float)state.out_height * state.scale_y + 0.5f);
    }

    state.out_x =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_X);
    state.out_y =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_POINT_OUT], NV_PVIDEO_POINT_OUT_Y);
    state.color_key_enabled =
        GET_MASK(d->pvideo.regs[NV_PVIDEO_FORMAT], NV_PVIDEO_FORMAT_DISPLAY);
    state.color_key = d->pvideo.regs[NV_PVIDEO_COLOR_KEY] & 0xFFFFFF;

    if (!state.in_width || !state.in_height || !state.out_width ||
        !state.out_height || !state.pitch) {
        state.enabled = false;
        return state;
    }

    assert(state.offset + state.pitch * state.in_height <= state.limit);
    hwaddr end =
        state.base + state.offset + (hwaddr)state.pitch * state.in_height;
    assert(end <= memory_region_size(d->vram));
    return state;
}

static WGPUSampler create_sampler(PGRAPHWgpuState *r, const char *label,
                                  WGPUFilterMode mag, WGPUFilterMode min)
{
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;

    desc.label = pgraph_wgpu_str(label);
    desc.addressModeU = WGPUAddressMode_Repeat;
    desc.addressModeV = WGPUAddressMode_Repeat;
    desc.addressModeW = WGPUAddressMode_Repeat;
    desc.magFilter = mag;
    desc.minFilter = min;
    desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    return wgpuDeviceCreateSampler(r->device, &desc);
}

static void ensure_texture(PGRAPHWgpuState *r, WGPUTexture *texture,
                           WGPUTextureView *view, unsigned int *cur_w,
                           unsigned int *cur_h, unsigned int width,
                           unsigned int height, WGPUTextureUsage usage,
                           const char *label)
{
    if (*texture && *cur_w == width && *cur_h == height) {
        return;
    }

    /* The previous frame's bind group may still hold this view. */
    release_bind_group(&r->display);
    destroy_texture(texture, view);

    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str(label);
    desc.usage = usage;
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    desc.size = (WGPUExtent3D){ width, height, 1 };
    *texture = wgpuDeviceCreateTexture(r->device, &desc);
    *view = wgpuTextureCreateView(*texture, NULL);
    *cur_w = width;
    *cur_h = height;
}

static void ensure_display_pipeline(PGRAPHWgpuState *r,
                                    WGPUTextureFormat format);

static void ensure_display_target(PGRAPHWgpuState *r, unsigned int width,
                                  unsigned int height)
{
    WgpuDisplayState *disp = &r->display;

    /*
     * The UI pass samples this image on the same device. A shared IOSurface
     * is bgra8unorm and is not sampled here. Drop it and render rgba8unorm
     * that the gamma blit can bind.
     */
    if (disp->sampleable) {
#ifndef __EMSCRIPTEN__
        if (disp->shared) {
            destroy_shared_display(r);
        }
#endif
        if (disp->image && !disp->image_sampleable) {
            destroy_texture(&disp->image, &disp->image_view);
            disp->width = 0;
            disp->height = 0;
        }
        ensure_texture(r, &disp->image, &disp->image_view, &disp->width,
                       &disp->height, width, height,
                       WGPUTextureUsage_RenderAttachment |
                           WGPUTextureUsage_CopySrc |
                           WGPUTextureUsage_TextureBinding,
                       "Display");
        disp->image_sampleable = true;
        ensure_display_pipeline(r, WGPUTextureFormat_RGBA8Unorm);
        return;
    }

#ifndef __EMSCRIPTEN__
    if (r->shared_display && !qatomic_read(&disp->shared_gl_failed)) {
        if (disp->shared && disp->image && disp->width == width &&
            disp->height == height) {
            return;
        }
        if (create_shared_display(r, width, height)) {
            return;
        }
        log_shared_failure("import failed");
        qatomic_set(&disp->shared_gl_failed, 1);
    } else if (disp->shared) {
        destroy_shared_display(r);
    }
#endif
    ensure_texture(r, &disp->image, &disp->image_view, &disp->width,
                   &disp->height, width, height,
                   WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
                   "Display");
    disp->image_sampleable = false;
    ensure_display_pipeline(r, WGPUTextureFormat_RGBA8Unorm);
}

static void upload_pvideo_image(PGRAPHState *pg, const PvideoState *state)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;
    unsigned int width = state->in_width;
    unsigned int height = state->in_height;
    uint32_t packed_row = width * 4;
    uint32_t row_bytes = ROUND_UP(packed_row, 256);

    ensure_texture(r, &disp->pvideo_image, &disp->pvideo_view,
                   &disp->pvideo_width, &disp->pvideo_height, width, height,
                   WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                   "PVIDEO");

    g_autofree uint8_t *packed = g_malloc(packed_row * height);
    g_autofree uint8_t *rows = g_malloc0((size_t)row_bytes * height);
    convert_cr8yb8cb8ya8(packed, d->vram_ptr + state->base + state->offset,
                         width, height, state->pitch);
    for (unsigned int y = 0; y < height; y++) {
        memcpy(rows + (size_t)y * row_bytes, packed + (size_t)y * packed_row,
               packed_row);
    }

    /* Queue writes run before commands that have not been submitted yet. */
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);

    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    WGPUExtent3D extent = { width, height, 1 };
    dst.texture = disp->pvideo_image;
    layout.bytesPerRow = row_bytes;
    layout.rowsPerImage = height;
    wgpuQueueWriteTexture(r->queue, &dst, rows, (size_t)row_bytes * height,
                          &layout, &extent);
}

#ifndef __EMSCRIPTEN__
static void ensure_readback(PGRAPHWgpuState *r, uint64_t size)
{
    WgpuDisplayState *disp = &r->display;

    if (disp->readback && disp->readback_size >= size) {
        return;
    }
    if (disp->readback) {
        wgpuBufferRelease(disp->readback);
    }

    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Display readback");
    desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    desc.size = ROUND_UP(size, 1024 * 1024);
    disp->readback = wgpuDeviceCreateBuffer(r->device, &desc);
    disp->readback_size = desc.size;
}

static void record_display_readback(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;
    uint32_t packed_row = disp->width * 4;
    uint32_t src_row = ROUND_UP(packed_row, 256);
    uint64_t src_size = (uint64_t)src_row * disp->height;
    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    WGPUExtent3D extent = { disp->width, disp->height, 1 };

    ensure_readback(r, src_size);
    src.texture = disp->image;
    dst.buffer = disp->readback;
    dst.layout.bytesPerRow = src_row;
    dst.layout.rowsPerImage = disp->height;
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
}

static bool map_display_readback(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;
    uint32_t packed_row = disp->width * 4;
    uint32_t src_row = ROUND_UP(packed_row, 256);
    uint64_t src_size = (uint64_t)src_row * disp->height;
    size_t packed_size = (size_t)packed_row * disp->height;
    const uint8_t *mapped =
        pgraph_wgpu_read_buffer(r, disp->readback, src_size);

    if (!mapped) {
        return false;
    }

    if (disp->pixels_size != packed_size) {
        disp->pixels = g_realloc(disp->pixels, packed_size);
        disp->pixels_size = packed_size;
    }
    for (unsigned int y = 0; y < disp->height; y++) {
        memcpy(disp->pixels + (size_t)y * packed_row,
               mapped + (size_t)y * src_row, packed_row);
    }
    wgpuBufferUnmap(disp->readback);
    return true;
}

static bool readback_display(PGRAPHState *pg)
{
    record_display_readback(pg);
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_5);
    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);
    return map_display_readback(pg);
}
#endif

static void update_bind_group(PGRAPHWgpuState *r, WGPUTextureView frame)
{
    WgpuDisplayState *disp = &r->display;
    WGPUTextureView pvideo = disp->pvideo_view ?
                                 disp->pvideo_view :
                                 r->dummy_texture_views[PSH_TEXTURE_TYPE_2D];
    WGPUBindGroupEntry entries[5];

    release_bind_group(disp);

    entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[0].binding = 0;
    entries[0].textureView = frame;

    entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[1].binding = 1;
    entries[1].sampler = disp->frame_sampler;

    entries[2] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[2].binding = 2;
    entries[2].textureView = pvideo;

    entries[3] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[3].binding = 3;
    entries[3].sampler = disp->pvideo_sampler;

    entries[4] = WGPU_BIND_GROUP_ENTRY_INIT;
    entries[4].binding = 4;
    entries[4].buffer = disp->uniform;
    entries[4].size = sizeof(DisplayParams);

    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.label = pgraph_wgpu_str("Display");
    desc.layout = disp->bind_group_layout;
    desc.entryCount = ARRAY_SIZE(entries);
    desc.entries = entries;
    disp->bind_group = wgpuDeviceCreateBindGroup(r->device, &desc);
}

static void record_display_pass(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;

    pgraph_wgpu_end_render_pass(r);
    WGPUCommandEncoder encoder = pgraph_wgpu_ensure_encoder(pg);

    WGPURenderPassColorAttachment color =
        WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = disp->image_view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = (WGPUColor){ 0, 0, 0, 1 };

    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.label = pgraph_wgpu_str("Display");
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;

    WGPURenderPassEncoder pass =
        wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    wgpuRenderPassEncoderSetPipeline(pass, disp->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, disp->bind_group, 0, NULL);
    wgpuRenderPassEncoderSetViewport(pass, 0, 0, disp->width, disp->height, 0,
                                     1);
    wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, disp->width, disp->height);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

static WGPUBindGroupLayoutEntry texture_entry(uint32_t binding)
{
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.texture.sampleType = WGPUTextureSampleType_Float;
    entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    return entry;
}

static WGPUBindGroupLayoutEntry sampler_entry(uint32_t binding)
{
    WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

    entry.binding = binding;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.sampler.type = WGPUSamplerBindingType_Filtering;
    return entry;
}

static void ensure_display_pipeline(PGRAPHWgpuState *r,
                                    WGPUTextureFormat format)
{
    WgpuDisplayState *disp = &r->display;
    WGPUPipelineLayoutDescriptor layout_desc =
        WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    WGPUColorTargetState color_target = WGPU_COLOR_TARGET_STATE_INIT;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    WGPUPipelineLayout layout;

    if (disp->pipeline && disp->format == format) {
        return;
    }
    if (disp->pipeline) {
        wgpuRenderPipelineRelease(disp->pipeline);
        disp->pipeline = NULL;
    }

    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &disp->bind_group_layout;
    layout = wgpuDeviceCreatePipelineLayout(r->device, &layout_desc);

    color_target.format = format;
    color_target.writeMask = WGPUColorWriteMask_Red | WGPUColorWriteMask_Green |
                             WGPUColorWriteMask_Blue | WGPUColorWriteMask_Alpha;

    fragment.module = disp->module;
    fragment.entryPoint = pgraph_wgpu_str("fs");
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    desc.label = pgraph_wgpu_str("Display");
    desc.layout = layout;
    desc.vertex.module = disp->module;
    desc.vertex.entryPoint = pgraph_wgpu_str("vs");
    desc.fragment = &fragment;
    disp->pipeline = wgpuDeviceCreateRenderPipeline(r->device, &desc);
    disp->format = format;
    wgpuPipelineLayoutRelease(layout);
}

void pgraph_wgpu_init_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;
    WGPUBindGroupLayoutEntry entries[5];

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    disp->drm_fd = -1;
    disp->prime_fd = -1;
#endif
#ifndef __EMSCRIPTEN__
    wgpu_window_init(&display_window);
#endif

    disp->module = pgraph_wgpu_create_wgsl_module(r, "Display", display_wgsl);

    entries[0] = texture_entry(0);
    entries[1] = sampler_entry(1);
    entries[2] = texture_entry(2);
    entries[3] = sampler_entry(3);
    entries[4] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    entries[4].binding = 4;
    entries[4].visibility = WGPUShaderStage_Fragment;
    entries[4].buffer.type = WGPUBufferBindingType_Uniform;
    entries[4].buffer.minBindingSize = sizeof(DisplayParams);

    WGPUBindGroupLayoutDescriptor bgl_desc =
        WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl_desc.label = pgraph_wgpu_str("Display");
    bgl_desc.entryCount = ARRAY_SIZE(entries);
    bgl_desc.entries = entries;
    disp->bind_group_layout =
        wgpuDeviceCreateBindGroupLayout(r->device, &bgl_desc);

    ensure_display_pipeline(r, WGPUTextureFormat_RGBA8Unorm);

    disp->frame_sampler = create_sampler(
        r, "Display frame", WGPUFilterMode_Nearest, WGPUFilterMode_Nearest);
    disp->pvideo_sampler = create_sampler(
        r, "Display PVIDEO", WGPUFilterMode_Linear, WGPUFilterMode_Nearest);

    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = pgraph_wgpu_str("Display uniforms");
    buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    buffer_desc.size = sizeof(DisplayParams);
    disp->uniform = wgpuDeviceCreateBuffer(r->device, &buffer_desc);
}

#ifdef __EMSCRIPTEN__
bool pgraph_wgpu_display_canvas_describe(PGRAPHWgpuState *r,
                                         WGPUTextureFormat *format, int *width,
                                         int *height)
{
    uint32_t w = 0;
    uint32_t h = 0;
    WGPUTextureFormat fmt = WGPUTextureFormat_Undefined;

    if (!r->instance || !r->adapter || !r->device ||
        !wgpu_canvas_describe(&display_canvas, r->instance, r->adapter,
                              r->device, &fmt, &w, &h)) {
        return false;
    }
    if (format) {
        *format = fmt;
    }
    if (width) {
        *width = (int)w;
    }
    if (height) {
        *height = (int)h;
    }
    return true;
}

bool pgraph_wgpu_display_canvas_acquire(PGRAPHWgpuState *r,
                                        WGPUTextureView *view,
                                        WGPUTextureFormat *format, int *width,
                                        int *height)
{
    static int logged;
    uint32_t w = 0;
    uint32_t h = 0;
    WGPUTextureFormat fmt = WGPUTextureFormat_Undefined;
    WGPUTextureView acquired = NULL;

    if (!r->instance || !r->adapter || !r->device ||
        !wgpu_canvas_acquire(&display_canvas, r->instance, r->adapter,
                             r->device, &fmt, &w, &h, &acquired)) {
        return false;
    }
    if (view) {
        *view = acquired;
    }
    if (format) {
        *format = fmt;
    }
    if (width) {
        *width = (int)w;
    }
    if (height) {
        *height = (int)h;
    }
    if (!logged) {
        fprintf(stderr, "[WebGPU] Presenting to the canvas (%ux%u)\n", w, h);
        logged = 1;
    }
    return true;
}

void pgraph_wgpu_display_canvas_release(PGRAPHWgpuState *r)
{
    (void)r;
    wgpu_canvas_release(&display_canvas);
}
#else
bool pgraph_wgpu_display_window_active(void)
{
    return wgpu_window_active();
}

bool pgraph_wgpu_display_window_describe(PGRAPHWgpuState *r,
                                         WGPUTextureFormat *format, int *width,
                                         int *height)
{
    uint32_t w = 0;
    uint32_t h = 0;
    WGPUTextureFormat fmt = WGPUTextureFormat_Undefined;

    if (!r->instance || !r->adapter || !r->device ||
        !wgpu_window_describe(&display_window, r->instance, r->adapter,
                              r->device, &fmt, &w, &h)) {
        return false;
    }
    if (format) {
        *format = fmt;
    }
    if (width) {
        *width = (int)w;
    }
    if (height) {
        *height = (int)h;
    }
    return true;
}

bool pgraph_wgpu_display_window_acquire(PGRAPHWgpuState *r,
                                        WGPUTextureView *view,
                                        WGPUTextureFormat *format, int *width,
                                        int *height)
{
    uint32_t w = 0;
    uint32_t h = 0;
    WGPUTextureFormat fmt = WGPUTextureFormat_Undefined;
    WGPUTextureView acquired = NULL;

    if (!r->instance || !r->adapter || !r->device ||
        !wgpu_window_acquire(&display_window, r->instance, r->adapter,
                             r->device, &fmt, &w, &h, &acquired)) {
        return false;
    }
    if (view) {
        *view = acquired;
    }
    if (format) {
        *format = fmt;
    }
    if (width) {
        *width = (int)w;
    }
    if (height) {
        *height = (int)h;
    }
    return true;
}

bool pgraph_wgpu_display_window_present(PGRAPHWgpuState *r)
{
    (void)r;
    return wgpu_window_present(&display_window);
}

void pgraph_wgpu_display_window_release(PGRAPHWgpuState *r)
{
    (void)r;
    wgpu_window_release(&display_window);
}
#endif

void pgraph_wgpu_finalize_display(PGRAPHState *pg)
{
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;

#ifdef __EMSCRIPTEN__
    wgpu_canvas_fini(&display_canvas);
#else
    wgpu_window_fini(&display_window);
#endif
    release_bind_group(disp);
#ifndef __EMSCRIPTEN__
    destroy_shared_display(r);
    finalize_shared_os(disp);
#else
    destroy_texture(&disp->image, &disp->image_view);
#endif
    destroy_texture(&disp->pvideo_image, &disp->pvideo_view);
    disp->width = 0;
    disp->height = 0;
    disp->pvideo_width = 0;
    disp->pvideo_height = 0;

    if (disp->readback) {
        wgpuBufferRelease(disp->readback);
        disp->readback = NULL;
        disp->readback_size = 0;
    }
    g_free(disp->pixels);
    disp->pixels = NULL;
    disp->pixels_size = 0;
    disp->pixels_valid = false;

    if (disp->uniform) {
        wgpuBufferRelease(disp->uniform);
        disp->uniform = NULL;
    }
    if (disp->frame_sampler) {
        wgpuSamplerRelease(disp->frame_sampler);
        disp->frame_sampler = NULL;
    }
    if (disp->pvideo_sampler) {
        wgpuSamplerRelease(disp->pvideo_sampler);
        disp->pvideo_sampler = NULL;
    }
    if (disp->pipeline) {
        wgpuRenderPipelineRelease(disp->pipeline);
        disp->pipeline = NULL;
    }
    if (disp->bind_group_layout) {
        wgpuBindGroupLayoutRelease(disp->bind_group_layout);
        disp->bind_group_layout = NULL;
    }
    if (disp->module) {
        wgpuShaderModuleRelease(disp->module);
        disp->module = NULL;
    }

#ifdef CONFIG_OPENGL
    retire_gl_objects(disp->gl_texture, disp->gl_memory);
    disp->gl_texture = 0;
    disp->gl_memory = 0;
    disp->gl_width = 0;
    disp->gl_height = 0;
    disp->gl_serial = 0;
#endif
}

void pgraph_wgpu_render_display(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHWgpuState *r = pg->wgpu_renderer_state;
    WgpuDisplayState *disp = &r->display;

    disp->pixels_valid = false;
    disp->presented = false;
    if (!disp->pipeline) {
        return;
    }

    VGADisplayParams vga_display_params;
    d->vga.get_params(&d->vga, &vga_display_params);

    SurfaceBinding *surface = pgraph_wgpu_surface_get_within(
        d, d->pcrtc.start + vga_display_params.line_offset);
    if (surface == NULL || !surface->color || !surface->width ||
        !surface->height || !surface->view) {
        return;
    }

    unsigned int width = 0, height = 0;
    d->vga.get_resolution(&d->vga, (int *)&width, (int *)&height);
    if (d->vga.cr[NV_PRMCIO_INTERLACE_MODE] !=
        NV_PRMCIO_INTERLACE_MODE_DISABLED) {
        height *= 2;
    }
    pgraph_apply_scaling_factor(pg, &width, &height);
    if (!width || !height) {
        return;
    }

    if (r->encoder && surface->draw_time >= r->encoder_start_time) {
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);
    }
    pgraph_wgpu_upload_surface_data(d, surface, !tcg_enabled());

    PvideoState pvideo = get_pvideo_state(pg);
    if (pvideo.enabled) {
        pgraph_wgpu_download_surfaces_in_range_if_dirty(
            pg, pvideo.base + pvideo.offset,
            (hwaddr)pvideo.pitch * pvideo.in_height);
        upload_pvideo_image(pg, &pvideo);
    }

    ensure_display_target(r, width, height);

    DisplayParams params = { 0 };
    int line_offset = vga_display_params.line_offset ?
                          surface->pitch / vga_display_params.line_offset :
                          1;
    params.line_offset = line_offset ? line_offset : 1;
    params.display_size[0] = width;
    params.display_size[1] = height;
    params.pvideo_enable = pvideo.enabled;
    if (pvideo.enabled) {
        params.pvideo_color_key_enable = pvideo.color_key_enabled;
        params.pvideo_color_key[0] =
            GET_MASK(pvideo.color_key, NV_PVIDEO_COLOR_KEY_RED) / 255.0f;
        params.pvideo_color_key[1] =
            GET_MASK(pvideo.color_key, NV_PVIDEO_COLOR_KEY_GREEN) / 255.0f;
        params.pvideo_color_key[2] =
            GET_MASK(pvideo.color_key, NV_PVIDEO_COLOR_KEY_BLUE) / 255.0f;
        params.pvideo_in_pos[0] = pvideo.in_s / 16.f;
        params.pvideo_in_pos[1] = pvideo.in_t / 8.f;
        params.pvideo_pos[0] = pvideo.out_x;
        params.pvideo_pos[1] = pvideo.out_y;
        params.pvideo_pos[2] = pvideo.out_width;
        params.pvideo_pos[3] = pvideo.out_height;
        params.pvideo_scale[0] = pvideo.scale_x;
        params.pvideo_scale[1] = pvideo.scale_y;
        unsigned int scale = MAX(pg->surface_scale_factor, 1);
        params.pvideo_scale[2] = 1.0f / scale;
        params.pvideo_scale[3] = 1.0f;
    }

    pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);
    wgpuQueueWriteBuffer(r->queue, disp->uniform, 0, &params, sizeof(params));
    update_bind_group(r, surface->view);

#ifndef __EMSCRIPTEN__
    if (disp->shared) {
        if (!begin_shared_access(r)) {
            log_shared_failure("begin failed");
            qatomic_set(&disp->shared_gl_failed, 1);
            destroy_shared_display(r);
            ensure_texture(r, &disp->image, &disp->image_view, &disp->width,
                           &disp->height, width, height,
                           WGPUTextureUsage_RenderAttachment |
                               WGPUTextureUsage_CopySrc,
                           "Display");
            ensure_display_pipeline(r, WGPUTextureFormat_RGBA8Unorm);
        } else {
            record_display_pass(pg);
            pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);
            if (!end_shared_access(r)) {
                log_shared_failure("end failed");
                qatomic_set(&disp->shared_gl_failed, 1);
            }
            pgraph_wgpu_wait_idle(r);
            return;
        }
    }
#endif
    record_display_pass(pg);
    disp->presented = true;
    if (disp->sampleable) {
        /* The UI pass samples this image in a later submit. Skip the
         * CPU readback. Queue order makes the sample see this pass. */
        pgraph_wgpu_finish(pg, WGPU_FINISH_REASON_PRESENTING, false);
        return;
    }
#ifdef __EMSCRIPTEN__
    /* The canvas is the present. Guest RAM is not a copy of this image. */
    return;
#else
    disp->pixels_valid = readback_display(pg);
#endif
}

#ifdef CONFIG_OPENGL
static void drop_gl_binding(WgpuDisplayState *disp)
{
    if (disp->gl_texture) {
        glDeleteTextures(1, &disp->gl_texture);
        disp->gl_texture = 0;
    }
#if defined(__linux__) || defined(_WIN32)
    if (disp->gl_memory) {
        glDeleteMemoryObjectsEXT(1, &disp->gl_memory);
        disp->gl_memory = 0;
    }
#endif
#ifdef __APPLE__
    delete_iosurface_gl();
#endif
    disp->gl_width = 0;
    disp->gl_height = 0;
    disp->gl_serial = 0;
}

#ifndef __EMSCRIPTEN__
#ifdef __APPLE__
static bool bind_shared_display(WgpuDisplayState *disp)
{
    CGLContextObj ctx = CGLGetCurrentContext();
    GLint previous_draw = 0;
    GLint previous_read = 0;
    GLenum status;

    if (!ctx || !disp->io_surface) {
        return false;
    }
    if (!iosurface_rect) {
        glGenTextures(1, &iosurface_rect);
    }
    if (!disp->gl_texture) {
        glGenTextures(1, &disp->gl_texture);
    }
    if (!iosurface_read_fbo) {
        glGenFramebuffers(1, &iosurface_read_fbo);
    }
    if (!iosurface_draw_fbo) {
        glGenFramebuffers(1, &iosurface_draw_fbo);
    }

    /*
     * GL_BGRA + UNSIGNED_INT_8_8_8_8_REV is the 8-bit pair CGL accepts.
     * On little-endian the first byte is blue, which matches the
     * bgra8unorm IOSurface. The target has to be a rectangle texture,
     * which the UI cannot sample, so blit it to 2D.
     */
    glBindTexture(GL_TEXTURE_RECTANGLE, iosurface_rect);
    if (disp->gl_serial != disp->shared_serial) {
        CGLError err = CGLTexImageIOSurface2D(
            ctx, GL_TEXTURE_RECTANGLE, GL_RGBA8, disp->width, disp->height,
            GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
            (IOSurfaceRef)disp->io_surface, 0);

        if (err != kCGLNoError) {
            fprintf(stderr, "[WebGPU] CGLTexImageIOSurface2D failed (%d)\n",
                    err);
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, disp->gl_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, disp->width, disp->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        disp->gl_width = disp->width;
        disp->gl_height = disp->height;
        disp->gl_serial = disp->shared_serial;
    }

    /* Rebind so this context sees Metal's writes to the IOSurface. */
    glBindTexture(GL_TEXTURE_RECTANGLE, iosurface_rect);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, iosurface_read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_RECTANGLE, iosurface_rect, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, iosurface_draw_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, disp->gl_texture, 0);
    status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE ||
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr,
                "[WebGPU] IOSurface blit framebuffer incomplete "
                "(%u)\n",
                status);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
        return false;
    }
    glBlitFramebuffer(0, 0, disp->width, disp->height, 0, 0, disp->width,
                      disp->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
    return true;
}
#elif defined(__linux__)
static bool bind_shared_display(WgpuDisplayState *disp)
{
    int fd;
    GLuint memory = 0;
    GLuint texture = 0;

    if (disp->gl_texture && disp->gl_memory &&
        disp->gl_serial == disp->shared_serial) {
        glBindTexture(GL_TEXTURE_2D, disp->gl_texture);
        return true;
    }
    if (disp->prime_fd < 0 || !disp->prime_size) {
        return false;
    }
    fd = dup(disp->prime_fd);
    if (fd < 0) {
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {
    }
    glCreateMemoryObjectsEXT(1, &memory);
    glImportMemoryFdEXT(memory, disp->prime_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT,
                        fd);
    if (glGetError() != GL_NO_ERROR) {
        close(fd);
        if (memory) {
            glDeleteMemoryObjectsEXT(1, &memory);
        }
        return false;
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_LINEAR_TILING_EXT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, disp->width, disp->height,
                         memory, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        glDeleteMemoryObjectsEXT(1, &memory);
        return false;
    }

    drop_gl_binding(disp);
    disp->gl_texture = texture;
    disp->gl_memory = memory;
    disp->gl_width = disp->width;
    disp->gl_height = disp->height;
    disp->gl_serial = disp->shared_serial;
    return true;
}
#elif defined(_WIN32)
static bool bind_shared_display(WgpuDisplayState *disp)
{
    GLuint memory = 0;
    GLuint texture = 0;
    GLuint64 size = (GLuint64)disp->width * disp->height * 4;

    if (disp->gl_texture && disp->gl_memory &&
        disp->gl_serial == disp->shared_serial) {
        glBindTexture(GL_TEXTURE_2D, disp->gl_texture);
        return true;
    }
    if (!disp->shared_handle) {
        return false;
    }

    while (glGetError() != GL_NO_ERROR) {
    }
    glCreateMemoryObjectsEXT(1, &memory);
    glImportMemoryWin32HandleEXT(
        memory, size, GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT, disp->shared_handle);
    if (glGetError() != GL_NO_ERROR) {
        if (memory) {
            glDeleteMemoryObjectsEXT(1, &memory);
        }
        return false;
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT,
                    GL_OPTIMAL_TILING_EXT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, disp->width, disp->height,
                         memory, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        glDeleteMemoryObjectsEXT(1, &memory);
        return false;
    }

    drop_gl_binding(disp);
    disp->gl_texture = texture;
    disp->gl_memory = memory;
    disp->gl_width = disp->width;
    disp->gl_height = disp->height;
    disp->gl_serial = disp->shared_serial;
    return true;
}
#else
static bool bind_shared_display(WgpuDisplayState *disp)
{
    (void)disp;
    return false;
}
#endif
#endif

GLuint pgraph_wgpu_present_display(PGRAPHWgpuState *r)
{
    WgpuDisplayState *disp = &r->display;

    delete_retired_gl_objects();

#ifndef __EMSCRIPTEN__
    if (disp->shared && !qatomic_read(&disp->shared_gl_failed)) {
        static int logged;

        if (bind_shared_display(disp)) {
            if (!logged) {
                fprintf(stderr, "[WebGPU] Display image is shared with GL\n");
                logged = 1;
            }
            return disp->gl_texture;
        }
        log_shared_failure("GL bind failed");
        qatomic_set(&disp->shared_gl_failed, 1);
        drop_gl_binding(disp);
    }
#endif

    if (!disp->pixels_valid || !disp->pixels || !disp->width || !disp->height) {
        return 0;
    }

    if (!disp->gl_texture) {
        glGenTextures(1, &disp->gl_texture);
    }
    glBindTexture(GL_TEXTURE_2D, disp->gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (disp->gl_width != disp->width || disp->gl_height != disp->height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, disp->width, disp->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, disp->pixels);
        disp->gl_width = disp->width;
        disp->gl_height = disp->height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, disp->width, disp->height,
                        GL_RGBA, GL_UNSIGNED_BYTE, disp->pixels);
    }
    return disp->gl_texture;
}
#endif
