/* GnGeo build config for Retro-Go SD (freestanding Cortex-M7). */
#ifndef GNGEO_CONFIG_H
#define GNGEO_CONFIG_H

#define HAVE_CONFIG_H 1

/* Musashi + gngeo --dump / UniBIOS on disk: program ROMs are LE word order.
 * Do NOT define USE_GENERATOR68K (that assumes BE bytes + SWAP16). */
#define USE_MUSASHI68K 1
#define USE_MAMEZ80 1

/* Prefer portable C blitters (video_template.h). Upstream video_arm.S is
 * not vendored here; enabling PROCESSOR_ARM would leave unresolved
 * draw_tile_arm_* / draw_one_char_arm and hardcode a 352-wide pitch. */
/* #define PROCESSOR_ARM 1 */
#define IS_LITTLE_ENDIAN 1
/* Do not define WORDS_BIGENDIAN */

#define GN_TRUE  1
#define GN_FALSE 0

#define ROOTPATH ""

/* Embedded: no zlib / unzip / resfile / menu / GP2X / 940T */
#undef HAVE_LIBZ
#undef ENABLE_940T
#undef GP2X
#undef ENABLE_PROFILER
#undef HAVE_NASM
#undef USE_OSS

#define DATA_DIRECTORY ""

/*
 * Soft-buffer X origin for sprites/fix.
 * Host keeps classic GnGeo 352×256 with a 16px gutter (visible starts at 16).
 * G&W draws into the 320×240 LCD FB — gutter would steal 16px and crop the
 * right edge early, so draw at x=0 with a full 320-wide viewport.
 */
#ifdef HOST_BUILD
#define NEO_SCR_XOFF 16
#else
#define NEO_SCR_XOFF 0
#endif

/*
 * NEO_DISABLE_VIDEO — skip draw_screen / LCD present; Z80 + YM + audio keep
 * running. Use to A/B whether voice glitches track frameskip / video load:
 *   make CORE_C_DEFS+='-DNEO_DISABLE_VIDEO=1'
 * Default 0 (video on). Set 1 to A/B audio without draw load.
 */
#ifndef NEO_DISABLE_VIDEO
#define NEO_DISABLE_VIDEO 0
#endif

/*
 * NEO_DISABLE_YM2610 — skip YM2610Update_stream (silence). Z80 still runs so
 * games keep talking to the chip; use this to measure FM/ADPCM CPU cost.
 * Override with -DNEO_DISABLE_YM2610=1 in CORE_C_DEFS / make host.
 */
#ifndef NEO_DISABLE_YM2610
#define NEO_DISABLE_YM2610 0
#endif

/*
 * NEO_YM_EARLYOUT — shared host/device YM2610Update_stream shortcuts:
 * skip LFO/SSG when idle, skip chan_calc for EG_OFF FM channels.
 * ADPCM paths stay full (voices). Disable with -DNEO_YM_EARLYOUT=0 to A/B.
 */
#ifndef NEO_YM_EARLYOUT
#define NEO_YM_EARLYOUT 1
#endif

/*
 * Z80 / YM timer slices per video frame (stock GnGeo = 256). Lower = less
 * CPU, slightly coarser audio timers. 16 is a good G&W default; override
 * with -DNEO_Z80_SLICES=32 if a title needs tighter timer accuracy.
 */
#ifndef NEO_Z80_SLICES
#define NEO_Z80_SLICES 16
#endif

#endif
