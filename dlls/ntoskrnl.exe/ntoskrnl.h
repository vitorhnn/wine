#include "ddk/wdm.h"

struct _KTHREAD
{
    DISPATCHER_HEADER header; /* This is here for compatibility reasons */
    KWAIT_BLOCK WaitBlock[4];
    PEPROCESS Process;
    LIST_ENTRY MutantListHead;

    /* wine specific data*/
    HANDLE wakeup_event;
};

struct _EPROCESS
{
    DISPATCHER_HEADER header; /* This is here for compatibility reasons */
    DWORD Pid;
    PPEB PebAddress;
    HANDLE ProcessHandle;
};

struct _OBJECT_TYPE
{
    UNICODE_STRING Name;
};

/* Kernel Object Types - Taken from ReactOS
 * https://doxygen.reactos.org/dd/d83/ndk_2ketypes_8h_source.html#l00385*/
typedef enum _KOBJECTS
{
    EventNotificationObject = 0,
    EventSynchronizationObject = 1,
    MutantObject = 2,
    ProcessObject = 3,
    QueueObject = 4,
    SemaphoreObject = 5,
    ThreadObject = 6,
    GateObject = 7,
    TimerNotificationObject = 8,
    TimerSynchronizationObject = 9,
    Spare2Object = 10,
    Spare3Object = 11,
    Spare4Object = 12,
    Spare5Object = 13,
    Spare6Object = 14,
    Spare7Object = 15,
    Spare8Object = 16,
    Spare9Object = 17,
    ApcObject = 18,
    DpcObject = 19,
    DeviceQueueObject = 20,
    EventPairObject = 21,
    InterruptObject = 22,
    ProfileObject = 23,
    ThreadedDpcObject = 24,
    MaximumKernelObject = 25
} KOBJECTS;
