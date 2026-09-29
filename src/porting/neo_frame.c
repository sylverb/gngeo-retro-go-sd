/* Single-frame Neo Geo step for Retro-Go main loop (replaces main_loop). */
#include "emu.h"
#include "memory.h"
#include "video.h"
#include "sound.h"
#include "timer.h"
#include "pd4990a.h"
#include "frame_skip.h"
#include "conf.h"
#include "profiler.h"
#include "gngeo_platform.h"
#include "config.h"

#ifndef NEO_DISABLE_VIDEO
#define NEO_DISABLE_VIDEO 0
#endif

#ifndef HOST_BUILD
#include "gw_core_bridge.h"
#endif

extern int nb_interlace;
extern char skip_next_frame;

static int fc;
static int skip_this_frame;
static int want_draw;
static Uint32 tm_cycle;
static int neo_int1_bios_frames; /* consecutive frames with $10FD80 bit7 */
static int neo_softlock_frames;

static int neo_frame_irq(void)
{
    /* UniBIOS hardware test / RTC need retrace ticks. */
    pd4990a_addretrace();

    if (!(memory.vid.irq2control & 0x8)) {
        if (fc >= (int)neogeo_frame_counter_speed) {
            fc = 0;
            neogeo_frame_counter++;
        }
        fc++;
    }

    skip_this_frame = skip_next_frame;
    skip_next_frame = frame_skip(0);
    /* draw_screen is deferred to neo_draw_frame() so submit_audio can run
     * first — a slow blit must not starve the DMA half-buffer. */
    return 1;
}

/* Detect mslug5-style soft-lock: cart frame wait while BIOS owns INT1. */
static void neo_trace_int1_owner(void)
{
    Uint8 fd80 = memory.ram[0xfd80 ^ 1];
    Uint32 pc = cpu_68k_getpc() & 0xffffffu;
    Uint8 ctr = memory.ram[0x16 ^ 1];
    Uint8 tgt = memory.ram[0x17 ^ 1];

    if (fd80 & 0x80) {
        neo_int1_bios_frames++;
        if (neo_int1_bios_frames == 1 || neo_int1_bios_frames == 60 ||
            (neo_int1_bios_frames % 300) == 0)
            printf("neo: INT1=BIOS fd80=%02x pc=%06x (%d frames)\n",
                   fd80, pc, neo_int1_bios_frames);
    } else {
        if (neo_int1_bios_frames >= 60)
            printf("neo: INT1=CART again after %d BIOS frames pc=%06x\n",
                   neo_int1_bios_frames, pc);
        neo_int1_bios_frames = 0;
    }

    /* Busy-wait near $EFx with bit7 set and ctr < tgt → soft-lock. */
    if ((fd80 & 0x80) && pc >= 0xef0 && pc < 0xf00 && ctr < tgt) {
        neo_softlock_frames++;
        if (neo_softlock_frames == 30 || neo_softlock_frames == 120)
            printf("neo: softlock? wait pc=%06x fd80=%02x ctr=%02x/%02x\n",
                   pc, fd80, ctr, tgt);
    } else {
        neo_softlock_frames = 0;
    }
}

void neo_run_frame(int draw_video)
{
    int i;
    int a;
    Uint32 cpu_68k_timeslice = 200000;
    Uint32 cpu_z80_timeslice = 73333;
    Uint32 cpu_z80_timeslice_interlace = cpu_z80_timeslice / (Uint32)nb_interlace;

    want_draw = draw_video && !NEO_DISABLE_VIDEO;

    if (conf.test_switch == 1)
        conf.test_switch = 0;

    if (neo_sound_gen_enabled()) {
        PROFILER_START(PROF_Z80);
        for (i = 0; i < nb_interlace; i++) {
            cpu_z80_run((int)cpu_z80_timeslice_interlace);
            my_timer();
#ifndef HOST_BUILD
            if ((i & 7) == 0)
                wdog_refresh();
#endif
        }
        PROFILER_STOP(PROF_Z80);
    }

    PROFILER_START(PROF_68K);
    tm_cycle = cpu_68k_run(cpu_68k_timeslice - tm_cycle);
    PROFILER_STOP(PROF_68K);
    neo_trace_int1_owner();
    a = neo_frame_irq();

    memory.watchdog++;
    if (memory.watchdog > 60) {
        memory.watchdog = 0;
        cpu_68k_reset();
    }

    if (a)
        cpu_68k_interrupt(a);
}

int neo_frame_needs_draw(void)
{
    return want_draw && !skip_this_frame && !NEO_DISABLE_VIDEO;
}

void neo_draw_frame(void)
{
    if (!neo_frame_needs_draw())
        return;
    PROFILER_START(PROF_VIDEO);
    draw_screen();
    PROFILER_STOP(PROF_VIDEO);
}
