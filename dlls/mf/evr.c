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

#include "mf_private.h"
#include "mferror.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

struct video_renderer
{
    IMFMediaSink IMFMediaSink_iface;
    LONG refcount;
    BOOL is_shut_down;
    CRITICAL_SECTION cs;
    HWND target_hwnd;
};

static struct video_renderer *impl_from_IMFMediaSink(IMFMediaSink *iface)
{
    return CONTAINING_RECORD(iface, struct video_renderer, IMFMediaSink_iface);
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
    FIXME("%p, %p stub!\n", iface, flags);

    return E_NOTIMPL;
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

    *count = 1

    return S_OK;
}

static HRESULT WINAPI video_renderer_sink_GetStreamSinkByIndex(IMFMediaSink *iface, DWORD index,
        IMFStreamSink **stream)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    FIXME("%p, %u, %p stub!\n", iface, index, stream);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_renderer_sink_GetStreamSinkById(IMFMediaSink *iface, DWORD stream_sink_id,
        IMFStreamSink **stream)
{
    struct video_renderer *renderer = impl_from_IMFMediaSink(iface);
    FIXME("%p, %#x, %p stub!\n", iface, stream_sink_id, stream);

    return E_NOTIMPL;
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

static HRESULT evr_create_object(IMFAttributes *attributes, void *user_context, IUnknown **obj)
{
    struct video_renderer *renderer;

    TRACE("%p, %p, %p\n", attributes, user_context, obj);

    if (!(renderer = heap_alloc_zero(sizeof(*renderer))))
        return E_OUTOFMEMORY;

    renderer->IMFMediaSink_iface.lpVtbl = &video_renderer_sink_vtbl;
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

