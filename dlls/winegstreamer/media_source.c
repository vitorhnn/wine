#include "config.h"

#include <gst/gst.h>

#include "gst_private.h"
#include "gst_cbs.h"

#include <stdarg.h>

#define COBJMACROS
#define NONAMELESSUNION

#include "mfapi.h"
#include "mferror.h"
#include "mfidl.h"
#include "mfobjects.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

static struct source_desc
{
    GstStaticCaps bytestream_caps;
} source_descs[] =
{
    {/*SOURCE_TYPE_MPEG_4*/
        GST_STATIC_CAPS("video/quicktime"),
    }
};

struct media_source
{
    IMFMediaSource IMFMediaSource_iface;
    LONG ref;
    enum source_type type;
    IMFMediaEventQueue *event_queue;
    IMFByteStream *byte_stream;
    GstPad *my_src;
    enum
    {
        SOURCE_OPENING,
        SOURCE_STOPPED, /* (READY) */
        SOURCE_PAUSED,
        SOURCE_RUNNING,
        SOURCE_SHUTDOWN,
    } state;
};

static inline struct media_source *impl_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFMediaSource_iface);
}

GstFlowReturn pull_from_bytestream(GstPad *pad, GstObject *parent, guint64 ofs, guint len,
        GstBuffer **buf)
{
    struct media_source *source = gst_pad_get_element_private(pad);
    IMFByteStream *byte_stream = source->byte_stream;
    ULONG bytes_read;
    GstMapInfo info;
    BOOL is_eof;
    HRESULT hr;

    TRACE("gstreamer requesting %u bytes at %s from source %p into buffer %p\n", len, wine_dbgstr_longlong(ofs), source, buf);

    if (ofs != GST_BUFFER_OFFSET_NONE)
    {
        if (FAILED(IMFByteStream_SetCurrentPosition(byte_stream, ofs)))
            return GST_FLOW_ERROR;
    }

    if (FAILED(IMFByteStream_IsEndOfStream(byte_stream, &is_eof)))
        return GST_FLOW_ERROR;
    if (is_eof)
        return GST_FLOW_EOS;

    if (!(*buf))
        *buf = gst_buffer_new_and_alloc(len);
    gst_buffer_map(*buf, &info, GST_MAP_WRITE);
    hr = IMFByteStream_Read(byte_stream, info.data, len, &bytes_read);
    gst_buffer_unmap(*buf, &info);

    gst_buffer_set_size(*buf, bytes_read);

    if (FAILED(hr))
    {
        return GST_FLOW_ERROR;
    }
    GST_BUFFER_OFFSET(*buf) = ofs;
    return GST_FLOW_OK;
}

static gboolean query_bytestream(GstPad *pad, GstObject *parent, GstQuery *query)
{
    struct media_source *source = gst_pad_get_element_private(pad);
    GstFormat format;
    QWORD bytestream_len;

    TRACE("GStreamer queries source %p for %s\n", source, GST_QUERY_TYPE_NAME(query));

    if (FAILED(IMFByteStream_GetLength(source->byte_stream, &bytestream_len)))
        return FALSE;

    switch (GST_QUERY_TYPE(query))
    {
        case GST_QUERY_DURATION:
        {
            gst_query_parse_duration (query, &format, NULL);
            if (format == GST_FORMAT_PERCENT) {
                gst_query_set_duration (query, GST_FORMAT_PERCENT, GST_FORMAT_PERCENT_MAX);
                return TRUE;
            }
            else if (format == GST_FORMAT_BYTES)
            {
                QWORD length;
                IMFByteStream_GetLength(source->byte_stream, &length);
                gst_query_set_duration (query, GST_FORMAT_BYTES, length);
                return TRUE;
            }
            return FALSE;
        }
        case GST_QUERY_SEEKING:
        {
            gst_query_parse_seeking (query, &format, NULL, NULL, NULL);
            if (format != GST_FORMAT_BYTES)
            {
                WARN("Cannot seek using format \"%s\".\n", gst_format_get_name(format));
                return FALSE;
            }
            gst_query_set_seeking(query, GST_FORMAT_BYTES, 1, 0, bytestream_len);
            return TRUE;
        }
        case GST_QUERY_SCHEDULING:
        {
            gst_query_set_scheduling(query, GST_SCHEDULING_FLAG_SEEKABLE, 1, -1, 0);
            gst_query_add_scheduling_mode(query, GST_PAD_MODE_PULL);
            return TRUE;
        }
        case GST_QUERY_CAPS:
        {
            GstCaps *caps, *filter;

            gst_query_parse_caps(query, &filter);

            caps = gst_static_caps_get(&source_descs[source->type].bytestream_caps);

            if (filter) {
                GstCaps* filtered;
                filtered = gst_caps_intersect_full(
                        filter, caps, GST_CAPS_INTERSECT_FIRST);
                gst_caps_unref(caps);
                caps = filtered;
            }
            gst_query_set_caps_result(query, caps);
            gst_caps_unref(caps);
            return TRUE;
        }
        default:
        {
            WARN("Unhandled query type %s\n", GST_QUERY_TYPE_NAME(query));
            return FALSE;
        }
    }
}

static gboolean activate_bytestream_pad_mode(GstPad *pad, GstObject *parent, GstPadMode mode, gboolean activate)
{
    struct media_source *source = gst_pad_get_element_private(pad);

    TRACE("%s source pad for mediasource %p in %s mode.\n",
            activate ? "Activating" : "Deactivating", source, gst_pad_mode_get_name(mode));

    switch (mode) {
      case GST_PAD_MODE_PULL:
        return TRUE;
      default:
        return FALSE;
    }
    return FALSE;
}

static gboolean process_bytestream_pad_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
    struct media_source *source = gst_pad_get_element_private(pad);

    TRACE("source %p, type \"%s\".\n", source, GST_EVENT_TYPE_NAME(event));

    switch (event->type) {
        /* the seek event should fail in pull mode */
        case GST_EVENT_SEEK:
            return FALSE;
        default:
            WARN("Ignoring \"%s\" event.\n", GST_EVENT_TYPE_NAME(event));
        case GST_EVENT_TAG:
        case GST_EVENT_QOS:
        case GST_EVENT_RECONFIGURE:
            return gst_pad_event_default(pad, parent, event);
    }
    return TRUE;
}

static HRESULT WINAPI media_source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%s %p)\n", source, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaSource) ||
        IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *out = &source->IMFMediaSource_iface;
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

static ULONG WINAPI media_source_AddRef(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedIncrement(&source->ref);

    TRACE("(%p) ref=%u\n", source, ref);

    return ref;
}

static ULONG WINAPI media_source_Release(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);
    ULONG ref = InterlockedDecrement(&source->ref);

    TRACE("(%p) ref=%u\n", source, ref);

    if (!ref)
    {
        heap_free(source);
    }

    return ref;
}

static HRESULT WINAPI media_source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%#x, %p)\n", source, flags, event);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_GetEvent(source->event_queue, flags, event);
}

static HRESULT WINAPI media_source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%p, %p)\n", source, callback, state);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_BeginGetEvent(source->event_queue, callback, state);
}

static HRESULT WINAPI media_source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%p, %p)\n", source, result, event);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_EndGetEvent(source->event_queue, result, event);
}

static HRESULT WINAPI media_source_QueueEvent(IMFMediaSource *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)->(%d, %s, %#x, %p)\n", source, event_type, debugstr_guid(ext_type), hr, value);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return IMFMediaEventQueue_QueueEventParamVar(source->event_queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI media_source_GetCharacteristics(IMFMediaSource *iface, DWORD *characteristics)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p)->(%p): stub\n", source, characteristics);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **descriptor)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p)->(%p): stub\n", source, descriptor);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor,
                                     const GUID *time_format, const PROPVARIANT *start_position)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p)->(%p, %p, %p): stub\n", source, descriptor, time_format, start_position);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_Stop(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p): stub\n", source);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_Pause(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    FIXME("(%p): stub\n", source);

    if (source->state == SOURCE_SHUTDOWN)
        return MF_E_SHUTDOWN;

    return E_NOTIMPL;
}

static HRESULT media_source_teardown(struct media_source *source)
{
    if (source->my_src)
        gst_object_unref(GST_OBJECT(source->my_src));
    if (source->event_queue)
        IMFMediaEventQueue_Release(source->event_queue);
    if (source->byte_stream)
        IMFByteStream_Release(source->byte_stream);

    return S_OK;
}

static HRESULT WINAPI media_source_Shutdown(IMFMediaSource *iface)
{
    struct media_source *source = impl_from_IMFMediaSource(iface);

    TRACE("(%p)\n", source);

    source->state = SOURCE_SHUTDOWN;
    return media_source_teardown(source);
}

static const IMFMediaSourceVtbl IMFMediaSource_vtbl =
{
    media_source_QueryInterface,
    media_source_AddRef,
    media_source_Release,
    media_source_GetEvent,
    media_source_BeginGetEvent,
    media_source_EndGetEvent,
    media_source_QueueEvent,
    media_source_GetCharacteristics,
    media_source_CreatePresentationDescriptor,
    media_source_Start,
    media_source_Stop,
    media_source_Pause,
    media_source_Shutdown,
};

static HRESULT media_source_constructor(IMFByteStream *bytestream, enum source_type type, struct media_source **out_media_source)
{
    GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
        "mf_src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        source_descs[type].bytestream_caps);

    struct media_source *object = heap_alloc_zero(sizeof(*object));
    HRESULT hr;

    if (!object)
        return E_OUTOFMEMORY;

    object->IMFMediaSource_iface.lpVtbl = &IMFMediaSource_vtbl;
    object->ref = 1;
    object->type = type;
    object->byte_stream = bytestream;
    IMFByteStream_AddRef(bytestream);

    if (FAILED(hr = MFCreateEventQueue(&object->event_queue)))
        goto fail;

    object->my_src = gst_pad_new_from_static_template(&src_template, "mf-src");
    gst_pad_set_element_private(object->my_src, object);
    gst_pad_set_getrange_function(object->my_src, pull_from_bytestream_wrapper);
    gst_pad_set_query_function(object->my_src, query_bytestream_wrapper);
    gst_pad_set_activatemode_function(object->my_src, activate_bytestream_pad_mode_wrapper);
    gst_pad_set_event_function(object->my_src, process_bytestream_pad_event_wrapper);

    object->state = SOURCE_STOPPED;

    *out_media_source = object;
    return S_OK;

    fail:
    WARN("Failed to construct MFMediaSource, hr %#x.\n", hr);

    media_source_teardown(object);
    heap_free(object);
    *out_media_source = NULL;
    return hr;
}

/* IMFByteStreamHandler */

struct container_stream_handler_result
{
    struct list entry;
    IMFAsyncResult *result;
    MF_OBJECT_TYPE obj_type;
    IUnknown *object;
};

struct container_stream_handler
{
    IMFByteStreamHandler IMFByteStreamHandler_iface;
    IMFAsyncCallback IMFAsyncCallback_iface;
    LONG refcount;
    enum source_type type;
    struct list results;
    CRITICAL_SECTION cs;
};

static struct container_stream_handler *impl_from_IMFByteStreamHandler(IMFByteStreamHandler *iface)
{
    return CONTAINING_RECORD(iface, struct container_stream_handler, IMFByteStreamHandler_iface);
}

static struct container_stream_handler *impl_from_IMFAsyncCallback(IMFAsyncCallback *iface)
{
    return CONTAINING_RECORD(iface, struct container_stream_handler, IMFAsyncCallback_iface);
}

static HRESULT WINAPI container_stream_handler_QueryInterface(IMFByteStreamHandler *iface, REFIID riid, void **obj)
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

static ULONG WINAPI container_stream_handler_AddRef(IMFByteStreamHandler *iface)
{
    struct container_stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedIncrement(&handler->refcount);

    TRACE("%p, refcount %u.\n", handler, refcount);

    return refcount;
}

static ULONG WINAPI container_stream_handler_Release(IMFByteStreamHandler *iface)
{
    struct container_stream_handler *handler = impl_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedDecrement(&handler->refcount);
    struct container_stream_handler_result *result, *next;

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        LIST_FOR_EACH_ENTRY_SAFE(result, next, &handler->results, struct container_stream_handler_result, entry)
        {
            list_remove(&result->entry);
            IMFAsyncResult_Release(result->result);
            if (result->object)
                IUnknown_Release(result->object);
            heap_free(result);
        }
        DeleteCriticalSection(&handler->cs);
        heap_free(handler);
    }

    return refcount;
}

struct create_object_context
{
    IUnknown IUnknown_iface;
    LONG refcount;

    IPropertyStore *props;
    IMFByteStream *stream;
    WCHAR *url;
    DWORD flags;
};

static struct create_object_context *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct create_object_context, IUnknown_iface);
}

static HRESULT WINAPI create_object_context_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI create_object_context_AddRef(IUnknown *iface)
{
    struct create_object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&context->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI create_object_context_Release(IUnknown *iface)
{
    struct create_object_context *context = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&context->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        if (context->props)
            IPropertyStore_Release(context->props);
        if (context->stream)
            IMFByteStream_Release(context->stream);
        if (context->url)
            heap_free(context->url);
        heap_free(context);
    }

    return refcount;
}

static const IUnknownVtbl create_object_context_vtbl =
{
    create_object_context_QueryInterface,
    create_object_context_AddRef,
    create_object_context_Release,
};

static WCHAR *heap_strdupW(const WCHAR *str)
{
    WCHAR *ret = NULL;

    if (str)
    {
        unsigned int size;

        size = (lstrlenW(str) + 1) * sizeof(WCHAR);
        ret = heap_alloc(size);
        if (ret)
            memcpy(ret, str, size);
    }

    return ret;
}

static HRESULT WINAPI container_stream_handler_BeginCreateObject(IMFByteStreamHandler *iface, IMFByteStream *stream, const WCHAR *url, DWORD flags,
        IPropertyStore *props, IUnknown **cancel_cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct container_stream_handler *this = impl_from_IMFByteStreamHandler(iface);
    struct create_object_context *context;
    IMFAsyncResult *caller, *item;
    HRESULT hr;

    TRACE("%p, %s, %#x, %p, %p, %p, %p.\n", iface, debugstr_w(url), flags, props, cancel_cookie, callback, state);

    if (cancel_cookie)
        *cancel_cookie = NULL;

    if (FAILED(hr = MFCreateAsyncResult(NULL, callback, state, &caller)))
        return hr;

    context = heap_alloc(sizeof(*context));
    if (!context)
    {
        IMFAsyncResult_Release(caller);
        return E_OUTOFMEMORY;
    }

    context->IUnknown_iface.lpVtbl = &create_object_context_vtbl;
    context->refcount = 1;
    context->props = props;
    if (context->props)
        IPropertyStore_AddRef(context->props);
    context->flags = flags;
    context->stream = stream;
    if (context->stream)
        IMFByteStream_AddRef(context->stream);
    if (url)
        context->url = heap_strdupW(url);
    if (!context->stream)
    {
        IMFAsyncResult_Release(caller);
        IUnknown_Release(&context->IUnknown_iface);
        return E_OUTOFMEMORY;
    }

    hr = MFCreateAsyncResult(&context->IUnknown_iface, &this->IMFAsyncCallback_iface, (IUnknown *)caller, &item);
    IUnknown_Release(&context->IUnknown_iface);
    if (SUCCEEDED(hr))
    {
        if (SUCCEEDED(hr = MFPutWorkItemEx(MFASYNC_CALLBACK_QUEUE_IO, item)))
        {
            if (cancel_cookie)
            {
                *cancel_cookie = (IUnknown *)caller;
                IUnknown_AddRef(*cancel_cookie);
            }
        }

        IMFAsyncResult_Release(item);
    }
    IMFAsyncResult_Release(caller);

    return hr;
}

static HRESULT WINAPI container_stream_handler_EndCreateObject(IMFByteStreamHandler *iface, IMFAsyncResult *result,
        MF_OBJECT_TYPE *obj_type, IUnknown **object)
{
    struct container_stream_handler *this = impl_from_IMFByteStreamHandler(iface);
    struct container_stream_handler_result *found = NULL, *cur;
    HRESULT hr;

    TRACE("%p, %p, %p, %p.\n", iface, result, obj_type, object);

    EnterCriticalSection(&this->cs);

    LIST_FOR_EACH_ENTRY(cur, &this->results, struct container_stream_handler_result, entry)
    {
        if (result == cur->result)
        {
            list_remove(&cur->entry);
            found = cur;
            break;
        }
    }

    LeaveCriticalSection(&this->cs);

    if (found)
    {
        *obj_type = found->obj_type;
        *object = found->object;
        hr = IMFAsyncResult_GetStatus(found->result);
        IMFAsyncResult_Release(found->result);
        heap_free(found);
    }
    else
    {
        *obj_type = MF_OBJECT_INVALID;
        *object = NULL;
        hr = MF_E_UNEXPECTED;
    }

    return hr;
}

static HRESULT WINAPI container_stream_handler_CancelObjectCreation(IMFByteStreamHandler *iface, IUnknown *cancel_cookie)
{
    struct container_stream_handler *this = impl_from_IMFByteStreamHandler(iface);
    struct container_stream_handler_result *found = NULL, *cur;

    TRACE("%p, %p.\n", iface, cancel_cookie);

    EnterCriticalSection(&this->cs);

    LIST_FOR_EACH_ENTRY(cur, &this->results, struct container_stream_handler_result, entry)
    {
        if (cancel_cookie == (IUnknown *)cur->result)
        {
            list_remove(&cur->entry);
            found = cur;
            break;
        }
    }

    LeaveCriticalSection(&this->cs);

    if (found)
    {
        IMFAsyncResult_Release(found->result);
        if (found->object)
            IUnknown_Release(found->object);
        heap_free(found);
    }

    return found ? S_OK : MF_E_UNEXPECTED;
}

static HRESULT WINAPI container_stream_handler_GetMaxNumberOfBytesRequiredForResolution(IMFByteStreamHandler *iface, QWORD *bytes)
{
    FIXME("stub (%p %p)\n", iface, bytes);
    return E_NOTIMPL;
}

static const IMFByteStreamHandlerVtbl container_stream_handler_vtbl =
{
    container_stream_handler_QueryInterface,
    container_stream_handler_AddRef,
    container_stream_handler_Release,
    container_stream_handler_BeginCreateObject,
    container_stream_handler_EndCreateObject,
    container_stream_handler_CancelObjectCreation,
    container_stream_handler_GetMaxNumberOfBytesRequiredForResolution,
};

static HRESULT WINAPI container_stream_handler_callback_QueryInterface(IMFAsyncCallback *iface, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFAsyncCallback) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFAsyncCallback_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI container_stream_handler_callback_AddRef(IMFAsyncCallback *iface)
{
    struct container_stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFByteStreamHandler_AddRef(&handler->IMFByteStreamHandler_iface);
}

static ULONG WINAPI container_stream_handler_callback_Release(IMFAsyncCallback *iface)
{
    struct container_stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    return IMFByteStreamHandler_Release(&handler->IMFByteStreamHandler_iface);
}

static HRESULT WINAPI container_stream_handler_callback_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue)
{
    return E_NOTIMPL;
}

static HRESULT container_stream_handler_create_object(struct container_stream_handler *This, WCHAR *url, IMFByteStream *stream, DWORD flags,
                                            IPropertyStore *props, IUnknown **out_object, MF_OBJECT_TYPE *out_obj_type)
{
    TRACE("(%p %s %p %u %p %p %p)\n", This, debugstr_w(url), stream, flags, props, out_object, out_obj_type);

    if (flags & MF_RESOLUTION_MEDIASOURCE)
    {
        HRESULT hr;
        struct media_source *new_source;

        if (FAILED(hr = media_source_constructor(stream, This->type, &new_source)))
            return hr;

        TRACE("->(%p)\n", new_source);

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

static HRESULT WINAPI container_stream_handler_callback_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)
{
    struct container_stream_handler *handler = impl_from_IMFAsyncCallback(iface);
    struct container_stream_handler_result *handler_result;
    MF_OBJECT_TYPE obj_type = MF_OBJECT_INVALID;
    IUnknown *object = NULL, *context_object;
    struct create_object_context *context;
    IMFAsyncResult *caller;
    HRESULT hr;

    caller = (IMFAsyncResult *)IMFAsyncResult_GetStateNoAddRef(result);

    if (FAILED(hr = IMFAsyncResult_GetObject(result, &context_object)))
    {
        WARN("Expected context set for callee result.\n");
        return hr;
    }

    context = impl_from_IUnknown(context_object);

    hr = container_stream_handler_create_object(handler, context->url, context->stream, context->flags, context->props, &object, &obj_type);

    handler_result = heap_alloc(sizeof(*handler_result));
    if (handler_result)
    {
        handler_result->result = caller;
        IMFAsyncResult_AddRef(handler_result->result);
        handler_result->obj_type = obj_type;
        handler_result->object = object;

        EnterCriticalSection(&handler->cs);
        list_add_tail(&handler->results, &handler_result->entry);
        LeaveCriticalSection(&handler->cs);
    }
    else
    {
        if (object)
            IUnknown_Release(object);
        hr = E_OUTOFMEMORY;
    }

    IUnknown_Release(&context->IUnknown_iface);

    IMFAsyncResult_SetStatus(caller, hr);
    MFInvokeCallback(caller);

    return S_OK;
}

static const IMFAsyncCallbackVtbl container_stream_handler_callback_vtbl =
{
    container_stream_handler_callback_QueryInterface,
    container_stream_handler_callback_AddRef,
    container_stream_handler_callback_Release,
    container_stream_handler_callback_GetParameters,
    container_stream_handler_callback_Invoke,
};

HRESULT container_stream_handler_construct(REFIID riid, void **obj, enum source_type type)
{
    struct container_stream_handler *this;
    HRESULT hr;

    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    this = heap_alloc_zero(sizeof(*this));
    if (!this)
        return E_OUTOFMEMORY;

    list_init(&this->results);
    InitializeCriticalSection(&this->cs);

    this->type = type;
    this->IMFByteStreamHandler_iface.lpVtbl = &container_stream_handler_vtbl;
    this->IMFAsyncCallback_iface.lpVtbl = &container_stream_handler_callback_vtbl;
    this->refcount = 1;

    hr = IMFByteStreamHandler_QueryInterface(&this->IMFByteStreamHandler_iface, riid, obj);
    IMFByteStreamHandler_Release(&this->IMFByteStreamHandler_iface);

    return hr;
}

/* helper for callback forwarding */
void perform_cb_media_source(struct cb_data *cbdata)
{
    switch(cbdata->type)
    {
    case PULL_FROM_BYTESTREAM:
        {
            struct getrange_data *data = &cbdata->u.getrange_data;
            cbdata->u.getrange_data.ret = pull_from_bytestream(data->pad, data->parent,
                    data->ofs, data->len, data->buf);
            break;
        }
    case QUERY_BYTESTREAM:
        {
            struct query_function_data *data = &cbdata->u.query_function_data;
            cbdata->u.query_function_data.ret = query_bytestream(data->pad, data->parent, data->query);
            break;
        }
    case ACTIVATE_BYTESTREAM_PAD_MODE:
        {
            struct activate_mode_data *data = &cbdata->u.activate_mode_data;
            cbdata->u.activate_mode_data.ret = activate_bytestream_pad_mode(data->pad, data->parent, data->mode, data->activate);
            break;
        }
    case PROCESS_BYTESTREAM_PAD_EVENT:
        {
            struct event_src_data *data = &cbdata->u.event_src_data;
            cbdata->u.event_src_data.ret = process_bytestream_pad_event(data->pad, data->parent, data->event);
            break;
        }
    default:
        {
            ERR("Wrong callback forwarder called\n");
            return;
        }
    }
}
