#include "ddk/wdm.h"

typedef struct _KTHREAD
{
    DISPATCHER_HEADER header; /* This is here for compatibility reasons */
    KWAIT_BLOCK WaitBlock[4];
    LIST_ENTRY MutantListHead;

    /* wine specific data*/
    HANDLE wakeup_event;
};
