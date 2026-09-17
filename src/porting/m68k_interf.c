/*
 * Musashi/gwenesis M68K wrapper — GnGeo cpu_68k_* API.
 * Memory dispatch is a switch (no 96 KiB fetch tables).
 * Opcode jump table is built once and cached in external flash.
 */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "config.h"
#include "m68k/m68k.h"
#include "memory.h"
#include "emu.h"
#include "conf.h"
#include "gnutil.h"
#include "gno_flash.h"
#include "state.h"
#include "gw_malloc.h"
#include "neo_mem.h"
#include "gngeo_platform.h"

#ifndef HOST_BUILD
#include "gw_lcd.h"
#include "gw_core_bridge.h"
#endif

extern Uint32 bankaddress;
extern void (**m68ki_instruction_jump_table)(void);
extern unsigned char *m68ki_cycles;

static Uint32 cycles_used;
static Uint32 frame_cycle_base;

static unsigned int neo_read8(unsigned int address)
{
    address &= 0xFFFFFF;
    switch (address >> 16) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        return mem68k_fetch_cpu_byte(address);
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        return mem68k_fetch_ram_byte(address);
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        return mem68k_fetch_bk_normal_byte(address);
    case 0x30: return mem68k_fetch_ctl1_byte(address);
    case 0x32: return mem68k_fetch_coin_byte(address);
    case 0x34: return mem68k_fetch_ctl2_byte(address);
    case 0x38: return mem68k_fetch_ctl3_byte(address);
    case 0x3c: return mem68k_fetch_video_byte(address);
    case 0x40: case 0x41: return mem68k_fetch_pal_byte(address);
    case 0x80: return mem68k_fetch_memcrd_byte(address);
    /* BIOS mirrors across 0xC00000-0xCFFFFF (128 KiB wrap) — UniBIOS checksum. */
    case 0xc0: case 0xc1: case 0xc2: case 0xc3:
    case 0xc4: case 0xc5: case 0xc6: case 0xc7:
    case 0xc8: case 0xc9: case 0xca: case 0xcb:
    case 0xcc: case 0xcd: case 0xce: case 0xcf:
        return mem68k_fetch_bios_byte(address);
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
    case 0xd4: case 0xd5: case 0xd6: case 0xd7:
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
        return mem68k_fetch_sram_byte(address);
    default: return mem68k_fetch_invalid_byte(address);
    }
}

static unsigned int neo_read16(unsigned int address)
{
    address &= 0xFFFFFF;
    switch (address >> 16) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        return mem68k_fetch_cpu_word(address);
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        return mem68k_fetch_ram_word(address);
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        return mem68k_fetch_bk_normal_word(address);
    case 0x30: return mem68k_fetch_ctl1_word(address);
    case 0x32: return mem68k_fetch_coin_word(address);
    case 0x34: return mem68k_fetch_ctl2_word(address);
    case 0x38: return mem68k_fetch_ctl3_word(address);
    case 0x3c: return mem68k_fetch_video_word(address);
    case 0x40: case 0x41: return mem68k_fetch_pal_word(address);
    case 0x80: return mem68k_fetch_memcrd_word(address);
    case 0xc0: case 0xc1: case 0xc2: case 0xc3:
    case 0xc4: case 0xc5: case 0xc6: case 0xc7:
    case 0xc8: case 0xc9: case 0xca: case 0xcb:
    case 0xcc: case 0xcd: case 0xce: case 0xcf:
        return mem68k_fetch_bios_word(address);
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
    case 0xd4: case 0xd5: case 0xd6: case 0xd7:
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
        return mem68k_fetch_sram_word(address);
    default: return mem68k_fetch_invalid_word(address);
    }
}

static void neo_write8(unsigned int address, unsigned int data)
{
    address &= 0xFFFFFF;
    switch (address >> 16) {
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        mem68k_store_ram_byte(address, (Uint8)data); break;
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        mem68k_store_bk_normal_byte(address, (Uint8)data); break;
    case 0x32: mem68k_store_z80_byte(address, (Uint8)data); break;
    case 0x38: mem68k_store_pd4990_byte(address, (Uint8)data); break;
    case 0x3a: mem68k_store_setting_byte(address, (Uint8)data); break;
    case 0x3c: mem68k_store_video_byte(address, (Uint8)data); break;
    case 0x40: case 0x41: mem68k_store_pal_byte(address, (Uint8)data); break;
    case 0x80: mem68k_store_memcrd_byte(address, (Uint8)data); break;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
    case 0xd4: case 0xd5: case 0xd6: case 0xd7:
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
        mem68k_store_sram_byte(address, (Uint8)data); break;
    default: mem68k_store_invalid_byte(address, (Uint8)data); break;
    }
}

static void neo_write16(unsigned int address, unsigned int data)
{
    address &= 0xFFFFFF;
    switch (address >> 16) {
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        mem68k_store_ram_word(address, (Uint16)data); break;
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        mem68k_store_bk_normal_word(address, (Uint16)data); break;
    case 0x32: mem68k_store_z80_word(address, (Uint16)data); break;
    case 0x38: mem68k_store_pd4990_word(address, (Uint16)data); break;
    case 0x3a: mem68k_store_setting_word(address, (Uint16)data); break;
    case 0x3c: mem68k_store_video_word(address, (Uint16)data); break;
    case 0x40: case 0x41: mem68k_store_pal_word(address, (Uint16)data); break;
    case 0x80: mem68k_store_memcrd_word(address, (Uint16)data); break;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
    case 0xd4: case 0xd5: case 0xd6: case 0xd7:
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf:
        mem68k_store_sram_word(address, (Uint16)data); break;
    default: mem68k_store_invalid_word(address, (Uint16)data); break;
    }
}

int mem68k_init(void) { return 0; }

void cpu_68k_bankswitch(Uint32 address) { bankaddress = address; }
void cpu_68k_reset(void) { m68k_pulse_reset(); }
void cpu_68k_mkstate(gzFile gzf, int mode) { (void)gzf; (void)mode; }

/*
 * Prepare Musashi opcode jump table (256 KiB of absolute fn pointers) in
 * RAM_EMU via ram_malloc. Returns 0 = fail, 1 = scratch ready (caller builds).
 */
int neo_m68k_prepare_jump_table(void)
{
    const uint32_t jt_bytes = 0x10000u * sizeof(void *);
    void *scratch;
    size_t free_b;

#ifndef HOST_BUILD
    wdog_refresh();
#endif
    free_b = ram_get_free_size();
    if (free_b < jt_bytes) {
        printf("m68k: JT need %lu have %u\n",
               (unsigned long)jt_bytes, (unsigned)free_b);
        return 0;
    }
    scratch = ram_malloc(jt_bytes);
    if (!neo_alloc_ok(scratch)) {
        printf("m68k: JT ram_malloc failed\n");
        return 0;
    }
    m68ki_instruction_jump_table = (void (**)(void))scratch;
    printf("m68k: JT in RAM_EMU @ %p (%lu B, free left %u)\n",
           scratch, (unsigned long)jt_bytes, (unsigned)ram_get_free_size());
    return 1;
}

void neo_m68k_persist_jump_table(void)
{
    /* JT stays in RAM_EMU for the session — nothing to persist. */
}

static void setup_memory_map(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        m68k.memory_map[i].base = (unsigned char *)memory.ram;
        m68k.memory_map[i].read8 = neo_read8;
        m68k.memory_map[i].read16 = neo_read16;
        m68k.memory_map[i].write8 = neo_write8;
        m68k.memory_map[i].write16 = neo_write16;
    }
}

void cpu_68k_init(void)
{
    printf("MUSASHI 68K INIT\n");
    /* Hot 68k state in DTCM (~70 KiB): core + per-opcode cycle table. */
    {
        static int hot_ready;
        if (!hot_ready) {
            neo_dtcm_ensure();
            m68k_core = dtc_calloc(1, sizeof(*m68k_core));
            m68ki_cycles = dtc_malloc(0x10000);
            if (!neo_alloc_ok(m68k_core) || !neo_alloc_ok(m68ki_cycles)) {
                printf("m68k: FATAL DTCM alloc (core=%p cycles=%p)\n",
                       (void *)m68k_core, (void *)m68ki_cycles);
                return;
            }
            hot_ready = 1;
            printf("m68k: DTCM core=%p cycles=%p free=%u\n",
                   (void *)m68k_core, (void *)m68ki_cycles,
                   (unsigned)dtc_get_free_size());
        }
    }
#ifndef HOST_BUILD
    wdog_refresh();
#endif
    mem68k_init();
#ifndef HOST_BUILD
    wdog_refresh();
#endif
    m68k_init();
#ifndef HOST_BUILD
    wdog_refresh();
#endif
    neo_m68k_persist_jump_table();
#ifndef HOST_BUILD
    {
        uintptr_t jp = (uintptr_t)m68ki_instruction_jump_table;
        /* JT must live in RAM_EMU bump (handlers still in .text / ITCM). */
        if (!m68ki_instruction_jump_table ||
            jp < 0x2404b000u || jp >= 0x24100000u) {
            printf("m68k: FATAL JT not in RAM_EMU (ptr=%p) — abort 68k\n",
                   (void *)jp);
            return;
        }
    }
    lcd_clear_buffers();
    wdog_refresh();
#else
    if (!m68ki_instruction_jump_table) {
        printf("m68k: FATAL JT null — abort 68k\n");
        return;
    }
#endif
    setup_memory_map();
    bankaddress = 0;
    m68k.cycles = 0;
    m68k_pulse_reset();
}

int cpu_68k_run(Uint32 nb_cycle)
{
    unsigned int start = m68k.cycles;
    frame_cycle_base = start;
    m68k_run(start + nb_cycle);
    cycles_used = m68k.cycles - start;
    return (int)(m68k.cycles - (start + nb_cycle));
}

Uint32 cpu_68k_getpc(void) { return (Uint32)m68k.pc; }

int cpu_68k_run_step(void)
{
    unsigned int start = m68k.cycles;
    frame_cycle_base = start;
    m68k_run(start + 4);
    return (int)(m68k.cycles - start);
}

int cpu_68k_debuger(void (*execstep)(void), void (*dump)(void))
{
    (void)execstep; (void)dump; return -1;
}

void cpu_68k_interrupt(int a) { m68k_set_irq((unsigned int)a); }
/* Live cycles in the current timeslice — BIOS polls raster via 0x3C0006. */
int cpu_68k_getcycle(void) { return (int)(m68k.cycles - frame_cycle_base); }
void cpu_68k_disassemble(int pc, int nb_instr) { (void)pc; (void)nb_instr; }
void cpu_68k_dumpreg(void) {}
