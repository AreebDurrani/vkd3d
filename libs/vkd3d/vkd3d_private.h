/*
 * Copyright 2016 Józef Kucia for CodeWeavers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __VKD3D_PRIVATE_H
#define __VKD3D_PRIVATE_H

#define COBJMACROS
#define NONAMELESSUNION
#include "vkd3d.h"
#include "vkd3d_memory.h"
#include "vkd3d_vulkan.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>

#define VKD3D_DESCRIPTOR_MAGIC_FREE 0x00000000u
#define VKD3D_DESCRIPTOR_MAGIC_RTV  0x00565452u

struct d3d12_command_list;
struct d3d12_device;

struct vkd3d_instance
{
    VkInstance vk_instance;
    struct vkd3d_vk_instance_procs vk_procs;
};

struct vkd3d_fence_worker
{
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool should_exit;

    size_t fence_count;
    VkFence *vk_fences;
    size_t vk_fences_size;
    struct vkd3d_waiting_fence
    {
        ID3D12Fence *fence;
        UINT64 value;
    } *fences;
    size_t fences_size;

    struct d3d12_device *device;
};

HRESULT vkd3d_start_fence_worker(struct vkd3d_fence_worker *worker,
        struct d3d12_device *device) DECLSPEC_HIDDEN;
HRESULT vkd3d_stop_fence_worker(struct vkd3d_fence_worker *worker) DECLSPEC_HIDDEN;

/* ID3D12Fence */
struct d3d12_fence
{
    ID3D12Fence ID3D12Fence_iface;
    LONG refcount;

    UINT64 value;
    pthread_mutex_t mutex;

    struct vkd3d_waiting_event
    {
        UINT64 value;
        HANDLE event;
    } *events;
    size_t events_size;
    size_t event_count;

    struct d3d12_device *device;
};

HRESULT d3d12_fence_create(struct d3d12_device *device,
        UINT64 initial_value, D3D12_FENCE_FLAGS flags, struct d3d12_fence **fence) DECLSPEC_HIDDEN;

/* ID3D12Resource */
struct d3d12_resource
{
    ID3D12Resource ID3D12Resource_iface;
    LONG refcount;

    D3D12_RESOURCE_DESC desc;

    union
    {
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
        VkBuffer vk_buffer;
        VkImage vk_image;
    } u;
    VkDeviceMemory vk_memory;

    unsigned int map_count;
    void *map_data;

    struct d3d12_device *device;
};

HRESULT d3d12_committed_resource_create(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_resource **resource) DECLSPEC_HIDDEN;
struct d3d12_resource *unsafe_impl_from_ID3D12Resource(ID3D12Resource *iface) DECLSPEC_HIDDEN;

struct d3d12_cbv_srv_uav_desc
{
    uint32_t magic;
};

struct d3d12_sampler_desc
{
    uint32_t magic;
};

struct d3d12_rtv_desc
{
    uint32_t magic;
    VkFormat format;
    uint64_t width;
    unsigned int height;
    VkImageView vk_view;
};

void d3d12_rtv_desc_create_rtv(struct d3d12_rtv_desc *rtv_desc, struct d3d12_device *device,
        struct d3d12_resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc) DECLSPEC_HIDDEN;

struct d3d12_dsv_desc
{
    uint32_t magic;
};

/* ID3D12DescriptorHeap */
struct d3d12_descriptor_heap
{
    ID3D12DescriptorHeap ID3D12DescriptorHeap_iface;
    LONG refcount;

    D3D12_DESCRIPTOR_HEAP_DESC desc;

    struct d3d12_device *device;

    BYTE descriptors[];
};

HRESULT d3d12_descriptor_heap_create(struct d3d12_device *device,
        const D3D12_DESCRIPTOR_HEAP_DESC *desc, struct d3d12_descriptor_heap **descriptor_heap) DECLSPEC_HIDDEN;

/* ID3D12RootSignature */
struct d3d12_root_signature
{
    ID3D12RootSignature ID3D12RootSignature_iface;
    LONG refcount;

    VkPipelineLayout vk_pipeline_layout;

    struct d3d12_device *device;
};

HRESULT d3d12_root_signature_create(struct d3d12_device *device,
        const D3D12_ROOT_SIGNATURE_DESC *desc, struct d3d12_root_signature **root_signature) DECLSPEC_HIDDEN;

/* ID3D12PipelineState */
struct d3d12_pipeline_state
{
    ID3D12PipelineState ID3D12PipelineState_iface;
    LONG refcount;

    VkPipeline vk_pipeline;

    struct d3d12_device *device;
};

HRESULT d3d12_pipeline_state_create_compute(struct d3d12_device *device,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, struct d3d12_pipeline_state **state) DECLSPEC_HIDDEN;

/* ID3D12CommandAllocator */
struct d3d12_command_allocator
{
    ID3D12CommandAllocator ID3D12CommandAllocator_iface;
    LONG refcount;

    D3D12_COMMAND_LIST_TYPE type;

    VkCommandPool vk_command_pool;

    VkRenderPass *passes;
    size_t passes_size;
    size_t pass_count;

    VkFramebuffer *framebuffers;
    size_t framebuffers_size;
    size_t framebuffer_count;

    struct d3d12_command_list *current_command_list;
    struct d3d12_device *device;
};

HRESULT d3d12_command_allocator_create(struct d3d12_device *device,
        D3D12_COMMAND_LIST_TYPE type, struct d3d12_command_allocator **allocator) DECLSPEC_HIDDEN;

/* ID3D12CommandList */
struct d3d12_command_list
{
    ID3D12GraphicsCommandList ID3D12GraphicsCommandList_iface;
    LONG refcount;

    D3D12_COMMAND_LIST_TYPE type;
    ID3D12PipelineState *pipeline_state;

    VkCommandBuffer vk_command_buffer;
    BOOL is_recording;

    uint32_t strides[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    struct VkPipelineInputAssemblyStateCreateInfo ia_desc;

    struct d3d12_command_allocator *allocator;
    struct d3d12_device *device;
};

HRESULT d3d12_command_list_create(struct d3d12_device *device,
        UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator_iface,
        ID3D12PipelineState *initial_pipeline_state, struct d3d12_command_list **list) DECLSPEC_HIDDEN;

/* ID3D12CommandQueue */
struct d3d12_command_queue
{
    ID3D12CommandQueue ID3D12CommandQueue_iface;
    LONG refcount;

    D3D12_COMMAND_QUEUE_DESC desc;

    VkQueue vk_queue;

    struct d3d12_device *device;
};

HRESULT d3d12_command_queue_create(struct d3d12_device *device,
        const D3D12_COMMAND_QUEUE_DESC *desc, struct d3d12_command_queue **queue) DECLSPEC_HIDDEN;

/* ID3D12Device */
struct d3d12_device
{
    ID3D12Device ID3D12Device_iface;
    LONG refcount;

    VkDevice vk_device;
    struct vkd3d_vk_device_procs vk_procs;
    vkd3d_signal_event_pfn signal_event;

    struct vkd3d_fence_worker fence_worker;

    unsigned int direct_queue_family_index;
    unsigned int copy_queue_family_index;
    VkPhysicalDeviceMemoryProperties memory_properties;

    struct vkd3d_instance vkd3d_instance;
};

HRESULT d3d12_device_create(const struct vkd3d_device_create_info *create_info,
        struct d3d12_device **device) DECLSPEC_HIDDEN;

/* utils */
struct vkd3d_format
{
    DXGI_FORMAT dxgi_format;
    VkFormat vk_format;
    size_t byte_count;
    VkImageAspectFlags vk_aspect_mask;
};

const struct vkd3d_format *vkd3d_get_format(DXGI_FORMAT dxgi_format) DECLSPEC_HIDDEN;

BOOL is_valid_feature_level(D3D_FEATURE_LEVEL feature_level) DECLSPEC_HIDDEN;
BOOL check_feature_level_support(D3D_FEATURE_LEVEL feature_level) DECLSPEC_HIDDEN;

HRESULT return_interface(IUnknown *iface, REFIID iface_riid,
        REFIID requested_riid, void **object) DECLSPEC_HIDDEN;

const char *debug_vk_extent_3d(VkExtent3D extent) DECLSPEC_HIDDEN;
const char *debug_vk_memory_heap_flags(VkMemoryHeapFlags flags) DECLSPEC_HIDDEN;
const char *debug_vk_memory_property_flags(VkMemoryPropertyFlags flags) DECLSPEC_HIDDEN;
const char *debug_vk_queue_flags(VkQueueFlags flags) DECLSPEC_HIDDEN;

bool vkd3d_array_reserve(void **elements, size_t *capacity,
        size_t element_count, size_t element_size) DECLSPEC_HIDDEN;

HRESULT hresult_from_vk_result(VkResult vr) DECLSPEC_HIDDEN;

HRESULT vkd3d_load_vk_instance_procs(struct vkd3d_vk_instance_procs *procs,
        VkInstance instance) DECLSPEC_HIDDEN;
HRESULT vkd3d_load_vk_device_procs(struct vkd3d_vk_device_procs *procs,
        const struct vkd3d_vk_instance_procs *parent_procs, VkDevice device) DECLSPEC_HIDDEN;

#endif  /* __VKD3D_PRIVATE_H */
