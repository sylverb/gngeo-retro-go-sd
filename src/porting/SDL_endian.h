#ifndef SDL_ENDIAN_H
#define SDL_ENDIAN_H

#include "SDL_types.h"

#define SDL_LIL_ENDIAN  1234
#define SDL_BIG_ENDIAN  4321
#define SDL_BYTEORDER   SDL_LIL_ENDIAN

static inline Uint16 SDL_Swap16(Uint16 x)
{
    return (Uint16)((x << 8) | (x >> 8));
}

static inline Uint32 SDL_Swap32(Uint32 x)
{
    return ((x << 24) | ((x << 8) & 0x00FF0000u) |
            ((x >> 8) & 0x0000FF00u) | (x >> 24));
}

#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)
#define SDL_SwapBE16(x) SDL_Swap16(x)
#define SDL_SwapBE32(x) SDL_Swap32(x)

#endif
