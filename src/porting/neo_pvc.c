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

Uint16 *neo_pvc_ram_ptr(void)
{
	return pvc_ram;
}

Uint32 neo_pvc_ram_bytes(void)
{
	return pvc_want ? (Uint32)PVC_RAM_BYTES : 0;
}

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
