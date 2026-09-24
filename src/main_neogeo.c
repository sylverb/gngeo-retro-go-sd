/*
 * Neo Geo (GnGeo) dynamic core for Retro-Go SD.
 * Entry: app_main_neogeo
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "rom_manager.h"
#include "odroid_system.h"
#include "appid.h"
#include "odroid_overlay.h"
#include "odroid_settings.h"
#include "gw_malloc.h"
#include "neo_mem.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#include "gw_core_i18n.h"
#else
#include "host_compat.h"
#include "gw_core_i18n.h"
#endif

#include "config.h"
#include "emu.h"
#include "roms.h"
#include "screen.h"
#include "memory.h"
#include "conf.h"
#include "event.h"
#include "gnutil.h"
#include "gngeo_platform.h"
#include "gno_flash.h"
#include "neogeo_i18n.h"
#include "video.h"
#include "neo_state.h"
#include "ym2610/ym2610.h"

extern void (**m68ki_instruction_jump_table)(void);

#ifndef NEO_DISABLE_VIDEO
#define NEO_DISABLE_VIDEO 0
#endif

#define APP_ID       APPID_CORE
#define FPS          60
#define SAMPLE_RATE  18000
#define AUDIO_LENGTH (SAMPLE_RATE / FPS) /* 300 — exact */

static odroid_gamepad_state_t pad;
static const char *boot_fail_reason = NULL;

static bool LoadState(const char *path)
{
    return neo_load_state_path(path) != 0;
}

static bool SaveState(const char *path)
{
    return neo_save_state_path(path) != 0;
}

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    if (lcd_get_active_buffer())
        neo_present_to_lcd(lcd_get_active_buffer());
    return lcd_get_active_buffer();
}

static void neo_repaint(void)
{
#ifndef HOST_BUILD
    neo_bind_lcd_buffer();
#endif
    draw_screen();
    neo_present_to_lcd(lcd_get_active_buffer());
    common_ingame_overlay();
}

static void Shutdown(void) {}

static void SleepWake(void)
{
    odroid_audio_init(SAMPLE_RATE);
    audio_clear_buffers();
    audio_start_playing(AUDIO_LENGTH);
}

static void SramSave(void)
{
    /* TODO: write memory.sram via ODROID_PATH_SAVE_SRAM when dirty */
}

static void map_input(const odroid_gamepad_state_t *joy)
{
    Uint8 buttons = 0;
    if (joy->values[ODROID_INPUT_A])     buttons |= 0x01;
    if (joy->values[ODROID_INPUT_B])     buttons |= 0x02;
    if (joy->values[ODROID_INPUT_X])     buttons |= 0x04; /* C */
    if (joy->values[ODROID_INPUT_Y])     buttons |= 0x08; /* D */
    if (joy->values[ODROID_INPUT_UP])    buttons |= 0x10;
    if (joy->values[ODROID_INPUT_DOWN])  buttons |= 0x20;
    if (joy->values[ODROID_INPUT_LEFT])  buttons |= 0x40;
    if (joy->values[ODROID_INPUT_RIGHT]) buttons |= 0x80;

    /* START + SELECT(coin). VOLUME also inserts coin on G&W. */
    neo_set_input(buttons,
                  joy->values[ODROID_INPUT_START] ? 1 : 0,
                  (joy->values[ODROID_INPUT_SELECT] ||
                   joy->values[ODROID_INPUT_VOLUME]) ? 1 : 0);
}

static void submit_audio(void)
{
    int16_t *buf;
    uint16_t len;
    int32_t vol;
    int i;

    buf = audio_get_active_buffer();
    len = audio_get_buffer_length();
    if (!buf || !len)
        return;

    if (!neo_sound_gen_enabled()) {
        memset(buf, 0, (size_t)len * sizeof(int16_t));
        return;
    }

    neo_render_audio(buf, (int)len);
    vol = common_emu_sound_get_volume();
    if (vol < 255) {
        for (i = 0; i < (int)len; i++)
            buf[i] = (int16_t)((buf[i] * vol) / 255);
    }
}

static bool boot_game(void)
{
    if (!ACTIVE_FILE || !ACTIVE_FILE->path[0]) {
        printf("neogeo: no ACTIVE_FILE\n");
        boot_fail_reason = "no ROM path";
        return false;
    }

    /* Hot pools outside RAM_EMU so the 256 KiB Musashi JT still fits. */
    neo_dtcm_ensure();
    /* FM tables in DTCM (~30 KiB), built with accurate sin/log — not .rodata
     * (that stole JT space → memory_map NULL → hardfault PC=0). */
    if (!YM2610_prepare_tables()) {
        boot_fail_reason = "DTCM OOM (ym tables)";
        return false;
    }
    /* fix_board before init_game (SFIX usage fill). RAM_EMU bump — leave AHB
     * for battery SRAM + firmware menus (savestate calloc ~2 KiB). */
    if (!memory.fix_board_usage) {
        memory.fix_board_usage = ram_calloc(1, 4096);
        if (!neo_alloc_ok(memory.fix_board_usage)) {
            printf("neogeo: FATAL RAM_EMU fix_board_usage\n");
            boot_fail_reason = "RAM OOM (fix)";
            return false;
        }
    }

    cf_init();
    cf_get_item_by_name("dump"); /* ensure dump=true default */
    conf.sound = 1;
    conf.sample_rate = SAMPLE_RATE;
    conf.screen320 = 1;
    conf.system = SYS_UNIBIOS;
    conf.country = CTY_USA;
    /* Retro-Go / host pace via common_emu_frame_loop — never busy-wait. */
    conf.autoframeskip = 0;
    conf.sleep_idle = 0;

    if (screen_init() != GN_TRUE) {
        printf("neogeo: screen_init failed\n");
        boot_fail_reason = "screen_init OOM";
        return false;
    }

    if (init_game((char *)ACTIVE_FILE->path) != GN_TRUE) {
        printf("neogeo: init_game failed\n");
        boot_fail_reason = gno_flash_last_error();
        return false;
    }

    /* Battery SRAM + memcard on AHB. Only fix_board (4 KiB) is on the RAM_EMU
     * bump — memcard before JT would leave <256 KiB and the 68k table fails. */
    if (!memory.sram) {
        memory.sram = ahb_calloc(1, 0x10000);
        if (!neo_alloc_ok(memory.sram)) {
            printf("neogeo: FATAL AHB sram\n");
            boot_fail_reason = "AHB OOM (sram)";
            return false;
        }
        printf("neogeo: AHB sram=%p free=%u\n",
               (void *)memory.sram, (unsigned)ahb_get_free_size());
    }
    if (!memory.memcard) {
        memory.memcard = ahb_calloc(1, 0x800);
        if (!neo_alloc_ok(memory.memcard)) {
            printf("neogeo: FATAL AHB memcard\n");
            boot_fail_reason = "AHB OOM (memcard)";
            return false;
        }
    }

    init_neo();
    if (!m68ki_instruction_jump_table) {
        printf("neogeo: FATAL no 68k jump table (RAM_EMU too tight)\n");
        boot_fail_reason = "RAM OOM (68k JT)";
        return false;
    }
    printf("neogeo: booted %s (AHB free=%u RAM free=%u)\n",
           conf.game ? conf.game : "?",
           (unsigned)ahb_get_free_size(), (unsigned)ram_get_free_size());
    return true;
}

void app_main_neogeo(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    // Set the clock to 3 (300MHz)
    SystemClock_Config(3);

    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[1];

    gw_core_bridge_init();
    memset(&pad, 0, sizeof(pad));

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / FPS + 0.5f);
    lcd_set_refresh_rate(FPS);

    odroid_system_init(APP_ID, SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot,
                           &Shutdown, &SleepWake, &SramSave, NULL);

    options[0] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;

    audio_start_playing(AUDIO_LENGTH);

    if (!boot_game()) {
        uint16_t *fb = lcd_get_active_buffer();
        const char *why = boot_fail_reason ? boot_fail_reason : "unknown";
        if (fb) {
            memset(fb, 0, WIDTH * HEIGHT * 2);
            odroid_overlay_draw_text(8, 8, 0, "Neo Geo: boot failed", 0xFFFF, 0);
            odroid_overlay_draw_text(8, 28, 0, (char *)why, 0xFFFF, 0);
            lcd_swap();
        }
        while (1) {
            wdog_refresh();
            odroid_input_read_gamepad(&joystick);
            common_emu_input_loop(&joystick, options, &neo_repaint);
        }
    }

    if (load_state)
        odroid_system_emu_load_state(save_slot);
    else
        lcd_clear_buffers();

    while (1) {
        wdog_refresh();

        bool draw_frame = common_emu_frame_loop();
        /* Like main_gw: never blit while LTDC still owns the active FB.
         * Do not lcd_sleep_while_swap_pending() here — audio sync paces us. */
        bool can_present = draw_frame && !NEO_DISABLE_VIDEO;
#ifndef HOST_BUILD
        if (lcd_is_swap_pending())
            can_present = false;
#endif

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &neo_repaint);
        common_emu_input_loop_handle_turbo(&joystick);
        pad = joystick;
        map_input(&joystick);

        /* Emu first, then audio, then video. A slow draw_screen must not
         * sit between YM fill and DMA — that was the voice chop/repeat. */
        neo_run_frame(can_present ? 1 : 0);
        submit_audio();
        common_emu_sound_sync(false);

        if (neo_frame_needs_draw()) {
#ifndef HOST_BUILD
            neo_bind_lcd_buffer();
#endif
            neo_draw_frame();
            neo_present_to_lcd(lcd_get_active_buffer());
            common_ingame_overlay();
            lcd_swap();
        }
    }
}
