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

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#include "gw_lcd.h"
#include "rg_storage.h"
#endif

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

/* Convert raw SFIX like GnGeo convert_all_char (LE). Scratch must be writable. */
static void neo_convert_sfix(Uint8 *Ptr, int Taille)
{
    int i, j;
    Uint8 *Src;
    Uint8 *sav;
#ifdef HOST_BUILD
    Src = (Uint8 *)malloc((size_t)Taille);
#else
    /* LCD pool is free during ROM load (JT built later). */
    if ((uint32_t)Taille > 150u * 1024u)
        return;
    Src = (Uint8 *)(uintptr_t)(0x24000000u + 150u * 1024u);
#endif
    if (!Src)
        return;
    sav = Src;
    memcpy(Src, Ptr, (size_t)Taille);
    for (i = Taille; i > 0; i -= 32) {
        for (j = 0; j < 8; j++) {
            *Ptr++ = *(Src + 16);
            *Ptr++ = *(Src + 24);
            *Ptr++ = *(Src);
            *Ptr++ = *(Src + 8);
            Src++;
        }
        Src += 24;
    }
#ifdef HOST_BUILD
    free(sav);
#else
    (void)sav;
#endif
}

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

/* R/O flash blob → LE BIOS in flash data cache (LCD scratch). */
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
    q = lookup_data_in_flash(key, &got);
    if (!q || got != nbytes)
        return GN_FALSE;
    *pp = (Uint8 *)(uintptr_t)q;
    printf("gno: BIOS endian-fixed → flash cache\n");
    return GN_TRUE;
}

/* Raw SFIX → converted 128 KiB in flash data cache. */
static int device_ensure_converted_sfix(Uint8 **pp, uint32_t *psz)
{
    Uint8 *raw = *pp;
    uint32_t raw_sz = *psz;
    char key[48];
    uint32_t got = 0;
    const uint8_t *q;
    Uint8 *scratch;
    uint32_t n = 0x20000;

    snprintf(key, sizeof(key), "neogeo/sfix_c_%lu", (unsigned long)raw_sz);
    q = lookup_data_in_flash(key, &got);
    if (q && got == n) {
        *pp = (Uint8 *)(uintptr_t)q;
        *psz = n;
        return GN_TRUE;
    }
    scratch = (Uint8 *)(uintptr_t)0x24000000u;
    memset(scratch, 0, n);
    if (raw_sz > n)
        raw_sz = n;
    memcpy(scratch, raw, raw_sz);
    neo_convert_sfix(scratch, (int)n);
    wdog_refresh();
    if (!store_data_in_flash(key, scratch, n))
        return GN_FALSE;
    q = lookup_data_in_flash(key, &got);
    if (!q || got != n)
        return GN_FALSE;
    *pp = (Uint8 *)(uintptr_t)q;
    *psz = n;
    printf("gno: sfix converted → flash cache\n");
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
    if (device_try_named(roots, cands_sfix, &p, &sz)) {
        if (device_ensure_converted_sfix(&p, &sz)) {
            r->bios_sfix.p = p;
            r->bios_sfix.size = sz;
            got_sfix = 1;
        }
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

    if (type == 0)
        return bind_region(r, size, pp);

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
    mapped = odroid_overlay_cache_file_in_flash(path, &size, false);
    if (!mapped || size < 24) {
        printf("gno: flash cache failed for %s\n", path);
        gno_set_err("flash cache failed");
        return GN_FALSE;
    }

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
    if (!rom_name || !rom_name[0])
        return GN_FALSE;
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
