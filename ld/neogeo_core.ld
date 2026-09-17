/*
 * Neo Geo (GnGeo) core — RAM_EMU + ITCM hot segment.
 *
 * ITCM (.text only, 64 KiB): soft LSPC (video.o) + MAME Z80 + YM2610.
 * Musashi handlers stay in RAM_EMU (~119 KiB .text — do not fit).
 *
 * Budget note: after .bss, RAM_EMU must keep ≥256 KiB free for the Musashi
 * opcode jump table (ram_malloc). Do not grow .core_entry/.bss without
 * checking (__RAM_EMU_END__ - __CORE_BSS_END__).
 *
 * Hot data (m68ki_cycles, m68k core, play_buffer, …) uses dtc_* DTCM
 * bump at runtime — pack_core rejects DTCM segments.
 */

INCLUDE gnw_ram_emu.ld
INCLUDE gnw_itcm_core.ld

MEMORY
{
  RAM_EMU (xrw) : ORIGIN = __RAM_EMU_START__, LENGTH = __RAM_EMU_LENGTH__
  ITCM    (xrw) : ORIGIN = __ITCM_CORE_START__, LENGTH = __ITCM_CORE_LENGTH__
}

/* Object paths match BUILD_DIR=build/core (flat names under that dir). */

SECTIONS
{
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))

    . = ALIGN(4);
    EXCLUDE_FILE(
      build/core/video.o
      build/core/z80.o
      build/core/ym2610.o
    ) *(.text*)
    *(.rodata*)
    *(.glue_7)
    *(.glue_7t)

    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;
  } >RAM_EMU

  .data :
  {
    *(.data*)
  } >RAM_EMU

  __CORE_CODE_END__ = .;

  .bss (NOLOAD) :
  {
    . = ALIGN(8);
    *(.bss*)
    *(COMMON)
    . = ALIGN(8);
  } >RAM_EMU

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: Neo Geo core RAM_EMU segment overflows RAM_EMU")

  .core_itcm __ITCM_CORE_START__ :
  {
    __CORE_ITCM_CODE_START__ = .;
    build/core/video.o(.text*)
    build/core/z80.o(.text*)
    build/core/ym2610.o(.text*)
    . = ALIGN(4);
    __CORE_ITCM_CODE_END__ = .;
  } >ITCM AT>RAM_EMU

  __CORE_ITCM_BSS_END__ = __CORE_ITCM_CODE_END__;

  ASSERT(__CORE_ITCM_BSS_END__ <= __ITCM_CORE_START__ + __ITCM_CORE_LENGTH__,
         "Error: Neo Geo core ITCM segment overflows the 64K ITCM region")

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
