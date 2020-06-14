/*
 * Copyright 2020 Victor Hermann Chiletto
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

#define COBJMACROS

#include "mfapi.h"
#include "mf_private.h"
#include "mferror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

struct video_renderer
{
    IMFMediaSink IMFMediaSink_iface;
    IMFStreamSink IMFStreamSink_iface;
    IMFMediaTypeHandler IMFMediaTypeHandler_iface;
    LONG refcount;
    IMFMediaType *media_type;
    BOOL is_shut_down;
    CRITICAL_SECTION cs;
    HWND target_hwnd;
};

static struct video_renderer *impl_from_IMFMediaSink(IMFMediaSink *iface)
{
    return CONTAINING_RECORD(iface, struct video_renderer, IMFMediaSink_iface);
}

static struct video_renderer *impl_from_IMFStreamSink(IMFStreamSink *iface)
{
    return CONTAINING_RECORD(iface, struct video_renderer, IMFStreamSink_iface);
}

static struct video_renderer *impl_from_IMFMediaTypeHandler(IMFMediaTypeHandler *iface)
{
    return CONTAINING_RECORD(iface, struct video_renderer, IMFMediaTypeHandler_iface);
}

static HRESULT WINAPI video_renderer_sink_QueryInterface(IMFMediaSink *iface, REFIID riid, void **obj)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFMediaSink) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
    }
    else
    {
        WARN("Unknown iface %s.\n", debugstr_guid(riid));
        *obj = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*obj);

    return S_OK;
}

static ULONG WINAPI video_renderer_sink_AddRef(IMFMediaSink *iface)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    ULONG refcount = InterlockedIncrement(&renderer->refcount);
    TRACE("%p, refcount %u.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI video_renderer_sink_Release(IMFMediaSink *iface)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    ULONG refcount = InterlockedDecrement(&renderer->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        DeleteCriticalSection(&renderer->cs);
        heap_free(renderer);
    }

    return refcount;
}

static HRESULT WINAPI video_renderer_sink_GetCharacteristics(IMFMediaSink *iface, DWORD *flags)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    TRACE("%p, %p.\n", iface, flags);

    if (renderer->is_shut_down)
    {
        return MF_E_SHUTDOWN;
    }

    *flags = MEDIASINK_FIXED_STREAMS;

    return S_OK;
}

static HRESULT WINAPI video_renderer_sink_AddStreamSink(IMFMediaSink *iface, DWORD stream_sink_id,
        IMFMediaType *media_type, IMFStreamSink **stream_sink)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    TRACE("%p, %#x, %p, %p.\n", iface, stream_sink_id, media_type, stream_sink);

    return renderer->is_shut_down ? MF_E_SHUTDOWN : MF_E_STREAMSINKS_FIXED;
}

static HRESULT WINAPI video_renderer_sink_RemoveStreamSink(IMFMediaSink *iface, DWORD stream_sink_id)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    TRACE("%p, %#x.\n", iface, stream_sink_id);

    return renderer->is_shut_down ? MF_E_SHUTDOWN : MF_E_STREAMSINKS_FIXED;
}

static HRESULT WINAPI video_renderer_sink_GetStreamSinkCount(IMFMediaSink *iface, DWORD *count)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    TRACE("%p, %p.\n", iface, count);

    if (!count)
    {
        return E_POINTER;
    }

    if (renderer->is_shut_down)
    {
        return MF_E_SHUTDOWN;
    }

    *count = 1;

    return S_OK;
}

static HRESULT WINAPI video_renderer_sink_GetStreamSinkByIndex(IMFMediaSink *iface, DWORD index,
        IMFStreamSink **stream)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %u, %p.\n", iface, index, stream);

    EnterCriticalSection(&renderer->cs);

    if (renderer->is_shut_down)
    {
        hr = MF_E_SHUTDOWN;
    }
    else if (index > 0)
    {
        hr = MF_E_INVALIDINDEX;
    }
    else
    {
        *stream = &renderer->IMFStreamSink_iface;
        IMFStreamSink_AddRef(*stream);
    }

    LeaveCriticalSection(&renderer->cs);

    return hr;
}

static HRESULT WINAPI video_renderer_sink_GetStreamSinkById(IMFMediaSink *iface, DWORD stream_sink_id,
        IMFStreamSink **stream)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %u, %p.\n", iface, stream_sink_id, stream);

    EnterCriticalSection(&renderer->cs);

    if (renderer->is_shut_down)
    {
        hr = MF_E_SHUTDOWN;
    }
    else if (stream_sink_id > 0)
    {
        hr = MF_E_INVALIDSTREAMNUMBER;
    }
    else
    {
        *stream = &renderer->IMFStreamSink_iface;
        IMFStreamSink_AddRef(*stream);
    }

    LeaveCriticalSection(&renderer->cs);

    return hr;
}

static HRESULT WINAPI video_renderer_sink_SetPresentationClock(IMFMediaSink *iface, IMFPresentationClock *clock)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    FIXME("%p, %p stub!\n", iface, clock);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_sink_GetPresentationClock(IMFMediaSink *iface, IMFPresentationClock **clock)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    FIXME("%p, %p stub!\n", iface, clock);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_sink_Shutdown(IMFMediaSink *iface)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    TRACE("%p.\n", iface);

    if (renderer->is_shut_down)
    {
        return MF_E_SHUTDOWN;
    }

    EnterCriticalSection(&renderer->cs);
    renderer->is_shut_down = TRUE;
    LeaveCriticalSection(&renderer->cs);

    return S_OK;
}

static const IMFMediaSinkVtbl video_renderer_sink_vtbl =
{
    video_renderer_sink_QueryInterface,
    video_renderer_sink_AddRef,
    video_renderer_sink_Release,
    video_renderer_sink_GetCharacteristics,
    video_renderer_sink_AddStreamSink,
    video_renderer_sink_RemoveStreamSink,
    video_renderer_sink_GetStreamSinkCount,
    video_renderer_sink_GetStreamSinkByIndex,
    video_renderer_sink_GetStreamSinkById,
    video_renderer_sink_SetPresentationClock,
    video_renderer_sink_GetPresentationClock,
    video_renderer_sink_Shutdown,
};

static HRESULT WINAPI video_renderer_stream_QueryInterface(IMFStreamSink *iface, REFIID riid, void **obj)
{
    struct video_renderer *renderer = impl_from_IMFStreamSink(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IMFStreamSink) ||
            IsEqualGUID(riid, &IID_IUnknown))
    {
        *obj = &renderer->IMFStreamSink_iface;
    }
    else if (IsEqualGUID(riid, &IID_IMFMediaTypeHandler))
    {
        *obj = &renderer->IMFMediaTypeHandler_iface;
    }
    else
    {
        WARN("Unknown iface %s.\n", debugstr_guid(riid));
        *obj = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*obj);

    return S_OK;
}

static ULONG WINAPI video_renderer_stream_AddRef(IMFStreamSink *iface)
{
    struct video_renderer *renderer = impl_from_IMFStreamSink(iface);
    return IMFMediaSink_AddRef(&renderer->IMFMediaSink_iface);
}

static ULONG WINAPI video_renderer_stream_Release(IMFStreamSink *iface)
{
    struct video_renderer *renderer = impl_from_IMFStreamSink(iface);
    return IMFMediaSink_Release(&renderer->IMFMediaSink_iface);
}

static HRESULT WINAPI video_renderer_stream_GetEvent(IMFStreamSink *iface, DWORD flags, IMFMediaEvent **event)
{
    FIXME("%p, %#x, %p stub!\n", iface, flags, event);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_stream_BeginGetEvent(IMFStreamSink *iface, IMFAsyncCallback *callback,
        IUnknown *state)
{
    FIXME("%p, %p, %p stub!\n", iface, callback, state);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_stream_EndGetEvent(IMFStreamSink *iface, IMFAsyncResult *result,
        IMFMediaEvent **event)
{
    FIXME("%p, %p, %p stub!\n", iface, result, event);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_stream_QueueEvent(IMFStreamSink *iface, MediaEventType event_type,
        REFGUID ext_type, HRESULT hr, const PROPVARIANT *value)
{
    FIXME("%p, %u, %s, %#x, %p stub!\n", iface, event_type, debugstr_guid(ext_type), hr, value);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_stream_GetMediaSink(IMFStreamSink *iface, IMFMediaSink **sink)
{
    struct video_renderer *renderer = impl_from_IMFStreamSink(iface);

    TRACE("%p, %p.\n", iface, sink);

    if (renderer->is_shut_down)
    {
        /* could also return MF_E_SHUTDOWN here */
        return MF_E_STREAMSINK_REMOVED;
    }

    *sink = &renderer->IMFMediaSink_iface;
    IMFMediaSink_AddRef(*sink);

    return S_OK;
}

static HRESULT WINAPI video_renderer_stream_GetIdentifier(IMFStreamSink *iface, DWORD *identifier)
{
    struct video_renderer *renderer = impl_from_IMFStreamSink(iface);

    TRACE("%p, %p.\n", iface, identifier);

    if (renderer->is_shut_down)
    {
        /* could also return MF_E_SHUTDOWN here */
        return MF_E_STREAMSINK_REMOVED;
    }

    *identifier = 0;

    return S_OK;
}

static HRESULT WINAPI video_renderer_stream_GetMediaTypeHandler(IMFStreamSink *iface, IMFMediaTypeHandler **handler)
{
    struct video_renderer *renderer = impl_from_IMFStreamSink(iface);
    TRACE("%p, %p.\n", iface, handler);

    if (!handler)
    {
        return E_POINTER;
    }

    if (renderer->is_shut_down)
    {
        return MF_E_STREAMSINK_REMOVED;
    }

    *handler = &renderer->IMFMediaTypeHandler_iface;
    IMFMediaTypeHandler_AddRef(*handler);

    return S_OK;
}

static HRESULT WINAPI video_renderer_stream_ProcessSample(IMFStreamSink *iface, IMFSample *sample)
{
    FIXME("%p, %p stub!\n", iface, sample);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_stream_PlaceMarker(IMFStreamSink *iface, MFSTREAMSINK_MARKER_TYPE marker_type,
        const PROPVARIANT *marker_value, const PROPVARIANT *context_value)
{
    FIXME("%p, %d, %p, %p stub!\n", iface, marker_type, marker_value, context_value);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_stream_Flush(IMFStreamSink *iface)
{
    FIXME("%p stub!\n", iface);

    return E_NOTIMPL;
}

static const IMFStreamSinkVtbl video_renderer_stream_vtbl =
{
    video_renderer_stream_QueryInterface,
    video_renderer_stream_AddRef,
    video_renderer_stream_Release,
    video_renderer_stream_GetEvent,
    video_renderer_stream_BeginGetEvent,
    video_renderer_stream_EndGetEvent,
    video_renderer_stream_QueueEvent,
    video_renderer_stream_GetMediaSink,
    video_renderer_stream_GetIdentifier,
    video_renderer_stream_GetMediaTypeHandler,
    video_renderer_stream_ProcessSample,
    video_renderer_stream_PlaceMarker,
    video_renderer_stream_Flush,
};

static HRESULT WINAPI video_renderer_stream_type_handler_QueryInterface(IMFMediaTypeHandler *iface, REFIID riid, void **obj)
{
    struct video_renderer *renderer = impl_from_IMFMediaTypeHandler(iface);

    return IMFStreamSink_QueryInterface(&renderer->IMFStreamSink_iface, riid, obj);
}

static ULONG WINAPI video_renderer_stream_type_handler_AddRef(IMFMediaTypeHandler *iface)
{
    struct video_renderer *renderer = impl_from_IMFMediaTypeHandler(iface);

    return IMFStreamSink_AddRef(&renderer->IMFStreamSink_iface);
}

static ULONG WINAPI video_renderer_stream_type_handler_Release(IMFMediaTypeHandler *iface)
{
    struct video_renderer *renderer = impl_from_IMFMediaTypeHandler(iface);

    return IMFStreamSink_Release(&renderer->IMFStreamSink_iface);
}

static HRESULT WINAPI video_renderer_stream_type_handler_IsMediaTypeSuported(IMFMediaTypeHandler *iface,
        IMFMediaType *in_type, IMFMediaType **out_type)
{
    struct video_renderer *renderer = impl_from_IMFMediaTypeHandler(iface);
    DWORD flags;
    HRESULT hr;

    TRACE("%p, %p, %p.\n", iface, in_type, out_type);

    if (out_type)
    {
        *out_type = NULL;
    }

    if (!in_type)
    {
        return E_POINTER;
    }

    EnterCriticalSection(&renderer->cs);

    hr = renderer->media_type &&
            IMFMediaType_IsEqual(renderer->media_type, in_type, &flags) == S_OK ?
            S_OK : MF_E_INVALIDREQUEST;

    LeaveCriticalSection(&renderer->cs);

    return hr;
}

static HRESULT WINAPI video_renderer_stream_type_handler_GetMediaTypeCount(IMFMediaTypeHandler *iface,
        DWORD *count)
{
    TRACE("%p, %p.\n", iface, count);

    if (!count)
    {
        return E_POINTER;
    }

    *count = 1;

    return S_OK;
}

static HRESULT WINAPI video_renderer_stream_type_handler_GetMediaTypeByIndex(IMFMediaTypeHandler *iface,
        DWORD index, IMFMediaType **type)
{
    struct video_renderer *renderer = impl_from_IMFMediaTypeHandler(iface);

    TRACE("%p, %d.\n", iface, index);

    if (index > 0)
    {
        return MF_E_NO_MORE_TYPES;
    }

    *type = renderer->media_type;
    IMFMediaType_AddRef(*type);

    return S_OK;
}

static HRESULT WINAPI video_renderer_stream_type_handler_SetCurrentMediaType(IMFMediaTypeHandler *iface,
        IMFMediaType *type)
{
    FIXME("%p, %p stub!\n", iface, type);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_stream_type_handler_GetCurrentMediaType(IMFMediaTypeHandler *iface,
        IMFMediaType **type)
{
    struct video_renderer *renderer = impl_from_IMFMediaTypeHandler(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, type);

    if (!type)
    {
        return E_POINTER;
    }

    EnterCriticalSection(&renderer->cs);

    if (renderer->media_type)
    {
        *type = renderer->media_type;
        IMFMediaType_AddRef(*type);
    }
    else
    {
        hr = MF_E_NOT_INITIALIZED;
    }

    LeaveCriticalSection(&renderer->cs);

    return hr;
}

static HRESULT WINAPI video_renderer_stream_type_handler_GetMajorType(IMFMediaTypeHandler *iface, GUID *type)
{
    TRACE("%p, %p.\n", iface, type);

    if (!type)
    {
        return E_POINTER;
    }

    memcpy(type, &MFMediaType_Video, sizeof(*type));

    return S_OK;
}

static const IMFMediaTypeHandlerVtbl video_renderer_stream_type_handler_vtbl =
{
    video_renderer_stream_type_handler_QueryInterface,
    video_renderer_stream_type_handler_AddRef,
    video_renderer_stream_type_handler_Release,
    video_renderer_stream_type_handler_IsMediaTypeSuported,
    video_renderer_stream_type_handler_GetMediaTypeCount,
    video_renderer_stream_type_handler_GetMediaTypeByIndex,
    video_renderer_stream_type_handler_SetCurrentMediaType,
    video_renderer_stream_type_handler_GetCurrentMediaType,
    video_renderer_stream_type_handler_GetMajorType,
};

static HRESULT evr_create_object(IMFAttributes *attributes, void *user_context, IUnknown **obj)
{
    struct video_renderer *renderer;

    TRACE("%p, %p, %p\n", attributes, user_context, obj);

    if (!(renderer = heap_alloc_zero(sizeof(*renderer))))
        return E_OUTOFMEMORY;

    renderer->IMFMediaSink_iface.lpVtbl = &video_renderer_sink_vtbl;
    renderer->IMFStreamSink_iface.lpVtbl = &video_renderer_stream_vtbl;
    renderer->IMFMediaTypeHandler_iface.lpVtbl = &video_renderer_stream_type_handler_vtbl;
    renderer->target_hwnd = user_context;
    renderer->refcount = 1;
    renderer->is_shut_down = FALSE;
    InitializeCriticalSection(&renderer->cs);

    *obj = (IUnknown *) &renderer->IMFMediaSink_iface;

    return S_OK;
}

static void evr_shutdown_object(void *user_context, IUnknown *obj)
{
    IMFMediaSink *sink;

    if (SUCCEEDED(IUnknown_QueryInterface(obj, &IID_IMFMediaSink, (void **)&sink)))
    {
        IMFMediaSink_Shutdown(sink);
        IMFMediaSink_Release(sink);
    }
}

static void evr_free_private(void *user_context)
{
}

static const struct activate_funcs evr_activate_funcs =
{
    evr_create_object,
    evr_shutdown_object,
    evr_free_private,
};

HRESULT WINAPI MFCreateVideoRendererActivate(HWND hwnd, IMFActivate **activate)
{
    TRACE("%p, %p.\n", hwnd, activate);

    if (!activate)
        return E_POINTER;

    return create_activation_object(hwnd, &evr_activate_funcs, activate);
}

