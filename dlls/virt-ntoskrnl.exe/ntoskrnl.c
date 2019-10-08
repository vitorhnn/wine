#include <wine/asm.h>

int DllMainCRTStartup(void) {}

int __wine_ntoskrnl_entry(void)
{
    return 1;
}


