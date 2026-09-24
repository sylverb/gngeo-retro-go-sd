#ifndef NEO_FLASH_RO_H
#define NEO_FLASH_RO_H

#ifdef __cplusplus
extern "C" {
#endif

/* SD path of the cold .text/.rodata sidecar (next to neogeo.bin). */
#define NEO_FLASH_RO_PATH "/cores/neogeo.ro"

/* Map neogeo.ro into QSPI and patch RAM_EMU absolute CAFE pointers.
 * Returns 0 on failure (missing file). Host: no-op success. */
int neo_load_flash_cold(void);

#ifdef __cplusplus
}
#endif

#endif
