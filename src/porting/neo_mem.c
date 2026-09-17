#include "neo_mem.h"

void neo_dtcm_ensure(void)
{
    static int inited;
    if (!inited) {
        dtc_init();
        inited = 1;
    }
}

int neo_alloc_ok(const void *p)
{
    if (!p)
        return 0;
#ifndef HOST_BUILD
    if ((uintptr_t)p == 0xffffffffu)
        return 0;
#endif
    return 1;
}
