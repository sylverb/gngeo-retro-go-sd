/*
 * Host stand-ins for gw_malloc.h pools.
 * Device zeros every *_malloc / *_calloc via mem_ctl — match that here.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gw_malloc.h"

uint32_t ram_start;

static uint8_t *ram_pool;
static size_t ram_pool_size;
static size_t ram_pool_used;

static void host_ram_pool_ensure(void)
{
    if (ram_pool)
        return;
    ram_pool_size = 1u * 1024u * 1024u;
    ram_pool = (uint8_t *)malloc(ram_pool_size);
    ram_pool_used = 0;
    if (ram_pool)
        ram_start = (uint32_t)(uintptr_t)ram_pool;
}

void *ram_malloc(size_t size)
{
    void *p;

    host_ram_pool_ensure();
    size = (size + 7u) & ~7u;
    if (!ram_pool || ram_pool_used + size > ram_pool_size)
        return NULL;
    p = ram_pool + ram_pool_used;
    ram_pool_used += size;
    memset(p, 0, size);
    return p;
}

void *ram_calloc(size_t count, size_t size)
{
    return ram_malloc(count * size);
}

size_t ram_get_free_size(void)
{
    host_ram_pool_ensure();
    return ram_pool ? (ram_pool_size - ram_pool_used) : 0;
}

void ram_init(void)
{
    host_ram_pool_ensure();
    ram_pool_used = 0;
}

void *ahb_malloc(size_t size) { return calloc(1, size); }
void *ahb_calloc(size_t count, size_t size) { return calloc(count, size); }
size_t ahb_get_free_size(void) { return 16u * 1024u * 1024u; }

void itc_init(void) {}
void *itc_malloc(size_t size) { return calloc(1, size); }
void *itc_calloc(size_t count, size_t size) { return calloc(count, size); }
size_t itc_get_free_size(void) { return 64u * 1024u; }

void dtc_init(void) {}
void *dtc_malloc(size_t size) { return calloc(1, size); }
void *dtc_calloc(size_t count, size_t size) { return calloc(count, size); }
size_t dtc_get_free_size(void) { return 104u * 1024u; }

/* Aliases some cores still call. */
void dtcm_init(void) { dtc_init(); }
void *dtcm_malloc(size_t size) { return dtc_malloc(size); }
void *dtcm_calloc(size_t count, size_t size) { return dtc_calloc(count, size); }
size_t dtcm_get_free_size(void) { return dtc_get_free_size(); }
