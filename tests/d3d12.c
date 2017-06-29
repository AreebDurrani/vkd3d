/*
 * Copyright 2016-2017 Józef Kucia for CodeWeavers
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

/* Hack for MinGW-w64 headers.
 *
 * We want to use WIDL C inline wrappers because some methods
 * in D3D12 interfaces return aggregate objects. Unfortunately,
 * WIDL C inline wrappers are broken when used with MinGW-w64
 * headers because FORCEINLINE expands to extern inline
 * which leads to the "multiple storage classes in declaration
 * specifiers" compiler error.
 */
#ifdef __MINGW32__
# include <_mingw.h>
# ifdef __MINGW64_VERSION_MAJOR
#  undef __forceinline
#  define __forceinline __inline__ __attribute__((__always_inline__,__gnu_inline__))
# endif

# define _HRESULT_DEFINED
typedef int HRESULT;
#endif

#include <inttypes.h>
#include <limits.h>
#include <math.h>

#define COBJMACROS
#define INITGUID
#include "vkd3d_test.h"
#include "vkd3d_windows.h"
#define WIDL_C_INLINE_WRAPPERS
#include "d3d12.h"

#ifndef _WIN32
# include <pthread.h>
# include "vkd3d_utils.h"
#endif

struct vec2
{
    float x, y;
};

struct vec4
{
    float x, y, z, w;
};

struct uvec4
{
    unsigned int x, y, z, w;
};

struct ivec4
{
    int x, y, z, w;
};

static BOOL compare_float(float f, float g, unsigned int ulps)
{
    int x, y;
    union
    {
        float f;
        int i;
    } u;

    u.f = f;
    x = u.i;
    u.f = g;
    y = u.i;

    if (x < 0)
        x = INT_MIN - x;
    if (y < 0)
        y = INT_MIN - y;

    if (abs(x - y) > ulps)
        return FALSE;

    return TRUE;
}

static BOOL compare_vec4(const struct vec4 *v1, const struct vec4 *v2, unsigned int ulps)
{
    return compare_float(v1->x, v2->x, ulps)
            && compare_float(v1->y, v2->y, ulps)
            && compare_float(v1->z, v2->z, ulps)
            && compare_float(v1->w, v2->w, ulps);
}

static BOOL compare_uvec4(const struct uvec4* v1, const struct uvec4 *v2)
{
    return v1->x == v2->x && v1->y == v2->y && v1->z == v2->z && v1->w == v2->w;
}

static ULONG get_refcount(void *iface)
{
    IUnknown *unk = iface;
    IUnknown_AddRef(unk);
    return IUnknown_Release(unk);
}

#define check_interface(a, b, c) check_interface_(__LINE__, (IUnknown *)a, b, c)
static void check_interface_(unsigned int line, IUnknown *iface, REFIID riid, bool supported)
{
    HRESULT hr, expected_hr;
    IUnknown *unk;

    expected_hr = supported ? S_OK : E_NOINTERFACE;

    hr = IUnknown_QueryInterface(iface, riid, (void **)&unk);
    ok_(line)(hr == expected_hr, "Got hr %#x, expected %#x.\n", hr, expected_hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
}

#define create_root_signature(a, b, c) create_root_signature_(__LINE__, a, b, c)
#if _WIN32
static HRESULT create_root_signature_(unsigned int line, ID3D12Device *device,
        const D3D12_ROOT_SIGNATURE_DESC *desc, ID3D12RootSignature **root_signature)
{
    ID3DBlob *blob;
    HRESULT hr;

    if (FAILED(hr = D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, NULL)))
        return hr;

    hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)root_signature);
    ID3D10Blob_Release(blob);
    return hr;
}
#else
/* XXX: Root signature byte code is not supported yet. We allow to pass D3D12_ROOT_SIGNATURE_DESC
 * directly to CreateRootSignature(). */
static HRESULT create_root_signature_(unsigned int line, ID3D12Device *device,
        const D3D12_ROOT_SIGNATURE_DESC *desc, ID3D12RootSignature **root_signature)
{
    return ID3D12Device_CreateRootSignature(device, 0, desc, ~(SIZE_T)0,
            &IID_ID3D12RootSignature, (void **)root_signature);
}
#endif

static D3D12_SHADER_BYTECODE shader_bytecode(const DWORD *code, size_t size)
{
    D3D12_SHADER_BYTECODE shader_bytecode = { code, size };
    return shader_bytecode;
}

#if _WIN32
# define SHADER_BYTECODE(dxbc, spirv) ((void)spirv, shader_bytecode(dxbc, sizeof(dxbc)))
#else
# define SHADER_BYTECODE(dxbc, spirv) ((void)dxbc, shader_bytecode(spirv, sizeof(spirv)))
#endif

static void transition_resource_state(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
        D3D12_RESOURCE_STATES state_before, D3D12_RESOURCE_STATES state_after)
{
    D3D12_RESOURCE_BARRIER barrier;

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state_before;
    barrier.Transition.StateAfter = state_after;

    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

static void uav_barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource)
{
    D3D12_RESOURCE_BARRIER barrier;

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;

    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

static void exec_command_list(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list)
{
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)list};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
}

#ifdef _WIN32
static HANDLE create_event(void)
{
    return CreateEventA(NULL, FALSE, FALSE, NULL);
}

static void signal_event(HANDLE event)
{
    SetEvent(event);
}

static unsigned int wait_event(HANDLE event, unsigned int milliseconds)
{
    return WaitForSingleObject(event, milliseconds);
}

static void destroy_event(HANDLE event)
{
    CloseHandle(event);
}
#else
static HANDLE create_event(void)
{
    return vkd3d_create_event();
}

static void signal_event(HANDLE event)
{
    vkd3d_signal_event(event);
}

static unsigned int wait_event(HANDLE event, unsigned int milliseconds)
{
    return vkd3d_wait_event(event, milliseconds);
}

static void destroy_event(HANDLE event)
{
    vkd3d_destroy_event(event);
}
#endif

typedef void (*thread_main_pfn)(void *data);

struct test_thread_data
{
    thread_main_pfn main_pfn;
    void *user_data;
};

#ifdef _WIN32
static DWORD WINAPI test_thread_main(void *untyped_data)
{
    struct test_thread_data *data = untyped_data;
    data->main_pfn(data->user_data);
    free(untyped_data);
    return 0;
}

static HANDLE create_thread(thread_main_pfn main_pfn, void *user_data)
{
    struct test_thread_data *data;

    if (!(data = malloc(sizeof(*data))))
        return NULL;
    data->main_pfn = main_pfn;
    data->user_data = user_data;

    return CreateThread(NULL, 0, test_thread_main, data, 0, NULL);
}

static bool join_thread(HANDLE thread)
{
    int ret;

    ret = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return ret == WAIT_OBJECT_0;
}
#else
static void *test_thread_main(void *untyped_data)
{
    struct test_thread_data *data = untyped_data;
    data->main_pfn(data->user_data);
    free(untyped_data);
    return NULL;
}

static HANDLE create_thread(thread_main_pfn main_pfn, void *user_data)
{
    struct test_thread_data *data;
    pthread_t *thread;

    if (!(thread = malloc(sizeof(*thread))))
        return NULL;

    if (!(data = malloc(sizeof(*data))))
    {
        free(thread);
        return NULL;
    }
    data->main_pfn = main_pfn;
    data->user_data = user_data;

    if (pthread_create(thread, NULL, test_thread_main, data))
    {
        free(data);
        free(thread);
        return NULL;
    }

    return thread;
}

static bool join_thread(HANDLE untyped_thread)
{
    pthread_t *thread = untyped_thread;
    int rc;

    rc = pthread_join(*thread, NULL);
    free(thread);
    return !rc;
}
#endif

static HRESULT wait_for_fence(ID3D12Fence *fence, UINT64 value)
{
    unsigned int ret;
    HANDLE event;
    HRESULT hr;

    if (ID3D12Fence_GetCompletedValue(fence) >= value)
        return S_OK;

    if (!(event = create_event()))
        return E_FAIL;

    if (FAILED(hr = ID3D12Fence_SetEventOnCompletion(fence, value, event)))
    {
        destroy_event(event);
        return hr;
    }

    ret = wait_event(event, INFINITE);
    destroy_event(event);

    return ret == WAIT_OBJECT_0;
}

#define wait_queue_idle(a, b) wait_queue_idle_(__LINE__, a, b)
static void wait_queue_idle_(unsigned int line, ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence;
    HRESULT hr;

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok_(line)(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    hr = ID3D12CommandQueue_Signal(queue, fence, 1);
    ok_(line)(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);
    hr = wait_for_fence(fence, 1);
    ok_(line)(SUCCEEDED(hr), "Failed to wait for fence, hr %#x.\n", hr);

    ID3D12Fence_Release(fence);
}

static unsigned int format_size(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
            return 16;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return 4;
        default:
            trace("Unhandled format %#x.\n", format);
            return 1;
    }
}

struct resource_readback
{
    unsigned int width;
    unsigned int height;
    ID3D12Resource *resource;
    unsigned int row_pitch;
    void *data;
};

static void get_texture_readback_with_command_list(ID3D12Resource *texture, unsigned int sub_resource,
        struct resource_readback *rb, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list)
{
    D3D12_TEXTURE_COPY_LOCATION dst_location, src_location;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *resource;
    D3D12_RANGE read_range;
    unsigned int miplevel;
    ID3D12Device *device;
    DXGI_FORMAT format;
    HRESULT hr;

    hr = ID3D12Resource_GetDevice(texture, &IID_ID3D12Device, (void **)&device);
    ok(SUCCEEDED(hr), "Failed to get device, hr %#x.\n", hr);

    resource_desc = ID3D12Resource_GetDesc(texture);
    ok(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER,
            "Resource %p is not texture.\n", texture);
    ok(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D,
            "Readback not implemented for 3D textures.\n");

    miplevel = sub_resource % resource_desc.MipLevels;
    rb->width = max(1, resource_desc.Width >> miplevel);
    rb->height = max(1, resource_desc.Height >> miplevel);
    rb->row_pitch = align(rb->width * format_size(resource_desc.Format), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    rb->data = NULL;

    format = resource_desc.Format;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = rb->row_pitch * rb->height;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);
    rb->resource = resource;

    dst_location.pResource = resource;
    dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_location.PlacedFootprint.Offset = 0;
    dst_location.PlacedFootprint.Footprint.Format = format;
    dst_location.PlacedFootprint.Footprint.Width = rb->width;
    dst_location.PlacedFootprint.Footprint.Height = rb->height;
    dst_location.PlacedFootprint.Footprint.Depth = 1;
    dst_location.PlacedFootprint.Footprint.RowPitch = rb->row_pitch;

    src_location.pResource = texture;
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_location.SubresourceIndex = sub_resource;

    ID3D12GraphicsCommandList_CopyTextureRegion(command_list, &dst_location, 0, 0, 0, &src_location, NULL);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);

    read_range.Begin = 0;
    read_range.End = resource_desc.Width;
    hr = ID3D12Resource_Map(resource, 0, &read_range, &rb->data);
    ok(SUCCEEDED(hr), "Map failed, hr %#x.\n", hr);

    ID3D12Device_Release(device);
}

static void *get_readback_data(struct resource_readback *rb, unsigned int x, unsigned int y,
        size_t element_size)
{
    return &((BYTE *)rb->data)[rb->row_pitch * y + x * element_size];
}

static unsigned int get_readback_uint(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return *(unsigned int *)get_readback_data(rb, x, y, sizeof(unsigned int));
}

static const struct vec4 *get_readback_vec4(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return get_readback_data(rb, x, y, sizeof(struct vec4));
}

static const struct uvec4 *get_readback_uvec4(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return get_readback_data(rb, x, y, sizeof(struct uvec4));
}

static void release_resource_readback(struct resource_readback *rb)
{
    D3D12_RANGE range = {0, 0};
    ID3D12Resource_Unmap(rb->resource, 0, &range);
    ID3D12Resource_Release(rb->resource);
}

static ID3D12Device *create_device(void)
{
    ID3D12Device *device;
    HRESULT hr;

    if (FAILED(hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
        return NULL;

    return device;
}

#define create_empty_root_signature(device, flags) create_empty_root_signature_(__LINE__, device, flags)
static ID3D12RootSignature *create_empty_root_signature_(unsigned int line,
        ID3D12Device *device, D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature = NULL;
    HRESULT hr;

    root_signature_desc.NumParameters = 0;
    root_signature_desc.pParameters = NULL;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = flags;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok_(line)(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    return root_signature;
}

#define create_cb_root_signature(a, b, c, e) create_cb_root_signature_(__LINE__, a, b, c, e)
static ID3D12RootSignature *create_cb_root_signature_(unsigned int line,
        ID3D12Device *device, unsigned int reg_idx, D3D12_SHADER_VISIBILITY shader_visibility,
        D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature = NULL;
    D3D12_ROOT_PARAMETER root_parameter;
    HRESULT hr;

    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameter.Descriptor.ShaderRegister = reg_idx;
    root_parameter.Descriptor.RegisterSpace = 0;
    root_parameter.ShaderVisibility = shader_visibility;

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_parameter;
    root_signature_desc.Flags = flags;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok_(line)(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    return root_signature;
}

#define create_pipeline_state(a, b, c, d, e, f) create_pipeline_state_(__LINE__, a, b, c, d, e, f)
static ID3D12PipelineState *create_pipeline_state_(unsigned int line, ID3D12Device *device,
        ID3D12RootSignature *root_signature, DXGI_FORMAT rt_format,
        const D3D12_SHADER_BYTECODE *vs, const D3D12_SHADER_BYTECODE *ps,
        const D3D12_INPUT_LAYOUT_DESC *input_layout)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_state_desc;
    ID3D12PipelineState *pipeline_state;
    HRESULT hr;

    static const DWORD vs_code[] =
    {
#if 0
        void main(uint id : SV_VertexID, out float4 position : SV_Position,
                out float2 texcoord : Texcoord)
        {
            texcoord = float2((id << 1) & 2, id & 2);
            position = float4(texcoord * float2(2, -2) + float2(-1, 1), 0, 1);
            texcoord.y = 1.0 - texcoord.y;
        }
#endif
        0x43425844, 0x67dfa8f5, 0x122d22ee, 0x711ad39f, 0x70a2910f, 0x00000001, 0x000001f0, 0x00000003,
        0x0000002c, 0x00000060, 0x000000b8, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000006, 0x00000001, 0x00000000, 0x00000101, 0x565f5653, 0x65747265, 0x00444978,
        0x4e47534f, 0x00000050, 0x00000002, 0x00000008, 0x00000038, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x00000044, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x00000c03,
        0x505f5653, 0x7469736f, 0x006e6f69, 0x63786554, 0x64726f6f, 0xababab00, 0x58454853, 0x00000130,
        0x00010050, 0x0000004c, 0x0100086a, 0x04000060, 0x00101012, 0x00000000, 0x00000006, 0x04000067,
        0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x00102032, 0x00000001, 0x02000068, 0x00000001,
        0x0b00008c, 0x00100012, 0x00000000, 0x00004001, 0x00000001, 0x00004001, 0x00000001, 0x0010100a,
        0x00000000, 0x00004001, 0x00000000, 0x07000001, 0x00100082, 0x00000000, 0x0010100a, 0x00000000,
        0x00004001, 0x00000002, 0x05000056, 0x00100032, 0x00000000, 0x001000c6, 0x00000000, 0x0f000032,
        0x00102032, 0x00000000, 0x00100046, 0x00000000, 0x00004002, 0x40000000, 0xc0000000, 0x00000000,
        0x00000000, 0x00004002, 0xbf800000, 0x3f800000, 0x00000000, 0x00000000, 0x08000000, 0x00100042,
        0x00000000, 0x8010001a, 0x00000041, 0x00000000, 0x00004001, 0x3f800000, 0x05000036, 0x00102032,
        0x00000001, 0x00100086, 0x00000000, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000,
        0x00000000, 0x00000000, 0x3f800000, 0x0100003e,
    };
    static const DWORD ps_code[] =
    {
#if 0
        void main(const in float4 position : SV_Position, out float4 target : SV_Target0)
        {
            target = float4(0.0f, 1.0f, 0.0f, 1.0f);
        }
#endif
        0x43425844, 0x8a4a8140, 0x5eba8e0b, 0x714e0791, 0xb4b8eed2, 0x00000001, 0x000000d8, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000003c, 0x00000050,
        0x0000000f, 0x0100086a, 0x03000065, 0x001020f2, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000, 0x0100003e,
    };

    memset(&pipeline_state_desc, 0, sizeof(pipeline_state_desc));
    pipeline_state_desc.pRootSignature = root_signature;
    if (vs)
        pipeline_state_desc.VS = *vs;
    else
        pipeline_state_desc.VS = shader_bytecode(vs_code, sizeof(vs_code));
    if (ps)
        pipeline_state_desc.PS = *ps;
    else
        pipeline_state_desc.PS = shader_bytecode(ps_code, sizeof(ps_code));
    pipeline_state_desc.StreamOutput.RasterizedStream = 0;
    pipeline_state_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline_state_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline_state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    if (input_layout)
        pipeline_state_desc.InputLayout = *input_layout;
    pipeline_state_desc.SampleMask = ~(UINT)0;
    pipeline_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline_state_desc.NumRenderTargets = 1;
    pipeline_state_desc.RTVFormats[0] = rt_format;
    pipeline_state_desc.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(device, &pipeline_state_desc,
            &IID_ID3D12PipelineState, (void **)&pipeline_state);
    ok_(line)(SUCCEEDED(hr), "Failed to create graphics pipeline state, hr %#x.\n", hr);

    return pipeline_state;
}

struct draw_test_context_desc
{
    DXGI_FORMAT rt_format;
    BOOL no_root_signature;
    BOOL no_pipeline;
};

struct draw_test_context
{
    ID3D12Device *device;

    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;

    D3D12_RESOURCE_DESC render_target_desc;
    ID3D12Resource *render_target;

    ID3D12DescriptorHeap *rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;

    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;
};

#define create_render_target(context, desc) create_render_target_(__LINE__, context, desc)
static void create_render_target_(unsigned int line, struct draw_test_context *context,
        const struct draw_test_context_desc *desc)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_CLEAR_VALUE clear_value;
    DXGI_FORMAT rt_format;
    HRESULT hr;

    rt_format = desc && desc->rt_format ? desc->rt_format : DXGI_FORMAT_R8G8B8A8_UNORM;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = rt_format;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear_value.Format = rt_format;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 1.0f;
    clear_value.Color[2] = 1.0f;
    clear_value.Color[3] = 1.0f;
    hr = ID3D12Device_CreateCommittedResource(context->device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)&context->render_target);
    ok_(line)(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    context->render_target_desc = resource_desc;

    ID3D12Device_CreateRenderTargetView(context->device, context->render_target, NULL, context->rtv);
}

#define init_draw_test_context(context, ps) init_draw_test_context_(__LINE__, context, ps)
static bool init_draw_test_context_(unsigned int line, struct draw_test_context *context,
        const struct draw_test_context_desc *desc)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
    ID3D12Device *device;
    HRESULT hr;

    memset(context, 0, sizeof(*context));

    if (!(context->device = create_device()))
    {
        skip_(line)("Failed to create device.\n");
        return false;
    }
    device = context->device;

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&context->queue);
    ok_(line)(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&context->allocator);
    ok_(line)(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            context->allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&context->list);
    ok_(line)(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &rtv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&context->rtv_heap);
    ok_(line)(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);

    context->rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(context->rtv_heap);

    create_render_target_(line, context, desc);

    if (!desc || !desc->no_root_signature)
        context->root_signature = create_empty_root_signature_(line,
                device, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    if (!desc || (!desc->no_root_signature && !desc->no_pipeline))
        context->pipeline_state = create_pipeline_state_(line, device,
                context->root_signature, context->render_target_desc.Format,
                NULL, NULL, NULL);

    return true;
}

#define destroy_draw_test_context(context) destroy_draw_test_context_(__LINE__, context)
static void destroy_draw_test_context_(unsigned int line, struct draw_test_context *context)
{
    ULONG refcount;

    if (context->pipeline_state)
        ID3D12PipelineState_Release(context->pipeline_state);
    if (context->root_signature)
        ID3D12RootSignature_Release(context->root_signature);

    ID3D12DescriptorHeap_Release(context->rtv_heap);
    ID3D12Resource_Release(context->render_target);

    ID3D12CommandAllocator_Release(context->allocator);
    ID3D12CommandQueue_Release(context->queue);
    ID3D12GraphicsCommandList_Release(context->list);

    refcount = ID3D12Device_Release(context->device);
    ok_(line)(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_device(void)
{
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    check_interface(device, &IID_ID3D12Object, TRUE);
    check_interface(device, &IID_ID3D12DeviceChild, FALSE);
    check_interface(device, &IID_ID3D12Pageable, FALSE);
    check_interface(device, &IID_ID3D12Device, TRUE);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    ok(hr == S_OK, "D3D12CreateDevice failed, hr %#x.\n", hr);
    ID3D12Device_Release(device);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_9_1, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_9_2, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_9_3, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_10_0, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_10_1, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = D3D12CreateDevice(NULL, 0, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "D3D12CreateDevice failed, hr %#x.\n", hr);
    hr = D3D12CreateDevice(NULL, ~0u, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "D3D12CreateDevice failed, hr %#x.\n", hr);
}

static void test_node_count(void)
{
    ID3D12Device *device;
    UINT node_count;
    ULONG refcount;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    node_count = ID3D12Device_GetNodeCount(device);
    trace("Node count: %u.\n", node_count);
    ok(1 <= node_count && node_count <= 32, "Got unexpected node count %u.\n", node_count);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_check_feature_support(void)
{
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels;
    D3D_FEATURE_LEVEL max_supported_feature_level;
    D3D12_FEATURE_DATA_ARCHITECTURE architecture;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    static const D3D_FEATURE_LEVEL all_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    static const D3D_FEATURE_LEVEL d3d12_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    static const D3D_FEATURE_LEVEL d3d_9_x_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    static const D3D_FEATURE_LEVEL invalid_feature_levels[] =
    {
        0x0000,
        0x3000,
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    /* Architecture. */
    memset(&architecture, 0, sizeof(architecture));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ARCHITECTURE,
            &architecture, sizeof(architecture));
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    ok(!architecture.NodeIndex, "Got unexpected node %u.\n", architecture.NodeIndex);
    ok(!architecture.CacheCoherentUMA || architecture.UMA,
            "Got unexpected cache coherent UMA %#x (UMA %#x).\n",
            architecture.CacheCoherentUMA, architecture.UMA);
    trace("UMA %#x, cache coherent UMA %#x, tile based renderer %#x.\n",
            architecture.UMA, architecture.CacheCoherentUMA, architecture.TileBasedRenderer);

    if (ID3D12Device_GetNodeCount(device) == 1)
    {
        memset(&architecture, 0, sizeof(architecture));
        architecture.NodeIndex = 1;
        hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ARCHITECTURE,
                &architecture, sizeof(architecture));
        ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    }

    /* Feature levels */
    memset(&feature_levels, 0, sizeof(feature_levels));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    feature_levels.NumFeatureLevels = ARRAY_SIZE(all_feature_levels);
    feature_levels.pFeatureLevelsRequested = all_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "CheckFeatureSupport failed, hr %#x.\n", hr);
    trace("Max supported feature level %#x.\n", feature_levels.MaxSupportedFeatureLevel);
    max_supported_feature_level = feature_levels.MaxSupportedFeatureLevel;

    feature_levels.NumFeatureLevels = ARRAY_SIZE(d3d12_feature_levels);
    feature_levels.pFeatureLevelsRequested = d3d12_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "CheckFeatureSupport failed, hr %#x.\n", hr);
    ok(feature_levels.MaxSupportedFeatureLevel == max_supported_feature_level,
            "Got unexpected feature level %#x, expected %#x.\n",
            feature_levels.MaxSupportedFeatureLevel, max_supported_feature_level);

    /* Check invalid size. */
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels) + 1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels) - 1);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    feature_levels.NumFeatureLevels = ARRAY_SIZE(d3d_9_x_feature_levels);
    feature_levels.pFeatureLevelsRequested = d3d_9_x_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "CheckFeatureSupport failed, hr %#x.\n", hr);
    ok(feature_levels.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_9_3,
            "Got unexpected max feature level %#x.\n", feature_levels.MaxSupportedFeatureLevel);

    feature_levels.NumFeatureLevels = ARRAY_SIZE(invalid_feature_levels);
    feature_levels.pFeatureLevelsRequested = invalid_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(SUCCEEDED(hr), "CheckFeatureSupport failed, hr %#x.\n", hr);
    ok(feature_levels.MaxSupportedFeatureLevel == 0x3000,
            "Got unexpected max feature level %#x.\n", feature_levels.MaxSupportedFeatureLevel);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_command_allocator(void)
{
    ID3D12CommandAllocator *command_allocator;
    ID3D12Device *device, *tmp_device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12CommandAllocator_GetDevice(command_allocator, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(command_allocator, &IID_ID3D12Object, TRUE);
    check_interface(command_allocator, &IID_ID3D12DeviceChild, TRUE);
    check_interface(command_allocator, &IID_ID3D12Pageable, TRUE);
    check_interface(command_allocator, &IID_ID3D12CommandAllocator, TRUE);

    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COPY,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, ~0u,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(hr == E_INVALIDARG, "CreateCommandAllocator failed, hr %#x.\n", hr);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_command_list(void)
{
    ID3D12CommandAllocator *command_allocator;
    ID3D12Device *device, *tmp_device;
    ID3D12CommandList *command_list;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            NULL, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    refcount = get_refcount(command_allocator);
    ok(refcount == 1, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12CommandList_GetDevice(command_list, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 4, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(command_list, &IID_ID3D12Object, TRUE);
    check_interface(command_list, &IID_ID3D12DeviceChild, TRUE);
    check_interface(command_list, &IID_ID3D12Pageable, FALSE);
    check_interface(command_list, &IID_ID3D12CommandList, TRUE);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    check_interface(command_list, &IID_ID3D12CommandAllocator, FALSE);

    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_COPY,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_COPY,
            command_allocator, NULL, &IID_ID3D12CommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);
    check_interface(command_list, &IID_ID3D12GraphicsCommandList, TRUE);
    refcount = ID3D12CommandList_Release(command_list);
    ok(!refcount, "ID3D12CommandList has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12CommandAllocator_Release(command_allocator);
    ok(!refcount, "ID3D12CommandAllocator has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_command_queue(void)
{
    D3D12_COMMAND_QUEUE_DESC desc, result_desc;
    ID3D12Device *device, *tmp_device;
    ID3D12CommandQueue *queue;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12CommandQueue_GetDevice(queue, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(queue, &IID_ID3D12Object, TRUE);
    check_interface(queue, &IID_ID3D12DeviceChild, TRUE);
    check_interface(queue, &IID_ID3D12Pageable, TRUE);
    check_interface(queue, &IID_ID3D12CommandQueue, TRUE);

    result_desc = ID3D12CommandQueue_GetDesc(queue);
    ok(result_desc.Type == desc.Type, "Got unexpected type %#x.\n", result_desc.Type);
    ok(result_desc.Priority == desc.Priority, "Got unexpected priority %#x.\n", result_desc.Priority);
    ok(result_desc.Flags == desc.Flags, "Got unexpected flags %#x.\n", result_desc.Flags);
    ok(result_desc.NodeMask == 0x1, "Got unexpected node mask 0x%08x.\n", result_desc.NodeMask);

    refcount = ID3D12CommandQueue_Release(queue);
    ok(!refcount, "ID3D12CommandQueue has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_committed_resource(void)
{
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Device *device, *tmp_device;
    D3D12_CLEAR_VALUE clear_value;
    ID3D12Resource *resource;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 0.0f;
    clear_value.Color[2] = 0.0f;
    clear_value.Color[3] = 1.0f;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12Resource_GetDevice(resource, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(resource, &IID_ID3D12Object, TRUE);
    check_interface(resource, &IID_ID3D12DeviceChild, TRUE);
    check_interface(resource, &IID_ID3D12Pageable, TRUE);
    check_interface(resource, &IID_ID3D12Resource, TRUE);

    gpu_address = ID3D12Resource_GetGPUVirtualAddress(resource);
    ok(!gpu_address, "Got unexpected GPU virtual address %#"PRIx64".\n", gpu_address);

    refcount = ID3D12Resource_Release(resource);
    ok(!refcount, "ID3D12Resource has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear_value, &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    /* For D3D12_RESOURCE_STATE_RENDER_TARGET the D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET flag is required. */
    resource_desc.Flags = 0;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    todo(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12Resource_Release(resource);

    /* A texture cannot be created on a UPLOAD heap. */
    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    /* A texture cannot be created on a READBACK heap. */
    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    check_interface(resource, &IID_ID3D12Object, TRUE);
    check_interface(resource, &IID_ID3D12DeviceChild, TRUE);
    check_interface(resource, &IID_ID3D12Pageable, TRUE);
    check_interface(resource, &IID_ID3D12Resource, TRUE);

    gpu_address = ID3D12Resource_GetGPUVirtualAddress(resource);
    ok(gpu_address, "Got unexpected GPU virtual address %#"PRIx64".\n", gpu_address);

    refcount = ID3D12Resource_Release(resource);
    ok(!refcount, "ID3D12Resource has %u references left.\n", (unsigned int)refcount);

    /* The clear value must be NULL for buffers. */
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, &clear_value,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    /* For D3D12_HEAP_TYPE_UPLOAD the state must be D3D12_RESOURCE_STATE_GENERIC_READ. */
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);
    refcount = ID3D12Resource_Release(resource);
    ok(!refcount, "ID3D12Resource has %u references left.\n", (unsigned int)refcount);

    /* For D3D12_HEAP_TYPE_READBACK the state must be D3D12_RESOURCE_STATE_COPY_DEST. */
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COPY_SOURCE, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_descriptor_heap(void)
{
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    ID3D12Device *device, *tmp_device;
    ID3D12DescriptorHeap *heap;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 16;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12DescriptorHeap_GetDevice(heap, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(heap, &IID_ID3D12Object, TRUE);
    check_interface(heap, &IID_ID3D12DeviceChild, TRUE);
    check_interface(heap, &IID_ID3D12Pageable, TRUE);
    check_interface(heap, &IID_ID3D12DescriptorHeap, TRUE);

    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap);
    ok(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);
    refcount = ID3D12DescriptorHeap_Release(heap);
    ok(!refcount, "ID3D12DescriptorHeap has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_root_signature(void)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_DESCRIPTOR_RANGE descriptor_ranges[1];
    D3D12_ROOT_PARAMETER root_parameters[1];
    ID3D12RootSignature *root_signature;
    ID3D12Device *device, *tmp_device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[0].NumDescriptors = 1;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = descriptor_ranges;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    todo(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    if (SUCCEEDED(hr))
    {
        refcount = get_refcount(device);
        ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
        hr = ID3D12RootSignature_GetDevice(root_signature, &IID_ID3D12Device, (void **)&tmp_device);
        ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
        refcount = get_refcount(device);
        ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
        refcount = ID3D12Device_Release(tmp_device);
        ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

        check_interface(root_signature, &IID_ID3D12Object, TRUE);
        check_interface(root_signature, &IID_ID3D12DeviceChild, TRUE);
        check_interface(root_signature, &IID_ID3D12Pageable, FALSE);
        check_interface(root_signature, &IID_ID3D12RootSignature, TRUE);

        refcount = ID3D12RootSignature_Release(root_signature);
        ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);
    }

    root_signature_desc.NumParameters = 0;
    root_signature_desc.pParameters = NULL;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);
    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_pipeline_state(void)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_state_desc;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;
    ID3D12Device *device, *tmp_device;
    ULONG refcount;
    HRESULT hr;

    static const DWORD dxbc_code[] =
    {
#if 0
        [numthreads(1, 1, 1)]
        void main() { }
#endif
        0x43425844, 0x1acc3ad0, 0x71c7b057, 0xc72c4306, 0xf432cb57, 0x00000001, 0x00000074, 0x00000003,
        0x0000002c, 0x0000003c, 0x0000004c, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x00000008, 0x00000000, 0x00000008, 0x58454853, 0x00000020, 0x00050050, 0x00000008, 0x0100086a,
        0x0400009b, 0x00000001, 0x00000001, 0x00000001, 0x0100003e,
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    root_signature_desc.NumParameters = 0;
    root_signature_desc.pParameters = NULL;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(SUCCEEDED(hr), "Failed to create root signature, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    memset(&pipeline_state_desc, 0, sizeof(pipeline_state_desc));
    pipeline_state_desc.pRootSignature = root_signature;
    pipeline_state_desc.CS = shader_bytecode(dxbc_code, sizeof(dxbc_code));
    pipeline_state_desc.NodeMask = 0;
    pipeline_state_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = ID3D12Device_CreateComputePipelineState(device, &pipeline_state_desc,
            &IID_ID3D12PipelineState, (void **)&pipeline_state);
    ok(SUCCEEDED(hr), "CreateComputePipelineState failed, hr %#x.\n", hr);

    refcount = get_refcount(root_signature);
    ok(refcount == 1, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12PipelineState_GetDevice(pipeline_state, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 4, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(pipeline_state, &IID_ID3D12Object, TRUE);
    check_interface(pipeline_state, &IID_ID3D12DeviceChild, TRUE);
    check_interface(pipeline_state, &IID_ID3D12Pageable, TRUE);
    check_interface(pipeline_state, &IID_ID3D12PipelineState, TRUE);

    refcount = ID3D12PipelineState_Release(pipeline_state);
    ok(!refcount, "ID3D12PipelineState has %u references left.\n", (unsigned int)refcount);
    refcount = ID3D12RootSignature_Release(root_signature);
    ok(!refcount, "ID3D12RootSignature has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_create_fence(void)
{
    ID3D12Device *device, *tmp_device;
    ID3D12Fence *fence;
    ULONG refcount;
    UINT64 value;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    refcount = get_refcount(device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    hr = ID3D12Fence_GetDevice(fence, &IID_ID3D12Device, (void **)&tmp_device);
    ok(SUCCEEDED(hr), "GetDevice failed, hr %#x.\n", hr);
    refcount = get_refcount(device);
    ok(refcount == 3, "Got unexpected refcount %u.\n", (unsigned int)refcount);
    refcount = ID3D12Device_Release(tmp_device);
    ok(refcount == 2, "Got unexpected refcount %u.\n", (unsigned int)refcount);

    check_interface(fence, &IID_ID3D12Object, TRUE);
    check_interface(fence, &IID_ID3D12DeviceChild, TRUE);
    check_interface(fence, &IID_ID3D12Pageable, TRUE);
    check_interface(fence, &IID_ID3D12Fence, TRUE);

    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    refcount = ID3D12Fence_Release(fence);
    ok(!refcount, "ID3D12Fence has %u references left.\n", (unsigned int)refcount);

    hr = ID3D12Device_CreateFence(device, 99, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 99, "Got unexpected value %"PRIu64".\n", value);
    refcount = ID3D12Fence_Release(fence);
    ok(!refcount, "ID3D12Fence has %u references left.\n", (unsigned int)refcount);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_reset_command_allocator(void)
{
    ID3D12CommandAllocator *command_allocator, *command_allocator2;
    ID3D12GraphicsCommandList *command_list, *command_list2;
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == E_FAIL, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == E_FAIL, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting Command list failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == E_FAIL, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator2);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    uav_barrier(command_list, NULL);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    exec_command_list(queue, command_list);

    /* A command list can be reset when it is in use. */
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator2, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    wait_queue_idle(device, queue);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);

    uav_barrier(command_list, NULL);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    exec_command_list(queue, command_list);

    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    wait_queue_idle(device, queue);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Resetting command list failed, hr %#x.\n", hr);

    /* A command allocator can be used with one command list at a time. */
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list2);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator2, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list2);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12GraphicsCommandList_Close(command_list2);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list2, command_allocator, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandAllocator_Release(command_allocator2);
    ID3D12CommandQueue_Release(queue);
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12GraphicsCommandList_Release(command_list2);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_cpu_signal_fence(void)
{
    HANDLE event1, event2;
    ID3D12Device *device;
    unsigned int i, ret;
    ID3D12Fence *fence;
    ULONG refcount;
    UINT64 value;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    hr = ID3D12Fence_Signal(fence, 1);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 1, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12Fence_Signal(fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 10, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12Fence_Signal(fence, 5);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 5, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    /* Basic tests with single event. */
    event1 = create_event();
    ok(!!event1, "Failed to create event.\n");
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 5);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 6, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 7);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Event is signaled immediately when value <= GetCompletedValue(). */
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    for (i = 0; i <= ID3D12Fence_GetCompletedValue(fence); ++i)
    {
        hr = ID3D12Fence_SetEventOnCompletion(fence, i, event1);
        ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
        ret = wait_event(event1, 0);
        ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x for %u.\n", ret, i);
        ret = wait_event(event1, 0);
        ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x for %u.\n", ret, i);
    }
    hr = ID3D12Fence_SetEventOnCompletion(fence, i, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, i);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach event to multiple values. */
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 3, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 9, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    for (i = 1; i < 13; ++i)
    {
        hr = ID3D12Fence_Signal(fence, i);
        ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
        if (i == 3 || i == 5 || i == 9 || i == 12)
        {
            ret = wait_event(event1, 0);
            ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x for %u.\n", ret, i);
        }
        ret = wait_event(event1, 0);
        ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x for %u.\n", ret, i);
    }

    /* Tests with 2 events. */
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    event2 = create_event();
    ok(!!event2, "Failed to create event.\n");

    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 100, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, ~(UINT64)0, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);

    hr = ID3D12Fence_Signal(fence, 50);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 99);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 101);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach two events to the same value. */
    hr = ID3D12Fence_Signal(fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_Signal(fence, 3);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Test passing signaled event. */
    hr = ID3D12Fence_Signal(fence, 20);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 20, "Got unexpected value %"PRIu64".\n", value);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    signal_event(event1);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 30, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(fence, 30);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    destroy_event(event1);
    destroy_event(event2);

    ID3D12Fence_Release(fence);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_gpu_signal_fence(void)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandQueue *queue;
    HANDLE event1, event2;
    ID3D12Device *device;
    unsigned int i, ret;
    ID3D12Fence *fence;
    ULONG refcount;
    UINT64 value;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    /* XXX: It seems that when a queue is idle a fence is signalled immediately
     * in D3D12. Vulkan implementations don't signal a fence immediately so
     * libvkd3d doesn't as well. In order to make this test reliable
     * wait_queue_idle() is inserted after every ID3D12CommandQueue_Signal(). */
    hr = ID3D12CommandQueue_Signal(queue, fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 10, "Got unexpected value %"PRIu64".\n", value);

    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    /* Basic tests with single event. */
    event1 = create_event();
    ok(!!event1, "Failed to create event.\n");
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 5);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 6, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 7);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 10);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach one event to multiple values. */
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 3, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 5, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 9, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 12, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    for (i = 1; i < 13; ++i)
    {
        hr = ID3D12CommandQueue_Signal(queue, fence, i);
        ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
        wait_queue_idle(device, queue);
        if (i == 3 || i == 5 || i == 9 || i == 12)
        {
            ret = wait_event(event1, 0);
            ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x for %u.\n", ret, i);
        }
        ret = wait_event(event1, 0);
        ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x for %u.\n", ret, i);
    }

    /* Tests with 2 events. */
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    value = ID3D12Fence_GetCompletedValue(fence);
    ok(value == 0, "Got unexpected value %"PRIu64".\n", value);

    event2 = create_event();
    ok(!!event2, "Failed to create event.\n");

    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 100, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, ~(UINT64)0, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);

    hr = ID3D12CommandQueue_Signal(queue, fence, 50);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 99);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 101);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, 100);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, fence, ~(UINT64)0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    /* Attach two events to the same value. */
    hr = ID3D12CommandQueue_Signal(queue, fence, 0);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event1);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event2);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    hr = ID3D12CommandQueue_Signal(queue, fence, 3);
    ok(SUCCEEDED(hr), "Signal failed, hr %#x.\n", hr);
    wait_queue_idle(device, queue);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event1, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);
    ret = wait_event(event2, 0);
    ok(ret == WAIT_TIMEOUT, "Got unexpected return value %#x.\n", ret);

    wait_queue_idle(device, queue);

    destroy_event(event1);
    destroy_event(event2);

    ID3D12Fence_Release(fence);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

struct multithread_fence_wait_data
{
    HANDLE event;
    ID3D12Fence *fence;
    UINT64 value;
};

static void fence_event_wait_main(void *untyped_data)
{
    struct multithread_fence_wait_data *data = untyped_data;
    HANDLE event;
    HRESULT hr;
    int ret;

    event = create_event();
    ok(!!event, "Failed to create event.\n");

    hr = ID3D12Fence_SetEventOnCompletion(data->fence, data->value, event);
    ok(SUCCEEDED(hr), "SetEventOnCompletion failed, hr %#x.\n", hr);

    signal_event(data->event);

    ret = wait_event(event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Got unexpected return value %#x.\n", ret);

    destroy_event(event);
}

static void fence_busy_wait_main(void *untyped_data)
{
    struct multithread_fence_wait_data *data = untyped_data;

    signal_event(data->event);

    while (ID3D12Fence_GetCompletedValue(data->fence) < data->value)
        ;
}

static void test_multithread_fence_wait(void)
{
    struct multithread_fence_wait_data thread_data;
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    unsigned int ret;
    ULONG refcount;
    HANDLE thread;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    thread_data.event = create_event();
    thread_data.value = 0;
    ok(!!thread_data.event, "Failed to create event.\n");
    hr = ID3D12Device_CreateFence(device, thread_data.value, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&thread_data.fence);
    ok(SUCCEEDED(hr), "CreateFence failed, hr %#x.\n", hr);

    /* Signal fence on host. */
    ++thread_data.value;
    thread = create_thread(fence_event_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    ++thread_data.value;
    thread = create_thread(fence_busy_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12Fence_Signal(thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    /* Signal fence on device. */
    ++thread_data.value;
    thread = create_thread(fence_event_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    ++thread_data.value;
    thread = create_thread(fence_busy_wait_main, &thread_data);
    ok(!!thread, "Failed to create thread.\n");
    ret = wait_event(thread_data.event, INFINITE);
    ok(ret == WAIT_OBJECT_0, "Failed to wait for thread start, return value %#x.\n", ret);

    hr = ID3D12CommandQueue_Signal(queue, thread_data.fence, thread_data.value);
    ok(SUCCEEDED(hr), "Failed to signal fence, hr %#x.\n", hr);

    ok(join_thread(thread), "Failed to join thread.\n");

    destroy_event(thread_data.event);
    ID3D12Fence_Release(thread_data.fence);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_clear_depth_stencil_view(void)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    unsigned int dsv_increment_size;
    ID3D12DescriptorHeap *dsv_heap;
    D3D12_CLEAR_VALUE clear_value;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    unsigned int x, y;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &dsv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    ok(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);

    dsv_increment_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    trace("DSV descriptor handle increment size: %u.\n", dsv_increment_size);
    ok(dsv_increment_size, "Got unexpectd increment size %#x.\n", dsv_increment_size);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_D32_FLOAT;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    clear_value.Format = DXGI_FORMAT_D32_FLOAT;
    clear_value.DepthStencil.Depth = 0.5f;
    clear_value.DepthStencil.Stencil = 0x3;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value, &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    dsv_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
    ID3D12Device_CreateDepthStencilView(device, resource, NULL, dsv_handle);

    ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv_handle,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.75f, 0x7, 0, NULL);
    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);

    get_texture_readback_with_command_list(resource, 0, &rb, queue, command_list);
    for (y = 0; y < resource_desc.Height; ++y)
    {
        for (x = 0; x < resource_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           ok(v == 0x3f400000, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12Resource_Release(resource);
    ID3D12CommandQueue_Release(queue);
    ID3D12DescriptorHeap_Release(dsv_heap);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_clear_render_target_view(void)
{
    static const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    unsigned int rtv_increment_size;
    ID3D12DescriptorHeap *rtv_heap;
    D3D12_CLEAR_VALUE clear_value;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    unsigned int x, y;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &rtv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    ok(SUCCEEDED(hr), "CreateDescriptorHeap failed, hr %#x.\n", hr);

    rtv_increment_size = ID3D12Device_GetDescriptorHandleIncrementSize(device,
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    trace("RTV descriptor handle increment size: %u.\n", rtv_increment_size);

    rtv_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 0.0f;
    clear_value.Color[2] = 0.0f;
    clear_value.Color[3] = 1.0f;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    ID3D12Device_CreateRenderTargetView(device, resource, NULL, rtv_handle);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handle, green, 0, NULL);
    transition_resource_state(command_list, resource,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);

    get_texture_readback_with_command_list(resource, 0, &rb, queue, command_list);
    for (y = 0; y < resource_desc.Height; ++y)
    {
        for (x = 0; x < resource_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           ok(v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12Resource_Release(resource);
    ID3D12CommandQueue_Release(queue);
    ID3D12DescriptorHeap_Release(rtv_heap);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_draw_instanced(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    struct draw_test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    D3D12_VIEWPORT viewport;
    RECT scissor_rect;
    unsigned int x, y;

    if (!init_draw_test_context(&context, NULL))
        return;
    command_list = context.list;
    queue = context.queue;

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = context.render_target_desc.Width;
    viewport.Height = context.render_target_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 0.0f;

    scissor_rect.left = scissor_rect.top = 0;
    scissor_rect.right = context.render_target_desc.Width;
    scissor_rect.bottom = context.render_target_desc.Height;

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    /* This draw call is ignored. */
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < context.render_target_desc.Height; ++y)
    {
        for (x = 0; x < context.render_target_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           ok(v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    destroy_draw_test_context(&context);
}

static void test_draw_indexed_instanced(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const uint16_t indices[] = {0, 1, 2};
    ID3D12GraphicsCommandList *command_list;
    D3D12_RESOURCE_DESC resource_desc;
    struct draw_test_context context;
    D3D12_HEAP_PROPERTIES heap_desc;
    struct resource_readback rb;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ID3D12CommandQueue *queue;
    D3D12_VIEWPORT viewport;
    ID3D12Resource *ib;
    RECT scissor_rect;
    unsigned int x, y;
    HRESULT hr;
    void *ptr;

    if (!init_draw_test_context(&context, NULL))
        return;
    command_list = context.list;
    queue = context.queue;

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = context.render_target_desc.Width;
    viewport.Height = context.render_target_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 0.0f;

    scissor_rect.left = scissor_rect.top = 0;
    scissor_rect.right = context.render_target_desc.Width;
    scissor_rect.bottom = context.render_target_desc.Height;

    heap_desc.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_desc.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_desc.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_desc.CreationNodeMask = 1;
    heap_desc.VisibleNodeMask = 1;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = sizeof(indices);
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = ID3D12Device_CreateCommittedResource(context.device, &heap_desc, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&ib);
    ok(SUCCEEDED(hr), "Failed to create index buffer, hr %#x.\n", hr);
    hr = ID3D12Resource_Map(ib, 0, NULL, (void **)&ptr);
    ok(SUCCEEDED(hr), "Failed to map index buffer, hr %#x.\n", hr);
    memcpy(ptr, indices, sizeof(indices));
    ID3D12Resource_Unmap(ib, 0, NULL);

    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ib);
    ibv.SizeInBytes = sizeof(indices);
    ibv.Format = DXGI_FORMAT_R16_UINT;

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    /* This draw call is ignored. */
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 3, 1, 0, 0, 0);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 3, 1, 0, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < context.render_target_desc.Height; ++y)
    {
        for (x = 0; x < context.render_target_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           ok(v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(ib);
    destroy_draw_test_context(&context);
}

static void test_fragment_coords(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    struct draw_test_context_desc desc;
    struct draw_test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    D3D12_VIEWPORT viewport;
    unsigned int x, y;
    RECT scissor_rect;

    static const DWORD ps_code[] =
    {
#if 0
        float4 main(float4 position: sv_position) : sv_target
        {
            return position;
        }
#endif
        0x43425844, 0xac408178, 0x2ca4213f, 0x4f2551e1, 0x1626b422, 0x00000001, 0x000000d8, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x00000f0f, 0x705f7673, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x745f7673, 0x65677261, 0xabab0074, 0x52444853, 0x0000003c, 0x00000040,
        0x0000000f, 0x04002064, 0x001010f2, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x05000036, 0x001020f2, 0x00000000, 0x00101e46, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_pipeline = true;
    if (!init_draw_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, NULL, &ps, NULL);

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = context.render_target_desc.Width;
    viewport.Height = context.render_target_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 0.0f;

    scissor_rect.left = scissor_rect.top = 0;
    scissor_rect.right = context.render_target_desc.Width;
    scissor_rect.bottom = context.render_target_desc.Height;

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    viewport.TopLeftX = 10.0f;
    viewport.TopLeftY = 10.0f;
    viewport.Width = 20.0f;
    viewport.Height = 30.0f;
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < context.render_target_desc.Height; ++y)
    {
        for (x = 0; x < context.render_target_desc.Width; ++x)
        {
            const struct vec4 *v = get_readback_vec4(&rb, x, y);
            struct vec4 expected = {x + 0.5f, y + 0.5f, 0.0f, 1.0f};
            ok(compare_vec4(v, &expected, 0),
                    "Got %.8e, %.8e, %.8e, %.8e expected %.8e, %.8e, %.8e, %.8e.\n",
                    v->x, v->y, v->z, v->w, expected.x, expected.y, expected.z, expected.w);
        }
    }
    release_resource_readback(&rb);

    destroy_draw_test_context(&context);
}

static void test_fractional_viewports(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_INPUT_LAYOUT_DESC input_layout;
    struct draw_test_context_desc desc;
    D3D12_RESOURCE_DESC resource_desc;
    struct draw_test_context context;
    D3D12_VERTEX_BUFFER_VIEW vbv;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    D3D12_VIEWPORT viewport;
    unsigned int i, x, y;
    ID3D12Resource *vb;
    RECT scissor_rect;
    HRESULT hr;
    void *ptr;

    static const DWORD vs_code[] =
    {
#if 0
        void main(in float4 in_position : POSITION,
                in float2 in_texcoord : TEXCOORD,
                out float4 position : SV_Position,
                out float2 texcoord : TEXCOORD)
        {
            position = in_position;
            texcoord = in_texcoord;
        }
#endif
        0x43425844, 0x4df282ca, 0x85c8bbfc, 0xd44ad19f, 0x1158be97, 0x00000001, 0x00000148, 0x00000003,
        0x0000002c, 0x00000080, 0x000000d8, 0x4e475349, 0x0000004c, 0x00000002, 0x00000008, 0x00000038,
        0x00000000, 0x00000000, 0x00000003, 0x00000000, 0x00000f0f, 0x00000041, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x49534f50, 0x4e4f4954, 0x58455400, 0x524f4f43, 0xabab0044,
        0x4e47534f, 0x00000050, 0x00000002, 0x00000008, 0x00000038, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x00000044, 0x00000000, 0x00000000, 0x00000003, 0x00000001, 0x00000c03,
        0x505f5653, 0x7469736f, 0x006e6f69, 0x43584554, 0x44524f4f, 0xababab00, 0x52444853, 0x00000068,
        0x00010040, 0x0000001a, 0x0300005f, 0x001010f2, 0x00000000, 0x0300005f, 0x00101032, 0x00000001,
        0x04000067, 0x001020f2, 0x00000000, 0x00000001, 0x03000065, 0x00102032, 0x00000001, 0x05000036,
        0x001020f2, 0x00000000, 0x00101e46, 0x00000000, 0x05000036, 0x00102032, 0x00000001, 0x00101046,
        0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        float4 main(float4 position : SV_Position,
                float2 texcoord : TEXCOORD) : SV_Target
        {
            return float4(position.xy, texcoord);
        }
#endif
        0x43425844, 0xa15616bc, 0x6862ab1c, 0x28b915c0, 0xdb0df67c, 0x00000001, 0x0000011c, 0x00000003,
        0x0000002c, 0x00000084, 0x000000b8, 0x4e475349, 0x00000050, 0x00000002, 0x00000008, 0x00000038,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000030f, 0x00000044, 0x00000000, 0x00000000,
        0x00000003, 0x00000001, 0x00000303, 0x505f5653, 0x7469736f, 0x006e6f69, 0x43584554, 0x44524f4f,
        0xababab00, 0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000,
        0x00000003, 0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x52444853, 0x0000005c,
        0x00000040, 0x00000017, 0x04002064, 0x00101032, 0x00000000, 0x00000001, 0x03001062, 0x00101032,
        0x00000001, 0x03000065, 0x001020f2, 0x00000000, 0x05000036, 0x00102032, 0x00000000, 0x00101046,
        0x00000000, 0x05000036, 0x001020c2, 0x00000000, 0x00101406, 0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    static const D3D12_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static const struct
    {
        struct vec2 position;
        struct vec2 texcoord;
    }
    quad[] =
    {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}},
        {{-1.0f,  1.0f}, {0.0f, 1.0f}},
        {{ 1.0f, -1.0f}, {1.0f, 0.0f}},
        {{ 1.0f,  1.0f}, {1.0f, 1.0f}},
    };
    static const float viewport_offsets[] =
    {
        0.0f, 1.0f / 2.0f, 1.0f / 4.0f, 1.0f / 8.0f, 1.0f / 16.0f, 1.0f / 32.0f,
        1.0f / 64.0f, 1.0f / 128.0f, 1.0f / 256.0f, 63.0f / 128.0f,
    };

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_draw_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_empty_root_signature(context.device,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    input_layout.pInputElementDescs = layout_desc;
    input_layout.NumElements = ARRAY_SIZE(layout_desc);
    context.pipeline_state = create_pipeline_state(context.device,
            context.root_signature, desc.rt_format, &vs, &ps, &input_layout);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = sizeof(quad);
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = ID3D12Device_CreateCommittedResource(context.device, &heap_properties,
            D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL, &IID_ID3D12Resource, (void **)&vb);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);
    hr = ID3D12Resource_Map(vb, 0, NULL, (void **)&ptr);
    ok(SUCCEEDED(hr), "Failed to map vertex buffer, hr %#x.\n", hr);
    memcpy(ptr, quad, sizeof(quad));
    ID3D12Resource_Unmap(vb, 0, NULL);

    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb);
    vbv.StrideInBytes = sizeof(*quad);
    vbv.SizeInBytes = sizeof(quad);

    scissor_rect.left = scissor_rect.top = 0;
    scissor_rect.right = context.render_target_desc.Width;
    scissor_rect.bottom = context.render_target_desc.Height;

    for (i = 0; i < ARRAY_SIZE(viewport_offsets); ++i)
    {
        viewport.TopLeftX = viewport_offsets[i];
        viewport.TopLeftY = viewport_offsets[i];
        viewport.Width = context.render_target_desc.Width;
        viewport.Height = context.render_target_desc.Height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        if (i)
            transition_resource_state(command_list, context.render_target,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 4, 1, 0, 0);

        transition_resource_state(command_list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
        for (y = 0; y < context.render_target_desc.Height; ++y)
        {
            for (x = 0; x < context.render_target_desc.Width; ++x)
            {
                const struct vec4 *v = get_readback_vec4(&rb, x, y);
                struct vec4 expected = {x + 0.5f, y + 0.5f,
                        (x + 0.5f - viewport_offsets[i]) / context.render_target_desc.Width,
                        1.0f - (y + 0.5f - viewport_offsets[i]) / context.render_target_desc.Height};
                ok(compare_float(v->x, expected.x, 0) && compare_float(v->y, expected.y, 0),
                        "Got fragcoord {%.8e, %.8e}, expected {%.8e, %.8e} at (%u, %u), offset %.8e.\n",
                        v->x, v->y, expected.x, expected.y, x, y, viewport_offsets[i]);
                ok(compare_float(v->z, expected.z, 2) && compare_float(v->w, expected.w, 2),
                        "Got texcoord {%.8e, %.8e}, expected {%.8e, %.8e} at (%u, %u), offset %.8e.\n",
                        v->z, v->w, expected.z, expected.w, x, y, viewport_offsets[i]);
            }
        }
        release_resource_readback(&rb);

        hr = ID3D12CommandAllocator_Reset(context.allocator);
        ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
        hr = ID3D12GraphicsCommandList_Reset(command_list, context.allocator, NULL);
        ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);
    }

    ID3D12Resource_Release(vb);
    destroy_draw_test_context(&context);
}

static void test_texture_resource_barriers(void)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_BARRIER barriers[8];
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12CommandQueue *queue;
    ID3D12Resource *resource;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].Transition.pResource = resource;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[0]);

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].UAV.pResource = resource;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[1]);

    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[2].Transition.pResource = resource;
    barriers[2].Transition.Subresource = 0;
    barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[2]);

    barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[3].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[3].Transition.pResource = resource;
    barriers[3].Transition.Subresource = 0;
    barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[3]);

    barriers[4].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[4].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[4].Transition.pResource = resource;
    barriers[4].Transition.Subresource = 0;
    barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[4].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[4]);

    barriers[5].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[5].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[5].Transition.pResource = resource;
    barriers[5].Transition.Subresource = 0;
    barriers[5].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[5].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[5]);

    barriers[6].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[6].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[6].UAV.pResource = resource;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[6]);
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[6]);

    barriers[7].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[7].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[7].Transition.pResource = resource;
    barriers[7].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[7].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[7].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barriers[7]);

    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 8, barriers);

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);
    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);

    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12Resource_Release(resource);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_invalid_texture_resource_barriers(void)
{
    ID3D12Resource *texture, *readback_buffer, *upload_buffer;
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL,
            &IID_ID3D12Resource, (void **)&texture);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Height = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = 0;

    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&upload_buffer);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
    hr = ID3D12Device_CreateCommittedResource(device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&readback_buffer);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    /* The following invalid barrier is not detected by the runtime. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(SUCCEEDED(hr), "Close failed, hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);

    /* The before state does not match with the previous state. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    /* The returned error code has changed after a Windows update. */
    ok(hr == S_OK || hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (hr == S_OK)
    {
        exec_command_list(queue, command_list);
        wait_queue_idle(device, queue);
    }

    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* The before state does not match with the previous state. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    /* The returned error code has changed after a Windows update. */
    ok(hr == S_OK || hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);
    if (hr == S_OK)
    {
        exec_command_list(queue, command_list);
        wait_queue_idle(device, queue);
    }

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Exactly one write state or a combination of read-only states are allowed. */
    transition_resource_state(command_list, texture,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Readback resources cannot transition from D3D12_RESOURCE_STATE_COPY_DEST. */
    transition_resource_state(command_list, readback_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    todo(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12CommandAllocator_Reset(command_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    ID3D12GraphicsCommandList_Release(command_list);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Upload resources cannot transition from D3D12_RESOURCE_STATE_GENERIC_READ. */
    transition_resource_state(command_list, upload_buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    todo(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(queue);
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12Resource_Release(readback_buffer);
    ID3D12Resource_Release(texture);
    ID3D12Resource_Release(upload_buffer);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_device_removed_reason(void)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;
    ID3D12CommandQueue *queue, *tmp_queue;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_GetDeviceRemovedReason(device);
    ok(hr == S_OK, "Got unexpected hr %#x.\n", hr);

    command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(SUCCEEDED(hr), "CreateCommandQueue failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&command_list);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    /* Execute a command list in the recording state. */
    exec_command_list(queue, command_list);

    hr = ID3D12Device_GetDeviceRemovedReason(device);
    todo(hr == DXGI_ERROR_INVALID_CALL, "Got unexpected hr %#x.\n", hr);

    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&tmp_queue);
    todo(hr == DXGI_ERROR_DEVICE_REMOVED, "Got unexpected hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12CommandQueue_Release(tmp_queue);

    hr = ID3D12Device_GetDeviceRemovedReason(device);
    todo(hr == DXGI_ERROR_INVALID_CALL, "Got unexpected hr %#x.\n", hr);

    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(queue);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_map_resource(void)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *resource;
    ID3D12Device *device;
    ULONG refcount;
    void *data;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = 0;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    /* Resources on a DEFAULT heap cannot be mapped. */
    hr = ID3D12Resource_Map(resource, 0, NULL, &data);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12Resource_Release(resource);

    heap_properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    if (FAILED(hr))
    {
        skip("Failed to create texture on custom heap.\n");
    }
    else
    {
        /* The data pointer must be NULL for the UNKNOWN layout. */
        hr = ID3D12Resource_Map(resource, 0, NULL, &data);
        ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

        ID3D12Resource_Release(resource);
    }

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Height = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    /* Resources on a DEFAULT heap cannot be mapped. */
    hr = ID3D12Resource_Map(resource, 0, NULL, &data);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x.\n", hr);

    ID3D12Resource_Release(resource);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void test_bundle_state_inheritance(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list, *bundle;
    ID3D12CommandAllocator *bundle_allocator;
    struct draw_test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    D3D12_VIEWPORT viewport;
    ID3D12Device *device;
    RECT scissor_rect;
    unsigned int x, y;
    HRESULT hr;

#ifndef _WIN32
    /* Avoid 2048 test todos. */
    skip("Bundles are not implemented yet.\n");
    return;
#endif

    if (!init_draw_test_context(&context, NULL))
        return;
    device = context.device;
    command_list = context.list;
    queue = context.queue;

    hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&bundle_allocator);
    ok(SUCCEEDED(hr), "CreateCommandAllocator failed, hr %#x.\n", hr);
    hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            bundle_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&bundle);
    ok(SUCCEEDED(hr), "CreateCommandList failed, hr %#x.\n", hr);

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = context.render_target_desc.Width;
    viewport.Height = context.render_target_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 0.0f;

    scissor_rect.left = scissor_rect.top = 0;
    scissor_rect.right = context.render_target_desc.Width;
    scissor_rect.bottom = context.render_target_desc.Height;

    /* A bundle does not inherit the current pipeline state. */
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);

    ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < context.render_target_desc.Height; ++y)
    {
        for (x = 0; x < context.render_target_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           /* This works on AMD. */
           ok(v == 0xffffffff || v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    hr = ID3D12CommandAllocator_Reset(context.allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, context.allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(bundle_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(bundle, bundle_allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);

    /* A bundle does not inherit the current primitive topology. */
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);

    ID3D12GraphicsCommandList_SetPipelineState(bundle, context.pipeline_state);
    ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < context.render_target_desc.Height; ++y)
    {
        for (x = 0; x < context.render_target_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           /* This works on AMD, even though the debug layer says that the primitive topology is undefined. */
           ok(v == 0xffffffff || v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    hr = ID3D12CommandAllocator_Reset(context.allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, context.allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(bundle_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(bundle, bundle_allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);

    /* A bundle inherit all other states. */
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);

    ID3D12GraphicsCommandList_SetPipelineState(bundle, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < context.render_target_desc.Height; ++y)
    {
        for (x = 0; x < context.render_target_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           todo(v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    hr = ID3D12CommandAllocator_Reset(context.allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(command_list, context.allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);
    hr = ID3D12CommandAllocator_Reset(bundle_allocator);
    ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(bundle, bundle_allocator, NULL);
    ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);

    /* All state that is set in a bundle affects a command list. */
    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);

    ID3D12GraphicsCommandList_SetGraphicsRootSignature(bundle, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(bundle, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(SUCCEEDED(hr), "Failed to close bundle, hr %#x.\n", hr);

    ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);

    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_resource_state(command_list, context.render_target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
    for (y = 0; y < context.render_target_desc.Height; ++y)
    {
        for (x = 0; x < context.render_target_desc.Width; ++x)
        {
           unsigned int v = get_readback_uint(&rb, x, y);
           todo(v == 0xff00ff00, "Got unexpected value 0x%08x at (%u, %u).\n", v, x, y);
        }
    }
    release_resource_readback(&rb);

    ID3D12CommandAllocator_Release(bundle_allocator);
    ID3D12GraphicsCommandList_Release(bundle);
    destroy_draw_test_context(&context);
}

static void test_shader_instructions(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    const D3D12_SHADER_BYTECODE *current_ps;
    ID3D12GraphicsCommandList *command_list;
    D3D12_HEAP_PROPERTIES heap_properties;
    struct draw_test_context_desc desc;
    D3D12_RESOURCE_DESC resource_desc;
    struct draw_test_context context;
    struct resource_readback rb;
    ID3D12CommandQueue *queue;
    D3D12_VIEWPORT viewport;
    unsigned int i, x, y;
    ID3D12Resource *cb;
    RECT scissor_rect;
    HRESULT hr;
    void *ptr;

    static const DWORD ps_dot2_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst.x = dot(src0.xy, src1.xy);
            dst.yzw = (float3)0;
        }
#endif
        0x43425844, 0x3621a1c7, 0x79d3be21, 0x9f14138c, 0x9f5506f2, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x0900000f, 0x00102012, 0x00000000, 0x00208046, 0x00000000, 0x00000000, 0x00208046, 0x00000000,
        0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_dot2 = {ps_dot2_code, sizeof(ps_dot2_code)};
    static const DWORD ps_eq_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst = (uint4)0;
            if (src0.x == src1.x)
                dst.x = asfloat(0xffffffff);
        }
#endif
        0x43425844, 0x7bce1728, 0xa7d5d0f0, 0xaef5bc00, 0x7bb6b161, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000018, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000,
        0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_eq = {ps_eq_code, sizeof(ps_eq_code)};
    static const DWORD ps_ne_code[] =
    {
#if 0
        float4 src0;
        float4 src1;

        void main(out float4 dst : SV_Target)
        {
            dst = (uint4)0;
            if (src0.x != src1.x)
                dst.x = asfloat(0xffffffff);
        }
#endif
        0x43425844, 0x5bbb7f90, 0x1a44971c, 0x4ee3d92e, 0x149ceecf, 0x00000001, 0x000000e8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000070, 0x00000050, 0x0000001c,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000039, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020800a, 0x00000000,
        0x00000001, 0x08000036, 0x001020e2, 0x00000000, 0x00004002, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ne = {ps_ne_code, sizeof(ps_ne_code)};
    static const DWORD ps_if_code[] =
    {
        /* compiled with /Gfp option */
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            if (src0.x)
                dst = float4(0, 1, 0, 1);
            else
                dst = float4(1, 0, 0, 1);
        }
#endif
        0x43425844, 0xfe5b6a47, 0x123f8934, 0xfa5910fe, 0x497aad93, 0x00000001, 0x0000012c, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000b4, 0x00000050, 0x0000002d,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x0b000039, 0x00100012, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000,
        0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000,
        0x01000012, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x00000000, 0x00000000,
        0x3f800000, 0x01000015, 0x0100003e
    };
    static const D3D12_SHADER_BYTECODE ps_if = {ps_if_code, sizeof(ps_if_code)};
    static const DWORD ps_if_return_code[] =
    {
#if 0
        float4 src0;

        void main(out float4 dst : SV_Target)
        {
            dst = (float4)0;
            if (src0.x < 4)
                return;
            dst.x = 1;
            if (src0.y < 4)
                return;
            dst.y = 1;
            if (src0.z >= 4)
                return;
            dst.z = 1;
            if (src0.w <= src0.x)
                return;
            dst.w = 1;
        }
#endif
        0x43425844, 0xa2797349, 0xd0a60aee, 0x7ae89f23, 0xf9681bfe, 0x00000001, 0x00000220, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000001a8, 0x00000050, 0x0000006a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x08000031, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000,
        0x00004001, 0x40800000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e, 0x01000015, 0x08000031,
        0x00100012, 0x00000000, 0x0020801a, 0x00000000, 0x00000000, 0x00004001, 0x40800000, 0x0304001f,
        0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x00000000,
        0x00000000, 0x00000000, 0x0100003e, 0x01000015, 0x0800001d, 0x00100012, 0x00000000, 0x0020802a,
        0x00000000, 0x00000000, 0x00004001, 0x40800000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x3f800000, 0x00000000, 0x00000000, 0x0100003e,
        0x01000015, 0x0900001d, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0020803a,
        0x00000000, 0x00000000, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x3f800000, 0x3f800000, 0x3f800000, 0x00000000, 0x0100003e, 0x01000015, 0x08000036,
        0x001020f2, 0x00000000, 0x00004002, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_if_return = {ps_if_return_code, sizeof(ps_if_return_code)};
    static const DWORD ps_ftou_code[] =
    {
#if 0
        float src;

        void main(out float4 dst : SV_Target)
        {
            dst = asfloat(uint4(src, -src, 0, 0));
        }
#endif
        0x43425844, 0x7a61c2fa, 0x4f20de14, 0x3492a5ae, 0x0a1fdc98, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0600001c, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0700001c, 0x00102022,
        0x00000000, 0x8020800a, 0x00000041, 0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ftou = {ps_ftou_code, sizeof(ps_ftou_code)};
    static const DWORD ps_ftoi_code[] =
    {
#if 0
        float src;

        void main(out float4 dst : SV_Target)
        {
            dst = asfloat(int4(src, -src, 0, 0));
        }
#endif
        0x43425844, 0x2737f059, 0x5a2faecc, 0x7eab1956, 0xf96357b5, 0x00000001, 0x000000f8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000080, 0x00000050, 0x00000020,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0600001b, 0x00102012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x0700001b, 0x00102022,
        0x00000000, 0x8020800a, 0x00000041, 0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000,
        0x00004002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ftoi = {ps_ftoi_code, sizeof(ps_ftoi_code)};
    static const DWORD ps_bfi_code[] =
    {
#if 0
        uint bits, offset, insert, base;

        uint4 main() : SV_Target
        {
            uint mask = ((1 << bits) - 1) << offset;
            return ((insert << offset) & mask) | (base & ~mask);
        }
#endif
        0x43425844, 0xbe9af688, 0xf5caec6f, 0x63ed2522, 0x5f91f209, 0x00000001, 0x000000e0, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000068, 0x00000050, 0x0000001a,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0f00008c, 0x001020f2, 0x00000000, 0x00208006, 0x00000000, 0x00000000, 0x00208556, 0x00000000,
        0x00000000, 0x00208aa6, 0x00000000, 0x00000000, 0x00208ff6, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_bfi = {ps_bfi_code, sizeof(ps_bfi_code)};
    static const DWORD ps_ibfe_code[] =
    {
#if 0
        ps_5_0
        dcl_globalFlags refactoringAllowed
        dcl_constantbuffer cb0[1], immediateIndexed
        dcl_output o0.xyzw
        ibfe o0.xyzw, cb0[0].xxxx, cb0[0].yyyy, cb0[0].zzzz
        ret
#endif
        0x43425844, 0x4b2225f7, 0xd0860f66, 0xe38775bb, 0x6d23d1d2, 0x00000001, 0x000000d4, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000005c, 0x00000050, 0x00000017,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x0c00008b, 0x001020f2, 0x00000000, 0x00208006, 0x00000000, 0x00000000, 0x00208556, 0x00000000,
        0x00000000, 0x00208aa6, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ibfe = {ps_ibfe_code, sizeof(ps_ibfe_code)};
    static const DWORD ps_ubfe_code[] =
    {
#if 0
        uint u;

        uint4 main() : SV_Target
        {
            return uint4((u & 0xf0) >> 4, (u & 0x7fffff00) >> 8, (u & 0xfe) >> 1, (u & 0x7fffffff) >> 1);
        }
#endif
        0x43425844, 0xc4ac0509, 0xaea83154, 0xf1fb3b80, 0x4c22e3cc, 0x00000001, 0x000000e4, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000006c, 0x00000050, 0x0000001b,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x1000008a, 0x001020f2, 0x00000000, 0x00004002, 0x00000004, 0x00000017, 0x00000007, 0x0000001e,
        0x00004002, 0x00000004, 0x00000008, 0x00000001, 0x00000001, 0x00208006, 0x00000000, 0x00000000,
        0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ubfe = {ps_ubfe_code, sizeof(ps_ubfe_code)};
    static const DWORD ps_bfrev_code[] =
    {
#if 0
        uint bits;

        uint4 main() : SV_Target
        {
            return uint4(reversebits(bits), reversebits(reversebits(bits)),
                    reversebits(bits & 0xFFFF), reversebits(bits >> 16));
        }
#endif
        0x43425844, 0x73daef82, 0xe52befa3, 0x8504d5f0, 0xebdb321d, 0x00000001, 0x00000154, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x000000dc, 0x00000050, 0x00000037,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x02000068, 0x00000001, 0x08000001, 0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000,
        0x00004001, 0x0000ffff, 0x0500008d, 0x00102042, 0x00000000, 0x0010000a, 0x00000000, 0x08000055,
        0x00100012, 0x00000000, 0x0020800a, 0x00000000, 0x00000000, 0x00004001, 0x00000010, 0x0500008d,
        0x00102082, 0x00000000, 0x0010000a, 0x00000000, 0x0600008d, 0x00100012, 0x00000000, 0x0020800a,
        0x00000000, 0x00000000, 0x0500008d, 0x00102022, 0x00000000, 0x0010000a, 0x00000000, 0x05000036,
        0x00102012, 0x00000000, 0x0010000a, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_bfrev = {ps_bfrev_code, sizeof(ps_bfrev_code)};
    static const DWORD ps_ishr_code[] =
    {
#if 0
        int4 src0;
        int4 src1;

        void main(out uint4 dst : SV_Target)
        {
            dst = src0 >> src1;
        }
#endif
        0x43425844, 0x4551d737, 0xd3dcf723, 0xdf387a99, 0xb6d6b00b, 0x00000001, 0x000000c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000050, 0x00000050, 0x00000014,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x0900002a, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00208e46, 0x00000000,
        0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ishr = {ps_ishr_code, sizeof(ps_ishr_code)};
    static const DWORD ps_ushr_code[] =
    {
#if 0
        uint4 src0;
        uint4 src1;

        void main(out uint4 dst : SV_Target)
        {
            dst = src0 >> src1;
        }
#endif
        0x43425844, 0x00f49f17, 0xe7933d92, 0xf527d4e6, 0x1fe1c216, 0x00000001, 0x000000c8, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000050, 0x00000050, 0x00000014,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000002, 0x03000065, 0x001020f2, 0x00000000,
        0x09000055, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x00208e46, 0x00000000,
        0x00000001, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps_ushr = {ps_ushr_code, sizeof(ps_ushr_code)};
    static const struct
    {
        const D3D12_SHADER_BYTECODE *ps;
        struct
        {
            struct vec4 src0;
            struct vec4 src1;
        } input;
        union
        {
            struct vec4 f;
            struct uvec4 u;
            struct ivec4 i;
        } output;
    }
    tests[] =
    {
        {&ps_dot2, {{1.0f, 1.0f}, {1.0f, 1.0f}}, {{2.0f}}},
        {&ps_dot2, {{1.0f, 1.0f}, {2.0f, 3.0f}}, {{5.0f}}},

        {&ps_eq, {{0.0f}, {0.0f}}, {.u = {0xffffffff}}},
        {&ps_eq, {{1.0f}, {0.0f}}, {.u = {0x00000000}}},
        {&ps_eq, {{0.0f}, {1.0f}}, {.u = {0x00000000}}},
        {&ps_eq, {{1.0f}, {1.0f}}, {.u = {0xffffffff}}},
        {&ps_eq, {{0.0f},  {NAN}}, {.u = {0x00000000}}},
        {&ps_eq, {{1.0f},  {NAN}}, {.u = {0x00000000}}},
        {&ps_eq, { {NAN},  {NAN}}, {.u = {0x00000000}}},

        {&ps_ne, {{0.0f}, {0.0f}}, {.u = {0x00000000}}},
        {&ps_ne, {{1.0f}, {0.0f}}, {.u = {0xffffffff}}},
        {&ps_ne, {{0.0f}, {1.0f}}, {.u = {0xffffffff}}},
        {&ps_ne, {{1.0f}, {1.0f}}, {.u = {0x00000000}}},
        {&ps_ne, {{0.0f},  {NAN}}, {.u = {0xffffffff}}},
        {&ps_ne, {{1.0f},  {NAN}}, {.u = {0xffffffff}}},
        {&ps_ne, { {NAN},  {NAN}}, {.u = {0xffffffff}}},

        {&ps_if, {{0.0f}}, {{1.0f, 0.0f, 0.0f, 1.0f}}},
        {&ps_if, {{1.0f}}, {{0.0f, 1.0f, 0.0f, 1.0f}}},

        {&ps_if_return, {{0.0f, 0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{ NAN, 0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{3.0f, 0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 0.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f,  NAN, 0.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 3.0f, 0.0f, 0.0f}}, {{1.0f, 0.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f,  NAN, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 3.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 4.0f, 0.0f}}, {{1.0f, 1.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 5.0f, 0.0f}}, {{1.0f, 1.0f, 0.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f,  NAN}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 1.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 2.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 3.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 4.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{4.0f, 4.0f, 0.0f, 5.0f}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},
        {&ps_if_return, {{5.0f, 4.0f, 0.0f, 5.0f}}, {{1.0f, 1.0f, 1.0f, 0.0f}}},
        {&ps_if_return, {{ NAN,  NAN,  NAN,  NAN}}, {{1.0f, 1.0f, 1.0f, 1.0f}}},

        {&ps_ftou, {{     -NAN}}, {.u = { 0,  0 }}},
        {&ps_ftou, {{      NAN}}, {.u = { 0,  0 }}},
        {&ps_ftou, {{-INFINITY}}, {.u = { 0, ~0u}}},
        {&ps_ftou, {{ INFINITY}}, {.u = {~0u, 0 }}},
        {&ps_ftou, {{    -1.0f}}, {.u = { 0,  1 }}},
        {&ps_ftou, {{     1.0f}}, {.u = { 1,  0 }}},

        {&ps_ftoi, {{     -NAN}}, {.u = {      0,       0}}},
        {&ps_ftoi, {{      NAN}}, {.u = {      0,       0}}},
        {&ps_ftoi, {{-INFINITY}}, {.u = {INT_MIN, INT_MAX}}},
        {&ps_ftoi, {{ INFINITY}}, {.i = {INT_MAX, INT_MIN}}},
        {&ps_ftoi, {{    -1.0f}}, {.i = {     -1,       1}}},
        {&ps_ftoi, {{     1.0f}}, {.i = {      1,      -1}}},
    };

    static const struct
    {
        const D3D12_SHADER_BYTECODE *ps;
        struct
        {
            struct uvec4 src0;
            struct uvec4 src1;
        } input;
        union
        {
            struct uvec4 u;
            struct ivec4 i;
            struct vec4 f;
        } output;
    }
    uint_tests[] =
    {
        {&ps_bfi, {{     0,      0,    0,    0}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{     0,      0,    0,    1}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{   ~0u,      0,  ~0u,    0}}, {{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}}},
        {&ps_bfi, {{   ~0u,    ~0u,  ~0u,    0}}, {{0x80000000, 0x80000000, 0x80000000, 0x80000000}}},
        {&ps_bfi, {{   ~0u,  0x1fu,  ~0u,    0}}, {{0x80000000, 0x80000000, 0x80000000, 0x80000000}}},
        {&ps_bfi, {{   ~0u, ~0x1fu,  ~0u,    0}}, {{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}}},
        {&ps_bfi, {{     0,      0, 0xff,    1}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{     0,      0, 0xff,    2}}, {{         2,          2,          2,          2}}},
        {&ps_bfi, {{    16,     16, 0xff, 0xff}}, {{0x00ff00ff, 0x00ff00ff, 0x00ff00ff, 0x00ff00ff}}},
        {&ps_bfi, {{     0,      0,  ~0u,  ~0u}}, {{       ~0u,        ~0u,        ~0u,        ~0u}}},
        {&ps_bfi, {{~0x1fu,      0,  ~0u,    0}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{~0x1fu,      0,  ~0u,    1}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{~0x1fu,      0,  ~0u,    2}}, {{         2,          2,          2,          2}}},
        {&ps_bfi, {{     0, ~0x1fu,  ~0u,    0}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{     0, ~0x1fu,  ~0u,    1}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{     0, ~0x1fu,  ~0u,    2}}, {{         2,          2,          2,          2}}},
        {&ps_bfi, {{~0x1fu, ~0x1fu,  ~0u,    0}}, {{         0,          0,          0,          0}}},
        {&ps_bfi, {{~0x1fu, ~0x1fu,  ~0u,    1}}, {{         1,          1,          1,          1}}},
        {&ps_bfi, {{~0x1fu, ~0x1fu,  ~0u,    2}}, {{         2,          2,          2,          2}}},

        {&ps_ibfe, {{ 0,  4, 0x00000000}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{ 0,  4, 0xffffffff}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{ 0,  4, 0x7fffffff}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{ 4,  0, 0x00000000}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{ 4,  0, 0xfffffffa}}, {{0xfffffffa, 0xfffffffa, 0xfffffffa, 0xfffffffa}}},
        {&ps_ibfe, {{ 4,  0, 0x7ffffffc}}, {{0xfffffffc, 0xfffffffc, 0xfffffffc, 0xfffffffc}}},
        {&ps_ibfe, {{ 4,  4, 0x00000000}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{ 4,  4, 0xffffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{ 4,  4, 0xffffff1f}}, {{0x00000001, 0x00000001, 0x00000001, 0x00000001}}},
        {&ps_ibfe, {{ 4,  4, 0x7fffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{23,  8, 0x00000000}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{23,  8, 0xffffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{23,  8, 0x7fffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{30,  1, 0x00000000}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ibfe, {{30,  1, 0xffffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{30,  1, 0x7fffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{15, 15, 0x7fffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{15, 15, 0x3fffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{15, 15, 0x1fffffff}}, {{0x00003fff, 0x00003fff, 0x00003fff, 0x00003fff}}},
        {&ps_ibfe, {{15, 15, 0xffff00ff}}, {{0xfffffffe, 0xfffffffe, 0xfffffffe, 0xfffffffe}}},
        {&ps_ibfe, {{16, 15, 0xffffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{16, 15, 0x3fffffff}}, {{0x00007fff, 0x00007fff, 0x00007fff, 0x00007fff}}},
        {&ps_ibfe, {{20, 15, 0xffffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{31, 31, 0xffffffff}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{31, 31, 0x80000000}}, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ibfe, {{31, 31, 0x7fffffff}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},

        {&ps_ubfe, {{0x00000000}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ubfe, {{0xffffffff}}, {{0x0000000f, 0x007fffff, 0x0000007f, 0x3fffffff}}},
        {&ps_ubfe, {{0xff000000}}, {{0x00000000, 0x007f0000, 0x00000000, 0x3f800000}}},
        {&ps_ubfe, {{0x00ff0000}}, {{0x00000000, 0x0000ff00, 0x00000000, 0x007f8000}}},
        {&ps_ubfe, {{0x000000ff}}, {{0x0000000f, 0x00000000, 0x0000007f, 0x0000007f}}},
        {&ps_ubfe, {{0x80000001}}, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ubfe, {{0xc0000003}}, {{0x00000000, 0x00400000, 0x00000001, 0x20000001}}},

        {&ps_bfrev, {{0x12345678}}, {{0x1e6a2c48, 0x12345678, 0x1e6a0000, 0x2c480000}}},
        {&ps_bfrev, {{0xffff0000}}, {{0x0000ffff, 0xffff0000, 0x00000000, 0xffff0000}}},
        {&ps_bfrev, {{0xffffffff}}, {{0xffffffff, 0xffffffff, 0xffff0000, 0xffff0000}}},

        {&ps_ishr, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {~0x1fu, 0, 32, 64}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ishr, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}, {~0x1fu, 0, 32, 64}},
                   {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ishr, {{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}, {~0x1fu, 0, 32, 64}},
                   {{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}}},
        {&ps_ishr, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {    31, 7, 15, 11}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ishr, {{0x80000000, 0x80000000, 0x80000000, 0x80000000}, {    31, 7, 15, 11}},
                   {{0xffffffff, 0xff000000, 0xffff0000, 0xfff00000}}},

        {&ps_ushr, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {~0x1fu, 0, 32, 64}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ushr, {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}, {~0x1fu, 0, 32, 64}},
                   {{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}}},
        {&ps_ushr, {{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}, {~0x1fu, 0, 32, 64}},
                   {{0xfefefefe, 0x0fefefef, 0x0f0f0f0f, 0x12345678}}},
        {&ps_ushr, {{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {    31, 7, 15, 11}},
                   {{0x00000000, 0x00000000, 0x00000000, 0x00000000}}},
        {&ps_ushr, {{0x80000000, 0x80000000, 0x80000000, 0x80000000}, {    31, 7, 15, 11}},
                   {{0x00000001, 0x01000000, 0x00010000, 0x00100000}}},
    };

    assert(sizeof(tests->input) == sizeof(uint_tests->input));

    memset(&desc, 0, sizeof(desc));
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.no_root_signature = true;
    if (!init_draw_test_context(&context, &desc))
        return;
    command_list = context.list;
    queue = context.queue;

    context.root_signature = create_cb_root_signature(context.device,
            0, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = sizeof(tests->input);
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = ID3D12Device_CreateCommittedResource(context.device, &heap_properties,
            D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL, &IID_ID3D12Resource, (void **)&cb);
    ok(SUCCEEDED(hr), "CreateCommittedResource failed, hr %#x.\n", hr);

    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = context.render_target_desc.Width;
    viewport.Height = context.render_target_desc.Height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    scissor_rect.left = scissor_rect.top = 0;
    scissor_rect.right = context.render_target_desc.Width;
    scissor_rect.bottom = context.render_target_desc.Height;

    current_ps = NULL;
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        if (current_ps != tests[i].ps)
        {
            if (context.pipeline_state)
                ID3D12PipelineState_Release(context.pipeline_state);
            current_ps = tests[i].ps;
            context.pipeline_state = create_pipeline_state(context.device,
                    context.root_signature, desc.rt_format, NULL, current_ps, NULL);
        }

        hr = ID3D12Resource_Map(cb, 0, NULL, (void **)&ptr);
        ok(SUCCEEDED(hr), "Failed to map constant buffer, hr %#x.\n", hr);
        memcpy(ptr, &tests[i].input, sizeof(tests[i].input));
        ID3D12Resource_Unmap(cb, 0, NULL);

        if (i)
            transition_resource_state(command_list, context.render_target,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list, 0,
                ID3D12Resource_GetGPUVirtualAddress(cb));
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_resource_state(command_list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
        for (y = 0; y < context.render_target_desc.Height; ++y)
        {
            for (x = 0; x < context.render_target_desc.Width; ++x)
            {
                const struct vec4 *v = get_readback_vec4(&rb, x, y);
                ok(compare_vec4(v, &tests[i].output.f, 0),
                        "Got %.8e, %.8e, %.8e, %.8e expected %.8e, %.8e, %.8e, %.8e.\n",
                        v->x, v->y, v->z, v->w, tests[i].output.f.x, tests[i].output.f.y,
                        tests[i].output.f.z, tests[i].output.f.w);
            }
        }
        release_resource_readback(&rb);

        hr = ID3D12CommandAllocator_Reset(context.allocator);
        ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
        hr = ID3D12GraphicsCommandList_Reset(command_list, context.allocator, NULL);
        ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);
    }

    ID3D12Resource_Release(context.render_target);
    desc.rt_format = DXGI_FORMAT_R32G32B32A32_UINT;
    create_render_target(&context, &desc);

    for (i = 0; i < ARRAY_SIZE(uint_tests); ++i)
    {
        if (current_ps != uint_tests[i].ps)
        {
            if (context.pipeline_state)
                ID3D12PipelineState_Release(context.pipeline_state);
            current_ps = uint_tests[i].ps;
            context.pipeline_state = create_pipeline_state(context.device,
                    context.root_signature, desc.rt_format, NULL, current_ps, NULL);
        }

        hr = ID3D12Resource_Map(cb, 0, NULL, (void **)&ptr);
        ok(SUCCEEDED(hr), "Failed to map constant buffer, hr %#x.\n", hr);
        memcpy(ptr, &uint_tests[i].input, sizeof(uint_tests[i].input));
        ID3D12Resource_Unmap(cb, 0, NULL);

        if (i)
            transition_resource_state(command_list, context.render_target,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list, 0,
                ID3D12Resource_GetGPUVirtualAddress(cb));
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_resource_state(command_list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        get_texture_readback_with_command_list(context.render_target, 0, &rb, queue, command_list);
        for (y = 0; y < context.render_target_desc.Height; ++y)
        {
            for (x = 0; x < context.render_target_desc.Width; ++x)
            {
                const struct uvec4 *v = get_readback_uvec4(&rb, x, y);
                ok(compare_uvec4(v, &uint_tests[i].output.u),
                        "Got 0x%08x, 0x%08x, 0x%08x, 0x%08x expected 0x%08x, 0x%08x, 0x%08x, 0x%08x.\n",
                        v->x, v->y, v->z, v->w, uint_tests[i].output.u.x, uint_tests[i].output.u.y,
                        uint_tests[i].output.u.z, uint_tests[i].output.u.w);
            }
        }
        release_resource_readback(&rb);

        hr = ID3D12CommandAllocator_Reset(context.allocator);
        ok(SUCCEEDED(hr), "Command allocator reset failed, hr %#x.\n", hr);
        hr = ID3D12GraphicsCommandList_Reset(command_list, context.allocator, NULL);
        ok(SUCCEEDED(hr), "Command list reset failed, hr %#x.\n", hr);
    }

    ID3D12Resource_Release(cb);
    destroy_draw_test_context(&context);
}

static void check_descriptor_range_(unsigned int line, const D3D12_DESCRIPTOR_RANGE *range,
        const D3D12_DESCRIPTOR_RANGE *expected_range)
{
    ok_(line)(range->RangeType == expected_range->RangeType,
            "Got range type %#x, expected %#x.\n", range->RangeType, expected_range->RangeType);
    ok_(line)(range->NumDescriptors == expected_range->NumDescriptors,
            "Got descriptor count %u, expected %u.\n", range->NumDescriptors, expected_range->NumDescriptors);
    ok_(line)(range->BaseShaderRegister == expected_range->BaseShaderRegister,
            "Got base shader register %u, expected %u.\n",
            range->BaseShaderRegister, expected_range->BaseShaderRegister);
    ok_(line)(range->RegisterSpace == expected_range->RegisterSpace,
            "Got register space %u, expected %u.\n", range->RegisterSpace, expected_range->RegisterSpace);
    ok_(line)(range->OffsetInDescriptorsFromTableStart == expected_range->OffsetInDescriptorsFromTableStart,
            "Got offset %u, expected %u.\n", range->OffsetInDescriptorsFromTableStart,
            expected_range->OffsetInDescriptorsFromTableStart);
}

static void check_root_parameter_(unsigned int line, const D3D12_ROOT_PARAMETER *parameter,
        const D3D12_ROOT_PARAMETER *expected_parameter)
{
    const D3D12_ROOT_DESCRIPTOR *descriptor, *expected_descriptor;
    const D3D12_ROOT_DESCRIPTOR_TABLE *table, *expected_table;
    const D3D12_ROOT_CONSTANTS *constants, *expected_constants;
    unsigned int i;

    ok_(line)(parameter->ParameterType == expected_parameter->ParameterType,
            "Got type %#x, expected %#x.\n", parameter->ParameterType, expected_parameter->ParameterType);
    if (parameter->ParameterType != expected_parameter->ParameterType)
        return;

    switch (parameter->ParameterType)
    {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            table = &parameter->DescriptorTable;
            expected_table = &expected_parameter->DescriptorTable;
            ok_(line)(table->NumDescriptorRanges == expected_table->NumDescriptorRanges,
                    "Got range count %u, expected %u.\n",
                    table->NumDescriptorRanges, expected_table->NumDescriptorRanges);
            if (table->NumDescriptorRanges == expected_table->NumDescriptorRanges)
            {
                for (i = 0; i < table->NumDescriptorRanges; ++i)
                    check_descriptor_range_(line, &table->pDescriptorRanges[i],
                            &expected_table->pDescriptorRanges[i]);
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            constants = &parameter->Constants;
            expected_constants = &expected_parameter->Constants;
            ok_(line)(constants->ShaderRegister == expected_constants->ShaderRegister,
                    "Got shader register %u, expected %u.\n",
                    constants->ShaderRegister, expected_constants->ShaderRegister);
            ok_(line)(constants->RegisterSpace == expected_constants->RegisterSpace,
                    "Got register space %u, expected %u.\n",
                    constants->RegisterSpace, expected_constants->RegisterSpace);
            ok_(line)(constants->Num32BitValues == expected_constants->Num32BitValues,
                    "Got 32-bit value count %u, expected %u.\n",
                    constants->Num32BitValues, expected_constants->Num32BitValues);
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            descriptor = &parameter->Descriptor;
            expected_descriptor = &expected_parameter->Descriptor;
            ok_(line)(descriptor->ShaderRegister == expected_descriptor->ShaderRegister,
                    "Got shader register %u, expected %u.\n",
                    descriptor->ShaderRegister, expected_descriptor->ShaderRegister);
            ok_(line)(descriptor->RegisterSpace == expected_descriptor->RegisterSpace,
                    "Got register space %u, expected %u.\n",
                    descriptor->RegisterSpace, expected_descriptor->RegisterSpace);
            break;
        default:
            trace("Unhandled type %#x.\n", parameter->ParameterType);
    }

    ok_(line)(parameter->ShaderVisibility == expected_parameter->ShaderVisibility,
            "Got shader visibility %#x, expected %#x.\n",
            parameter->ShaderVisibility, expected_parameter->ShaderVisibility);
}

#define check_root_signature_desc(desc, expected) check_root_signature_desc_(__LINE__, desc, expected)
static void check_root_signature_desc_(unsigned int line, const D3D12_ROOT_SIGNATURE_DESC *desc,
        const D3D12_ROOT_SIGNATURE_DESC *expected_desc)
{
    unsigned int i;

    ok_(line)(desc->NumParameters == expected_desc->NumParameters,
            "Got parameter count %u, expected %u.\n",
            desc->NumParameters, expected_desc->NumParameters);
    if (!expected_desc->pParameters)
    {
        ok_(line)(!desc->pParameters, "Got unexpected parameters %p.\n", desc->pParameters);
    }
    else if (desc->NumParameters == expected_desc->NumParameters)
    {
        for (i = 0; i < desc->NumParameters; ++i)
            check_root_parameter_(line, &desc->pParameters[i], &expected_desc->pParameters[i]);
    }
    ok_(line)(desc->NumStaticSamplers == expected_desc->NumStaticSamplers,
            "Got static sampler count %u, expected %u.\n",
            desc->NumStaticSamplers, expected_desc->NumStaticSamplers);
    if (!expected_desc->pStaticSamplers)
        ok_(line)(!desc->pStaticSamplers, "Got unexpected static samplers %p.\n", desc->pStaticSamplers);
    else
        trace("Implement samplers comparison!\n");
    ok_(line)(desc->Flags == expected_desc->Flags, "Got flags %#x, expected %#x.\n",
            desc->Flags, expected_desc->Flags);
}

#define test_root_signature_deserialization(a, b, c) test_root_signature_deserialization_(__LINE__, a, b, c)
static void test_root_signature_deserialization_(unsigned int line, const DWORD *code, size_t code_size,
        const D3D12_ROOT_SIGNATURE_DESC *expected_desc)
{
    ID3D12RootSignatureDeserializer *deserializer;
    const D3D12_ROOT_SIGNATURE_DESC *desc;
    ULONG refcount;
    HRESULT hr;

    hr = D3D12CreateRootSignatureDeserializer(code, code_size,
            &IID_ID3D12RootSignatureDeserializer, (void **)&deserializer);
    ok_(line)(hr == S_OK, "Failed to create deserializer, hr %#x.\n", hr);

    desc = ID3D12RootSignatureDeserializer_GetRootSignatureDesc(deserializer);
    ok(desc, "Got NULL root signature desc.\n");
    check_root_signature_desc_(line, desc, expected_desc);

    refcount = ID3D12RootSignatureDeserializer_Release(deserializer);
    ok_(line)(!refcount, "ID3D12RootSignatureDeserializer has %u references left.\n", (unsigned int)refcount);
}

static void test_root_signature_deserializer(void)
{
    ID3D12RootSignatureDeserializer *deserializer;
    ULONG refcount;
    HRESULT hr;

    /* /T rootsig_1_0 /E RS */
    static const DWORD empty_rootsig[] =
    {
#if 0
        #define RS ""
#endif
        0x43425844, 0xd64afc1d, 0x5dc27735, 0x9edacb4a, 0x6bd8a7fa, 0x00000001, 0x00000044, 0x00000001,
        0x00000024, 0x30535452, 0x00000018, 0x00000001, 0x00000000, 0x00000018, 0x00000000, 0x00000018,
        0x00000000,
    };
    static const D3D12_ROOT_SIGNATURE_DESC empty_rootsig_desc =
    {
        .Flags = 0,
    };
    static const DWORD ia_rootsig[] =
    {
#if 0
        #define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)"
#endif
        0x43425844, 0x05bbd62e, 0xc74d3646, 0xde1407a5, 0x0d99273d, 0x00000001, 0x00000044, 0x00000001,
        0x00000024, 0x30535452, 0x00000018, 0x00000001, 0x00000000, 0x00000018, 0x00000000, 0x00000018,
        0x00000001,
    };
    static const D3D12_ROOT_SIGNATURE_DESC ia_rootsig_desc =
    {
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
    };
    static const DWORD deny_ps_rootsig[] =
    {
#if 0
        #define RS "RootFlags(DENY_PIXEL_SHADER_ROOT_ACCESS)"
#endif
        0x43425844, 0xfad3a4ce, 0xf246286e, 0xeaa9e176, 0x278d5137, 0x00000001, 0x00000044, 0x00000001,
        0x00000024, 0x30535452, 0x00000018, 0x00000001, 0x00000000, 0x00000018, 0x00000000, 0x00000018,
        0x00000020,
    };
    static const D3D12_ROOT_SIGNATURE_DESC deny_ps_rootsig_desc =
    {
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS,
    };
    static const DWORD cbv_rootsig[] =
    {
#if 0
        #define RS "CBV(b3, space = 0)"
#endif
        0x43425844, 0x8dc5087e, 0x5cb9bf0d, 0x2e465ae3, 0x6291e0e0, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000000, 0x00000002, 0x00000000, 0x00000024, 0x00000003, 0x00000000,

    };
    static const D3D12_ROOT_PARAMETER cbv_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_CBV, .Descriptor = {3, 0}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC cbv_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(cbv_parameters),
        .pParameters = cbv_parameters,
    };
    static const DWORD cbv2_rootsig[] =
    {
#if 0
        #define RS "CBV(b4, space = 1, visibility = SHADER_VISIBILITY_GEOMETRY)"
#endif
        0x43425844, 0x6d4cfb48, 0xbfecaa8d, 0x379ff9c3, 0x0cc56997, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000000, 0x00000002, 0x00000004, 0x00000024, 0x00000004, 0x00000001,
    };
    static const D3D12_ROOT_PARAMETER cbv2_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_CBV, .Descriptor = {4, 1}, D3D12_SHADER_VISIBILITY_GEOMETRY},
    };
    static const D3D12_ROOT_SIGNATURE_DESC cbv2_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(cbv2_parameters),
        .pParameters = cbv2_parameters,
    };
    static const DWORD srv_rootsig[] =
    {
#if 0
        #define RS "RootFlags(DENY_VERTEX_SHADER_ROOT_ACCESS), SRV(t13)"
#endif
        0x43425844, 0xbc00e5e0, 0xffff2fd3, 0x85c2d405, 0xa61db5e5, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000002, 0x00000003, 0x00000000, 0x00000024, 0x0000000d, 0x00000000,
    };
    static const D3D12_ROOT_PARAMETER srv_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_SRV, .Descriptor = {13}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC srv_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(srv_parameters),
        .pParameters = srv_parameters,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS,
    };
    static const DWORD uav_rootsig[] =
    {
#if 0
        #define RS "UAV(u6)"
#endif
        0x43425844, 0xf873c52c, 0x69f5cbea, 0xaf6bc9f4, 0x2ccf8b54, 0x00000001, 0x00000058, 0x00000001,
        0x00000024, 0x30535452, 0x0000002c, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x0000002c,
        0x00000000, 0x00000004, 0x00000000, 0x00000024, 0x00000006, 0x00000000,
    };
    static const D3D12_ROOT_PARAMETER uav_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = {6}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC uav_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(uav_parameters),
        .pParameters = uav_parameters,
    };
    static const DWORD constants_rootsig[] =
    {
#if 0
        #define RS "RootConstants(num32BitConstants=3, b4), " \
                "RootConstants(num32BitConstants=4, b5, space = 3)"
#endif
        0x43425844, 0xbc015590, 0xa9a4a345, 0x7e446850, 0x2be05281, 0x00000001, 0x00000074, 0x00000001,
        0x00000024, 0x30535452, 0x00000048, 0x00000001, 0x00000002, 0x00000018, 0x00000000, 0x00000048,
        0x00000000, 0x00000001, 0x00000000, 0x00000030, 0x00000001, 0x00000000, 0x0000003c, 0x00000004,
        0x00000000, 0x00000003, 0x00000005, 0x00000003, 0x00000004,
    };
    static const D3D12_ROOT_PARAMETER constants_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = {4, 0, 3}},
        {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = {5, 3, 4}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC constants_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(constants_parameters),
        .pParameters = constants_parameters,
    };
    static const DWORD descriptor_table_rootsig[] =
    {
#if 0
        #define RS "DescriptorTable(CBV(b1, space = 7), " \
                "SRV(t16, numDescriptors = 8), " \
                "UAV(u3, numDescriptors = unbounded, offset = 44))"
#endif
        0x43425844, 0x0f92e563, 0x4766993f, 0x2304e283, 0x14f0d8dc, 0x00000001, 0x00000094, 0x00000001,
        0x00000024, 0x30535452, 0x00000068, 0x00000001, 0x00000001, 0x00000018, 0x00000000, 0x00000068,
        0x00000000, 0x00000000, 0x00000000, 0x00000024, 0x00000003, 0x0000002c, 0x00000002, 0x00000001,
        0x00000001, 0x00000007, 0xffffffff, 0x00000000, 0x00000008, 0x00000010, 0x00000000, 0xffffffff,
        0x00000001, 0xffffffff, 0x00000003, 0x00000000, 0x0000002c,
    };
    static const D3D12_DESCRIPTOR_RANGE descriptor_ranges[] =
    {
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,        1,  1, 7, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,        8, 16, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX,  3, 0,                                   44},
    };
    static const D3D12_ROOT_PARAMETER descriptor_table_parameters[] =
    {
        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                .DescriptorTable = {ARRAY_SIZE(descriptor_ranges), descriptor_ranges}},
    };
    static const D3D12_ROOT_SIGNATURE_DESC descriptor_table_rootsig_desc =
    {
        .NumParameters = ARRAY_SIZE(descriptor_table_parameters),
        .pParameters = descriptor_table_parameters,
    };
    /* TODO: static samplers */

    hr = D3D12CreateRootSignatureDeserializer(empty_rootsig, sizeof(empty_rootsig),
            &IID_IUnknown, (void **)&deserializer);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#x.\n", hr);

    hr = D3D12CreateRootSignatureDeserializer(empty_rootsig, sizeof(empty_rootsig),
            &IID_ID3D12RootSignatureDeserializer, (void **)&deserializer);
    ok(hr == S_OK, "Failed to create deserializer, hr %#x.\n", hr);

    check_interface(deserializer, &IID_IUnknown, FALSE);
    check_interface(deserializer, &IID_ID3D12RootSignatureDeserializer, TRUE);
    check_interface(deserializer, &IID_ID3D12Object, FALSE);
    check_interface(deserializer, &IID_ID3D12DeviceChild, FALSE);
    check_interface(deserializer, &IID_ID3D12Pageable, FALSE);

    refcount = ID3D12RootSignatureDeserializer_Release(deserializer);
    ok(!refcount, "ID3D12RootSignatureDeserializer has %u references left.\n", (unsigned int)refcount);

    test_root_signature_deserialization(empty_rootsig, sizeof(empty_rootsig), &empty_rootsig_desc);
    test_root_signature_deserialization(ia_rootsig, sizeof(ia_rootsig), &ia_rootsig_desc);
    test_root_signature_deserialization(deny_ps_rootsig, sizeof(deny_ps_rootsig), &deny_ps_rootsig_desc);
    test_root_signature_deserialization(cbv_rootsig, sizeof(cbv_rootsig), &cbv_rootsig_desc);
    test_root_signature_deserialization(cbv2_rootsig, sizeof(cbv2_rootsig), &cbv2_rootsig_desc);
    test_root_signature_deserialization(srv_rootsig, sizeof(srv_rootsig), &srv_rootsig_desc);
    test_root_signature_deserialization(uav_rootsig, sizeof(uav_rootsig), &uav_rootsig_desc);
    test_root_signature_deserialization(constants_rootsig, sizeof(constants_rootsig), &constants_rootsig_desc);
    test_root_signature_deserialization(descriptor_table_rootsig, sizeof(descriptor_table_rootsig),
            &descriptor_table_rootsig_desc);
}

START_TEST(d3d12)
{
    BOOL enable_debug_layer = FALSE;
    ID3D12Debug *debug;

    if (argc >= 2 && !strcmp(argv[1], "--validate"))
        enable_debug_layer = TRUE;

    if (enable_debug_layer && SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)))
    {
        ID3D12Debug_EnableDebugLayer(debug);
        ID3D12Debug_Release(debug);
    }

    run_test(test_create_device);
    run_test(test_node_count);
    run_test(test_check_feature_support);
    run_test(test_create_command_allocator);
    run_test(test_create_command_list);
    run_test(test_create_command_queue);
    run_test(test_create_committed_resource);
    run_test(test_create_descriptor_heap);
    run_test(test_create_root_signature);
    run_test(test_create_pipeline_state);
    run_test(test_create_fence);
    run_test(test_reset_command_allocator);
    run_test(test_cpu_signal_fence);
    run_test(test_gpu_signal_fence);
    run_test(test_multithread_fence_wait);
    run_test(test_clear_depth_stencil_view);
    run_test(test_clear_render_target_view);
    run_test(test_draw_instanced);
    run_test(test_draw_indexed_instanced);
    run_test(test_fragment_coords);
    run_test(test_fractional_viewports);
    run_test(test_texture_resource_barriers);
    run_test(test_invalid_texture_resource_barriers);
    run_test(test_device_removed_reason);
    run_test(test_map_resource);
    run_test(test_bundle_state_inheritance);
    run_test(test_shader_instructions);
    run_test(test_root_signature_deserializer);
}
