#ifndef _RESFILE_H_
#define _RESFILE_H_
#include "roms.h"
static inline ROM_DEF *res_load_drv(char *name) { (void)name; return NULL; }
static inline int res_verify_datafile(char *file) { (void)file; return 0; }
static inline void *res_load_data(char *name) { (void)name; return NULL; }
#endif
