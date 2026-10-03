/*
 * Flash-mapped .gno loader for Retro-Go SD.
 *
 * Expects an XIP .gno (all regions type 0 / uncompressed) produced by
 * tools/make_gno_xip.py. Large regions stay in OSPI-mapped flash; only a
 * 0x80-byte vector patch is kept in RAM for the BIOS overlay.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "gno_flash.h"
#include "roms.h"
#include "emu.h"
#include "memory.h"
#include "video.h"
#include "screen.h"
#include "gnutil.h"
#include "frame_skip.h"
#include "conf.h"

#include "odroid_overlay.h"
#include "gw_malloc.h"
#include "neo_mem.h"
#include "gw_flash_alloc.h"
#include "neo_flash_ro.h"

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#include "gw_lcd.h"
#include "rg_storage.h"
#include "main.h" /* SCB_CleanInvalidateDCache — CMSIS via HAL */
#endif

#include "neo_zip_flash.h"
#include "neo_pvc.h"

int neogeo_fix_bank_type = 0;


static const uint8_t *gno_base;
static uint32_t gno_size;
static uint8_t vector_patch[0x80] __attribute__((aligned(16)));
static int vector_patched;
static char game_name_storage[16];
static char gno_err[48];
static char neo_bios_dir[256];

void neo_bios_set_dir(const char *dir)
{
    if (!dir || !dir[0]) {
        neo_bios_dir[0] = 0;
        return;
    }
    snprintf(neo_bios_dir, sizeof(neo_bios_dir), "%s", dir);
}

const char *gno_flash_last_error(void)
{
    return gno_err[0] ? gno_err : "unknown";
}

static void gno_set_err(const char *msg)
{
    size_t n = 0;
    if (!msg)
        msg = "error";
    while (msg[n] && n < sizeof(gno_err) - 1) {
        gno_err[n] = msg[n];
        n++;
    }
    gno_err[n] = 0;
}

#ifndef HOST_BUILD
/*
 * OSPI program/erase bypasses the D-cache. Extflash (0x9…) is Normal
 * cacheable by default MPU map, so a freshly written .gno / BIOS blob can
 * still be read as the previous occupant of that address until the cache
 * is dropped. Quit→relaunch works because NVIC_SystemReset clears it.
 *
 * Must Clean+Invalidate (not Invalidate alone): RAM_EMU / FatFS live in
 * write-back AXI SRAM — a bare Invalidate drops dirty lines and breaks
 * later SD opens ("missing BIOS files").
 */
static void neo_xip_sync(void)
{
    SCB_CleanInvalidateDCache();
    __DSB();
    __ISB();
}
#endif

/* From GnGeo roms.c — convert SFIX bitplanes (board BIOS). Unused at
 * runtime when make_gno_xip.py / gngeo --dump already converted SFIX. */
#if 0
static void convert_all_char(Uint8 *Ptr, int Taille, Uint8 *usage_ptr)
{
    (void)Ptr; (void)Taille; (void)usage_ptr;
}
#endif

/* Build fix_board_usage from already-converted SFIX (flash-safe, R/O). */
static void fill_fix_usage(const Uint8 *sfix, int size, Uint8 *usage_ptr)
{
    int i, n = size / 32;
    for (i = 0; i < n && i < 4096; i++) {
        unsigned char u = 0;
        int j;
        const Uint8 *t = sfix + i * 32;
        for (j = 0; j < 32; j++)
            u |= t[j];
        usage_ptr[i] = u;
    }
}

/* Exported for memptr override — XIP ROM is R/O, so vectors live here. */
Uint8 *neo_vector_patch(void)
{
    return vector_patched ? vector_patch : NULL;
}

void neo_vector_use_bios(void)
{
    if (!memory.rom.bios_m68k.p)
        return;
    memcpy(vector_patch, memory.rom.bios_m68k.p, 0x80);
    vector_patched = 1;
    memory.current_vector = 0;
}

void neo_vector_use_game(void)
{
    memcpy(vector_patch, memory.game_vector, 0x80);
    vector_patched = 1;
    memory.current_vector = 1;
}

static int read_u32_le(const uint8_t **pp, uint32_t *out)
{
    const uint8_t *p = *pp;
    if ((uint32_t)(p + 4 - gno_base) > gno_size)
        return GN_FALSE;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *pp = p + 4;
    return GN_TRUE;
}

static int bind_region(ROM_REGION *r, uint32_t size, const uint8_t **pp)
{
    if ((uint32_t)(*pp + size - gno_base) > gno_size)
        return GN_FALSE;
    r->p = (Uint8 *)(uintptr_t)*pp; /* flash XIP — not freeable */
    r->size = size;
    *pp += size;
    return GN_TRUE;
}

#ifdef HOST_BUILD
#include <zlib.h>

/* Classic gngeo --dump stores tiles as zlib blocks (type 1). Expand to RAM. */
static int bind_region_compressed(ROM_REGION *r, uint32_t size, const uint8_t **pp)
{
    uint32_t block_size, nb_block, cmp_size, i;
    const uint8_t *table;
    Uint8 *out;
    const uint8_t *p = *pp;

    if ((uint32_t)(p + 4 - gno_base) > gno_size)
        return GN_FALSE;
    block_size = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    if (block_size == 0 || (size % block_size) != 0)
        return GN_FALSE;
    nb_block = size / block_size;
    if ((uint32_t)(p + nb_block * 4 + 4 - gno_base) > gno_size)
        return GN_FALSE;
    table = p;
    p += nb_block * 4;
    cmp_size = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    out = (Uint8 *)malloc(size);
    if (!out) {
        printf("gno: malloc(%u) for type1 region failed\n", size);
        return GN_FALSE;
    }

    for (i = 0; i < nb_block; i++) {
        uint32_t abs_off = (uint32_t)table[i * 4] | ((uint32_t)table[i * 4 + 1] << 8) |
                           ((uint32_t)table[i * 4 + 2] << 16) | ((uint32_t)table[i * 4 + 3] << 24);
        uint32_t clen;
        uLongf dest_len = block_size;
        const uint8_t *cptr;
        if (abs_off + 4 > gno_size)
            goto fail;
        clen = (uint32_t)gno_base[abs_off] | ((uint32_t)gno_base[abs_off + 1] << 8) |
               ((uint32_t)gno_base[abs_off + 2] << 16) | ((uint32_t)gno_base[abs_off + 3] << 24);
        cptr = gno_base + abs_off + 4;
        if (abs_off + 4 + clen > gno_size)
            goto fail;
        if (uncompress(out + i * block_size, &dest_len, cptr, clen) != Z_OK ||
            dest_len != block_size) {
            printf("gno: zlib block %u failed\n", i);
            goto fail;
        }
    }

    (void)cmp_size;
    /* Advance past compressed stream (GnGeo skips cmp_size bytes). */
    {
        const uint8_t *end = p + cmp_size;
        if (nb_block) {
            uint32_t last = (uint32_t)table[(nb_block - 1) * 4] |
                            ((uint32_t)table[(nb_block - 1) * 4 + 1] << 8) |
                            ((uint32_t)table[(nb_block - 1) * 4 + 2] << 16) |
                            ((uint32_t)table[(nb_block - 1) * 4 + 3] << 24);
            uint32_t last_clen = (uint32_t)gno_base[last] | ((uint32_t)gno_base[last + 1] << 8) |
                                 ((uint32_t)gno_base[last + 2] << 16) | ((uint32_t)gno_base[last + 3] << 24);
            const uint8_t *last_end = gno_base + last + 4 + last_clen;
            if (last_end > end)
                end = last_end;
        }
        *pp = end;
    }
    r->p = out;
    r->size = size;
    printf("gno: expanded type1 region → %u bytes\n", size);
    return GN_TRUE;
fail:
    free(out);
    return GN_FALSE;
}

static int host_load_file(const char *path, Uint8 **out, uint32_t *out_size)
{
    FILE *f;
    long sz;
    Uint8 *buf;
    size_t n;
    f = fopen(path, "rb");
    if (!f)
        return GN_FALSE;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return GN_FALSE;
    }
    sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        return GN_FALSE;
    }
    buf = (Uint8 *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return GN_FALSE;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return GN_FALSE;
    }
    *out = buf;
    *out_size = (uint32_t)sz;
    return GN_TRUE;
}

static int host_load_from_zip(const char *zip, const char *member, Uint8 **out, uint32_t *out_size)
{
    char cmd[1024];
    FILE *p;
    Uint8 *buf = NULL;
    size_t cap = 0, len = 0;
    size_t n;
    unsigned char chunk[65536];

    snprintf(cmd, sizeof(cmd), "unzip -p \"%s\" \"%s\" 2>/dev/null", zip, member);
    p = popen(cmd, "r");
    if (!p)
        return GN_FALSE;
    while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) {
        Uint8 *nb = (Uint8 *)realloc(buf, len + n);
        if (!nb) {
            free(buf);
            pclose(p);
            return GN_FALSE;
        }
        buf = nb;
        memcpy(buf + len, chunk, n);
        len += n;
        cap = len;
    }
    (void)cap;
    if (pclose(p) != 0 || !buf || len == 0) {
        free(buf);
        return GN_FALSE;
    }
    *out = buf;
    *out_size = (uint32_t)len;
    return GN_TRUE;
}
#endif /* HOST_BUILD — zip/file helpers + type1 expand */

/* Convert raw SFIX like GnGeo convert_all_char (LE), one 8×8 tile (32 B). */
static void convert_one_sfix_tile(uint8_t *tile)
{
    uint8_t src[32];
    uint8_t *d = tile;
    const uint8_t *s = src;
    int j;

    memcpy(src, tile, 32);
    for (j = 0; j < 8; j++) {
        *d++ = s[16];
        *d++ = s[24];
        *d++ = s[0];
        *d++ = s[8];
        s++;
    }
}

static void convert_sfix_tiles_inplace(uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0; i + 32u <= len; i += 32u)
        convert_one_sfix_tile(buf + i);
}

#ifdef HOST_BUILD
/* Host path: full buffer already in malloc'd RAM. */
static void neo_convert_sfix(Uint8 *Ptr, int Taille)
{
    if (!Ptr || Taille <= 0)
        return;
    convert_sfix_tiles_inplace(Ptr, (uint32_t)Taille);
}
#endif

/* Convert BIOS dumps to LE word order (matches gngeo --dump / .gno carts). */
static void neo_endian_fix_m68k_bios(Uint8 *p, uint32_t size)
{
    uint32_t i;
    if (!p || size < 8)
        return;
    if (p[0] == 0x00 && p[1] == 0x10) {
        for (i = 0; i + 1 < size; i += 2) {
            Uint8 t = p[i];
            p[i] = p[i + 1];
            p[i + 1] = t;
        }
    }
}

static int neo_bios_needs_swap(const Uint8 *p, uint32_t size)
{
    return (p && size >= 2 && p[0] == 0x00 && p[1] == 0x10);
}

#ifdef HOST_BUILD
static int host_try_named(const char *const *roots, const char *const *names,
                          Uint8 **out, uint32_t *out_size)
{
    int r, n;
    char path[1024];

    for (r = 0; roots[r]; r++) {
        const char *root = roots[r];
        if (!root[0])
            continue;
        for (n = 0; names[n]; n++) {
            if (strstr(root, ".zip")) {
                if (host_load_from_zip(root, names[n], out, out_size))
                    return GN_TRUE;
            } else {
                snprintf(path, sizeof(path), "%s/%s", root, names[n]);
                if (host_load_file(path, out, out_size))
                    return GN_TRUE;
                if (host_load_file(names[n], out, out_size))
                    return GN_TRUE;
            }
        }
    }
    return GN_FALSE;
}
#else
/* Map an SD file into OSPI (R/O). */
static int device_map_file(const char *path, Uint8 **out, uint32_t *out_size)
{
    uint32_t sz = 0;
    uint8_t *p;

    if (!path || !path[0])
        return GN_FALSE;
    p = odroid_overlay_cache_file_in_flash(path, &sz, false);
    if (!p || sz == 0)
        return GN_FALSE;
    neo_xip_sync();
    *out = p;
    *out_size = sz;
    return GN_TRUE;
}

static int device_try_named(const char *const *roots, const char *const *names,
                            Uint8 **out, uint32_t *out_size)
{
    int r, n;
    char path[256];

    for (r = 0; roots[r]; r++) {
        for (n = 0; names[n]; n++) {
            snprintf(path, sizeof(path), "%s/%s", roots[r], names[n]);
            if (device_map_file(path, out, out_size)) {
                printf("gno: mapped %s (%u bytes)\n", path, (unsigned)*out_size);
                return GN_TRUE;
            }
        }
    }
    return GN_FALSE;
}

/* SD→flash relocate hook: convert each 16 KiB chunk in the firmware's
 * program buffer (not the LCD FB). Same contract as neo_flash_ro. */
static void sfix_relocate_cb(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                              uint8_t *file_address, uint32_t file_size)
{
    (void)offset_in_file;
    (void)file_address;
    (void)file_size;
    convert_sfix_tiles_inplace(buffer, length);
    wdog_refresh();
}

/* Cache sfix with tile convert in the write buffer. Prefer versioned
 * store_data key so an older raw path-cache hit cannot skip convert. */
static int device_map_sfix_converted(const char *path, Uint8 **out, uint32_t *out_size)
{
    static const char key[] = "neogeo/sfix_rt1";
    uint32_t got = 0;
    const uint8_t *hit;
    FILE *f;
    flash_stream_t st;
    uint8_t buf[4096];
    uint32_t done = 0;
    const uint32_t n = 0x20000;

    hit = lookup_data_in_flash(key, &got);
    if (hit && got == n) {
        *out = (Uint8 *)(uintptr_t)hit;
        *out_size = n;
        return GN_TRUE;
    }

    f = fopen(path, "rb");
    if (!f)
        return GN_FALSE;

    memset(&st, 0, sizeof(st));
    if (!store_data_begin(&st, key, n)) {
        fclose(f);
        return GN_FALSE;
    }

    while (done < n) {
        uint32_t want = n - done;
        size_t rd;
        if (want > sizeof(buf))
            want = sizeof(buf);
        rd = fread(buf, 1, want, f);
        if (rd == 0) {
            memset(buf, 0, want);
            rd = want;
        } else if (rd < want) {
            memset(buf + rd, 0, want - rd);
            rd = want;
        }
        /* Same transform as cache_file_in_flash_relocate's relocate_cb. */
        sfix_relocate_cb(buf, (uint32_t)rd, done, NULL, n);
        if (!store_data_append(&st, buf, (uint32_t)rd)) {
            store_data_abort(&st);
            fclose(f);
            return GN_FALSE;
        }
        done += (uint32_t)rd;
        {
            uint8_t pct = (uint8_t)((done * 100u) / n);
            if (pct > 99)
                pct = 99;
            if (!odroid_overlay_draw_progress_bar_cancellable("sfix", pct)) {
                store_data_abort(&st);
                fclose(f);
                return GN_FALSE;
            }
        }
        wdog_refresh();
    }
    fclose(f);

    hit = store_data_finish(&st);
    if (!hit) {
        store_data_abort(&st);
        return GN_FALSE;
    }
    neo_xip_sync();
    *out = (Uint8 *)(uintptr_t)hit;
    *out_size = n;
    printf("gno: sfix converted → flash (%s)\n", path);
    (void)odroid_overlay_draw_progress_bar_cancellable("sfix", 100);
    return GN_TRUE;
}

static int device_try_named_sfix(const char *const *roots, const char *const *names,
                                  Uint8 **out, uint32_t *out_size)
{
    int r, n;
    char path[256];

    for (r = 0; roots[r]; r++) {
        for (n = 0; names[n]; n++) {
            snprintf(path, sizeof(path), "%s/%s", roots[r], names[n]);
            if (device_map_sfix_converted(path, out, out_size)) {
                printf("gno: mapped %s (%u bytes, converted)\n",
                       path, (unsigned)*out_size);
                return GN_TRUE;
            }
        }
    }
    return GN_FALSE;
}

/* R/O flash blob → LE BIOS in flash data cache (LCD scratch — BIOS only,
 * and only when endian swap is required; uncommon for UniBIOS LE dumps). */
static int device_ensure_le_bios(Uint8 **pp, uint32_t *psz)
{
    Uint8 *p = *pp;
    uint32_t nbytes = *psz;
    char key[48];
    uint32_t got = 0;
    const uint8_t *q;
    Uint8 *scratch;

    if (!neo_bios_needs_swap(p, nbytes))
        return GN_TRUE;
    snprintf(key, sizeof(key), "neogeo/bios_le_%lu", (unsigned long)nbytes);
    q = lookup_data_in_flash(key, &got);
    if (q && got == nbytes) {
        *pp = (Uint8 *)(uintptr_t)q;
        return GN_TRUE;
    }
    if (nbytes > 300u * 1024u)
        return GN_FALSE;
    scratch = (Uint8 *)(uintptr_t)0x24000000u;
    memcpy(scratch, p, nbytes);
    neo_endian_fix_m68k_bios(scratch, nbytes);
    wdog_refresh();
    if (!store_data_in_flash(key, scratch, nbytes))
        return GN_FALSE;
    neo_xip_sync();
    q = lookup_data_in_flash(key, &got);
    if (!q || got != nbytes)
        return GN_FALSE;
    *pp = (Uint8 *)(uintptr_t)q;
    printf("gno: BIOS endian-fixed → flash cache\n");
    return GN_TRUE;
}
#endif /* !HOST_BUILD */

/*
 * Load board files from an external BIOS directory when present.
 * Prefer these over regions embedded in the .gno so one SD UniBIOS
 * applies to every game (host_roms vs plain dumps stay consistent).
 */
static int neo_load_system_bios(GAME_ROMS *r)
{
    const char *roots[10];
    int nroot = 0;
    const char *cands_cpu[] = {
        "uni-bios.rom", "uni-bios_4_0.rom", "uni-bios_3_3.rom", "sp-s2.sp1",
        "usa_2slt.bin", NULL
    };
    const char *cands_sfix[] = { "sfix.sfx", "sfix.sfix", NULL };
    const char *cands_lo[] = { "000-lo.lo", NULL };
    Uint8 *p;
    uint32_t sz;
    int got_cpu = 0, got_sfix = 0;

#ifdef HOST_BUILD
    {
        const char *env = getenv("NEOGEO_BIOS");
        if (neo_bios_dir[0])
            roots[nroot++] = neo_bios_dir;
        if (env && env[0])
            roots[nroot++] = env;
        roots[nroot++] = "uni-bios-40";
        roots[nroot++] = "neogeo.zip";
        roots[nroot++] = "bios/neogeo";
        roots[nroot++] = "bios";
    }
#else
    if (neo_bios_dir[0])
        roots[nroot++] = neo_bios_dir;
    roots[nroot++] = "/bios/neogeo";
    roots[nroot++] = RG_BASE_PATH_BIOS "/neogeo";
#endif
    roots[nroot] = NULL;

    p = NULL;
    sz = 0;
#ifdef HOST_BUILD
    if (host_try_named(roots, cands_cpu, &p, &sz)) {
        neo_endian_fix_m68k_bios(p, sz);
        r->bios_m68k.p = p;
        r->bios_m68k.size = sz;
        got_cpu = 1;
        printf("gno: external bios_m68k %u bytes\n", (unsigned)sz);
    }
#else
    if (device_try_named(roots, cands_cpu, &p, &sz)) {
        if (device_ensure_le_bios(&p, &sz)) {
            r->bios_m68k.p = p;
            r->bios_m68k.size = sz;
            got_cpu = 1;
        }
    }
#endif

    p = NULL;
    sz = 0;
#ifdef HOST_BUILD
    if (host_try_named(roots, cands_sfix, &p, &sz)) {
        if (sz < 0x20000) {
            Uint8 *padded = (Uint8 *)realloc(p, 0x20000);
            if (padded) {
                memset(padded + sz, 0, 0x20000 - sz);
                p = padded;
                sz = 0x20000;
            }
        }
        neo_convert_sfix(p, 0x20000);
        r->bios_sfix.p = p;
        r->bios_sfix.size = sz < 0x20000 ? sz : 0x20000;
        got_sfix = 1;
        printf("gno: external bios_sfix converted\n");
    }
#else
    if (device_try_named_sfix(roots, cands_sfix, &p, &sz)) {
        r->bios_sfix.p = p;
        r->bios_sfix.size = sz;
        got_sfix = 1;
    }
#endif

    p = NULL;
    sz = 0;
#ifdef HOST_BUILD
    if (host_try_named(roots, cands_lo, &p, &sz)) {
        r->zoom_table.p = p;
        r->zoom_table.size = sz;
        printf("gno: external 000-lo.lo %u bytes\n", (unsigned)sz);
    }
#else
    if (device_try_named(roots, cands_lo, &p, &sz)) {
        r->zoom_table.p = p;
        r->zoom_table.size = sz;
    }
#endif

    if (!r->bios_m68k.p || !r->bios_sfix.p) {
        printf("gno: need bios_m68k+sfix (external%s)\n",
               got_cpu || got_sfix ? " partial" : "");
#ifdef HOST_BUILD
        printf("gno: host: --bios DIR or NEOGEO_BIOS=… or uni-bios-40/\n");
#else
        printf("gno: device: put files in /bios/neogeo/ "
               "(uni-bios.rom, sfix.sfix, 000-lo.lo)\n");
#endif
        return GN_FALSE;
    }
    if (!got_cpu || !got_sfix)
        printf("gno: using BIOS regions from .gno "
               "(prefer /bios/neogeo for a shared UniBIOS)\n");
    return GN_TRUE;
}

static int parse_region(const uint8_t **pp, GAME_ROMS *roms)
{
    uint32_t size;
    uint8_t lid, type;
    ROM_REGION *r = NULL;

    if (!read_u32_le(pp, &size))
        return GN_FALSE;
    if ((uint32_t)(*pp + 2 - gno_base) > gno_size)
        return GN_FALSE;
    lid = *(*pp)++;
    type = *(*pp)++;

    switch (lid) {
    case REGION_MAIN_CPU_CARTRIDGE:   r = &roms->cpu_m68k; break;
    case REGION_AUDIO_CPU_CARTRIDGE:  r = &roms->cpu_z80; break;
    case REGION_AUDIO_DATA_1:         r = &roms->adpcma; break;
    case REGION_AUDIO_DATA_2:         r = &roms->adpcmb; break;
    case REGION_FIXED_LAYER_CARTRIDGE:r = &roms->game_sfix; break;
    case REGION_SPRITES:              r = &roms->tiles; break;
    case REGION_SPR_USAGE:            r = &roms->spr_usage; break;
    case REGION_GAME_FIX_USAGE:       r = &roms->gfix_usage; break;
    case REGION_FIXED_LAYER_BIOS:     r = &roms->bios_sfix; break;
    case REGION_MAIN_CPU_BIOS:        r = &roms->bios_m68k; break;
    case REGION_AUDIO_CPU_BIOS:       r = &roms->bios_audio; break;
    case REGION_ZOOM_TABLE:           r = &roms->zoom_table; break;
    default:
        printf("gno: unknown region id %u\n", lid);
        return GN_FALSE;
    }

    if (type == 0) {
        if (!bind_region(r, size, pp)) {
            printf("gno: region id=%u size=%lu past end (off=%lu/%lu)\n",
                   lid, (unsigned long)size,
                   (unsigned long)(*pp - gno_base),
                   (unsigned long)gno_size);
            return GN_FALSE;
        }
        return GN_TRUE;
    }

#ifdef HOST_BUILD
    if (type == 1)
        return bind_region_compressed(r, size, pp);
#endif

    printf("gno: compressed region %u not supported (use make_gno_xip.py)\n", lid);
    gno_set_err("need XIP .gno");
    return GN_FALSE;
}

static void apply_bios_vectors(GAME_ROMS *r)
{
    if (!r->cpu_m68k.p || !r->bios_m68k.p)
        return;
    memcpy(memory.game_vector, r->cpu_m68k.p, 0x80);
    memcpy(vector_patch, r->bios_m68k.p, 0x80);
    vector_patched = 1;
}

int neo_rom_bind_regions(const char *stem,
                         const uint8_t *p, uint32_t p_sz,
                         const uint8_t *m, uint32_t m_sz,
                         const uint8_t *v, uint32_t v_sz,
                         const uint8_t *s, uint32_t s_sz,
                         const uint8_t *gfix, uint32_t g_sz,
                         const uint8_t *c, uint32_t c_sz,
                         const uint8_t *spr, uint32_t spr_sz)
{
    GAME_ROMS *r = &memory.rom;

    if (!stem || !p || !m || !v || !s || !gfix || !c || !spr) {
        gno_set_err("bad region ptrs");
        return GN_FALSE;
    }

    memset(r, 0, sizeof(*r));
    vector_patched = 0;
    memory.bksw_handler = 0;
    memory.bksw_unscramble = NULL;
    memory.bksw_offset = NULL;
    memory.vid.spr_cache.data = NULL;
    memory.vid.spr_cache.gno = NULL;
    gno_err[0] = 0;

    snprintf(game_name_storage, sizeof(game_name_storage), "%s", stem);
    r->info.name = game_name_storage;
    r->info.longname = game_name_storage;
    r->info.flags = 0;

    r->cpu_m68k.p = (Uint8 *)(uintptr_t)p;
    r->cpu_m68k.size = p_sz;
    r->cpu_z80.p = (Uint8 *)(uintptr_t)m;
    r->cpu_z80.size = m_sz;
    r->adpcma.p = (Uint8 *)(uintptr_t)v;
    r->adpcma.size = v_sz;
    r->adpcmb.p = r->adpcma.p;
    r->adpcmb.size = r->adpcma.size;
    r->game_sfix.p = (Uint8 *)(uintptr_t)s;
    r->game_sfix.size = s_sz;
    r->gfix_usage.p = (Uint8 *)(uintptr_t)gfix;
    r->gfix_usage.size = g_sz;
    r->tiles.p = (Uint8 *)(uintptr_t)c;
    r->tiles.size = c_sz;
    r->spr_usage.p = (Uint8 *)(uintptr_t)spr;
    r->spr_usage.size = spr_sz;

    memory.fix_game_usage = r->gfix_usage.p;
    memory.nb_of_tiles = r->tiles.size >> 7;

    printf("neo: game=%s p=%u m=%u v=%u s=%u c=%u\n",
           stem, (unsigned)p_sz, (unsigned)m_sz, (unsigned)v_sz,
           (unsigned)s_sz, (unsigned)c_sz);

#ifndef HOST_BUILD
    neo_xip_sync();
#endif

    /* Lasting RAM_EMU allocs happen after zip's ram_init(). */
    if (!memory.fix_board_usage) {
        memory.fix_board_usage = ram_calloc(1, 4096);
        if (!neo_alloc_ok(memory.fix_board_usage)) {
            memory.fix_board_usage = NULL;
            gno_set_err("OOM fix_board");
            return GN_FALSE;
        }
    }

    if (neo_load_system_bios(r) != GN_TRUE) {
        gno_set_err("missing BIOS files");
        return GN_FALSE;
    }

    if (r->zoom_table.p && r->zoom_table.size >= 0x10000) {
        memory.ng_lo = r->zoom_table.p;
    } else if (!memory.ng_lo) {
        memory.ng_lo = ram_malloc(0x10000);
        if (neo_alloc_ok(memory.ng_lo))
            memset(memory.ng_lo, 0, 0x10000);
        else
            memory.ng_lo = NULL;
        printf("neo: warning — missing 000-lo.lo zoom table\n");
    }

    if (r->bios_sfix.p)
        fill_fix_usage(r->bios_sfix.p, (int)r->bios_sfix.size, memory.fix_board_usage);

    apply_bios_vectors(r);
    conf.game = r->info.name;

    /* ZIP/GNO path never runs init_roms(); apply PVC + fix-bank by name. */
    neo_game_special_init(stem);

    memset(memory.vid.ram, 0, sizeof(memory.vid.ram));
    memset(memory.vid.pal_neo, 0, sizeof(memory.vid.pal_neo));
    memset(memory.vid.pal_host, 0, sizeof(memory.vid.pal_host));
    memory.vid.currentpal = 0;
    memory.vid.currentfix = 0;
    current_pal = memory.vid.pal_neo[0];
    current_pc_pal = (Uint32 *)memory.vid.pal_host[0];
    current_fix = r->bios_sfix.p;
    fix_usage = memory.fix_board_usage;
    update_all_pal();
    init_video();
    return GN_TRUE;
}

int gno_flash_load(const char *path)
{
    uint32_t size = 0;
    uint8_t *mapped;
    const uint8_t *p;
    GAME_ROMS *r = &memory.rom;
    uint8_t nb_sec;
    uint32_t flags;
    int i;
    char name[9];

    memset(r, 0, sizeof(*r));
    vector_patched = 0;
    memory.bksw_handler = 0;
    memory.bksw_unscramble = NULL;
    memory.bksw_offset = NULL;
    memory.vid.spr_cache.data = NULL;
    memory.vid.spr_cache.gno = NULL;

    gno_err[0] = 0;

#ifndef HOST_BUILD
    /* Refuse oversized XIP dumps before the long "Caching game…" pass.
     * Same budget as ZIP: usable cache − BIOS − neogeo.ro. */
    {
        FILE *fz = fopen(path, "rb");
        uint32_t usable = flash_cache_usable_size();
        if (fz && usable) {
            long fsz;
            fseek(fz, 0, SEEK_END);
            fsz = ftell(fz);
            fclose(fz);
            if (fsz > 0) {
                uint32_t reserved = (0x20000u + 0x20000u + 0x10000u);
                uint32_t ro = neo_flash_ro_size();
                uint32_t max_game, need;
                reserved = (reserved + 4095u) & ~4095u;
                reserved += (ro + 4095u) & ~4095u;
                max_game = (reserved < usable) ? (usable - reserved) : 0;
                need = ((uint32_t)fsz + 4095u) & ~4095u;
                if (max_game && need > max_game) {
                    printf("gno: %s needs %u, max %u (usable=%u)\n",
                           path, (unsigned)need, (unsigned)max_game,
                           (unsigned)usable);
                    gno_set_err("ROM too large for flash");
                    return GN_FALSE;
                }
            }
        } else if (fz) {
            fclose(fz);
        }
    }
#endif

    mapped = odroid_overlay_cache_file_in_flash(path, &size, false);
    if (!mapped) {
        printf("gno: flash cache failed for %s\n", path);
        gno_set_err("ROM too large for flash");
        return GN_FALSE;
    }
    if (size < 24) {
        printf("gno: truncated .gno (%u bytes)\n", (unsigned)size);
        gno_set_err("bad .gno");
        return GN_FALSE;
    }
#ifndef HOST_BUILD
    /* Drop stale D-cache lines left from a previous XIP occupant of this
     * address (circular flash rewrite). Without this, the first boot after
     * "Caching game…" can parse garbage → "bad .gno region". */
    neo_xip_sync();
#endif

    gno_base = mapped;
    gno_size = size;
    p = mapped;

    if (memcmp(p, "gnodmpv1", 8) != 0) {
        printf("gno: bad magic\n");
        gno_set_err("bad .gno magic");
        return GN_FALSE;
    }
    p += 8;

    memset(name, 0, sizeof(name));
    memcpy(name, p, 8);
    p += 8;
    {
        char *sp = strchr(name, ' ');
        if (sp) *sp = 0;
    }
    strncpy(game_name_storage, name, sizeof(game_name_storage) - 1);
    r->info.name = game_name_storage;
    r->info.longname = game_name_storage;

    flags = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    r->info.flags = flags;
    nb_sec = *p++;

    printf("gno: game=%s flags=%08lx sections=%u size=%lu\n",
           r->info.name, (unsigned long)flags, nb_sec, (unsigned long)size);

    for (i = 0; i < nb_sec; i++) {
        if (!parse_region(&p, r)) {
            if (!gno_err[0])
                gno_set_err("bad .gno region");
            return GN_FALSE;
        }
    }

    if (r->adpcmb.p == NULL) {
        r->adpcmb.p = r->adpcma.p;
        r->adpcmb.size = r->adpcma.size;
    }

    memory.fix_game_usage = r->gfix_usage.p;
    memory.nb_of_tiles = r->tiles.size >> 7;

    /* Prefer SD /bios/neogeo (or host --bios) over regions baked into the
     * .gno so every game uses the same UniBIOS/sfix/000-lo. */
    if (!memory.fix_board_usage) {
        memory.fix_board_usage = ram_calloc(1, 4096);
        if (!neo_alloc_ok(memory.fix_board_usage)) {
            memory.fix_board_usage = NULL;
            gno_set_err("OOM fix_board");
            return GN_FALSE;
        }
    }
    if (neo_load_system_bios(r) != GN_TRUE) {
        gno_set_err("missing BIOS files");
        return GN_FALSE;
    }

    if (r->zoom_table.p && r->zoom_table.size >= 0x10000) {
        memory.ng_lo = r->zoom_table.p;
    } else if (!memory.ng_lo) {
        memory.ng_lo = ram_malloc(0x10000);
        if (memory.ng_lo)
            memset(memory.ng_lo, 0, 0x10000);
        printf("gno: warning — missing 000-lo.lo zoom table (sprites may glitch)\n");
    }

    /* Board SFIX must be GnGeo-converted (external path converts; XIP dump must). */
    if (r->bios_sfix.p)
        fill_fix_usage(r->bios_sfix.p, (int)r->bios_sfix.size, memory.fix_board_usage);

    apply_bios_vectors(r);

    conf.game = r->info.name;

    /* Palette / fix-layer pointers (normally set in roms.c init_game). */
    memset(memory.vid.ram, 0, sizeof(memory.vid.ram));
    memset(memory.vid.pal_neo, 0, sizeof(memory.vid.pal_neo));
    memset(memory.vid.pal_host, 0, sizeof(memory.vid.pal_host));
    memory.vid.currentpal = 0;
    memory.vid.currentfix = 0;
    current_pal = memory.vid.pal_neo[0];
    current_pc_pal = (Uint32 *)memory.vid.pal_host[0];
    current_fix = r->bios_sfix.p;
    fix_usage = memory.fix_board_usage;
    update_all_pal();

    init_video();

    return GN_TRUE;
}

/* --- roms.h API shims --------------------------------------------------- */

int init_game(char *rom_name)
{
    const char *ext;
    if (!rom_name || !rom_name[0])
        return GN_FALSE;

    ext = strrchr(rom_name, '.');
    if (ext && ((ext[1] == 'z' || ext[1] == 'Z') &&
                (ext[2] == 'i' || ext[2] == 'I') &&
                (ext[3] == 'p' || ext[3] == 'P') &&
                ext[4] == '\0')) {
        if (!neo_zip_flash_load(rom_name)) {
            gno_set_err(neo_zip_flash_last_error());
            return GN_FALSE;
        }
        reset_frame_skip();
        return GN_TRUE;
    }

    if (gno_flash_load(rom_name) != GN_TRUE)
        return GN_FALSE;
    reset_frame_skip();
    return GN_TRUE;
}

int close_game(void)
{
    return GN_TRUE;
}

void dr_free_roms(GAME_ROMS *r)
{
    (void)r;
    /* Regions live in flash — nothing to free. */
}

int dr_load_game(char *zip)
{
    (void)zip;
    return GN_FALSE;
}

int dr_open_gno(char *filename)
{
    return gno_flash_load(filename);
}

char *dr_gno_romname(char *filename)
{
    (void)filename;
    return game_name_storage;
}

ROM_DEF *dr_check_zip(const char *filename)
{
    (void)filename;
    return NULL;
}

int dr_save_gno(GAME_ROMS *r, char *filename)
{
    (void)r; (void)filename;
    return GN_FALSE;
}

int dr_load_roms(GAME_ROMS *r, char *rom_path, char *name)
{
    (void)r; (void)rom_path; (void)name;
    return GN_FALSE;
}
