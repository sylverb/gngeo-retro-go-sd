#ifndef _RESFILE_H_
#define _RESFILE_H_
#include "roms.h"
static inline ROM_DEF *res_load_drv(char *name) { (void)name; return NULL; }
static inline int res_verify_datafile(char *file) { (void)file; return 0; }
#ifdef GNGEO_DUMP_TOOL
/* Host decrypt tool supplies a real loader (cmc50.xor, …). */
void *res_load_data(char *name);
#else
static inline void *res_load_data(char *name) { (void)name; return NULL; }
#endif
#endif
