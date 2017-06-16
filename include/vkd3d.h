/*
 * Copyright 2016 Józef Kucia for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __VKD3D_H
#define __VKD3D_H

#include "vkd3d_windows.h"
#include "d3d12.h"
#include <vulkan/vulkan.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

typedef bool (*vkd3d_signal_event_pfn)(HANDLE event);

struct vkd3d_device_create_info
{
    D3D_FEATURE_LEVEL minimum_feature_level;
    vkd3d_signal_event_pfn signal_event_pfn;
};

/* resource flags */
#define VKD3D_RESOURCE_INITIAL_STATE_TRANSITION 0x00000001
#define VKD3D_RESOURCE_SWAPCHAIN_IMAGE          0x00000002

HRESULT vkd3d_create_device(const struct vkd3d_device_create_info *create_info,
        REFIID riid, void **device);
HRESULT vkd3d_create_image_resource(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc,
        VkImage vk_image, unsigned int resource_flags, ID3D12Resource **resource);
VkDevice vkd3d_get_vk_device(ID3D12Device *device);
VkFormat vkd3d_get_vk_format(DXGI_FORMAT format);
VkInstance vkd3d_get_vk_instance(ID3D12Device *device);
VkPhysicalDevice vkd3d_get_vk_physical_device(ID3D12Device *device);
VkQueue vkd3d_get_vk_queue(ID3D12CommandQueue *queue);
uint32_t vkd3d_get_vk_queue_family_index(ID3D12CommandQueue *queue);

#ifdef __cplusplus
}
#endif  /* __cplusplus */

#endif  /* __VKD3D_H */
