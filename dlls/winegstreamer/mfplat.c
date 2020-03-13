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

#define COBJMACROS
#define NONAMELESSUNION

#include "mfapi.h"
#include "codecapi.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#define defer_merge(a,b) a##b
#define defer_cleanup(a) defer_merge(defer_cleanup_, a)
#define defer_scopevar(a) defer_merge(defer_scopevar_, a)
#define defer auto void defer_cleanup(__LINE__)(void**); __attribute__((cleanup(defer_cleanup(__LINE__)))) void* defer_scopevar(__LINE__) = 0; void defer_cleanup(__LINE__)(void** unused_param_deadbeef)

static LONG object_locks;

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

static HRESULT h264_decoder_create(REFIID riid, void **ret)
{
    return generic_decoder_construct(riid, ret, DECODER_TYPE_H264);
}

static HRESULT aac_decoder_create(REFIID riid, void **ret)
{
    return generic_decoder_construct(riid, ret, DECODER_TYPE_AAC);
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
    { &CLSID_CMSH264DecoderMFT, &h264_decoder_create },
    { &CLSID_CMSAACDecMFT, &aac_decoder_create },
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

HRESULT mfplat_can_unload_now(void)
{
    return !object_locks ? S_OK : S_FALSE;
}

struct register_type_info
{
    const GUID *major_type;
    const GUID *sub_type;
};

static WCHAR h264decoderW[] = {'H','.','2','6','4',' ','D','e','c','o','d','e','r',0};
const struct register_type_info h264_decoder_input_types[] =
{
    {
        &MFMediaType_Video,
        &MFVideoFormat_H264
    }
};
const struct register_type_info h264_decoder_output_types[] =
{
    {
        &MFMediaType_Video,
        &MFVideoFormat_I420
    },
    {
        &MFMediaType_Video,
        &MFVideoFormat_IYUV
    },
    {
        &MFMediaType_Video,
        &MFVideoFormat_NV12
    },
    {
        &MFMediaType_Video,
        &MFVideoFormat_YUY2,
    },
    {
        &MFMediaType_Video,
        &MFVideoFormat_YV12,
    }
};

static WCHAR aacdecoderW[] = {'A','A','C',' ','D','e','c','o','d','e','r',0};
const struct register_type_info aac_decoder_input_types[] =
{
    {
        &MFMediaType_Audio,
        &MFAudioFormat_AAC
    }
};

const struct register_type_info aac_decoder_output_types[] =
{
    {
        &MFMediaType_Audio,
        &MFAudioFormat_Float
    }
};

static const struct mft
{
    const GUID *clsid;
    const GUID *category;
    LPWSTR name;
    const UINT32 flags;
    const UINT32 input_types_count;
    const struct register_type_info *input_types;
    const UINT32 output_types_count;
    const struct register_type_info *output_types;
    IMFAttributes *attributes;
}
mfts[] =
{
    {
        &CLSID_CMSH264DecoderMFT,
        &MFT_CATEGORY_VIDEO_DECODER,
        h264decoderW,
        MFT_ENUM_FLAG_SYNCMFT,
        ARRAY_SIZE(h264_decoder_input_types),
        h264_decoder_input_types,
        ARRAY_SIZE(h264_decoder_output_types),
        h264_decoder_output_types,
        NULL
    },
    {
        &CLSID_CMSAACDecMFT,
        &MFT_CATEGORY_AUDIO_DECODER,
        aacdecoderW,
        MFT_ENUM_FLAG_SYNCMFT,
        ARRAY_SIZE(aac_decoder_input_types),
        aac_decoder_input_types,
        ARRAY_SIZE(aac_decoder_output_types),
        aac_decoder_output_types,
        NULL
    }
};

HRESULT mfplat_DllRegisterServer(void)
{
    HRESULT hr;

    for (unsigned int i = 0; i < ARRAY_SIZE(mfts); i++)
    {
        const struct mft *cur = &mfts[i];

        MFT_REGISTER_TYPE_INFO *input_types, *output_types;
        input_types = heap_alloc(cur->input_types_count * sizeof(input_types[0]));
        output_types = heap_alloc(cur->output_types_count * sizeof(output_types[0]));
        for (unsigned int i = 0; i < cur->input_types_count; i++)
        {
            input_types[i].guidMajorType = *(cur->input_types[i].major_type);
            input_types[i].guidSubtype = *(cur->input_types[i].sub_type);
        }
        for (unsigned int i = 0; i < cur->output_types_count; i++)
        {
            output_types[i].guidMajorType = *(cur->output_types[i].major_type);
            output_types[i].guidSubtype = *(cur->output_types[i].sub_type);
        }

        hr = MFTRegister(*(cur->clsid), *(cur->category), cur->name, cur->flags, cur->input_types_count,
                    input_types, cur->output_types_count, output_types, cur->attributes);

        heap_free(input_types);
        heap_free(output_types);

        if (FAILED(hr))
        {
            FIXME("Failed to register MFT, hr %#x\n", hr);
            return hr;
        }
    }
    return S_OK;
}

struct aac_user_data
{
    WORD payload_type;
    WORD profile_level_indication;
    WORD struct_type;
    WORD reserved;
    //BYTE audio_specific_config;
};

/* IMPORTANT: caps will be modified to represent the exact type needed for the format */
IMFMediaType* mfplat_media_type_from_caps(GstCaps *caps)
{
    IMFMediaType *media_type;
    GstStructure *info;
    const char *media_type_name;
    gchar *human_readable;

    if (FAILED(MFCreateMediaType(&media_type)))
    {
        return NULL;
    }

    info = gst_caps_get_structure(caps, 0);
    media_type_name = gst_structure_get_name(info);

    human_readable = gst_caps_to_string(caps);
    TRACE("caps = %s\n", human_readable);
    g_free(human_readable);

    if (!(strncmp(media_type_name, "video", 5)))
    {
        const char *video_format = media_type_name + 6;
        gint width, height, framerate_num, framerate_den;

        if (gst_structure_get_int(info, "width", &width) && gst_structure_get_int(info, "height", &height))
        {
            IMFMediaType_SetUINT64(media_type, &MF_MT_FRAME_SIZE, ((UINT64)width << 32) | height);
        }
        if (gst_structure_get_fraction(info, "framerate", &framerate_num, &framerate_den))
        {
            IMFMediaType_SetUINT64(media_type, &MF_MT_FRAME_RATE, ((UINT64)framerate_num << 32) | framerate_den);
        }

        IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        if (!(strcmp(video_format, "x-h264")))
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
                    ERR("Unrecognized profile %s\n", profile);
            }
            if ((level = gst_structure_get_string(info, "level")))
            {
                if (!(strcmp(level, "1")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_LEVEL, eAVEncH264VLevel1);
                else if (!(strcmp(level, "1.3")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_LEVEL, eAVEncH264VLevel1_3);
                else if (!(strcmp(level, "4")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_LEVEL, eAVEncH264VLevel4);
                else if (!(strcmp(level, "4.1")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_LEVEL, eAVEncH264VLevel4_1);
                else if (!(strcmp(level, "4.2")))
                    IMFMediaType_SetUINT32(media_type, &MF_MT_MPEG2_LEVEL, eAVEncH264VLevel4_2);
                else
                    ERR("Unrecognized level %s\n", level);
            }
            gst_caps_set_simple(caps, "stream-format", G_TYPE_STRING, "byte-stream", NULL);
            gst_caps_set_simple(caps, "alignment", G_TYPE_STRING, "au", NULL);
            for (unsigned int i = 0; i < gst_caps_get_size(caps); i++)
            {
                GstStructure *structure = gst_caps_get_structure (caps, i);
                gst_structure_remove_field(structure, "codec_data");
            }
        }
        else if (!(strcmp(video_format, "mpeg")))
        {
            IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_M4S2);
            IMFMediaType_SetUINT32(media_type, &MF_MT_COMPRESSED, TRUE);
        }
        else if (!(strcmp(video_format, "x-raw")))
        {
            const char *uncompressed_format = gst_structure_get_string(info, "stream-format");
            IMFMediaType_SetUINT32(media_type, &MF_MT_COMPRESSED, FALSE);
            if (uncompressed_format)
            {
                if (!(strcmp(uncompressed_format, "NV12")))
                    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
                else if (!(strcmp(uncompressed_format, "YV12")))
                    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFVideoFormat_YV12);
                else
                    ERR("Unrecognized stream format %s\n", uncompressed_format);
            }
            else
                ERR("uncompressed video has no stream-format\n");
        }
        else
            ERR("Unrecognized video format %s\n", video_format);
    }
    else if (!(strncmp(media_type_name, "audio", 5)))
    {
        const char *audio_format = media_type_name + 6;

        IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        if (!(strcmp(audio_format, "mpeg")))
        {
            int mpeg_version = -1;

            IMFMediaType_SetUINT32(media_type, &MF_MT_COMPRESSED, TRUE);

            if (!(gst_structure_get_int(info, "mpegversion", &mpeg_version)))
                ERR("Failed to get mpegversion\n");
            switch (mpeg_version)
            {
                case 1:
                {
                    IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFAudioFormat_MPEG);
                    break;
                }
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
                        else
                            ERR("Unrecognized stream-format\n");
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
                    /* Data from https://docs.microsoft.com/en-us/windows/win32/medfound/aac-encoder#output-types */
                    if (profile && level)
                    {
                        if (!(strcmp(profile, "lc")) && !(strcmp(level, "2")))
                            profile_level_indication = 0x29;
                        else if (!(strcmp(profile, "lc")) && !(strcmp(level, "4")))
                            profile_level_indication = 0x2A;
                        else if (!(strcmp(profile, "lc")) && !(strcmp(level, "5")))
                            profile_level_indication = 0x2B;
                        else
                            ERR("Unhandled profile/level combo\n");
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
                    ERR("Unhandled mpegversion %d\n", mpeg_version);
            }
        }
        else if (!(strcmp(audio_format, "x-raw")))
        {
            IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, &MFAudioFormat_Float);

            gst_caps_set_simple(caps, "format", G_TYPE_STRING, "F32LE", NULL);
        }
        else
            ERR("Unrecognized audio format %s\n", audio_format);
    }
    else
    {
        goto fail;
    }

    return media_type;
    fail:
    IMFMediaType_Release(media_type);
    return NULL;
}

GstCaps *caps_from_media_type(IMFMediaType *type)
{
    GUID major_type;
    GUID subtype;
    GstCaps *output = NULL;

    if (FAILED(IMFMediaType_GetMajorType(type, &major_type)))
        return NULL;
    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return NULL;

    if (IsEqualGUID(&major_type, &MFMediaType_Video))
    {
        const char *gst_type = NULL, *format = NULL, *stream_format = NULL, *alignment = NULL, *profile = NULL;
        const char *level = NULL;
        UINT64 frame_rate = 0, frame_size = 0;
        DWORD *framerate_num = ((DWORD*)&frame_rate) + 1;
        DWORD *framerate_den = ((DWORD*)&frame_rate);
        DWORD *width = ((DWORD*)&frame_size) + 1;
        DWORD *height = ((DWORD*)&frame_size);

        IMFMediaType_GetUINT64(type, &MF_MT_FRAME_RATE, &frame_rate);
        IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size);

        if (IsEqualGUID(&subtype, &MFVideoFormat_H264))
        {
            gst_type = "video/x-h264";
            stream_format = "byte-stream";
            alignment = "au";
            enum eAVEncH264VProfile h264_profile;
            enum eAVEncH264VLevel h264_level;

            if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_MPEG2_PROFILE, &h264_profile)))
            {
                switch (h264_profile)
                {
                    case eAVEncH264VProfile_Main:
                        profile = "main";
                        break;
                    case eAVEncH264VProfile_High:
                        profile = "high";
                        break;
                    case eAVEncH264VProfile_444:
                        profile = "high-4:4:4";
                        break;
                    default:
                        ERR("Unknown profile %u\n", h264_profile);
                }
            }
            if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_MPEG2_LEVEL, &h264_level)))
            {
                switch (h264_level)
                {
                    case eAVEncH264VLevel1:
                        level = "1";
                        break;
                    case eAVEncH264VLevel1_3:
                        level = "1.3";
                        break;
                    case eAVEncH264VLevel4:
                        level = "4";
                        break;
                    case eAVEncH264VLevel4_1:
                        level = "4.1";
                        break;
                    case eAVEncH264VLevel4_2:
                        level = "4.2";
                        break;
                    default:
                        ERR("Unknown level %u\n", h264_level);
                }
            }
        } else
        if (IsEqualGUID(&subtype, &MFVideoFormat_NV12))
        {
            gst_type = "video/x-raw";
            format = "NV12";
        }
        else if (IsEqualGUID(&subtype, &MFVideoFormat_YV12))
        {
            gst_type = "video/x-raw";
            format = "YV12";
        }
        else {
            ERR("Unrecognized subtype %s\n", debugstr_guid(&subtype));
            return NULL;
        }

        output = gst_caps_new_empty_simple(gst_type);
        if (format)
            gst_caps_set_simple(output, "format", G_TYPE_STRING, format, NULL);
        if (stream_format)
            gst_caps_set_simple(output, "stream-format", G_TYPE_STRING, stream_format, NULL);
        if (alignment)
            gst_caps_set_simple(output, "alignment", G_TYPE_STRING, alignment, NULL);
        if (frame_rate)
            gst_caps_set_simple(output, "framerate", GST_TYPE_FRACTION, *framerate_num, *framerate_den, NULL);
        if (frame_size)
        {
            gst_caps_set_simple(output, "width", G_TYPE_INT, *width, NULL);
            gst_caps_set_simple(output, "height", G_TYPE_INT, *height, NULL);
        }
        if (profile)
            gst_caps_set_simple(output, "profile", G_TYPE_STRING, profile, NULL);
        if (level)
            gst_caps_set_simple(output, "level", G_TYPE_STRING, level, NULL);
        return output;
    }
    else if (IsEqualGUID(&major_type, &MFMediaType_Audio))
    {
        DWORD rate, channels;

        if (IsEqualGUID(&subtype, &MFAudioFormat_AAC))
        {
            output = gst_caps_new_empty_simple("audio/mpeg");
            DWORD payload_type, indication;
            struct aac_user_data *user_data;
            UINT32 user_data_size;

            /* TODO */
            gst_caps_set_simple(output, "framed", G_TYPE_BOOLEAN, TRUE, NULL);
            gst_caps_set_simple(output, "mpegversion", G_TYPE_INT, 4, NULL);

            if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_AAC_PAYLOAD_TYPE, &payload_type)))
            {
                switch (payload_type)
                {
                    case 0:
                        gst_caps_set_simple(output, "stream-format", G_TYPE_STRING, "raw", NULL);
                        break;
                    case 1:
                        gst_caps_set_simple(output, "stream-format", G_TYPE_STRING, "adts", NULL);
                        break;
                    default:
                        gst_caps_set_simple(output, "stream-format", G_TYPE_STRING, "raw", NULL);
                }
            }
            else
                gst_caps_set_simple(output, "stream-format", G_TYPE_STRING, "raw", NULL);

            if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, &indication)))
            {
                switch (indication)
                {
                    case 0x29:
                    {
                        gst_caps_set_simple(output, "profile", G_TYPE_STRING, "lc", NULL);
                        gst_caps_set_simple(output, "level", G_TYPE_STRING, "2", NULL);
                        break;
                    }
                    case 0x2A:
                    {
                        gst_caps_set_simple(output, "profile", G_TYPE_STRING, "lc", NULL);
                        gst_caps_set_simple(output, "level", G_TYPE_STRING, "4", NULL);
                        break;
                    }
                    case 0x2B:
                    {
                        gst_caps_set_simple(output, "profile", G_TYPE_STRING, "lc", NULL);
                        gst_caps_set_simple(output, "level", G_TYPE_STRING, "5", NULL);
                        break;
                    }
                    default:
                        ERR("Unrecognized profile-level-indication %u\n", indication);
                }
            }

            if (SUCCEEDED(IMFMediaType_GetAllocatedBlob(type, &MF_MT_USER_DATA, (BYTE **) &user_data, &user_data_size)))
            {
                if (user_data_size > sizeof(sizeof(*user_data)))
                {
                    GstBuffer *audio_specific_config = gst_buffer_new_allocate(NULL, user_data_size - sizeof(*user_data), NULL);
                    gst_buffer_fill(audio_specific_config, 0, user_data + 1, user_data_size - sizeof(*user_data));

                    gst_caps_set_simple(output, "codec_data", GST_TYPE_BUFFER, audio_specific_config, NULL);
                    gst_buffer_unref(audio_specific_config);
                }
                CoTaskMemFree(user_data);
            }
        }
        else if (IsEqualGUID(&subtype, &MFAudioFormat_Float))
        {
            output = gst_caps_new_empty_simple("audio/x-raw");

            gst_caps_set_simple(output, "format", G_TYPE_STRING, "F32LE", NULL);
        }
        else
        {
            ERR("Unrecognized subtype %s\n", debugstr_guid(&subtype));
            if (output)
                gst_caps_unref(output);
            return NULL;
        }
        if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)))
        {
            gst_caps_set_simple(output, "rate", G_TYPE_INT, rate, NULL);
        }
        if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &channels)))
        {
            gst_caps_set_simple(output, "channels", G_TYPE_INT, channels, NULL);
        }

        return output;
    }

    ERR("Unrecognized major type %s\n", debugstr_guid(&major_type));
    return NULL;
}

static DWORD get_type_fourcc(IMFMediaType *type)
{
    GUID subtype, template;

    if (FAILED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return 0;

    template = MFVideoFormat_I420;
    template.Data1 = subtype.Data1;

    if (IsEqualGUID(&subtype, &template))
        return template.Data1;
    return 0;
}

/* IMFSample = GstBuffer
   IMFBuffer = GstMemory */

/* TODO: Future optimization will be to create a custom
   IMFMediaBuffer wrapper around GstMemory, and to utilize
   gst_memory_new_wrapped on IMFMediaBuffer data */

IMFSample* mf_sample_from_gst_buffer(GstBuffer *gst_buffer, IMFMediaType *buffer_type)
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
        GstMemory *memory = gst_buffer_get_memory(gst_buffer, i);;
        GstMapInfo map_info;
        IMFMediaBuffer *mf_buffer = NULL;
        BYTE *buf_data;

        defer {gst_memory_unref(memory);}

        if (!(gst_memory_map(memory, &map_info, GST_MAP_READ)))
        {
            ERR("Failed to map memory from GstBuffer\n");
            hr = ERROR_INTERNAL_ERROR;
            goto fail;
        }
        defer {gst_memory_unmap(memory, &map_info);}

        /*if (buffer_type)
        {
            DWORD format;
            if ((format = get_type_fourcc(buffer_type)))
            {
                UINT64 frame_size;
                if (SUCCEEDED(IMFMediaType_GetUINT64(buffer_type, &MF_MT_FRAME_SIZE, &frame_size)))
                {
                    MFCreate2DMediaBuffer((DWORD) (frame_size >> 32), (DWORD) (frame_size & 0xffffffff), format, FALSE, &mf_buffer);
                }
            }
        }*/
        if (!mf_buffer)
            if (FAILED(hr = MFCreateMemoryBuffer(map_info.maxsize, &mf_buffer)))
                goto fail;
        defer {IMFMediaBuffer_Release(mf_buffer);}

        if (FAILED(hr = IMFMediaBuffer_Lock(mf_buffer, &buf_data, NULL, NULL)))
            goto fail;

        memcpy(buf_data, map_info.data, map_info.size);

        if (FAILED(hr = IMFMediaBuffer_Unlock(mf_buffer)))
            goto fail;

        if (FAILED(hr = IMFMediaBuffer_SetCurrentLength(mf_buffer, map_info.size)))
            goto fail;

        if (FAILED(hr = IMFSample_AddBuffer(out, mf_buffer)))
            goto fail;
    }

    return out;
    fail:
    ERR("Failed to copy IMFSample to GstBuffer, hr = %#x\n", hr);
    IMFSample_Release(out);
    return NULL;
}

GstBuffer* gst_buffer_from_mf_sample(IMFSample *mf_sample)
{
    GstBuffer *out = gst_buffer_new();
    LONGLONG duration, time;
    DWORD buffer_count;
    HRESULT hr;

    if (FAILED(hr = IMFSample_GetSampleDuration(mf_sample, &duration)))
        goto fail;

    if (FAILED(hr = IMFSample_GetSampleTime(mf_sample, &time)))
        goto fail;

    GST_BUFFER_DURATION(out) = duration;
    GST_BUFFER_PTS(out) = time * 100;

    if (FAILED(hr = IMFSample_GetBufferCount(mf_sample, &buffer_count)))
        goto fail;

    for (unsigned int i = 0; i < buffer_count; i++)
    {
        IMFMediaBuffer *mf_buffer;
        DWORD buffer_max_size, buffer_size;
        GstMemory *memory;
        GstMapInfo map_info;
        BYTE *buf_data;

        if (FAILED(hr = IMFSample_GetBufferByIndex(mf_sample, i, &mf_buffer)))
            goto fail;
        defer {IMFMediaBuffer_Release(mf_buffer);}

        if (FAILED(hr = IMFMediaBuffer_GetMaxLength(mf_buffer, &buffer_max_size)))
            goto fail;

        if (FAILED(hr = IMFMediaBuffer_GetCurrentLength(mf_buffer, &buffer_size)))
            goto fail;

        memory = gst_allocator_alloc(NULL, buffer_size, NULL);
        gst_memory_resize(memory, 0, buffer_size);

        if (!(gst_memory_map(memory, &map_info, GST_MAP_WRITE)))
        {
            ERR("Failed to map memory from GstBuffer\n");
            hr = ERROR_INTERNAL_ERROR;
            goto fail;
        }

        if (FAILED(hr = IMFMediaBuffer_Lock(mf_buffer, &buf_data, NULL, NULL)))
            goto fail;

        memcpy(map_info.data, buf_data, buffer_size);

        if (FAILED(hr = IMFMediaBuffer_Unlock(mf_buffer)))
            goto fail;

        if (FAILED(hr = IMFMediaBuffer_SetCurrentLength(mf_buffer, buffer_size)))
            goto fail;

        gst_memory_unmap(memory, &map_info);

        gst_buffer_append_memory(out, memory);
    }

    return out;

    fail:
    ERR("Failed to copy IMFSample to GstBuffer, hr = %#x\n", hr);
    gst_buffer_unref(out);
    return NULL;
}