/*
 * Copyright 2019 Nikolay Sivov for CodeWeavers
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

#include "config.h"
#include <gst/gst.h>

#include "gst_private.h"

#include <stdarg.h>

#include "gst_private.h"
#include "mfapi.h"
#include "mfidl.h"
#include "codecapi.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

struct video_processor
{
    IMFTransform IMFTransform_iface;
    LONG refcount;
    IMFAttributes *attributes;
    IMFAttributes *output_attributes;
};

static struct video_processor *impl_video_processor_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct video_processor, IMFTransform_iface);
}

static HRESULT WINAPI video_processor_QueryInterface(IMFTransform *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IMFTransform) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFTransform_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI video_processor_AddRef(IMFTransform *iface)
{
    struct video_processor *transform = impl_video_processor_from_IMFTransform(iface);
    ULONG refcount = InterlockedIncrement(&transform->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI video_processor_Release(IMFTransform *iface)
{
    struct video_processor *transform = impl_video_processor_from_IMFTransform(iface);
    ULONG refcount = InterlockedDecrement(&transform->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        if (transform->attributes)
            IMFAttributes_Release(transform->attributes);
        if (transform->output_attributes)
            IMFAttributes_Release(transform->output_attributes);
        heap_free(transform);
    }

    return refcount;
}

static HRESULT WINAPI video_processor_GetStreamLimits(IMFTransform *iface, DWORD *input_minimum, DWORD *input_maximum,
        DWORD *output_minimum, DWORD *output_maximum)
{
    TRACE("%p, %p, %p, %p, %p.\n", iface, input_minimum, input_maximum, output_minimum, output_maximum);

    *input_minimum = *input_maximum = *output_minimum = *output_maximum = 1;

    return S_OK;
}

static HRESULT WINAPI video_processor_GetStreamCount(IMFTransform *iface, DWORD *inputs, DWORD *outputs)
{
    TRACE("%p, %p, %p.\n", iface, inputs, outputs);

    *inputs = *outputs = 1;

    return S_OK;
}

static HRESULT WINAPI video_processor_GetStreamIDs(IMFTransform *iface, DWORD input_size, DWORD *inputs,
        DWORD output_size, DWORD *outputs)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetAttributes(IMFTransform *iface, IMFAttributes **attributes)
{
    struct video_processor *transform = impl_video_processor_from_IMFTransform(iface);

    TRACE("%p, %p.\n", iface, attributes);

    *attributes = transform->attributes;
    IMFAttributes_AddRef(*attributes);

    return S_OK;
}

static HRESULT WINAPI video_processor_GetInputStreamAttributes(IMFTransform *iface, DWORD id,
        IMFAttributes **attributes)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetOutputStreamAttributes(IMFTransform *iface, DWORD id,
        IMFAttributes **attributes)
{
    struct video_processor *transform = impl_video_processor_from_IMFTransform(iface);

    TRACE("%p, %u, %p.\n", iface, id, attributes);

    *attributes = transform->output_attributes;
    IMFAttributes_AddRef(*attributes);

    return S_OK;
}

static HRESULT WINAPI video_processor_DeleteInputStream(IMFTransform *iface, DWORD id)
{
    TRACE("%p, %u.\n", iface, id);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_AddInputStreams(IMFTransform *iface, DWORD streams, DWORD *ids)
{
    TRACE("%p, %u, %p.\n", iface, streams, ids);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetInputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    FIXME("%p, %u, %u, %p.\n", iface, id, index, type);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetOutputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    FIXME("%p, %u, %u, %p.\n", iface, id, index, type);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    FIXME("%p, %u, %p, %#x.\n", iface, id, type, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    FIXME("%p, %u, %p, %#x.\n", iface, id, type, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    FIXME("%p, %u, %p.\n", iface, id, type);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    FIXME("%p, %u, %p.\n", iface, id, type);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    FIXME("%p, %u, %p.\n", iface, id, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_GetOutputStatus(IMFTransform *iface, DWORD *flags)
{
    FIXME("%p, %p.\n", iface, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_SetOutputBounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper)
{
    FIXME("%p, %s, %s.\n", iface, wine_dbgstr_longlong(lower), wine_dbgstr_longlong(upper));

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_ProcessEvent(IMFTransform *iface, DWORD id, IMFMediaEvent *event)
{
    TRACE("%p, %u, %p.\n", iface, id, event);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    FIXME("%p, %u.\n", iface, message);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    FIXME("%p, %u, %p, %#x.\n", iface, id, sample, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_processor_ProcessOutput(IMFTransform *iface, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
    FIXME("%p, %#x, %u, %p, %p.\n", iface, flags, count, samples, status);

    return E_NOTIMPL;
}

static const IMFTransformVtbl video_processor_vtbl =
{
    video_processor_QueryInterface,
    video_processor_AddRef,
    video_processor_Release,
    video_processor_GetStreamLimits,
    video_processor_GetStreamCount,
    video_processor_GetStreamIDs,
    video_processor_GetInputStreamInfo,
    video_processor_GetOutputStreamInfo,
    video_processor_GetAttributes,
    video_processor_GetInputStreamAttributes,
    video_processor_GetOutputStreamAttributes,
    video_processor_DeleteInputStream,
    video_processor_AddInputStreams,
    video_processor_GetInputAvailableType,
    video_processor_GetOutputAvailableType,
    video_processor_SetInputType,
    video_processor_SetOutputType,
    video_processor_GetInputCurrentType,
    video_processor_GetOutputCurrentType,
    video_processor_GetInputStatus,
    video_processor_GetOutputStatus,
    video_processor_SetOutputBounds,
    video_processor_ProcessEvent,
    video_processor_ProcessMessage,
    video_processor_ProcessInput,
    video_processor_ProcessOutput,
};

struct class_factory
{
    IClassFactory IClassFactory_iface;
    LONG refcount;
    HRESULT (*create_instance)(REFIID riid, void **obj);
};

static struct class_factory *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, struct class_factory, IClassFactory_iface);
}

static HRESULT WINAPI class_factory_QueryInterface(IClassFactory *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IClassFactory) ||
            IsEqualGUID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }

    WARN("%s is not supported.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI class_factory_AddRef(IClassFactory *iface)
{
    struct class_factory *factory = impl_from_IClassFactory(iface);
    return InterlockedIncrement(&factory->refcount);
}

static ULONG WINAPI class_factory_Release(IClassFactory *iface)
{
    struct class_factory *factory = impl_from_IClassFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    if (!refcount)
        heap_free(factory);

    return refcount;
}

static HRESULT WINAPI class_factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **obj)
{
    struct class_factory *factory = impl_from_IClassFactory(iface);

    TRACE("%p, %p, %s, %p.\n", iface, outer, debugstr_guid(riid), obj);

    if (outer)
    {
        *obj = NULL;
        return CLASS_E_NOAGGREGATION;
    }

    return factory->create_instance(riid, obj);
}

static HRESULT WINAPI class_factory_LockServer(IClassFactory *iface, BOOL dolock)
{
    TRACE("%p, %d.\n", iface, dolock);

    if (dolock)
        InterlockedIncrement(&object_locks);
    else
        InterlockedDecrement(&object_locks);

    return S_OK;
}

static const IClassFactoryVtbl class_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    class_factory_CreateInstance,
    class_factory_LockServer,
};

static HRESULT video_processor_create(REFIID riid, void **ret)
{
    struct video_processor *object;
    HRESULT hr;

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IMFTransform_iface.lpVtbl = &video_processor_vtbl;
    object->refcount = 1;

    if (FAILED(hr = MFCreateAttributes(&object->attributes, 0)))
        goto failed;

    if (FAILED(hr = MFCreateAttributes(&object->output_attributes, 0)))
        goto failed;

    *ret = &object->IMFTransform_iface;
    return S_OK;

failed:

    IMFTransform_Release(&object->IMFTransform_iface);
    return hr;
}

static HRESULT mp4_stream_handler_create(REFIID riid, void **ret)
{
    return container_stream_handler_construct(riid, ret, SOURCE_TYPE_MPEG_4);
}

static const struct class_object
{
    const GUID *clsid;
    HRESULT (*create_instance)(REFIID riid, void **obj);
}
class_objects[] =
{
    { &CLSID_VideoProcessorMFT, &video_processor_create },
    { &CLSID_MPEG4ByteStreamHandler, &mp4_stream_handler_create },
};

HRESULT mfplat_get_class_object(REFCLSID rclsid, REFIID riid, void **obj)
{
    struct class_factory *factory;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(class_objects); ++i)
    {
        if (IsEqualGUID(class_objects[i].clsid, rclsid))
        {
            if (!(factory = heap_alloc(sizeof(*factory))))
                return E_OUTOFMEMORY;

            factory->IClassFactory_iface.lpVtbl = &class_factory_vtbl;
            factory->refcount = 1;
            factory->create_instance = class_objects[i].create_instance;

            hr = IClassFactory_QueryInterface(&factory->IClassFactory_iface, riid, obj);
            IClassFactory_Release(&factory->IClassFactory_iface);
            return hr;
        }
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

const static struct
{
    const GUID *subtype;
    GstVideoFormat format;
}
uncompressed_formats[] =
{
    {&MFVideoFormat_ARGB32,  GST_VIDEO_FORMAT_BGRA},
    {&MFVideoFormat_RGB32,   GST_VIDEO_FORMAT_BGRx},
    {&MFVideoFormat_RGB24,   GST_VIDEO_FORMAT_BGR},
    {&MFVideoFormat_RGB565,  GST_VIDEO_FORMAT_BGR16},
    {&MFVideoFormat_RGB555,  GST_VIDEO_FORMAT_BGR15},
};

struct aac_user_data
{
    WORD payload_type;
    WORD profile_level_indication;
    WORD struct_type;
    WORD reserved;
  /* audio-specific-config is stored here */
};

/* IMPORTANT: caps will be modified to represent the exact type needed for the format */
IMFMediaType* mf_media_type_from_caps(GstCaps *caps)
{
    IMFMediaType *media_type;
    GstStructure *info;
    const char *mime_type;

    if (TRACE_ON(mfplat))
    {
        gchar *human_readable = gst_caps_to_string(caps);
        TRACE("caps = %s\n", debugstr_a(human_readable));
        g_free(human_readable);
    }

    if (FAILED(MFCreateMediaType(&media_type)))
    {
        return NULL;
    }

    info = gst_caps_get_structure(caps, 0);
    mime_type = gst_structure_get_name(info);

    if (!(strncmp(mime_type, "video", 5)))
    {
        GstVideoInfo video_info;

        if (!(gst_video_info_from_caps(&video_info, caps)))
        {
            return NULL;
        }

        IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);

        IMFMediaType_SetUINT64(media_type, &MF_MT_FRAME_SIZE, ((UINT64)video_info.width << 32) | video_info.height);

        IMFMediaType_SetUINT64(media_type, &MF_MT_FRAME_RATE, ((UINT64)video_info.fps_n << 32) | video_info.fps_d);

        if (!(strcmp(mime_type, "video/x-raw")))
        {
            GUID fourcc_subtype = MFVideoFormat_Base;
            unsigned int i;

            IMFMediaType_SetUINT32(media_type, &MF_MT_COMPRESSED, FALSE);

            /* First try FOURCC */
            if ((fourcc_subtype.Data1 = gst_video_format_to_fourcc(video_info.finfo->format)))
            {
                IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &fourcc_subtype);
            }
            else
            {
                for (i = 0; i < ARRAY_SIZE(uncompressed_formats); i++)
                {
                    if (uncompressed_formats[i].format == video_info.finfo->format)
                    {
                        IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, uncompressed_formats[i].subtype);
                        break;
                    }
                }
                if (i == ARRAY_SIZE(uncompressed_formats))
                    FIXME("Unrecognized format.\n");
            }
                
        }
        else if (!(strcmp(mime_type, "video/x-h264")))
        {
            const char *profile, *level;

            IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
            IMFMediaType_SetUINT32(media_type, &MF_MT_COMPRESSED, TRUE);

            if ((profile = gst_structure_get_string(info, "profile")))
            {
                if (!(strcmp(profile, "main")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
                else if (!(strcmp(profile, "high")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
                else if (!(strcmp(profile, "high-4:4:4")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_444);
                else
                    FIXME("Unrecognized profile %s\n", profile);
            }
            if ((level = gst_structure_get_string(info, "level")))
            {
                unsigned int i;

                const static struct
                {
                    const char *name;
                    enum eAVEncH264VLevel val;
                } levels[] =
                {
                    {"1",   eAVEncH264VLevel1},
                    {"1.1", eAVEncH264VLevel1_1},
                    {"1.2", eAVEncH264VLevel1_2},
                    {"1.3", eAVEncH264VLevel1_3},
                    {"2",   eAVEncH264VLevel2},
                    {"2.1", eAVEncH264VLevel2_1},
                    {"2.2", eAVEncH264VLevel2_2},
                    {"3",   eAVEncH264VLevel3},
                    {"3.1", eAVEncH264VLevel3_1},
                    {"3.2", eAVEncH264VLevel3_2},
                    {"4",   eAVEncH264VLevel4},
                    {"4.1", eAVEncH264VLevel4_1},
                    {"4.2", eAVEncH264VLevel4_2},
                    {"5",   eAVEncH264VLevel5},
                    {"5.1", eAVEncH264VLevel5_1},
                    {"5.2", eAVEncH264VLevel5_2},
                };
                for (i = 0 ; i < ARRAY_SIZE(levels); i++)
                {
                    if (!(strcmp(level, levels[i].name)))
                    {
                        IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_LEVEL, levels[i].val);
                        break;
                    }
                }
                if (i == ARRAY_SIZE(levels))
                {
                    FIXME("Unrecognized level %s", level);
                }
            }
            gst_caps_set_simple(caps, "stream-format", G_TYPE_STRING, "byte-stream", NULL);
            gst_caps_set_simple(caps, "alignment", G_TYPE_STRING, "au", NULL);
            for (unsigned int i = 0; i < gst_caps_get_size(caps); i++)
            {
                GstStructure *structure = gst_caps_get_structure (caps, i);
                gst_structure_remove_field(structure, "codec_data");
            }
        }
        else if (!(strcmp(mime_type, "video/x-wmv")))
        {
            gint wmv_version;
            const char *format;
            const GValue *codec_data;

            if (gst_structure_get_int(info, "wmvversion", &wmv_version))
            {
                switch (wmv_version)
                {
                    case 1:
                        IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_WMV1);
                        break;
                    case 2:
                        IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_WMV2);
                        break;
                    case 3:
                        IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_WMV3);
                        break;
                    default:
                        FIXME("Unrecognized wmvversion %d\n", wmv_version);
                }
            }

            if ((format = gst_structure_get_string(info, "format")))
            {
                if (!(strcmp(format, "WVC1")))
                    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_WVC1);
                else
                    FIXME("Unrecognized format %s\n", format);
            }

            if ((codec_data = gst_structure_get_value(info, "codec_data")))
            {
                GstBuffer *codec_data_buffer = gst_value_get_buffer(codec_data);
                if (codec_data_buffer)
                {
                    gsize codec_data_size = gst_buffer_get_size(codec_data_buffer);
                    gpointer codec_data_raw = heap_alloc(codec_data_size);
                    gst_buffer_extract(codec_data_buffer, 0, codec_data_raw, codec_data_size);
                    IMFMediaType_SetBlob(media_type, &MF_MT_USER_DATA, codec_data_raw, codec_data_size);
                }
            }
        }
        else if (!(strcmp(mime_type, "video/mpeg")))
        {
            gint mpegversion;
            if (gst_structure_get_int(info, "mpegversion", &mpegversion))
            {
                switch (mpegversion)
                {
                    case 1: IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_MPG1);
                    case 2: IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_MPEG2);
                    case 4: IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_M4S2);
                    default: FIXME("Unrecognized mpeg version %d\n", mpegversion);
                }
            }
            IMFMediaType_SetUINT32(media_type, &MF_MT_COMPRESSED, TRUE);
        }
        else
            FIXME("Unrecognized video format %s\n", mime_type);
    }
    else if (!(strncmp(mime_type, "audio", 5)))
    {
        IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);

        if (!(strcmp(mime_type, "audio/x-raw")))
        {
            const char *format;
            if ((format = gst_structure_get_string(info, "format")))
            {
                char type;
                unsigned int bits_per_sample;
                char endian[2];
                char new_format[6];
                if ((strlen(format) > 5) || (sscanf(format, "%c%u%2c", &type, &bits_per_sample, endian) < 2))
                {
                    FIXME("Unhandled audio format %s\n", format);
                    IMFMediaType_Release(media_type);
                    return NULL;
                }

                if (type == 'F')
                {
                    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
                }
                else if (type == 'U' || type == 'S')
                {
                    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
                    if (bits_per_sample == 8)
                        type = 'U';
                    else
                        type = 'S';
                }
                else
                {
                    FIXME("Unrecognized audio format: %s\n", format);
                    IMFMediaType_Release(media_type);
                    return NULL;
                }

                IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, bits_per_sample);

                if (endian[0] == 'B')
                    endian[0] = 'L';

                sprintf(new_format, "%c%u%.2s", type, bits_per_sample, bits_per_sample > 8 ? endian : 0);
                gst_caps_set_simple(caps, "format", G_TYPE_STRING, new_format, NULL);
            }
            else
            {
                ERR("Failed to get audio format\n");
            }
        }
        else if (!(strcmp(mime_type, "audio/mpeg")))
        {
            int mpeg_version = -1;

            IMFMediaType_SetUINT32(media_type, &MF_MT_COMPRESSED, TRUE);

            if (!(gst_structure_get_int(info, "mpegversion", &mpeg_version)))
                ERR("Failed to get mpegversion\n");
            switch (mpeg_version)
            {
                case 2:
                case 4:
                {
                    const char *format, *profile, *level;
                    DWORD profile_level_indication = 0;
                    const GValue *codec_data;
                    DWORD asc_size = 0;
                    struct aac_user_data *user_data = NULL;

                    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);

                    codec_data = gst_structure_get_value(info, "codec_data");
                    if (codec_data)
                    {
                        GstBuffer *codec_data_buffer = gst_value_get_buffer(codec_data);
                        if (codec_data_buffer)
                        {
                            if ((asc_size = gst_buffer_get_size(codec_data_buffer)) >= 2)
                            {
                                user_data = heap_alloc_zero(sizeof(*user_data)+asc_size);
                                gst_buffer_extract(codec_data_buffer, 0, (gpointer)(user_data + 1), asc_size);
                            }
                            else
                                ERR("Unexpected buffer size\n");
                        }
                        else
                            ERR("codec_data not a buffer\n");
                    }
                    else
                        ERR("codec_data not found\n");
                    if (!user_data)
                        user_data = heap_alloc_zero(sizeof(*user_data));

                    {
                        int rate;
                        if (gst_structure_get_int(info, "rate", &rate))
                            IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                    }
                    {
                        int channels;
                        if (gst_structure_get_int(info, "channels", &channels))
                            IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_NUM_CHANNELS, channels);
                    }

                    if ((format = gst_structure_get_string(info, "stream-format")))
                    {
                        DWORD payload_type = -1;
                        if (!(strcmp(format, "raw")))
                            payload_type = 0;
                        else if (!(strcmp(format, "adts")))
                            payload_type = 1;
                        else if (!(strcmp(format, "adif")))
                            payload_type = 2;
                        else if (!(strcmp(format, "loas")))
                            payload_type = 3;
                        else
                            FIXME("Unrecognized stream-format\n");
                        if (payload_type != -1)
                        {
                            IMFMediaType_SetUINT32(media_type, &MF_MT_AAC_PAYLOAD_TYPE, payload_type);
                            user_data->payload_type = payload_type;
                        }
                    }
                    else
                    {
                        ERR("Stream format not present\n");
                    }

                    profile = gst_structure_get_string(info, "profile");
                    level = gst_structure_get_string(info, "level");
                    /* Data from http://archive.is/whp6P#45% */
                    if (profile && level)
                    {
                        if (!(strcmp(profile, "lc")) && !(strcmp(level, "2")))
                            profile_level_indication = 0x29;
                        else if (!(strcmp(profile, "lc")) && !(strcmp(level, "4")))
                            profile_level_indication = 0x2A;
                        else if (!(strcmp(profile, "lc")) && !(strcmp(level, "5")))
                            profile_level_indication = 0x2B;
                        else
                            FIXME("Unhandled profile/level combo\n");
                    }
                    else
                        ERR("Profile or level not present\n");

                    if (profile_level_indication)
                    {
                        IMFMediaType_SetUINT32(media_type, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, profile_level_indication);
                        user_data->profile_level_indication = profile_level_indication;
                    }

                    IMFMediaType_SetBlob(media_type, &MF_MT_USER_DATA, (BYTE *)user_data, sizeof(user_data) + asc_size);
                    heap_free(user_data);
                    break;
                }
                default:
                    FIXME("Unhandled mpegversion %d\n", mpeg_version);
            }
        }
        else
            FIXME("Unrecognized audio format %s\n", mime_type);
    }
    else
    {
        IMFMediaType_Release(media_type);
        return NULL;
    }

    return media_type;
}

/* IMFSample = GstBuffer
   IMFBuffer = GstMemory */

/* TODO: Future optimization could be to create a custom
   IMFMediaBuffer wrapper around GstMemory, and to utilize
   gst_memory_new_wrapped on IMFMediaBuffer data.  However,
   this wouldn't work if we allow the callers to allocate
   the buffers. */

IMFSample* mf_sample_from_gst_buffer(GstBuffer *gst_buffer)
{
    IMFSample *out = NULL;
    LONGLONG duration, time;
    int buffer_count;
    HRESULT hr;

    if (FAILED(hr = MFCreateSample(&out)))
        goto fail;

    duration = GST_BUFFER_DURATION(gst_buffer);
    time = GST_BUFFER_PTS(gst_buffer);

    if (FAILED(IMFSample_SetSampleDuration(out, duration / 100)))
        goto fail;

    if (FAILED(IMFSample_SetSampleTime(out, time / 100)))
        goto fail;

    buffer_count = gst_buffer_n_memory(gst_buffer);

    for (unsigned int i = 0; i < buffer_count; i++)
    {
        GstMemory *memory = gst_buffer_get_memory(gst_buffer, i);
        IMFMediaBuffer *mf_buffer = NULL;
        GstMapInfo map_info;
        BYTE *buf_data;

        if (!memory)
        {
            hr = E_FAIL;
            goto loop_done;
        }

        if (!(gst_memory_map(memory, &map_info, GST_MAP_READ)))
        {
            hr = E_FAIL;
            goto loop_done;
        }

        if (FAILED(hr = MFCreateMemoryBuffer(map_info.maxsize, &mf_buffer)))
        {
            gst_memory_unmap(memory, &map_info);
            goto loop_done;
        }

        if (FAILED(hr = IMFMediaBuffer_Lock(mf_buffer, &buf_data, NULL, NULL)))
        {
            gst_memory_unmap(memory, &map_info);
            goto loop_done;
        }

        memcpy(buf_data, map_info.data, map_info.size);

        gst_memory_unmap(memory, &map_info);

        if (FAILED(hr = IMFMediaBuffer_Unlock(mf_buffer)))
            goto loop_done;

        if (FAILED(hr = IMFMediaBuffer_SetCurrentLength(mf_buffer, map_info.size)))
            goto loop_done;

        if (FAILED(hr = IMFSample_AddBuffer(out, mf_buffer)))
            goto loop_done;

        loop_done:
        if (mf_buffer)
            IMFMediaBuffer_Release(mf_buffer);
        if (memory)
            gst_memory_unref(memory);
        if (FAILED(hr))
            goto fail;
    }

    return out;
    fail:
    ERR("Failed to copy IMFSample to GstBuffer, hr = %#x\n", hr);
    IMFSample_Release(out);
    return NULL;
}
