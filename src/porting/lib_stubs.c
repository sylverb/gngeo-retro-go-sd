/* Missing libc / zlib stubs for freestanding Neo Geo core. */
#include <stdint.h>
#include <stddef.h>

#ifndef HOST_BUILD
#include "zlib.h"
#endif

int usleep(unsigned int usec)
{
    (void)usec;
    return 0;
}

int raise(int sig)
{
    (void)sig;
    return 0;
}

#ifndef HOST_BUILD
/* Device: type-1 .gno is unsupported (use make_gno_xip.py). Host links real -lz. */
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLongf sourceLen)
{
    (void)dest;
    (void)destLen;
    (void)source;
    (void)sourceLen;
    return -1;
}
#endif
