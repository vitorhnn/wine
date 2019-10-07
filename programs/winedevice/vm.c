#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <inttypes.h>
#include <wchar.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <winternl.h>

#include <unicorn/unicorn.h>

#include "wine/debug.h"

#include "winedevice_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(winedevice);

#if defined(__x86_64__) && defined(HAVE_UNICORN)

const uint64_t user_space_start = 0x10000;
const uint64_t user_space_end = 0x7fffffff0000;
const uint64_t user_space_size = user_space_end - user_space_start;
const uint64_t system_space_start = 0xffff080000000000;

static uc_engine *engine;

/* free mapping manager */

/* convert PE image VirtualAddress to Real Address */
static inline void *get_absolute_va( HMODULE module, DWORD va )
{
    return (void *)((char *)module + va);
}

static inline uint32_t nt_to_uc_prot( ULONG nt_prot )
{
    uint32_t uc_prot;
    switch(nt_prot)
    {
        case PAGE_EXECUTE: uc_prot = UC_PROT_EXEC; break;
        case PAGE_EXECUTE_READ: uc_prot = UC_PROT_EXEC | UC_PROT_READ; break;
        case PAGE_EXECUTE_READWRITE: uc_prot = UC_PROT_EXEC | UC_PROT_READ | UC_PROT_WRITE; break;
        case PAGE_NOACCESS: uc_prot = UC_PROT_NONE; break;
        case PAGE_READONLY: uc_prot = UC_PROT_READ; break;
        case PAGE_READWRITE: uc_prot = UC_PROT_READ | UC_PROT_WRITE; break;
        case PAGE_WRITECOPY: uc_prot = UC_PROT_READ | UC_PROT_WRITE; break;
        default: uc_prot = UC_PROT_NONE;
    }
    return uc_prot;
}

NTSTATUS move_pe_into_vm(HMODULE module)
{
    /* !!temporary: */ const uint64_t vm_addr = 0xffff080000000000;

    /* relocate it to vm memory */
    IMAGE_NT_HEADERS *nt = RtlImageNtHeader(module);
    uint32_t img_size = nt->OptionalHeader.SizeOfImage;
    char *base = (char*)nt->OptionalHeader.ImageBase;
    const IMAGE_DATA_DIRECTORY *relocs = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    ULONG protect_old[96];

    if (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)
    {
        WARN( "Need to relocate module from %p to %016lx, but there are no relocation records\n",
            base, vm_addr );
        return STATUS_CONFLICTING_ADDRESSES;
    }

    if (!relocs->Size) return STATUS_SUCCESS;
    if (!relocs->VirtualAddress) return STATUS_CONFLICTING_ADDRESSES;

    if (nt->FileHeader.NumberOfSections > ARRAY_SIZE( protect_old ))
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_SECTION_HEADER *sec = (const IMAGE_SECTION_HEADER *)((const char *)&nt->OptionalHeader +
                                        nt->FileHeader.SizeOfOptionalHeader);

    for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        void *addr = get_absolute_va( module, sec[i].VirtualAddress );
        SIZE_T size = sec[i].SizeOfRawData;
        NtProtectVirtualMemory( NtCurrentProcess(), &addr,
                                &size, PAGE_READWRITE, &protect_old[i] );
    }

    TRACE( "relocating from %p-%p to %016lx-%016lx\n",
        base, base + img_size, vm_addr, vm_addr + img_size );

    IMAGE_BASE_RELOCATION *cur_reloc = get_absolute_va(module, relocs->VirtualAddress);
    IMAGE_BASE_RELOCATION *reloc_end = get_absolute_va(module, relocs->VirtualAddress + relocs->Size);

    uint64_t delta = (char*)vm_addr - base;

    while (cur_reloc < reloc_end - 1 && cur_reloc->SizeOfBlock)
    {
        if (cur_reloc->VirtualAddress >= img_size)
        {
            WARN( "invalid address %p in relocation %p\n", get_absolute_va( module, cur_reloc->VirtualAddress ), cur_reloc );
            return STATUS_ACCESS_VIOLATION;
        }
        cur_reloc = LdrProcessRelocationBlock( get_absolute_va( module, cur_reloc->VirtualAddress ),
                                        (cur_reloc->SizeOfBlock - sizeof(*cur_reloc)) / sizeof(USHORT),
                                        (USHORT *)(cur_reloc + 1), delta );

        if (!cur_reloc) return STATUS_INVALID_IMAGE_FORMAT;
    }

    /* then we allocate and copy the memory over */

    uc_mem_map(engine, vm_addr, img_size, UC_PROT_NONE);

    uc_mem_write(engine, vm_addr, module, img_size);
    
    for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        uint64_t sec_addr = (uint64_t)get_absolute_va( (HMODULE)vm_addr, sec[i].VirtualAddress );
        uint32_t sec_size = sec[i].SizeOfRawData;
        uc_mem_protect(engine, sec_addr, sec_size, nt_to_uc_prot(protect_old[i]));TRACE("test\n");
    }

    return STATUS_SUCCESS;
}

int initialize_vm(void)
{
    uc_err err = UC_ERR_OK;
    NTSTATUS stat = STATUS_SUCCESS;

    //assert (sizeof(wchar_t) == sizeof(WCHAR));

    TRACE("starting VM\n");
    if ((err = uc_open(UC_ARCH_X86, UC_MODE_64, &engine)) != UC_ERR_OK) goto done;

    /* setup shared data */
    if ((err = uc_mem_map_ptr(engine, 0xfffff78000000000, 0x10000, UC_PROT_READ | UC_PROT_WRITE, (void*)0x7ffe0000)) != UC_ERR_OK) goto done;
    /* load ntoskrnl.exe */
    HMODULE tmp_ntoskrnl_mod = LoadLibraryA("/home/derek/fake_ntoskrnl/build/ntoskrnl.exe");
    if ((stat = move_pe_into_vm(tmp_ntoskrnl_mod))) goto done;
    FreeLibrary(tmp_ntoskrnl_mod);

    /* setup CPU state */

    done:
    if (err || stat)
        ERR("winedevice VM failed to initialize, err = %d, stat = %d\n", err, stat);
    return err == UC_ERR_OK;
}

void shutdown_vm(void)
{
    if(engine)
        uc_close(engine);
}

#else
#error 2
int initialize_vm(void)
{
    return 0;
}

void shutdown_vm(void)
{
    return;
}

#endif