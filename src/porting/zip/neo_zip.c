/*
 * Core-local ZIP reader (miniz + tinfl). Scratch from caller (ram_emu).
 * Dual-open for Neo Geo CROM interleave.
 */
#include "neo_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

#ifndef NEO_ZIP_MAX_OPEN
#define NEO_ZIP_MAX_OPEN 2
#endif

static int neo_tolower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

static int neo_strcasecmp(const char *a, const char *b)
{
    int ca, cb;
    if (!a || !b)
        return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        ca = neo_tolower((unsigned char)*a++);
        cb = neo_tolower((unsigned char)*b++);
        if (ca != cb)
            return ca - cb;
    }
    return neo_tolower((unsigned char)*a) - neo_tolower((unsigned char)*b);
}

#define NEO_ZIP_LOCAL_SIG        0x04034b50u
#define NEO_ZIP_LOCAL_HDR_SIZE   30
#define NEO_ZIP_LDH_FILENAME_LEN 26
#define NEO_ZIP_LDH_EXTRA_LEN    28
#define NEO_ZIP_GPBF_ENCRYPTED   1u
#define NEO_ZIP_GPBF_STRONG_ENC  64u

#define NEO_ZIP_IO_SIZE   4096
#define NEO_ZIP_DICT_SIZE TINFL_LZ_DICT_SIZE
#define NEO_ZIP_SLOT_BYTES (NEO_ZIP_IO_SIZE + NEO_ZIP_DICT_SIZE)

struct neo_zip {
    mz_zip_archive zip;
    int            alive;
};

struct neo_zip_file {
    neo_zip_t         *owner;
    tinfl_decompressor inflator;
    uint8_t           *in_buf;
    uint8_t           *dict;
    uint64_t           cur_ofs;
    uint32_t           comp_remaining;
    uint32_t           uncomp_size;
    uint32_t           out_ofs;
    uint32_t           method;
    size_t             read_buf_avail;
    size_t             read_buf_ofs;
    size_t             out_blk_remain;
    tinfl_status       status;
    int                in_use;
};

static neo_zip_t      s_zip_slot;
static neo_zip_file_t s_file_slots[NEO_ZIP_MAX_OPEN];
static uint8_t       *s_scratch;
static uint32_t       s_scratch_bytes;

uint32_t neo_zip_scratch_bytes(void)
{
    return (uint32_t)NEO_ZIP_MAX_OPEN * (uint32_t)NEO_ZIP_SLOT_BYTES;
}

void neo_zip_set_scratch(void *buf, uint32_t bytes)
{
    s_scratch = (uint8_t *)buf;
    s_scratch_bytes = bytes;
}

static uint8_t *neo_zip_slot_io(int idx)
{
    return s_scratch + (size_t)idx * NEO_ZIP_SLOT_BYTES;
}

static uint8_t *neo_zip_slot_dict(int idx)
{
    return neo_zip_slot_io(idx) + NEO_ZIP_IO_SIZE;
}

neo_zip_t *neo_zip_open(const char *path)
{
    if (!path || !path[0])
        return NULL;
    if (!s_scratch || s_scratch_bytes < neo_zip_scratch_bytes()) {
        printf("neo_zip: scratch not set (%u need %u)\n",
               (unsigned)s_scratch_bytes, (unsigned)neo_zip_scratch_bytes());
        return NULL;
    }
    if (s_zip_slot.alive) {
        printf("neo_zip: only one archive open at a time\n");
        return NULL;
    }
    memset(&s_zip_slot, 0, sizeof(s_zip_slot));
    memset(&s_zip_slot.zip, 0, sizeof(s_zip_slot.zip));
    if (!mz_zip_reader_init_file(&s_zip_slot.zip, path, 0)) {
        printf("neo_zip: open failed %s (%s)\n", path,
               mz_zip_get_error_string(mz_zip_get_last_error(&s_zip_slot.zip)));
        return NULL;
    }
    s_zip_slot.alive = 1;
    return &s_zip_slot;
}

void neo_zip_close(neo_zip_t *z)
{
    int i;
    if (!z || !z->alive)
        return;
    for (i = 0; i < NEO_ZIP_MAX_OPEN; i++) {
        if (s_file_slots[i].in_use && s_file_slots[i].owner == z)
            neo_zip_fclose(&s_file_slots[i]);
    }
    mz_zip_reader_end(&z->zip);
    z->alive = 0;
}

neo_zip_file_t *neo_zip_fopen(neo_zip_t *z, const char *name, uint32_t crc)
{
    int idx = -1;
    int i;
    mz_uint file_index;
    mz_zip_archive_file_stat st;
    mz_uint8 local_header[NEO_ZIP_LOCAL_HDR_SIZE];
    uint64_t data_ofs;

    if (!z || !z->alive || !name)
        return NULL;

    for (i = 0; i < NEO_ZIP_MAX_OPEN; i++) {
        if (!s_file_slots[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        printf("neo_zip: too many open members (max %d)\n", NEO_ZIP_MAX_OPEN);
        return NULL;
    }

    file_index = (mz_uint)-1;
    {
        mz_uint n = mz_zip_reader_get_num_files(&z->zip);
        mz_uint fi;
        for (fi = 0; fi < n; fi++) {
            if (!mz_zip_reader_file_stat(&z->zip, fi, &st))
                continue;
            if (st.m_is_directory)
                continue;
            if (neo_strcasecmp(st.m_filename, name) != 0) {
                const char *base = strrchr(st.m_filename, '/');
                base = base ? base + 1 : st.m_filename;
                if (neo_strcasecmp(base, name) != 0)
                    continue;
            }
            if (crc != 0 && st.m_crc32 != crc)
                continue;
            file_index = fi;
            break;
        }
    }
    if (file_index == (mz_uint)-1) {
        printf("neo_zip: member not found: %s\n", name);
        return NULL;
    }
    if (!mz_zip_reader_file_stat(&z->zip, file_index, &st))
        return NULL;

    if ((st.m_bit_flag & (NEO_ZIP_GPBF_ENCRYPTED | NEO_ZIP_GPBF_STRONG_ENC)) ||
        st.m_is_encrypted) {
        printf("neo_zip: encrypted member %s\n", name);
        return NULL;
    }
    if (st.m_method != 0 && st.m_method != MZ_DEFLATED) {
        printf("neo_zip: unsupported method %u for %s\n",
               (unsigned)st.m_method, name);
        return NULL;
    }

    data_ofs = st.m_local_header_ofs;
    if (z->zip.m_pRead(z->zip.m_pIO_opaque, data_ofs, local_header,
                       NEO_ZIP_LOCAL_HDR_SIZE) != NEO_ZIP_LOCAL_HDR_SIZE) {
        printf("neo_zip: local header read failed for %s\n", name);
        return NULL;
    }
    if (MZ_READ_LE32(local_header) != NEO_ZIP_LOCAL_SIG) {
        printf("neo_zip: bad local header for %s\n", name);
        return NULL;
    }
    data_ofs += NEO_ZIP_LOCAL_HDR_SIZE
        + MZ_READ_LE16(local_header + NEO_ZIP_LDH_FILENAME_LEN)
        + MZ_READ_LE16(local_header + NEO_ZIP_LDH_EXTRA_LEN);

    memset(&s_file_slots[idx], 0, sizeof(s_file_slots[idx]));
    s_file_slots[idx].owner = z;
    s_file_slots[idx].in_buf = neo_zip_slot_io(idx);
    s_file_slots[idx].dict = neo_zip_slot_dict(idx);
    s_file_slots[idx].cur_ofs = data_ofs;
    s_file_slots[idx].comp_remaining = (uint32_t)st.m_comp_size;
    s_file_slots[idx].uncomp_size = (uint32_t)st.m_uncomp_size;
    s_file_slots[idx].method = st.m_method;
    s_file_slots[idx].status = TINFL_STATUS_DONE;
    s_file_slots[idx].in_use = 1;
    if (st.m_method == MZ_DEFLATED)
        tinfl_init(&s_file_slots[idx].inflator);
    return &s_file_slots[idx];
}

int neo_zip_fread(neo_zip_file_t *f, void *buf, int len)
{
    size_t copied = 0;
    size_t buf_size;
    mz_zip_archive *zip;

    if (!f || !f->in_use || !buf || len <= 0 || !f->owner)
        return -1;
    zip = &f->owner->zip;
    buf_size = (size_t)len;

    if (f->method == 0) {
        size_t n = buf_size;
        if (n > f->comp_remaining)
            n = f->comp_remaining;
        if (!n)
            return 0;
        if (zip->m_pRead(zip->m_pIO_opaque, f->cur_ofs, buf, n) != n) {
            printf("neo_zip: stored read failed\n");
            return -1;
        }
        f->cur_ofs += n;
        f->comp_remaining -= (uint32_t)n;
        f->out_ofs += (uint32_t)n;
        return (int)n;
    }

    do {
        mz_uint8 *pWrite_cur = f->dict + (f->out_ofs & (NEO_ZIP_DICT_SIZE - 1));
        size_t in_buf_size;
        size_t out_buf_size = NEO_ZIP_DICT_SIZE - (f->out_ofs & (NEO_ZIP_DICT_SIZE - 1));

        if (!f->out_blk_remain) {
            if (!f->read_buf_avail) {
                size_t want = NEO_ZIP_IO_SIZE;
                if (want > f->comp_remaining)
                    want = f->comp_remaining;
                f->read_buf_avail = want;
                if (want) {
                    if (zip->m_pRead(zip->m_pIO_opaque, f->cur_ofs, f->in_buf, want) != want) {
                        printf("neo_zip: deflate input read failed\n");
                        return (copied > 0) ? (int)copied : -1;
                    }
                    f->cur_ofs += want;
                    f->comp_remaining -= (uint32_t)want;
                }
                f->read_buf_ofs = 0;
            }

            in_buf_size = f->read_buf_avail;
            f->status = tinfl_decompress(
                &f->inflator,
                f->in_buf + f->read_buf_ofs, &in_buf_size,
                f->dict, pWrite_cur, &out_buf_size,
                f->comp_remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            f->read_buf_avail -= in_buf_size;
            f->read_buf_ofs += in_buf_size;
            f->out_blk_remain = out_buf_size;

            if (f->status < 0) {
                printf("neo_zip: inflate failed (%d)\n", (int)f->status);
                return (copied > 0) ? (int)copied : -1;
            }
        }

        if (f->out_blk_remain) {
            size_t to_copy = buf_size - copied;
            if (to_copy > f->out_blk_remain)
                to_copy = f->out_blk_remain;
            memcpy((uint8_t *)buf + copied, pWrite_cur, to_copy);
            f->out_blk_remain -= to_copy;
            f->out_ofs += (uint32_t)to_copy;
            copied += to_copy;
            if (f->out_ofs > f->uncomp_size) {
                printf("neo_zip: inflate size overrun\n");
                return -1;
            }
        }
    } while (copied < buf_size &&
             (f->status == TINFL_STATUS_NEEDS_MORE_INPUT ||
              f->status == TINFL_STATUS_HAS_MORE_OUTPUT ||
              f->out_blk_remain));

    return (int)copied;
}

uint32_t neo_zip_fsize(neo_zip_file_t *f)
{
    return f && f->in_use ? f->uncomp_size : 0;
}

void neo_zip_fclose(neo_zip_file_t *f)
{
    if (!f || !f->in_use)
        return;
    f->owner = NULL;
    f->in_buf = NULL;
    f->dict = NULL;
    f->uncomp_size = 0;
    f->in_use = 0;
}

int neo_zip_num_files(neo_zip_t *z)
{
    if (!z || !z->alive)
        return 0;
    return (int)mz_zip_reader_get_num_files(&z->zip);
}

int neo_zip_stat(neo_zip_t *z, int index, char *name_out, unsigned name_sz,
                 uint32_t *uncomp_size, uint32_t *crc32)
{
    mz_zip_archive_file_stat st;
    const char *base;
    unsigned n;

    if (!z || !z->alive || index < 0 || !name_out || name_sz < 2)
        return 0;
    if (!mz_zip_reader_file_stat(&z->zip, (mz_uint)index, &st))
        return 0;
    if (st.m_is_directory)
        return 0;
    base = strrchr(st.m_filename, '/');
    base = base ? base + 1 : st.m_filename;
    if (!base[0])
        return 0;
    n = 0;
    while (base[n] && n + 1u < name_sz) {
        name_out[n] = base[n];
        n++;
    }
    name_out[n] = 0;
    if (uncomp_size)
        *uncomp_size = (uint32_t)st.m_uncomp_size;
    if (crc32)
        *crc32 = (uint32_t)st.m_crc32;
    return 1;
}
