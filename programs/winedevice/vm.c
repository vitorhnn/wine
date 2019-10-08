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
        case PAGE_EXECUTE_READWRITE: uc_prot = UC_PROT_ALL; break;
        case PAGE_NOACCESS: uc_prot = UC_PROT_NONE; break;
        case PAGE_READONLY: uc_prot = UC_PROT_READ; break;
        case PAGE_READWRITE: uc_prot = UC_PROT_READ | UC_PROT_WRITE; break;
        case PAGE_WRITECOPY: uc_prot = UC_PROT_READ | UC_PROT_WRITE; break;
        default:
            FIXME("Unhandled NT Protection %08x\n", nt_prot);
            uc_prot = UC_PROT_NONE;
    }
    return uc_prot;
}

/* relocate it to vm memory */
NTSTATUS vm_load_pe(const char *name, HMODULE *mod_out)
{
    NTSTATUS status;
    IO_STATUS_BLOCK iosb;

    ANSI_STRING ansi;
    UNICODE_STRING nt_name;
    nt_name.Buffer = NULL;
    RtlInitString( &ansi, name );
    if ((status = wine_unix_to_nt_file_name( &ansi, &nt_name ))) return status;

    OBJECT_ATTRIBUTES attr;
    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
    attr.ObjectName = &nt_name;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    HANDLE pe_file;
    status = NtOpenFile(&pe_file, GENERIC_READ | SYNCHRONIZE, &attr, &iosb, 0, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (status) return status;

    HANDLE pe_mapping;
    LARGE_INTEGER size = {.QuadPart = 0};
    status = NtCreateSection( &pe_mapping, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY |
                            SECTION_MAP_READ | SECTION_MAP_EXECUTE,
                            NULL, &size, PAGE_EXECUTE_READ, SEC_IMAGE, pe_file );
    if (status) return status;

    HMODULE module = NULL;
    SIZE_T view_len = 0;
    status = NtMapViewOfSection(pe_mapping, GetCurrentProcess(), &module, 0, 0, NULL, &view_len, 0, 0, PAGE_EXECUTE_READ);
    if (status == STATUS_IMAGE_NOT_AT_BASE) status = STATUS_SUCCESS;
    if (status) return status;

    uint64_t vm_addr = system_space_start + (uint64_t)module; /* this might not be able to handle high addresses */
    *mod_out = (void*)vm_addr;
    IMAGE_NT_HEADERS *nt = RtlImageNtHeader(module);
    uint32_t img_size = nt->OptionalHeader.SizeOfImage;
    char *base = (char*)nt->OptionalHeader.ImageBase;
    const IMAGE_DATA_DIRECTORY *relocs = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    ULONG protect_old[96];

    TRACE("loading module at %016lx\n", vm_addr);
    uc_mem_map_ptr(engine, vm_addr, img_size, UC_PROT_READ | UC_PROT_WRITE, module);

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

    for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        uint64_t sec_addr = (uint64_t)get_absolute_va( (HMODULE)vm_addr, sec[i].VirtualAddress );
        uint32_t sec_size = sec[i].SizeOfRawData;
        uc_mem_protect(engine, sec_addr, sec_size, nt_to_uc_prot(protect_old[i]));
    }

    return STATUS_SUCCESS;
}

void *vm_get_proc_addr(HMODULE mod, const char *name)
{
    
}

void interupt_handler(uc_engine *engine, uint32_t intno, void *user_data)
{
    TRACE("test %u\n", intno);
}

void trace_handler(uc_engine *engine, uint64_t address, uint32_t size, void *user_data)
{
    TRACE("%016lx (%u):", address, size);
    for (unsigned int i; i < size; i++)
    {
        unsigned char byte;
        uc_mem_read(engine, address + i, &byte, 1);
        TRACE(" %02X", byte);
    }
    TRACE("\n");
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
    HMODULE virt_ntoskrnl_mod;
    if ((stat = vm_load_pe("/home/derek/wine-master-dir/wine64-build/dlls/virt-ntoskrnl.exe/ntoskrnl.exe", &virt_ntoskrnl_mod))) goto done;

    /* setup CPU state */
    uc_hook hook;
    if ((err = uc_hook_add(engine, &hook, UC_HOOK_INTR, interupt_handler, NULL, 0, UINT64_MAX)) != UC_ERR_OK) goto done;
    if ((err = uc_hook_add(engine, &hook, UC_HOOK_CODE, trace_handler, NULL, 0, UINT64_MAX)) != UC_ERR_OK) goto done;

    if ((err = uc_mem_map(engine, system_space_start + 0x100000, 0x10000, UC_PROT_READ | UC_PROT_WRITE))) goto done;
    uint64_t stackbase = system_space_start + 0x100000 + 0x9000;
    uc_reg_write(engine, UC_X86_REG_RSP, &stackbase);
    uc_reg_write(engine, UC_X86_REG_RBP, &stackbase);

    uint64_t entry_point = vm_get_proc_addr(virt_ntoskrnl_mod, "__wine_ntoskrnl_entry");
    TRACE("calling ntoskrnl entry %016lx\n", entry_point);
    if ((err = uc_emu_start(engine, entry_point, 0, 0, 0)) != UC_ERR_OK) goto done;

    done:
    if (err || stat)
        ERR("winedevice VM failed to initialize, err = %d, stat = %x\n", err, stat);
    return err == UC_ERR_OK;
}

void shutdown_vm(void)
{
    if(engine)
        uc_close(engine);
}

#else
int initialize_vm(void)
{
    return 0;
}

void shutdown_vm(void)
{
    return;
}

#endif