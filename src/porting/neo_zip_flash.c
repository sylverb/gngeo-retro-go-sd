/*
 * MAME Neo Geo .zip → per-region QSPI cache (runtime-optimal layout).
 *
 * Keys: neogeo/<stem>/{p2,m,v,s,gfix,c,spr}
 * (p2 = program ROM after CONTINUE-vs-linear autodetection)
 * Scratch (inflate + converts) from ram_emu; ram_init() when done.
 * OSPI page alignment (256 B) coalesced here — no firmware change.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "roms.h"
#include "neo_zip_flash.h"
#include "gno_flash.h"
#include "transpack.h"
#include "neo_zip.h"
#include "gw_malloc.h"
#include "neo_flash_ro.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#include "odroid_overlay.h"
#include "gw_flash_alloc.h"
#include "gw_lcd.h"
#else
#include "gw_flash_alloc.h"
bool odroid_overlay_draw_progress_bar_cancellable(const char *header, uint8_t progress);
bool odroid_overlay_progress_poll_cancel(void);
void wdog_refresh(void);
#endif

/* Typical OSPI smallest erase — payloads are rounded up when programmed. */
#define NEO_FLASH_ERASE_ALIGN  4096u

/* Shared system blobs that must remain in the cache with any game
 * (UniBIOS + converted sfix + 000-lo). Loaded after the cart in bind, so
 * reserve them up-front when sizing a game. */
#define NEO_BIOS_FLASH_BYTES   (0x20000u + 0x20000u + 0x10000u)
static char zip_err[64];

const char *neo_zip_flash_last_error(void)
{
    return zip_err[0] ? zip_err : "unknown";
}

static void zip_set_err(const char *msg)
{
    size_t n = 0;
    if (!msg)
        msg = "error";
    while (msg[n] && n < sizeof(zip_err) - 1) {
        zip_err[n] = msg[n];
        n++;
    }
    zip_err[n] = 0;
}

/* ---- ROM_DEF from MAME zip names (unencrypted carts only) ------------
 *
 * No per-game table: classify members by *-p1 / *-c2 / *.s1 style names
 * (same idea as NeoBuilder / FBNeo loaders). Encrypted sets (SMA/PVC/CMC,
 * missing SFIX, odd C count, ep1, …) are rejected — those need a real
 * driver / decrypt path we do not ship.
 */

static ROM_DEF s_auto_drv;

typedef struct {
    char name[32];
    uint32_t size;
    uint32_t crc;
    int idx;
} neo_chip_t;

static int neo_islower_ch(unsigned char c)
{
    return (c >= 'a' && c <= 'z');
}

static int neo_isdigit_ch(unsigned char c)
{
    return (c >= '0' && c <= '9');
}

static char neo_lower_ch(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

/* Parse chip type + index from a MAME-ish basename.
 * Accepts: 239-p1.p1, 239-p2.sp2, 001-c1.bin, foo-s1.s1, mslug_v1.rom
 * Returns chip letter p/s/m/v/c, or 0 if not a cart ROM we understand. */
static char parse_neo_chip(const char *base, int *idx_out)
{
    char buf[40];
    size_t n = 0;
    const char *p;
    char chip = 0;
    int idx = 0;
    int i;

    if (!base || !idx_out)
        return 0;
    while (base[n] && n + 1 < sizeof(buf)) {
        buf[n] = neo_lower_ch(base[n]);
        n++;
    }
    buf[n] = 0;

    /* Prefer "sp2" / "sp1" as program bank 2/1 before generic 's'. */
    for (i = 0; buf[i]; i++) {
        if ((buf[i] == '-' || buf[i] == '.' || buf[i] == '_' || i == 0) &&
            buf[i + (i ? 1 : 0)] == 's' && buf[i + (i ? 2 : 1)] == 'p' &&
            neo_isdigit_ch((unsigned char)buf[i + (i ? 3 : 2)])) {
            int off = i ? i + 1 : 0;
            chip = 'p';
            idx = 0;
            p = buf + off + 2;
            while (neo_isdigit_ch((unsigned char)*p))
                idx = idx * 10 + (*p++ - '0');
            if (idx > 0) {
                *idx_out = idx;
                return chip;
            }
        }
    }

    /* Scan for -[psmvc]<digits> or .<psmvc><digits> (also _ and start). */
    for (i = 0; buf[i]; i++) {
        char c0;
        int off;
        if (!(buf[i] == '-' || buf[i] == '.' || buf[i] == '_' || i == 0))
            continue;
        off = (i == 0 && neo_islower_ch((unsigned char)buf[0]) &&
               neo_isdigit_ch((unsigned char)buf[1]))
                  ? 0
                  : i + 1;
        if (!buf[off])
            continue;
        c0 = buf[off];
        if (c0 != 'p' && c0 != 's' && c0 != 'm' && c0 != 'v' && c0 != 'c')
            continue;
        /* Encrypted program dumps: …-ep1 / …_ep2 */
        if (c0 == 'p' && off > 0 && buf[off - 1] == 'e')
            continue;
        if (!neo_isdigit_ch((unsigned char)buf[off + 1]))
            continue;
        /* Chip letter must be followed only by digits then end/dot/dash. */
        {
            int j = off + 1;
            int val = 0;
            while (neo_isdigit_ch((unsigned char)buf[j])) {
                val = val * 10 + (buf[j] - '0');
                j++;
            }
            if (val <= 0 || val > 64)
                continue;
            if (buf[j] != 0 && buf[j] != '.' && buf[j] != '-' &&
                buf[j] != '_')
                continue;
            chip = c0;
            idx = val;
            *idx_out = idx;
            return chip;
        }
    }
    return 0;
}

static void sort_chips(neo_chip_t *a, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (a[j].idx < a[i].idx) {
                neo_chip_t t = a[i];
                a[i] = a[j];
                a[j] = t;
            }
        }
    }
}

static int add_chip(neo_chip_t *arr, int *n, int max, const char *name,
                    uint32_t size, uint32_t crc, int idx)
{
    int i;
    if (*n >= max || !name || !name[0] || size == 0)
        return 0;
    for (i = 0; i < *n; i++) {
        if (arr[i].idx == idx)
            return 0; /* duplicate bank */
    }
    memset(&arr[*n], 0, sizeof(arr[*n]));
    {
        size_t len = strlen(name);
        if (len >= sizeof(arr[*n].name))
            len = sizeof(arr[*n].name) - 1;
        memcpy(arr[*n].name, name, len);
    }
    arr[*n].size = size;
    arr[*n].crc = crc;
    arr[*n].idx = idx;
    (*n)++;
    return 1;
}

/* LE byteswapped "NEO-GEO" at cart header $100 → already in memory order
 * (decrypt_neogeo_zip SWAPHALF / linear dumps). MAME CONTINUE dumps lack it. */
static int p2m_already_linear(neo_zip_t *z, const char *name, uint32_t crc)
{
    neo_zip_file_t *f;
    uint8_t buf[0x108];
    uint32_t got = 0;

    f = neo_zip_fopen(z, name, crc);
    if (!f)
        f = neo_zip_fopen(z, name, 0);
    if (!f)
        return 0;
    while (got < sizeof(buf)) {
        int r = neo_zip_fread(f, buf + got, (int)(sizeof(buf) - got));
        if (r <= 0)
            break;
        got += (uint32_t)r;
    }
    neo_zip_fclose(f);
    if (got < 0x106)
        return 0;
    return buf[0x100] == 0x45 && buf[0x101] == 0x4e &&
           buf[0x102] == 0x2d && buf[0x103] == 0x4f &&
           buf[0x104] == 0x45 && buf[0x105] == 0x47;
}

static int append_rom_files(ROM_DEF *drv, const neo_chip_t *arr, int n,
                            uint8_t region, uint32_t *region_size_out,
                            int p_already_linear)
{
    uint32_t off = 0;
    int i;

    /* Classic 2 MiB single P1: MAME loads file[0..1M) at $100000 and
     * file[1M..2M) at $000000 (ROM_CONTINUE). Hardware maps the second
     * mebibyte of the chip to $000000. Linear dump → white screen.
     * Skip when the zip already holds a memory-order image (SWAPHALF
     * decrypt output) — applying CONTINUE again double-swaps → white. */
    if (region == REGION_MAIN_CPU_CARTRIDGE && n == 1 &&
        arr[0].size == 0x200000u && !p_already_linear) {
        if (drv->nb_romfile + 2 > 32)
            return 0;
        for (i = 0; i < 2; i++) {
            uint32_t src = (i == 0) ? 0x100000u : 0u;
            uint32_t dest = (i == 0) ? 0u : 0x100000u;
            memset(&drv->rom[drv->nb_romfile], 0, sizeof(drv->rom[0]));
            {
                size_t len = strlen(arr[0].name);
                if (len >= 32)
                    len = 31;
                memcpy(drv->rom[drv->nb_romfile].filename, arr[0].name, len);
            }
            drv->rom[drv->nb_romfile].region = region;
            drv->rom[drv->nb_romfile].src = src;
            drv->rom[drv->nb_romfile].dest = dest;
            drv->rom[drv->nb_romfile].size = 0x100000u;
            drv->rom[drv->nb_romfile].crc = arr[0].crc;
            drv->nb_romfile++;
        }
        if (region_size_out)
            *region_size_out = 0x200000u;
        return 1;
    }

    for (i = 0; i < n; i++) {
        if (drv->nb_romfile >= 32)
            return 0;
        memset(&drv->rom[drv->nb_romfile], 0, sizeof(drv->rom[0]));
        {
            size_t len = strlen(arr[i].name);
            if (len >= 32)
                len = 31;
            memcpy(drv->rom[drv->nb_romfile].filename, arr[i].name, len);
        }
        drv->rom[drv->nb_romfile].region = region;
        drv->rom[drv->nb_romfile].src = 0;
        drv->rom[drv->nb_romfile].dest = off;
        drv->rom[drv->nb_romfile].size = arr[i].size;
        drv->rom[drv->nb_romfile].crc = arr[i].crc;
        drv->nb_romfile++;
        off += arr[i].size;
    }
    if (region_size_out)
        *region_size_out = off;
    return 1;
}

static int append_crom_pairs(ROM_DEF *drv, const neo_chip_t *c, int nc,
                             uint32_t *tiles_out)
{
    uint32_t off = 0;
    int i;
    if (nc < 2 || (nc & 1))
        return 0;
    for (i = 0; i < nc; i += 2) {
        uint32_t sz = c[i].size;
        if (c[i + 1].size != sz)
            return 0;
        if (drv->nb_romfile + 2 > 32)
            return 0;
        /* even plane */
        memset(&drv->rom[drv->nb_romfile], 0, sizeof(drv->rom[0]));
        {
            size_t len = strlen(c[i].name);
            if (len >= 32)
                len = 31;
            memcpy(drv->rom[drv->nb_romfile].filename, c[i].name, len);
        }
        drv->rom[drv->nb_romfile].region = REGION_SPRITES;
        drv->rom[drv->nb_romfile].dest = off;
        drv->rom[drv->nb_romfile].size = sz;
        drv->rom[drv->nb_romfile].crc = c[i].crc;
        drv->nb_romfile++;
        /* odd plane */
        memset(&drv->rom[drv->nb_romfile], 0, sizeof(drv->rom[0]));
        {
            size_t len = strlen(c[i + 1].name);
            if (len >= 32)
                len = 31;
            memcpy(drv->rom[drv->nb_romfile].filename, c[i + 1].name, len);
        }
        drv->rom[drv->nb_romfile].region = REGION_SPRITES;
        drv->rom[drv->nb_romfile].dest = off + 1;
        drv->rom[drv->nb_romfile].size = sz;
        drv->rom[drv->nb_romfile].crc = c[i + 1].crc;
        drv->nb_romfile++;
        off += sz * 2u;
    }
    if (tiles_out)
        *tiles_out = off;
    return 1;
}

/* Build s_auto_drv from zip CD. Returns pointer or NULL + zip_err. */
static const ROM_DEF *rom_def_from_zip(neo_zip_t *z, const char *stem)
{
    neo_chip_t p[8], s[2], m[2], v[16], c[32];
    int np = 0, ns = 0, nm = 0, nv = 0, nc = 0;
    int nfiles, fi;
    uint32_t psz = 0, msz = 0, vsz = 0, ssz = 0, csz = 0;

    memset(&s_auto_drv, 0, sizeof(s_auto_drv));
    {
        size_t len = stem ? strlen(stem) : 0;
        if (len >= sizeof(s_auto_drv.name))
            len = sizeof(s_auto_drv.name) - 1;
        if (stem && len)
            memcpy(s_auto_drv.name, stem, len);
    }
    memcpy(s_auto_drv.parent, "neogeo", 6);
    {
        size_t len = stem ? strlen(stem) : 0;
        if (len >= sizeof(s_auto_drv.longname))
            len = sizeof(s_auto_drv.longname) - 1;
        if (stem && len)
            memcpy(s_auto_drv.longname, stem, len);
    }

    nfiles = neo_zip_num_files(z);
    for (fi = 0; fi < nfiles; fi++) {
        char name[32];
        uint32_t sz = 0, crc = 0;
        char chip;
        int idx = 0;
        if (!neo_zip_stat(z, fi, name, sizeof(name), &sz, &crc))
            continue;
        chip = parse_neo_chip(name, &idx);
        if (!chip)
            continue;
        switch (chip) {
        case 'p':
            add_chip(p, &np, 8, name, sz, crc, idx);
            break;
        case 's':
            add_chip(s, &ns, 2, name, sz, crc, idx);
            break;
        case 'm':
            add_chip(m, &nm, 2, name, sz, crc, idx);
            break;
        case 'v':
            add_chip(v, &nv, 16, name, sz, crc, idx);
            break;
        case 'c':
            add_chip(c, &nc, 32, name, sz, crc, idx);
            break;
        default:
            break;
        }
    }

    if (np < 1 || nm < 1 || ns < 1 || nv < 1 || nc < 2) {
        zip_set_err("zip missing p/m/s/v/c");
        printf("neo_zip: auto-detect fail p=%d m=%d s=%d v=%d c=%d\n",
               np, nm, ns, nv, nc);
        return NULL;
    }
    if (nc & 1) {
        zip_set_err("odd CROM count");
        return NULL;
    }

    sort_chips(p, np);
    sort_chips(s, ns);
    sort_chips(m, nm);
    sort_chips(v, nv);
    sort_chips(c, nc);

    /* Expect C indices 1,2,3,4… pairing (1,2),(3,4),… */
    {
        int i;
        for (i = 0; i < nc; i++) {
            if (c[i].idx != i + 1) {
                zip_set_err("CROM index gap");
                printf("neo_zip: CROM idx[%d]=%d expected %d\n",
                       i, c[i].idx, i + 1);
                return NULL;
            }
        }
    }

    {
        int p_linear = 0;
        if (np == 1 && p[0].size == 0x200000u) {
            p_linear = p2m_already_linear(z, p[0].name, p[0].crc);
            if (p_linear)
                printf("neo_zip: 2MiB P already linear (skip CONTINUE)\n");
            else
                printf("neo_zip: 2MiB P CONTINUE half-swap\n");
        }
        if (!append_rom_files(&s_auto_drv, p, np, REGION_MAIN_CPU_CARTRIDGE,
                              &psz, p_linear) ||
            !append_rom_files(&s_auto_drv, s, ns, REGION_FIXED_LAYER_CARTRIDGE,
                             &ssz, 0) ||
            !append_rom_files(&s_auto_drv, m, nm, REGION_AUDIO_CPU_CARTRIDGE,
                             &msz, 0) ||
            !append_rom_files(&s_auto_drv, v, nv, REGION_AUDIO_DATA_1,
                             &vsz, 0) ||
            !append_crom_pairs(&s_auto_drv, c, nc, &csz)) {
            zip_set_err("ROM_DEF overflow");
            return NULL;
        }
    }

    s_auto_drv.romsize[REGION_MAIN_CPU_CARTRIDGE] = psz;
    s_auto_drv.romsize[REGION_FIXED_LAYER_CARTRIDGE] = ssz;
    s_auto_drv.romsize[REGION_AUDIO_CPU_CARTRIDGE] = msz;
    s_auto_drv.romsize[REGION_AUDIO_DATA_1] = vsz;
    s_auto_drv.romsize[REGION_SPRITES] = csz;

    printf("neo_zip: auto '%s' p=%u m=%u s=%u v=%u c=%u files=%u\n",
           s_auto_drv.name, (unsigned)psz, (unsigned)msz, (unsigned)ssz,
           (unsigned)vsz, (unsigned)csz, (unsigned)s_auto_drv.nb_romfile);
    return &s_auto_drv;
}

static void path_stem(const char *path, char *out, size_t out_sz)
{
    const char *base = path;
    const char *slash = strrchr(path, '/');
    const char *dot;
    size_t n;
    if (slash)
        base = slash + 1;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= out_sz)
        n = out_sz - 1;
    memcpy(out, base, n);
    out[n] = 0;
}

static void make_key(char *out, size_t out_sz, const char *stem, const char *suf)
{
    snprintf(out, out_sz, "neogeo/%s/%s", stem, suf);
}

/* ---- converts -------------------------------------------------------- */

/* Convert one 128 B tile in-place (always *tile, not an indexed ROM).
 * tileno is only for packing TILE_INVISIBLE into the right 2-bit slot of
 * the 16-tile spr_usage word — same as GnGeo convert_roms_tile(..., i). */
static uint32_t convert_one_tile(uint8_t *tile, int tileno)
{
    unsigned char swap[128];
    unsigned int *gfxdata;
    int x, y;
    unsigned int pen, usage = 0;
    gfxdata = (unsigned int *)tile;
    memcpy(swap, tile, 128);
    for (y = 0; y < 16; y++) {
        unsigned int dw = 0;
        for (x = 0; x < 8; x++) {
            pen = ((swap[64 + (y << 2) + 3] >> x) & 1) << 3;
            pen |= ((swap[64 + (y << 2) + 1] >> x) & 1) << 2;
            pen |= ((swap[64 + (y << 2) + 2] >> x) & 1) << 1;
            pen |= (swap[64 + (y << 2)] >> x) & 1;
            dw |= pen << ((7 - x) << 2);
            usage |= (1 << pen);
        }
        *(gfxdata++) = dw;
        dw = 0;
        for (x = 0; x < 8; x++) {
            pen = ((swap[(y << 2) + 3] >> x) & 1) << 3;
            pen |= ((swap[(y << 2) + 1] >> x) & 1) << 2;
            pen |= ((swap[(y << 2) + 2] >> x) & 1) << 1;
            pen |= (swap[(y << 2)] >> x) & 1;
            dw |= pen << ((7 - x) << 2);
            usage |= (1 << pen);
        }
        *(gfxdata++) = dw;
    }
    if ((usage & ~1u) == 0)
        return (uint32_t)(TILE_INVISIBLE << ((tileno & 0xF) * 2));
    return 0;
}

static void convert_sfix_inplace(uint8_t *Ptr, int Taille, uint8_t *usage_ptr)
{
    int i, j;
    unsigned char usage;
    uint8_t tile[32];

    if (!Ptr || Taille <= 0)
        return;
    for (i = 0; i < Taille; i += 32) {
        uint8_t *dst = Ptr + i;
        const uint8_t *Src = tile;
        memcpy(tile, dst, 32);
        usage = 0;
        for (j = 0; j < 8; j++) {
            *dst = *(Src + 16); usage |= *dst++;
            *dst = *(Src + 24); usage |= *dst++;
            *dst = *(Src);      usage |= *dst++;
            *dst = *(Src + 8);  usage |= *dst++;
            Src++;
        }
        if (usage_ptr)
            *usage_ptr++ = usage;
    }
}

/* ---- progress + 256 B flash coalesce --------------------------------- */

static char s_prog_hdr[36];
static uint32_t s_prog_done;
static uint32_t s_prog_total;
static uint32_t s_prog_since_paint;
static uint8_t s_prog_last_pct;
static uint8_t s_prog_step;   /* 1..s_prog_steps while caching */
static uint8_t s_prog_steps;
static uint8_t s_page[256];
static uint32_t s_page_used;

/* Progress tracks ZIP bytes extracted (+ small derived gfix/spr writes),
 * not flash coalesce chunks. Counting only flash made the bar race to 99%
 * on P/M/V then freeze for the whole CROM inflate/convert. */

static int prog_paint(uint8_t pct, int wait_swap)
{
#ifndef HOST_BUILD
    if (wait_swap)
        lcd_sleep_while_swap_pending();
    else if (lcd_is_swap_pending()) {
        /* Leave last_pct stale so the next bump retries. */
        wdog_refresh();
        return 1;
    }
#endif
    if (!odroid_overlay_draw_progress_bar_cancellable(
            s_prog_hdr[0] ? s_prog_hdr : "Neo Geo", pct)) {
        zip_set_err("Cancelled");
        return 0;
    }
#ifndef HOST_BUILD
    lcd_swap();
#endif
    s_prog_last_pct = pct;
    s_prog_since_paint = 0;
    wdog_refresh();
    return 1;
}

static uint8_t prog_pct(void)
{
    if (!s_prog_total)
        return 0;
    if (s_prog_done >= s_prog_total)
        return 99;
    return (uint8_t)((uint64_t)s_prog_done * 99u / (uint64_t)s_prog_total);
}

static int prog_redraw(int force)
{
    uint8_t pct = prog_pct();

    if (!force && pct == s_prog_last_pct) {
        wdog_refresh();
        return 1;
    }
    return prog_paint(pct, force);
}

/* Title: "3/14 239-c1.c1" (fits the overlay header ~30 chars). */
static int prog_set_file(const char *name)
{
    const char *base = name ? strrchr(name, '/') : NULL;
    char shortn[24];
    size_t n;

    base = base ? base + 1 : (name && name[0] ? name : "?");
    n = strlen(base);
    if (n >= sizeof(shortn))
        n = sizeof(shortn) - 1;
    memcpy(shortn, base, n);
    shortn[n] = 0;

    if (s_prog_steps)
        snprintf(s_prog_hdr, sizeof(s_prog_hdr), "%u/%u %s",
                 (unsigned)s_prog_step, (unsigned)s_prog_steps, shortn);
    else
        snprintf(s_prog_hdr, sizeof(s_prog_hdr), "%s", shortn);
    s_prog_last_pct = 0xff;
    return prog_redraw(1);
}

static int prog_next_file(const char *name)
{
    if (s_prog_step < 255)
        s_prog_step++;
    return prog_set_file(name);
}

static int prog_bump(uint32_t n)
{
    if (!n)
        return 1;
    s_prog_done += n;
    if (s_prog_done > s_prog_total && s_prog_total)
        s_prog_done = s_prog_total;
    s_prog_since_paint += n;
    /* Poll cancel even when we skip a repaint — a short B tap must not be
     * missed between 64 KiB paint intervals. */
#ifndef HOST_BUILD
    if (odroid_overlay_progress_poll_cancel()) {
        zip_set_err("Cancelled");
        return 0;
    }
#endif
    /* Repaint on % change, or every 64 KiB so long CROM inflates move. */
    return prog_redraw(s_prog_since_paint >= (64u * 1024u));
}

static void prog_finish(void)
{
    if (s_prog_total)
        s_prog_done = s_prog_total;
    s_prog_last_pct = 0xff;
    (void)prog_paint(100, 1);
}

static int key_hit(const char *stem, const char *suf, uint32_t expect)
{
    char key[48];
    uint32_t got = 0;
    const uint8_t *hit;

    make_key(key, sizeof(key), stem, suf);
    hit = lookup_data_in_flash(key, &got);
    return (hit && got == expect) ? 1 : 0;
}

static int count_rom_files(const ROM_DEF *drv, uint8_t region)
{
    int i, n = 0;
    for (i = 0; i < (int)drv->nb_romfile; i++) {
        if (drv->rom[i].region == region)
            n++;
    }
    return n;
}

/* Full cart weight as total; already-cached regions start as done so the
 * bar only advances over remaining ZIP extract / derived writes. */
static void prog_plan(const ROM_DEF *drv, const char *stem)
{
    uint32_t p = drv->romsize[REGION_MAIN_CPU_CARTRIDGE];
    uint32_t m = drv->romsize[REGION_AUDIO_CPU_CARTRIDGE];
    uint32_t v = drv->romsize[REGION_AUDIO_DATA_1];
    uint32_t tiles = drv->romsize[REGION_SPRITES];
    uint32_t sfix = drv->romsize[REGION_FIXED_LAYER_CARTRIDGE];
    uint32_t gfix = sfix >> 5;
    uint32_t spr = (tiles >> 11) * 4u;
    int hit_p = key_hit(stem, "p2", p);
    int hit_m = key_hit(stem, "m", m);
    int hit_v = key_hit(stem, "v", v);
    int hit_s = key_hit(stem, "s", sfix) && key_hit(stem, "gfix", gfix);
    int hit_c = key_hit(stem, "c", tiles) && key_hit(stem, "spr", spr);

    s_prog_done = 0;
    s_prog_total = p + m + v + sfix + gfix + tiles + spr;
    s_prog_since_paint = 0;
    s_prog_steps = 0;
    s_prog_step = 0;
    s_prog_last_pct = 0xff;
    s_prog_hdr[0] = 0;

    if (hit_p)
        s_prog_done += p;
    else
        s_prog_steps = (uint8_t)(s_prog_steps +
            count_rom_files(drv, REGION_MAIN_CPU_CARTRIDGE));

    if (hit_m)
        s_prog_done += m;
    else
        s_prog_steps = (uint8_t)(s_prog_steps +
            count_rom_files(drv, REGION_AUDIO_CPU_CARTRIDGE));

    if (hit_v)
        s_prog_done += v;
    else
        s_prog_steps = (uint8_t)(s_prog_steps +
            count_rom_files(drv, REGION_AUDIO_DATA_1));

    if (hit_s)
        s_prog_done += sfix + gfix;
    else
        s_prog_steps = (uint8_t)(s_prog_steps + 2); /* sfix member + gfix */

    if (hit_c)
        s_prog_done += tiles + spr;
    else {
        /* One step per CROM plane (even+odd) + spr_usage. */
        s_prog_steps = (uint8_t)(s_prog_steps +
            count_rom_files(drv, REGION_SPRITES) + 1);
    }

    if (s_prog_done > s_prog_total)
        s_prog_done = s_prog_total;
}

static void page_reset(void)
{
    s_page_used = 0;
}

static int page_flush(flash_stream_t *st)
{
    if (!s_page_used)
        return 1;
    if (!store_data_append(st, s_page, s_page_used))
        return 0;
    s_page_used = 0;
    return 1;
}

static int append_bytes(flash_stream_t *st, const void *p, uint32_t n)
{
    const uint8_t *src = (const uint8_t *)p;

    while (n) {
        if (s_page_used) {
            uint32_t space = 256u - s_page_used;
            uint32_t take = n < space ? n : space;
            memcpy(s_page + s_page_used, src, take);
            s_page_used += take;
            src += take;
            n -= take;
            if (s_page_used == 256u) {
                if (!store_data_append(st, s_page, 256u))
                    return 0;
                s_page_used = 0;
            }
        } else {
            uint32_t aligned = n & ~255u;
            if (aligned >= 256u) {
                if (!store_data_append(st, src, aligned))
                    return 0;
                src += aligned;
                n -= aligned;
            }
            if (n) {
                memcpy(s_page, src, n);
                s_page_used = n;
                src += n;
                n = 0;
            }
        }
    }
    return 1;
}

static int stream_begin(flash_stream_t *st, const char *key, uint32_t total)
{
    page_reset();
    return store_data_begin(st, key, total) ? 1 : 0;
}

#ifndef HOST_BUILD
static uint32_t flash_align_erase(uint32_t n)
{
    return (n + (NEO_FLASH_ERASE_ALIGN - 1u)) & ~(NEO_FLASH_ERASE_ALIGN - 1u);
}

/* Max bytes a Neo Geo cart may occupy in the QSPI cache:
 *   flash_cache_usable_size()  — chip size minus OFW/layout reserve (64/63/60 MiB)
 *   minus erase-aligned BIOS + neogeo.ro
 * Returns 0 if the ABI is missing (old firmware) — caller should not refuse. */
static uint32_t neo_flash_max_game_bytes(void)
{
    uint32_t usable = flash_cache_usable_size();
    uint32_t reserved;

    if (usable == 0)
        return 0;

    reserved = flash_align_erase(NEO_BIOS_FLASH_BYTES);
    reserved += flash_align_erase(neo_flash_ro_size());
    if (reserved >= usable)
        return 0;
    return usable - reserved;
}

/* Erase-aligned weight of one cart (each region is its own flash blob). */
static uint32_t neo_flash_game_weight(const ROM_DEF *drv)
{
    uint32_t p = drv->romsize[REGION_MAIN_CPU_CARTRIDGE];
    uint32_t m = drv->romsize[REGION_AUDIO_CPU_CARTRIDGE];
    uint32_t v = drv->romsize[REGION_AUDIO_DATA_1];
    uint32_t tiles = drv->romsize[REGION_SPRITES];
    uint32_t sfix = drv->romsize[REGION_FIXED_LAYER_CARTRIDGE];
    uint32_t gfix = sfix >> 5;
    uint32_t spr = (tiles >> 11) * 4u;

    return flash_align_erase(p) + flash_align_erase(m) + flash_align_erase(v) +
           flash_align_erase(sfix) + flash_align_erase(gfix) +
           flash_align_erase(tiles) + flash_align_erase(spr);
}
#endif /* !HOST_BUILD */

static const uint8_t *stream_finish(flash_stream_t *st)
{
    if (!page_flush(st)) {
        store_data_abort(st);
        return NULL;
    }
    return store_data_finish(st);
}

static void stream_abort(flash_stream_t *st)
{
    page_reset();
    store_data_abort(st);
}

static neo_zip_file_t *zip_open_member(neo_zip_t *z, const char *name, uint32_t crc)
{
    neo_zip_file_t *f = neo_zip_fopen(z, name, crc);
    if (!f)
        f = neo_zip_fopen(z, name, 0);
    return f;
}

static int stream_members_raw(neo_zip_t *z, const ROM_DEF *drv, uint8_t region,
                              flash_stream_t *st)
{
    int i;
    uint8_t buf[4096];
    for (i = 0; i < (int)drv->nb_romfile; i++) {
        neo_zip_file_t *f;
        uint32_t skip;
        uint32_t left;
        if (drv->rom[i].region != region)
            continue;
        if (!prog_next_file(drv->rom[i].filename)) {
            return 0;
        }
        f = zip_open_member(z, drv->rom[i].filename, drv->rom[i].crc);
        if (!f) {
            zip_set_err("zip member missing");
            return 0;
        }
        /* Honor src (byte offset into the zip member) — used by the 2 MiB
         * P1 CONTINUE split so we can emit $000000 before $100000. */
        skip = drv->rom[i].src;
        while (skip) {
            int n = (int)(skip > sizeof(buf) ? sizeof(buf) : skip);
            int r = neo_zip_fread(f, buf, n);
            if (r <= 0) {
                neo_zip_fclose(f);
                zip_set_err("zip member short skip");
                return 0;
            }
            skip -= (uint32_t)r;
        }
        left = drv->rom[i].size;
        while (left) {
            int n = (int)(left > sizeof(buf) ? sizeof(buf) : left);
            int r = neo_zip_fread(f, buf, n);
            if (r <= 0) {
                neo_zip_fclose(f);
                zip_set_err("zip member short");
                return 0;
            }
            if (!append_bytes(st, buf, (uint32_t)r)) {
                neo_zip_fclose(f);
                zip_set_err("flash append failed");
                return 0;
            }
            if (!prog_bump((uint32_t)r)) {
                neo_zip_fclose(f);
                return 0;
            }
            left -= (uint32_t)r;
        }
        neo_zip_fclose(f);
    }
    return 1;
}

static int ensure_raw_region(neo_zip_t *z, const ROM_DEF *drv, const char *stem,
                             const char *suf, uint8_t region, uint32_t expect,
                             const uint8_t **out_ptr, uint32_t *out_sz)
{
    char key[48];
    uint32_t got = 0;
    const uint8_t *hit;
    flash_stream_t st;

    make_key(key, sizeof(key), stem, suf);
    hit = lookup_data_in_flash(key, &got);
    if (hit && got == expect) {
        *out_ptr = hit;
        *out_sz = got;
        return 1;
    }
    memset(&st, 0, sizeof(st));
    if (!stream_begin(&st, key, expect)) {
        zip_set_err("ROM too large for flash");
        return 0;
    }
    if (!stream_members_raw(z, drv, region, &st)) {
        stream_abort(&st);
        return 0;
    }
    hit = stream_finish(&st);
    if (!hit) {
        zip_set_err("flash finish failed");
        return 0;
    }
    printf("neo_zip: cached %s (%u)\n", key, (unsigned)expect);
    *out_ptr = hit;
    *out_sz = expect;
    return 1;
}

static int ensure_sfix(neo_zip_t *z, const ROM_DEF *drv, const char *stem,
                        const uint8_t **s_out, uint32_t *s_sz,
                        const uint8_t **g_out, uint32_t *g_sz)
{
    char key_s[48], key_g[48];
    uint32_t sfix_sz = drv->romsize[REGION_FIXED_LAYER_CARTRIDGE];
    uint32_t gfix_sz = sfix_sz >> 5;
    uint32_t got_s = 0, got_g = 0;
    const uint8_t *hit_s, *hit_g;
    uint8_t *gfix_usage;
    flash_stream_t st;
    neo_zip_file_t *fs = NULL;
    const char *sname = NULL;
    uint32_t scrc = 0;
    int i;

    make_key(key_s, sizeof(key_s), stem, "s");
    make_key(key_g, sizeof(key_g), stem, "gfix");
    hit_s = lookup_data_in_flash(key_s, &got_s);
    hit_g = lookup_data_in_flash(key_g, &got_g);
    if (hit_s && got_s == sfix_sz && hit_g && got_g == gfix_sz) {
        *s_out = hit_s;
        *s_sz = got_s;
        *g_out = hit_g;
        *g_sz = got_g;
        return 1;
    }

    for (i = 0; i < (int)drv->nb_romfile; i++) {
        if (drv->rom[i].region == REGION_FIXED_LAYER_CARTRIDGE) {
            sname = drv->rom[i].filename;
            scrc = drv->rom[i].crc;
            break;
        }
    }
    if (!sname) {
        zip_set_err("no sfix in def");
        return 0;
    }

    if (!prog_next_file(sname))
        return 0;

    /* Convert is 32 B/tile — stream zip→flash so 512 KiB S1 (kof2000/2003)
     * never needs a full RAM staging buffer on top of inflate scratch. */
    gfix_usage = (uint8_t *)ram_calloc(1, gfix_sz);
    if (!gfix_usage) {
        zip_set_err("OOM staging gfix");
        return 0;
    }

    fs = zip_open_member(z, sname, scrc);
    if (!fs) {
        zip_set_err("sfix open failed");
        return 0;
    }
    memset(&st, 0, sizeof(st));
    if (!stream_begin(&st, key_s, sfix_sz)) {
        neo_zip_fclose(fs);
        stream_abort(&st);
        zip_set_err("ROM too large for flash");
        return 0;
    }
    {
        uint32_t left = sfix_sz;
        uint32_t gi = 0;
        uint8_t tile[32];

        while (left) {
            uint32_t chunk = left > sizeof(tile) ? (uint32_t)sizeof(tile) : left;
            int r = neo_zip_fread(fs, tile, (int)chunk);
            if (r != (int)chunk) {
                neo_zip_fclose(fs);
                stream_abort(&st);
                zip_set_err("sfix read failed");
                return 0;
            }
            if (!prog_bump(chunk)) {
                neo_zip_fclose(fs);
                stream_abort(&st);
                return 0;
            }
            if (chunk == 32) {
                convert_sfix_inplace(tile, 32,
                                      gfix_usage ? gfix_usage + gi : NULL);
                gi++;
            }
            if (!append_bytes(&st, tile, chunk)) {
                neo_zip_fclose(fs);
                stream_abort(&st);
                zip_set_err("ROM too large for flash");
                return 0;
            }
            left -= chunk;
        }
    }
    neo_zip_fclose(fs);
    hit_s = stream_finish(&st);
    if (!hit_s) {
        zip_set_err("sfix finish failed");
        return 0;
    }

    if (!prog_next_file("gfix"))
        return 0;
    memset(&st, 0, sizeof(st));
    if (!stream_begin(&st, key_g, gfix_sz) ||
        !append_bytes(&st, gfix_usage, gfix_sz)) {
        stream_abort(&st);
        zip_set_err("ROM too large for flash");
        return 0;
    }
    hit_g = stream_finish(&st);
    if (!hit_g) {
        zip_set_err("gfix finish failed");
        return 0;
    }
    if (!prog_bump(gfix_sz))
        return 0;
    printf("neo_zip: cached %s + %s\n", key_s, key_g);
    *s_out = hit_s;
    *s_sz = sfix_sz;
    *g_out = hit_g;
    *g_sz = gfix_sz;
    return 1;
}

static int stream_crom_pair(neo_zip_t *z, const char *even_name, uint32_t even_crc,
                            const char *odd_name, uint32_t odd_crc,
                            uint32_t file_size, uint32_t *spr_usage,
                            uint32_t tile_base_index, flash_stream_t *st)
{
    neo_zip_file_t *fe, *fo;
    uint8_t even[64], odd[64], tile[128];
    uint32_t left = file_size;
    uint32_t tileno = tile_base_index;
    uint32_t odd_title_at;

    fe = zip_open_member(z, even_name, even_crc);
    fo = zip_open_member(z, odd_name, odd_crc);
    if (!fe || !fo) {
        if (fe) neo_zip_fclose(fe);
        if (fo) neo_zip_fclose(fo);
        zip_set_err("CROM pair missing");
        return 0;
    }

    /* Show even name first; switch to odd halfway so both filenames appear. */
    if (!prog_next_file(even_name)) {
        neo_zip_fclose(fe);
        neo_zip_fclose(fo);
        return 0;
    }
    odd_title_at = file_size / 2u;

    while (left) {
        uint32_t chunk = left > 64 ? 64 : left;
        uint32_t done_before = file_size - left;
        int re, ro, i;
        memset(even, 0, sizeof(even));
        memset(odd, 0, sizeof(odd));
        re = neo_zip_fread(fe, even, (int)chunk);
        ro = neo_zip_fread(fo, odd, (int)chunk);
        if (re != (int)chunk || ro != (int)chunk) {
            neo_zip_fclose(fe);
            neo_zip_fclose(fo);
            zip_set_err("CROM short read");
            return 0;
        }

        if (done_before < odd_title_at &&
            done_before + chunk >= odd_title_at) {
            if (!prog_next_file(odd_name)) {
                neo_zip_fclose(fe);
                neo_zip_fclose(fo);
                return 0;
            }
        }

        for (i = 0; i < (int)chunk; i++) {
            tile[i * 2] = even[i];
            tile[i * 2 + 1] = odd[i];
        }
        if (chunk < 64)
            memset(tile + chunk * 2, 0, 128 - chunk * 2);
        {
            uint32_t u = convert_one_tile(tile, (int)tileno);
            spr_usage[tileno >> 4] |= u;
        }
        if (!append_bytes(st, tile, 128)) {
            neo_zip_fclose(fe);
            neo_zip_fclose(fo);
            zip_set_err("tile append failed");
            return 0;
        }
        /* After convert+flash so the bar tracks real CROM work, not just inflate. */
        if (!prog_bump((uint32_t)chunk * 2u)) {
            neo_zip_fclose(fe);
            neo_zip_fclose(fo);
            return 0;
        }
        tileno++;
        left -= chunk;
    }
    neo_zip_fclose(fe);
    neo_zip_fclose(fo);
    return 1;
}

static int ensure_tiles(neo_zip_t *z, const ROM_DEF *drv, const char *stem,
                        const uint8_t **c_out, uint32_t *c_sz,
                        const uint8_t **spr_out, uint32_t *spr_sz)
{
    char key_c[48], key_spr[48];
    uint32_t tiles = drv->romsize[REGION_SPRITES];
    uint32_t spr_usage_sz = (tiles >> 11) * 4u;
    uint32_t got_c = 0, got_spr = 0;
    const uint8_t *hit_c, *hit_spr;
    uint32_t *spr_usage;
    flash_stream_t st;
    int i;

    typedef struct {
        const char *even, *odd;
        uint32_t ecrc, ocrc, size, dest;
    } crom_pair_t;
    crom_pair_t pairs[8];
    int np = 0;

    make_key(key_c, sizeof(key_c), stem, "c");
    make_key(key_spr, sizeof(key_spr), stem, "spr");
    hit_c = lookup_data_in_flash(key_c, &got_c);
    hit_spr = lookup_data_in_flash(key_spr, &got_spr);
    if (hit_c && got_c == tiles && hit_spr && got_spr == spr_usage_sz) {
        *c_out = hit_c;
        *c_sz = got_c;
        *spr_out = hit_spr;
        *spr_sz = got_spr;
        return 1;
    }

    for (i = 0; i < (int)drv->nb_romfile; i++) {
        if (drv->rom[i].region != REGION_SPRITES)
            continue;
        if ((drv->rom[i].dest & 1u) == 0) {
            int j, found = 0;
            for (j = 0; j < np; j++) {
                if (pairs[j].dest == (drv->rom[i].dest & ~1u)) {
                    pairs[j].even = drv->rom[i].filename;
                    pairs[j].ecrc = drv->rom[i].crc;
                    pairs[j].size = drv->rom[i].size;
                    found = 1;
                    break;
                }
            }
            if (!found && np < 8) {
                pairs[np].even = drv->rom[i].filename;
                pairs[np].ecrc = drv->rom[i].crc;
                pairs[np].odd = NULL;
                pairs[np].ocrc = 0;
                pairs[np].size = drv->rom[i].size;
                pairs[np].dest = drv->rom[i].dest & ~1u;
                np++;
            }
        } else {
            int j, found = 0;
            uint32_t base = drv->rom[i].dest & ~1u;
            for (j = 0; j < np; j++) {
                if (pairs[j].dest == base) {
                    pairs[j].odd = drv->rom[i].filename;
                    pairs[j].ocrc = drv->rom[i].crc;
                    found = 1;
                    break;
                }
            }
            if (!found && np < 8) {
                pairs[np].even = NULL;
                pairs[np].odd = drv->rom[i].filename;
                pairs[np].ocrc = drv->rom[i].crc;
                pairs[np].ecrc = 0;
                pairs[np].size = drv->rom[i].size;
                pairs[np].dest = base;
                np++;
            }
        }
    }
    {
        int a, b;
        for (a = 0; a < np; a++) {
            for (b = a + 1; b < np; b++) {
                if (pairs[b].dest < pairs[a].dest) {
                    crom_pair_t tmp = pairs[a];
                    pairs[a] = pairs[b];
                    pairs[b] = tmp;
                }
            }
        }
    }
    for (i = 0; i < np; i++) {
        if (!pairs[i].even || !pairs[i].odd) {
            zip_set_err("incomplete C pair");
            return 0;
        }
    }

    spr_usage = (uint32_t *)ram_calloc(1, spr_usage_sz);
    if (!spr_usage) {
        zip_set_err("OOM staging spr");
        return 0;
    }

    memset(&st, 0, sizeof(st));
    if (!stream_begin(&st, key_c, tiles)) {
        zip_set_err("ROM too large for flash");
        return 0;
    }
    for (i = 0; i < np; i++) {
        uint32_t tile_base = pairs[i].dest / 128u;
        if (!stream_crom_pair(z, pairs[i].even, pairs[i].ecrc,
                              pairs[i].odd, pairs[i].ocrc,
                              pairs[i].size, spr_usage, tile_base, &st)) {
            stream_abort(&st);
            return 0;
        }
    }
    hit_c = stream_finish(&st);
    if (!hit_c) {
        zip_set_err("tiles finish failed");
        return 0;
    }

    if (!prog_next_file("spr"))
        return 0;
    memset(&st, 0, sizeof(st));
    if (!stream_begin(&st, key_spr, spr_usage_sz) ||
        !append_bytes(&st, spr_usage, spr_usage_sz)) {
        stream_abort(&st);
        zip_set_err("ROM too large for flash");
        return 0;
    }
    hit_spr = stream_finish(&st);
    if (!hit_spr) {
        zip_set_err("spr finish failed");
        return 0;
    }
    if (!prog_bump(spr_usage_sz))
        return 0;
    printf("neo_zip: cached %s + %s\n", key_c, key_spr);
    *c_out = hit_c;
    *c_sz = tiles;
    *spr_out = hit_spr;
    *spr_sz = spr_usage_sz;
    return 1;
}

/* Drop all ram_emu bump allocations then reinstall inflate scratch.
 * mz_zip_archive state lives on the AHB heap / FILE*; only neo_zip file
 * slots used the scratch, and those are closed between region stages. */
static int refresh_zip_scratch(uint32_t scratch_need)
{
    uint8_t *scratch;
    ram_init();
    scratch = (uint8_t *)ram_malloc(scratch_need);
    if (!scratch) {
        zip_set_err("OOM zip scratch");
        neo_zip_set_scratch(NULL, 0);
        return 0;
    }
    neo_zip_set_scratch(scratch, scratch_need);
    return 1;
}

int neo_zip_flash_load(const char *zip_path)
{
    char stem[32];
    const ROM_DEF *drv;
    neo_zip_t *z;
    const uint8_t *p, *m, *v, *s, *gfix, *c, *spr;
    uint32_t p_sz, m_sz, v_sz, s_sz, g_sz, c_sz, spr_sz;
    uint32_t scratch_need;

    zip_err[0] = 0;
    if (!zip_path || !zip_path[0]) {
        zip_set_err("no path");
        return 0;
    }
    path_stem(zip_path, stem, sizeof(stem));

    /* Inflate scratch lives only for the cache phase. */
    scratch_need = neo_zip_scratch_bytes();
    if (!refresh_zip_scratch(scratch_need))
        return 0;

    z = neo_zip_open(zip_path);
    if (!z) {
        zip_set_err("zip open failed");
        ram_init();
        return 0;
    }

    drv = rom_def_from_zip(z, stem);
    if (!drv) {
        neo_zip_close(z);
        neo_zip_set_scratch(NULL, 0);
        ram_init();
        return 0;
    }

    prog_plan(drv, stem);
    if (s_prog_steps) {
#ifndef HOST_BUILD
        uint32_t weight = neo_flash_game_weight(drv);
        uint32_t max_game = neo_flash_max_game_bytes();

        printf("neo_zip: cache %u/%u bytes done, %u files left\n",
               (unsigned)s_prog_done, (unsigned)s_prog_total,
               (unsigned)s_prog_steps);
        if (max_game && weight > max_game) {
            printf("neo_zip: game needs %u bytes, max %u "
                   "(usable=%u bios+ro reserved)\n",
                   (unsigned)weight, (unsigned)max_game,
                   (unsigned)flash_cache_usable_size());
            zip_set_err("ROM too large for flash");
            neo_zip_close(z);
            neo_zip_set_scratch(NULL, 0);
            ram_init();
            return 0;
        }
#else
        printf("neo_zip: cache %u/%u bytes done, %u files left\n",
               (unsigned)s_prog_done, (unsigned)s_prog_total,
               (unsigned)s_prog_steps);
#endif
        snprintf(s_prog_hdr, sizeof(s_prog_hdr), "Neo Geo");
        /* progress==0 clears the B-cancel latch from a previous attempt. */
        (void)odroid_overlay_draw_progress_bar_cancellable(s_prog_hdr, 0);
        if (!prog_paint(prog_pct(), 1)) {
            neo_zip_close(z);
            neo_zip_set_scratch(NULL, 0);
            ram_init();
            return 0;
        }
    } else
        printf("neo_zip: all regions already cached\n");

    printf("neo_zip: loading %s → neogeo/%s/*\n", zip_path, stem);

    if (!ensure_raw_region(z, drv, stem, "p2", REGION_MAIN_CPU_CARTRIDGE,
                           drv->romsize[REGION_MAIN_CPU_CARTRIDGE], &p, &p_sz) ||
        !ensure_raw_region(z, drv, stem, "m", REGION_AUDIO_CPU_CARTRIDGE,
                           drv->romsize[REGION_AUDIO_CPU_CARTRIDGE], &m, &m_sz) ||
        !ensure_raw_region(z, drv, stem, "v", REGION_AUDIO_DATA_1,
                           drv->romsize[REGION_AUDIO_DATA_1], &v, &v_sz) ||
        !ensure_sfix(z, drv, stem, &s, &s_sz, &gfix, &g_sz)) {
        printf("neo_zip: fail before tiles: %s (RAM free=%u)\n",
               zip_err, (unsigned)ram_get_free_size());
        neo_zip_close(z);
        neo_zip_set_scratch(NULL, 0);
        ram_init();
        return 0;
    }

    /* Drop gfix staging (+ inflate scratch) before spr_usage (~64 KiB). */
    if (!refresh_zip_scratch(scratch_need)) {
        neo_zip_close(z);
        ram_init();
        return 0;
    }

    if (!ensure_tiles(z, drv, stem, &c, &c_sz, &spr, &spr_sz)) {
        printf("neo_zip: fail tiles: %s (RAM free=%u)\n",
               zip_err, (unsigned)ram_get_free_size());
        neo_zip_close(z);
        neo_zip_set_scratch(NULL, 0);
        ram_init();
        return 0;
    }
    neo_zip_close(z);
    neo_zip_set_scratch(NULL, 0);

    /* Reclaim all cache-phase ram_emu (scratch + spr staging). */
    ram_init();

    if (s_prog_steps)
        prog_finish();

    if (!neo_rom_bind_regions(stem, p, p_sz, m, m_sz, v, v_sz,
                              s, s_sz, gfix, g_sz, c, c_sz, spr, spr_sz)) {
        printf("neo_zip: bind failed: %s\n", gno_flash_last_error());
        zip_set_err(gno_flash_last_error());
        return 0;
    }
    return 1;
}
