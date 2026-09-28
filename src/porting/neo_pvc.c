/*
 * NEO-PVC protection — Metal Slug 5 / SVC / KOF2003.
 * Cart RAM @ $2FE000-$2FFFFF, bankswitch + colour pack/unpack.
 * ROMs must already be decrypted (decrypt_neogeo_zip / gngeo --dump).
 *
 * pvc_ram must not live in .bss (Musashi JT needs 256 KiB after .bss).
 * Allocate late: prefer RAM_EMU after JT; else AHB.
 */
#include <string.h>
#include <stdio.h>

#include "neo_pvc.h"
#include "memory.h"
#include "emu.h"
#include "gw_malloc.h"
#include "neo_mem.h"

extern int neogeo_fix_bank_type;
extern Uint32 bankaddress;
void cpu_68k_bankswitch(Uint32 address);

#define PVC_RAM_WORDS 0x1000u
#define PVC_RAM_BYTES (PVC_RAM_WORDS * sizeof(Uint16))

static Uint16 *pvc_ram;
static int pvc_want;
static int pvc_on;

int neo_pvc_ensure_ram(void)
{
	if (!pvc_want)
		return 1;
	if (pvc_ram) {
		pvc_on = 1;
		return 1;
	}
	pvc_ram = (Uint16 *)ram_calloc(1, PVC_RAM_BYTES);
	if (!neo_alloc_ok(pvc_ram)) {
		printf("neo: ram pvc failed (free=%u), try AHB\n",
		       (unsigned)ram_get_free_size());
		pvc_ram = NULL;
		pvc_ram = (Uint16 *)ahb_calloc(1, PVC_RAM_BYTES);
	}
	if (!neo_alloc_ok(pvc_ram)) {
		printf("neo: FATAL pvc_ram (%u B)\n", (unsigned)PVC_RAM_BYTES);
		pvc_ram = NULL;
		pvc_on = 0;
		return 0;
	}
	pvc_on = 1;
	printf("neo: PVC @ %p (%u B, ram_free=%u ahb_free=%u)\n",
	       (void *)pvc_ram, (unsigned)PVC_RAM_BYTES,
	       (unsigned)ram_get_free_size(), (unsigned)ahb_get_free_size());
	/* Remount $2Fxxxx onto callbacks now that PVC RAM exists. */
	cpu_68k_bankswitch(bankaddress);
	return 1;
}

void neo_pvc_reset(void)
{
	if (pvc_ram)
		memset(pvc_ram, 0, PVC_RAM_BYTES);
	pvc_on = 0;
	pvc_want = 0;
}

int neo_pvc_active(void)
{
	return pvc_on && pvc_ram;
}

int neo_pvc_wanted(void)
{
	return pvc_want;
}

static void pvc_unpack_color(void)
{
	Uint16 pen = pvc_ram[0xff0];
	Uint8 b = (Uint8)(((pen & 0x000f) << 1) | ((pen & 0x1000) >> 12));
	Uint8 g = (Uint8)(((pen & 0x00f0) >> 3) | ((pen & 0x2000) >> 13));
	Uint8 r = (Uint8)(((pen & 0x0f00) >> 7) | ((pen & 0x4000) >> 14));
	Uint8 s = (Uint8)((pen & 0x8000) >> 15);

	pvc_ram[0xff1] = (Uint16)((g << 8) | b);
	pvc_ram[0xff2] = (Uint16)((s << 8) | r);
}

static void pvc_pack_color(void)
{
	Uint16 gb = pvc_ram[0xff4];
	Uint16 sr = pvc_ram[0xff5];

	pvc_ram[0xff6] = (Uint16)(
		((gb & 0x001e) >> 1) |
		((gb & 0x1e00) >> 5) |
		((sr & 0x001e) << 7) |
		((gb & 0x0001) << 12) |
		((gb & 0x0100) << 5) |
		((sr & 0x0001) << 14) |
		((sr & 0x0100) << 7));
}

static void pvc_bankswitch(void)
{
	Uint32 bank = (Uint32)((pvc_ram[0xff8] >> 8) | (pvc_ram[0xff9] << 8));

	pvc_ram[0xff8] = (Uint16)((pvc_ram[0xff8] & 0xfe00) | 0x00a0);
	pvc_ram[0xff9] &= 0x7fff;
	bankaddress = bank + 0x100000u;
	if (bankaddress >= memory.rom.cpu_m68k.size)
		bankaddress = 0x100000u;
	cpu_68k_bankswitch(bankaddress);
}

void neo_pvc_install(void)
{
	pvc_want = 1;
	pvc_on = 0;
	memory.bksw_handler = 1;
	printf("neo: PVC protection armed\n");
}

static inline unsigned pvc_idx(Uint32 addr)
{
	return (unsigned)((addr & 0x1ffeu) >> 1);
}

Uint16 neo_pvc_read_word(Uint32 addr)
{
	if (!pvc_ram)
		return 0;
	return pvc_ram[pvc_idx(addr)];
}

Uint8 neo_pvc_read_byte(Uint32 addr)
{
	Uint16 w;
	if (!pvc_ram)
		return 0;
	w = pvc_ram[pvc_idx(addr)];
	return (addr & 1) ? (Uint8)(w & 0xff) : (Uint8)(w >> 8);
}

void neo_pvc_write_word(Uint32 addr, Uint16 data)
{
	unsigned i;

	if (!pvc_ram)
		return;
	i = pvc_idx(addr);
	pvc_ram[i] = data;
	if (i == 0xff0)
		pvc_unpack_color();
	else if (i >= 0xff4 && i <= 0xff5)
		pvc_pack_color();
	else if (i >= 0xff8)
		pvc_bankswitch();
}

void neo_pvc_write_byte(Uint32 addr, Uint8 data)
{
	unsigned i;
	Uint16 w;

	if (!pvc_ram)
		return;
	i = pvc_idx(addr);
	w = pvc_ram[i];
	if (addr & 1)
		w = (Uint16)((w & 0xff00) | data);
	else
		w = (Uint16)((w & 0x00ff) | ((Uint16)data << 8));
	neo_pvc_write_word(addr & ~1u, w);
}

/* SMA bank tables (from roms.c init_* — roms.c is not linked in this core). */
static int bankoffset_kof99[64] = {
	0x000000, 0x100000, 0x200000, 0x300000, 0x3cc000,
	0x4cc000, 0x3f2000, 0x4f2000, 0x407800, 0x507800, 0x40d000, 0x50d000,
	0x417800, 0x517800, 0x420800, 0x520800, 0x424800, 0x524800, 0x429000,
	0x529000, 0x42e800, 0x52e800, 0x431800, 0x531800, 0x54d000, 0x551000,
	0x567000, 0x592800, 0x588800, 0x581800, 0x599800, 0x594800, 0x598000,
};
static Uint8 scramblecode_kof99[7] = {0xF0, 14, 6, 8, 10, 12, 5};

static int bankoffset_garou[64] = {
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
static Uint8 scramblecode_garou[7] = {0xC0, 5, 9, 7, 6, 14, 12};

static int bankoffset_mslug3[64] = {
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
static Uint8 scramblecode_mslug3[7] = {0xE4, 14, 12, 15, 6, 3, 9};

static int bankoffset_kof2000[64] = {
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
static Uint8 scramblecode_kof2000[7] = {0xEC, 15, 14, 7, 3, 10, 5};

static int name_is(const char *name, const char *prefix)
{
	size_t n = strlen(prefix);
	return name && (!strncmp(name, prefix, n) &&
			(name[n] == '\0' || name[n] == '_' || name[n] == '-'));
}

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

	/* PVC carts (decrypted zip stems may be mslug5_dec). */
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
		memory.bksw_offset = bankoffset_kof99;
		memory.bksw_unscramble = scramblecode_kof99;
		memory.sma_rng_addr = 0xF8FA;
		printf("neo: SMA kof99 bank_type=0 (%s)\n", name);
		return;
	}
	if (name_is(name, "garou") && !name_is(name, "garoubl")) {
		neogeo_fix_bank_type = 1;
		memory.bksw_offset = bankoffset_garou;
		memory.bksw_unscramble = scramblecode_garou;
		memory.sma_rng_addr = 0xCCF0;
		printf("neo: SMA garou bank_type=1 (%s)\n", name);
		return;
	}
	if (name_is(name, "mslug3") && !strstr(name, "mslug3h") &&
	    !strstr(name, "mslug3n") && !strstr(name, "mslug3b")) {
		neogeo_fix_bank_type = 1;
		memory.bksw_offset = bankoffset_mslug3;
		memory.bksw_unscramble = scramblecode_mslug3;
		memory.sma_rng_addr = 0;
		printf("neo: SMA mslug3 bank_type=1 (%s)\n", name);
		return;
	}
	if (name_is(name, "kof2000") && !strstr(name, "kof2000n")) {
		neogeo_fix_bank_type = 2;
		memory.bksw_offset = bankoffset_kof2000;
		memory.bksw_unscramble = scramblecode_kof2000;
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
