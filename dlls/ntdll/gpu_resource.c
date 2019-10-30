#include "config.h"

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntdll_misc.h"
#include "dxgi1_2.h"

/* Closes Staging FD afterwards if successful */
NTSTATUS CDECL __wine_create_gpu_resource(PHANDLE handle, PHANDLE kmt_handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr, int fd )
{
    NTSTATUS ret;
    data_size_t len;
    struct object_attributes *objattr;

    if ((ret = alloc_object_attributes( attr, &objattr, &len ))) return ret;

    wine_server_send_fd( fd );

    SERVER_START_REQ( create_gpu_resource )
    {
        req->access = access;
        req->fd = fd;
        wine_server_add_data( req, objattr, len );
        ret = wine_server_call( req );
        if(handle) *handle = wine_server_ptr_handle( reply->handle );
        if(kmt_handle) *kmt_handle = wine_server_ptr_handle( reply->kmt_handle );
    }
    SERVER_END_REQ;

    RtlFreeHeap( GetProcessHeap(), 0, objattr );
    if (!ret)
        close(fd);

    return ret;
}

/* Opens a GPU-resource handle from a KMT handle or name */
NTSTATUS CDECL __wine_open_gpu_resource(HANDLE kmt_handle, OBJECT_ATTRIBUTES *attr, DWORD access, PHANDLE handle )
{
    NTSTATUS ret;

    if ((kmt_handle && attr) || !handle)
        return STATUS_INVALID_PARAMETER;

    SERVER_START_REQ( open_gpu_resource )
    {
        req->access = access;
        req->kmt_handle = wine_server_obj_handle( kmt_handle );
        if (attr)
        {
            req->attributes = attr->Attributes;
            req->rootdir    = wine_server_obj_handle( attr->RootDirectory );
            if (attr->ObjectName)
                wine_server_add_data( req, attr->ObjectName->Buffer, attr->ObjectName->Length );
        }
        if (!(ret = wine_server_call( req )))
            *handle = wine_server_ptr_handle( reply->handle );
        else
            *handle = INVALID_HANDLE_VALUE;
    }
    SERVER_END_REQ;

    return ret;
}


/* Gets an FD from GPU resource handle */
NTSTATUS CDECL __wine_get_gpu_resource_fd(HANDLE handle, int *fd, int *needs_close)
{
    NTSTATUS ret;
    enum server_fd_type type;

    ret = server_get_unix_fd( handle, DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE, fd, needs_close, &type, NULL );

    if (type != FD_TYPE_RESOURCE)
    {
        if (*needs_close)
            close(*fd);
        *fd = -1;
        return STATUS_INVALID_HANDLE;
    }

    return ret;
}

/* gets KMT handle */
NTSTATUS CDECL __wine_get_gpu_resource_info(HANDLE handle, HANDLE *kmt_handle)
{
    NTSTATUS ret;

    SERVER_START_REQ(query_gpu_resource)
    {
        req->handle = wine_server_obj_handle( handle );
        if (!(ret = wine_server_call(req)))
            *kmt_handle = wine_server_ptr_handle( reply->kmt_handle );
    }
    SERVER_END_REQ;

    return ret;
}