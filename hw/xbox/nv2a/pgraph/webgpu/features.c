/*
 * Geforce NV2A PGRAPH WebGPU Renderer
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "features.h"

bool pgraph_wgpu_feature_disabled(const char *list, const char *name)
{
    size_t name_len;

    if (!list || !list[0] || !name || !name[0]) {
        return false;
    }
    name_len = strlen(name);
    while (*list) {
        const char *comma = strchr(list, ',');
        size_t len = comma ? (size_t)(comma - list) : strlen(list);

        while (len && (*list == ' ' || *list == '\t')) {
            list++;
            len--;
        }
        while (len && (list[len - 1] == ' ' || list[len - 1] == '\t')) {
            len--;
        }
        if (len == name_len && !memcmp(list, name, name_len)) {
            return true;
        }
        if (!comma) {
            break;
        }
        list = comma + 1;
    }
    return false;
}

WgpuZetaBacking pgraph_wgpu_zeta_backing(bool depth32float_stencil8, bool z16)
{
    if (depth32float_stencil8) {
        return WGPU_ZETA_BACKING_DEPTH32_STENCIL8;
    }
    return z16 ? WGPU_ZETA_BACKING_DEPTH32 :
                 WGPU_ZETA_BACKING_DEPTH24PLUS_STENCIL8;
}
