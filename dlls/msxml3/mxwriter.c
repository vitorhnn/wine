/*
 *    MXWriter implementation
 *
 * Copyright 2011-2014 Nikolay Sivov for CodeWeavers
 * Copyright 2011 Thomas Mullaly
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
#include "config.h"

#include <stdarg.h>
#ifdef HAVE_LIBXML2
# include <libxml/parser.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "ole2.h"

#include "msxml6.h"

#include "wine/debug.h"

#include "msxml_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(msxml);

static const WCHAR emptyW[] = {0};
static const WCHAR spaceW[] = {' '};
static const WCHAR quotW[]  = {'\"'};
static const WCHAR closetagW[] = {'>','\r','\n'};
static const WCHAR crlfW[] = {'\r','\n'};
static const WCHAR entityW[] = {'<','!','E','N','T','I','T','Y',' '};

/* should be ordered as encoding names are sorted */
typedef enum
{
    XmlEncoding_ISO_8859_1 = 0,
    XmlEncoding_ISO_8859_13,
    XmlEncoding_ISO_8859_15,
    XmlEncoding_ISO_8859_2,
    XmlEncoding_ISO_8859_3,
    XmlEncoding_ISO_8859_4,
    XmlEncoding_ISO_8859_5,
    XmlEncoding_ISO_8859_7,
    XmlEncoding_ISO_8859_9,
    XmlEncoding_UTF16,
    XmlEncoding_UTF8,
    XmlEncoding_Unknown
} xml_encoding;

struct xml_encoding_data
{
    const WCHAR *encoding;
    xml_encoding enc;
    UINT cp;
};

static const WCHAR iso_8859_1W[] = {'i','s','o','-','8','8','5','9','-','1',0};
static const WCHAR iso_8859_2W[] = {'i','s','o','-','8','8','5','9','-','2',0};
static const WCHAR iso_8859_3W[] = {'i','s','o','-','8','8','5','9','-','3',0};
static const WCHAR iso_8859_4W[] = {'i','s','o','-','8','8','5','9','-','4',0};
static const WCHAR iso_8859_5W[] = {'i','s','o','-','8','8','5','9','-','5',0};
static const WCHAR iso_8859_7W[] = {'i','s','o','-','8','8','5','9','-','7',0};
static const WCHAR iso_8859_9W[] = {'i','s','o','-','8','8','5','9','-','9',0};
static const WCHAR iso_8859_13W[] = {'i','s','o','-','8','8','5','9','-','1','3',0};
static const WCHAR iso_8859_15W[] = {'i','s','o','-','8','8','5','9','-','1','5',0};
static const WCHAR utf16W[] = {'U','T','F','-','1','6',0};
static const WCHAR utf8W[] = {'U','T','F','-','8',0};

static const struct xml_encoding_data xml_encoding_map[] = {
    { iso_8859_1W,  XmlEncoding_ISO_8859_1,  28591 },
    { iso_8859_13W, XmlEncoding_ISO_8859_13, 28603 },
    { iso_8859_15W, XmlEncoding_ISO_8859_15, 28605 },
    { iso_8859_2W,  XmlEncoding_ISO_8859_2,  28592 },
    { iso_8859_3W,  XmlEncoding_ISO_8859_3,  28593 },
    { iso_8859_4W,  XmlEncoding_ISO_8859_4,  28594 },
    { iso_8859_5W,  XmlEncoding_ISO_8859_5,  28595 },
    { iso_8859_7W,  XmlEncoding_ISO_8859_7,  28597 },
    { iso_8859_9W,  XmlEncoding_ISO_8859_9,  28599 },
    { utf16W,       XmlEncoding_UTF16,          ~0 },
    { utf8W,        XmlEncoding_UTF8,      CP_UTF8 }
};

typedef enum
{
    OutputBuffer_Native  = 0x001,
    OutputBuffer_Encoded = 0x010,
    OutputBuffer_Both    = 0x100
} output_mode;

typedef enum
{
    MXWriter_BOM = 0,
    MXWriter_DisableEscaping,
    MXWriter_Indent,
    MXWriter_OmitXmlDecl,
    MXWriter_Standalone,
    MXWriter_LastProp
} mxwriter_prop;

typedef enum
{
    EscapeValue,
    EscapeText
} escape_mode;

typedef struct
{
    char *data;
    unsigned int allocated;
    unsigned int written;
} encoded_buffer;

typedef struct
{
    encoded_buffer utf16;
    encoded_buffer encoded;
    UINT code_page;
} output_buffer;

typedef struct
{
    DispatchEx dispex;
    IMXWriter IMXWriter_iface;
    ISAXContentHandler ISAXContentHandler_iface;
    ISAXLexicalHandler ISAXLexicalHandler_iface;
    ISAXDeclHandler    ISAXDeclHandler_iface;
    ISAXDTDHandler     ISAXDTDHandler_iface;
    ISAXErrorHandler   ISAXErrorHandler_iface;
    IVBSAXDeclHandler  IVBSAXDeclHandler_iface;
    IVBSAXLexicalHandler IVBSAXLexicalHandler_iface;
    IVBSAXContentHandler IVBSAXContentHandler_iface;
    IVBSAXDTDHandler     IVBSAXDTDHandler_iface;
    IVBSAXErrorHandler   IVBSAXErrorHandler_iface;

    LONG ref;
    MSXML_VERSION class_version;

    VARIANT_BOOL props[MXWriter_LastProp];
    BOOL prop_changed;
    BOOL cdata;

    BOOL text; /* last node was text node, so we shouldn't indent next node */
    BOOL newline; /* newline was already added as a part of previous call */
    UINT indent; /* indentation level for next node */

    BSTR version;

    BSTR encoding; /* exact property value */
    xml_encoding xml_enc;

    /* contains a pending (or not closed yet) element name or NULL if
       we don't have to close */
    BSTR element;

    IStream *dest;
    ULONG dest_written;

    output_buffer *buffer;
} mxwriter;

typedef struct
{
    BSTR qname;
    BSTR local;
    BSTR uri;
    BSTR type;
    BSTR value;
} mxattribute;

typedef struct
{
    DispatchEx dispex;
    IMXAttributes IMXAttributes_iface;
    ISAXAttributes ISAXAttributes_iface;
    IVBSAXAttributes IVBSAXAttributes_iface;
    LONG ref;

    MSXML_VERSION class_version;

    mxattribute *attr;
    int length;
    int allocated;
} mxattributes;

static inline mxattributes *impl_from_IMXAttributes( IMXAttributes *iface )
{
    return CONTAINING_RECORD(iface, mxattributes, IMXAttributes_iface);
}

static inline mxattributes *impl_from_ISAXAttributes( ISAXAttributes *iface )
{
    return CONTAINING_RECORD(iface, mxattributes, ISAXAttributes_iface);
}

static inline mxattributes *impl_from_IVBSAXAttributes( IVBSAXAttributes *iface )
{
    return CONTAINING_RECORD(iface, mxattributes, IVBSAXAttributes_iface);
}

static HRESULT mxattributes_grow(mxattributes *This)
{
    if (This->length < This->allocated) return S_OK;

    This->allocated *= 2;
    This->attr = heap_realloc(This->attr, This->allocated*sizeof(mxattribute));

    return This->attr ? S_OK : E_OUTOFMEMORY;
}

static xml_encoding parse_encoding_name(const WCHAR *encoding)
{
    int min, max, n, c;

    min = 0;
    max = sizeof(xml_encoding_map)/sizeof(struct xml_encoding_data) - 1;

    while (min <= max)
    {
        n = (min+max)/2;

        c = strcmpiW(xml_encoding_map[n].encoding, encoding);
        if (!c)
            return xml_encoding_map[n].enc;

        if (c > 0)
            max = n-1;
        else
            min = n+1;
    }

    return XmlEncoding_Unknown;
}

static HRESULT init_encoded_buffer(encoded_buffer *buffer)
{
    const int initial_len = 0x2000;
    buffer->data = heap_alloc(initial_len);
    if (!buffer->data) return E_OUTOFMEMORY;

    memset(buffer->data, 0, 4);
    buffer->allocated = initial_len;
    buffer->written = 0;

    return S_OK;
}

static void free_encoded_buffer(encoded_buffer *buffer)
{
    heap_free(buffer->data);
}

static HRESULT get_code_page(xml_encoding encoding, UINT *cp)
{
    const struct xml_encoding_data *data;

    if (encoding == XmlEncoding_Unknown)
    {
        FIXME("unsupported encoding %d\n", encoding);
        return E_NOTIMPL;
    }

    data = &xml_encoding_map[encoding];
    *cp = data->cp;

    return S_OK;
}

static HRESULT alloc_output_buffer(xml_encoding encoding, output_buffer **buffer)
{
    output_buffer *ret;
    HRESULT hr;

    ret = heap_alloc(sizeof(*ret));
    if (!ret) return E_OUTOFMEMORY;

    hr = get_code_page(encoding, &ret->code_page);
    if (hr != S_OK) {
        heap_free(ret);
        return hr;
    }

    hr = init_encoded_buffer(&ret->utf16);
    if (hr != S_OK) {
        heap_free(ret);
        return hr;
    }

    /* currently we always create a default output buffer that is UTF-16 only,
       but it's possible to allocate with specific encoding too */
    if (encoding != XmlEncoding_UTF16) {
        hr = init_encoded_buffer(&ret->encoded);
        if (hr != S_OK) {
            free_encoded_buffer(&ret->utf16);
            heap_free(ret);
            return hr;
        }
    }
    else
        memset(&ret->encoded, 0, sizeof(ret->encoded));

    *buffer = ret;

    return S_OK;
}

static void free_output_buffer(output_buffer *buffer)
{
    free_encoded_buffer(&buffer->encoded);
    free_encoded_buffer(&buffer->utf16);
    heap_free(buffer);
}

static void grow_buffer(encoded_buffer *buffer, int length)
{
    /* grow if needed, plus 4 bytes to be sure null terminator will fit in */
    if (buffer->allocated < buffer->written + length + 4)
    {
        int grown_size = max(2*buffer->allocated, buffer->allocated + length);
        buffer->data = heap_realloc(buffer->data, grown_size);
        buffer->allocated = grown_size;
    }
}

static HRESULT write_output_buffer_mode(output_buffer *buffer, output_mode mode, const WCHAR *data, int len)
{
    int length;
    char *ptr;

    if (mode & (OutputBuffer_Encoded | OutputBuffer_Both)) {
        if (buffer->code_page != ~0)
        {
            length = WideCharToMultiByte(buffer->code_page, 0, data, len, NULL, 0, NULL, NULL);
            grow_buffer(&buffer->encoded, length);
            ptr = buffer->encoded.data + buffer->encoded.written;
            length = WideCharToMultiByte(buffer->code_page, 0, data, len, ptr, length, NULL, NULL);
            buffer->encoded.written += len == -1 ? length-1 : length;
        }
    }

    if (mode & (OutputBuffer_Native | OutputBuffer_Both)) {
        /* WCHAR data just copied */
        length = len == -1 ? strlenW(data) : len;
        if (length)
        {
            length *= sizeof(WCHAR);

            grow_buffer(&buffer->utf16, length);
            ptr = buffer->utf16.data + buffer->utf16.written;

            memcpy(ptr, data, length);
            buffer->utf16.written += length;
            ptr += length;
            /* null termination */
            memset(ptr, 0, sizeof(WCHAR));
        }
    }

    return S_OK;
}

static HRESULT write_output_buffer(output_buffer *buffer, const WCHAR *data, int len)
{
    return write_output_buffer_mode(buffer, OutputBuffer_Both, data, len);
}

static HRESULT write_output_buffer_quoted(output_buffer *buffer, const WCHAR *data, int len)
{
    write_output_buffer(buffer, quotW, 1);
    write_output_buffer(buffer, data, len);
    write_output_buffer(buffer, quotW, 1);

    return S_OK;
}

/* frees buffer data, reallocates with a default lengths */
static void close_output_buffer(mxwriter *This)
{
    heap_free(This->buffer->utf16.data);
    heap_free(This->buffer->encoded.data);
    init_encoded_buffer(&This->buffer->utf16);
    init_encoded_buffer(&This->buffer->encoded);
    get_code_page(This->xml_enc, &This->buffer->code_page);
}

/* escapes special characters like:
   '<' -> "&lt;"
   '&' -> "&amp;"
   '"' -> "&quot;"
   '>' -> "&gt;"
*/
static WCHAR *get_escaped_string(const WCHAR *str, escape_mode mode, int *len)
{
    static const WCHAR ltW[]    = {'&','l','t',';'};
    static const WCHAR ampW[]   = {'&','a','m','p',';'};
    static const WCHAR equotW[] = {'&','q','u','o','t',';'};
    static const WCHAR gtW[]    = {'&','g','t',';'};

    const int default_alloc = 100;
    const int grow_thresh = 10;
    int p = *len, conv_len;
    WCHAR *ptr, *ret;

    /* default buffer size to something if length is unknown */
    conv_len = *len == -1 ? default_alloc : max(2**len, default_alloc);
    ptr = ret = heap_alloc(conv_len*sizeof(WCHAR));

    while (*str && p)
    {
        if (ptr - ret > conv_len - grow_thresh)
        {
            int written = ptr - ret;
            conv_len *= 2;
            ptr = ret = heap_realloc(ret, conv_len*sizeof(WCHAR));
            ptr += written;
        }

        switch (*str)
        {
        case '<':
            memcpy(ptr, ltW, sizeof(ltW));
            ptr += sizeof(ltW)/sizeof(WCHAR);
            break;
        case '&':
            memcpy(ptr, ampW, sizeof(ampW));
            ptr += sizeof(ampW)/sizeof(WCHAR);
            break;
        case '>':
            memcpy(ptr, gtW, sizeof(gtW));
            ptr += sizeof(gtW)/sizeof(WCHAR);
            break;
        case '"':
            if (mode == EscapeValue)
            {
                memcpy(ptr, equotW, sizeof(equotW));
                ptr += sizeof(equotW)/sizeof(WCHAR);
                break;
            }
            /* fallthrough for text mode */
        default:
            *ptr++ = *str;
            break;
        }

        str++;
        if (*len != -1) p--;
    }

    if (*len != -1) *len = ptr-ret;
    *++ptr = 0;

    return ret;
}

static void write_prolog_buffer(mxwriter *This)
{
    static const WCHAR versionW[] = {'<','?','x','m','l',' ','v','e','r','s','i','o','n','='};
    static const WCHAR encodingW[] = {' ','e','n','c','o','d','i','n','g','=','\"'};
    static const WCHAR standaloneW[] = {' ','s','t','a','n','d','a','l','o','n','e','=','\"'};
    static const WCHAR yesW[] = {'y','e','s','\"','?','>'};
    static const WCHAR noW[] = {'n','o','\"','?','>'};

    /* version */
    write_output_buffer(This->buffer, versionW, sizeof(versionW)/sizeof(WCHAR));
    write_output_buffer_quoted(This->buffer, This->version, -1);

    /* encoding */
    write_output_buffer(This->buffer, encodingW, sizeof(encodingW)/sizeof(WCHAR));

    /* always write UTF-16 to WCHAR buffer */
    write_output_buffer_mode(This->buffer, OutputBuffer_Native, utf16W, sizeof(utf16W)/sizeof(WCHAR) - 1);
    write_output_buffer_mode(This->buffer, OutputBuffer_Encoded, This->encoding, -1);
    write_output_buffer(This->buffer, quotW, 1);

    /* standalone */
    write_output_buffer(This->buffer, standaloneW, sizeof(standaloneW)/sizeof(WCHAR));
    if (This->props[MXWriter_Standalone] == VARIANT_TRUE)
        write_output_buffer(This->buffer, yesW, sizeof(yesW)/sizeof(WCHAR));
    else
        write_output_buffer(This->buffer, noW, sizeof(noW)/sizeof(WCHAR));

    write_output_buffer(This->buffer, crlfW, sizeof(crlfW)/sizeof(WCHAR));
    This->newline = TRUE;
}

/* Attempts to the write data from the mxwriter's buffer to
 * the destination stream (if there is one).
 */
static HRESULT write_data_to_stream(mxwriter *This)
{
    encoded_buffer *buffer;
    ULONG written = 0;
    HRESULT hr;

    if (!This->dest)
        return S_OK;

    if (This->xml_enc != XmlEncoding_UTF16)
        buffer = &This->buffer->encoded;
    else
        buffer = &This->buffer->utf16;

    if (This->dest_written > buffer->written) {
        ERR("Failed sanity check! Not sure what to do... (%d > %d)\n", This->dest_written, buffer->written);
        return E_FAIL;
    } else if (This->dest_written == buffer->written && This->xml_enc != XmlEncoding_UTF8)
        /* Windows seems to make an empty write call when the encoding is UTF-8 and
         * all the data has been written to the stream. It doesn't seem make this call
         * for any other encodings.
         */
        return S_OK;

    /* Write the current content from the output buffer into 'dest'.
     * TODO: Check what Windows does if the IStream doesn't write all of
     *       the data we give it at once.
     */
    hr = IStream_Write(This->dest, buffer->data+This->dest_written,
                         buffer->written-This->dest_written, &written);
    if (FAILED(hr)) {
        WARN("Failed to write data to IStream (0x%08x)\n", hr);
        return hr;
    }

    This->dest_written += written;
    return hr;
}

/* Newly added element start tag left unclosed cause for empty elements
   we have to close it differently. */
static void close_element_starttag(const mxwriter *This)
{
    static const WCHAR gtW[] = {'>'};
    if (!This->element) return;
    write_output_buffer(This->buffer, gtW, 1);
}

static void write_node_indent(mxwriter *This)
{
    static const WCHAR tabW[] = {'\t'};
    int indent = This->indent;

    if (!This->props[MXWriter_Indent] || This->text)
    {
        This->text = FALSE;
        return;
    }

    /* This is to workaround PI output logic that always puts newline chars,
       document prolog PI does that too. */
    if (!This->newline)
        write_output_buffer(This->buffer, crlfW, sizeof(crlfW)/sizeof(WCHAR));
    while (indent--)
        write_output_buffer(This->buffer, tabW, 1);

    This->newline = FALSE;
    This->text = FALSE;
}

static inline void writer_inc_indent(mxwriter *This)
{
    This->indent++;
}

static inline void writer_dec_indent(mxwriter *This)
{
    if (This->indent) This->indent--;
    /* depth is decreased only when element is closed, meaning it's not a text node
       at this point */
    This->text = FALSE;
}

static void set_element_name(mxwriter *This, const WCHAR *name, int len)
{
    SysFreeString(This->element);
    if (name)
        This->element = len != -1 ? SysAllocStringLen(name, len) : SysAllocString(name);
    else
        This->element = NULL;
}

static inline HRESULT flush_output_buffer(mxwriter *This)
{
    close_element_starttag(This);
    set_element_name(This, NULL, 0);
    This->cdata = FALSE;
    return write_data_to_stream(This);
}

/* Resets the mxwriter's output buffer by closing it, then creating a new
 * output buffer using the given encoding.
 */
static inline void reset_output_buffer(mxwriter *This)
{
    close_output_buffer(This);
    This->dest_written = 0;
}

static HRESULT writer_set_property(mxwriter *writer, mxwriter_prop property, VARIANT_BOOL value)
{
    writer->props[property] = value;
    writer->prop_changed = TRUE;
    return S_OK;
}

static HRESULT writer_get_property(const mxwriter *writer, mxwriter_prop property, VARIANT_BOOL *value)
{
    if (!value) return E_POINTER;
    *value = writer->props[property];
    return S_OK;
}

static inline mxwriter *impl_from_IMXWriter(IMXWriter *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, IMXWriter_iface);
}

static inline mxwriter *impl_from_ISAXContentHandler(ISAXContentHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, ISAXContentHandler_iface);
}

static inline mxwriter *impl_from_IVBSAXContentHandler(IVBSAXContentHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, IVBSAXContentHandler_iface);
}

static inline mxwriter *impl_from_ISAXLexicalHandler(ISAXLexicalHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, ISAXLexicalHandler_iface);
}

static inline mxwriter *impl_from_IVBSAXLexicalHandler(IVBSAXLexicalHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, IVBSAXLexicalHandler_iface);
}

static inline mxwriter *impl_from_ISAXDeclHandler(ISAXDeclHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, ISAXDeclHandler_iface);
}

static inline mxwriter *impl_from_IVBSAXDeclHandler(IVBSAXDeclHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, IVBSAXDeclHandler_iface);
}

static inline mxwriter *impl_from_ISAXDTDHandler(ISAXDTDHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, ISAXDTDHandler_iface);
}

static inline mxwriter *impl_from_IVBSAXDTDHandler(IVBSAXDTDHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, IVBSAXDTDHandler_iface);
}

static inline mxwriter *impl_from_ISAXErrorHandler(ISAXErrorHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, ISAXErrorHandler_iface);
}

static inline mxwriter *impl_from_IVBSAXErrorHandler(IVBSAXErrorHandler *iface)
{
    return CONTAINING_RECORD(iface, mxwriter, IVBSAXErrorHandler_iface);
}

static HRESULT WINAPI mxwriter_QueryInterface(IMXWriter *iface, REFIID riid, void **obj)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), obj);

    *obj = NULL;

    if ( IsEqualGUID( riid, &IID_IMXWriter ) ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *obj = &This->IMXWriter_iface;
    }
    else if ( IsEqualGUID( riid, &IID_ISAXContentHandler ) )
    {
        *obj = &This->ISAXContentHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_ISAXLexicalHandler ) )
    {
        *obj = &This->ISAXLexicalHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_ISAXDeclHandler ) )
    {
        *obj = &This->ISAXDeclHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_ISAXDTDHandler ) )
    {
        *obj = &This->ISAXDTDHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_ISAXErrorHandler ) )
    {
        *obj = &This->ISAXErrorHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_IVBSAXDeclHandler ) )
    {
        *obj = &This->IVBSAXDeclHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_IVBSAXLexicalHandler ) )
    {
        *obj = &This->IVBSAXLexicalHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_IVBSAXContentHandler ) )
    {
        *obj = &This->IVBSAXContentHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_IVBSAXDTDHandler ) )
    {
        *obj = &This->IVBSAXDTDHandler_iface;
    }
    else if ( IsEqualGUID( riid, &IID_IVBSAXErrorHandler ) )
    {
        *obj = &This->IVBSAXErrorHandler_iface;
    }
    else if (dispex_query_interface(&This->dispex, riid, obj))
    {
        return *obj ? S_OK : E_NOINTERFACE;
    }
    else
    {
        ERR("interface %s not implemented\n", debugstr_guid(riid));
        *obj = NULL;
        return E_NOINTERFACE;
    }

    IMXWriter_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI mxwriter_AddRef(IMXWriter *iface)
{
    mxwriter *This = impl_from_IMXWriter( iface );
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p)->(%d)\n", This, ref);

    return ref;
}

static ULONG WINAPI mxwriter_Release(IMXWriter *iface)
{
    mxwriter *This = impl_from_IMXWriter( iface );
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(%d)\n", This, ref);

    if(!ref)
    {
        /* Windows flushes the buffer when the interface is destroyed. */
        flush_output_buffer(This);
        free_output_buffer(This->buffer);

        if (This->dest) IStream_Release(This->dest);
        SysFreeString(This->version);
        SysFreeString(This->encoding);

        SysFreeString(This->element);
        release_dispex(&This->dispex);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI mxwriter_GetTypeInfoCount(IMXWriter *iface, UINT* pctinfo)
{
    mxwriter *This = impl_from_IMXWriter( iface );
    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI mxwriter_GetTypeInfo(
    IMXWriter *iface,
    UINT iTInfo, LCID lcid,
    ITypeInfo** ppTInfo )
{
    mxwriter *This = impl_from_IMXWriter( iface );
    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface,
        iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI mxwriter_GetIDsOfNames(
    IMXWriter *iface,
    REFIID riid, LPOLESTR* rgszNames,
    UINT cNames, LCID lcid, DISPID* rgDispId )
{
    mxwriter *This = impl_from_IMXWriter( iface );
    return IDispatchEx_GetIDsOfNames(&This->dispex.IDispatchEx_iface,
        riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI mxwriter_Invoke(
    IMXWriter *iface,
    DISPID dispIdMember, REFIID riid, LCID lcid,
    WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult,
    EXCEPINFO* pExcepInfo, UINT* puArgErr )
{
    mxwriter *This = impl_from_IMXWriter( iface );
    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface,
        dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI mxwriter_put_output(IMXWriter *iface, VARIANT dest)
{
    mxwriter *This = impl_from_IMXWriter( iface );
    HRESULT hr;

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&dest));

    hr = flush_output_buffer(This);
    if (FAILED(hr))
        return hr;

    switch (V_VT(&dest))
    {
    case VT_EMPTY:
    {
        if (This->dest) IStream_Release(This->dest);
        This->dest = NULL;
        reset_output_buffer(This);
        break;
    }
    case VT_UNKNOWN:
    {
        IStream *stream;

        hr = IUnknown_QueryInterface(V_UNKNOWN(&dest), &IID_IStream, (void**)&stream);
        if (hr == S_OK)
        {
            /* Recreate the output buffer to make sure it's using the correct encoding. */
            reset_output_buffer(This);

            if (This->dest) IStream_Release(This->dest);
            This->dest = stream;
            break;
        }

        FIXME("unhandled interface type for VT_UNKNOWN destination\n");
        return E_NOTIMPL;
    }
    default:
        FIXME("unhandled destination type %s\n", debugstr_variant(&dest));
        return E_NOTIMPL;
    }

    return S_OK;
}

static HRESULT WINAPI mxwriter_get_output(IMXWriter *iface, VARIANT *dest)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, dest);

    if (!dest) return E_POINTER;

    if (!This->dest)
    {
        HRESULT hr = flush_output_buffer(This);
        if (FAILED(hr))
            return hr;

        V_VT(dest)   = VT_BSTR;
        V_BSTR(dest) = SysAllocString((WCHAR*)This->buffer->utf16.data);

        return S_OK;
    }

    /* we only support IStream output so far */
    V_VT(dest) = VT_UNKNOWN;
    V_UNKNOWN(dest) = (IUnknown*)This->dest;
    IStream_AddRef(This->dest);

    return S_OK;
}

static HRESULT WINAPI mxwriter_put_encoding(IMXWriter *iface, BSTR encoding)
{
    mxwriter *This = impl_from_IMXWriter( iface );
    xml_encoding enc;
    HRESULT hr;

    TRACE("(%p)->(%s)\n", This, debugstr_w(encoding));

    enc = parse_encoding_name(encoding);
    if (enc == XmlEncoding_Unknown)
    {
        FIXME("unsupported encoding %s\n", debugstr_w(encoding));
        return E_INVALIDARG;
    }

    hr = flush_output_buffer(This);
    if (FAILED(hr))
        return hr;

    SysReAllocString(&This->encoding, encoding);
    This->xml_enc = enc;

    TRACE("got encoding %d\n", This->xml_enc);
    reset_output_buffer(This);
    return S_OK;
}

static HRESULT WINAPI mxwriter_get_encoding(IMXWriter *iface, BSTR *encoding)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, encoding);

    if (!encoding) return E_POINTER;

    *encoding = SysAllocString(This->encoding);
    if (!*encoding) return E_OUTOFMEMORY;

    return S_OK;
}

static HRESULT WINAPI mxwriter_put_byteOrderMark(IMXWriter *iface, VARIANT_BOOL value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%d)\n", This, value);
    return writer_set_property(This, MXWriter_BOM, value);
}

static HRESULT WINAPI mxwriter_get_byteOrderMark(IMXWriter *iface, VARIANT_BOOL *value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, value);
    return writer_get_property(This, MXWriter_BOM, value);
}

static HRESULT WINAPI mxwriter_put_indent(IMXWriter *iface, VARIANT_BOOL value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%d)\n", This, value);
    return writer_set_property(This, MXWriter_Indent, value);
}

static HRESULT WINAPI mxwriter_get_indent(IMXWriter *iface, VARIANT_BOOL *value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, value);
    return writer_get_property(This, MXWriter_Indent, value);
}

static HRESULT WINAPI mxwriter_put_standalone(IMXWriter *iface, VARIANT_BOOL value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%d)\n", This, value);
    return writer_set_property(This, MXWriter_Standalone, value);
}

static HRESULT WINAPI mxwriter_get_standalone(IMXWriter *iface, VARIANT_BOOL *value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, value);
    return writer_get_property(This, MXWriter_Standalone, value);
}

static HRESULT WINAPI mxwriter_put_omitXMLDeclaration(IMXWriter *iface, VARIANT_BOOL value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%d)\n", This, value);
    return writer_set_property(This, MXWriter_OmitXmlDecl, value);
}

static HRESULT WINAPI mxwriter_get_omitXMLDeclaration(IMXWriter *iface, VARIANT_BOOL *value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, value);
    return writer_get_property(This, MXWriter_OmitXmlDecl, value);
}

static HRESULT WINAPI mxwriter_put_version(IMXWriter *iface, BSTR version)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%s)\n", This, debugstr_w(version));

    if (!version) return E_INVALIDARG;

    SysFreeString(This->version);
    This->version = SysAllocString(version);

    return S_OK;
}

static HRESULT WINAPI mxwriter_get_version(IMXWriter *iface, BSTR *version)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, version);

    if (!version) return E_POINTER;

    return return_bstr(This->version, version);
}

static HRESULT WINAPI mxwriter_put_disableOutputEscaping(IMXWriter *iface, VARIANT_BOOL value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%d)\n", This, value);
    return writer_set_property(This, MXWriter_DisableEscaping, value);
}

static HRESULT WINAPI mxwriter_get_disableOutputEscaping(IMXWriter *iface, VARIANT_BOOL *value)
{
    mxwriter *This = impl_from_IMXWriter( iface );

    TRACE("(%p)->(%p)\n", This, value);
    return writer_get_property(This, MXWriter_DisableEscaping, value);
}

static HRESULT WINAPI mxwriter_flush(IMXWriter *iface)
{
    mxwriter *This = impl_from_IMXWriter( iface );
    TRACE("(%p)\n", This);
    return flush_output_buffer(This);
}

static const struct IMXWriterVtbl MXWriterVtbl =
{
    mxwriter_QueryInterface,
    mxwriter_AddRef,
    mxwriter_Release,
    mxwriter_GetTypeInfoCount,
    mxwriter_GetTypeInfo,
    mxwriter_GetIDsOfNames,
    mxwriter_Invoke,
    mxwriter_put_output,
    mxwriter_get_output,
    mxwriter_put_encoding,
    mxwriter_get_encoding,
    mxwriter_put_byteOrderMark,
    mxwriter_get_byteOrderMark,
    mxwriter_put_indent,
    mxwriter_get_indent,
    mxwriter_put_standalone,
    mxwriter_get_standalone,
    mxwriter_put_omitXMLDeclaration,
    mxwriter_get_omitXMLDeclaration,
    mxwriter_put_version,
    mxwriter_get_version,
    mxwriter_put_disableOutputEscaping,
    mxwriter_get_disableOutputEscaping,
    mxwriter_flush
};

/*** ISAXContentHandler ***/
static HRESULT WINAPI SAXContentHandler_QueryInterface(
    ISAXContentHandler *iface,
    REFIID riid,
    void **obj)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI SAXContentHandler_AddRef(ISAXContentHandler *iface)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI SAXContentHandler_Release(ISAXContentHandler *iface)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI SAXContentHandler_putDocumentLocator(
    ISAXContentHandler *iface,
    ISAXLocator *locator)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    FIXME("(%p)->(%p)\n", This, locator);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXContentHandler_startDocument(ISAXContentHandler *iface)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );

    TRACE("(%p)\n", This);

    /* If properties have been changed since the last "endDocument" call
     * we need to reset the output buffer. If we don't the output buffer
     * could end up with multiple XML documents in it, plus this seems to
     * be how Windows works.
     */
    if (This->prop_changed) {
        reset_output_buffer(This);
        This->prop_changed = FALSE;
    }

    if (This->props[MXWriter_OmitXmlDecl] == VARIANT_TRUE) return S_OK;

    write_prolog_buffer(This);

    if (This->dest && This->xml_enc == XmlEncoding_UTF16) {
        static const char utf16BOM[] = {0xff,0xfe};

        if (This->props[MXWriter_BOM] == VARIANT_TRUE)
            /* Windows passes a NULL pointer as the pcbWritten parameter and
             * ignores any error codes returned from this Write call.
             */
            IStream_Write(This->dest, utf16BOM, sizeof(utf16BOM), NULL);
    }

    return S_OK;
}

static HRESULT WINAPI SAXContentHandler_endDocument(ISAXContentHandler *iface)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    TRACE("(%p)\n", This);
    This->prop_changed = FALSE;
    return flush_output_buffer(This);
}

static HRESULT WINAPI SAXContentHandler_startPrefixMapping(
    ISAXContentHandler *iface,
    const WCHAR *prefix,
    int nprefix,
    const WCHAR *uri,
    int nuri)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    FIXME("(%p)->(%s %s)\n", This, debugstr_wn(prefix, nprefix), debugstr_wn(uri, nuri));
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXContentHandler_endPrefixMapping(
    ISAXContentHandler *iface,
    const WCHAR *prefix,
    int nprefix)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    FIXME("(%p)->(%s)\n", This, debugstr_wn(prefix, nprefix));
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXContentHandler_startElement(
    ISAXContentHandler *iface,
    const WCHAR *namespaceUri,
    int nnamespaceUri,
    const WCHAR *local_name,
    int nlocal_name,
    const WCHAR *QName,
    int nQName,
    ISAXAttributes *attr)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    static const WCHAR ltW[] = {'<'};

    TRACE("(%p)->(%s %s %s %p)\n", This, debugstr_wn(namespaceUri, nnamespaceUri),
        debugstr_wn(local_name, nlocal_name), debugstr_wn(QName, nQName), attr);

    if (((!namespaceUri || !local_name || !QName) && This->class_version != MSXML6) ||
        (nQName == -1 && This->class_version == MSXML6))
        return E_INVALIDARG;

    close_element_starttag(This);
    set_element_name(This, QName ? QName  : emptyW,
                           QName ? nQName : 0);

    write_node_indent(This);

    write_output_buffer(This->buffer, ltW, 1);
    write_output_buffer(This->buffer, QName, nQName);
    writer_inc_indent(This);

    if (attr)
    {
        int length, i, escape;
        HRESULT hr;

        hr = ISAXAttributes_getLength(attr, &length);
        if (FAILED(hr)) return hr;

        escape = This->props[MXWriter_DisableEscaping] == VARIANT_FALSE ||
            (This->class_version == MSXML4 || This->class_version == MSXML6);

        for (i = 0; i < length; i++)
        {
            static const WCHAR eqW[] = {'='};
            const WCHAR *str;
            int len = 0;

            hr = ISAXAttributes_getQName(attr, i, &str, &len);
            if (FAILED(hr)) return hr;

            /* space separator in front of every attribute */
            write_output_buffer(This->buffer, spaceW, 1);
            write_output_buffer(This->buffer, str, len);

            write_output_buffer(This->buffer, eqW, 1);

            len = 0;
            hr = ISAXAttributes_getValue(attr, i, &str, &len);
            if (FAILED(hr)) return hr;

            if (escape)
            {
                WCHAR *escaped = get_escaped_string(str, EscapeValue, &len);
                write_output_buffer_quoted(This->buffer, escaped, len);
                heap_free(escaped);
            }
            else
                write_output_buffer_quoted(This->buffer, str, len);
        }
    }

    return S_OK;
}

static HRESULT WINAPI SAXContentHandler_endElement(
    ISAXContentHandler *iface,
    const WCHAR *namespaceUri,
    int nnamespaceUri,
    const WCHAR * local_name,
    int nlocal_name,
    const WCHAR *QName,
    int nQName)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );

    TRACE("(%p)->(%s:%d %s:%d %s:%d)\n", This, debugstr_wn(namespaceUri, nnamespaceUri), nnamespaceUri,
        debugstr_wn(local_name, nlocal_name), nlocal_name, debugstr_wn(QName, nQName), nQName);

    if (((!namespaceUri || !local_name || !QName) && This->class_version != MSXML6) ||
         (nQName == -1 && This->class_version == MSXML6))
        return E_INVALIDARG;

    writer_dec_indent(This);

    if (This->element)
    {
        static const WCHAR closeW[] = {'/','>'};
        write_output_buffer(This->buffer, closeW, 2);
    }
    else
    {
        static const WCHAR closetagW[] = {'<','/'};
        static const WCHAR gtW[] = {'>'};

        write_node_indent(This);
        write_output_buffer(This->buffer, closetagW, 2);
        write_output_buffer(This->buffer, QName, nQName);
        write_output_buffer(This->buffer, gtW, 1);
    }

    set_element_name(This, NULL, 0);

    return S_OK;
}

static HRESULT WINAPI SAXContentHandler_characters(
    ISAXContentHandler *iface,
    const WCHAR *chars,
    int nchars)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );

    TRACE("(%p)->(%s:%d)\n", This, debugstr_wn(chars, nchars), nchars);

    if (!chars) return E_INVALIDARG;

    close_element_starttag(This);
    set_element_name(This, NULL, 0);

    if (!This->cdata)
        This->text = TRUE;

    if (nchars)
    {
        if (This->cdata || This->props[MXWriter_DisableEscaping] == VARIANT_TRUE)
            write_output_buffer(This->buffer, chars, nchars);
        else
        {
            int len = nchars;
            WCHAR *escaped;

            escaped = get_escaped_string(chars, EscapeText, &len);
            write_output_buffer(This->buffer, escaped, len);
            heap_free(escaped);
        }
    }

    return S_OK;
}

static HRESULT WINAPI SAXContentHandler_ignorableWhitespace(
    ISAXContentHandler *iface,
    const WCHAR *chars,
    int nchars)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );

    TRACE("(%p)->(%s)\n", This, debugstr_wn(chars, nchars));

    if (!chars) return E_INVALIDARG;

    write_output_buffer(This->buffer, chars, nchars);

    return S_OK;
}

static HRESULT WINAPI SAXContentHandler_processingInstruction(
    ISAXContentHandler *iface,
    const WCHAR *target,
    int ntarget,
    const WCHAR *data,
    int ndata)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    static const WCHAR openpiW[] = {'<','?'};
    static const WCHAR closepiW[] = {'?','>','\r','\n'};

    TRACE("(%p)->(%s %s)\n", This, debugstr_wn(target, ntarget), debugstr_wn(data, ndata));

    if (!target) return E_INVALIDARG;

    write_node_indent(This);
    write_output_buffer(This->buffer, openpiW, sizeof(openpiW)/sizeof(WCHAR));

    if (*target)
        write_output_buffer(This->buffer, target, ntarget);

    if (data && *data && ndata)
    {
        write_output_buffer(This->buffer, spaceW, 1);
        write_output_buffer(This->buffer, data, ndata);
    }

    write_output_buffer(This->buffer, closepiW, sizeof(closepiW)/sizeof(WCHAR));
    This->newline = TRUE;

    return S_OK;
}

static HRESULT WINAPI SAXContentHandler_skippedEntity(
    ISAXContentHandler *iface,
    const WCHAR *name,
    int nname)
{
    mxwriter *This = impl_from_ISAXContentHandler( iface );
    FIXME("(%p)->(%s)\n", This, debugstr_wn(name, nname));
    return E_NOTIMPL;
}

static const struct ISAXContentHandlerVtbl SAXContentHandlerVtbl =
{
    SAXContentHandler_QueryInterface,
    SAXContentHandler_AddRef,
    SAXContentHandler_Release,
    SAXContentHandler_putDocumentLocator,
    SAXContentHandler_startDocument,
    SAXContentHandler_endDocument,
    SAXContentHandler_startPrefixMapping,
    SAXContentHandler_endPrefixMapping,
    SAXContentHandler_startElement,
    SAXContentHandler_endElement,
    SAXContentHandler_characters,
    SAXContentHandler_ignorableWhitespace,
    SAXContentHandler_processingInstruction,
    SAXContentHandler_skippedEntity
};

/*** ISAXLexicalHandler ***/
static HRESULT WINAPI SAXLexicalHandler_QueryInterface(ISAXLexicalHandler *iface,
    REFIID riid, void **obj)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI SAXLexicalHandler_AddRef(ISAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI SAXLexicalHandler_Release(ISAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI SAXLexicalHandler_startDTD(ISAXLexicalHandler *iface,
    const WCHAR *name, int name_len, const WCHAR *publicId, int publicId_len,
    const WCHAR *systemId, int systemId_len)
{
    static const WCHAR doctypeW[] = {'<','!','D','O','C','T','Y','P','E',' '};
    static const WCHAR openintW[] = {'[','\r','\n'};

    mxwriter *This = impl_from_ISAXLexicalHandler( iface );

    TRACE("(%p)->(%s %s %s)\n", This, debugstr_wn(name, name_len), debugstr_wn(publicId, publicId_len),
        debugstr_wn(systemId, systemId_len));

    if (!name) return E_INVALIDARG;

    write_output_buffer(This->buffer, doctypeW, sizeof(doctypeW)/sizeof(WCHAR));

    if (*name)
    {
        write_output_buffer(This->buffer, name, name_len);
        write_output_buffer(This->buffer, spaceW, 1);
    }

    if (publicId)
    {
        static const WCHAR publicW[] = {'P','U','B','L','I','C',' '};

        write_output_buffer(This->buffer, publicW, sizeof(publicW)/sizeof(WCHAR));
        write_output_buffer_quoted(This->buffer, publicId, publicId_len);

        if (!systemId) return E_INVALIDARG;

        if (*publicId)
            write_output_buffer(This->buffer, spaceW, 1);

        write_output_buffer_quoted(This->buffer, systemId, systemId_len);

        if (*systemId)
            write_output_buffer(This->buffer, spaceW, 1);
    }
    else if (systemId)
    {
        static const WCHAR systemW[] = {'S','Y','S','T','E','M',' '};

        write_output_buffer(This->buffer, systemW, sizeof(systemW)/sizeof(WCHAR));
        write_output_buffer_quoted(This->buffer, systemId, systemId_len);
        if (*systemId)
            write_output_buffer(This->buffer, spaceW, 1);
    }

    write_output_buffer(This->buffer, openintW, sizeof(openintW)/sizeof(WCHAR));

    return S_OK;
}

static HRESULT WINAPI SAXLexicalHandler_endDTD(ISAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    static const WCHAR closedtdW[] = {']','>','\r','\n'};

    TRACE("(%p)\n", This);

    write_output_buffer(This->buffer, closedtdW, sizeof(closedtdW)/sizeof(WCHAR));

    return S_OK;
}

static HRESULT WINAPI SAXLexicalHandler_startEntity(ISAXLexicalHandler *iface, const WCHAR *name, int len)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    FIXME("(%p)->(%s): stub\n", This, debugstr_wn(name, len));
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXLexicalHandler_endEntity(ISAXLexicalHandler *iface, const WCHAR *name, int len)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    FIXME("(%p)->(%s): stub\n", This, debugstr_wn(name, len));
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXLexicalHandler_startCDATA(ISAXLexicalHandler *iface)
{
    static const WCHAR scdataW[] = {'<','!','[','C','D','A','T','A','['};
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );

    TRACE("(%p)\n", This);

    write_node_indent(This);
    write_output_buffer(This->buffer, scdataW, sizeof(scdataW)/sizeof(WCHAR));
    This->cdata = TRUE;

    return S_OK;
}

static HRESULT WINAPI SAXLexicalHandler_endCDATA(ISAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    static const WCHAR ecdataW[] = {']',']','>'};

    TRACE("(%p)\n", This);

    write_output_buffer(This->buffer, ecdataW, sizeof(ecdataW)/sizeof(WCHAR));
    This->cdata = FALSE;

    return S_OK;
}

static HRESULT WINAPI SAXLexicalHandler_comment(ISAXLexicalHandler *iface, const WCHAR *chars, int nchars)
{
    mxwriter *This = impl_from_ISAXLexicalHandler( iface );
    static const WCHAR copenW[] = {'<','!','-','-'};
    static const WCHAR ccloseW[] = {'-','-','>','\r','\n'};

    TRACE("(%p)->(%s:%d)\n", This, debugstr_wn(chars, nchars), nchars);

    if (!chars) return E_INVALIDARG;

    close_element_starttag(This);
    write_node_indent(This);

    write_output_buffer(This->buffer, copenW, sizeof(copenW)/sizeof(WCHAR));
    if (nchars)
        write_output_buffer(This->buffer, chars, nchars);
    write_output_buffer(This->buffer, ccloseW, sizeof(ccloseW)/sizeof(WCHAR));

    return S_OK;
}

static const struct ISAXLexicalHandlerVtbl SAXLexicalHandlerVtbl =
{
    SAXLexicalHandler_QueryInterface,
    SAXLexicalHandler_AddRef,
    SAXLexicalHandler_Release,
    SAXLexicalHandler_startDTD,
    SAXLexicalHandler_endDTD,
    SAXLexicalHandler_startEntity,
    SAXLexicalHandler_endEntity,
    SAXLexicalHandler_startCDATA,
    SAXLexicalHandler_endCDATA,
    SAXLexicalHandler_comment
};

/*** ISAXDeclHandler ***/
static HRESULT WINAPI SAXDeclHandler_QueryInterface(ISAXDeclHandler *iface,
    REFIID riid, void **obj)
{
    mxwriter *This = impl_from_ISAXDeclHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI SAXDeclHandler_AddRef(ISAXDeclHandler *iface)
{
    mxwriter *This = impl_from_ISAXDeclHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI SAXDeclHandler_Release(ISAXDeclHandler *iface)
{
    mxwriter *This = impl_from_ISAXDeclHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI SAXDeclHandler_elementDecl(ISAXDeclHandler *iface,
    const WCHAR *name, int n_name, const WCHAR *model, int n_model)
{
    static const WCHAR elementW[] = {'<','!','E','L','E','M','E','N','T',' '};
    mxwriter *This = impl_from_ISAXDeclHandler( iface );

    TRACE("(%p)->(%s:%d %s:%d)\n", This, debugstr_wn(name, n_name), n_name,
        debugstr_wn(model, n_model), n_model);

    if (!name || !model) return E_INVALIDARG;

    write_output_buffer(This->buffer, elementW, sizeof(elementW)/sizeof(WCHAR));
    if (n_name) {
        write_output_buffer(This->buffer, name, n_name);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
    }
    if (n_model)
        write_output_buffer(This->buffer, model, n_model);
    write_output_buffer(This->buffer, closetagW, sizeof(closetagW)/sizeof(WCHAR));

    return S_OK;
}

static HRESULT WINAPI SAXDeclHandler_attributeDecl(ISAXDeclHandler *iface,
    const WCHAR *element, int n_element, const WCHAR *attr, int n_attr,
    const WCHAR *type, int n_type, const WCHAR *Default, int n_default,
    const WCHAR *value, int n_value)
{
    mxwriter *This = impl_from_ISAXDeclHandler( iface );
    static const WCHAR attlistW[] = {'<','!','A','T','T','L','I','S','T',' '};
    static const WCHAR closetagW[] = {'>','\r','\n'};

    TRACE("(%p)->(%s:%d %s:%d %s:%d %s:%d %s:%d)\n", This, debugstr_wn(element, n_element), n_element,
        debugstr_wn(attr, n_attr), n_attr, debugstr_wn(type, n_type), n_type, debugstr_wn(Default, n_default), n_default,
        debugstr_wn(value, n_value), n_value);

    write_output_buffer(This->buffer, attlistW, sizeof(attlistW)/sizeof(WCHAR));
    if (n_element) {
        write_output_buffer(This->buffer, element, n_element);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
    }

    if (n_attr) {
        write_output_buffer(This->buffer, attr, n_attr);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
    }

    if (n_type) {
        write_output_buffer(This->buffer, type, n_type);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
    }

    if (n_default) {
        write_output_buffer(This->buffer, Default, n_default);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
    }

    if (n_value)
        write_output_buffer_quoted(This->buffer, value, n_value);

    write_output_buffer(This->buffer, closetagW, sizeof(closetagW)/sizeof(WCHAR));

    return S_OK;
}

static HRESULT WINAPI SAXDeclHandler_internalEntityDecl(ISAXDeclHandler *iface,
    const WCHAR *name, int n_name, const WCHAR *value, int n_value)
{
    mxwriter *This = impl_from_ISAXDeclHandler( iface );

    TRACE("(%p)->(%s:%d %s:%d)\n", This, debugstr_wn(name, n_name), n_name,
        debugstr_wn(value, n_value), n_value);

    if (!name || !value) return E_INVALIDARG;

    write_output_buffer(This->buffer, entityW, sizeof(entityW)/sizeof(WCHAR));
    if (n_name) {
        write_output_buffer(This->buffer, name, n_name);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
    }

    if (n_value)
        write_output_buffer_quoted(This->buffer, value, n_value);

    write_output_buffer(This->buffer, closetagW, sizeof(closetagW)/sizeof(WCHAR));

    return S_OK;
}

static HRESULT WINAPI SAXDeclHandler_externalEntityDecl(ISAXDeclHandler *iface,
    const WCHAR *name, int n_name, const WCHAR *publicId, int n_publicId,
    const WCHAR *systemId, int n_systemId)
{
    static const WCHAR publicW[] = {'P','U','B','L','I','C',' '};
    static const WCHAR systemW[] = {'S','Y','S','T','E','M',' '};
    mxwriter *This = impl_from_ISAXDeclHandler( iface );

    TRACE("(%p)->(%s:%d %s:%d %s:%d)\n", This, debugstr_wn(name, n_name), n_name,
        debugstr_wn(publicId, n_publicId), n_publicId, debugstr_wn(systemId, n_systemId), n_systemId);

    if (!name) return E_INVALIDARG;
    if (publicId && !systemId) return E_INVALIDARG;
    if (!publicId && !systemId) return E_INVALIDARG;

    write_output_buffer(This->buffer, entityW, sizeof(entityW)/sizeof(WCHAR));
    if (n_name) {
        write_output_buffer(This->buffer, name, n_name);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
    }

    if (publicId)
    {
        write_output_buffer(This->buffer, publicW, sizeof(publicW)/sizeof(WCHAR));
        write_output_buffer_quoted(This->buffer, publicId, n_publicId);
        write_output_buffer(This->buffer, spaceW, sizeof(spaceW)/sizeof(WCHAR));
        write_output_buffer_quoted(This->buffer, systemId, n_systemId);
    }
    else
    {
        write_output_buffer(This->buffer, systemW, sizeof(systemW)/sizeof(WCHAR));
        write_output_buffer_quoted(This->buffer, systemId, n_systemId);
    }

    write_output_buffer(This->buffer, closetagW, sizeof(closetagW)/sizeof(WCHAR));

    return S_OK;
}

static const ISAXDeclHandlerVtbl SAXDeclHandlerVtbl = {
    SAXDeclHandler_QueryInterface,
    SAXDeclHandler_AddRef,
    SAXDeclHandler_Release,
    SAXDeclHandler_elementDecl,
    SAXDeclHandler_attributeDecl,
    SAXDeclHandler_internalEntityDecl,
    SAXDeclHandler_externalEntityDecl
};

/*** IVBSAXDeclHandler ***/
static HRESULT WINAPI VBSAXDeclHandler_QueryInterface(IVBSAXDeclHandler *iface,
    REFIID riid, void **obj)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI VBSAXDeclHandler_AddRef(IVBSAXDeclHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI VBSAXDeclHandler_Release(IVBSAXDeclHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI VBSAXDeclHandler_GetTypeInfoCount(IVBSAXDeclHandler *iface, UINT* pctinfo)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );
    return IMXWriter_GetTypeInfoCount(&This->IMXWriter_iface, pctinfo);
}

static HRESULT WINAPI VBSAXDeclHandler_GetTypeInfo(IVBSAXDeclHandler *iface, UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );
    return IMXWriter_GetTypeInfo(&This->IMXWriter_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI VBSAXDeclHandler_GetIDsOfNames(IVBSAXDeclHandler *iface, REFIID riid, LPOLESTR* rgszNames,
    UINT cNames, LCID lcid, DISPID* rgDispId )
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );
    return IMXWriter_GetIDsOfNames(&This->IMXWriter_iface, riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI VBSAXDeclHandler_Invoke(IVBSAXDeclHandler *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
    WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr )
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );
    return IMXWriter_Invoke(&This->IMXWriter_iface, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult,
        pExcepInfo, puArgErr);
}

static HRESULT WINAPI VBSAXDeclHandler_elementDecl(IVBSAXDeclHandler *iface, BSTR *name, BSTR *model)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );

    TRACE("(%p)->(%p %p)\n", This, name, model);

    if (!name || !model)
        return E_POINTER;

    return ISAXDeclHandler_elementDecl(&This->ISAXDeclHandler_iface, *name, -1, *model, -1);
}

static HRESULT WINAPI VBSAXDeclHandler_attributeDecl(IVBSAXDeclHandler *iface,
    BSTR *element, BSTR *attr, BSTR *type, BSTR *default_value, BSTR *value)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );

    TRACE("(%p)->(%p %p %p %p %p)\n", This, element, attr, type, default_value, value);

    if (!element || !attr || !type || !default_value || !value)
        return E_POINTER;

    return ISAXDeclHandler_attributeDecl(&This->ISAXDeclHandler_iface, *element, -1, *attr, -1, *type, -1,
        *default_value, -1, *value, -1);
}

static HRESULT WINAPI VBSAXDeclHandler_internalEntityDecl(IVBSAXDeclHandler *iface, BSTR *name, BSTR *value)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );

    TRACE("(%p)->(%p %p)\n", This, name, value);

    if (!name || !value)
        return E_POINTER;

    return ISAXDeclHandler_internalEntityDecl(&This->ISAXDeclHandler_iface, *name, -1, *value, -1);
}

static HRESULT WINAPI VBSAXDeclHandler_externalEntityDecl(IVBSAXDeclHandler *iface,
    BSTR *name, BSTR *publicid, BSTR *systemid)
{
    mxwriter *This = impl_from_IVBSAXDeclHandler( iface );

    TRACE("(%p)->(%p %p %p)\n", This, name, publicid, systemid);

    if (!name || !publicid || !systemid)
        return E_POINTER;

    return ISAXDeclHandler_externalEntityDecl(&This->ISAXDeclHandler_iface, *name, -1, *publicid, -1, *systemid, -1);
}

static const IVBSAXDeclHandlerVtbl VBSAXDeclHandlerVtbl = {
    VBSAXDeclHandler_QueryInterface,
    VBSAXDeclHandler_AddRef,
    VBSAXDeclHandler_Release,
    VBSAXDeclHandler_GetTypeInfoCount,
    VBSAXDeclHandler_GetTypeInfo,
    VBSAXDeclHandler_GetIDsOfNames,
    VBSAXDeclHandler_Invoke,
    VBSAXDeclHandler_elementDecl,
    VBSAXDeclHandler_attributeDecl,
    VBSAXDeclHandler_internalEntityDecl,
    VBSAXDeclHandler_externalEntityDecl
};

/*** IVBSAXLexicalHandler ***/
static HRESULT WINAPI VBSAXLexicalHandler_QueryInterface(IVBSAXLexicalHandler *iface,
    REFIID riid, void **obj)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI VBSAXLexicalHandler_AddRef(IVBSAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI VBSAXLexicalHandler_Release(IVBSAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI VBSAXLexicalHandler_GetTypeInfoCount(IVBSAXLexicalHandler *iface, UINT* pctinfo)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return IMXWriter_GetTypeInfoCount(&This->IMXWriter_iface, pctinfo);
}

static HRESULT WINAPI VBSAXLexicalHandler_GetTypeInfo(IVBSAXLexicalHandler *iface, UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return IMXWriter_GetTypeInfo(&This->IMXWriter_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI VBSAXLexicalHandler_GetIDsOfNames(IVBSAXLexicalHandler *iface, REFIID riid, LPOLESTR* rgszNames,
    UINT cNames, LCID lcid, DISPID* rgDispId )
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return IMXWriter_GetIDsOfNames(&This->IMXWriter_iface, riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI VBSAXLexicalHandler_Invoke(IVBSAXLexicalHandler *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
    WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr )
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return IMXWriter_Invoke(&This->IMXWriter_iface, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult,
        pExcepInfo, puArgErr);
}

static HRESULT WINAPI VBSAXLexicalHandler_startDTD(IVBSAXLexicalHandler *iface, BSTR *name, BSTR *publicId, BSTR *systemId)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );

    TRACE("(%p)->(%p %p %p)\n", This, name, publicId, systemId);

    if (!name || !publicId || !systemId)
        return E_POINTER;

    return ISAXLexicalHandler_startDTD(&This->ISAXLexicalHandler_iface, *name, -1, *publicId, -1, *systemId, -1);
}

static HRESULT WINAPI VBSAXLexicalHandler_endDTD(IVBSAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return ISAXLexicalHandler_endDTD(&This->ISAXLexicalHandler_iface);
}

static HRESULT WINAPI VBSAXLexicalHandler_startEntity(IVBSAXLexicalHandler *iface, BSTR *name)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );

    TRACE("(%p)->(%p)\n", This, name);

    if (!name)
        return E_POINTER;

    return ISAXLexicalHandler_startEntity(&This->ISAXLexicalHandler_iface, *name, -1);
}

static HRESULT WINAPI VBSAXLexicalHandler_endEntity(IVBSAXLexicalHandler *iface, BSTR *name)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );

    TRACE("(%p)->(%p)\n", This, name);

    if (!name)
        return E_POINTER;

    return ISAXLexicalHandler_endEntity(&This->ISAXLexicalHandler_iface, *name, -1);
}

static HRESULT WINAPI VBSAXLexicalHandler_startCDATA(IVBSAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return ISAXLexicalHandler_startCDATA(&This->ISAXLexicalHandler_iface);
}

static HRESULT WINAPI VBSAXLexicalHandler_endCDATA(IVBSAXLexicalHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );
    return ISAXLexicalHandler_endCDATA(&This->ISAXLexicalHandler_iface);
}

static HRESULT WINAPI VBSAXLexicalHandler_comment(IVBSAXLexicalHandler *iface, BSTR *chars)
{
    mxwriter *This = impl_from_IVBSAXLexicalHandler( iface );

    TRACE("(%p)->(%p)\n", This, chars);

    if (!chars)
        return E_POINTER;

    return ISAXLexicalHandler_comment(&This->ISAXLexicalHandler_iface, *chars, -1);
}

static const IVBSAXLexicalHandlerVtbl VBSAXLexicalHandlerVtbl = {
    VBSAXLexicalHandler_QueryInterface,
    VBSAXLexicalHandler_AddRef,
    VBSAXLexicalHandler_Release,
    VBSAXLexicalHandler_GetTypeInfoCount,
    VBSAXLexicalHandler_GetTypeInfo,
    VBSAXLexicalHandler_GetIDsOfNames,
    VBSAXLexicalHandler_Invoke,
    VBSAXLexicalHandler_startDTD,
    VBSAXLexicalHandler_endDTD,
    VBSAXLexicalHandler_startEntity,
    VBSAXLexicalHandler_endEntity,
    VBSAXLexicalHandler_startCDATA,
    VBSAXLexicalHandler_endCDATA,
    VBSAXLexicalHandler_comment
};

/*** IVBSAXContentHandler ***/
static HRESULT WINAPI VBSAXContentHandler_QueryInterface(IVBSAXContentHandler *iface, REFIID riid, void **obj)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI VBSAXContentHandler_AddRef(IVBSAXContentHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI VBSAXContentHandler_Release(IVBSAXContentHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI VBSAXContentHandler_GetTypeInfoCount(IVBSAXContentHandler *iface, UINT* pctinfo)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return IMXWriter_GetTypeInfoCount(&This->IMXWriter_iface, pctinfo);
}

static HRESULT WINAPI VBSAXContentHandler_GetTypeInfo(IVBSAXContentHandler *iface, UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return IMXWriter_GetTypeInfo(&This->IMXWriter_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI VBSAXContentHandler_GetIDsOfNames(IVBSAXContentHandler *iface, REFIID riid, LPOLESTR* rgszNames,
    UINT cNames, LCID lcid, DISPID* rgDispId )
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return IMXWriter_GetIDsOfNames(&This->IMXWriter_iface, riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI VBSAXContentHandler_Invoke(IVBSAXContentHandler *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
    WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr )
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return IMXWriter_Invoke(&This->IMXWriter_iface, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult,
        pExcepInfo, puArgErr);
}

static HRESULT WINAPI VBSAXContentHandler_putref_documentLocator(IVBSAXContentHandler *iface, IVBSAXLocator *locator)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    FIXME("(%p)->(%p): stub\n", This, locator);
    return E_NOTIMPL;
}

static HRESULT WINAPI VBSAXContentHandler_startDocument(IVBSAXContentHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return ISAXContentHandler_startDocument(&This->ISAXContentHandler_iface);
}

static HRESULT WINAPI VBSAXContentHandler_endDocument(IVBSAXContentHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    return ISAXContentHandler_endDocument(&This->ISAXContentHandler_iface);
}

static HRESULT WINAPI VBSAXContentHandler_startPrefixMapping(IVBSAXContentHandler *iface, BSTR *prefix, BSTR *uri)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );

    TRACE("(%p)->(%p %p)\n", This, prefix, uri);

    if (!prefix || !uri)
        return E_POINTER;

    return ISAXContentHandler_startPrefixMapping(&This->ISAXContentHandler_iface, *prefix, -1, *uri, -1);
}

static HRESULT WINAPI VBSAXContentHandler_endPrefixMapping(IVBSAXContentHandler *iface, BSTR *prefix)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );

    TRACE("(%p)->(%p)\n", This, prefix);

    if (!prefix)
        return E_POINTER;

    return ISAXContentHandler_endPrefixMapping(&This->ISAXContentHandler_iface, *prefix, -1);
}

static HRESULT WINAPI VBSAXContentHandler_startElement(IVBSAXContentHandler *iface,
    BSTR *namespaceURI, BSTR *localName, BSTR *QName, IVBSAXAttributes *attrs)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    FIXME("(%p)->(%p %p %p %p): stub\n", This, namespaceURI, localName, QName, attrs);
    return E_NOTIMPL;
}

static HRESULT WINAPI VBSAXContentHandler_endElement(IVBSAXContentHandler *iface, BSTR *namespaceURI,
    BSTR *localName, BSTR *QName)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );
    FIXME("(%p)->(%p %p %p): stub\n", This, namespaceURI, localName, QName);
    return E_NOTIMPL;
}

static HRESULT WINAPI VBSAXContentHandler_characters(IVBSAXContentHandler *iface, BSTR *chars)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );

    TRACE("(%p)->(%p)\n", This, chars);

    if (!chars)
        return E_POINTER;

    return ISAXContentHandler_characters(&This->ISAXContentHandler_iface, *chars, -1);
}

static HRESULT WINAPI VBSAXContentHandler_ignorableWhitespace(IVBSAXContentHandler *iface, BSTR *chars)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );

    TRACE("(%p)->(%p)\n", This, chars);

    if (!chars)
        return E_POINTER;

    return ISAXContentHandler_ignorableWhitespace(&This->ISAXContentHandler_iface, *chars, -1);
}

static HRESULT WINAPI VBSAXContentHandler_processingInstruction(IVBSAXContentHandler *iface,
    BSTR *target, BSTR *data)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );

    TRACE("(%p)->(%p %p)\n", This, target, data);

    if (!target || !data)
        return E_POINTER;

    return ISAXContentHandler_processingInstruction(&This->ISAXContentHandler_iface, *target, -1, *data, -1);
}

static HRESULT WINAPI VBSAXContentHandler_skippedEntity(IVBSAXContentHandler *iface, BSTR *name)
{
    mxwriter *This = impl_from_IVBSAXContentHandler( iface );

    TRACE("(%p)->(%p)\n", This, name);

    if (!name)
        return E_POINTER;

    return ISAXContentHandler_skippedEntity(&This->ISAXContentHandler_iface, *name, -1);
}

static const IVBSAXContentHandlerVtbl VBSAXContentHandlerVtbl = {
    VBSAXContentHandler_QueryInterface,
    VBSAXContentHandler_AddRef,
    VBSAXContentHandler_Release,
    VBSAXContentHandler_GetTypeInfoCount,
    VBSAXContentHandler_GetTypeInfo,
    VBSAXContentHandler_GetIDsOfNames,
    VBSAXContentHandler_Invoke,
    VBSAXContentHandler_putref_documentLocator,
    VBSAXContentHandler_startDocument,
    VBSAXContentHandler_endDocument,
    VBSAXContentHandler_startPrefixMapping,
    VBSAXContentHandler_endPrefixMapping,
    VBSAXContentHandler_startElement,
    VBSAXContentHandler_endElement,
    VBSAXContentHandler_characters,
    VBSAXContentHandler_ignorableWhitespace,
    VBSAXContentHandler_processingInstruction,
    VBSAXContentHandler_skippedEntity
};

static HRESULT WINAPI SAXDTDHandler_QueryInterface(ISAXDTDHandler *iface, REFIID riid, void **obj)
{
    mxwriter *This = impl_from_ISAXDTDHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI SAXDTDHandler_AddRef(ISAXDTDHandler *iface)
{
    mxwriter *This = impl_from_ISAXDTDHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI SAXDTDHandler_Release(ISAXDTDHandler *iface)
{
    mxwriter *This = impl_from_ISAXDTDHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI SAXDTDHandler_notationDecl(ISAXDTDHandler *iface,
    const WCHAR *name, INT nname,
    const WCHAR *publicid, INT npublicid,
    const WCHAR *systemid, INT nsystemid)
{
    mxwriter *This = impl_from_ISAXDTDHandler( iface );
    FIXME("(%p)->(%s:%d, %s:%d, %s:%d): stub\n", This, debugstr_wn(name, nname), nname,
        debugstr_wn(publicid, npublicid), npublicid, debugstr_wn(systemid, nsystemid), nsystemid);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXDTDHandler_unparsedEntityDecl(ISAXDTDHandler *iface,
    const WCHAR *name, INT nname,
    const WCHAR *publicid, INT npublicid,
    const WCHAR *systemid, INT nsystemid,
    const WCHAR *notation, INT nnotation)
{
    mxwriter *This = impl_from_ISAXDTDHandler( iface );
    FIXME("(%p)->(%s:%d, %s:%d, %s:%d, %s:%d): stub\n", This, debugstr_wn(name, nname), nname,
        debugstr_wn(publicid, npublicid), npublicid, debugstr_wn(systemid, nsystemid), nsystemid,
        debugstr_wn(notation, nnotation), nnotation);
    return E_NOTIMPL;
}

static const ISAXDTDHandlerVtbl SAXDTDHandlerVtbl = {
    SAXDTDHandler_QueryInterface,
    SAXDTDHandler_AddRef,
    SAXDTDHandler_Release,
    SAXDTDHandler_notationDecl,
    SAXDTDHandler_unparsedEntityDecl
};

/*** IVBSAXDTDHandler ***/
static HRESULT WINAPI VBSAXDTDHandler_QueryInterface(IVBSAXDTDHandler *iface, REFIID riid, void **obj)
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI VBSAXDTDHandler_AddRef(IVBSAXDTDHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI VBSAXDTDHandler_Release(IVBSAXDTDHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI VBSAXDTDHandler_GetTypeInfoCount(IVBSAXDTDHandler *iface, UINT* pctinfo)
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );
    return IMXWriter_GetTypeInfoCount(&This->IMXWriter_iface, pctinfo);
}

static HRESULT WINAPI VBSAXDTDHandler_GetTypeInfo(IVBSAXDTDHandler *iface, UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo)
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );
    return IMXWriter_GetTypeInfo(&This->IMXWriter_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI VBSAXDTDHandler_GetIDsOfNames(IVBSAXDTDHandler *iface, REFIID riid, LPOLESTR* rgszNames,
    UINT cNames, LCID lcid, DISPID* rgDispId )
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );
    return IMXWriter_GetIDsOfNames(&This->IMXWriter_iface, riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI VBSAXDTDHandler_Invoke(IVBSAXDTDHandler *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
    WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr )
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );
    return IMXWriter_Invoke(&This->IMXWriter_iface, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult,
        pExcepInfo, puArgErr);
}

static HRESULT WINAPI VBSAXDTDHandler_notationDecl(IVBSAXDTDHandler *iface, BSTR *name, BSTR *publicId, BSTR *systemId)
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );

    TRACE("(%p)->(%p %p %p)\n", This, name, publicId, systemId);

    if (!name || !publicId || !systemId)
        return E_POINTER;

    return ISAXDTDHandler_notationDecl(&This->ISAXDTDHandler_iface, *name, -1, *publicId, -1, *systemId, -1);
}

static HRESULT WINAPI VBSAXDTDHandler_unparsedEntityDecl(IVBSAXDTDHandler *iface, BSTR *name, BSTR *publicId,
    BSTR *systemId, BSTR *notation)
{
    mxwriter *This = impl_from_IVBSAXDTDHandler( iface );

    TRACE("(%p)->(%p %p %p %p)\n", This, name, publicId, systemId, notation);

    if (!name || !publicId || !systemId || !notation)
        return E_POINTER;

    return ISAXDTDHandler_unparsedEntityDecl(&This->ISAXDTDHandler_iface, *name, -1, *publicId, -1,
        *systemId, -1, *notation, -1);
}

static const IVBSAXDTDHandlerVtbl VBSAXDTDHandlerVtbl = {
    VBSAXDTDHandler_QueryInterface,
    VBSAXDTDHandler_AddRef,
    VBSAXDTDHandler_Release,
    VBSAXDTDHandler_GetTypeInfoCount,
    VBSAXDTDHandler_GetTypeInfo,
    VBSAXDTDHandler_GetIDsOfNames,
    VBSAXDTDHandler_Invoke,
    VBSAXDTDHandler_notationDecl,
    VBSAXDTDHandler_unparsedEntityDecl
};

/* ISAXErrorHandler */
static HRESULT WINAPI SAXErrorHandler_QueryInterface(ISAXErrorHandler *iface, REFIID riid, void **obj)
{
    mxwriter *This = impl_from_ISAXErrorHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI SAXErrorHandler_AddRef(ISAXErrorHandler *iface)
{
    mxwriter *This = impl_from_ISAXErrorHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI SAXErrorHandler_Release(ISAXErrorHandler *iface)
{
    mxwriter *This = impl_from_ISAXErrorHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI SAXErrorHandler_error(ISAXErrorHandler *iface,
    ISAXLocator *locator, const WCHAR *message, HRESULT hr)
{
    mxwriter *This = impl_from_ISAXErrorHandler( iface );

    FIXME("(%p)->(%p %s 0x%08x)\n", This, locator, debugstr_w(message), hr);

    return E_NOTIMPL;
}

static HRESULT WINAPI SAXErrorHandler_fatalError(ISAXErrorHandler *iface,
    ISAXLocator *locator, const WCHAR *message, HRESULT hr)
{
    mxwriter *This = impl_from_ISAXErrorHandler( iface );

    FIXME("(%p)->(%p %s 0x%08x)\n", This, locator, debugstr_w(message), hr);

    return E_NOTIMPL;
}

static HRESULT WINAPI SAXErrorHandler_ignorableWarning(ISAXErrorHandler *iface,
    ISAXLocator *locator, const WCHAR *message, HRESULT hr)
{
    mxwriter *This = impl_from_ISAXErrorHandler( iface );

    FIXME("(%p)->(%p %s 0x%08x)\n", This, locator, debugstr_w(message), hr);

    return E_NOTIMPL;
}

static const ISAXErrorHandlerVtbl SAXErrorHandlerVtbl = {
    SAXErrorHandler_QueryInterface,
    SAXErrorHandler_AddRef,
    SAXErrorHandler_Release,
    SAXErrorHandler_error,
    SAXErrorHandler_fatalError,
    SAXErrorHandler_ignorableWarning
};

/*** IVBSAXErrorHandler ***/
static HRESULT WINAPI VBSAXErrorHandler_QueryInterface(IVBSAXErrorHandler *iface, REFIID riid, void **obj)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    return IMXWriter_QueryInterface(&This->IMXWriter_iface, riid, obj);
}

static ULONG WINAPI VBSAXErrorHandler_AddRef(IVBSAXErrorHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    return IMXWriter_AddRef(&This->IMXWriter_iface);
}

static ULONG WINAPI VBSAXErrorHandler_Release(IVBSAXErrorHandler *iface)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    return IMXWriter_Release(&This->IMXWriter_iface);
}

static HRESULT WINAPI VBSAXErrorHandler_GetTypeInfoCount(IVBSAXErrorHandler *iface, UINT* pctinfo)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    return IMXWriter_GetTypeInfoCount(&This->IMXWriter_iface, pctinfo);
}

static HRESULT WINAPI VBSAXErrorHandler_GetTypeInfo(IVBSAXErrorHandler *iface, UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    return IMXWriter_GetTypeInfo(&This->IMXWriter_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI VBSAXErrorHandler_GetIDsOfNames(IVBSAXErrorHandler *iface, REFIID riid, LPOLESTR* rgszNames,
    UINT cNames, LCID lcid, DISPID* rgDispId )
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    return IMXWriter_GetIDsOfNames(&This->IMXWriter_iface, riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI VBSAXErrorHandler_Invoke(IVBSAXErrorHandler *iface, DISPID dispIdMember, REFIID riid, LCID lcid,
    WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr )
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    return IMXWriter_Invoke(&This->IMXWriter_iface, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult,
        pExcepInfo, puArgErr);
}

static HRESULT WINAPI VBSAXErrorHandler_error(IVBSAXErrorHandler *iface, IVBSAXLocator *locator, BSTR *message, LONG code)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    FIXME("(%p)->(%p %p %x): stub\n", This, locator, message, code);
    return E_NOTIMPL;
}

static HRESULT WINAPI VBSAXErrorHandler_fatalError(IVBSAXErrorHandler *iface, IVBSAXLocator *locator, BSTR *message, LONG code)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    FIXME("(%p)->(%p %p %x): stub\n", This, locator, message, code);
    return E_NOTIMPL;
}

static HRESULT WINAPI VBSAXErrorHandler_ignorableWarning(IVBSAXErrorHandler *iface, IVBSAXLocator *locator, BSTR *message, LONG code)
{
    mxwriter *This = impl_from_IVBSAXErrorHandler( iface );
    FIXME("(%p)->(%p %p %x): stub\n", This, locator, message, code);
    return E_NOTIMPL;
}

static const IVBSAXErrorHandlerVtbl VBSAXErrorHandlerVtbl = {
    VBSAXErrorHandler_QueryInterface,
    VBSAXErrorHandler_AddRef,
    VBSAXErrorHandler_Release,
    VBSAXErrorHandler_GetTypeInfoCount,
    VBSAXErrorHandler_GetTypeInfo,
    VBSAXErrorHandler_GetIDsOfNames,
    VBSAXErrorHandler_Invoke,
    VBSAXErrorHandler_error,
    VBSAXErrorHandler_fatalError,
    VBSAXErrorHandler_ignorableWarning
};

static const tid_t mxwriter_iface_tids[] = {
    IMXWriter_tid,
    0
};

static dispex_static_data_t mxwriter_dispex = {
    NULL,
    IMXWriter_tid,
    NULL,
    mxwriter_iface_tids
};

HRESULT MXWriter_create(MSXML_VERSION version, void **ppObj)
{
    static const WCHAR version10W[] = {'1','.','0',0};
    mxwriter *This;
    HRESULT hr;

    TRACE("(%p)\n", ppObj);

    This = heap_alloc( sizeof (*This) );
    if(!This)
        return E_OUTOFMEMORY;

    This->IMXWriter_iface.lpVtbl = &MXWriterVtbl;
    This->ISAXContentHandler_iface.lpVtbl = &SAXContentHandlerVtbl;
    This->ISAXLexicalHandler_iface.lpVtbl = &SAXLexicalHandlerVtbl;
    This->ISAXDeclHandler_iface.lpVtbl = &SAXDeclHandlerVtbl;
    This->ISAXDTDHandler_iface.lpVtbl = &SAXDTDHandlerVtbl;
    This->ISAXErrorHandler_iface.lpVtbl = &SAXErrorHandlerVtbl;
    This->IVBSAXDeclHandler_iface.lpVtbl = &VBSAXDeclHandlerVtbl;
    This->IVBSAXLexicalHandler_iface.lpVtbl = &VBSAXLexicalHandlerVtbl;
    This->IVBSAXContentHandler_iface.lpVtbl = &VBSAXContentHandlerVtbl;
    This->IVBSAXDTDHandler_iface.lpVtbl = &VBSAXDTDHandlerVtbl;
    This->IVBSAXErrorHandler_iface.lpVtbl = &VBSAXErrorHandlerVtbl;
    This->ref = 1;
    This->class_version = version;

    This->props[MXWriter_BOM] = VARIANT_TRUE;
    This->props[MXWriter_DisableEscaping] = VARIANT_FALSE;
    This->props[MXWriter_Indent] = VARIANT_FALSE;
    This->props[MXWriter_OmitXmlDecl] = VARIANT_FALSE;
    This->props[MXWriter_Standalone] = VARIANT_FALSE;
    This->prop_changed = FALSE;
    This->encoding = SysAllocString(utf16W);
    This->version  = SysAllocString(version10W);
    This->xml_enc  = XmlEncoding_UTF16;

    This->element = NULL;
    This->cdata = FALSE;
    This->indent = 0;
    This->text = FALSE;
    This->newline = FALSE;

    This->dest = NULL;
    This->dest_written = 0;

    hr = alloc_output_buffer(This->xml_enc, &This->buffer);
    if (hr != S_OK) {
        SysFreeString(This->encoding);
        SysFreeString(This->version);
        heap_free(This);
        return hr;
    }

    init_dispex(&This->dispex, (IUnknown*)&This->IMXWriter_iface, &mxwriter_dispex);

    *ppObj = &This->IMXWriter_iface;

    TRACE("returning iface %p\n", *ppObj);

    return S_OK;
}

static HRESULT WINAPI MXAttributes_QueryInterface(IMXAttributes *iface, REFIID riid, void **ppObj)
{
    mxattributes *This = impl_from_IMXAttributes( iface );

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid( riid ), ppObj);

    *ppObj = NULL;

    if ( IsEqualGUID( riid, &IID_IUnknown )  ||
         IsEqualGUID( riid, &IID_IDispatch ) ||
         IsEqualGUID( riid, &IID_IMXAttributes ))
    {
        *ppObj = iface;
    }
    else if ( IsEqualGUID( riid, &IID_ISAXAttributes ))
    {
        *ppObj = &This->ISAXAttributes_iface;
    }
    else if ( IsEqualGUID( riid, &IID_IVBSAXAttributes ))
    {
        *ppObj = &This->IVBSAXAttributes_iface;
    }
    else if (dispex_query_interface(&This->dispex, riid, ppObj))
    {
        return *ppObj ? S_OK : E_NOINTERFACE;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    IMXAttributes_AddRef( iface );

    return S_OK;
}

static ULONG WINAPI MXAttributes_AddRef(IMXAttributes *iface)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    ULONG ref = InterlockedIncrement( &This->ref );
    TRACE("(%p)->(%d)\n", This, ref );
    return ref;
}

static ULONG WINAPI MXAttributes_Release(IMXAttributes *iface)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    LONG ref = InterlockedDecrement( &This->ref );

    TRACE("(%p)->(%d)\n", This, ref);

    if (ref == 0)
    {
        int i;

        for (i = 0; i < This->length; i++)
        {
            SysFreeString(This->attr[i].qname);
            SysFreeString(This->attr[i].local);
            SysFreeString(This->attr[i].uri);
            SysFreeString(This->attr[i].type);
            SysFreeString(This->attr[i].value);
        }

        release_dispex(&This->dispex);
        heap_free(This->attr);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI MXAttributes_GetTypeInfoCount(IMXAttributes *iface, UINT* pctinfo)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI MXAttributes_GetTypeInfo(IMXAttributes *iface, UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI MXAttributes_GetIDsOfNames(
    IMXAttributes *iface,
    REFIID riid,
    LPOLESTR* rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID* rgDispId)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    return IDispatchEx_GetIDsOfNames(&This->dispex.IDispatchEx_iface,
        riid, rgszNames, cNames, lcid, rgDispId);
}

static HRESULT WINAPI MXAttributes_Invoke(
    IMXAttributes *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS* pDispParams,
    VARIANT* pVarResult,
    EXCEPINFO* pExcepInfo,
    UINT* puArgErr)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface,
        dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI MXAttributes_addAttribute(IMXAttributes *iface,
    BSTR uri, BSTR localName, BSTR QName, BSTR type, BSTR value)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    mxattribute *attr;
    HRESULT hr;

    TRACE("(%p)->(%s %s %s %s %s)\n", This, debugstr_w(uri), debugstr_w(localName),
        debugstr_w(QName), debugstr_w(type), debugstr_w(value));

    if ((!uri || !localName || !QName || !type || !value) && This->class_version != MSXML6)
        return E_INVALIDARG;

    /* ensure array is large enough */
    hr = mxattributes_grow(This);
    if (hr != S_OK) return hr;

    attr = &This->attr[This->length];

    attr->qname = SysAllocString(QName);
    attr->local = SysAllocString(localName);
    attr->uri   = SysAllocString(uri);
    attr->type  = SysAllocString(type ? type : emptyW);
    attr->value = SysAllocString(value);
    This->length++;

    return S_OK;
}

static HRESULT WINAPI MXAttributes_addAttributeFromIndex(IMXAttributes *iface,
    VARIANT atts, int index)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    FIXME("(%p)->(%s %d): stub\n", This, debugstr_variant(&atts), index);
    return E_NOTIMPL;
}

static HRESULT WINAPI MXAttributes_clear(IMXAttributes *iface)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    int i;

    TRACE("(%p)\n", This);

    for (i = 0; i < This->length; i++)
    {
        SysFreeString(This->attr[i].qname);
        SysFreeString(This->attr[i].local);
        SysFreeString(This->attr[i].uri);
        SysFreeString(This->attr[i].type);
        SysFreeString(This->attr[i].value);
        memset(&This->attr[i], 0, sizeof(mxattribute));
    }

    This->length = 0;

    return S_OK;
}

static mxattribute *get_attribute_byindex(mxattributes *attrs, int index)
{
    if (index < 0 || index >= attrs->length) return NULL;
    return &attrs->attr[index];
}

static HRESULT WINAPI MXAttributes_removeAttribute(IMXAttributes *iface, int index)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    mxattribute *dst;

    TRACE("(%p)->(%d)\n", This, index);

    if (!(dst = get_attribute_byindex(This, index))) return E_INVALIDARG;

    /* no need to remove last attribute, just make it inaccessible */
    if (index + 1 == This->length)
    {
        This->length--;
        return S_OK;
    }

    memmove(dst, dst + 1, (This->length-index-1)*sizeof(*dst));
    This->length--;

    return S_OK;
}

static HRESULT WINAPI MXAttributes_setAttribute(IMXAttributes *iface, int index,
    BSTR uri, BSTR localName, BSTR QName, BSTR type, BSTR value)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    FIXME("(%p)->(%d %s %s %s %s %s): stub\n", This, index, debugstr_w(uri),
        debugstr_w(localName), debugstr_w(QName), debugstr_w(type), debugstr_w(value));
    return E_NOTIMPL;
}

static HRESULT WINAPI MXAttributes_setAttributes(IMXAttributes *iface, VARIANT atts)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    FIXME("(%p)->(%s): stub\n", This, debugstr_variant(&atts));
    return E_NOTIMPL;
}

static HRESULT WINAPI MXAttributes_setLocalName(IMXAttributes *iface, int index,
    BSTR localName)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    mxattribute *attr;

    TRACE("(%p)->(%d %s)\n", This, index, debugstr_w(localName));

    if (!(attr = get_attribute_byindex(This, index))) return E_INVALIDARG;

    SysFreeString(attr->local);
    attr->local = SysAllocString(localName);

    return S_OK;
}

static HRESULT WINAPI MXAttributes_setQName(IMXAttributes *iface, int index, BSTR QName)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    mxattribute *attr;

    TRACE("(%p)->(%d %s)\n", This, index, debugstr_w(QName));

    if (!(attr = get_attribute_byindex(This, index))) return E_INVALIDARG;

    SysFreeString(attr->qname);
    attr->qname = SysAllocString(QName);

    return S_OK;
}

static HRESULT WINAPI MXAttributes_setURI(IMXAttributes *iface, int index, BSTR uri)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    mxattribute *attr;

    TRACE("(%p)->(%d %s)\n", This, index, debugstr_w(uri));

    if (!(attr = get_attribute_byindex(This, index))) return E_INVALIDARG;

    SysFreeString(attr->uri);
    attr->uri = SysAllocString(uri);

    return S_OK;
}

static HRESULT WINAPI MXAttributes_setValue(IMXAttributes *iface, int index, BSTR value)
{
    mxattributes *This = impl_from_IMXAttributes( iface );
    mxattribute *attr;

    TRACE("(%p)->(%d %s)\n", This, index, debugstr_w(value));

    if (!(attr = get_attribute_byindex(This, index))) return E_INVALIDARG;

    SysFreeString(attr->value);
    attr->value = SysAllocString(value);

    return S_OK;
}

static const IMXAttributesVtbl MXAttributesVtbl = {
    MXAttributes_QueryInterface,
    MXAttributes_AddRef,
    MXAttributes_Release,
    MXAttributes_GetTypeInfoCount,
    MXAttributes_GetTypeInfo,
    MXAttributes_GetIDsOfNames,
    MXAttributes_Invoke,
    MXAttributes_addAttribute,
    MXAttributes_addAttributeFromIndex,
    MXAttributes_clear,
    MXAttributes_removeAttribute,
    MXAttributes_setAttribute,
    MXAttributes_setAttributes,
    MXAttributes_setLocalName,
    MXAttributes_setQName,
    MXAttributes_setURI,
    MXAttributes_setValue
};

static HRESULT WINAPI SAXAttributes_QueryInterface(ISAXAttributes *iface, REFIID riid, void **ppObj)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    return IMXAttributes_QueryInterface(&This->IMXAttributes_iface, riid, ppObj);
}

static ULONG WINAPI SAXAttributes_AddRef(ISAXAttributes *iface)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    return IMXAttributes_AddRef(&This->IMXAttributes_iface);
}

static ULONG WINAPI SAXAttributes_Release(ISAXAttributes *iface)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    return IMXAttributes_Release(&This->IMXAttributes_iface);
}

static HRESULT WINAPI SAXAttributes_getLength(ISAXAttributes *iface, int *length)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    TRACE("(%p)->(%p)\n", This, length);

    if (!length && (This->class_version == MSXML_DEFAULT || This->class_version == MSXML3))
       return E_POINTER;

    *length = This->length;

    return S_OK;
}

static HRESULT WINAPI SAXAttributes_getURI(ISAXAttributes *iface, int index, const WCHAR **uri,
    int *len)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );

    TRACE("(%p)->(%d %p %p)\n", This, index, uri, len);

    if (index >= This->length || index < 0) return E_INVALIDARG;
    if (!uri || !len) return E_POINTER;

    *len = SysStringLen(This->attr[index].uri);
    *uri = This->attr[index].uri;

    return S_OK;
}

static HRESULT WINAPI SAXAttributes_getLocalName(ISAXAttributes *iface, int index, const WCHAR **name,
    int *len)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );

    TRACE("(%p)->(%d %p %p)\n", This, index, name, len);

    if (index >= This->length || index < 0) return E_INVALIDARG;
    if (!name || !len) return E_POINTER;

    *len = SysStringLen(This->attr[index].local);
    *name = This->attr[index].local;

    return S_OK;
}

static HRESULT WINAPI SAXAttributes_getQName(ISAXAttributes *iface, int index, const WCHAR **qname, int *length)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );

    TRACE("(%p)->(%d %p %p)\n", This, index, qname, length);

    if (index >= This->length) return E_INVALIDARG;
    if (!qname || !length) return E_POINTER;

    *qname = This->attr[index].qname;
    *length = SysStringLen(This->attr[index].qname);

    return S_OK;
}

static HRESULT WINAPI SAXAttributes_getName(ISAXAttributes *iface, int index, const WCHAR **uri, int *uri_len,
    const WCHAR **local, int *local_len, const WCHAR **qname, int *qname_len)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );

    TRACE("(%p)->(%d %p %p %p %p %p %p)\n", This, index, uri, uri_len, local, local_len, qname, qname_len);

    if (index >= This->length || index < 0)
        return E_INVALIDARG;

    if (!uri || !uri_len || !local || !local_len || !qname || !qname_len)
        return E_POINTER;

    *uri_len = SysStringLen(This->attr[index].uri);
    *uri = This->attr[index].uri;

    *local_len = SysStringLen(This->attr[index].local);
    *local = This->attr[index].local;

    *qname_len = SysStringLen(This->attr[index].qname);
    *qname = This->attr[index].qname;

    TRACE("(%s, %s, %s)\n", debugstr_w(*uri), debugstr_w(*local), debugstr_w(*qname));

    return S_OK;
}

static HRESULT WINAPI SAXAttributes_getIndexFromName(ISAXAttributes *iface, const WCHAR *uri, int uri_len,
    const WCHAR *name, int len, int *index)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    int i;

    TRACE("(%p)->(%s:%d %s:%d %p)\n", This, debugstr_wn(uri, uri_len), uri_len,
        debugstr_wn(name, len), len, index);

    if (!index && (This->class_version == MSXML_DEFAULT || This->class_version == MSXML3))
        return E_POINTER;

    if (!uri || !name || !index) return E_INVALIDARG;

    for (i = 0; i < This->length; i++)
    {
        if (uri_len != SysStringLen(This->attr[i].uri)) continue;
        if (strncmpW(uri, This->attr[i].uri, uri_len)) continue;

        if (len != SysStringLen(This->attr[i].local)) continue;
        if (strncmpW(name, This->attr[i].local, len)) continue;

        *index = i;
        return S_OK;
    }

    return E_INVALIDARG;
}

static HRESULT WINAPI SAXAttributes_getIndexFromQName(ISAXAttributes *iface, const WCHAR *qname,
    int len, int *index)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    int i;

    TRACE("(%p)->(%s:%d %p)\n", This, debugstr_wn(qname, len), len, index);

    if (!index && (This->class_version == MSXML_DEFAULT || This->class_version == MSXML3))
        return E_POINTER;

    if (!qname || !index || !len) return E_INVALIDARG;

    for (i = 0; i < This->length; i++)
    {
        if (len != SysStringLen(This->attr[i].qname)) continue;
        if (strncmpW(qname, This->attr[i].qname, len)) continue;

        *index = i;
        return S_OK;
    }

    return E_INVALIDARG;
}

static HRESULT WINAPI SAXAttributes_getType(ISAXAttributes *iface, int index, const WCHAR **type,
    int *len)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );

    TRACE("(%p)->(%d %p %p)\n", This, index, type, len);

    if (index >= This->length) return E_INVALIDARG;

    if ((!type || !len) && (This->class_version == MSXML_DEFAULT || This->class_version == MSXML3))
       return E_POINTER;

    *type = This->attr[index].type;
    *len = SysStringLen(This->attr[index].type);

    return S_OK;
}

static HRESULT WINAPI SAXAttributes_getTypeFromName(ISAXAttributes *iface, const WCHAR * pUri, int nUri,
    const WCHAR * pLocalName, int nLocalName, const WCHAR ** pType, int * nType)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    FIXME("(%p)->(%s:%d %s:%d %p %p): stub\n", This, debugstr_wn(pUri, nUri), nUri,
        debugstr_wn(pLocalName, nLocalName), nLocalName, pType, nType);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXAttributes_getTypeFromQName(ISAXAttributes *iface, const WCHAR * pQName,
    int nQName, const WCHAR ** pType, int * nType)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    FIXME("(%p)->(%s:%d %p %p): stub\n", This, debugstr_wn(pQName, nQName), nQName, pType, nType);
    return E_NOTIMPL;
}

static HRESULT WINAPI SAXAttributes_getValue(ISAXAttributes *iface, int index, const WCHAR **value,
    int *len)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );

    TRACE("(%p)->(%d %p %p)\n", This, index, value, len);

    if (index >= This->length) return E_INVALIDARG;

    if ((!value || !len) && (This->class_version == MSXML_DEFAULT || This->class_version == MSXML3))
       return E_POINTER;

    *value = This->attr[index].value;
    *len = SysStringLen(This->attr[index].value);

    return S_OK;
}

static HRESULT WINAPI SAXAttributes_getValueFromName(ISAXAttributes *iface, const WCHAR *uri,
    int uri_len, const WCHAR *name, int name_len, const WCHAR **value, int *value_len)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    HRESULT hr;
    int index;

    TRACE("(%p)->(%s:%d %s:%d %p %p)\n", This, debugstr_wn(uri, uri_len), uri_len,
        debugstr_wn(name, name_len), name_len, value, value_len);

    if (!uri || !name || !value || !value_len)
        return (This->class_version == MSXML_DEFAULT || This->class_version == MSXML3) ? E_POINTER : E_INVALIDARG;

    hr = ISAXAttributes_getIndexFromName(iface, uri, uri_len, name, name_len, &index);
    if (hr == S_OK)
        hr = ISAXAttributes_getValue(iface, index, value, value_len);

    return hr;
}

static HRESULT WINAPI SAXAttributes_getValueFromQName(ISAXAttributes *iface, const WCHAR *qname,
    int qname_len, const WCHAR **value, int *value_len)
{
    mxattributes *This = impl_from_ISAXAttributes( iface );
    HRESULT hr;
    int index;

    TRACE("(%p)->(%s:%d %p %p)\n", This, debugstr_wn(qname, qname_len), qname_len, value, value_len);

    if (!qname || !value || !value_len)
        return (This->class_version == MSXML_DEFAULT || This->class_version == MSXML3) ? E_POINTER : E_INVALIDARG;

    hr = ISAXAttributes_getIndexFromQName(iface, qname, qname_len, &index);
    if (hr == S_OK)
        hr = ISAXAttributes_getValue(iface, index, value, value_len);

    return hr;
}

static const ISAXAttributesVtbl SAXAttributesVtbl = {
    SAXAttributes_QueryInterface,
    SAXAttributes_AddRef,
    SAXAttributes_Release,
    SAXAttributes_getLength,
    SAXAttributes_getURI,
    SAXAttributes_getLocalName,
    SAXAttributes_getQName,
    SAXAttributes_getName,
    SAXAttributes_getIndexFromName,
    SAXAttributes_getIndexFromQName,
    SAXAttributes_getType,
    SAXAttributes_getTypeFromName,
    SAXAttributes_getTypeFromQName,
    SAXAttributes_getValue,
    SAXAttributes_getValueFromName,
    SAXAttributes_getValueFromQName
};

static HRESULT WINAPI VBSAXAttributes_QueryInterface(
        IVBSAXAttributes* iface,
        REFIID riid,
        void **ppvObject)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    TRACE("%p %s %p\n", This, debugstr_guid(riid), ppvObject);
    return ISAXAttributes_QueryInterface(&This->ISAXAttributes_iface, riid, ppvObject);
}

static ULONG WINAPI VBSAXAttributes_AddRef(IVBSAXAttributes* iface)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_AddRef(&This->ISAXAttributes_iface);
}

static ULONG WINAPI VBSAXAttributes_Release(IVBSAXAttributes* iface)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_Release(&This->ISAXAttributes_iface);
}

static HRESULT WINAPI VBSAXAttributes_GetTypeInfoCount( IVBSAXAttributes *iface, UINT* pctinfo )
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );

    TRACE("(%p)->(%p)\n", This, pctinfo);

    *pctinfo = 1;

    return S_OK;
}

static HRESULT WINAPI VBSAXAttributes_GetTypeInfo(
    IVBSAXAttributes *iface,
    UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo )
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    TRACE("(%p)->(%u %u %p)\n", This, iTInfo, lcid, ppTInfo);
    return get_typeinfo(IVBSAXAttributes_tid, ppTInfo);
}

static HRESULT WINAPI VBSAXAttributes_GetIDsOfNames(
    IVBSAXAttributes *iface,
    REFIID riid,
    LPOLESTR* rgszNames,
    UINT cNames,
    LCID lcid,
    DISPID* rgDispId)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%s %p %u %u %p)\n", This, debugstr_guid(riid), rgszNames, cNames,
          lcid, rgDispId);

    if(!rgszNames || cNames == 0 || !rgDispId)
        return E_INVALIDARG;

    hr = get_typeinfo(IVBSAXAttributes_tid, &typeinfo);
    if(SUCCEEDED(hr))
    {
        hr = ITypeInfo_GetIDsOfNames(typeinfo, rgszNames, cNames, rgDispId);
        ITypeInfo_Release(typeinfo);
    }

    return hr;
}

static HRESULT WINAPI VBSAXAttributes_Invoke(
    IVBSAXAttributes *iface,
    DISPID dispIdMember,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS* pDispParams,
    VARIANT* pVarResult,
    EXCEPINFO* pExcepInfo,
    UINT* puArgErr)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    ITypeInfo *typeinfo;
    HRESULT hr;

    TRACE("(%p)->(%d %s %d %d %p %p %p %p)\n", This, dispIdMember, debugstr_guid(riid),
          lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(IVBSAXAttributes_tid, &typeinfo);
    if(SUCCEEDED(hr))
    {
        hr = ITypeInfo_Invoke(typeinfo, &This->IVBSAXAttributes_iface, dispIdMember, wFlags,
                pDispParams, pVarResult, pExcepInfo, puArgErr);
        ITypeInfo_Release(typeinfo);
    }

    return hr;
}

static HRESULT WINAPI VBSAXAttributes_get_length(IVBSAXAttributes* iface, int *len)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getLength(&This->ISAXAttributes_iface, len);
}

static HRESULT WINAPI VBSAXAttributes_getURI(IVBSAXAttributes* iface, int index, BSTR *uri)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getURI(&This->ISAXAttributes_iface, index, (const WCHAR**)uri, &len);
}

static HRESULT WINAPI VBSAXAttributes_getLocalName(IVBSAXAttributes* iface, int index, BSTR *name)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getLocalName(&This->ISAXAttributes_iface, index, (const WCHAR**)name, &len);
}

static HRESULT WINAPI VBSAXAttributes_getQName(IVBSAXAttributes* iface, int index, BSTR *qname)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getQName(&This->ISAXAttributes_iface, index, (const WCHAR**)qname, &len);
}

static HRESULT WINAPI VBSAXAttributes_getIndexFromName(IVBSAXAttributes* iface, BSTR uri, BSTR name, int *index)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getIndexFromName(&This->ISAXAttributes_iface, uri, SysStringLen(uri),
            name, SysStringLen(name), index);
}

static HRESULT WINAPI VBSAXAttributes_getIndexFromQName(IVBSAXAttributes* iface, BSTR qname, int *index)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    return ISAXAttributes_getIndexFromQName(&This->ISAXAttributes_iface, qname,
            SysStringLen(qname), index);
}

static HRESULT WINAPI VBSAXAttributes_getType(IVBSAXAttributes* iface, int index,BSTR *type)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getType(&This->ISAXAttributes_iface, index, (const WCHAR**)type, &len);
}

static HRESULT WINAPI VBSAXAttributes_getTypeFromName(IVBSAXAttributes* iface, BSTR uri,
    BSTR name, BSTR *type)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getTypeFromName(&This->ISAXAttributes_iface, uri, SysStringLen(uri),
            name, SysStringLen(name), (const WCHAR**)type, &len);
}

static HRESULT WINAPI VBSAXAttributes_getTypeFromQName(IVBSAXAttributes* iface, BSTR qname, BSTR *type)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getTypeFromQName(&This->ISAXAttributes_iface, qname, SysStringLen(qname),
            (const WCHAR**)type, &len);
}

static HRESULT WINAPI VBSAXAttributes_getValue(IVBSAXAttributes* iface, int index, BSTR *value)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getValue(&This->ISAXAttributes_iface, index, (const WCHAR**)value, &len);
}

static HRESULT WINAPI VBSAXAttributes_getValueFromName(IVBSAXAttributes* iface, BSTR uri, BSTR name,
    BSTR *value)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getValueFromName(&This->ISAXAttributes_iface, uri, SysStringLen(uri),
            name, SysStringLen(name), (const WCHAR**)value, &len);
}

static HRESULT WINAPI VBSAXAttributes_getValueFromQName(IVBSAXAttributes* iface, BSTR qname, BSTR *value)
{
    mxattributes *This = impl_from_IVBSAXAttributes( iface );
    int len;

    return ISAXAttributes_getValueFromQName(&This->ISAXAttributes_iface, qname, SysStringLen(qname),
        (const WCHAR**)value, &len);
}

static const struct IVBSAXAttributesVtbl VBSAXAttributesVtbl =
{
    VBSAXAttributes_QueryInterface,
    VBSAXAttributes_AddRef,
    VBSAXAttributes_Release,
    VBSAXAttributes_GetTypeInfoCount,
    VBSAXAttributes_GetTypeInfo,
    VBSAXAttributes_GetIDsOfNames,
    VBSAXAttributes_Invoke,
    VBSAXAttributes_get_length,
    VBSAXAttributes_getURI,
    VBSAXAttributes_getLocalName,
    VBSAXAttributes_getQName,
    VBSAXAttributes_getIndexFromName,
    VBSAXAttributes_getIndexFromQName,
    VBSAXAttributes_getType,
    VBSAXAttributes_getTypeFromName,
    VBSAXAttributes_getTypeFromQName,
    VBSAXAttributes_getValue,
    VBSAXAttributes_getValueFromName,
    VBSAXAttributes_getValueFromQName
};

static const tid_t mxattrs_iface_tids[] = {
    IMXAttributes_tid,
    0
};

static dispex_static_data_t mxattrs_dispex = {
    NULL,
    IMXAttributes_tid,
    NULL,
    mxattrs_iface_tids
};

HRESULT SAXAttributes_create(MSXML_VERSION version, void **ppObj)
{
    static const int default_count = 10;
    mxattributes *This;

    TRACE("(%p)\n", ppObj);

    This = heap_alloc( sizeof (*This) );
    if( !This )
        return E_OUTOFMEMORY;

    This->IMXAttributes_iface.lpVtbl = &MXAttributesVtbl;
    This->ISAXAttributes_iface.lpVtbl = &SAXAttributesVtbl;
    This->IVBSAXAttributes_iface.lpVtbl = &VBSAXAttributesVtbl;
    This->ref = 1;

    This->class_version = version;

    This->attr = heap_alloc(default_count*sizeof(mxattribute));
    This->length = 0;
    This->allocated = default_count;

    *ppObj = &This->IMXAttributes_iface;

    init_dispex(&This->dispex, (IUnknown*)&This->IMXAttributes_iface, &mxattrs_dispex);

    TRACE("returning iface %p\n", *ppObj);

    return S_OK;
}
