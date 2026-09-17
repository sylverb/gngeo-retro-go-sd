#ifndef GNGEO_PLATFORM_H
#define GNGEO_PLATFORM_H

#include <stdint.h>

void neo_present_to_lcd(uint16_t *fb);
void neo_render_audio(int16_t *dst, int len);
/* 0 when G&W volume is 0 / muted — skip YM2610 + Z80 to free CPU. */
int neo_sound_gen_enabled(void);
/* Emu only (Z80/68K/IRQ). Draw is neo_draw_frame() after audio. */
void neo_run_frame(int draw_video);
/* 1 if this frame should blit (respects frameskip + NEO_DISABLE_VIDEO). */
int neo_frame_needs_draw(void);
void neo_draw_frame(void);
#ifndef HOST_BUILD
void neo_bind_lcd_buffer(void);
#endif
void neo_dtcm_ensure(void);

#endif
