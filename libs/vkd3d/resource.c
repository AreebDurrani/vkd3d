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

#include "vkd3d_private.h"

static VkImageType vk_image_type_from_d3d12_resource_dimension(D3D12_RESOURCE_DIMENSION dimension)
{
    switch (dimension)
    {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            return VK_IMAGE_TYPE_1D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            return VK_IMAGE_TYPE_2D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            return VK_IMAGE_TYPE_3D;
        default:
            ERR("Invalid resource dimension %#x.\n", dimension);
            return VK_IMAGE_TYPE_2D;
    }
}

static VkSampleCountFlagBits vk_samples_from_dxgi_sample_desc(const DXGI_SAMPLE_DESC *desc)
{
    switch (desc->Count)
    {
        case 1:
            return VK_SAMPLE_COUNT_1_BIT;
        case 2:
            return VK_SAMPLE_COUNT_2_BIT;
        case 4:
            return VK_SAMPLE_COUNT_4_BIT;
        case 8:
            return VK_SAMPLE_COUNT_8_BIT;
        case 16:
            return VK_SAMPLE_COUNT_16_BIT;
        case 32:
            return VK_SAMPLE_COUNT_32_BIT;
        case 64:
            return VK_SAMPLE_COUNT_64_BIT;
        default:
            FIXME("Unhandled sample count %u.\n", desc->Count);
            return VK_SAMPLE_COUNT_1_BIT;
    }
}

HRESULT vkd3d_create_buffer(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc, VkBuffer *vk_buffer)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkBufferCreateInfo buffer_info;
    VkResult vr;

    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext = NULL;
    buffer_info.flags = 0;
    buffer_info.size = desc->Width;

    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
            | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
            | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
            | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    if (heap_properties->Type == D3D12_HEAP_TYPE_UPLOAD)
        buffer_info.usage &= ~VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    else if (heap_properties->Type == D3D12_HEAP_TYPE_READBACK)
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        buffer_info.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (!(desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        buffer_info.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;

    /* FIXME: Buffers always can be accessed from multiple queues. */
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    buffer_info.queueFamilyIndexCount = 0;
    buffer_info.pQueueFamilyIndices = 0;

    if ((vr = VK_CALL(vkCreateBuffer(device->vk_device, &buffer_info, NULL, vk_buffer))) < 0)
    {
        WARN("Failed to create Vulkan buffer, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

static HRESULT vkd3d_create_image(struct d3d12_resource *resource, struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    const struct vkd3d_format *format;
    VkImageCreateInfo image_info;
    VkResult vr;

    if (!(format = vkd3d_format_from_d3d12_resource_desc(desc, 0)))
    {
        WARN("Invalid DXGI format %#x.\n", desc->Format);
        return E_INVALIDARG;
    }

    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = NULL;
    image_info.flags = 0;
    if (!(desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && dxgi_format_is_typeless(desc->Format))
        image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc->Width == desc->Height)
        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    image_info.imageType = vk_image_type_from_d3d12_resource_dimension(desc->Dimension);
    image_info.format = format->vk_format;
    image_info.extent.width = desc->Width;
    image_info.extent.height = desc->Height;

    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    {
        image_info.extent.depth = desc->DepthOrArraySize;
        image_info.arrayLayers = 1;
    }
    else
    {
        image_info.extent.depth = 1;
        image_info.arrayLayers = desc->DepthOrArraySize;
    }

    image_info.mipLevels = desc->MipLevels;
    image_info.samples = vk_samples_from_dxgi_sample_desc(&desc->SampleDesc);

    if (desc->Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN)
    {
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    }
    else if (desc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
    {
        image_info.tiling = VK_IMAGE_TILING_LINEAR;
    }
    else
    {
        FIXME("Unsupported layout %#x.\n", desc->Layout);
        return E_NOTIMPL;
    }

    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        image_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        image_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        image_info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!(desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
        FIXME("Ignoring D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS.\n");

    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = 0;
    image_info.pQueueFamilyIndices = NULL;

    image_info.initialLayout = is_cpu_accessible_heap(heap_properties) ?
            VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED;

    if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, &resource->u.vk_image))) < 0)
    {
        WARN("Failed to create Vulkan image, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

static unsigned int vkd3d_select_memory_type(struct d3d12_device *device, uint32_t memory_type_mask,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags)
{
    VkPhysicalDeviceMemoryProperties *memory_info = &device->memory_properties;
    VkMemoryPropertyFlags required_flags;
    unsigned int i;

    switch (heap_properties->Type)
    {
        case D3D12_HEAP_TYPE_DEFAULT:
            required_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;

        case D3D12_HEAP_TYPE_CUSTOM:
            FIXME("Custom heaps not supported yet.\n");
        case D3D12_HEAP_TYPE_UPLOAD:
        case D3D12_HEAP_TYPE_READBACK:
            required_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;

        default:
            WARN("Invalid heap type %#x.\n", heap_properties->Type);
            return ~0u;
    }

    for (i = 0; i < memory_info->memoryTypeCount; ++i)
    {
        if (!(memory_type_mask & (1u << i)))
            continue;
        if ((memory_info->memoryTypes[i].propertyFlags & required_flags) == required_flags)
            return i;
    }

    return ~0u;
}

static HRESULT vkd3d_allocate_device_memory(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const VkMemoryRequirements *memory_requirements, VkDeviceMemory *vk_memory)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkMemoryAllocateInfo allocate_info;
    VkResult vr;

    TRACE("Memory requirements: size %#"PRIx64", alignment %#"PRIx64".\n",
            memory_requirements->size, memory_requirements->alignment);

    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = NULL;
    allocate_info.allocationSize = memory_requirements->size;
    allocate_info.memoryTypeIndex = vkd3d_select_memory_type(device,
            memory_requirements->memoryTypeBits, heap_properties, heap_flags);

    if (allocate_info.memoryTypeIndex == ~0u)
    {
        FIXME("Failed to find suitable memory type (allowed types %#x).\n", memory_requirements->memoryTypeBits);
        *vk_memory = VK_NULL_HANDLE;
        return E_FAIL;
    }

    TRACE("Allocating memory type %u.\n", allocate_info.memoryTypeIndex);

    if ((vr = VK_CALL(vkAllocateMemory(device->vk_device, &allocate_info, NULL, vk_memory))) < 0)
    {
        WARN("Failed to allocate device memory, vr %d.\n", vr);
        *vk_memory = VK_NULL_HANDLE;
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

HRESULT vkd3d_allocate_buffer_memory(struct d3d12_device *device, VkBuffer vk_buffer,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        VkDeviceMemory *vk_memory)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkMemoryRequirements memory_requirements;
    VkResult vr;
    HRESULT hr;

    VK_CALL(vkGetBufferMemoryRequirements(device->vk_device, vk_buffer, &memory_requirements));
    if (FAILED(hr = vkd3d_allocate_device_memory(device, heap_properties, heap_flags,
            &memory_requirements, vk_memory)))
        return hr;

    if ((vr = VK_CALL(vkBindBufferMemory(device->vk_device, vk_buffer, *vk_memory, 0))) < 0)
    {
        WARN("Failed to bind memory, vr %d.\n", vr);
        VK_CALL(vkFreeMemory(device->vk_device, *vk_memory, NULL));
        *vk_memory = VK_NULL_HANDLE;
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

static HRESULT vkd3d_allocate_image_memory(struct d3d12_resource *resource, struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkMemoryRequirements memory_requirements;
    VkResult vr;
    HRESULT hr;

    assert(D3D12_RESOURCE_DIMENSION_TEXTURE1D <= resource->desc.Dimension
            && resource->desc.Dimension <= D3D12_RESOURCE_DIMENSION_TEXTURE3D);

    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, resource->u.vk_image, &memory_requirements));
    if (FAILED(hr = vkd3d_allocate_device_memory(device, heap_properties, heap_flags,
            &memory_requirements, &resource->vk_memory)))
        return hr;

    if ((vr = VK_CALL(vkBindImageMemory(device->vk_device, resource->u.vk_image, resource->vk_memory, 0))) < 0)
    {
        WARN("Failed to bind memory, vr %d.\n", vr);
        VK_CALL(vkFreeMemory(device->vk_device, resource->vk_memory, NULL));
        resource->vk_memory = VK_NULL_HANDLE;
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

static void d3d12_resource_destroy(struct d3d12_resource *resource, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    if (resource->flags & VKD3D_RESOURCE_EXTERNAL)
        return;

    if (resource->gpu_address)
        vkd3d_gpu_va_allocator_free(&device->gpu_va_allocator, resource->gpu_address);

    if (d3d12_resource_is_buffer(resource))
        VK_CALL(vkDestroyBuffer(device->vk_device, resource->u.vk_buffer, NULL));
    else
        VK_CALL(vkDestroyImage(device->vk_device, resource->u.vk_image, NULL));

    if (resource->vk_memory)
        VK_CALL(vkFreeMemory(device->vk_device, resource->vk_memory, NULL));
}

/* ID3D12Resource */
static inline struct d3d12_resource *impl_from_ID3D12Resource(ID3D12Resource *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_resource, ID3D12Resource_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_QueryInterface(ID3D12Resource *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12Resource)
            || IsEqualGUID(riid, &IID_ID3D12Pageable)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12Resource_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_resource_AddRef(ID3D12Resource *iface)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);
    ULONG refcount = InterlockedIncrement(&resource->refcount);

    TRACE("%p increasing refcount to %u.\n", resource, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_resource_Release(ID3D12Resource *iface)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);
    ULONG refcount = InterlockedDecrement(&resource->refcount);

    TRACE("%p decreasing refcount to %u.\n", resource, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = resource->device;

        d3d12_resource_destroy(resource, device);
        vkd3d_free(resource);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_GetPrivateData(ID3D12Resource *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    FIXME("iface %p, guid %s, data_size %p, data %p stub!", iface, debugstr_guid(guid), data_size, data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_SetPrivateData(ID3D12Resource *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    FIXME("iface %p, guid %s, data_size %u, data %p stub!\n", iface, debugstr_guid(guid), data_size, data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_SetPrivateDataInterface(ID3D12Resource *iface,
        REFGUID guid, const IUnknown *data)
{
    FIXME("iface %p, guid %s, data %p stub!\n", iface, debugstr_guid(guid), data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_SetName(ID3D12Resource *iface, const WCHAR *name)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);

    FIXME("iface %p, name %s stub!\n", iface, debugstr_w(name, resource->device->wchar_size));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_GetDevice(ID3D12Resource *iface,
        REFIID riid, void **device)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return ID3D12Device_QueryInterface(&resource->device->ID3D12Device_iface, riid, device);
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_Map(ID3D12Resource *iface, UINT sub_resource,
        const D3D12_RANGE *read_range, void **data)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    struct d3d12_device *device;
    VkResult vr;

    TRACE("iface %p, sub_resource %u, read_range %p, data %p.\n",
            iface, sub_resource, read_range, data);

    device = resource->device;
    vk_procs = &device->vk_procs;

    if (!is_cpu_accessible_heap(&resource->heap_properties))
    {
        WARN("Resource is not CPU accessible.\n");
        return E_INVALIDARG;
    }

    if (d3d12_resource_is_texture(resource))
    {
        /* Textures seem to be mappable only on UMA adapters. */
        FIXME("Not implemented for textures.\n");
        return E_INVALIDARG;
    }

    if (!resource->vk_memory)
    {
        FIXME("Not implemented for this resource type.\n");
        return E_NOTIMPL;
    }

    WARN("Ignoring read range %p.\n", read_range);

    if (!resource->map_count)
    {
        if ((vr = VK_CALL(vkMapMemory(device->vk_device, resource->vk_memory,
                0, VK_WHOLE_SIZE, 0, &resource->map_data))) < 0)
        {
            WARN("Failed to map device memory, vr %d.\n", vr);
            return hresult_from_vk_result(vr);
        }
    }

    *data = resource->map_data;
    ++resource->map_count;

    return S_OK;
}

static void STDMETHODCALLTYPE d3d12_resource_Unmap(ID3D12Resource *iface, UINT sub_resource,
        const D3D12_RANGE *written_range)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    struct d3d12_device *device;

    TRACE("iface %p, sub_resource %u, written_range %p.\n",
            iface, sub_resource, written_range);

    device = resource->device;
    vk_procs = &device->vk_procs;

    if (d3d12_resource_is_texture(resource))
    {
        FIXME("Not implemented for textures.\n");
        return;
    }

    if (!resource->map_count)
    {
        WARN("Resource %p is not mapped.\n", resource);
        return;
    }

    --resource->map_count;
    if (!resource->map_count)
    {
        resource->map_data = NULL;
        VK_CALL(vkUnmapMemory(device->vk_device, resource->vk_memory));
    }
}

static D3D12_RESOURCE_DESC * STDMETHODCALLTYPE d3d12_resource_GetDesc(ID3D12Resource *iface,
        D3D12_RESOURCE_DESC *resource_desc)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);

    TRACE("iface %p, resource_desc %p.\n", iface, resource_desc);

    *resource_desc = resource->desc;
    return resource_desc;
}

static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE d3d12_resource_GetGPUVirtualAddress(ID3D12Resource *iface)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);

    TRACE("iface %p.\n", iface);

    return resource->gpu_address;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_WriteToSubresource(ID3D12Resource *iface,
        UINT dst_sub_resource, const D3D12_BOX *dst_box, const void *src_data,
        UINT src_row_pitch, UINT src_slice_pitch)
{
    FIXME("iface %p, dst_sub_resource %u, dst_box %p, src_data %p, "
            "src_row_pitch %u, src_slice_pitch %u stub!\n",
            iface, dst_sub_resource, dst_box,
            src_data, src_row_pitch, src_slice_pitch);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_ReadFromSubresource(ID3D12Resource *iface,
        void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
        UINT src_sub_resource, const D3D12_BOX *src_box)
{
    FIXME("iface %p, dst_data %p, dst_row_pitch %u, dst_slice_pitch %u, "
            "src_sub_resource %u, src_box %p stub!\n",
            iface, dst_data, dst_row_pitch, dst_slice_pitch,
            src_sub_resource, src_box);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_GetHeapProperties(ID3D12Resource *iface,
        D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS *flags)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource(iface);

    TRACE("iface %p, heap_properties %p, flags %p.\n",
            iface, heap_properties, flags);

    if (heap_properties)
        *heap_properties = resource->heap_properties;
    if (flags)
        *flags = resource->heap_flags;

    return S_OK;
}

static const struct ID3D12ResourceVtbl d3d12_resource_vtbl =
{
    /* IUnknown methods */
    d3d12_resource_QueryInterface,
    d3d12_resource_AddRef,
    d3d12_resource_Release,
    /* ID3D12Object methods */
    d3d12_resource_GetPrivateData,
    d3d12_resource_SetPrivateData,
    d3d12_resource_SetPrivateDataInterface,
    d3d12_resource_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_resource_GetDevice,
    /* ID3D12Resource methods */
    d3d12_resource_Map,
    d3d12_resource_Unmap,
    d3d12_resource_GetDesc,
    d3d12_resource_GetGPUVirtualAddress,
    d3d12_resource_WriteToSubresource,
    d3d12_resource_ReadFromSubresource,
    d3d12_resource_GetHeapProperties,
};

struct d3d12_resource *unsafe_impl_from_ID3D12Resource(ID3D12Resource *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d12_resource_vtbl);
    return impl_from_ID3D12Resource(iface);
}

static HRESULT d3d12_committed_resource_init(struct d3d12_resource *resource, struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value)
{
    HRESULT hr;

    resource->ID3D12Resource_iface.lpVtbl = &d3d12_resource_vtbl;
    resource->refcount = 1;

    resource->desc = *desc;

    if (d3d12_resource_is_texture(resource)
            && (heap_properties->Type == D3D12_HEAP_TYPE_UPLOAD
            || heap_properties->Type == D3D12_HEAP_TYPE_READBACK))
    {
        WARN("Texture cannot be created on a UPLOAD/READBACK heap.\n");
        return E_INVALIDARG;
    }

    if (heap_properties->Type == D3D12_HEAP_TYPE_UPLOAD && initial_state != D3D12_RESOURCE_STATE_GENERIC_READ)
    {
        WARN("For D3D12_HEAP_TYPE_UPLOAD the state must be D3D12_RESOURCE_STATE_GENERIC_READ.\n");
        return E_INVALIDARG;
    }
    if (heap_properties->Type == D3D12_HEAP_TYPE_READBACK && initial_state != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        WARN("For D3D12_HEAP_TYPE_READBACK the state must be D3D12_RESOURCE_STATE_COPY_DEST.\n");
        return E_INVALIDARG;
    }

    if (!is_valid_resource_state(initial_state))
    {
        WARN("Invalid initial resource state %#x.\n", initial_state);
        return E_INVALIDARG;
    }

    if (optimized_clear_value && d3d12_resource_is_buffer(resource))
    {
        WARN("Optimized clear value must be NULL for buffers.\n");
        return E_INVALIDARG;
    }

    if (optimized_clear_value)
        WARN("Ignoring optimized clear value.\n");

    resource->gpu_address = 0;
    resource->flags = 0;

    switch (desc->Dimension)
    {
        case D3D12_RESOURCE_DIMENSION_BUFFER:
            if (FAILED(hr = vkd3d_create_buffer(device, heap_properties, heap_flags, desc,
                    &resource->u.vk_buffer)))
                return hr;
            if (!(resource->gpu_address = vkd3d_gpu_va_allocator_allocate(&device->gpu_va_allocator,
                    desc->Width, resource)))
            {
                ERR("Failed to allocate GPU VA.\n");
                d3d12_resource_destroy(resource, device);
                return E_OUTOFMEMORY;
            }
            if (FAILED(hr = vkd3d_allocate_buffer_memory(device, resource->u.vk_buffer,
                    heap_properties, heap_flags, &resource->vk_memory)))
            {
                d3d12_resource_destroy(resource, device);
                return hr;
            }
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            resource->flags |= VKD3D_RESOURCE_INITIAL_STATE_TRANSITION;
            if (FAILED(hr = vkd3d_create_image(resource, device, heap_properties, heap_flags, desc)))
                return hr;
            if (FAILED(hr = vkd3d_allocate_image_memory(resource, device, heap_properties, heap_flags)))
            {
                d3d12_resource_destroy(resource, device);
                return hr;
            }
            break;

        default:
            WARN("Invalid resource dimension %#x.\n", resource->desc.Dimension);
            return E_INVALIDARG;
    }

    resource->map_count = 0;
    resource->map_data = NULL;

    resource->heap_properties = *heap_properties;
    resource->heap_flags = heap_flags;
    resource->initial_state = initial_state;

    resource->device = device;
    ID3D12Device_AddRef(&device->ID3D12Device_iface);

    return S_OK;
}

HRESULT d3d12_committed_resource_create(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_resource **resource)
{
    struct d3d12_resource *object;
    HRESULT hr;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_committed_resource_init(object, device, heap_properties, heap_flags,
            desc, initial_state, optimized_clear_value)))
    {
        vkd3d_free(object);
        return hr;
    }

    TRACE("Created committed resource %p.\n", object);

    *resource = object;

    return S_OK;
}

HRESULT vkd3d_create_image_resource(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc,
        VkImage vk_image, unsigned int resource_flags, ID3D12Resource **resource)
{
    struct d3d12_device *d3d12_device = unsafe_impl_from_ID3D12Device(device);
    struct d3d12_resource *object;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D12Resource_iface.lpVtbl = &d3d12_resource_vtbl;
    object->refcount = 1;
    object->desc = *desc;
    object->u.vk_image = vk_image;
    object->vk_memory = VK_NULL_HANDLE;
    object->flags = VKD3D_RESOURCE_EXTERNAL;
    object->flags |= resource_flags & VKD3D_RESOURCE_PUBLIC_FLAGS;
    object->map_count = 0;
    object->map_data = NULL;
    memset(&object->heap_properties, 0, sizeof(object->heap_properties));
    object->heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    object->initial_state = D3D12_RESOURCE_STATE_COMMON;
    object->device = d3d12_device;
    ID3D12Device_AddRef(&d3d12_device->ID3D12Device_iface);

    TRACE("Created resource %p.\n", object);

    *resource = &object->ID3D12Resource_iface;

    return S_OK;
}

/* CBVs, SRVs, UAVs */
static struct vkd3d_view *vkd3d_view_create(void)
{
    struct vkd3d_view *view;

    if ((view = vkd3d_malloc(sizeof(*view))))
    {
        view->refcount = 1;
        view->vk_counter_view = VK_NULL_HANDLE;
    }
    return view;
}

static void vkd3d_view_incref(struct vkd3d_view *view)
{
    InterlockedIncrement(&view->refcount);
}

static void vkd3d_view_decref(struct vkd3d_view *view,
        const struct d3d12_desc *descriptor, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    ULONG refcount = InterlockedDecrement(&view->refcount);

    if (refcount)
        return;

    TRACE("Destroying view %p.\n", view);

    if (descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_SRV || descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_UAV)
    {
        if (descriptor->vk_descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                || descriptor->vk_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
            VK_CALL(vkDestroyBufferView(device->vk_device, view->u.vk_buffer_view, NULL));
        else
            VK_CALL(vkDestroyImageView(device->vk_device, view->u.vk_image_view, NULL));

        if (view->vk_counter_view)
            VK_CALL(vkDestroyBufferView(device->vk_device, view->vk_counter_view, NULL));
    }
    else if (descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_SAMPLER)
    {
        VK_CALL(vkDestroySampler(device->vk_device, view->u.vk_sampler, NULL));
    }

    vkd3d_free(view);
}

static void d3d12_desc_destroy(struct d3d12_desc *descriptor,
        struct d3d12_device *device)
{
    /* Nothing to do for VKD3D_DESCRIPTOR_MAGIC_CBV. */
    if (descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_SRV
            || descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_UAV
            || descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_SAMPLER)
    {
        vkd3d_view_decref(descriptor->u.view, descriptor, device);
    }

    memset(descriptor, 0, sizeof(*descriptor));
}

void d3d12_desc_copy(struct d3d12_desc *dst, const struct d3d12_desc *src,
        struct d3d12_device *device)
{
    d3d12_desc_destroy(dst, device);

    *dst = *src;

    if (src->magic == VKD3D_DESCRIPTOR_MAGIC_SRV
            || src->magic == VKD3D_DESCRIPTOR_MAGIC_UAV
            || src->magic == VKD3D_DESCRIPTOR_MAGIC_SAMPLER)
    {
        vkd3d_view_incref(src->u.view);
    }
}

static bool vkd3d_create_vk_buffer_view(struct d3d12_device *device,
        struct d3d12_resource *resource, const struct vkd3d_format *format,
        VkDeviceSize offset, VkDeviceSize range, VkBufferView *vk_view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct VkBufferViewCreateInfo view_desc;
    VkResult vr;

    assert(d3d12_resource_is_buffer(resource));

    if (vkd3d_format_is_compressed(format))
    {
        WARN("Invalid format for buffer view %#x.\n", format->dxgi_format);
        return false;
    }

    view_desc.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    view_desc.pNext = NULL;
    view_desc.flags = 0;
    view_desc.buffer = resource->u.vk_buffer;
    view_desc.format = format->vk_format;
    view_desc.offset = offset;
    view_desc.range = range;
    if ((vr = VK_CALL(vkCreateBufferView(device->vk_device, &view_desc, NULL, vk_view))) < 0)
        WARN("Failed to create Vulkan buffer view, vr %d.\n", vr);
    return vr == VK_SUCCESS;
}

#define VKD3D_VIEW_RAW_BUFFER 0x1

static bool vkd3d_create_buffer_view(struct d3d12_device *device,
        struct d3d12_resource *resource, DXGI_FORMAT view_format,
        unsigned int offset, unsigned int size, unsigned int structure_stride,
        unsigned int flags, VkBufferView *vk_view)
{
    const struct vkd3d_format *format;
    unsigned int element_size;

    if (view_format == DXGI_FORMAT_R32_TYPELESS && (flags & VKD3D_VIEW_RAW_BUFFER))
    {
        format = vkd3d_get_format(DXGI_FORMAT_R32_UINT, false);
        element_size = format->byte_count;
    }
    else if (view_format == DXGI_FORMAT_UNKNOWN && structure_stride)
    {
        format = vkd3d_get_format(DXGI_FORMAT_R32_UINT, false);
        element_size = structure_stride;
    }
    else if ((format = vkd3d_format_from_d3d12_resource_desc(&resource->desc, view_format)))
    {
        element_size = format->byte_count;
    }
    else
    {
        WARN("Failed to find format for %#x.\n", resource->desc.Format);
        return false;
    }

    return vkd3d_create_vk_buffer_view(device, resource, format,
            offset * element_size, size * element_size, vk_view);
}

static VkResult vkd3d_create_texture_view(struct d3d12_device *device,
        struct d3d12_resource *resource, const struct vkd3d_format *format, VkImageViewType view_type,
        uint32_t miplevel_idx, uint32_t miplevel_count, uint32_t layer_idx, uint32_t layer_count,
        VkImageView *vk_view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct VkImageViewCreateInfo view_desc;
    VkResult vr;

    assert(resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);

    view_desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_desc.pNext = NULL;
    view_desc.flags = 0;
    view_desc.image = resource->u.vk_image;
    view_desc.viewType = view_type;
    view_desc.format = format->vk_format;
    if (format->vk_aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT)
    {
        view_desc.components.r = VK_COMPONENT_SWIZZLE_ZERO;
        view_desc.components.g = VK_COMPONENT_SWIZZLE_R;
        view_desc.components.b = VK_COMPONENT_SWIZZLE_ZERO;
        view_desc.components.a = VK_COMPONENT_SWIZZLE_ZERO;
    }
    else
    {
        view_desc.components.r = VK_COMPONENT_SWIZZLE_R;
        view_desc.components.g = VK_COMPONENT_SWIZZLE_G;
        view_desc.components.b = VK_COMPONENT_SWIZZLE_B;
        view_desc.components.a = VK_COMPONENT_SWIZZLE_A;
    }
    view_desc.subresourceRange.aspectMask = format->vk_aspect_mask;
    view_desc.subresourceRange.baseMipLevel = miplevel_idx;
    view_desc.subresourceRange.levelCount = miplevel_count;
    view_desc.subresourceRange.baseArrayLayer = layer_idx;
    view_desc.subresourceRange.layerCount = layer_count;
    if ((vr = VK_CALL(vkCreateImageView(device->vk_device, &view_desc, NULL, vk_view))) < 0)
        WARN("Failed to create Vulkan image view, vr %d.\n", vr);
    return vr;
}

void d3d12_desc_create_cbv(struct d3d12_desc *descriptor,
        struct d3d12_device *device, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc)
{
    struct VkDescriptorBufferInfo *buffer_info;
    struct d3d12_resource *resource;

    d3d12_desc_destroy(descriptor, device);

    if (!desc)
    {
        WARN("Constant buffer desc is NULL.\n");
        return;
    }

    if (desc->SizeInBytes & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
    {
        WARN("Size is not %u bytes aligned.\n", D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        return;
    }

    if (!desc->BufferLocation)
    {
        FIXME("NULL CBV not implemented.\n");
        return;
    }

    resource = vkd3d_gpu_va_allocator_dereference(&device->gpu_va_allocator, desc->BufferLocation);
    buffer_info = &descriptor->u.vk_cbv_info;
    buffer_info->buffer = resource->u.vk_buffer;
    buffer_info->offset = desc->BufferLocation - resource->gpu_address;
    buffer_info->range = min(desc->SizeInBytes, resource->desc.Width - buffer_info->offset);

    descriptor->magic = VKD3D_DESCRIPTOR_MAGIC_CBV;
    descriptor->vk_descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

static unsigned int vkd3d_view_flags_from_d3d12_buffer_srv_flags(D3D12_BUFFER_SRV_FLAGS flags)
{
    if (flags == D3D12_BUFFER_SRV_FLAG_RAW)
        return VKD3D_VIEW_RAW_BUFFER;
    if (flags)
        FIXME("Unhandled buffer SRV flags %#x.\n", flags);
    return 0;
}

static void vkd3d_create_buffer_srv(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    struct vkd3d_view *view;

    if (!desc)
    {
        FIXME("Default SRV views not supported.\n");
        return;
    }

    if (desc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
    {
        WARN("Unexpected view dimension %#x.\n", desc->ViewDimension);
        return;
    }

    if (!(view = vkd3d_view_create()))
        return;

    if (!vkd3d_create_buffer_view(device, resource, desc->Format,
            desc->u.Buffer.FirstElement, desc->u.Buffer.NumElements,
            desc->u.Buffer.StructureByteStride,
            vkd3d_view_flags_from_d3d12_buffer_srv_flags(desc->u.Buffer.Flags),
            &view->u.vk_buffer_view))
    {
        vkd3d_free(view);
        return;
    }

    descriptor->magic = VKD3D_DESCRIPTOR_MAGIC_SRV;
    descriptor->vk_descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    descriptor->u.view = view;
}

void d3d12_desc_create_srv(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    const struct vkd3d_format *format;
    VkImageViewType vk_view_type;
    struct vkd3d_view *view;

    d3d12_desc_destroy(descriptor, device);

    if (!resource)
    {
        FIXME("NULL resource SRV not implemented.\n");
        return;
    }

    if (d3d12_resource_is_buffer(resource))
    {
        vkd3d_create_buffer_srv(descriptor, device, resource, desc);
        return;
    }

    if (resource->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        FIXME("Resource dimension %#x not implemented.\n", resource->desc.Dimension);
        return;
    }

    if (desc)
        FIXME("Unhandled SRV desc %p.\n", desc);

    if (!(format = vkd3d_format_from_d3d12_resource_desc(&resource->desc, desc ? desc->Format : 0)))
    {
        FIXME("Failed to find format for %#x.\n", resource->desc.Format);
        return;
    }

    if (!(view = vkd3d_view_create()))
        return;

    vk_view_type = resource->desc.DepthOrArraySize > 1
            ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (vkd3d_create_texture_view(device, resource, format, vk_view_type,
            0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS, &view->u.vk_image_view) < 0)
    {
        vkd3d_free(view);
        return;
    }

    descriptor->magic = VKD3D_DESCRIPTOR_MAGIC_SRV;
    descriptor->vk_descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor->u.view = view;
}

static unsigned int vkd3d_view_flags_from_d3d12_buffer_uav_flags(D3D12_BUFFER_UAV_FLAGS flags)
{
    if (flags == D3D12_BUFFER_UAV_FLAG_RAW)
        return VKD3D_VIEW_RAW_BUFFER;
    if (flags)
        FIXME("Unhandled buffer UAV flags %#x.\n", flags);
    return 0;
}

static void vkd3d_create_buffer_uav(struct d3d12_desc *descriptor, struct d3d12_device *device,
        struct d3d12_resource *resource, struct d3d12_resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    struct vkd3d_view *view;

    if (!desc)
    {
        FIXME("Default UAV views not supported.\n");
        return;
    }

    if (desc->ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
    {
        WARN("Unexpected view dimension %#x.\n", desc->ViewDimension);
        return;
    }

    if (desc->u.Buffer.CounterOffsetInBytes)
        FIXME("Ignoring counter offset %"PRIu64".\n", desc->u.Buffer.CounterOffsetInBytes);

    if (!(view = vkd3d_view_create()))
        return;

    if (!vkd3d_create_buffer_view(device, resource, desc->Format,
            desc->u.Buffer.FirstElement, desc->u.Buffer.NumElements,
            desc->u.Buffer.StructureByteStride,
            vkd3d_view_flags_from_d3d12_buffer_uav_flags(desc->u.Buffer.Flags),
            &view->u.vk_buffer_view))
    {
        vkd3d_free(view);
        return;
    }

    descriptor->magic = VKD3D_DESCRIPTOR_MAGIC_UAV;
    descriptor->vk_descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    descriptor->u.view = view;

    if (counter_resource)
    {
        assert(d3d12_resource_is_buffer(counter_resource));
        assert(desc->u.Buffer.StructureByteStride);
        if (!vkd3d_create_buffer_view(device, counter_resource, DXGI_FORMAT_R32_UINT,
                desc->u.Buffer.CounterOffsetInBytes / sizeof(uint32_t), 1, 0, 0, &view->vk_counter_view))
        {
            view->vk_counter_view = VK_NULL_HANDLE;
            d3d12_desc_destroy(descriptor, device);
        }
    }

    /* FIXME: Clears are implemented only for R32_UINT buffer UAVs. */
    if ((desc->Format == DXGI_FORMAT_R32_TYPELESS && (desc->u.Buffer.Flags & VKD3D_VIEW_RAW_BUFFER))
            || desc->Format == DXGI_FORMAT_R32_UINT)
    {
        const struct vkd3d_format *format = vkd3d_get_format(DXGI_FORMAT_R32_UINT, false);

        descriptor->view_offset = desc->u.Buffer.FirstElement * format->byte_count;
        descriptor->view_size = desc->u.Buffer.NumElements * format->byte_count;
    }
}

static void vkd3d_create_texture_uav(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    uint32_t miplevel_idx, layer_idx, layer_count;
    const struct vkd3d_format *format;
    VkImageViewType vk_view_type;
    struct vkd3d_view *view;

    if (resource->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        FIXME("Resource dimension %#x not implemented.\n", resource->desc.Dimension);
        return;
    }

    if (!(format = vkd3d_format_from_d3d12_resource_desc(&resource->desc, desc ? desc->Format : 0)))
    {
        ERR("Failed to find format for %#x.\n", resource->desc.Format);
        return;
    }

    if (vkd3d_format_is_compressed(format))
    {
        WARN("UAVs cannot be created for compressed formats.\n");
        return;
    }

    vk_view_type = resource->desc.DepthOrArraySize > 1
            ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    miplevel_idx = 0;
    layer_idx = 0;
    layer_count = VK_REMAINING_ARRAY_LAYERS;
    if (desc)
    {
        if (desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D)
        {
            if (desc->u.Texture2D.PlaneSlice)
                FIXME("Ignoring plane slice %u.\n", desc->u.Texture2D.PlaneSlice);

            miplevel_idx = desc->u.Texture2D.MipSlice;
        }
        else if (desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY)
        {
            if (desc->u.Texture2DArray.PlaneSlice)
                FIXME("Ignoring plane slice %u.\n", desc->u.Texture2DArray.PlaneSlice);

            vk_view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            miplevel_idx = desc->u.Texture2DArray.MipSlice;
            layer_idx = desc->u.Texture2DArray.FirstArraySlice;
            layer_count = desc->u.Texture2DArray.ArraySize;
        }
        else
        {
            WARN("Unexpected view dimension %#x.\n", desc->ViewDimension);
            return;
        }
    }

    if (!(view = vkd3d_view_create()))
        return;

    if (vkd3d_create_texture_view(device, resource, format, vk_view_type,
            miplevel_idx, 1, layer_idx, layer_count, &view->u.vk_image_view) < 0)
    {
        vkd3d_free(view);
        return;
    }

    descriptor->magic = VKD3D_DESCRIPTOR_MAGIC_UAV;
    descriptor->vk_descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptor->u.view = view;
}

void d3d12_desc_create_uav(struct d3d12_desc *descriptor, struct d3d12_device *device,
        struct d3d12_resource *resource, struct d3d12_resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    d3d12_desc_destroy(descriptor, device);

    if (!resource)
    {
        FIXME("NULL resource UAV not implemented.\n");
        return;
    }

    if (d3d12_resource_is_buffer(resource))
    {
        vkd3d_create_buffer_uav(descriptor, device, resource, counter_resource, desc);
    }
    else
    {
        if (counter_resource)
            FIXME("Unexpected counter resource for texture view.\n");
        vkd3d_create_texture_uav(descriptor, device, resource, desc);
    }
}

bool vkd3d_create_raw_buffer_view(struct d3d12_device *device,
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address, VkBufferView *vk_buffer_view)
{
    const struct vkd3d_format *format;
    struct d3d12_resource *resource;

    format = vkd3d_get_format(DXGI_FORMAT_R32_UINT, false);
    resource = vkd3d_gpu_va_allocator_dereference(&device->gpu_va_allocator, gpu_address);
    return vkd3d_create_vk_buffer_view(device, resource, format,
            gpu_address - resource->gpu_address, VK_WHOLE_SIZE, vk_buffer_view);
}

/* samplers */
static VkFilter vk_filter_from_d3d12(D3D12_FILTER_TYPE type)
{
    switch (type)
    {
        case D3D12_FILTER_TYPE_POINT:
            return VK_FILTER_NEAREST;
        case D3D12_FILTER_TYPE_LINEAR:
            return VK_FILTER_LINEAR;
        default:
            FIXME("Unhandled filter type %#x.\n", type);
            return VK_FILTER_NEAREST;
    }
}

static VkSamplerMipmapMode vk_mipmap_mode_from_d3d12(D3D12_FILTER_TYPE type)
{
    switch (type)
    {
        case D3D12_FILTER_TYPE_POINT:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case D3D12_FILTER_TYPE_LINEAR:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        default:
            FIXME("Unhandled filter type %#x.\n", type);
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
}

static VkSamplerAddressMode vk_address_mode_from_d3d12(D3D12_TEXTURE_ADDRESS_MODE mode)
{
    switch (mode)
    {
        case D3D12_TEXTURE_ADDRESS_MODE_WRAP:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case D3D12_TEXTURE_ADDRESS_MODE_CLAMP:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            /* D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE requires VK_KHR_mirror_clamp_to_edge. */
        default:
            FIXME("Unhandled address mode %#x.\n", mode);
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkResult d3d12_create_sampler(struct d3d12_device *device, D3D12_FILTER filter,
        D3D12_TEXTURE_ADDRESS_MODE address_u, D3D12_TEXTURE_ADDRESS_MODE address_v,
        D3D12_TEXTURE_ADDRESS_MODE address_w, float mip_lod_bias, unsigned int max_anisotropy,
        D3D12_COMPARISON_FUNC comparison_func, float min_lod, float max_lod,
        VkSampler *vk_sampler)
{
    const struct vkd3d_vk_device_procs *vk_procs;
    struct VkSamplerCreateInfo sampler_desc;
    VkResult vr;

    vk_procs = &device->vk_procs;

    if (D3D12_DECODE_FILTER_REDUCTION(filter) == D3D12_FILTER_REDUCTION_TYPE_MINIMUM
            || D3D12_DECODE_FILTER_REDUCTION(filter) == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM)
        FIXME("Min/max reduction mode not supported.\n");

    sampler_desc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_desc.pNext = NULL;
    sampler_desc.flags = 0;
    sampler_desc.magFilter = vk_filter_from_d3d12(D3D12_DECODE_MAG_FILTER(filter));
    sampler_desc.minFilter = vk_filter_from_d3d12(D3D12_DECODE_MIN_FILTER(filter));
    sampler_desc.mipmapMode = vk_mipmap_mode_from_d3d12(D3D12_DECODE_MIP_FILTER(filter));
    sampler_desc.addressModeU = vk_address_mode_from_d3d12(address_u);
    sampler_desc.addressModeV = vk_address_mode_from_d3d12(address_v);
    sampler_desc.addressModeW = vk_address_mode_from_d3d12(address_w);
    sampler_desc.mipLodBias = mip_lod_bias;
    sampler_desc.anisotropyEnable = D3D12_DECODE_IS_ANISOTROPIC_FILTER(filter);
    sampler_desc.maxAnisotropy = max_anisotropy;
    sampler_desc.compareEnable = D3D12_DECODE_IS_COMPARISON_FILTER(filter);
    sampler_desc.compareOp = sampler_desc.compareEnable ? vk_compare_op_from_d3d12(comparison_func) : 0;
    sampler_desc.minLod = min_lod;
    sampler_desc.maxLod = max_lod;
    sampler_desc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sampler_desc.unnormalizedCoordinates = VK_FALSE;
    if ((vr = VK_CALL(vkCreateSampler(device->vk_device, &sampler_desc, NULL, vk_sampler))) < 0)
        WARN("Failed to create Vulkan sampler, vr %d.\n", vr);

    return vr;
}

void d3d12_desc_create_sampler(struct d3d12_desc *sampler,
        struct d3d12_device *device, const D3D12_SAMPLER_DESC *desc)
{
    struct vkd3d_view *view;

    d3d12_desc_destroy(sampler, device);

    if (!desc)
    {
        WARN("NULL sampler desc.\n");
        return;
    }

    if (desc->AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER
            || desc->AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER
            || desc->AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER)
        FIXME("Ignoring border color {%.8e, %.8e, %.8e, %.8e}.\n",
                desc->BorderColor[0], desc->BorderColor[1], desc->BorderColor[2], desc->BorderColor[3]);

    if (!(view = vkd3d_view_create()))
        return;

    if (d3d12_create_sampler(device, desc->Filter, desc->AddressU,
            desc->AddressV, desc->AddressW, desc->MipLODBias, desc->MaxAnisotropy,
            desc->ComparisonFunc, desc->MinLOD, desc->MaxLOD, &view->u.vk_sampler) < 0)
    {
        vkd3d_free(view);
        return;
    }

    sampler->magic = VKD3D_DESCRIPTOR_MAGIC_SAMPLER;
    sampler->vk_descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler->u.view = view;
}

HRESULT vkd3d_create_static_sampler(struct d3d12_device *device,
        const D3D12_STATIC_SAMPLER_DESC *desc, VkSampler *vk_sampler)
{
    VkResult vr;

    if (desc->AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER
            || desc->AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER
            || desc->AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER)
        FIXME("Ignoring border %#x.\n", desc->BorderColor);

    vr = d3d12_create_sampler(device, desc->Filter, desc->AddressU,
            desc->AddressV, desc->AddressW, desc->MipLODBias, desc->MaxAnisotropy,
            desc->ComparisonFunc, desc->MinLOD, desc->MaxLOD, vk_sampler);
    return hresult_from_vk_result(vr);
}

/* RTVs */
static void d3d12_rtv_desc_destroy(struct d3d12_rtv_desc *rtv, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    if (rtv->magic != VKD3D_DESCRIPTOR_MAGIC_RTV)
        return;

    VK_CALL(vkDestroyImageView(device->vk_device, rtv->vk_view, NULL));
    memset(rtv, 0, sizeof(*rtv));
}

void d3d12_rtv_desc_create_rtv(struct d3d12_rtv_desc *rtv_desc, struct d3d12_device *device,
        struct d3d12_resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc)
{
    const struct vkd3d_format *format;
    uint32_t miplevel_idx;

    d3d12_rtv_desc_destroy(rtv_desc, device);

    if (!resource)
    {
        FIXME("NULL resource RTV not implemented.\n");
        return;
    }

    if (resource->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        FIXME("Resource dimension %#x not implemented.\n", resource->desc.Dimension);
        return;
    }

    if (!(format = vkd3d_format_from_d3d12_resource_desc(&resource->desc, desc ? desc->Format : 0)))
    {
        WARN("Invalid DXGI format.\n");
        return;
    }

    if (format->vk_aspect_mask != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        WARN("Trying to create RTV for depth/stencil format %#x.\n", format->dxgi_format);
        return;
    }

    if (desc && desc->u.Texture2D.PlaneSlice)
        FIXME("Ignoring plane slice %u.\n", desc->u.Texture2D.PlaneSlice);

    miplevel_idx = desc ? desc->u.Texture2D.MipSlice : 0;
    if (vkd3d_create_texture_view(device, resource, format, VK_IMAGE_VIEW_TYPE_2D,
            miplevel_idx, 1, 0, 1, &rtv_desc->vk_view) < 0)
        return;

    rtv_desc->format = format->vk_format;
    rtv_desc->width = d3d12_resource_desc_get_width(&resource->desc, miplevel_idx);
    rtv_desc->height = d3d12_resource_desc_get_height(&resource->desc, miplevel_idx);
    rtv_desc->magic = VKD3D_DESCRIPTOR_MAGIC_RTV;
    rtv_desc->resource = resource;
}

/* DSVs */
static void d3d12_dsv_desc_destroy(struct d3d12_dsv_desc *dsv, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    if (dsv->magic != VKD3D_DESCRIPTOR_MAGIC_DSV)
        return;

    VK_CALL(vkDestroyImageView(device->vk_device, dsv->vk_view, NULL));
    memset(dsv, 0, sizeof(*dsv));
}

void d3d12_dsv_desc_create_dsv(struct d3d12_dsv_desc *dsv_desc, struct d3d12_device *device,
        struct d3d12_resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc)
{
    const struct vkd3d_format *format;
    uint32_t miplevel_idx;

    d3d12_dsv_desc_destroy(dsv_desc, device);

    if (!resource)
    {
        FIXME("NULL resource DSV not implemented.\n");
        return;
    }

    if (resource->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        FIXME("Resource dimension %#x not implemented.\n", resource->desc.Dimension);
        return;
    }

    if (!(format = vkd3d_format_from_d3d12_resource_desc(&resource->desc, desc ? desc->Format : 0)))
    {
        WARN("Invalid DXGI format.\n");
        return;
    }

    if (!(format->vk_aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))
    {
        WARN("Trying to create DSV for format %#x.\n", format->dxgi_format);
        return;
    }

    if (desc && desc->Flags)
        FIXME("Ignoring flags %#x.\n", desc->Flags);

    miplevel_idx = desc ? desc->u.Texture2D.MipSlice : 0;
    if (vkd3d_create_texture_view(device, resource, format, VK_IMAGE_VIEW_TYPE_2D,
            miplevel_idx, 1, 0, 1, &dsv_desc->vk_view) < 0)
        return;

    dsv_desc->format = format->vk_format;
    dsv_desc->width = d3d12_resource_desc_get_width(&resource->desc, miplevel_idx);
    dsv_desc->height = d3d12_resource_desc_get_height(&resource->desc, miplevel_idx);
    dsv_desc->magic = VKD3D_DESCRIPTOR_MAGIC_DSV;
    dsv_desc->resource = resource;
}

/* ID3D12DescriptorHeap */
static inline struct d3d12_descriptor_heap *impl_from_ID3D12DescriptorHeap(ID3D12DescriptorHeap *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_descriptor_heap, ID3D12DescriptorHeap_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_QueryInterface(ID3D12DescriptorHeap *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12DescriptorHeap)
            || IsEqualGUID(riid, &IID_ID3D12Pageable)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12DescriptorHeap_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_descriptor_heap_AddRef(ID3D12DescriptorHeap *iface)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);
    ULONG refcount = InterlockedIncrement(&heap->refcount);

    TRACE("%p increasing refcount to %u.\n", heap, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_descriptor_heap_Release(ID3D12DescriptorHeap *iface)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);
    ULONG refcount = InterlockedDecrement(&heap->refcount);

    TRACE("%p decreasing refcount to %u.\n", heap, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = heap->device;
        unsigned int i;

        switch (heap->desc.Type)
        {
            case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
            case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
            {
                struct d3d12_desc *descriptors = (struct d3d12_desc *)heap->descriptors;

                for (i = 0; i < heap->desc.NumDescriptors; ++i)
                {
                    d3d12_desc_destroy(&descriptors[i], device);
                }
                break;
            }

            case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
            {
                struct d3d12_rtv_desc *rtvs = (struct d3d12_rtv_desc *)heap->descriptors;

                for (i = 0; i < heap->desc.NumDescriptors; ++i)
                {
                    d3d12_rtv_desc_destroy(&rtvs[i], device);
                }
                break;
            }

            case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
            {
                struct d3d12_dsv_desc *dsvs = (struct d3d12_dsv_desc *)heap->descriptors;

                for (i = 0; i < heap->desc.NumDescriptors; ++i)
                {
                    d3d12_dsv_desc_destroy(&dsvs[i], device);
                }
                break;
            }

            default:
                break;
        }

        vkd3d_free(heap);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_GetPrivateData(ID3D12DescriptorHeap *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    FIXME("iface %p, guid %s, data_size %p, data %p stub!", iface, debugstr_guid(guid), data_size, data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_SetPrivateData(ID3D12DescriptorHeap *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    FIXME("iface %p, guid %s, data_size %u, data %p stub!\n", iface, debugstr_guid(guid), data_size, data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_SetPrivateDataInterface(ID3D12DescriptorHeap *iface,
        REFGUID guid, const IUnknown *data)
{
    FIXME("iface %p, guid %s, data %p stub!\n", iface, debugstr_guid(guid), data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_SetName(ID3D12DescriptorHeap *iface, const WCHAR *name)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    FIXME("iface %p, name %s stub!\n", iface, debugstr_w(name, heap->device->wchar_size));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_GetDevice(ID3D12DescriptorHeap *iface,
        REFIID riid, void **device)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return ID3D12Device_QueryInterface(&heap->device->ID3D12Device_iface, riid, device);
}

static D3D12_DESCRIPTOR_HEAP_DESC * STDMETHODCALLTYPE d3d12_descriptor_heap_GetDesc(ID3D12DescriptorHeap *iface,
        D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = heap->desc;
    return desc;
}

static D3D12_CPU_DESCRIPTOR_HANDLE * STDMETHODCALLTYPE d3d12_descriptor_heap_GetCPUDescriptorHandleForHeapStart(
        ID3D12DescriptorHeap *iface, D3D12_CPU_DESCRIPTOR_HANDLE *descriptor)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, descriptor %p.\n", iface, descriptor);

    descriptor->ptr = (SIZE_T)heap->descriptors;

    return descriptor;
}

static D3D12_GPU_DESCRIPTOR_HANDLE * STDMETHODCALLTYPE d3d12_descriptor_heap_GetGPUDescriptorHandleForHeapStart(
        ID3D12DescriptorHeap *iface, D3D12_GPU_DESCRIPTOR_HANDLE *descriptor)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, descriptor %p.\n", iface, descriptor);

    descriptor->ptr = (UINT64)(intptr_t)heap->descriptors;

    return descriptor;
}

static const struct ID3D12DescriptorHeapVtbl d3d12_descriptor_heap_vtbl =
{
    /* IUnknown methods */
    d3d12_descriptor_heap_QueryInterface,
    d3d12_descriptor_heap_AddRef,
    d3d12_descriptor_heap_Release,
    /* ID3D12Object methods */
    d3d12_descriptor_heap_GetPrivateData,
    d3d12_descriptor_heap_SetPrivateData,
    d3d12_descriptor_heap_SetPrivateDataInterface,
    d3d12_descriptor_heap_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_descriptor_heap_GetDevice,
    /* ID3D12DescriptorHeap methods */
    d3d12_descriptor_heap_GetDesc,
    d3d12_descriptor_heap_GetCPUDescriptorHandleForHeapStart,
    d3d12_descriptor_heap_GetGPUDescriptorHandleForHeapStart,
};

static void d3d12_descriptor_heap_init(struct d3d12_descriptor_heap *descriptor_heap,
        struct d3d12_device *device, const D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    descriptor_heap->ID3D12DescriptorHeap_iface.lpVtbl = &d3d12_descriptor_heap_vtbl;
    descriptor_heap->refcount = 1;

    descriptor_heap->desc = *desc;

    descriptor_heap->device = device;
    ID3D12Device_AddRef(&device->ID3D12Device_iface);
}

HRESULT d3d12_descriptor_heap_create(struct d3d12_device *device,
        const D3D12_DESCRIPTOR_HEAP_DESC *desc, struct d3d12_descriptor_heap **descriptor_heap)
{
    size_t max_descriptor_count, descriptor_size;
    struct d3d12_descriptor_heap *object;

    if (!(descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(&device->ID3D12Device_iface, desc->Type)))
    {
        WARN("No descriptor size for descriptor type %#x.\n", desc->Type);
        return E_INVALIDARG;
    }

    max_descriptor_count = (~(size_t)0 - sizeof(*object)) / descriptor_size;
    if (desc->NumDescriptors > max_descriptor_count)
    {
        WARN("Invalid descriptor count %u (max %zu).\n", desc->NumDescriptors, max_descriptor_count);
        return E_OUTOFMEMORY;
    }

    if (!(object = vkd3d_malloc(offsetof(struct d3d12_descriptor_heap,
            descriptors[descriptor_size * desc->NumDescriptors]))))
        return E_OUTOFMEMORY;

    d3d12_descriptor_heap_init(object, device, desc);
    memset(object->descriptors, 0, descriptor_size * desc->NumDescriptors);

    TRACE("Created descriptor heap %p.\n", object);

    *descriptor_heap = object;

    return S_OK;
}

/* ID3D12QueryHeap */
static inline struct d3d12_query_heap *impl_from_ID3D12QueryHeap(ID3D12QueryHeap *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_query_heap, ID3D12QueryHeap_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_QueryInterface(ID3D12QueryHeap *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID3D12QueryHeap)
            || IsEqualGUID(iid, &IID_ID3D12Pageable)
            || IsEqualGUID(iid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(iid, &IID_ID3D12Object)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID3D12QueryHeap_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_query_heap_AddRef(ID3D12QueryHeap *iface)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);
    ULONG refcount = InterlockedIncrement(&heap->refcount);

    TRACE("%p increasing refcount to %u.\n", heap, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_query_heap_Release(ID3D12QueryHeap *iface)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);
    ULONG refcount = InterlockedDecrement(&heap->refcount);

    TRACE("%p decreasing refcount to %u.\n", heap, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = heap->device;
        const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

        VK_CALL(vkDestroyQueryPool(device->vk_device, heap->vk_query_pool, NULL));

        vkd3d_free(heap);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_GetPrivateData(ID3D12QueryHeap *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    FIXME("iface %p, guid %s, data_size %p, data %p stub!", iface, debugstr_guid(guid), data_size, data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_SetPrivateData(ID3D12QueryHeap *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    FIXME("iface %p, guid %s, data_size %u, data %p stub!\n", iface, debugstr_guid(guid), data_size, data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_SetPrivateDataInterface(ID3D12QueryHeap *iface,
        REFGUID guid, const IUnknown *data)
{
    FIXME("iface %p, guid %s, data %p stub!\n", iface, debugstr_guid(guid), data);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_SetName(ID3D12QueryHeap *iface, const WCHAR *name)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);

    FIXME("iface %p, name %s stub!\n", iface, debugstr_w(name, heap->device->wchar_size));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_GetDevice(ID3D12QueryHeap *iface,
        REFIID iid, void **device)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return ID3D12Device_QueryInterface(&heap->device->ID3D12Device_iface, iid, device);
}

static const struct ID3D12QueryHeapVtbl d3d12_query_heap_vtbl =
{
    /* IUnknown methods */
    d3d12_query_heap_QueryInterface,
    d3d12_query_heap_AddRef,
    d3d12_query_heap_Release,
    /* ID3D12Object methods */
    d3d12_query_heap_GetPrivateData,
    d3d12_query_heap_SetPrivateData,
    d3d12_query_heap_SetPrivateDataInterface,
    d3d12_query_heap_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_query_heap_GetDevice,
};

struct d3d12_query_heap *unsafe_impl_from_ID3D12QueryHeap(ID3D12QueryHeap *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d12_query_heap_vtbl);
    return impl_from_ID3D12QueryHeap(iface);
}

HRESULT d3d12_query_heap_create(struct d3d12_device *device, const D3D12_QUERY_HEAP_DESC *desc,
        struct d3d12_query_heap **heap)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct d3d12_query_heap *object;
    VkQueryPoolCreateInfo pool_info;
    unsigned int element_count;
    VkResult vr;

    element_count = DIV_ROUND_UP(desc->Count, sizeof(*object->availability_mask) * CHAR_BIT);
    if (!(object = vkd3d_malloc(offsetof(struct d3d12_query_heap, availability_mask[element_count]))))
        return E_OUTOFMEMORY;

    object->ID3D12QueryHeap_iface.lpVtbl = &d3d12_query_heap_vtbl;
    object->refcount = 1;
    object->device = device;
    memset(object->availability_mask, 0, element_count * sizeof(*object->availability_mask));

    pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    pool_info.pNext = NULL;
    pool_info.flags = 0;
    pool_info.queryCount = desc->Count;

    switch (desc->Type)
    {
        case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
            pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
            pool_info.pipelineStatistics = 0;
            break;

        case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
            pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            pool_info.pipelineStatistics = 0;
            break;

        case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
            pool_info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            pool_info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT
                    | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
            break;

        case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:
            FIXME("Unsupported query heap type SO_STATISTICS.\n");
            vkd3d_free(object);
            return E_NOTIMPL;

        default:
            WARN("Invalid query heap type %u.\n", desc->Type);
            vkd3d_free(object);
            return E_INVALIDARG;
    }

    if ((vr = VK_CALL(vkCreateQueryPool(device->vk_device, &pool_info, NULL, &object->vk_query_pool))) < 0)
    {
        WARN("Failed to create Vulkan query pool, vr %d.\n", vr);
        vkd3d_free(object);
        return hresult_from_vk_result(vr);
    }

    ID3D12Device_AddRef(&device->ID3D12Device_iface);

    TRACE("Created query heap %p.\n", object);

    *heap = object;

    return S_OK;
}
