/*
 * Cart id + SMA/PVC/fix_bank setup (boot / init only).
 *
 * Cold: linked into /cores/gngeo.ro (ld/neogeo_core.ld). Keep out of
 * RAM_EMU so mslug5 still has 8 KiB for PVC after the Musashi JT.
 * SMA bank tables are XIP'd from flash at runtime (rare reads).
 */
#include <stdio.h>
#include <string.h>

#include "neo_pvc.h"
#include "memory.h"
#include "emu.h"

extern int neogeo_fix_bank_type;

static char neo_tolower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int neo_mem_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (neo_tolower(*a) != neo_tolower(*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static int neo_mem_startswith(const char *s, const char *prefix, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (!s[i] || neo_tolower(s[i]) != neo_tolower(prefix[i]))
			return 0;
	}
	return 1;
}

static int name_is(const char *name, const char *prefix)
{
	size_t n = strlen(prefix);
	return name && (!strncmp(name, prefix, n) &&
			(name[n] == '\0' || name[n] == '_' || name[n] == '-'));
}

/*
 * Exact zip-member → shortname. MAME P chips (decrypt_neogeo_zip detect=)
 * plus common PCB / clone P names. decrypt_neogeo_zip rewrites members to
 * {shortname}-p1.p1 — handled below by prefix scan.
 */
static const struct {
	const char *member;
	const char *game;
} s_member_exact[] = {
	{ "251-p1.p1", "kof99" },
	{ "253-ep1.p1", "garou" },
	{ "256-pg1.p1", "mslug3" },
	{ "257-p1.p1", "kof2000" },
	{ "262-p1-08-e0.p1", "kof2001" },
	{ "265-p1.p1", "kof2002" },
	{ "263-p1.p1", "mslug4" },
	{ "264-p1.p1", "rotd" },
	{ "pn202.p1", "pnyaa" },
	{ "252-p1.p1", "ganryu" },
	{ "254-p1.p1", "s1945p" },
	{ "255-p1.p1", "preisle2" },
	{ "259-p1.p1", "bangbead" },
	{ "260-p1.p1", "nitd" },
	{ "070-p1.p1", "zupapa" },
	{ "261-ph1.p1", "sengoku3" },
	{ "268-p1cr.p1", "mslug5" },
	{ "268-p1c.p1", "mslug5" },
	{ "269-p1.p1", "svc" },
	{ "271-p1c.p1", "kof2003" },
	{ "271-p1.p1", "kof2003" },
	{ "270-p1.p1", "samsho5" },
	{ "272-p1.p1", "samsh5sp" },
	{ "242-p1.p1", "kof98" },
	{ NULL, NULL }
};

/* Longest-first so mslug3h wins over mslug3, kof2000n over kof2000, etc. */
static const char *const s_game_prefixes[] = {
	"svcsplus",
	"samsh5sp",
	"preisle2",
	"sengoku3",
	"kof2003",
	"kof2002",
	"kof2001",
	"kof2000n",
	"kof2000",
	"mslug5",
	"ms5pcb",
	"ms5plus",
	"mslug4",
	"ms4plus",
	"mslug3h",
	"mslug3n",
	"mslug3b",
	"mslug3",
	"kof99n",
	"kof99",
	"garoubl",
	"garou",
	"bangbead",
	"s1945p",
	"samsho5",
	"matrim",
	"ganryu",
	"zupapa",
	"pnyaa",
	"nitd",
	"rotd",
	"svc",
	"kof98",
	NULL
};

const char *neo_game_match_member(const char *member_basename)
{
	const char *base;
	size_t i;

	if (!member_basename || !member_basename[0])
		return NULL;

	base = strrchr(member_basename, '/');
	base = base ? base + 1 : member_basename;

	for (i = 0; s_member_exact[i].member; i++) {
		if (neo_mem_eq(base, s_member_exact[i].member))
			return s_member_exact[i].game;
	}

	/* decrypt_neogeo_zip / FBNeo-style: {shortname}-p1.p1 (any -p* chip). */
	for (i = 0; s_game_prefixes[i]; i++) {
		size_t n = strlen(s_game_prefixes[i]);
		if (neo_mem_startswith(base, s_game_prefixes[i], n) &&
		    base[n] == '-' &&
		    (base[n + 1] == 'p' || base[n + 1] == 'P'))
			return s_game_prefixes[i];
	}
	return NULL;
}

/* SMA bank tables (from roms.c init_* — roms.c is not linked in this core). */
static const int bankoffset_kof99[64] = {
	0x000000, 0x100000, 0x200000, 0x300000, 0x3cc000,
	0x4cc000, 0x3f2000, 0x4f2000, 0x407800, 0x507800, 0x40d000, 0x50d000,
	0x417800, 0x517800, 0x420800, 0x520800, 0x424800, 0x524800, 0x429000,
	0x529000, 0x42e800, 0x52e800, 0x431800, 0x531800, 0x54d000, 0x551000,
	0x567000, 0x592800, 0x588800, 0x581800, 0x599800, 0x594800, 0x598000,
};
static const Uint8 scramblecode_kof99[7] = {0xF0, 14, 6, 8, 10, 12, 5};

static const int bankoffset_garou[64] = {
	0x000000, 0x100000, 0x200000, 0x300000,
	0x280000, 0x380000, 0x2d0000, 0x3d0000,
	0x2f0000, 0x3f0000, 0x400000, 0x500000,
	0x420000, 0x520000, 0x440000, 0x540000,
	0x498000, 0x598000, 0x4a0000, 0x5a0000,
	0x4a8000, 0x5a8000, 0x4b0000, 0x5b0000,
	0x4b8000, 0x5b8000, 0x4c0000, 0x5c0000,
	0x4c8000, 0x5c8000, 0x4d0000, 0x5d0000,
	0x458000, 0x558000, 0x460000, 0x560000,
	0x468000, 0x568000, 0x470000, 0x570000,
	0x478000, 0x578000, 0x480000, 0x580000,
	0x488000, 0x588000, 0x490000, 0x590000,
	0x5d0000, 0x5d8000, 0x5e0000, 0x5e8000,
	0x5f0000, 0x5f8000, 0x600000,
};
static const Uint8 scramblecode_garou[7] = {0xC0, 5, 9, 7, 6, 14, 12};

static const int bankoffset_mslug3[64] = {
	0x000000, 0x020000, 0x040000, 0x060000,
	0x070000, 0x090000, 0x0b0000, 0x0d0000,
	0x0e0000, 0x0f0000, 0x120000, 0x130000,
	0x140000, 0x150000, 0x180000, 0x190000,
	0x1a0000, 0x1b0000, 0x1e0000, 0x1f0000,
	0x200000, 0x210000, 0x240000, 0x250000,
	0x260000, 0x270000, 0x2a0000, 0x2b0000,
	0x2c0000, 0x2d0000, 0x300000, 0x310000,
	0x320000, 0x330000, 0x360000, 0x370000,
	0x380000, 0x390000, 0x3c0000, 0x3d0000,
	0x400000, 0x410000, 0x440000, 0x450000,
	0x460000, 0x470000, 0x4a0000, 0x4b0000,
	0x4c0000,
};
static const Uint8 scramblecode_mslug3[7] = {0xE4, 14, 12, 15, 6, 3, 9};

static const int bankoffset_kof2000[64] = {
	0x000000, 0x100000, 0x200000, 0x300000,
	0x3f7800, 0x4f7800, 0x3ff800, 0x4ff800,
	0x407800, 0x507800, 0x40f800, 0x50f800,
	0x416800, 0x516800, 0x41d800, 0x51d800,
	0x424000, 0x524000, 0x523800, 0x623800,
	0x526000, 0x626000, 0x528000, 0x628000,
	0x52a000, 0x62a000, 0x52b800, 0x62b800,
	0x52d000, 0x62d000, 0x52e800, 0x62e800,
	0x618000, 0x619000, 0x61a000, 0x61a800,
};
static const Uint8 scramblecode_kof2000[7] = {0xEC, 15, 14, 7, 3, 10, 5};

void neo_game_special_init(const char *name)
{
	neo_pvc_reset();
	neogeo_fix_bank_type = 0;
	memory.bksw_handler = 0;
	memory.bksw_unscramble = NULL;
	memory.bksw_offset = NULL;
	memory.sma_rng_addr = 0;

	if (!name || !name[0])
		return;

	/* PVC carts. */
	if (name_is(name, "mslug5") || !strcmp(name, "ms5pcb") ||
	    name_is(name, "svc") || !strcmp(name, "svcsplus") ||
	    name_is(name, "kof2003")) {
		if (!strcmp(name, "ms5pcb") || name_is(name, "svc") ||
		    name_is(name, "kof2003"))
			neogeo_fix_bank_type = 2;
		else
			neogeo_fix_bank_type = 1;
		neo_pvc_install();
		printf("neo: fix_bank_type=%d + PVC (%s)\n",
		       neogeo_fix_bank_type, name);
		return;
	}

	/* SMA protection (bankswitch + RNG) — still needed after P decrypt. */
	if (name_is(name, "kof99") && !name_is(name, "kof99n")) {
		neogeo_fix_bank_type = 0;
		memory.bksw_offset = (int *)bankoffset_kof99;
		memory.bksw_unscramble = (Uint8 *)scramblecode_kof99;
		memory.sma_rng_addr = 0xF8FA;
		printf("neo: SMA kof99 bank_type=0 (%s)\n", name);
		return;
	}
	if (name_is(name, "garou") && !name_is(name, "garoubl")) {
		neogeo_fix_bank_type = 1;
		memory.bksw_offset = (int *)bankoffset_garou;
		memory.bksw_unscramble = (Uint8 *)scramblecode_garou;
		memory.sma_rng_addr = 0xCCF0;
		printf("neo: SMA garou bank_type=1 (%s)\n", name);
		return;
	}
	if (name_is(name, "mslug3") && !strstr(name, "mslug3h") &&
	    !strstr(name, "mslug3n") && !strstr(name, "mslug3b")) {
		neogeo_fix_bank_type = 1;
		memory.bksw_offset = (int *)bankoffset_mslug3;
		memory.bksw_unscramble = (Uint8 *)scramblecode_mslug3;
		memory.sma_rng_addr = 0;
		printf("neo: SMA mslug3 bank_type=1 (%s)\n", name);
		return;
	}
	if (name_is(name, "kof2000") && !strstr(name, "kof2000n")) {
		neogeo_fix_bank_type = 2;
		memory.bksw_offset = (int *)bankoffset_kof2000;
		memory.bksw_unscramble = (Uint8 *)scramblecode_kof2000;
		memory.sma_rng_addr = 0xD8DA;
		printf("neo: SMA kof2000 bank_type=2 (%s)\n", name);
		return;
	}

	if (name_is(name, "matrim") || name_is(name, "kof2000n")) {
		neogeo_fix_bank_type = 2;
		printf("neo: fix_bank_type=2 (%s)\n", name);
		return;
	}

	if (name_is(name, "mslug4") || !strcmp(name, "ms4plus") ||
	    !strncmp(name, "mslug3", 6) || name_is(name, "rotd") ||
	    name_is(name, "pnyaa") || name_is(name, "kof2002") ||
	    name_is(name, "kof2001") || !strncmp(name, "kof99", 5) ||
	    !strncmp(name, "garou", 5) || name_is(name, "sengoku3") ||
	    name_is(name, "ganryu") || name_is(name, "s1945p") ||
	    name_is(name, "preisle2") || name_is(name, "bangbead") ||
	    name_is(name, "nitd") || name_is(name, "zupapa") ||
	    name_is(name, "samsho5") || name_is(name, "samsh5sp") ||
	    !strcmp(name, "ms5plus")) {
		neogeo_fix_bank_type = 1;
		printf("neo: fix_bank_type=1 (%s)\n", name);
	}
}
