/*
 * Neo Geo savestate for Retro-Go SD.
 *
 * Firmware passes a full path; we stream a versioned blob with fopen/fwrite
 * (zlib.h aliases gz* → FILE* on device). Pointers into XIP / DTCM / AHB are
 * never serialized — only buffer contents and CPU/sound regs.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "state.h"
#include "memory.h"
#include "emu.h"
#include "video.h"
#include "gnutil.h"
#include "zlib.h"
#include "pd4990a.h"
#include "m68k/m68k.h"
#include "gno_flash.h"

#ifndef HOST_BUILD
#include "gw_lcd.h"
#include "gw_core_bridge.h"
#endif

/* "NEOS01" = Neo Geo Offline State v1 (this port, not classic GNGST). */
#define NEO_SS_MAGIC "NEOS01"
#define NEO_SS_MAGIC_LEN 6

SDL_Surface *state_img;
Uint8 state_version = ST_VER3;

extern Uint32 bankaddress;
extern struct pd4990a_s pd4990a;

void cpu_68k_mkstate(gzFile gzf, int mode);
void cpu_z80_mkstate(gzFile gzf, int mode);
void ym2610_mkstate(gzFile gzf, int mode);
void cpu_68k_bankswitch(Uint32 address);

int mkstate_data(gzFile gzf, void *data, int size, int mode)
{
    uint8_t *p = (uint8_t *)data;
    int left = size;
    /* FatFS (FF_FS_TINY) does multi-sector disk_write() straight from the
     * user pointer when btw >= 512 and fptr is sector-aligned. That DMA
     * cannot touch DTCM (Musashi regs) and under the pause-menu stack it
     * also blows the frame — Memfault then shows PC in a .neo_flash
     * literal pool (e.g. ym2610_mkstate+0x3a0) with LR at the mkstate_data
     * call site. Keep chunks strictly below one sector and bounce so the
     * SD path only ever sees this buffer. */
    enum { SS_CHUNK = 256 };
    static uint8_t bounce[SS_CHUNK];

    if (!gzf || !data || size <= 0)
        return 0;

    while (left > 0) {
        int chunk = left > SS_CHUNK ? SS_CHUNK : left;
        size_t n;
#ifndef HOST_BUILD
        wdog_refresh();
#endif
        if (mode == STREAD) {
            n = fread(bounce, 1, (size_t)chunk, gzf);
            if (n != (size_t)chunk)
                return 0;
            memcpy(p, bounce, (size_t)chunk);
        } else {
            memcpy(bounce, p, (size_t)chunk);
            n = fwrite(bounce, 1, (size_t)chunk, gzf);
            if (n != (size_t)chunk)
                return 0;
        }
        p += chunk;
        left -= chunk;
    }
    return size;
}

void neogeo_init_save_state(void)
{
    /* Preview .raw is written by firmware from Screenshot(); no state_img. */
}

static int rw_ok(gzFile f, void *p, int sz, int mode)
{
    return mkstate_data(f, p, sz, mode) == sz;
}

/* VIDEO scalars (not spr_cache FILE* / ptrs). */
static int neo_ss_video_regs(gzFile f, int mode)
{
    VIDEO *v = &memory.vid;
    return rw_ok(f, &v->currentpal, sizeof(v->currentpal), mode)
        && rw_ok(f, &v->currentfix, sizeof(v->currentfix), mode)
        && rw_ok(f, &v->rbuf, sizeof(v->rbuf), mode)
        && rw_ok(f, &v->fc, sizeof(v->fc), mode)
        && rw_ok(f, &v->fc_speed, sizeof(v->fc_speed), mode)
        && rw_ok(f, &v->vptr, sizeof(v->vptr), mode)
        && rw_ok(f, &v->modulo, sizeof(v->modulo), mode)
        && rw_ok(f, &v->current_line, sizeof(v->current_line), mode)
        && rw_ok(f, &v->irq2control, sizeof(v->irq2control), mode)
        && rw_ok(f, &v->irq2taken, sizeof(v->irq2taken), mode)
        && rw_ok(f, &v->irq2start, sizeof(v->irq2start), mode)
        && rw_ok(f, &v->irq2pos, sizeof(v->irq2pos), mode);
}

static int neo_ss_body(gzFile f, int mode)
{
    if (!memory.sram || !memory.memcard)
        return 0;

    if (!rw_ok(f, &bankaddress, sizeof(bankaddress), mode))
        return 0;
    if (!rw_ok(f, &sram_lock, sizeof(sram_lock), mode))
        return 0;
    if (!rw_ok(f, z80_bank, sizeof(z80_bank), mode))
        return 0;

    if (!rw_ok(f, memory.ram, 0x10000, mode))
        return 0;
    if (!rw_ok(f, memory.vid.ram, 0x20000, mode))
        return 0;
    if (!rw_ok(f, memory.vid.pal_neo, sizeof(memory.vid.pal_neo), mode))
        return 0;
    if (!rw_ok(f, memory.vid.pal_host, sizeof(memory.vid.pal_host), mode))
        return 0;
    if (!neo_ss_video_regs(f, mode))
        return 0;

    if (!rw_ok(f, memory.game_vector, 0x80, mode))
        return 0;
    if (!rw_ok(f, &memory.current_vector, sizeof(memory.current_vector), mode))
        return 0;
    if (!rw_ok(f, &memory.intern_p1, sizeof(memory.intern_p1), mode))
        return 0;
    if (!rw_ok(f, &memory.intern_p2, sizeof(memory.intern_p2), mode))
        return 0;
    if (!rw_ok(f, &memory.intern_coin, sizeof(memory.intern_coin), mode))
        return 0;
    if (!rw_ok(f, &memory.intern_start, sizeof(memory.intern_start), mode))
        return 0;
    if (!rw_ok(f, &memory.bksw_handler, sizeof(memory.bksw_handler), mode))
        return 0;
    if (!rw_ok(f, &memory.sma_rng_addr, sizeof(memory.sma_rng_addr), mode))
        return 0;
    if (!rw_ok(f, &memory.watchdog, sizeof(memory.watchdog), mode))
        return 0;

    if (!rw_ok(f, memory.sram, 0x10000, mode))
        return 0;
    if (!rw_ok(f, memory.memcard, 0x800, mode))
        return 0;

    if (!rw_ok(f, &sound_code, sizeof(sound_code), mode))
        return 0;
    if (!rw_ok(f, &pending_command, sizeof(pending_command), mode))
        return 0;
    if (!rw_ok(f, &result_code, sizeof(result_code), mode))
        return 0;

    if (!rw_ok(f, &pd4990a, sizeof(pd4990a), mode))
        return 0;

    /* CPU / sound engines (use existing mkstate helpers). */
    state_version = ST_VER3;
    cpu_68k_mkstate(f, mode);
    cpu_z80_mkstate(f, mode);
    ym2610_mkstate(f, mode);

    if (mode == STREAD) {
        cpu_68k_bankswitch(bankaddress);

        if (memory.current_vector == 0)
            neo_vector_use_bios();
        else
            neo_vector_use_game();

        if (memory.vid.currentpal) {
            current_pal = memory.vid.pal_neo[1];
            current_pc_pal = (Uint32 *)memory.vid.pal_host[1];
        } else {
            current_pal = memory.vid.pal_neo[0];
            current_pc_pal = (Uint32 *)memory.vid.pal_host[0];
        }
        if (memory.vid.currentfix) {
            current_fix = memory.rom.game_sfix.p;
            fix_usage = memory.fix_game_usage;
        } else {
            current_fix = memory.rom.bios_sfix.p;
            fix_usage = memory.fix_board_usage;
        }
    }
    return 1;
}

int neo_save_state_path(const char *path)
{
    FILE *f;
    if (!path || !path[0])
        return 0;
    f = fopen(path, "wb");
    if (!f) {
        printf("neogeo: save open fail %s\n", path);
        return 0;
    }
#ifndef HOST_BUILD
    wdog_refresh();
#endif
    if (fwrite(NEO_SS_MAGIC, 1, NEO_SS_MAGIC_LEN, f) != NEO_SS_MAGIC_LEN
        || !neo_ss_body(f, STWRITE)) {
        printf("neogeo: save write fail\n");
        fclose(f);
        return 0;
    }
    fclose(f);
    printf("neogeo: saved %s\n", path);
    return 1;
}

int neo_load_state_path(const char *path)
{
    FILE *f;
    char magic[NEO_SS_MAGIC_LEN];
    if (!path || !path[0])
        return 0;
    f = fopen(path, "rb");
    if (!f) {
        printf("neogeo: load open fail %s\n", path);
        return 0;
    }
#ifndef HOST_BUILD
    wdog_refresh();
#endif
    if (fread(magic, 1, NEO_SS_MAGIC_LEN, f) != NEO_SS_MAGIC_LEN
        || memcmp(magic, NEO_SS_MAGIC, NEO_SS_MAGIC_LEN) != 0) {
        printf("neogeo: bad savestate magic\n");
        fclose(f);
        return 0;
    }
    if (!neo_ss_body(f, STREAD)) {
        printf("neogeo: load read fail\n");
        fclose(f);
        return 0;
    }
    fclose(f);
    printf("neogeo: loaded %s\n", path);
    return 1;
}

/* Classic GnGeo API — unused by Retro-Go; keep symbols for link. */
int save_state(char *game, int slot)
{
    (void)game; (void)slot;
    return GN_FALSE;
}

int load_state(char *game, int slot)
{
    (void)game; (void)slot;
    return GN_FALSE;
}

SDL_Surface *load_state_img(char *game, int slot)
{
    (void)game; (void)slot;
    return NULL;
}

Uint32 how_many_slot(char *game)
{
    (void)game;
    return 0;
}
