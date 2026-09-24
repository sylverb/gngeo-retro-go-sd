/*
 * Load /cores/neogeo.ro into the QSPI flash cache and rebase 0xCAFE…
 * absolute addresses in RAM_EMU (Zelda3-style sidecar).
 *
 * Kept in its own TU so main_neogeo.o is included in the rewrite walk —
 * otherwise veneer literals for neo_save / gno_flash_load stay at CAFE.
 */
#include <stdint.h>
#include <stdio.h>

#include "odroid_overlay.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#endif

#include "neo_flash_ro.h"

#ifndef HOST_BUILD
extern uint32_t __RAM_EMU_START__;
extern uint32_t __CORE_BSS_END__;
extern uint32_t __neo_flash_start__;
extern uint32_t __neo_flash_end__;
extern uint32_t _NEO_FLASH_RO_CODE_START;
extern uint32_t _NEO_FLASH_RO_CODE_END;

static int neo_rebase_cafe_word(uint32_t *word, uint32_t cafe_base, uint32_t cafe_size,
                                int32_t offset)
{
    uint32_t value = *word;
    uint32_t bare = value & ~1u;
    if (bare >= cafe_base && bare < cafe_base + cafe_size) {
        *word = (bare + (uint32_t)offset) | (value & 1u);
        return 1;
    }
    return 0;
}

static void neo_flash_relocate_cb(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                                  uint8_t *file_address, uint32_t file_size)
{
    uint32_t cafe_base = (uint32_t)(uintptr_t)&__neo_flash_start__;
    uint32_t cafe_size = (uint32_t)(uintptr_t)&__neo_flash_end__ - cafe_base;
    int32_t offset = (int32_t)((uint32_t)(uintptr_t)file_address - cafe_base);
    uint32_t *p = (uint32_t *)buffer;
    uint32_t n = length / 4;
    uint32_t i;

    (void)offset_in_file;
    (void)file_size;
    for (i = 0; i < n; i++) {
        neo_rebase_cafe_word(&p[i], cafe_base, cafe_size, offset);
        if ((i & 0xffu) == 0)
            wdog_refresh();
    }
}

static void neo_patch_ram_to_flash(uint8_t *flash, uint32_t flash_len)
{
    uint32_t *ptr = (uint32_t *)&__RAM_EMU_START__;
    uint32_t *end = (uint32_t *)&__CORE_BSS_END__;
    uint32_t cafe_base = (uint32_t)(uintptr_t)&__neo_flash_start__;
    int32_t offset = (int32_t)((uint32_t)(uintptr_t)flash - cafe_base);
    uint32_t *skip0 = (uint32_t *)&_NEO_FLASH_RO_CODE_START;
    uint32_t *skip1 = (uint32_t *)&_NEO_FLASH_RO_CODE_END;

    printf("neogeo: flash cold @ %p len=%lu cafe=0x%08lx off=0x%08lx\n",
           (void *)flash, (unsigned long)flash_len,
           (unsigned long)cafe_base, (unsigned long)(uint32_t)offset);

    while (ptr < end) {
        if (ptr < skip0 || ptr > skip1) {
            if (neo_rebase_cafe_word(ptr, cafe_base, flash_len, offset))
                wdog_refresh();
        }
        ptr++;
        if (((uintptr_t)ptr & 0x3ffu) == 0)
            wdog_refresh();
    }
}

int neo_load_flash_cold(void)
{
    uint32_t len = 0;
    uint8_t *p;

    p = odroid_overlay_cache_file_in_flash_relocate(
        NEO_FLASH_RO_PATH, &len, false, neo_flash_relocate_cb);
    if (!p || len == 0) {
        printf("neogeo: missing %s\n", NEO_FLASH_RO_PATH);
        return 0;
    }
    neo_patch_ram_to_flash(p, len);
    return 1;
}
#else
int neo_load_flash_cold(void)
{
    return 1;
}
#endif
