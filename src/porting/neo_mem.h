/*
 * Shared pool helpers — same call sites on device and HOST_BUILD.
 * Device: gw_malloc.h → firmware ABI. Host: host_malloc.c → calloc.
 *
 * Prefer including this (or gw_malloc.h) over raw malloc for hot buffers.
 * In TUs that already pull gw_core_bridge.h, declare neo_alloc_ok /
 * neo_dtcm_ensure / dtc_* instead of including this header (ram_start clash).
 */
#ifndef NEO_MEM_H
#define NEO_MEM_H

#include <stddef.h>
#include <stdint.h>
#include "gw_malloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DTCM bump once per session (host: no-op init, calloc backend). */
void neo_dtcm_ensure(void);

/* Reject NULL and the ITCM/DTCM failure sentinel used on device. */
int neo_alloc_ok(const void *p);

#ifdef __cplusplus
}
#endif

#endif
