/* zlib stand-in — save states use FILE* via state stubs.
 * On HOST_BUILD, defer to the system zlib (needed to expand type-1 .gno tiles).
 */
#ifndef ZLIB_H
#ifdef HOST_BUILD
#include_next <zlib.h>
#else
#define ZLIB_H

#include <stdio.h>
#include <stdint.h>

typedef FILE *gzFile;
typedef unsigned long uLongf;
typedef unsigned char Bytef;

#define Z_OK 0
#define Z_BEST_COMPRESSION 9

static inline gzFile gzopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}
static inline int gzclose(gzFile f) { return f ? fclose(f) : 0; }
static inline int gzread(gzFile f, void *buf, unsigned len)
{
    return (int)fread(buf, 1, len, f);
}
static inline int gzwrite(gzFile f, void *buf, unsigned len)
{
    return (int)fwrite(buf, 1, len, f);
}
static inline int compress(Bytef *dest, uLongf *destLen,
                           const Bytef *source, uLongf sourceLen)
{
    (void)dest; (void)destLen; (void)source; (void)sourceLen;
    return -1;
}
static inline uLongf compressBound(uLongf sourceLen) { return sourceLen + 64; }

/* Defined in lib_stubs.c — type-1 tile cache unused with XIP .gno. */
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source, uLongf sourceLen);

#endif /* !HOST_BUILD */
#endif /* ZLIB_H */
