#include "config.h"

#include <gst/gst.h>

#include "gst_private.h"

#include <stdarg.h>

#define COBJMACROS
#define NONAMELESSUNION

#include "mfapi.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

/* source */

struct mpeg4_source
{
    IMFMediaSource IMFMediaSource_iface;
    LONG ref;
    IMFMediaEventQueue *event_queue;
    IMFPresentationDescriptor *pres_desc;
};

static inline struct mpeg4_source *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg4_source, IMFMediaSource_iface);
}

static HRESULT WINAPI mpeg4_source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaSource) ||
        IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *out = &This->IMFMediaSource_iface;
    }
    else
    {
        FIXME("(%s, %p)\n", debugstr_guid(riid), out);
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*out);
    return S_OK;
}

static ULONG WINAPI mpeg4_source_AddRef(IMFMediaSource *iface)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%u\n", This, ref);

    return ref;
}

static ULONG WINAPI mpeg4_source_Release(IMFMediaSource *iface)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%u\n", This, ref);

    if (!ref)
    {
        IMFMediaEventQueue_Release(This->event_queue);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI mpeg4_source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%#x, %p)\n", This, flags, event);

    return IMFMediaEventQueue_GetEvent(This->event_queue, flags, event);
}

static HRESULT WINAPI mpeg4_source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%p, %p)\n", This, callback, state);

    return IMFMediaEventQueue_BeginGetEvent(This->event_queue, callback, state);
}

static HRESULT WINAPI mpeg4_source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%p, %p)\n", This, result, event);

    return IMFMediaEventQueue_EndGetEvent(This->event_queue, result, event);
}

static HRESULT WINAPI mpeg4_source_QueueEvent(IMFMediaSource *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%d, %s, %#x, %p)\n", This, event_type, debugstr_guid(ext_type), hr, value);

    return IMFMediaEventQueue_QueueEventParamVar(This->event_queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI mpeg4_source_GetCharacteristics(IMFMediaSource *iface, DWORD *characteristics)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    FIXME("(%p)->(%p): stub\n", This, characteristics);

    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg4_source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **descriptor)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    FIXME("(%p)->(%p): stub\n", This, descriptor);

    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg4_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor,
                                     const GUID *time_format, const PROPVARIANT *start_position)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    FIXME("(%p)->(%p, %p, %p): stub\n", This, descriptor, time_format, start_position);

    return E_NOTIMPL;
}


static HRESULT WINAPI mpeg4_source_Stop(IMFMediaSource *iface)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    FIXME("(%p): stub\n", This);

    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg4_source_Pause(IMFMediaSource *iface)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    FIXME("(%p): stub\n", This);

    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg4_source_Shutdown(IMFMediaSource *iface)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);

    FIXME("(%p): stub\n", This);

    return S_OK;
}

static const IMFMediaSourceVtbl mpeg4_source_vtbl =
{
    mpeg4_source_QueryInterface,
    mpeg4_source_AddRef,
    mpeg4_source_Release,
    mpeg4_source_GetEvent,
    mpeg4_source_BeginGetEvent,
    mpeg4_source_EndGetEvent,
    mpeg4_source_QueueEvent,
    mpeg4_source_GetCharacteristics,
    mpeg4_source_CreatePresentationDescriptor,
    mpeg4_source_Start,
    mpeg4_source_Stop,
    mpeg4_source_Pause,
    mpeg4_source_Shutdown,
};

static HRESULT mpeg4_source_constructor(struct mpeg4_source **out_mpeg4_source)
{
    HRESULT hr;
    struct mpeg4_source *This = heap_alloc_zero(sizeof(*This));

    if (!This)
        return E_OUTOFMEMORY;

    if (FAILED(hr = MFCreateEventQueue(&This->event_queue)))
        goto fail;

    This->IMFMediaSource_iface.lpVtbl = &mpeg4_source_vtbl;
    This->ref = 1;

    *out_mpeg4_source = This;
    return S_OK;

    fail:
    if (This->event_queue)
    {
        heap_free(This->event_queue);
    }
    heap_free(This);
    return hr;
}

/* IMFByteStreamHandler */

struct mpeg4_stream_handler
{
    IMFByteStreamHandler IMFByteStreamHandler_iface;
    LONG refcount;
    struct handler handler;
};

static struct mpeg4_stream_handler *impl_from_IMFByteStreamHandler(IMFByteStreamHandler *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg4_stream_handler, IMFByteStreamHandler_iface);
}

static HRESULT WINAPI mpeg_stream_handler_QueryInterface(IMFByteStreamHandler *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFByteStreamHandler) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFByteStreamHandler_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI mpeg_stream_handler_AddRef(IMFByteStreamHandler *iface)
{
    struct mpeg4_stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedIncrement(&handler->refcount);

    TRACE("%p, refcount %u.\n", handler, refcount);

    return refcount;
}

static ULONG WINAPI mpeg_stream_handler_Release(IMFByteStreamHandler *iface)
{
    struct mpeg4_stream_handler *this = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedDecrement(&this->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        handler_destruct(&this->handler);
    }

    return refcount;
}

static HRESULT WINAPI mpeg_stream_handler_BeginCreateObject(IMFByteStreamHandler *iface, IMFByteStream *stream, const WCHAR *url, DWORD flags,
        IPropertyStore *props, IUnknown **cancel_cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct mpeg4_stream_handler *this = impl_from_IMFByteStreamHandler(iface);

    TRACE("%p, %s, %#x, %p, %p, %p, %p.\n", iface, debugstr_w(url), flags, props, cancel_cookie, callback, state);
    return handler_begin_create_object(&this->handler, stream, url, flags, props, cancel_cookie, callback, state);
}

static HRESULT WINAPI mpeg_stream_handler_EndCreateObject(IMFByteStreamHandler *iface, IMFAsyncResult *result,
        MF_OBJECT_TYPE *obj_type, IUnknown **object)
{
    struct mpeg4_stream_handler *this = impl_from_IMFByteStreamHandler(iface);

    TRACE("%p, %p, %p, %p.\n", iface, result, obj_type, object);
    return handler_end_create_object(&this->handler, result, obj_type, object);
}

static HRESULT WINAPI mpeg_stream_handler_CancelObjectCreation(IMFByteStreamHandler *iface, IUnknown *cancel_cookie)
{
    struct mpeg4_stream_handler *this = impl_from_IMFByteStreamHandler(iface);

    TRACE("%p, %p.\n", iface, cancel_cookie);
    return handler_cancel_object_creation(&this->handler, cancel_cookie);
}

static HRESULT WINAPI mpeg_stream_handler_GetMaxNumberOfBytesRequiredForResolution(IMFByteStreamHandler *iface, QWORD *bytes)
{
    FIXME("stub (%p %p)\n", iface, bytes);
    return E_NOTIMPL;
}

static const IMFByteStreamHandlerVtbl mpeg4_stream_handler_vtbl =
{
    mpeg_stream_handler_QueryInterface,
    mpeg_stream_handler_AddRef,
    mpeg_stream_handler_Release,
    mpeg_stream_handler_BeginCreateObject,
    mpeg_stream_handler_EndCreateObject,
    mpeg_stream_handler_CancelObjectCreation,
    mpeg_stream_handler_GetMaxNumberOfBytesRequiredForResolution,
};

static HRESULT mpeg4_stream_handler_create_object(struct handler *handler, WCHAR *url, IMFByteStream *stream, DWORD flags,
                                            IPropertyStore *props, IUnknown **out_object, MF_OBJECT_TYPE *out_obj_type)
{
    TRACE("(%p %s %p %u %p %p %p)\n", handler, debugstr_w(url), stream, flags, props, out_object, out_obj_type);

    if (flags & MF_RESOLUTION_MEDIASOURCE)
    {
        HRESULT hr;
        struct mpeg4_source *new_source;

        if (FAILED(hr = mpeg4_source_constructor(&new_source)))
            return hr;

        *out_object = (IUnknown*)&new_source->IMFMediaSource_iface;
        *out_obj_type = MF_OBJECT_MEDIASOURCE;

        return S_OK;
    }
    else
    {
        FIXME("flags = %08x\n", flags);
        return E_NOTIMPL;
    }
}

HRESULT mpeg4_stream_handler_construct(REFIID riid, void **obj)
{
    struct mpeg4_stream_handler *this;
    HRESULT hr;

    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    this = heap_alloc_zero(sizeof(*this));
    if (!this)
        return E_OUTOFMEMORY;

    handler_construct(&this->handler, mpeg4_stream_handler_create_object);

    this->IMFByteStreamHandler_iface.lpVtbl = &mpeg4_stream_handler_vtbl;
    this->refcount = 1;

    hr = IMFByteStreamHandler_QueryInterface(&this->IMFByteStreamHandler_iface, riid, obj);
    IMFByteStreamHandler_Release(&this->IMFByteStreamHandler_iface);

    return hr;
}