#include "neo_settings.h"

#include <string.h>

#include "odroid_settings.h"
#include "memory.h"
#include "emu.h"
#include "event.h"

#define NEO_CFG_HW     "neo_hw"
#define NEO_CFG_REGION "neo_region"
#define NEO_CFG_SWAP_AB "neo_swap_ab"
#define NEO_CFG_SWAP_CD "neo_swap_cd"

neo_hw_t neo_settings_hw_get(void)
{
    int32_t v = odroid_settings_app_int32_get(NEO_CFG_HW, (int32_t)NEO_HW_AES);
    if (v != (int32_t)NEO_HW_MVS)
        return NEO_HW_AES;
    return NEO_HW_MVS;
}

void neo_settings_hw_set(neo_hw_t hw)
{
    odroid_settings_app_int32_set(NEO_CFG_HW, (int32_t)hw);
}

neo_region_t neo_settings_region_get(void)
{
    int32_t v = odroid_settings_app_int32_get(NEO_CFG_REGION,
                                              (int32_t)NEO_REGION_EUROPE);
    if (v < (int32_t)NEO_REGION_JAPAN || v > (int32_t)NEO_REGION_EUROPE)
        return NEO_REGION_EUROPE;
    return (neo_region_t)v;
}

void neo_settings_region_set(neo_region_t region)
{
    odroid_settings_app_int32_set(NEO_CFG_REGION, (int32_t)region);
}

/*
 * UniBIOS 4.x stores a small header at the start of backup RAM (MVS) /
 * memory card (AES):
 *   [0..1] "V2"
 *   [2]    0x80 = Arcade, 0x00 = Console  (matches BIOS $400 type byte)
 *   [3]    region: 0=Japan, 1=USA, 2=Europe
 *   [4]    flags (0x11 observed as default)
 * MVS also needs "BACKUP RAM OK !" at $10 or UniBIOS re-inits and wipes it.
 */
static void stamp_unibios_header(Uint8 *buf, int aes, neo_region_t region)
{
    if (!buf)
        return;
    buf[0] = 'V';
    buf[1] = '2';
    buf[2] = aes ? 0x00 : 0x80;
    buf[3] = (Uint8)region;
    if (buf[4] == 0)
        buf[4] = 0x11;
}

static void ensure_bram_ok(Uint8 *s)
{
    static const char ok[] = "BACKUP RAM OK !";
    static const Uint8 cab_init[] = {
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03,
        0x01, 0x00, 0x30, 0x3b
    };

    if (!s)
        return;
    if (memcmp(s + 0x10, ok, 15) == 0)
        return;

    /* Fresh BRAM: mark valid so UniBIOS keeps our V2 header. */
    memcpy(s + 0x10, ok, 15);
    s[0x1f] = 0x80;
    memcpy(s + 0x3a, cab_init, sizeof(cab_init));
    s[0x1ac] = 0xf0;
    s[0x1ad] = 0xf0;
    s[0x1ae] = 0xf0;
    s[0x1af] = 0xf0;
    s[0x1b0] = 0xff;
    s[0x1b1] = 0xff;
}

void neo_settings_apply_unibios(void)
{
    int aes = neo_settings_is_aes();
    neo_region_t region = neo_settings_region_get();

    conf.system = SYS_UNIBIOS;
    switch (region) {
    case NEO_REGION_JAPAN:  conf.country = CTY_JAPAN;  break;
    case NEO_REGION_USA:    conf.country = CTY_USA;    break;
    default:                conf.country = CTY_EUROPE; break;
    }

    if (memory.sram) {
        ensure_bram_ok(memory.sram);
        stamp_unibios_header(memory.sram, aes, region);
    }
    /* AES UniBIOS persists prefs on the memory card. */
    if (memory.memcard)
        stamp_unibios_header(memory.memcard, aes, region);

    /* Present AES/MVS on STATUS immediately (before any 68k BIOS code). */
    neo_set_input(0, 0, 0);
}

int neo_settings_swap_ab_get(void)
{
    return odroid_settings_app_int32_get(NEO_CFG_SWAP_AB, 1) != 0;
}

void neo_settings_swap_ab_set(int enabled)
{
    odroid_settings_app_int32_set(NEO_CFG_SWAP_AB, enabled ? 1 : 0);
}

int neo_settings_swap_cd_get(void)
{
    return odroid_settings_app_int32_get(NEO_CFG_SWAP_CD, 0) != 0;
}

void neo_settings_swap_cd_set(int enabled)
{
    odroid_settings_app_int32_set(NEO_CFG_SWAP_CD, enabled ? 1 : 0);
}
