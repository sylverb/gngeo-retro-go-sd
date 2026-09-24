/*
 * Neo Geo (GnGeo) core — RAM_EMU + ITCM hot + flash cold sidecar.
 *
 * ITCM (.text only, 64 KiB): soft LSPC (video.o) + MAME Z80 + YM2610 hot path.
 * Musashi handlers stay in RAM_EMU.
 *
 * Cold code/rodata (init, savestate, gno loader, crypt, large tables) is
 * linked at sentinel VMA 0xCAFE0000 and shipped as /cores/neogeo.ro. At
 * boot, main_neogeo maps that file into the QSPI cache and rebases every
 * absolute 0xCAFE… pointer in RAM_EMU (same idea as zelda3.ro). ITCM must
 * never call into the flash blob.
 *
 * Budget: after .bss, RAM_EMU must keep ≥256 KiB free for the Musashi
 * opcode jump table (ram_malloc).
 */

INCLUDE gnw_ram_emu.ld
INCLUDE gnw_itcm_core.ld

MEMORY
{
  RAM_EMU    (xrw) : ORIGIN = __RAM_EMU_START__, LENGTH = __RAM_EMU_LENGTH__
  ITCM       (xrw) : ORIGIN = __ITCM_CORE_START__, LENGTH = __ITCM_CORE_LENGTH__
  /* Sentinel VMA only — payload extracted to neogeo.ro, not packed in CORE. */
  FLASH_COLD (rx)  : ORIGIN = 0xCAFE0000,         LENGTH = 0xFFFFFFFF
}

PHDRS
{
  text  PT_LOAD FLAGS(5); /* PF_R | PF_X */
  data  PT_LOAD FLAGS(6); /* PF_R | PF_W */
  itcm  PT_LOAD FLAGS(5); /* PF_R | PF_X — LMA in RAM_EMU for pack_core */
  flash PT_LOAD FLAGS(5); /* PF_R | PF_X — CAFE sidecar, not packed in CORE */
}

/* Object paths match BUILD_DIR=build/core (flat names under that dir). */

SECTIONS
{
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))

    . = ALIGN(4);
    /* Entry helpers + cold-flash patcher stay in RAM; patcher excluded from
     * the CAFE rewrite walk via _NEO_FLASH_RO_CODE_START/_END. */
    _NEO_FLASH_RO_CODE_START = .;
    *neo_flash_ro.o (.text* .rodata* .data*)
    . = ALIGN(4);
    _NEO_FLASH_RO_CODE_END = .;

    . = ALIGN(4);
    /* Hot/normal .text — cold objects excluded (they live in .neo_flash). */
    EXCLUDE_FILE(
      build/core/video.o
      build/core/z80.o
      build/core/ym2610.o
      build/core/neo_state.o
      build/core/gno_flash.o
      build/core/neocrypt.o
      build/core/list.o
      build/core/transpack.o
      build/core/frame_skip.o
      build/core/conf_stub.o
      build/core/gnutil_stub.o
    ) *(.text*)

    /* Keep bridge / hot .rodata here; cold .rodata → .neo_flash. */
    EXCLUDE_FILE(
      build/core/neo_state.o
      build/core/gno_flash.o
      build/core/neocrypt.o
      build/core/list.o
      build/core/transpack.o
      build/core/frame_skip.o
      build/core/conf_stub.o
      build/core/gnutil_stub.o
    ) *(.rodata*)

    *(.glue_7)
    *(.glue_7t)

    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;
  } >RAM_EMU :text

  .data :
  {
    *(.data*)
  } >RAM_EMU :data

  __CORE_CODE_END__ = .;

  .bss (NOLOAD) :
  {
    . = ALIGN(8);
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
  } >RAM_EMU :data

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: Neo Geo core RAM_EMU segment overflows RAM_EMU")

  .core_itcm __ITCM_CORE_START__ :
  {
    __CORE_ITCM_CODE_START__ = .;
    /* .text* only — ym2610_mkstate lives in .neo_flash, not here. */
    build/core/video.o(.text*)
    build/core/z80.o(.text*)
    build/core/ym2610.o(.text*)
    . = ALIGN(4);
    __CORE_ITCM_CODE_END__ = .;
  } >ITCM AT>RAM_EMU :itcm

  __CORE_ITCM_BSS_END__ = __CORE_ITCM_CODE_END__;

  ASSERT(__CORE_ITCM_BSS_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
         "Error: Neo Geo core ITCM segment overflows the 64K ITCM region")

  /* Cold code + rodata → /cores/neogeo.ro (QSPI XIP after cache + patch). */
  .neo_flash :
  {
    . = ALIGN(4);
    __neo_flash_start__ = .;
    *(.neo_flash)
    *(.neo_flash.*)
    build/core/neo_state.o(.text* .rodata*)
    build/core/gno_flash.o(.text* .rodata*)
    build/core/neocrypt.o(.text* .rodata*)
    build/core/list.o(.text* .rodata*)
    build/core/transpack.o(.text* .rodata*)
    build/core/frame_skip.o(.text* .rodata*)
    build/core/conf_stub.o(.text* .rodata*)
    build/core/gnutil_stub.o(.text* .rodata*)
    /* Explicit .neo_flash pieces from hot objects (e.g. ym2610_mkstate). */
    build/core/ym2610.o(.neo_flash .neo_flash.*)
    build/core/m68kcpu.o(.neo_flash .neo_flash.*)
    . = ALIGN(4);
    __neo_flash_end__ = .;
  } >FLASH_COLD :flash

  __exidx_start = .;
  __exidx_end = .;

  /DISCARD/ :
  {
    *(.fini_array*)
    *(.preinit_array*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.ARM.attributes)
  }
}
