/*
 * NT basis DLL
 *
 * This file contains the Nt* API functions of NTDLL.DLL.
 * In the original ntdll.dll they all seem to just call int 0x2e (down to the NTOSKRNL)
 *
 * Copyright 1996-1998 Marcus Meissner
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

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "wine/debug.h"
#include "windef.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/library.h"
#include "wine/server.h"
#include "wine/unicode.h"

#ifdef HAVE_MACH_MACH_H
#include <mach/mach.h>
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(ntdll);

static ULONG execute_flags = MEM_EXECUTE_OPTION_DISABLE;

/*
 *	Process object
 */

/******************************************************************************
 *  NtTerminateProcess			[NTDLL.@]
 *
 *  Native applications must kill themselves when done
 */
NTSTATUS WINAPI NtTerminateProcess( HANDLE handle, LONG exit_code )
{
    NTSTATUS ret;
    BOOL self;
    SERVER_START_REQ( terminate_process )
    {
        req->handle    = wine_server_obj_handle( handle );
        req->exit_code = exit_code;
        ret = wine_server_call( req );
        self = !ret && reply->self;
    }
    SERVER_END_REQ;
    if (self && handle) _exit( exit_code );
    return ret;
}

/******************************************************************************
 *  RtlGetCurrentPeb  [NTDLL.@]
 *
 */
PEB * WINAPI RtlGetCurrentPeb(void)
{
    return NtCurrentTeb()->Peb;
}

/***********************************************************************
 *           __wine_make_process_system   (NTDLL.@)
 *
 * Mark the current process as a system process.
 * Returns the event that is signaled when all non-system processes have exited.
 */
HANDLE CDECL __wine_make_process_system(void)
{
    HANDLE ret = 0;
    SERVER_START_REQ( make_process_system )
    {
        if (!wine_server_call( req )) ret = wine_server_ptr_handle( reply->event );
    }
    SERVER_END_REQ;
    return ret;
}

static UINT process_error_mode;

#define UNIMPLEMENTED_INFO_CLASS(c) \
    case c: \
        FIXME("(process=%p) Unimplemented information class: " #c "\n", ProcessHandle); \
        ret = STATUS_INVALID_INFO_CLASS; \
        break

ULONG_PTR get_system_affinity_mask(void)
{
    ULONG num_cpus = NtCurrentTeb()->Peb->NumberOfProcessors;
    if (num_cpus >= sizeof(ULONG_PTR) * 8) return ~(ULONG_PTR)0;
    return ((ULONG_PTR)1 << num_cpus) - 1;
}

#if defined(HAVE_MACH_MACH_H)

static void fill_VM_COUNTERS(VM_COUNTERS* pvmi)
{
#if defined(MACH_TASK_BASIC_INFO)
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) == KERN_SUCCESS)
    {
        pvmi->VirtualSize = info.resident_size + info.virtual_size;
        pvmi->PagefileUsage = info.virtual_size;
        pvmi->WorkingSetSize = info.resident_size;
        pvmi->PeakWorkingSetSize = info.resident_size_max;
    }
#endif
}

#elif defined(linux)

static void fill_VM_COUNTERS(VM_COUNTERS* pvmi)
{
    FILE *f;
    char line[256];
    unsigned long value;

    f = fopen("/proc/self/status", "r");
    if (!f) return;

    while (fgets(line, sizeof(line), f))
    {
        if (sscanf(line, "VmPeak: %lu", &value))
            pvmi->PeakVirtualSize = (ULONG64)value * 1024;
        else if (sscanf(line, "VmSize: %lu", &value))
            pvmi->VirtualSize = (ULONG64)value * 1024;
        else if (sscanf(line, "VmHWM: %lu", &value))
            pvmi->PeakWorkingSetSize = (ULONG64)value * 1024;
        else if (sscanf(line, "VmRSS: %lu", &value))
            pvmi->WorkingSetSize = (ULONG64)value * 1024;
        else if (sscanf(line, "RssAnon: %lu", &value))
            pvmi->PagefileUsage += (ULONG64)value * 1024;
        else if (sscanf(line, "VmSwap: %lu", &value))
            pvmi->PagefileUsage += (ULONG64)value * 1024;
    }
    pvmi->PeakPagefileUsage = pvmi->PagefileUsage;

    fclose(f);
}

#else

static void fill_VM_COUNTERS(VM_COUNTERS* pvmi)
{
    /* FIXME : real data */
}

#endif

/******************************************************************************
*  NtQueryInformationProcess		[NTDLL.@]
*  ZwQueryInformationProcess		[NTDLL.@]
*
*/
NTSTATUS WINAPI NtQueryInformationProcess(
	IN HANDLE ProcessHandle,
	IN PROCESSINFOCLASS ProcessInformationClass,
	OUT PVOID ProcessInformation,
	IN ULONG ProcessInformationLength,
	OUT PULONG ReturnLength)
{
    NTSTATUS ret = STATUS_SUCCESS;
    ULONG len = 0;

    TRACE("(%p,0x%08x,%p,0x%08x,%p)\n",
          ProcessHandle,ProcessInformationClass,
          ProcessInformation,ProcessInformationLength,
          ReturnLength);

    switch (ProcessInformationClass) 
    {
    UNIMPLEMENTED_INFO_CLASS(ProcessQuotaLimits);
    UNIMPLEMENTED_INFO_CLASS(ProcessBasePriority);
    UNIMPLEMENTED_INFO_CLASS(ProcessRaisePriority);
    UNIMPLEMENTED_INFO_CLASS(ProcessExceptionPort);
    UNIMPLEMENTED_INFO_CLASS(ProcessAccessToken);
    UNIMPLEMENTED_INFO_CLASS(ProcessLdtInformation);
    UNIMPLEMENTED_INFO_CLASS(ProcessLdtSize);
    UNIMPLEMENTED_INFO_CLASS(ProcessIoPortHandlers);
    UNIMPLEMENTED_INFO_CLASS(ProcessPooledUsageAndLimits);
    UNIMPLEMENTED_INFO_CLASS(ProcessWorkingSetWatch);
    UNIMPLEMENTED_INFO_CLASS(ProcessUserModeIOPL);
    UNIMPLEMENTED_INFO_CLASS(ProcessEnableAlignmentFaultFixup);
    UNIMPLEMENTED_INFO_CLASS(ProcessWx86Information);
    UNIMPLEMENTED_INFO_CLASS(ProcessPriorityBoost);
    UNIMPLEMENTED_INFO_CLASS(ProcessDeviceMap);
    UNIMPLEMENTED_INFO_CLASS(ProcessSessionInformation);
    UNIMPLEMENTED_INFO_CLASS(ProcessForegroundInformation);
    UNIMPLEMENTED_INFO_CLASS(ProcessLUIDDeviceMapsEnabled);
    UNIMPLEMENTED_INFO_CLASS(ProcessBreakOnTermination);
    UNIMPLEMENTED_INFO_CLASS(ProcessHandleTracing);

    case ProcessBasicInformation:
        {
            PROCESS_BASIC_INFORMATION pbi;
            const ULONG_PTR affinity_mask = get_system_affinity_mask();

            if (ProcessInformationLength >= sizeof(PROCESS_BASIC_INFORMATION))
            {
                if (!ProcessInformation)
                    ret = STATUS_ACCESS_VIOLATION;
                else if (!ProcessHandle)
                    ret = STATUS_INVALID_HANDLE;
                else
                {
                    SERVER_START_REQ(get_process_info)
                    {
                        req->handle = wine_server_obj_handle( ProcessHandle );
                        if ((ret = wine_server_call( req )) == STATUS_SUCCESS)
                        {
                            pbi.ExitStatus = reply->exit_code;
                            pbi.PebBaseAddress = wine_server_get_ptr( reply->peb );
                            pbi.AffinityMask = reply->affinity & affinity_mask;
                            pbi.BasePriority = reply->priority;
                            pbi.UniqueProcessId = reply->pid;
                            pbi.InheritedFromUniqueProcessId = reply->ppid;
                        }
                    }
                    SERVER_END_REQ;

                    memcpy(ProcessInformation, &pbi, sizeof(PROCESS_BASIC_INFORMATION));

                    len = sizeof(PROCESS_BASIC_INFORMATION);
                }

                if (ProcessInformationLength > sizeof(PROCESS_BASIC_INFORMATION))
                    ret = STATUS_INFO_LENGTH_MISMATCH;
            }
            else
            {
                len = sizeof(PROCESS_BASIC_INFORMATION);
                ret = STATUS_INFO_LENGTH_MISMATCH;
            }
        }
        break;
    case ProcessIoCounters:
        {
            IO_COUNTERS pii;

            if (ProcessInformationLength >= sizeof(IO_COUNTERS))
            {
                if (!ProcessInformation)
                    ret = STATUS_ACCESS_VIOLATION;
                else if (!ProcessHandle)
                    ret = STATUS_INVALID_HANDLE;
                else
                {
                    /* FIXME : real data */
                    memset(&pii, 0 , sizeof(IO_COUNTERS));

                    memcpy(ProcessInformation, &pii, sizeof(IO_COUNTERS));

                    len = sizeof(IO_COUNTERS);
                }

                if (ProcessInformationLength > sizeof(IO_COUNTERS))
                    ret = STATUS_INFO_LENGTH_MISMATCH;
            }
            else
            {
                len = sizeof(IO_COUNTERS);
                ret = STATUS_INFO_LENGTH_MISMATCH;
            }
        }
        break;
    case ProcessVmCounters:
        {
            VM_COUNTERS pvmi;

            /* older Windows versions don't have the PrivatePageCount field */
            if (ProcessInformationLength >= FIELD_OFFSET(VM_COUNTERS,PrivatePageCount))
            {
                if (!ProcessInformation)
                    ret = STATUS_ACCESS_VIOLATION;
                else
                {
                    memset(&pvmi, 0 , sizeof(VM_COUNTERS));
                    if (ProcessHandle == GetCurrentProcess())
                        fill_VM_COUNTERS(&pvmi);
                    else
                    {
                        SERVER_START_REQ(get_process_vm_counters)
                        {
                            req->handle = wine_server_obj_handle( ProcessHandle );
                            if (!(ret = wine_server_call( req )))
                            {
                                pvmi.PeakVirtualSize = reply->peak_virtual_size;
                                pvmi.VirtualSize = reply->virtual_size;
                                pvmi.PeakWorkingSetSize = reply->peak_working_set_size;
                                pvmi.WorkingSetSize = reply->working_set_size;
                                pvmi.PagefileUsage = reply->pagefile_usage;
                                pvmi.PeakPagefileUsage = reply->peak_pagefile_usage;
                            }
                        }
                        SERVER_END_REQ;
                        if (ret) break;
                    }

                    len = ProcessInformationLength;
                    if (len != FIELD_OFFSET(VM_COUNTERS,PrivatePageCount)) len = sizeof(VM_COUNTERS);

                    memcpy(ProcessInformation, &pvmi, min(ProcessInformationLength,sizeof(VM_COUNTERS)));
                }

                if (ProcessInformationLength != FIELD_OFFSET(VM_COUNTERS,PrivatePageCount) &&
                    ProcessInformationLength != sizeof(VM_COUNTERS))
                    ret = STATUS_INFO_LENGTH_MISMATCH;
            }
            else
            {
                len = sizeof(pvmi);
                ret = STATUS_INFO_LENGTH_MISMATCH;
            }
        }
        break;
    case ProcessTimes:
        {
            KERNEL_USER_TIMES pti;

            if (ProcessInformationLength >= sizeof(KERNEL_USER_TIMES))
            {
                if (!ProcessInformation)
                    ret = STATUS_ACCESS_VIOLATION;
                else if (!ProcessHandle)
                    ret = STATUS_INVALID_HANDLE;
                else
                {
                    /* FIXME : User- and KernelTime have to be implemented */
                    memset(&pti, 0, sizeof(KERNEL_USER_TIMES));

                    SERVER_START_REQ(get_process_info)
                    {
                      req->handle = wine_server_obj_handle( ProcessHandle );
                      if ((ret = wine_server_call( req )) == STATUS_SUCCESS)
                      {
                          pti.CreateTime.QuadPart = reply->start_time;
                          pti.ExitTime.QuadPart = reply->end_time;
                      }
                    }
                    SERVER_END_REQ;

                    memcpy(ProcessInformation, &pti, sizeof(KERNEL_USER_TIMES));
                    len = sizeof(KERNEL_USER_TIMES);
                }

                if (ProcessInformationLength > sizeof(KERNEL_USER_TIMES))
                    ret = STATUS_INFO_LENGTH_MISMATCH;
            }
            else
            {
                len = sizeof(KERNEL_USER_TIMES);
                ret = STATUS_INFO_LENGTH_MISMATCH;
            }
        }
        break;
    case ProcessDebugPort:
        len = sizeof(DWORD_PTR);
        if (ProcessInformationLength == len)
        {
            if (!ProcessInformation)
                ret = STATUS_ACCESS_VIOLATION;
            else if (!ProcessHandle)
                ret = STATUS_INVALID_HANDLE;
            else
            {
                SERVER_START_REQ(get_process_info)
                {
                    req->handle = wine_server_obj_handle( ProcessHandle );
                    if ((ret = wine_server_call( req )) == STATUS_SUCCESS)
                    {
                        *(DWORD_PTR *)ProcessInformation = reply->debugger_present ? ~(DWORD_PTR)0 : 0;
                    }
                }
                SERVER_END_REQ;
            }
        }
        else
            ret = STATUS_INFO_LENGTH_MISMATCH;
        break;
    case ProcessDebugFlags:
        len = sizeof(DWORD);
        if (ProcessInformationLength == len)
        {
            if (!ProcessInformation)
                ret = STATUS_ACCESS_VIOLATION;
            else if (!ProcessHandle)
                ret = STATUS_INVALID_HANDLE;
            else
            {
                SERVER_START_REQ(get_process_info)
                {
                    req->handle = wine_server_obj_handle( ProcessHandle );
                    if ((ret = wine_server_call( req )) == STATUS_SUCCESS)
                    {
                        *(DWORD *)ProcessInformation = reply->debug_children;
                    }
                }
                SERVER_END_REQ;
            }
        }
        else
            ret = STATUS_INFO_LENGTH_MISMATCH;
        break;
    case ProcessDefaultHardErrorMode:
        len = sizeof(process_error_mode);
        if (ProcessInformationLength == len)
            memcpy(ProcessInformation, &process_error_mode, len);
        else
            ret = STATUS_INFO_LENGTH_MISMATCH;
        break;
    case ProcessDebugObjectHandle:
        /* "These are not the debuggers you are looking for." *
         * set it to 0 aka "no debugger" to satisfy copy protections */
        len = sizeof(HANDLE);
        if (ProcessInformationLength == len)
        {
            if (!ProcessInformation)
                ret = STATUS_ACCESS_VIOLATION;
            else if (!ProcessHandle)
                ret = STATUS_INVALID_HANDLE;
            else
            {
                memset(ProcessInformation, 0, ProcessInformationLength);
                ret = STATUS_PORT_NOT_SET;
            }
        }
        else
            ret = STATUS_INFO_LENGTH_MISMATCH;
        break;
    case ProcessHandleCount:
        if (ProcessInformationLength >= 4)
        {
            if (!ProcessInformation)
                ret = STATUS_ACCESS_VIOLATION;
            else if (!ProcessHandle)
                ret = STATUS_INVALID_HANDLE;
            else
            {
                memset(ProcessInformation, 0, 4);
                len = 4;
            }

            if (ProcessInformationLength > 4)
                ret = STATUS_INFO_LENGTH_MISMATCH;
        }
        else
        {
            len = 4;
            ret = STATUS_INFO_LENGTH_MISMATCH;
        }
        break;

    case ProcessAffinityMask:
        len = sizeof(ULONG_PTR);
        if (ProcessInformationLength == len)
        {
            const ULONG_PTR system_mask = get_system_affinity_mask();

            SERVER_START_REQ(get_process_info)
            {
                req->handle = wine_server_obj_handle( ProcessHandle );
                if (!(ret = wine_server_call( req )))
                    *(ULONG_PTR *)ProcessInformation = reply->affinity & system_mask;
            }
            SERVER_END_REQ;
        }
        else ret = STATUS_INFO_LENGTH_MISMATCH;
        break;

    case ProcessWow64Information:
        len = sizeof(ULONG_PTR);
        if (ProcessInformationLength != len) ret = STATUS_INFO_LENGTH_MISMATCH;
        else if (!ProcessInformation) ret = STATUS_ACCESS_VIOLATION;
        else if(!ProcessHandle) ret = STATUS_INVALID_HANDLE;
        else
        {
            ULONG_PTR val = 0;

            if (ProcessHandle == GetCurrentProcess()) val = is_wow64;
            else if (server_cpus & ((1 << CPU_x86_64) | (1 << CPU_ARM64)))
            {
                SERVER_START_REQ( get_process_info )
                {
                    req->handle = wine_server_obj_handle( ProcessHandle );
                    if (!(ret = wine_server_call( req )))
                        val = (reply->cpu != CPU_x86_64 && reply->cpu != CPU_ARM64);
                }
                SERVER_END_REQ;
            }
            *(ULONG_PTR *)ProcessInformation = val;
        }
        break;
    case ProcessImageFileName:
        /* FIXME: Should return a device path */
    case ProcessImageFileNameWin32:
        SERVER_START_REQ(get_dll_info)
        {
            UNICODE_STRING *image_file_name_str = ProcessInformation;

            req->handle = wine_server_obj_handle( ProcessHandle );
            req->base_address = 0; /* main module */
            wine_server_set_reply( req, image_file_name_str ? image_file_name_str + 1 : NULL,
                                   ProcessInformationLength > sizeof(UNICODE_STRING) ? ProcessInformationLength - sizeof(UNICODE_STRING) : 0 );
            ret = wine_server_call( req );
            if (ret == STATUS_BUFFER_TOO_SMALL) ret = STATUS_INFO_LENGTH_MISMATCH;

            len = sizeof(UNICODE_STRING) + reply->filename_len;
            if (ret == STATUS_SUCCESS)
            {
                image_file_name_str->MaximumLength = image_file_name_str->Length = reply->filename_len;
                image_file_name_str->Buffer = (PWSTR)(image_file_name_str + 1);
            }
        }
        SERVER_END_REQ;
        break;
    case ProcessExecuteFlags:
        len = sizeof(ULONG);
        if (ProcessInformationLength == len)
            *(ULONG *)ProcessInformation = execute_flags;
        else
            ret = STATUS_INFO_LENGTH_MISMATCH;
        break;
    case ProcessPriorityClass:
        len = sizeof(PROCESS_PRIORITY_CLASS);
        if (ProcessInformationLength == len)
        {
            if (!ProcessInformation)
                ret = STATUS_ACCESS_VIOLATION;
            else if (!ProcessHandle)
                ret = STATUS_INVALID_HANDLE;
            else
            {
                PROCESS_PRIORITY_CLASS *priority = ProcessInformation;

                SERVER_START_REQ(get_process_info)
                {
                    req->handle = wine_server_obj_handle( ProcessHandle );
                    if ((ret = wine_server_call( req )) == STATUS_SUCCESS)
                    {
                        priority->PriorityClass = reply->priority;
                        /* FIXME: Not yet supported by the wineserver */
                        priority->Foreground = FALSE;
                    }
                }
                SERVER_END_REQ;
            }
        }
        else
            ret = STATUS_INFO_LENGTH_MISMATCH;
        break;
    default:
        FIXME("(%p,info_class=%d,%p,0x%08x,%p) Unknown information class\n",
              ProcessHandle,ProcessInformationClass,
              ProcessInformation,ProcessInformationLength,
              ReturnLength);
        ret = STATUS_INVALID_INFO_CLASS;
        break;
    }

    if (ReturnLength) *ReturnLength = len;
    
    return ret;
}

/******************************************************************************
 * NtSetInformationProcess [NTDLL.@]
 * ZwSetInformationProcess [NTDLL.@]
 */
NTSTATUS WINAPI NtSetInformationProcess(
	IN HANDLE ProcessHandle,
	IN PROCESSINFOCLASS ProcessInformationClass,
	IN PVOID ProcessInformation,
	IN ULONG ProcessInformationLength)
{
    NTSTATUS ret = STATUS_SUCCESS;

    switch (ProcessInformationClass)
    {
    case ProcessDefaultHardErrorMode:
        if (ProcessInformationLength != sizeof(UINT)) return STATUS_INVALID_PARAMETER;
        process_error_mode = *(UINT *)ProcessInformation;
        break;
    case ProcessAffinityMask:
    {
        const ULONG_PTR system_mask = get_system_affinity_mask();

        if (ProcessInformationLength != sizeof(DWORD_PTR)) return STATUS_INVALID_PARAMETER;
        if (*(PDWORD_PTR)ProcessInformation & ~system_mask)
            return STATUS_INVALID_PARAMETER;
        if (!*(PDWORD_PTR)ProcessInformation)
            return STATUS_INVALID_PARAMETER;
        SERVER_START_REQ( set_process_info )
        {
            req->handle   = wine_server_obj_handle( ProcessHandle );
            req->affinity = *(PDWORD_PTR)ProcessInformation;
            req->mask     = SET_PROCESS_INFO_AFFINITY;
            ret = wine_server_call( req );
        }
        SERVER_END_REQ;
        break;
    }
    case ProcessPriorityClass:
        if (ProcessInformationLength != sizeof(PROCESS_PRIORITY_CLASS))
            return STATUS_INVALID_PARAMETER;
        else
        {
            PROCESS_PRIORITY_CLASS* ppc = ProcessInformation;

            SERVER_START_REQ( set_process_info )
            {
                req->handle   = wine_server_obj_handle( ProcessHandle );
                /* FIXME Foreground isn't used */
                req->priority = ppc->PriorityClass;
                req->mask     = SET_PROCESS_INFO_PRIORITY;
                ret = wine_server_call( req );
            }
            SERVER_END_REQ;
        }
        break;

    case ProcessExecuteFlags:
        if (ProcessInformationLength != sizeof(ULONG))
            return STATUS_INVALID_PARAMETER;
        else if (execute_flags & MEM_EXECUTE_OPTION_PERMANENT)
            return STATUS_ACCESS_DENIED;
        else
        {
            BOOL enable;
            switch (*(ULONG *)ProcessInformation & (MEM_EXECUTE_OPTION_ENABLE|MEM_EXECUTE_OPTION_DISABLE))
            {
            case MEM_EXECUTE_OPTION_ENABLE:
                enable = TRUE;
                break;
            case MEM_EXECUTE_OPTION_DISABLE:
                enable = FALSE;
                break;
            default:
                return STATUS_INVALID_PARAMETER;
            }
            execute_flags = *(ULONG *)ProcessInformation;
            VIRTUAL_SetForceExec( enable );
        }
        break;

    default:
        FIXME("(%p,0x%08x,%p,0x%08x) stub\n",
              ProcessHandle,ProcessInformationClass,ProcessInformation,
              ProcessInformationLength);
        ret = STATUS_NOT_IMPLEMENTED;
        break;
    }
    return ret;
}

/******************************************************************************
 * NtFlushInstructionCache [NTDLL.@]
 * ZwFlushInstructionCache [NTDLL.@]
 */
NTSTATUS WINAPI NtFlushInstructionCache( HANDLE handle, const void *addr, SIZE_T size )
{
#if defined(__x86_64__) || defined(__i386__)
    /* no-op */
#elif defined(HAVE___CLEAR_CACHE)
    if (handle == GetCurrentProcess())
    {
        __clear_cache( (char *)addr, (char *)addr + size );
    }
    else
    {
        static int once;
        if (!once++) FIXME( "%p %p %ld other process not supported\n", handle, addr, size );
    }
#else
    static int once;
    if (!once++) FIXME( "%p %p %ld\n", handle, addr, size );
#endif
    return STATUS_SUCCESS;
}

/******************************************************************
 *		NtOpenProcess [NTDLL.@]
 *		ZwOpenProcess [NTDLL.@]
 */
NTSTATUS  WINAPI NtOpenProcess(PHANDLE handle, ACCESS_MASK access,
                               const OBJECT_ATTRIBUTES* attr, const CLIENT_ID* cid)
{
    NTSTATUS    status;

    SERVER_START_REQ( open_process )
    {
        req->pid        = HandleToULong(cid->UniqueProcess);
        req->access     = access;
        req->attributes = attr ? attr->Attributes : 0;
        status = wine_server_call( req );
        if (!status) *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;
    return status;
}

/******************************************************************************
 * NtResumeProcess
 * ZwResumeProcess
 */
NTSTATUS WINAPI NtResumeProcess( HANDLE handle )
{
    FIXME("stub: %p\n", handle);
    return STATUS_NOT_IMPLEMENTED;
}

/******************************************************************************
 * NtSuspendProcess
 * ZwSuspendProcess
 */
NTSTATUS WINAPI NtSuspendProcess( HANDLE handle )
{
    FIXME("stub: %p\n", handle);
    return STATUS_NOT_IMPLEMENTED;
}

#define NE_FFLAGS_LIBMODULE  0x8000

enum binary_type
{
    BINARY_UNKNOWN = 0,
    BINARY_PE,
    BINARY_WIN16,
    BINARY_OS216,
    BINARY_DOS,
    BINARY_UNIX_EXE,
    BINARY_UNIX_LIB
};

#define BINARY_FLAG_DLL     0x01
#define BINARY_FLAG_64BIT   0x02
#define BINARY_FLAG_FAKEDLL 0x04

struct binary_info
{
    enum binary_type type;
    DWORD            arch;
    DWORD            flags;
    ULONGLONG        res_start;
    ULONGLONG        res_end;
};

static DWORD MODULE_Decide_OS2_OldWin(HANDLE hfile, const IMAGE_DOS_HEADER *mz, const IMAGE_OS2_HEADER *ne)
{
    DWORD ret = BINARY_OS216;
    LPWORD modtab = NULL;
    LPSTR nametab = NULL;
    int i;
    LARGE_INTEGER li;
    IO_STATUS_BLOCK io;

    /* read modref table */
    li.QuadPart = (mz->e_lfanew + ne->ne_modtab);
    if ( (!(modtab = RtlAllocateHeap( GetProcessHeap(), 0, ne->ne_cmod*sizeof(WORD))))
      || (NtReadFile(hfile, NULL, NULL, NULL, &io, modtab, ne->ne_cmod*sizeof(WORD), &li, NULL))
      || (io.Information != ne->ne_cmod*sizeof(WORD)) )
	goto done;

    /* read imported names table */
    li.QuadPart = (mz->e_lfanew + ne->ne_imptab);
    if ( (!(nametab = RtlAllocateHeap( GetProcessHeap(), 0, ne->ne_enttab - ne->ne_imptab)))
      || (NtReadFile(hfile, NULL, NULL, NULL, &io, nametab, ne->ne_enttab - ne->ne_imptab, &li, NULL))
      || (io.Information != ne->ne_enttab - ne->ne_imptab) )
	goto done;

    for (i=0; i < ne->ne_cmod; i++)
    {
        LPSTR module = &nametab[modtab[i]];
        TRACE("modref: %.*s\n", module[0], &module[1]);
        if (!(strncmp(&module[1], "KERNEL", module[0])))
        { /* very old Windows file */
            MESSAGE("This seems to be a very old (pre-3.0) Windows executable. Expect crashes, especially if this is a real-mode binary !\n");
            ret = BINARY_WIN16;
            break;
        }
    }

done:
    RtlFreeHeap( GetProcessHeap(), 0, modtab);
    RtlFreeHeap( GetProcessHeap(), 0, nametab);
    return ret;
}

/***********************************************************************
 *           get_binary_info
 */
void get_binary_info( HANDLE hfile, struct binary_info *info )
{
    union
    {
        struct
        {
            unsigned char magic[4];
            unsigned char class;
            unsigned char data;
            unsigned char ignored1[10];
            unsigned short type;
            unsigned short machine;
            unsigned char ignored2[8];
            unsigned int phoff;
            unsigned char ignored3[12];
            unsigned short phnum;
        } elf;
        struct
        {
            unsigned char magic[4];
            unsigned char class;
            unsigned char data;
            unsigned char ignored1[10];
            unsigned short type;
            unsigned short machine;
            unsigned char ignored2[12];
            unsigned __int64 phoff;
            unsigned char ignored3[16];
            unsigned short phnum;
        } elf64;
        struct
        {
            unsigned int magic;
            unsigned int cputype;
            unsigned int cpusubtype;
            unsigned int filetype;
        } macho;
        IMAGE_DOS_HEADER mz;
    } header;

    IO_STATUS_BLOCK io;
    LARGE_INTEGER li;
    NTSTATUS status;

    memset( info, 0, sizeof(*info) );

    /* Seek to the start of the file and read the header information. */
    li.QuadPart = 0;
    if( (status = NtReadFile(hfile, NULL, NULL, NULL, &io, &header, sizeof(header), &li, NULL)) ) return;

    if (!memcmp( header.elf.magic, "\177ELF", 4 ))
    {
#ifdef WORDS_BIGENDIAN
        BOOL byteswap = (header.elf.data == 1);
#else
        BOOL byteswap = (header.elf.data == 2);
#endif
        if (header.elf.class == 2) info->flags |= BINARY_FLAG_64BIT;
        if (byteswap)
        {
            header.elf.type = RtlUshortByteSwap( header.elf.type );
            header.elf.machine = RtlUshortByteSwap( header.elf.machine );
        }
        switch(header.elf.type)
        {
        case 2:
            info->type = BINARY_UNIX_EXE;
            break;
        case 3:
        {
            LARGE_INTEGER phoff;
            unsigned short phnum;
            unsigned int type;
            if (header.elf.class == 2)
            {
                phoff.QuadPart = byteswap ? RtlUlonglongByteSwap( header.elf64.phoff ) : header.elf64.phoff;
                phnum = byteswap ? RtlUshortByteSwap( header.elf64.phnum ) : header.elf64.phnum;
            }
            else
            {
                phoff.QuadPart = byteswap ? RtlUlongByteSwap( header.elf.phoff ) : header.elf.phoff;
                phnum = byteswap ? RtlUshortByteSwap( header.elf.phnum ) : header.elf.phnum;
            }
            while (phnum--)
            {
                if ( NtReadFile( hfile, NULL, NULL, NULL, &io, &type, sizeof(type), &phoff, NULL ) ) return;
                if (byteswap) type = RtlUlongByteSwap( type );
                if (type == 3)
                {
                    info->type = BINARY_UNIX_EXE;
                    break;
                }
                phoff.QuadPart += (header.elf.class == 2) ? 56 : 32;
            }
            if (!info->type) info->type = BINARY_UNIX_LIB;
            break;
        }
        default:
            return;
        }
        switch(header.elf.machine)
        {
        case 3:   info->arch = IMAGE_FILE_MACHINE_I386; break;
        case 20:  info->arch = IMAGE_FILE_MACHINE_POWERPC; break;
        case 40:  info->arch = IMAGE_FILE_MACHINE_ARMNT; break;
        case 50:  info->arch = IMAGE_FILE_MACHINE_IA64; break;
        case 62:  info->arch = IMAGE_FILE_MACHINE_AMD64; break;
        case 183: info->arch = IMAGE_FILE_MACHINE_ARM64; break;
        }
    }
    /* Mach-o File with Endian set to Big Endian or Little Endian */
    else if (header.macho.magic == 0xfeedface || header.macho.magic == 0xcefaedfe ||
             header.macho.magic == 0xfeedfacf || header.macho.magic == 0xcffaedfe)
    {
        if ((header.macho.cputype >> 24) == 1) info->flags |= BINARY_FLAG_64BIT;
        if (header.macho.magic == 0xcefaedfe || header.macho.magic == 0xcffaedfe)
        {
            header.macho.filetype = RtlUlongByteSwap( header.macho.filetype );
            header.macho.cputype = RtlUlongByteSwap( header.macho.cputype );
        }
        switch(header.macho.filetype)
        {
        case 2: info->type = BINARY_UNIX_EXE; break;
        case 8: info->type = BINARY_UNIX_LIB; break;
        }
        switch(header.macho.cputype)
        {
        case 0x00000007: info->arch = IMAGE_FILE_MACHINE_I386; break;
        case 0x01000007: info->arch = IMAGE_FILE_MACHINE_AMD64; break;
        case 0x0000000c: info->arch = IMAGE_FILE_MACHINE_ARMNT; break;
        case 0x0100000c: info->arch = IMAGE_FILE_MACHINE_ARM64; break;
        case 0x00000012: info->arch = IMAGE_FILE_MACHINE_POWERPC; break;
        }
    }
    /* Not ELF, try DOS */
    else if (header.mz.e_magic == IMAGE_DOS_SIGNATURE)
    {
        union
        {
            IMAGE_OS2_HEADER os2;
            IMAGE_NT_HEADERS32 nt;
            IMAGE_NT_HEADERS64 nt64;
        } ext_header;

        /* We do have a DOS image so we will now try to seek into
         * the file by the amount indicated by the field
         * "Offset to extended header" and read in the
         * "magic" field information at that location.
         * This will tell us if there is more header information
         * to read or not.
         */
        info->type = BINARY_DOS;
        info->arch = IMAGE_FILE_MACHINE_I386;
        li.QuadPart = header.mz.e_lfanew;
        if (NtReadFile(hfile, NULL, NULL, NULL, &io, &ext_header, sizeof(ext_header), &li, NULL)) return;


        /* Reading the magic field succeeded so
         * we will try to determine what type it is.
         */
        if (!memcmp( &ext_header.nt.Signature, "PE\0\0", 4 ))
        {
            if (io.Information >= sizeof(ext_header.nt.FileHeader))
            {
                static const char fakedll_signature[] = "Wine placeholder DLL";
                char buffer[sizeof(fakedll_signature)];

                info->type = BINARY_PE;
                info->arch = ext_header.nt.FileHeader.Machine;
                if (ext_header.nt.FileHeader.Characteristics & IMAGE_FILE_DLL)
                    info->flags |= BINARY_FLAG_DLL;
                if (io.Information < sizeof(ext_header))  /* clear remaining part of header if missing */
                    memset( (char *)&ext_header + io.Information, 0, sizeof(ext_header) - io.Information );
                switch (ext_header.nt.OptionalHeader.Magic)
                {
                case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
                    info->res_start = ext_header.nt.OptionalHeader.ImageBase;
                    info->res_end = info->res_start + ext_header.nt.OptionalHeader.SizeOfImage;
                    break;
                case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
                    info->res_start = ext_header.nt64.OptionalHeader.ImageBase;
                    info->res_end = info->res_start + ext_header.nt64.OptionalHeader.SizeOfImage;
                    info->flags |= BINARY_FLAG_64BIT;
                    break;
                }

                li.QuadPart = sizeof(header.mz);
                if (header.mz.e_lfanew >= sizeof(header.mz) + sizeof(fakedll_signature) &&
                    !NtReadFile( hfile, NULL, NULL, NULL, &io, buffer, sizeof(fakedll_signature), &li, NULL ) &&
                    io.Information == sizeof(fakedll_signature) &&
                    !memcmp( buffer, fakedll_signature, sizeof(fakedll_signature) ))
                {
                    info->flags |= BINARY_FLAG_FAKEDLL;
                }
            }
        }
        else if (!memcmp( &ext_header.os2.ne_magic, "NE", 2 ))
        {
            /* This is a Windows executable (NE) header.  This can
             * mean either a 16-bit OS/2 or a 16-bit Windows or even a
             * DOS program (running under a DOS extender).  To decide
             * which, we'll have to read the NE header.
             */
            if (io.Information >= sizeof(ext_header.os2))
            {
                if (ext_header.os2.ne_flags & NE_FFLAGS_LIBMODULE) info->flags |= BINARY_FLAG_DLL;
                switch ( ext_header.os2.ne_exetyp )
                {
                case 1:  info->type = BINARY_OS216; break; /* OS/2 */
                case 2:  info->type = BINARY_WIN16; break; /* Windows */
                case 3:  info->type = BINARY_DOS; break; /* European MS-DOS 4.x */
                case 4:  info->type = BINARY_WIN16; break; /* Windows 386; FIXME: is this 32bit??? */
                case 5:  info->type = BINARY_DOS; break; /* BOSS, Borland Operating System Services */
                /* other types, e.g. 0 is: "unknown" */
                default: info->type = MODULE_Decide_OS2_OldWin(hfile, &header.mz, &ext_header.os2); break;
                }
            }
        }
    }
}

static startup_info_t *create_startup_info(PRTL_USER_PROCESS_PARAMETERS startup, DWORD *info_size)
{
    const RTL_USER_PROCESS_PARAMETERS *cur_params;
    startup_info_t *info;
    DWORD size;
    void *ptr;
    HANDLE hstdin, hstdout, hstderr;
    
    
    cur_params = NtCurrentTeb()->Peb->ProcessParameters;
    
    /* convert ImagePathName and CommandLine to DOS format*/
    if(startup->ImagePathName.Buffer[5] == ':')
    {
        DWORD len = startup->ImagePathName.Length - 4 * sizeof(WCHAR);
        memmove( startup->ImagePathName.Buffer, startup->ImagePathName.Buffer + 4, len );
        startup->ImagePathName.Buffer[len / sizeof(WCHAR)] = 0;
        startup->ImagePathName.Length = len;
    }
    
    if(startup->CommandLine.Buffer[5] == ':')
    {
        DWORD len = startup->CommandLine.Length - 4 * sizeof(WCHAR);
        memmove( startup->CommandLine.Buffer, startup->CommandLine.Buffer + 4, len );
        startup->CommandLine.Buffer[len / sizeof(WCHAR)] = 0;
        startup->CommandLine.Length = len;
    }
    
    size = sizeof(*info);
    size += startup->CurrentDirectory.DosPath.Length;
    size += startup->DllPath.Length;
    size += startup->ImagePathName.Length;
    size += startup->CommandLine.Length;
    size += startup->WindowTitle.Length;
    size += startup->Desktop.Length;
    size += startup->ShellInfo.Length;
    size += startup->RuntimeInfo.Length;
    
    *info_size = size;
    
    info = RtlAllocateHeap( GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if(!info) FIXME("broke1");
    
    info->console_flags = startup->ConsoleFlags;
    
    if(startup->dwFlags & STARTF_USESTDHANDLES)
    {
        hstdin  = startup->hStdInput;
        hstdout = startup->hStdOutput;
        hstderr = startup->hStdError;
    }else {
        hstdin  = cur_params->hStdInput;
        hstdout = cur_params->hStdOutput;
        hstderr = cur_params->hStdError;
    }
    
    info->hstdin  = wine_server_obj_handle( hstdin );
    info->hstdout = wine_server_obj_handle( hstdout );
    info->hstderr = wine_server_obj_handle( hstderr );
    
    if (is_console_handle(hstdin))  info->hstdin  = console_handle_unmap(hstdin);
    if (is_console_handle(hstdout)) info->hstdout = console_handle_unmap(hstdout);
    if (is_console_handle(hstderr)) info->hstderr = console_handle_unmap(hstderr);
    
    info->x         = startup->dwX;
    info->y         = startup->dwY;
    info->xsize     = startup->dwXSize;
    info->ysize     = startup->dwYSize;
    info->xchars    = startup->dwXCountChars;
    info->ychars    = startup->dwYCountChars;
    info->attribute = startup->dwFillAttribute;
    info->flags     = startup->dwFlags;
    info->show      = startup->wShowWindow;
    
    /* add the strings */
    ptr = info + 1;
    
    info->curdir_len = startup->CurrentDirectory.DosPath.Length;
    memcpy( ptr, startup->CurrentDirectory.DosPath.Buffer, startup->CurrentDirectory.DosPath.Length );
    ptr = (char *)ptr + startup->CurrentDirectory.DosPath.Length;
    
    info->dllpath_len = startup->DllPath.Length;
    memcpy( ptr, startup->DllPath.Buffer, startup->DllPath.Length );
    ptr = (char *)ptr + startup->DllPath.Length;
    
    info->imagepath_len = startup->ImagePathName.Length;
    memcpy( ptr, startup->ImagePathName.Buffer, startup->ImagePathName.Length );
    ptr = (char *)ptr + startup->ImagePathName.Length;
    
    info->cmdline_len = startup->CommandLine.Length;
    memcpy( ptr, startup->CommandLine.Buffer, startup->CommandLine.Length );
    ptr = (char *)ptr + startup->CommandLine.Length;
    
    
    info->title_len = startup->WindowTitle.Length;
    memcpy( ptr, startup->WindowTitle.Buffer, startup->WindowTitle.Length );
    ptr = (char *)ptr + startup->WindowTitle.Length;
    
    
    info->desktop_len = startup->Desktop.Length;
    memcpy( ptr, startup->Desktop.Buffer, startup->Desktop.Length );
    ptr = (char *)ptr + startup->Desktop.Length;
    
    info->shellinfo_len = startup->ShellInfo.Length;
    memcpy( ptr, startup->ShellInfo.Buffer, startup->ShellInfo.Length );
    ptr = (char *)ptr + startup->ShellInfo.Length;
    
    info->runtime_len = startup->RuntimeInfo.Length;
    memcpy( ptr, startup->RuntimeInfo.Buffer, startup->RuntimeInfo.Length );
    ptr = (char *)ptr + startup->RuntimeInfo.Length;
    
    return info;
}

static const BOOL is_win64 = (sizeof(void *) > sizeof(int));

/***********************************************************************
 *           get_alternate_loader
 *
 * Get the name of the alternate (32 or 64 bit) Wine loader.
 */
static const char *get_alternate_loader( char **ret_env )
{
    char *env;
    const char *loader = NULL;
    const char *loader_env = getenv( "WINELOADER" );

    *ret_env = NULL;

    if (wine_get_build_dir()) loader = is_win64 ? "loader/wine" : "server/../loader/wine64";

    if (loader_env)
    {
        int len = strlen( loader_env );
        if (!is_win64)
        {
            if (!(env = RtlAllocateHeap( GetProcessHeap(), 0, sizeof("WINELOADER=") + len + 2 ))) return NULL;
            strcpy( env, "WINELOADER=" );
            strcat( env, loader_env );
            strcat( env, "64" );
        }
        else
        {
            if (!(env = RtlAllocateHeap( GetProcessHeap(), 0, sizeof("WINELOADER=") + len ))) return NULL;
            strcpy( env, "WINELOADER=" );
            strcat( env, loader_env );
            len += sizeof("WINELOADER=") - 1;
            if (!strcmp( env + len - 2, "64" )) env[len - 2] = 0;
        }
        if (!loader)
        {
            if ((loader = strrchr( env, '/' ))) loader++;
            else loader = env;
        }
        *ret_env = env;
    }
    if (!loader) loader = is_win64 ? "wine" : "wine64";
    return loader;
}

/***********************************************************************
 *           build_argv
 *
 * Build an argv array from a command-line.
 * 'reserved' is the number of args to reserve before the first one.
 */
static char **build_argv( const WCHAR *cmdlineW, int reserved )
{
    int argc;
    char** argv;
    char *arg,*s,*d,*cmdline;
    int in_quotes,bcount,len;

    len = ntdll_wcstoumbs( 0, cmdlineW, strlenW(cmdlineW), NULL, 0, NULL, NULL );
    if (!(cmdline = RtlAllocateHeap( GetProcessHeap(), 0, len ))) return NULL;
    ntdll_wcstoumbs( 0, cmdlineW, strlenW(cmdlineW), cmdline, len, NULL, NULL );

    argc=reserved+1;
    bcount=0;
    in_quotes=0;
    s=cmdline;
    while (1) {
        if (*s=='\0' || ((*s==' ' || *s=='\t') && !in_quotes)) {
            /* space */
            argc++;
            /* skip the remaining spaces */
            while (*s==' ' || *s=='\t') {
                s++;
            }
            if (*s=='\0')
                break;
            bcount=0;
            continue;
        } else if (*s=='\\') {
            /* '\', count them */
            bcount++;
        } else if ((*s=='"') && ((bcount & 1)==0)) {
            /* unescaped '"' */
            in_quotes=!in_quotes;
            bcount=0;
        } else {
            /* a regular character */
            bcount=0;
        }
        s++;
    }
    if (!(argv = RtlAllocateHeap( GetProcessHeap(), 0, argc*sizeof(*argv) + len )))
    {
        RtlFreeHeap( GetProcessHeap(), 0, cmdline );
        return NULL;
    }

    arg = d = s = (char *)(argv + argc);
    memcpy( d, cmdline, len );
    bcount=0;
    in_quotes=0;
    argc=reserved;
    while (*s) {
        if ((*s==' ' || *s=='\t') && !in_quotes) {
            /* Close the argument and copy it */
            *d=0;
            argv[argc++]=arg;

            /* skip the remaining spaces */
            do {
                s++;
            } while (*s==' ' || *s=='\t');

            /* Start with a new argument */
            arg=d=s;
            bcount=0;
        } else if (*s=='\\') {
            /* '\\' */
            *d++=*s++;
            bcount++;
        } else if (*s=='"') {
            /* '"' */
            if ((bcount & 1)==0) {
                /* Preceded by an even number of '\', this is half that
                 * number of '\', plus a '"' which we discard.
                 */
                d-=bcount/2;
                s++;
                in_quotes=!in_quotes;
            } else {
                /* Preceded by an odd number of '\', this is half that
                 * number of '\' followed by a '"'
                 */
                d=d-bcount/2-1;
                *d++='"';
                s++;
            }
            bcount=0;
        } else {
            /* a regular character */
            *d++=*s++;
            bcount=0;
        }
    }
    if (*arg) {
        *d='\0';
        argv[argc++]=arg;
    }
    argv[argc]=NULL;

    RtlFreeHeap( GetProcessHeap(), 0, cmdline );
    return argv;
}

static pid_t exec_loader( LPCWSTR cmd_line, int socketfd,
                          int stdin_fd, int stdout_fd, const char *unixdir, char *winedebug,
                          const struct binary_info *binary_info )
{
    pid_t pid;
    char *wineloader = NULL;
    const char *loader = NULL;
    char **argv;

    argv = build_argv( cmd_line, 1 );

    if (!is_win64 ^ !(binary_info->flags & BINARY_FLAG_64BIT))
        loader = get_alternate_loader( &wineloader );

    if (!(pid = fork()))  /* child */
    {
        if (!(pid = fork()))  /* grandchild */
        {
            char preloader_reserve[64], socket_env[64];

            
            if (stdin_fd != -1) dup2( stdin_fd, 0 );
            if (stdout_fd != -1) dup2( stdout_fd, 1 );
            

            if (stdin_fd != -1) close( stdin_fd );
            if (stdout_fd != -1) close( stdout_fd );

            /* Reset signals that we previously set to SIG_IGN */
            signal( SIGPIPE, SIG_DFL );

            sprintf( socket_env, "WINESERVERSOCKET=%u", socketfd );
            sprintf( preloader_reserve, "WINEPRELOADRESERVE=%x%08x-%x%08x",
                     (ULONG)(binary_info->res_start >> 32), (ULONG)binary_info->res_start,
                     (ULONG)(binary_info->res_end >> 32), (ULONG)binary_info->res_end );

            putenv( preloader_reserve );
            putenv( socket_env );
            if (winedebug) putenv( winedebug );
            if (wineloader) putenv( wineloader );
            if (unixdir) chdir(unixdir);

            if (argv)
            {
                do
                {
                    wine_exec_wine_binary( loader, argv, getenv("WINELOADER") );
                }while (0);
            }
            _exit(1);
        }

        _exit(pid == -1);
    }

    if (pid != -1)
    {
        /* reap child */
        pid_t wret;
        do {
            wret = waitpid(pid, NULL, 0);
        } while (wret < 0 && errno == EINTR);
    }

    RtlFreeHeap( GetProcessHeap(), 0, wineloader );
    RtlFreeHeap( GetProcessHeap(), 0, argv );
    return pid;
}

/**********************************************************************
 *           RtlCreateUserProcess [NTDLL.@]
 */
NTSTATUS WINAPI RtlCreateUserProcess(UNICODE_STRING *path, ULONG attributes, RTL_USER_PROCESS_PARAMETERS *parameters,
                                     SECURITY_DESCRIPTOR *process_descriptor, SECURITY_DESCRIPTOR *thread_descriptor,
                                     HANDLE parent, BOOLEAN inherit, HANDLE debug, HANDLE exception,
                                     RTL_USER_PROCESS_INFORMATION *info)
{
    FIXME("(%p %u %p %p %p %p %d %p %p %p): stub\n", path, attributes, parameters, process_descriptor, thread_descriptor,
                                     parent, inherit, debug, exception, info);
    return STATUS_NOT_IMPLEMENTED;
}


