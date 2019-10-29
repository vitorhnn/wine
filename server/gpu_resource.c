#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "wine/unicode.h"
#include "windef.h"
#include "winternl.h"
#include "dxgi1_2.h"

#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"

struct gpu_resource
{
    struct object       obj;
    struct list         kernel_object;
    struct fd          *fd;
    obj_handle_t        kmt_handle; /* more like an ID */

    /* used by API layers to store extra information about a resource */
    void               *user_data;
    data_size_t         user_data_len;
};

/* gpu_resource functions */
static void gpu_resource_dump( struct object*, int verbose );
static struct object_type *gpu_resource_get_type( struct object *obj );
static struct fd *gpu_resource_get_fd( struct object *obj );
static unsigned int gpu_resource_map_access( struct object *obj, unsigned int access );
static void gpu_resource_destroy( struct object *obj);
static enum server_fd_type gpu_resource_get_fd_type( struct fd *fd );

static const struct object_ops gpu_resource_ops =
{
    sizeof(struct gpu_resource), /* size */
    gpu_resource_dump,           /* dump */
    gpu_resource_get_type,       /* get_type */
    no_add_queue,                /* add_queue */
    NULL,                        /* remove_queue */
    NULL,                        /* signaled */
    NULL,                        /* satisfied */
    no_signal,                   /* signal */
    gpu_resource_get_fd,         /* get_fd */
    gpu_resource_map_access,     /* map_access */
    default_get_sd,              /* get_sd */
    default_set_sd,              /* set_sd */
    no_lookup_name,              /* lookup_name */
    directory_link_name,         /* link_name */
    default_unlink_name,         /* unlink_name */
    no_open_file,                /* no_open_file */
    no_kernel_obj_list,          /* no_kernel_obj_list */
    fd_close_handle,             /* close_handle */
    gpu_resource_destroy         /* destroy */
};

static const struct fd_ops gpu_resource_fd_ops =
{
    NULL,                         /* get_poll_events */
    NULL,                         /* poll_event */
    gpu_resource_get_fd_type,     /* get_fd_type */
    no_fd_read,                   /* read */
    no_fd_write,                  /* write */
    no_fd_flush,                  /* flush */
    no_fd_get_file_info,          /* get_file_info */
    no_fd_get_volume_info,        /* get_volume_info */
    no_fd_ioctl,                  /* ioctl */
    no_fd_queue_async,            /* queue_async */
    NULL                          /* reselect_async */
};

static void gpu_resource_destroy( struct object *obj )
{
    struct gpu_resource *resource = (struct gpu_resource *) obj;
    assert( obj->ops == &gpu_resource_ops );

    assert( resource->fd );

    release_object( resource->fd );
}

static void gpu_resource_dump( struct object *obj, int verbose )
{
    struct gpu_resource *resource = (struct gpu_resource *) obj;
    assert( obj->ops == &gpu_resource_ops );
    fprintf( stderr, "GPU-Resource fd=%p", resource->fd );
}

static struct object_type *gpu_resource_get_type( struct object *obj )
{
    static const WCHAR name[] = {'D','x','g','k','S','h','a','r','e','d','R','e','s','o','u','r','c','e'};
    static const struct unicode_str str = {name, sizeof(name) };
    return get_object_type( &str );
}

static struct fd *gpu_resource_get_fd( struct object *obj )
{
    struct gpu_resource *resource = (struct gpu_resource *) obj;
    assert( obj->ops == &gpu_resource_ops );
    return (struct fd *)grab_object( resource->fd );
}

static unsigned int gpu_resource_map_access( struct object *obj, unsigned int access )
{
    if (access & GENERIC_READ) access |= DXGI_SHARED_RESOURCE_READ;
    if (access & GENERIC_WRITE) access |= DXGI_SHARED_RESOURCE_WRITE;
    if (access & GENERIC_ALL) access |= (DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE);
    return access & (DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE);
}

static enum server_fd_type gpu_resource_get_fd_type( struct fd *fd )
{
    return FD_TYPE_RESOURCE;
}

/* mostly a copy of PTID code */
struct kmt_entry
{
    void *ptr;
    unsigned int next;
};

static struct kmt_entry *kmt_entries;      /* array of kmt entries */
static unsigned int used_kmt_entries;      /* number of entries in use */
static unsigned int alloc_kmt_entries;     /* number of allocated entries */
static unsigned int next_free_kmt_entry;   /* next free entry */
static unsigned int last_free_kmt_entry;   /* last free entry */
static unsigned int num_free_kmt_entries;  /* number of free entries */

inline obj_handle_t kmt_entry_to_handle( unsigned int entry )
{
    return (obj_handle_t) (entry + 1) * 4;
}

inline unsigned int kmt_handle_to_entry( obj_handle_t handle )
{
    return (unsigned int) (handle / 4) - 1;
}

/* allocate a new kmt handle */
obj_handle_t alloc_kmt_handle( void *ptr )
{
    struct kmt_entry *entry;
    obj_handle_t kmt_handle;

    if (used_kmt_entries < alloc_kmt_entries)
    {
        kmt_handle = kmt_entry_to_handle(used_kmt_entries);
        entry = &kmt_entries[used_kmt_entries++];
    }
    else if (next_free_kmt_entry && num_free_kmt_entries >= 256)
    {
        kmt_handle = kmt_entry_to_handle(next_free_kmt_entry * 4);
        entry = &kmt_entries[next_free_kmt_entry];
        if (!(next_free_kmt_entry = entry->next)) last_free_kmt_entry = 0;
        num_free_kmt_entries--;
    }
    else  /* need to grow the array */
    {
        unsigned int count = alloc_kmt_entries + (alloc_kmt_entries / 2);
        if (!count) count = 512;
        if (!(entry = realloc( kmt_entries, count * sizeof(*entry) )))
        {
            set_error( STATUS_NO_MEMORY );
            return 0;
        }
        kmt_entries = entry;
        alloc_kmt_entries = count;
        kmt_handle = kmt_entry_to_handle(used_kmt_entries);
        entry = &kmt_entries[used_kmt_entries++];
    }

    entry->ptr = ptr;
    return kmt_handle;
}

/* free a kmt handle */
void free_kmt_handle( obj_handle_t kmt_handle )
{
    unsigned int entry_id = kmt_handle_to_entry(kmt_handle);
    struct kmt_entry *entry = &kmt_entries[entry_id];

    entry->ptr  = NULL;
    entry->next = 0;

    /* append to end of free list so that we don't reuse it too early */
    if (last_free_kmt_entry) kmt_entries[last_free_kmt_entry].next = entry_id;
    else next_free_kmt_entry = entry_id;
    last_free_kmt_entry = entry_id;
    num_free_kmt_entries++;
}

/* retrieve the resource corresponding to a kmt handle */
void *get_kmt_entry( obj_handle_t kmt_handle )
{
    if (kmt_handle < 4 || kmt_handle % 4) return NULL;
    if (kmt_handle_to_entry(kmt_handle) >= used_kmt_entries) return NULL;
    return kmt_entries[kmt_handle_to_entry(kmt_handle)].ptr;
}

struct gpu_resource *create_gpu_resource(struct object *root, const struct unicode_str *name,
                                         unsigned int attr, int fd,
                                         const struct security_descriptor *sd )
{
    struct gpu_resource *resource;

    if ((resource = create_named_object( root, &gpu_resource_ops, name, attr, sd)))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            list_init( &resource->kernel_object );
            if (!(resource->fd = create_anonymous_fd( &gpu_resource_fd_ops, fd, &resource->obj, 0)))
            {
                release_object( resource );
                return NULL;
            }
            resource->kmt_handle = alloc_kmt_handle( resource );
            resource->user_data = 0;
            allow_fd_caching( resource->fd );
        }
    }
    return resource;
}

/* Create a GPU resource object */
DECL_HANDLER(create_gpu_resource)
{
    struct gpu_resource *resource;
    int fd;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    reply->handle = 0;

    if (!objattr) return;

    if ((fd = thread_get_inflight_fd( current, req->fd )) == -1)
    {
        set_error( STATUS_INVALID_HANDLE );
        return;
    }

    if ((resource = create_gpu_resource( root, &name, objattr->attributes, fd, sd)))
    {
        if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            reply->handle = alloc_handle( current->process, resource, req->access, objattr->attributes );
        else
            reply->handle = alloc_handle_no_access_check( current->process, resource,
                                                          req->access, objattr->attributes );
        reply->kmt_handle = resource->kmt_handle;
        release_object( resource );
    }

    if (root) release_object( root );
}

/* Open a GPU Resource */
DECL_HANDLER(open_gpu_resource)
{
    if (req->kmt_handle)
    {
        struct object *obj = (struct object *) get_kmt_entry(req->kmt_handle);
        if (!obj)
        {
            set_error(STATUS_INVALID_HANDLE);
            return;
        }
        if (obj->ops != &gpu_resource_ops)
        {
            set_error(STATUS_OBJECT_TYPE_MISMATCH);
            return;
        }
        grab_object(obj);
        reply->handle = alloc_handle_no_access_check( current->process, obj, req->access, 0 );
        release_object(obj);
    } else {
        struct unicode_str name = get_req_unicode_str();

        reply->handle = open_object( current->process, req->rootdir, req->access, &gpu_resource_ops, &name, req->attributes );
    }
}

struct gpu_resource *get_resource_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    return (struct gpu_resource *)get_handle_obj( process, handle, access, &gpu_resource_ops );
}

/* Query KMT handle and user data of GPU Resource */
DECL_HANDLER(query_gpu_resource)
{
    struct gpu_resource *resource;
    data_size_t reply_size = get_reply_max_size();

    if ((resource = get_resource_obj( current->process, req->handle, 0 )))
    {
        reply->kmt_handle = resource->kmt_handle;

        if (reply_size)
        {
            if (reply_size >= resource->user_data_len)
                set_reply_data(resource->user_data, resource->user_data_len);
            else
                set_error(STATUS_BUFFER_TOO_SMALL);
        }

        release_object(resource);
    }
}

/* Sets the user data of the GPU resource */
DECL_HANDLER(set_userdata_gpu_resource)
{
    struct gpu_resource *resource;
    data_size_t len = get_req_data_size();

    if ((resource = get_resource_obj( current->process, req->handle, 0 )))
    {
        free(resource->user_data);
        resource->user_data = mem_alloc(len);
        if (resource->user_data)
        {
            memcpy(resource->user_data, get_req_data(), len);
            resource->user_data_len = len;
        }
        else
        {
            set_error(STATUS_NO_MEMORY);
        }
        
        release_object(resource);
    }
}