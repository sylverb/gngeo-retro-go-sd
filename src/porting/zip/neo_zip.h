#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Core-local ZIP reader (miniz). Dual-open for Neo Geo CROM interleave.
 * Inflate scratch is allocated from ram_emu via neo_zip_set_scratch()
 * before open (freed/forgotten by the caller's ram_init()). */

typedef struct neo_zip neo_zip_t;
typedef struct neo_zip_file neo_zip_file_t;

/* Provide IO+dict scratch: need neo_zip_scratch_bytes() before any open. */
uint32_t neo_zip_scratch_bytes(void);
void neo_zip_set_scratch(void *buf, uint32_t bytes);

neo_zip_t *neo_zip_open(const char *path);
void neo_zip_close(neo_zip_t *z);

/* crc may be 0 to skip CRC check on locate. */
neo_zip_file_t *neo_zip_fopen(neo_zip_t *z, const char *name, uint32_t crc);
int neo_zip_fread(neo_zip_file_t *f, void *buf, int len);
uint32_t neo_zip_fsize(neo_zip_file_t *f);
void neo_zip_fclose(neo_zip_file_t *f);

/* Central-directory enumeration (for MAME set auto-detect). */
int neo_zip_num_files(neo_zip_t *z);
/* name_out gets basename only (truncated). Returns 0 on bad index / dir. */
int neo_zip_stat(neo_zip_t *z, int index, char *name_out, unsigned name_sz,
                 uint32_t *uncomp_size, uint32_t *crc32);

#ifdef __cplusplus
}
#endif
