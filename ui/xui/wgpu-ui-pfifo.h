/*
 * UI jobs the PFIFO thread runs on the NV2A WebGPU device.
 *
 * The UI thread fills the job and waits. WebGPU objects stay on the PFIFO
 * thread, which is the only thread that may use the device.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_WEBGPU
#include <webgpu/webgpu.h>

void xemu_wgpu_ui_execute(WGPUInstance instance, WGPUDevice device,
                          WGPUQueue queue, WGPUTextureView display,
                          int display_width, int display_height,
                          WGPUTextureView canvas, int canvas_width,
                          int canvas_height, WGPUTextureFormat canvas_format);
bool xemu_wgpu_ui_needs_display(void);

/* Poll, then suspend through Asyncify or JSPI when the future is still
 * pending. A finished future does not unwind. */
bool xemu_wgpu_wait(WGPUInstance instance, WGPUFuture future);
/* A synchronous buffer map. Counted with the waits and logged per guest
 * frame. */
void xemu_wgpu_note_download(void);
void xemu_wgpu_wait_frame(void);
#endif

#ifdef __cplusplus
}
#endif
