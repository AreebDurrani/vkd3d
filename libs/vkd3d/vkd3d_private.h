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

#ifndef __VKD3D_PRIVATE_H
#define __VKD3D_PRIVATE_H

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XCB_KHR

#define COBJMACROS
#define NONAMELESSUNION
#include "vkd3d.h"
#include "vkd3d_memory.h"

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>

#define VK_CALL(f) (vk_procs->f)

#define VKD3D_DESCRIPTOR_MAGIC_FREE    0x00000000u
#define VKD3D_DESCRIPTOR_MAGIC_CBV     0x00564243u
#define VKD3D_DESCRIPTOR_MAGIC_SRV     0x00565253u
#define VKD3D_DESCRIPTOR_MAGIC_UAV     0x00564155u
#define VKD3D_DESCRIPTOR_MAGIC_SAMPLER 0x504d4153u
#define VKD3D_DESCRIPTOR_MAGIC_DSV     0x00565344u
#define VKD3D_DESCRIPTOR_MAGIC_RTV     0x00565452u

#define VKD3D_MAX_SHADER_STAGES     5u

struct d3d12_command_list;
struct d3d12_device;

#define DECLARE_VK_PFN(name) PFN_##name name;
struct vkd3d_vk_instance_procs
{
#define VK_INSTANCE_PFN DECLARE_VK_PFN
#include "vulkan_procs.h"
};

struct vkd3d_vk_device_procs
{
#define VK_INSTANCE_PFN DECLARE_VK_PFN
#define VK_DEVICE_PFN   DECLARE_VK_PFN
#include "vulkan_procs.h"
};
#undef DECLARE_VK_PFN

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

HRESULT vkd3d_fence_worker_start(struct vkd3d_fence_worker *worker,
        struct d3d12_device *device) DECLSPEC_HIDDEN;
HRESULT vkd3d_fence_worker_stop(struct vkd3d_fence_worker *worker) DECLSPEC_HIDDEN;

struct vkd3d_gpu_va_allocator
{
    D3D12_GPU_VIRTUAL_ADDRESS floor;

    struct vkd3d_gpu_va_allocation
    {
        D3D12_GPU_VIRTUAL_ADDRESS base;
        SIZE_T size;
        void *ptr;
    } *allocations;
    size_t allocations_size;
    size_t allocation_count;
};

D3D12_GPU_VIRTUAL_ADDRESS vkd3d_gpu_va_allocator_allocate(struct vkd3d_gpu_va_allocator *allocator,
        size_t size, void *ptr) DECLSPEC_HIDDEN;
void vkd3d_gpu_va_allocator_cleanup(struct vkd3d_gpu_va_allocator *allocator) DECLSPEC_HIDDEN;
void *vkd3d_gpu_va_allocator_dereference(struct vkd3d_gpu_va_allocator *allocator,
        D3D12_GPU_VIRTUAL_ADDRESS address) DECLSPEC_HIDDEN;
void vkd3d_gpu_va_allocator_free(struct vkd3d_gpu_va_allocator *allocator,
        D3D12_GPU_VIRTUAL_ADDRESS address) DECLSPEC_HIDDEN;
void vkd3d_gpu_va_allocator_init(struct vkd3d_gpu_va_allocator *allocator) DECLSPEC_HIDDEN;

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

#define VKD3D_RESOURCE_PUBLIC_FLAGS \
        (VKD3D_RESOURCE_INITIAL_STATE_TRANSITION | VKD3D_RESOURCE_SWAPCHAIN_IMAGE)
#define VKD3D_RESOURCE_EXTERNAL 0x00000004

/* ID3D12Resource */
struct d3d12_resource
{
    ID3D12Resource ID3D12Resource_iface;
    LONG refcount;

    D3D12_RESOURCE_DESC desc;

    D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
    union
    {
        VkBuffer vk_buffer;
        VkImage vk_image;
    } u;
    VkDeviceMemory vk_memory;
    unsigned int flags;

    unsigned int map_count;
    void *map_data;

    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_STATES initial_state;

    struct d3d12_device *device;
};

static inline bool d3d12_resource_is_buffer(const struct d3d12_resource *resource)
{
    return resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
}

HRESULT d3d12_committed_resource_create(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_resource **resource) DECLSPEC_HIDDEN;
struct d3d12_resource *unsafe_impl_from_ID3D12Resource(ID3D12Resource *iface) DECLSPEC_HIDDEN;

struct d3d12_cbv_srv_uav_desc
{
    uint32_t magic;
    VkDescriptorType vk_descriptor_type;
    union
    {
        VkBufferView vk_buffer_view;
        VkImageView vk_image_view;
    } u;
};

void d3d12_cbv_srv_uav_desc_create_srv(struct d3d12_cbv_srv_uav_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc) DECLSPEC_HIDDEN;
void d3d12_cbv_srv_uav_desc_create_uav(struct d3d12_cbv_srv_uav_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc) DECLSPEC_HIDDEN;

struct d3d12_sampler_desc
{
    uint32_t magic;
    VkSampler vk_sampler;
};

void d3d12_sampler_desc_create_sampler(struct d3d12_sampler_desc *sampler,
        struct d3d12_device *device, const D3D12_SAMPLER_DESC *desc) DECLSPEC_HIDDEN;
HRESULT d3d12_device_create_static_sampler(struct d3d12_device *device,
        const D3D12_STATIC_SAMPLER_DESC *desc, VkSampler *vk_sampler) DECLSPEC_HIDDEN;

struct d3d12_rtv_desc
{
    uint32_t magic;
    VkFormat format;
    uint64_t width;
    unsigned int height;
    VkImageView vk_view;
    struct d3d12_resource *resource;
};

void d3d12_rtv_desc_create_rtv(struct d3d12_rtv_desc *rtv_desc, struct d3d12_device *device,
        struct d3d12_resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc) DECLSPEC_HIDDEN;

struct d3d12_dsv_desc
{
    uint32_t magic;
    VkFormat format;
    uint64_t width;
    unsigned int height;
    VkImageView vk_view;
    struct d3d12_resource *resource;
};

void d3d12_dsv_desc_create_dsv(struct d3d12_dsv_desc *dsv_desc, struct d3d12_device *device,
        struct d3d12_resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc) DECLSPEC_HIDDEN;

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

/* ID3D12QueryHeap */
struct d3d12_query_heap
{
    ID3D12QueryHeap ID3D12QueryHeap_iface;
    LONG refcount;

    struct d3d12_device *device;
};

HRESULT d3d12_query_heap_create(struct d3d12_device *device,
        struct d3d12_query_heap **heap) DECLSPEC_HIDDEN;

struct d3d12_root_constant
{
    unsigned int root_parameter_index;
    VkShaderStageFlags stage_flags;
    uint32_t offset;
};

/* ID3D12RootSignature */
struct d3d12_root_signature
{
    ID3D12RootSignature ID3D12RootSignature_iface;
    LONG refcount;

    VkPipelineLayout vk_pipeline_layout;
    VkDescriptorSetLayout vk_set_layout;

    struct VkDescriptorPoolSize *pool_sizes;
    size_t pool_size_count;

    unsigned int descriptor_count;
    struct vkd3d_shader_resource_binding *descriptor_mapping;

    unsigned int constant_count;
    struct d3d12_root_constant *constants;
    struct vkd3d_shader_push_constant *push_constants;

    unsigned int static_sampler_count;
    VkSampler *static_samplers;

    struct d3d12_device *device;
};

HRESULT d3d12_root_signature_create(struct d3d12_device *device,
        const D3D12_ROOT_SIGNATURE_DESC *desc, struct d3d12_root_signature **root_signature) DECLSPEC_HIDDEN;
struct d3d12_root_signature *unsafe_impl_from_ID3D12RootSignature(ID3D12RootSignature *iface) DECLSPEC_HIDDEN;

struct d3d12_graphics_pipeline_state
{
    struct VkPipelineShaderStageCreateInfo stages[VKD3D_MAX_SHADER_STAGES];
    size_t stage_count;

    struct VkVertexInputAttributeDescription attributes[D3D12_VS_INPUT_REGISTER_COUNT];
    enum VkVertexInputRate input_rates[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    size_t attribute_count;

    struct VkAttachmentDescription attachments[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1];
    struct VkAttachmentReference attachment_references[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1];
    struct VkPipelineColorBlendAttachmentState blend_attachments[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    size_t attachment_count, rt_idx;
    VkRenderPass render_pass;

    struct VkPipelineRasterizationStateCreateInfo rs_desc;
    struct VkPipelineMultisampleStateCreateInfo ms_desc;
    struct VkPipelineDepthStencilStateCreateInfo ds_desc;

    struct d3d12_root_signature *root_signature;
};

struct d3d12_compute_pipeline_state
{
    VkPipeline vk_pipeline;
};

/* ID3D12PipelineState */
struct d3d12_pipeline_state
{
    ID3D12PipelineState ID3D12PipelineState_iface;
    LONG refcount;

    union
    {
        struct d3d12_graphics_pipeline_state graphics;
        struct d3d12_compute_pipeline_state compute;
    } u;
    VkPipelineBindPoint vk_bind_point;

    struct d3d12_device *device;
};

HRESULT d3d12_pipeline_state_create_compute(struct d3d12_device *device,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, struct d3d12_pipeline_state **state) DECLSPEC_HIDDEN;
HRESULT d3d12_pipeline_state_create_graphics(struct d3d12_device *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, struct d3d12_pipeline_state **state) DECLSPEC_HIDDEN;
struct d3d12_pipeline_state *unsafe_impl_from_ID3D12PipelineState(ID3D12PipelineState *iface) DECLSPEC_HIDDEN;

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

    VkPipeline *pipelines;
    size_t pipelines_size;
    size_t pipeline_count;

    VkDescriptorPool *descriptor_pools;
    size_t descriptor_pools_size;
    size_t descriptor_pool_count;

    VkCommandBuffer *command_buffers;
    size_t command_buffers_size;
    size_t command_buffer_count;

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
    bool is_recording;
    bool is_valid;

    uint32_t strides[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    struct VkPipelineInputAssemblyStateCreateInfo ia_desc;

    VkImageView views[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT + 1];
    unsigned int fb_width;
    unsigned int fb_height;

    VkFramebuffer current_framebuffer;
    VkPipeline current_pipeline;
    VkDescriptorSet graphics_descriptor_set;
    VkDescriptorSet compute_descriptor_set;

    struct d3d12_pipeline_state *state;
    struct d3d12_root_signature *graphics_root_signature;
    struct d3d12_root_signature *compute_root_signature;

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
    uint32_t vk_queue_family_index;

    struct d3d12_device *device;
};

HRESULT d3d12_command_queue_create(struct d3d12_device *device,
        const D3D12_COMMAND_QUEUE_DESC *desc, struct d3d12_command_queue **queue) DECLSPEC_HIDDEN;

/* ID3D12CommandSignature */
struct d3d12_command_signature
{
    ID3D12CommandSignature ID3D12CommandSignature_iface;
    LONG refcount;

    struct d3d12_device *device;
};

HRESULT d3d12_command_signature_create(struct d3d12_device *device,
        struct d3d12_command_signature **signature) DECLSPEC_HIDDEN;

/* ID3D12Device */
struct d3d12_device
{
    ID3D12Device ID3D12Device_iface;
    LONG refcount;

    VkDevice vk_device;
    VkPhysicalDevice vk_physical_device;
    struct vkd3d_vk_device_procs vk_procs;
    vkd3d_signal_event_pfn signal_event;

    struct vkd3d_gpu_va_allocator gpu_va_allocator;
    struct vkd3d_fence_worker fence_worker;

    unsigned int direct_queue_family_index;
    unsigned int copy_queue_family_index;
    unsigned int compute_queue_family_index;
    VkPhysicalDeviceMemoryProperties memory_properties;

    struct vkd3d_instance vkd3d_instance;
};

HRESULT d3d12_device_create(const struct vkd3d_device_create_info *create_info,
        struct d3d12_device **device) DECLSPEC_HIDDEN;
struct d3d12_device *unsafe_impl_from_ID3D12Device(ID3D12Device *iface) DECLSPEC_HIDDEN;

/* utils */
struct vkd3d_format
{
    DXGI_FORMAT dxgi_format;
    VkFormat vk_format;
    size_t byte_count;
    VkImageAspectFlags vk_aspect_mask;
};

const struct vkd3d_format *vkd3d_get_format(DXGI_FORMAT dxgi_format) DECLSPEC_HIDDEN;

enum VkCompareOp vk_compare_op_from_d3d12(D3D12_COMPARISON_FUNC op) DECLSPEC_HIDDEN;

bool is_valid_feature_level(D3D_FEATURE_LEVEL feature_level) DECLSPEC_HIDDEN;
bool check_feature_level_support(D3D_FEATURE_LEVEL feature_level) DECLSPEC_HIDDEN;

bool is_valid_resource_state(D3D12_RESOURCE_STATES state) DECLSPEC_HIDDEN;
bool is_write_resource_state(D3D12_RESOURCE_STATES state) DECLSPEC_HIDDEN;

static inline bool is_cpu_accessible_heap(const struct D3D12_HEAP_PROPERTIES *properties)
{
    if (properties->Type == D3D12_HEAP_TYPE_DEFAULT)
        return false;
    if (properties->Type == D3D12_HEAP_TYPE_CUSTOM)
    {
        return properties->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE
                || properties->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    }
    return true;
}

HRESULT return_interface(IUnknown *iface, REFIID iface_riid,
        REFIID requested_riid, void **object) DECLSPEC_HIDDEN;

const char *debug_vk_extent_3d(VkExtent3D extent) DECLSPEC_HIDDEN;
const char *debug_vk_memory_heap_flags(VkMemoryHeapFlags flags) DECLSPEC_HIDDEN;
const char *debug_vk_memory_property_flags(VkMemoryPropertyFlags flags) DECLSPEC_HIDDEN;
const char *debug_vk_queue_flags(VkQueueFlags flags) DECLSPEC_HIDDEN;

HRESULT hresult_from_vk_result(VkResult vr) DECLSPEC_HIDDEN;

HRESULT vkd3d_load_vk_instance_procs(struct vkd3d_vk_instance_procs *procs,
        VkInstance instance) DECLSPEC_HIDDEN;
HRESULT vkd3d_load_vk_device_procs(struct vkd3d_vk_device_procs *procs,
        const struct vkd3d_vk_instance_procs *parent_procs, VkDevice device) DECLSPEC_HIDDEN;

/* We link directly to the loader library and use the following exported functions. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
        const char *name);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkInstance *instance);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer_name,
        uint32_t *property_count, VkExtensionProperties *properties);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *property_count,
        VkLayerProperties *properties);
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator);
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator);

#endif  /* __VKD3D_PRIVATE_H */
