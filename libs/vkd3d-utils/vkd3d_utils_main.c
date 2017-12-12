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

#include "vkd3d_utils_private.h"

HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **debug)
{
    FIXME("riid %s, debug %p stub!\n", debugstr_guid(riid), debug);

    return E_NOTIMPL;
}

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter,
        D3D_FEATURE_LEVEL minimum_feature_level, REFIID riid, void **device)
{
    struct vkd3d_device_create_info create_info;

    TRACE("adapter %p, minimum_feature_level %#x, riid %s, device %p.\n",
            adapter, minimum_feature_level, debugstr_guid(riid), device);

    if (adapter)
        FIXME("Ignoring adapter %p.\n", adapter);

    create_info.minimum_feature_level = minimum_feature_level;
    create_info.signal_event_pfn = vkd3d_signal_event;
    create_info.wchar_size = sizeof(WCHAR);

    return vkd3d_create_device(&create_info, riid, device);
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T data_size,
        REFIID iid, void **deserializer)
{
    TRACE("data %p, data_size %lu, iid %s, deserializer %p.\n",
            data, data_size, debugstr_guid(iid), deserializer);

    return vkd3d_create_root_signature_deserializer(data, data_size, iid, deserializer);
}

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *root_signature_desc,
        D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error_blob)
{
    TRACE("root_signature_desc %p, version %#x, blob %p, error_blob %p.\n",
            root_signature_desc, version, blob, error_blob);

    return vkd3d_serialize_root_signature(root_signature_desc, version, blob, error_blob);
}

/* Events */
HANDLE vkd3d_create_event(void)
{
    struct vkd3d_event *event;
    int rc;

    TRACE(".\n");

    if (!(event = vkd3d_malloc(sizeof(*event))))
        return NULL;

    if ((rc = pthread_mutex_init(&event->mutex, NULL)))
    {
        ERR("Failed to initialize mutex, error %d.\n", rc);
        vkd3d_free(event);
        return NULL;
    }
    if ((rc = pthread_cond_init(&event->cond, NULL)))
    {
        ERR("Failed to initialize condition variable, error %d.\n", rc);
        pthread_mutex_destroy(&event->mutex);
        vkd3d_free(event);
        return NULL;
    }

    event->is_signaled = false;

    TRACE("Created event %p.\n", event);

    return event;
}

unsigned int vkd3d_wait_event(HANDLE event, unsigned int milliseconds)
{
    struct vkd3d_event *impl = event;
    int rc;

    TRACE("event %p, milliseconds %u.\n", event, milliseconds);

    if ((rc = pthread_mutex_lock(&impl->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return WAIT_FAILED;
    }

    if (impl->is_signaled || !milliseconds)
    {
        bool is_signaled = impl->is_signaled;
        impl->is_signaled = false;
        pthread_mutex_unlock(&impl->mutex);
        return is_signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }

    if (milliseconds == INFINITE)
    {
        do
        {
            if ((rc = pthread_cond_wait(&impl->cond, &impl->mutex)))
            {
                ERR("Failed to wait on condition variable, error %d.\n", rc);
                pthread_mutex_unlock(&impl->mutex);
                return WAIT_FAILED;
            }
        } while (!impl->is_signaled);

        impl->is_signaled = false;
        pthread_mutex_unlock(&impl->mutex);
        return WAIT_OBJECT_0;
    }

    pthread_mutex_unlock(&impl->mutex);
    FIXME("Timed wait not implemented yet.\n");
    return WAIT_FAILED;
}

bool vkd3d_signal_event(HANDLE event)
{
    struct vkd3d_event *impl = event;
    int rc;

    TRACE("event %p.\n", event);

    if ((rc = pthread_mutex_lock(&impl->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return false;
    }
    impl->is_signaled = true;
    pthread_cond_signal(&impl->cond);
    pthread_mutex_unlock(&impl->mutex);

    return true;
}

void vkd3d_destroy_event(HANDLE event)
{
    struct vkd3d_event *impl = event;
    int rc;

    TRACE("event %p.\n", event);

    if ((rc = pthread_mutex_destroy(&impl->mutex)))
        ERR("Failed to destroy mutex, error %d.\n", rc);
    if ((rc = pthread_cond_destroy(&impl->cond)))
        ERR("Failed to destroy condition variable, error %d.\n", rc);
    vkd3d_free(impl);
}
