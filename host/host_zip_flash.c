/*
 * Host stand-in for firmware store_data_* / lookup blob cache.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gw_flash_alloc.h"

#ifndef HOST_ZIP_MAX_BLOBS
#define HOST_ZIP_MAX_BLOBS 64
#endif

typedef struct {
    char     key[64];
    uint8_t *data;
    uint32_t size;
    int      used;
} host_blob_t;

static host_blob_t s_blobs[HOST_ZIP_MAX_BLOBS];
static void (*s_progress_cb)(uint32_t done, uint32_t total);
static char s_stream_key[64];
/* flash_stream_t.flash_address is uint32_t (device XIP). On host we keep the
 * real malloc pointer here — truncating to 32 bits segfaults on arm64/x86_64. */
static uint8_t *s_stream_buf;

static host_blob_t *blob_find(const char *key)
{
    int i;
    for (i = 0; i < HOST_ZIP_MAX_BLOBS; i++) {
        if (s_blobs[i].used && strcmp(s_blobs[i].key, key) == 0)
            return &s_blobs[i];
    }
    return NULL;
}

static host_blob_t *blob_alloc_slot(void)
{
    int i;
    for (i = 0; i < HOST_ZIP_MAX_BLOBS; i++) {
        if (!s_blobs[i].used) {
            memset(&s_blobs[i], 0, sizeof(s_blobs[i]));
            return &s_blobs[i];
        }
    }
    return NULL;
}

const uint8_t *lookup_data_in_flash(const char *key, uint32_t *size_out)
{
    host_blob_t *b = blob_find(key);
    if (!b)
        return NULL;
    if (size_out)
        *size_out = b->size;
    return b->data;
}

const uint8_t *store_data_in_flash(const char *key, const uint8_t *data,
                                   uint32_t data_size)
{
    host_blob_t *b = blob_find(key);
    if (b && b->size == data_size)
        return b->data;
    if (!b) {
        b = blob_alloc_slot();
        if (!b)
            return NULL;
        memset(b, 0, sizeof(*b));
        snprintf(b->key, sizeof(b->key), "%s", key);
        b->used = 1;
    } else {
        free(b->data);
        b->data = NULL;
    }
    b->data = (uint8_t *)malloc(data_size ? data_size : 1);
    if (!b->data) {
        b->used = 0;
        return NULL;
    }
    if (data_size)
        memcpy(b->data, data, data_size);
    b->size = data_size;
    if (s_progress_cb)
        s_progress_cb(data_size, data_size);
    return b->data;
}

void store_data_set_progress_cb(void (*cb)(uint32_t done, uint32_t total))
{
    s_progress_cb = cb;
}

bool store_data_begin(flash_stream_t *st, const char *key, uint32_t total_size)
{
    uint8_t *buf;
    memset(st, 0, sizeof(*st));
    if (s_stream_buf) {
        fprintf(stderr, "host: store_data_begin while stream active\n");
        return false;
    }
    buf = (uint8_t *)malloc(total_size ? total_size : 1);
    if (!buf) {
        fprintf(stderr, "host: store_data_begin malloc(%u) failed\n",
                (unsigned)total_size);
        return false;
    }
    snprintf(s_stream_key, sizeof(s_stream_key), "%s", key ? key : "");
    s_stream_buf = buf;
    st->flash_address = 1; /* non-zero = active host buffer in s_stream_buf */
    st->total = total_size;
    st->active = true;
    return true;
}

bool store_data_append(flash_stream_t *st, const uint8_t *buf, uint32_t len)
{
    if (!st->active || !s_stream_buf || st->done + len > st->total)
        return false;
    memcpy(s_stream_buf + st->done, buf, len);
    st->done += len;
    st->prog_addr += len;
    if (s_progress_cb)
        s_progress_cb(st->done, st->total);
    return true;
}

const uint8_t *store_data_finish(flash_stream_t *st)
{
    host_blob_t *b;
    uint8_t *data;
    if (!st->active)
        return NULL;
    if (st->done != st->total) {
        store_data_abort(st);
        return NULL;
    }
    st->active = false;
    data = s_stream_buf;
    s_stream_buf = NULL;
    st->flash_address = 0;
    b = blob_find(s_stream_key);
    if (!b)
        b = blob_alloc_slot();
    if (!b) {
        free(data);
        return NULL;
    }
    free(b->data);
    snprintf(b->key, sizeof(b->key), "%s", s_stream_key);
    b->data = data;
    b->size = st->total;
    b->used = 1;
    return b->data;
}

void store_data_abort(flash_stream_t *st)
{
    if (!st->active)
        return;
    st->active = false;
    free(s_stream_buf);
    s_stream_buf = NULL;
    st->flash_address = 0;
}

uint32_t flash_cache_usable_size(void)
{
    /* Host has no OFW reserve — pretend a full 64 MiB chip. */
    return 64u * 1024u * 1024u;
}

void odroid_overlay_draw_progress_bar(const char *header, uint8_t progress)
{
    static int last = -1;
    if ((int)progress == last)
        return;
    last = (int)progress;
    printf("\r%s: %u%%", header ? header : "…", (unsigned)progress);
    if (progress >= 100)
        printf("\n");
    fflush(stdout);
}

bool odroid_overlay_draw_progress_bar_cancellable(const char *header, uint8_t progress)
{
    odroid_overlay_draw_progress_bar(header, progress);
    return true;
}

bool odroid_overlay_progress_poll_cancel(void)
{
    return false;
}
