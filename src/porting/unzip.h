#ifndef _UNZIP_H_
#define _UNZIP_H_
#include <stdint.h>
typedef void PKZIP;
typedef void ZFILE;
static inline PKZIP *gn_open_zip(const char *path) { (void)path; return NULL; }
static inline void gn_close_zip(PKZIP *z) { (void)z; }
static inline uint8_t *gn_unzip_file_malloc(PKZIP *z, const char *name, uint32_t off, unsigned int *sz)
{
    (void)z; (void)name; (void)off; if (sz) *sz = 0; return NULL;
}
#endif
