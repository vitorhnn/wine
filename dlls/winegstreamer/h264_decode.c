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

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

struct h264_decoder
{
    IMFTransform IMFTransform_iface;
};

HRESULT h264_decoder_construct(REFIID riid, void **obj)
{
    TRACE("%s, %p.\n", debugstr_guid(riid), obj);

    return S_OK;
}