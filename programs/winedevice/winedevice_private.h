#pragma once

extern uc_engine *g_engine;

const uint64_t user_space_start = 0x10000;
const uint64_t user_space_end = 0x7fffffff0000;
const uint64_t user_space_size = user_space_end - user_space_start;
const uint64_t system_space_start = 0xffff080000000000;

inline uint64_t vm_address(void *host_ptr)
{
    return system_space_start + (uint64_t) host_ptr;
}

inline void *host_address(uint64_t vm_ptr)
{
    return (void*)(vm_ptr - system_space_start);
}

void* vm_alloc(size_t size, uint32_t perms, const char *label);
void unmap_from_vm(void *ptr, size_t size);

typedef struct vm_thread;

// VM_CALL(thread, ntoskrnl_mod, NtCreateFile)(param_1, param_2)
// VM_CALL_SEND();
#define VM_CALL(thread, module, func)  \
    do \
    { \
        typeof(func) *get_ctx = GetThreadContext; \
        struct vm_thread *thread = thread;\
        void *func_ptr = vm_get_proc_addr(module, #func);\
        CONTEXT *ctx = get_ctx

#define VM_CALL_SEND(ret) \
        vm_thread_run_func(thread, (uint64_t) func_ptr, ctx, &ret) \
    }while(0)
        

int initialize_vm(void);
void shutdown_vm(void);