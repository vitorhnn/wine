/*
 * GStreamer splitter + decoder, adapted from parser.c
 *
 * Copyright 2010 Maarten Lankhorst for CodeWeavers
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

#ifndef __GST_PRIVATE_INCLUDED__
#define __GST_PRIVATE_INCLUDED__

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wtypes.h"
#include "winuser.h"
#include "dshow.h"
#include "strmif.h"
#include "mfidl.h"
#include "mfobjects.h"
#include "wine/heap.h"
#include "wine/strmbase.h"

#define MEDIATIME_FROM_BYTES(x) ((LONGLONG)(x) * 10000000)

IUnknown * CALLBACK avi_splitter_create(IUnknown *outer, HRESULT *phr) DECLSPEC_HIDDEN;
IUnknown * CALLBACK mpeg_splitter_create(IUnknown *outer, HRESULT *phr) DECLSPEC_HIDDEN;
IUnknown * CALLBACK Gstreamer_AudioConvert_create(IUnknown *pUnkOuter, HRESULT *phr);
IUnknown * CALLBACK Gstreamer_Mp3_create(IUnknown *pUnkOuter, HRESULT *phr);
IUnknown * CALLBACK Gstreamer_YUV2RGB_create(IUnknown *pUnkOuter, HRESULT *phr);
IUnknown * CALLBACK Gstreamer_YUV2ARGB_create(IUnknown *pUnkOuter, HRESULT *phr);
IUnknown * CALLBACK Gstreamer_Splitter_create(IUnknown *pUnkOuter, HRESULT *phr);
IUnknown * CALLBACK wave_parser_create(IUnknown *outer, HRESULT *phr) DECLSPEC_HIDDEN;

BOOL init_gstreamer(void) DECLSPEC_HIDDEN;

GstFlowReturn got_data(GstPad *pad, GstObject *parent, GstBuffer *buf) DECLSPEC_HIDDEN;
GstFlowReturn request_buffer(GstPad *pad, guint64 ofs, guint size, GstCaps *caps, GstBuffer **buf) DECLSPEC_HIDDEN;
void Gstreamer_transform_pad_added(GstElement *filter, GstPad *pad, gpointer user) DECLSPEC_HIDDEN;

void start_dispatch_thread(void) DECLSPEC_HIDDEN;

extern const char *media_quark_string DECLSPEC_HIDDEN;

extern HRESULT mfplat_DllRegisterServer(void) DECLSPEC_HIDDEN;
extern HRESULT mfplat_get_class_object(REFCLSID rclsid, REFIID riid, void **obj) DECLSPEC_HIDDEN;
extern HRESULT mfplat_can_unload_now(void) DECLSPEC_HIDDEN;

/* helper sub-object that handles ansyncronous nature of handlers */

struct handler;
typedef HRESULT (*p_create_object_callback)(struct handler *handler, WCHAR *url, IMFByteStream *stream, DWORD flags, IPropertyStore *props,
                                            IUnknown **out_object, MF_OBJECT_TYPE *out_obj_type);

struct handler
{
    IMFAsyncCallback IMFAsyncCallback_iface;
    struct list results;
    CRITICAL_SECTION cs;
    p_create_object_callback create_object;
};

void handler_construct(struct handler *handler, p_create_object_callback create_object_callback);
void handler_destruct(struct handler *handler);
HRESULT handler_begin_create_object(struct handler *handler, IMFByteStream *stream,
        const WCHAR *url, DWORD flags, IPropertyStore *props, IUnknown **cancel_cookie,
        IMFAsyncCallback *callback, IUnknown *state);
HRESULT handler_end_create_object(struct handler *handler, IMFAsyncResult *result,
        MF_OBJECT_TYPE *obj_type, IUnknown **object);
HRESULT handler_cancel_object_creation(struct handler *handler, IUnknown *cancel_cookie);

IMFMediaType* mfplat_media_type_from_caps(GstCaps *caps);
IMFSample* mf_sample_from_gst_sample(GstSample *in);
GstSample* gst_sample_from_mf_sample(IMFSample *in);

HRESULT mpeg4_stream_handler_construct(REFIID riid, void **obj);
HRESULT h264_decoder_construct(REFIID riid, void **obj);

#endif /* __GST_PRIVATE_INCLUDED__ */
