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
#include "neo_pvc.h"

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
#include "neo_flash_ro.h"
#include "neo_settings.h"
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

/* Like gwenesis: boost to OC lvl 2 only when the launcher setting is stock
 * (0). Do not override OC 1/2/3 the user already chose. Not persisted. */
static void neo_apply_cpu_clock(void)
{
    if (odroid_settings_cpu_oc_level_get() == 0)
        SystemClock_Config(2);
}

/* Hard restart SAI/DMA. Needed after long silent gaps (ROM load) and after
 * SystemClock_Config (audio PLL). audio_start_playing alone can no-op if SAI
 * is still BUSY from a previous half-buffer. */
static void neo_audio_start(void)
{
    audio_stop_playing();
    odroid_audio_init(SAMPLE_RATE);
    audio_clear_buffers();
    audio_start_playing(AUDIO_LENGTH);
    if (common_emu_state.pause_after_frames == 0)
        odroid_audio_mute(false);
}

static void SleepWake(void)
{
    /* gw_sleep restores the settings OC; re-boost when we had auto-forced lvl 2.
     * Always rebuild SAI — clock restore reprograms the audio PLL even when
     * OC was already non-zero. */
    if (odroid_settings_cpu_oc_level_get() == 0)
        SystemClock_Config(2);
    neo_audio_start();
}

static void SramSave(void)
{
    /* TODO: write memory.sram via ODROID_PATH_SAVE_SRAM when dirty */
}

static void map_input(const odroid_gamepad_state_t *joy)
{
    Uint8 buttons = 0;

    int a = joy->values[ODROID_INPUT_A];
    int b = joy->values[ODROID_INPUT_B];
    int c = joy->values[ODROID_INPUT_X];
    int d = joy->values[ODROID_INPUT_Y];

    if (neo_settings_swap_ab_get()) {
        int temp = a;
        a = b;
        b = temp;
    }

    if (neo_settings_swap_cd_get()) {
        int temp = c;
        c = d;
        d = temp;
    }

    if (a) buttons |= 0x01;
    if (b) buttons |= 0x02;
    if (c) buttons |= 0x04; /* C */
    if (d) buttons |= 0x08; /* D */
    if (joy->values[ODROID_INPUT_UP])    buttons |= 0x10;
    if (joy->values[ODROID_INPUT_DOWN])  buttons |= 0x20;
    if (joy->values[ODROID_INPUT_LEFT])  buttons |= 0x40;
    if (joy->values[ODROID_INPUT_RIGHT]) buttons |= 0x80;

    /* START + SELECT(coin/select). */
    neo_set_input(buttons,
                  joy->values[ODROID_INPUT_START] ? 1 : 0,
                  joy->values[ODROID_INPUT_SELECT] ? 1 : 0);
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

    /* YM tables in DTCM first. ZIP cache uses ram_emu then ram_init() —
     * lasting bump allocs (fix_board) happen inside bind after that. */
    neo_dtcm_ensure();
    if (!YM2610_prepare_tables()) {
        boot_fail_reason = "DTCM OOM (ym tables)";
        return false;
    }

    cf_init();
    cf_get_item_by_name("dump"); /* ensure dump=true default */
    conf.sound = 1;
    conf.sample_rate = SAMPLE_RATE;
    conf.screen320 = 1;
    conf.system = SYS_UNIBIOS;
    conf.country = CTY_EUROPE; /* overwritten by neo_settings_apply_unibios */
    /* Retro-Go / host pace via common_emu_frame_loop — never busy-wait. */
    conf.autoframeskip = 0;
    conf.sleep_idle = 0;

    if (screen_init() != GN_TRUE) {
        printf("neogeo: screen_init failed\n");
        boot_fail_reason = "screen_init OOM";
        return false;
    }

    if (init_game((char *)ACTIVE_FILE->path) != GN_TRUE) {
        boot_fail_reason = gno_flash_last_error();
        printf("neogeo: init_game failed: %s\n",
               boot_fail_reason ? boot_fail_reason : "?");
        return false;
    }

    /* Battery SRAM + memcard on AHB (after ZIP so AHB isn't fragmented by
     * transient FatFs/miniz state — heap is freeable but keep sram early). */
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

    /* UniBIOS prefs (AES/MVS + region) before the 68k boots the BIOS. */
    neo_settings_apply_unibios();

    init_neo();
    if (!m68ki_instruction_jump_table) {
        printf("neogeo: FATAL no 68k jump table (RAM_EMU too tight)\n");
        boot_fail_reason = "RAM OOM (68k JT)";
        return false;
    }
    /* PVC 8 KiB after JT so RAM_EMU leftover is known. */
    if (!neo_pvc_ensure_ram()) {
        boot_fail_reason = "OOM (pvc)";
        return false;
    }
    printf("neogeo: booted %s (AHB free=%u RAM free=%u)\n",
           conf.game ? conf.game : "?",
           (unsigned)ahb_get_free_size(), (unsigned)ram_get_free_size());
    return true;
}


/* True if any face/d-pad button is down (POWER excluded — that is sleep). */
static bool neo_any_button(const odroid_gamepad_state_t *j)
{
    for (int i = 0; i < ODROID_INPUT_MAX; i++) {
        if (i == ODROID_INPUT_POWER)
            continue;
        if (j->values[i])
            return true;
    }
    return false;
}

/* Show a fatal boot error until any button is pressed, then return to the
 * launcher. Waits for a release first so a leftover press from launch does
 * not dismiss immediately. */
static void neo_fatal_quit(const char *line1, const char *line2)
{
    odroid_gamepad_state_t joystick;
    uint16_t *fb = lcd_get_active_buffer();

    if (fb) {
        memset(fb, 0, WIDTH * HEIGHT * 2);
        if (line1)
            odroid_overlay_draw_text(8, 8, 0, (char *)line1, 0xFFFF, 0);
        if (line2)
            odroid_overlay_draw_text(8, 28, 0, (char *)line2, 0xFFFF, 0);
        odroid_overlay_draw_text(8, 56, 0, "Press any button", 0xFFFF, 0);
        lcd_swap();
    }

    do {
        wdog_refresh();
        odroid_input_read_gamepad(&joystick);
    } while (neo_any_button(&joystick));

    do {
        wdog_refresh();
        odroid_input_read_gamepad(&joystick);
    } while (!neo_any_button(&joystick));

    odroid_system_switch_app(0);
}

static char neo_opt_hw_str[12];
static char neo_opt_region_str[12];
static char neo_opt_swap_ab_str[4];
static char neo_opt_swap_cd_str[4];

static void neo_hw_to_str(neo_hw_t hw, char *buf)
{
    strcpy(buf, hw == NEO_HW_MVS ? "MVS" : "AES");
}

static void neo_region_to_str(neo_region_t region, char *buf)
{
    switch (region) {
    case NEO_REGION_JAPAN:  strcpy(buf, "Japan");  break;
    case NEO_REGION_USA:    strcpy(buf, "USA");    break;
    default:                strcpy(buf, "Europe"); break;
    }
}

static void neo_apply_and_reset(void)
{
    /*
     * UniBIOS reads region/mode once into 68k RAM at power-on; a plain
     * cpu_68k_reset keeps those values. Clear work RAM + restamp BRAM/
     * memcard + BIOS vectors so the splash re-reads our options.
     */
    neo_settings_apply_unibios();
    memset(memory.ram, 0, 0x10000);
    neo_vector_use_bios();
    neogeo_reset();
}

static bool update_hw_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                         uint32_t repeat)
{
    (void)repeat;
    neo_hw_t hw = neo_settings_hw_get();

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        hw = (hw == NEO_HW_AES) ? NEO_HW_MVS : NEO_HW_AES;
        neo_settings_hw_set(hw);
    }
    neo_hw_to_str(hw, option->value);
    if (event == ODROID_DIALOG_ENTER)
        neo_apply_and_reset();
    return event == ODROID_DIALOG_ENTER;
}

static bool update_region_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event,
                             uint32_t repeat)
{
    (void)repeat;
    int region = (int)neo_settings_region_get();

    if (event == ODROID_DIALOG_PREV)
        region = region > 0 ? region - 1 : 2;
    if (event == ODROID_DIALOG_NEXT)
        region = region < 2 ? region + 1 : 0;
    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT)
        neo_settings_region_set((neo_region_t)region);

    neo_region_to_str((neo_region_t)region, option->value);
    if (event == ODROID_DIALOG_ENTER)
        neo_apply_and_reset();
    return event == ODROID_DIALOG_ENTER;
}

static bool update_swap_ab_cb(odroid_dialog_choice_t *option,
                              odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int enabled = neo_settings_swap_ab_get();

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        enabled = !enabled;
        neo_settings_swap_ab_set(enabled);
    }

    strcpy(option->value, enabled ? "On" : "Off");
    return event == ODROID_DIALOG_ENTER;
}

static bool update_swap_cd_cb(odroid_dialog_choice_t *option,
                              odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int enabled = neo_settings_swap_cd_get();

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        enabled = !enabled;
        neo_settings_swap_cd_set(enabled);
    }

    strcpy(option->value, enabled ? "On" : "Off");
    return event == ODROID_DIALOG_ENTER;
}

void app_main_neogeo(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    odroid_gamepad_state_t joystick;

    gw_core_bridge_init();
    neo_apply_cpu_clock();
    memset(&pad, 0, sizeof(pad));

#ifndef HOST_BUILD
    /* Map cold .text/.rodata into QSPI and rebase CAFE pointers in RAM_EMU
     * before any call into neo_state / gno_flash / neocrypt / … */
    if (!neo_load_flash_cold()) {
        boot_fail_reason = "missing gngeo.ro";
        neo_fatal_quit("Neo Geo: missing gngeo.ro", "Copy /cores/gngeo.ro");
    }
#endif

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
                           NULL, &SleepWake, &SramSave, NULL);

    neo_hw_to_str(neo_settings_hw_get(), neo_opt_hw_str);
    neo_region_to_str(neo_settings_region_get(), neo_opt_region_str);
    strcpy(neo_opt_swap_ab_str, neo_settings_swap_ab_get() ? "On" : "Off");
    strcpy(neo_opt_swap_cd_str, neo_settings_swap_cd_get() ? "On" : "Off");

    /* Start SAI/DMA only after ROM/flash load. Starting earlier leaves the
     * DMA half unfilled for seconds (QSPI cache / zip inflate) and can leave
     * audio silently dead until the next cold boot. */
    if (!boot_game()) {
        /* User cancelled flash cache — back to launcher, no error screen. */
        if (boot_fail_reason && strcmp(boot_fail_reason, "Cancelled") == 0)
            odroid_system_switch_app(0);

        neo_fatal_quit("Neo Geo: boot failed",
                       boot_fail_reason ? boot_fail_reason : "unknown");
    }

    if (load_state)
        odroid_system_emu_load_state(save_slot);
    else
        lcd_clear_buffers();

    neo_audio_start();

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
        {
            odroid_dialog_choice_t options[] = {
                {100, "System", neo_opt_hw_str, 1, &update_hw_cb},
                {101, "Region", neo_opt_region_str, 1, &update_region_cb},
                {102, "Swap A/B", neo_opt_swap_ab_str, 1, &update_swap_ab_cb},
                {103, "Swap C/D", neo_opt_swap_cd_str, 1, &update_swap_cd_cb},
                ODROID_DIALOG_CHOICE_LAST
            };
            common_emu_input_loop(&joystick, options, &neo_repaint);
        }
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
