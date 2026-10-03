#ifndef NEO_FLASH_RO_H
#define NEO_FLASH_RO_H

#ifdef __cplusplus
extern "C" {
#endif

/* SD path of the cold .text/.rodata sidecar (next to gngeo.bin). */
#define NEO_FLASH_RO_PATH "/cores/gngeo.ro"

/* Map gngeo.ro into QSPI and patch RAM_EMU absolute CAFE pointers.
 * Returns 0 on failure (missing file). Host: no-op success. */
int neo_load_flash_cold(void);

/* Size of the mapped gngeo.ro blob (0 before load / on host). */
uint32_t neo_flash_ro_size(void);

#ifdef __cplusplus
}
#endif

#endif
