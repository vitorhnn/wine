#include "config.h"

#include <gst/gst.h>

#include "gst_private.h"
#include "gst_cbs.h"

#include <stdarg.h>

#define COBJMACROS
#define NONAMELESSUNION

#include "mfapi.h"
#include "mferror.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

struct sample_request
{
    struct list entry;
    IUnknown *token;
};

struct mpeg4_stream
{
    IMFMediaStream IMFMediaStream_iface;
    LONG ref;
    IMFMediaEventQueue *event_queue;
    IMFStreamDescriptor *descriptor;
    GstElement *appsink;
    GstPad *their_src, *appsink_sink;
    /* usually reflects state of source */
    enum
    {
        STREAM_INACTIVE,
        STREAM_ENABLED,
        STREAM_PAUSED,
        STREAM_RUNNING,
    } state;
    CRITICAL_SECTION dispatch_samples_cs;
    struct list sample_requests;
    unsigned int pending_samples;
};

struct mpeg4_source
{
    IMFMediaSource IMFMediaSource_iface;
    IMFGetService IMFGetService_iface;
    IMFSeekInfo IMFSeekInfo_iface;
    LONG ref;
    IMFMediaEventQueue *event_queue;
    IMFByteStream *byte_stream;
    struct mpeg4_stream **streams;
    ULONG stream_count;
    IMFPresentationDescriptor *pres_desc;
    GstBus *bus;
    GstElement *qtdemux;
    GstPad *my_src, *their_sink;
    enum
    {
        SOURCE_OPENING,
        SOURCE_STOPPED, /* (READY) */
        SOURCE_PAUSED,
        SOURCE_RUNNING,
    } state;
    CRITICAL_SECTION streams_cs;
    HANDLE init_complete_event;
};

/* stream */

static void stream_dispatch_samples(struct mpeg4_stream *This)
{
    struct sample_request *req, *cursor2;

    if (This->state != STREAM_RUNNING)
        return;

    EnterCriticalSection(&This->dispatch_samples_cs);

    if (!(This->pending_samples))
        goto leave;

    LIST_FOR_EACH_ENTRY_SAFE(req, cursor2, &This->sample_requests, struct sample_request, entry)
    {
        IMFSample *sample;

        /* Get the sample from the appsink, then construct an IMFSample */
        /* We do this in the dispatch function so we can have appsink buffer for us */
        {
            GstSample *gst_sample;

            g_signal_emit_by_name (This->appsink, "pull-sample", &gst_sample);
            if (!gst_sample)
            {
                ERR("Appsink has no samples and pending_samples != 0\n");
                goto leave;
            }

            sample = mf_sample_from_gst_sample(gst_sample);
        }

        if (req->token)
        {
            IMFSample_SetUnknown(sample, &MFSampleExtension_Token, req->token);
        }

        IMFMediaEventQueue_QueueEventParamUnk(This->event_queue, MEMediaSample, &GUID_NULL, S_OK, (IUnknown *)sample);

        if (req->token)
        {
            IUnknown_Release(req->token);
        }

        list_remove(&req->entry);

        This->pending_samples--;
    }

    leave:
    LeaveCriticalSection(&This->dispatch_samples_cs);
}

static inline struct mpeg4_stream *impl_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg4_stream, IMFMediaStream_iface);
}

static HRESULT WINAPI mpeg4_stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **out)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IMFMediaStream) ||
        IsEqualIID(riid, &IID_IMFMediaEventGenerator) ||
        IsEqualIID(riid, &IID_IUnknown))
    {
        *out = &This->IMFMediaStream_iface;
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

static ULONG WINAPI mpeg4_stream_AddRef(IMFMediaStream *iface)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%u\n", This, ref);

    return ref;
}

static ULONG WINAPI mpeg4_stream_Release(IMFMediaStream *iface)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);

    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%u\n", This, ref);

    if (!ref)
    {
        ERR("incomplete cleanup\n");
        IMFMediaEventQueue_Release(This->event_queue);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI mpeg4_stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%#x, %p)\n", This, flags, event);

    return IMFMediaEventQueue_GetEvent(This->event_queue, flags, event);
}

static HRESULT WINAPI mpeg4_stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%p, %p)\n", This, callback, state);

    return IMFMediaEventQueue_BeginGetEvent(This->event_queue, callback, state);
}

static HRESULT WINAPI mpeg4_stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%p, %p)\n", This, result, event);

    return IMFMediaEventQueue_EndGetEvent(This->event_queue, result, event);
}

static HRESULT WINAPI mpeg4_stream_QueueEvent(IMFMediaStream *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%d, %s, %#x, %p)\n", This, event_type, debugstr_guid(ext_type), hr, value);

    return IMFMediaEventQueue_QueueEventParamVar(This->event_queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI mpeg4_stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **source)
{
    FIXME("stub (%p)->(%p)\n", iface, source);
    return E_NOTIMPL;
}

static HRESULT WINAPI mpeg4_stream_GetStreamDescriptor(IMFMediaStream* iface, IMFStreamDescriptor **descriptor)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);

    TRACE("(%p)->(%p)\n", This, descriptor);

    IMFStreamDescriptor_AddRef(This->descriptor);
    *descriptor = This->descriptor;

    return S_OK;    
}

static HRESULT WINAPI mpeg4_stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct mpeg4_stream *This = impl_from_IMFMediaStream(iface);
    struct sample_request *req;

    TRACE("(%p)->(%p)\n", iface, token);

    if (This->state == STREAM_INACTIVE || This->state == STREAM_ENABLED)
    {
        WARN("Stream isn't active\n");
        return MF_E_INVALIDREQUEST;
    }

    req = heap_alloc(sizeof(*req));
    if (token)
        IUnknown_AddRef(token);
    req->token = token;
    list_add_tail(&This->sample_requests, &req->entry);

    stream_dispatch_samples(This);

    return S_OK;
}

static const IMFMediaStreamVtbl mpeg4_stream_vtbl =
{
    mpeg4_stream_QueryInterface,
    mpeg4_stream_AddRef,
    mpeg4_stream_Release,
    mpeg4_stream_GetEvent,
    mpeg4_stream_BeginGetEvent,
    mpeg4_stream_EndGetEvent,
    mpeg4_stream_QueueEvent,
    mpeg4_stream_GetMediaSource,
    mpeg4_stream_GetStreamDescriptor,
    mpeg4_stream_RequestSample
};

static GstFlowReturn stream_new_sample(GstElement *appsink, gpointer user)
{
    struct mpeg4_stream *This = (struct mpeg4_stream *) user;

    TRACE("(%p) got sample\n", This);

    if (This->state == STREAM_INACTIVE)
    {
        ERR("got sample on inactive stream\n");
    }

    This->pending_samples++;
    stream_dispatch_samples(This);
    return GST_FLOW_OK;
}

static HRESULT mpeg4_stream_constructor(struct mpeg4_source *source, GstPad *pad, DWORD stream_id, struct mpeg4_stream **out_stream)
{
    HRESULT hr;
    GstCaps *caps;
    IMFMediaType *media_type;
    IMFMediaTypeHandler *type_handler;
    struct mpeg4_stream *This = heap_alloc_zero(sizeof(*This));

    TRACE("(%p %p)->(%p)\n", source, pad, out_stream);

    if (FAILED(hr = MFCreateMediaType(&media_type)))
    {
        goto fail;
    }

    if (FAILED(hr = MFCreateEventQueue(&This->event_queue)))
    {
        goto fail;
    }

    caps = gst_pad_query_caps(pad, NULL);

    if (!(caps))
    {
        goto fail;
    }

    media_type = mfplat_media_type_from_caps(caps);

    MFCreateStreamDescriptor(stream_id, 1, &media_type, &This->descriptor);
    IMFMediaType_Release(media_type);

    IMFStreamDescriptor_GetMediaTypeHandler(This->descriptor, &type_handler);
    IMFMediaTypeHandler_SetCurrentMediaType(type_handler, media_type);
    IMFMediaTypeHandler_Release(type_handler);

    /* Setup appsink element, but don't link it to the demuxer */
    if (!(This->appsink = gst_element_factory_make("appsink", NULL)))
    {
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    g_object_set(This->appsink, "emit-signals", TRUE, NULL);
    g_signal_connect( This->appsink, "new-sample", G_CALLBACK(stream_new_sample_wrapper), This);

    This->appsink_sink = gst_element_get_static_pad(This->appsink, "sink");

    This->their_src = pad;
    gst_pad_set_element_private(pad, This);

    This->state = STREAM_INACTIVE;

    InitializeCriticalSection(&This->dispatch_samples_cs);
    This->pending_samples = 0;
    list_init(&This->sample_requests);

    This->IMFMediaStream_iface.lpVtbl = &mpeg4_stream_vtbl;
    This->ref = 1;

    TRACE("->(%p)\n", This);

    *out_stream = This;
    return S_OK;

    fail:
    ERR("incomplete cleanup\n");
    if (This->event_queue)
    {
        IMFMediaEventQueue_Release(This->event_queue);
    }
    return hr;
}

/* source */

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
    else if(IsEqualIID(riid, &IID_IMFGetService))
    {
        *out = &This->IMFGetService_iface;
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
        IMFByteStream_Release(This->byte_stream);
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

    TRACE("(%p)->(%p)\n", This, descriptor);

    if (!(This->pres_desc))
    {
        return MF_E_NOT_INITIALIZED;
    }

    IMFPresentationDescriptor_Clone(This->pres_desc, descriptor);

    return S_OK;
}

static HRESULT WINAPI mpeg4_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor,
                                     const GUID *time_format, const PROPVARIANT *start_position)
{
    struct mpeg4_source *This = impl_from_IMFMediaSource(iface);
    PROPVARIANT empty_var;
    empty_var.vt = VT_EMPTY;

    TRACE("(%p)->(%p, %p, %p)\n", This, descriptor, time_format, start_position);

    /* Find out which streams are active */
    for (unsigned int i = 0; i < This->stream_count; i++)
    {
        IMFStreamDescriptor *stream_desc;
        DWORD in_stream_id;
        BOOL selected;

        IMFPresentationDescriptor_GetStreamDescriptorByIndex(descriptor, i, &selected, &stream_desc);
        IMFStreamDescriptor_GetStreamIdentifier(stream_desc, &in_stream_id);

        for (unsigned int k = 0; k < This->stream_count; k++)
        {
            DWORD cur_stream_id;

            IMFStreamDescriptor_GetStreamIdentifier(This->streams[k]->descriptor, &cur_stream_id);

            if (in_stream_id == cur_stream_id)
            {
                BOOL was_active = This->streams[k]->state != STREAM_INACTIVE;
                This->streams[k]->state = selected ? STREAM_RUNNING : STREAM_INACTIVE;
                if (selected)
                {
                    IMFMediaEventQueue_QueueEventParamUnk(This->event_queue,
                        was_active ? MEUpdatedStream : MENewStream, &GUID_NULL,
                        S_OK, (IUnknown*) &This->streams[k]->IMFMediaStream_iface);
                    IMFMediaEventQueue_QueueEventParamVar(This->streams[k]->event_queue,
                        MEStreamStarted, &GUID_NULL, S_OK, &empty_var);
                    stream_dispatch_samples(This->streams[k]);
                }
            }
        }

        IMFStreamDescriptor_Release(stream_desc);
    }

    if (!IsEqualIID(time_format, &GUID_NULL) || start_position->vt != VT_EMPTY)
    {
        WARN("ignoring start time\n");
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    This->state = SOURCE_RUNNING;
    gst_element_set_state(This->qtdemux, GST_STATE_PLAYING);

    IMFMediaEventQueue_QueueEventParamVar(This->event_queue, MESourceStarted, &GUID_NULL, S_OK, &empty_var);

    return S_OK;
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

static const IMFMediaSourceVtbl IMFMediaSource_vtbl =
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

static inline struct mpeg4_source *impl_from_IMFGetService(IMFGetService *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg4_source, IMFGetService_iface);
}

static HRESULT WINAPI source_get_service_QueryInterface(IMFGetService *iface, REFIID riid, void **obj)
{
    struct mpeg4_source *source = impl_from_IMFGetService(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI source_get_service_AddRef(IMFGetService *iface)
{
    struct mpeg4_source *source = impl_from_IMFGetService(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI source_get_service_Release(IMFGetService *iface)
{
    struct mpeg4_source *source = impl_from_IMFGetService(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI source_get_service_GetService(IMFGetService *iface, REFGUID service, REFIID riid, void **obj)
{
    struct mpeg4_source *This = impl_from_IMFGetService(iface);

    TRACE("(%p)->(%s, %s, %p)\n", This, debugstr_guid(service), debugstr_guid(riid), obj);

    *obj = NULL;

    if (IsEqualIID(service, &MF_SCRUBBING_SERVICE))
    {
        if (IsEqualIID(riid, &IID_IMFSeekInfo))
        {
            *obj = &This->IMFSeekInfo_iface;
        }
    }

    if (*obj)
        IUnknown_AddRef((IUnknown*) *obj);
    
    return *obj ? S_OK : E_NOINTERFACE;
}

static const IMFGetServiceVtbl IMFGetService_vtbl =
{
    source_get_service_QueryInterface,
    source_get_service_AddRef,
    source_get_service_Release,
    source_get_service_GetService,
};

static inline struct mpeg4_source *impl_from_IMFSeekInfo(IMFSeekInfo *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg4_source, IMFSeekInfo_iface);
}

static HRESULT WINAPI source_seek_info_QueryInterface(IMFSeekInfo *iface, REFIID riid, void **obj)
{
    struct mpeg4_source *source = impl_from_IMFSeekInfo(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI source_seek_info_AddRef(IMFSeekInfo *iface)
{
    struct mpeg4_source *source = impl_from_IMFSeekInfo(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI source_seek_info_Release(IMFSeekInfo *iface)
{
    struct mpeg4_source *source = impl_from_IMFSeekInfo(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI source_seek_info_GetNearestKeyFrames(IMFSeekInfo *iface, const GUID *format,
        const PROPVARIANT *position, PROPVARIANT *prev_frame, PROPVARIANT *next_frame)
{
    struct mpeg4_source *This = impl_from_IMFSeekInfo(iface);

    FIXME("(%p)->(%s, %p, %p, %p) - stub\n", This, debugstr_guid(format), position, prev_frame, next_frame);
    
    return E_NOTIMPL;
}

static const IMFSeekInfoVtbl IMFSeekInfo_vtbl =
{
    source_seek_info_QueryInterface,
    source_seek_info_AddRef,
    source_seek_info_Release,
    source_seek_info_GetNearestKeyFrames,
};

GstFlowReturn pull_from_bytestream(GstPad *pad, GstObject *parent, guint64 ofs, guint len,
        GstBuffer **buf)
{
    struct mpeg4_source *This = gst_pad_get_element_private(pad);
    IMFByteStream *byte_stream = This->byte_stream;
    BOOL is_eof;
    GstMapInfo info;
    ULONG bytes_read;
    HRESULT hr;

    TRACE("gstreamer requesting %u bytes at %s from source %p into buffer %p\n", len, wine_dbgstr_longlong(ofs), This, buf);

    if (ofs != GST_BUFFER_OFFSET_NONE)
    {
        if (FAILED(IMFByteStream_SetCurrentPosition(byte_stream, ofs)))
            return GST_FLOW_ERROR;
    }

    if (FAILED(IMFByteStream_IsEndOfStream(byte_stream, &is_eof)))
        return GST_FLOW_ERROR;
    if (is_eof)
        return GST_FLOW_EOS;

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
    struct mpeg4_source *This = gst_pad_get_element_private(pad);
    GstFormat format;
    QWORD bytestream_len;
    gboolean ret;

    TRACE("GStreamer queries source %p for %s\n", This, GST_QUERY_TYPE_NAME(query));

    if (FAILED(IMFByteStream_GetLength(This->byte_stream, &bytestream_len)))
        return FALSE;

    switch (GST_QUERY_TYPE(query))
    {
        case GST_QUERY_DURATION:
        {
            LONGLONG duration;

            gst_query_parse_duration (query, &format, NULL);
            if (format == GST_FORMAT_PERCENT) {
                gst_query_set_duration (query, GST_FORMAT_PERCENT, GST_FORMAT_PERCENT_MAX);
                return TRUE;
            }
            ret = gst_pad_query_convert (pad, GST_FORMAT_BYTES, bytestream_len, format, &duration);
            gst_query_set_duration(query, format, duration);
            return ret;
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

            caps = gst_caps_new_any();

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
    struct mpeg4_source *source = gst_pad_get_element_private(pad);

    TRACE("%s source pad for mediasource %p in %s mode.\n",
            activate ? "Activating" : "Deactivating", source, gst_pad_mode_get_name(mode));

    /* There is no push mode in mfplat */

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
    struct mpeg4_source *This = gst_pad_get_element_private(pad);

    TRACE("filter %p, type \"%s\".\n", This, GST_EVENT_TYPE_NAME(event));

    switch (event->type) {
        default:
            WARN("Ignoring \"%s\" event.\n", GST_EVENT_TYPE_NAME(event));
        case GST_EVENT_TAG:
        case GST_EVENT_QOS:
        case GST_EVENT_RECONFIGURE:
            return gst_pad_event_default(pad, parent, event);
    }
    return TRUE;
}

static void source_stream_added(GstElement *element, GstPad *pad, gpointer user)
{
    struct mpeg4_stream *stream;
    struct mpeg4_source *source = (struct mpeg4_source *) user;
    struct mpeg4_stream **new_stream_array;
    gchar *g_stream_id;
    const char *stream_id_string;
    DWORD stream_id;

    EnterCriticalSection(&source->streams_cs);

    g_stream_id = gst_pad_get_stream_id(pad);
    stream_id_string = strstr(g_stream_id, "/");
    sscanf(stream_id_string, "/%03u", &stream_id);
    TRACE("stream-id: %u\n", stream_id);
    g_free(g_stream_id);

    /* find existing stream */
    for (unsigned int i = 0; i < source->stream_count; i++)
    {
        DWORD existing_stream_id;
        IMFStreamDescriptor *descriptor = source->streams[i]->descriptor;

        if (FAILED(IMFStreamDescriptor_GetStreamIdentifier(descriptor, &existing_stream_id)))
            goto leave;

        if (existing_stream_id == stream_id)
        {
            struct mpeg4_stream *existing_stream = source->streams[i];
            existing_stream->their_src = pad;
            if (!existing_stream->appsink_sink)
            {
                ERR("Couldn't find our appsink sink\n");
                goto leave;
            }
            if (existing_stream->state != STREAM_INACTIVE)
            {
                gst_pad_link(existing_stream->their_src, existing_stream->appsink_sink);
            }
            goto leave;
        }
    }

    if (FAILED(mpeg4_stream_constructor(source, pad, stream_id, &stream)))
    {
        goto leave;
    }

    if (!(new_stream_array = heap_realloc(source->streams, (source->stream_count + 1) * (sizeof(*new_stream_array)))))
    {
        ERR("Failed to add stream to source\n");
        goto leave;
    }

    source->streams = new_stream_array;
    source->streams[source->stream_count++] = stream;

    leave:
    LeaveCriticalSection(&source->streams_cs);
    return; 
}

static void source_stream_removed(GstElement *element, GstPad *pad, gpointer user)
{
    struct mpeg4_stream *stream;
    /* 0) Find stream 1) de-activate sinkpad 2) change response to RequestSample 3) unset their_src */

    stream = (struct mpeg4_stream *) gst_pad_get_element_private(pad);

    if (stream)
    {
        if (stream->their_src != pad)
        {
            ERR("assert: unexpected pad/user combination!!!\n");
            return;
        }
        if (stream->state != STREAM_INACTIVE)
        {
            gst_pad_unlink(stream->their_src, stream->appsink_sink);

            /* TODO */
            WARN("Need to send some events here\n ");
        }

        stream->their_src = NULL;

        gst_pad_set_element_private(pad, NULL);
    }
}

static void source_all_streams(GstElement *element, gpointer user)
{
    static const WCHAR videomp4W[] = {'v','i','d','e','o','/','m','p','4',0};
    IMFStreamDescriptor **descriptors;
    struct mpeg4_source *source = (struct mpeg4_source *) user;

    EnterCriticalSection(&source->streams_cs);
    if (source->state != SOURCE_OPENING)
        goto leave;

    /* Init presentation descriptor */

    descriptors = heap_alloc(source->stream_count * sizeof(IMFStreamDescriptor*));
    for (unsigned int i = 0; i < source->stream_count; i++)
    {
        IMFMediaStream_GetStreamDescriptor(&source->streams[i]->IMFMediaStream_iface, &descriptors[i]);
    }

    if (FAILED(MFCreatePresentationDescriptor(source->stream_count, descriptors, &source->pres_desc)))
        goto leave;

    IMFPresentationDescriptor_SetString(source->pres_desc, &MF_PD_MIME_TYPE, videomp4W);

    for (unsigned int i = 0; i < source->stream_count; i++)
    {
        IMFStreamDescriptor_Release(descriptors[i]);
    }
    heap_free(descriptors);

    SetEvent(source->init_complete_event);

    leave:
    LeaveCriticalSection(&source->streams_cs);
}

static HRESULT mpeg4_source_constructor(IMFByteStream *bytestream, struct mpeg4_source **out_mpeg4_source)
{
    GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
        "mf_src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS_ANY);

    HRESULT hr;
    int ret;
    struct mpeg4_source *This = heap_alloc_zero(sizeof(*This));

    if (!This)
        return E_OUTOFMEMORY;

    This->state = SOURCE_OPENING;
    InitializeCriticalSection(&This->streams_cs);
    This->init_complete_event = CreateEventA(NULL, TRUE, FALSE, NULL);

    if (FAILED(hr = IMFByteStream_QueryInterface(bytestream, &IID_IMFByteStream, (void **)&This->byte_stream)))
    {
        goto fail;
    }

    if (FAILED(hr = MFCreateEventQueue(&This->event_queue)))
        goto fail;

    /* create demuxer */

    This->my_src = gst_pad_new_from_static_template(&src_template, "mf-src");
    gst_pad_set_element_private(This->my_src, This);
    gst_pad_set_getrange_function(This->my_src, pull_from_bytestream_wrapper);
    gst_pad_set_query_function(This->my_src, query_bytestream_wrapper);
    gst_pad_set_activatemode_function(This->my_src, activate_bytestream_pad_mode_wrapper);
    gst_pad_set_event_function(This->my_src, process_bytestream_pad_event_wrapper);

    This->qtdemux = gst_element_factory_make("qtdemux", NULL);
    if (!(This->qtdemux))
    {
        WARN("Failed to create demuxer for source\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    This->bus = gst_bus_new();
    if (!(This->bus))
    {
        WARN("Failed to create bus\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    gst_element_set_bus(This->qtdemux, This->bus);

    This->their_sink = gst_element_get_static_pad(This->qtdemux, "sink");

    if ((ret = gst_pad_link(This->my_src, This->their_sink)) < 0)
    {
        WARN("Failed to link our bytestream pad to the demuxer input\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    g_signal_connect(This->qtdemux, "pad-added", G_CALLBACK(source_stream_added_wrapper), This);
    g_signal_connect(This->qtdemux, "pad-removed", G_CALLBACK(source_stream_removed_wrapper), This);
    g_signal_connect(This->qtdemux, "no-more-pads", G_CALLBACK(source_all_streams_wrapper), This);

    gst_element_set_state(This->qtdemux, GST_STATE_PLAYING);
    ret = gst_element_get_state(This->qtdemux, NULL, NULL, -1);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        ERR("Failed to play source.\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    WaitForSingleObject(This->init_complete_event, INFINITE);
    CloseHandle(This->init_complete_event);

    gst_element_set_state(This->qtdemux, GST_STATE_READY);
    if (!(This->pres_desc))
    {
        hr = E_FAIL;
        goto fail;
    }

    This->state = SOURCE_STOPPED;

    This->IMFMediaSource_iface.lpVtbl = &IMFMediaSource_vtbl;
    This->IMFGetService_iface.lpVtbl = &IMFGetService_vtbl;
    This->IMFSeekInfo_iface.lpVtbl = &IMFSeekInfo_vtbl;
    This->ref = 1;

    *out_mpeg4_source = This;
    return S_OK;

    fail:
    WARN("Failed to construct MFMediaSource, hr = %x\n", hr);
    if (This->event_queue)
    {
        IMFMediaEventQueue_Release(This->event_queue);
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

    if (!(init_gstreamer()))
        return E_FAIL;

    if (flags & MF_RESOLUTION_MEDIASOURCE)
    {
        HRESULT hr;
        struct mpeg4_source *new_source;

        if (FAILED(hr = mpeg4_source_constructor(stream, &new_source)))
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

/* helper for callback forwarding */
void forward_cb_mpeg4_source(struct cb_data *cbdata)
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
    case SOURCE_STREAM_ADDED:
        {
            struct pad_added_data *data = &cbdata->u.pad_added_data;
            source_stream_added(data->element, data->pad, data->user);
            break;
        }
    case SOURCE_STREAM_REMOVED:
        {
            struct pad_removed_data *data = &cbdata->u.pad_removed_data;
            source_stream_removed(data->element, data->pad, data->user);
            break;
        }
    case SOURCE_ALL_STREAMS:
        {
            struct no_more_pads_data *data = &cbdata->u.no_more_pads_data;
            source_all_streams(data->element, data->user);
            break;
        }
    case STREAM_NEW_SAMPLE:
        {
            struct new_sample_data *data = &cbdata->u.new_sample_data;
            cbdata->u.new_sample_data.ret = stream_new_sample(data->appsink, data->user);
            break;
        }
    default:
        {
            ERR("Wrong callback forwarder called\n");
            return;
        }
    }
}