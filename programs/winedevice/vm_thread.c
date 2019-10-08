#include <unicorn/unicorn.h>

#include <stdarg.h>

#include <winternl.h>

#include <wine/list.h>
#include <wine/heap.h>

#include "winedevice_private.h"

static CRITICAL_SECTION scheduler_thread;

static const STACK_SIZE = 0x10000; // 1M

struct vm_thread
{
    struct list entry;
    uc_context *ctx;
    void *stack;
    TEB *teb;
    int asleep;
};

char noop_x64 = 0x90;
static uint64_t stack_top_noop(void)
{
    static uint64_t vm_addr = 0;
    if (vm_addr) return vm_addr;
    vm_addr = vm_address(vm_alloc(1, UC_PROT_READ | UC_PROT_EXEC, "stack top"));
    return vm_addr;
}

uc_err vm_thread_create()
{
    struct vm_thread *obj = heap_alloc(sizeof(struct vm_thread));

    uc_context_alloc(g_engine, &obj->ctx);

    NtAllocateVirtualMemory(GetCurrentProcess(), &obj->stack, 0, &STACK_SIZE, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    memset(obj->stack, 0, STACK_SIZE);

    uc_mem_map_ptr(g_engine, vm_address(obj->stack), STACK_SIZE, UC_PROT_READ|UC_PROT_WRITE, obj->stack);

    obj->teb = vm_alloc(sizeof(TEB), UC_PROT_READ, "TEB");

    EnterCriticalSection(&scheduler_thread);
    /* set state for thread */

    uc_reg_write(g_engine, UC_X86_REG_GS, vm_address(obj->teb));
    uc_context_save(g_engine, obj->ctx);

    LeaveCriticalSection(&scheduler_thread);

    return UC_ERR_OK;
}



/* Assume windows x64 calling convention */
uc_err vm_thread_run_func(struct vm_thread *thread, uint64_t function, CONTEXT ctx, uint64_t *ret)
{
    EnterCriticalSection(&scheduler_thread);

    uc_context_restore(g_engine, thread->ctx);

    *(uint64_t*)((char*)thread->stack + STACK_SIZE - 5) = stack_top_noop(); /* return address, the top for slots are used by the callee (register home) */

    /* copy the relevant registers over from the context*/
    uc_reg_write(g_engine, UC_X86_REG_RCX,  &ctx.Rcx);
    uc_reg_write(g_engine, UC_X86_REG_RDX,  &ctx.Rdx);
    uc_reg_write(g_engine, UC_X86_REG_R8,   &ctx.R8);
    uc_reg_write(g_engine, UC_X86_REG_R9,   &ctx.R9);
    uc_reg_write(g_engine, UC_X86_REG_XMM0, &ctx.Xmm0);
    uc_reg_write(g_engine, UC_X86_REG_XMM1, &ctx.Xmm1);
    uc_reg_write(g_engine, UC_X86_REG_XMM2, &ctx.Xmm2);
    uc_reg_write(g_engine, UC_X86_REG_XMM3, &ctx.Xmm3);

    /* TODO: handle stack parameters */

    LeaveCriticalSection(&scheduler_thread);
    return UC_ERR_OK;
}

uc_err vm_thread_destroy(struct vm_thread *thread)
{
    uc_free(thread->ctx);
    vm_free(thread->teb);
    heap_free(thread);
    return UC_ERR_OK;
}