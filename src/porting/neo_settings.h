#ifndef NEO_SETTINGS_H
#define NEO_SETTINGS_H

#include <stdint.h>

/* Hardware presentation for UniBIOS (STATUS_A/B + BRDFIX). */
typedef enum {
    NEO_HW_AES = 0, /* Console default */
    NEO_HW_MVS = 1, /* Arcade */
} neo_hw_t;

/* UniBIOS / stock nationality byte at BIOS $401. */
typedef enum {
    NEO_REGION_JAPAN = 0,
    NEO_REGION_USA = 1,
    NEO_REGION_EUROPE = 2,
} neo_region_t;

neo_hw_t neo_settings_hw_get(void);
void neo_settings_hw_set(neo_hw_t hw);

neo_region_t neo_settings_region_get(void);
void neo_settings_region_set(neo_region_t region);

static inline int neo_settings_is_aes(void)
{
    return neo_settings_hw_get() == NEO_HW_AES;
}

/* Stamp UniBIOS prefs into backup RAM / memcard (call after buffers exist). */
void neo_settings_apply_unibios(void);

int neo_settings_swap_ab_get(void);
void neo_settings_swap_ab_set(int enabled);
int neo_settings_swap_cd_get(void);
void neo_settings_swap_cd_set(int enabled);

#endif
