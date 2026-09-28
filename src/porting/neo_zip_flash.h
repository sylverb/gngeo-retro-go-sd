#pragma once

/* Ensure MAME .zip regions are in QSPI (converted where needed) and bind
 * memory.rom. Returns 1 on success. */
int neo_zip_flash_load(const char *zip_path);
const char *neo_zip_flash_last_error(void);

/* Bind flash-mapped cart regions + load system BIOS (shared with .gno). */
int neo_rom_bind_regions(const char *stem,
                         const uint8_t *p, uint32_t p_sz,
                         const uint8_t *m, uint32_t m_sz,
                         const uint8_t *v, uint32_t v_sz,
                         const uint8_t *s, uint32_t s_sz,
                         const uint8_t *gfix, uint32_t g_sz,
                         const uint8_t *c, uint32_t c_sz,
                         const uint8_t *spr, uint32_t spr_sz);
