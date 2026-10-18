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

/*
 * Dawn's backend validation level lives on DawnInstanceDescriptor, which is
 * a C++ type. The rest of the renderer stays in C and calls this when
 * display.webgpu.validation is set.
 */

#include <dawn/native/DawnNative.h>

#include "validation.h"

WGPUInstance
pgraph_wgpu_create_validated_instance(const WGPUInstanceDescriptor *base)
{
    wgpu::InstanceDescriptor descriptor = {};

    descriptor.requiredFeatureCount = base->requiredFeatureCount;
    descriptor.requiredFeatures =
        reinterpret_cast<wgpu::InstanceFeatureName const *>(
            base->requiredFeatures);
    descriptor.nextInChain =
        reinterpret_cast<wgpu::ChainedStruct const *>(base->nextInChain);

    dawn::native::DawnInstanceDescriptor dawn_desc;
    dawn_desc.backendValidationLevel =
        dawn::native::BackendValidationLevel::Full;
    dawn_desc.nextInChain = descriptor.nextInChain;
    descriptor.nextInChain = &dawn_desc;

    return wgpu::CreateInstance(&descriptor).MoveToCHandle();
}
