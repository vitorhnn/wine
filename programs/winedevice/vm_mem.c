#include <unicorn/unicorn.h>

#include "winedevice_private.h"

/* returns host pointer to VM-Mapped Memory */
void* vm_alloc(size_t size, uint32_t perms, const char *label)
{
    void *ptr = heap_alloc(size);
    uint64_t vm_addr = vm_address(ptr);
    assert(!(uc_mem_map_ptr(g_engine, ptr, size, perms, vm_addr)));
    if (label)
    {
        /* TODO: Hook memory and display accesses to it w/ label for debugging */
    }
    return ptr;
}

/* Unmaps and frees VM mapped memory */
void unmap_from_vm(void *ptr, size_t size)
{
    uc_mem_unmap(g_engine, vm_address(ptr), size);
    free(ptr);
}