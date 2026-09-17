/*
 * Platform glue: SDL surface, screen, pull-audio for Retro-Go SD.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "SDL.h"
#include "config.h"

#define GN_TRUE  1
#define GN_FALSE 0
#include "screen.h"
#include "sound.h"
#include "emu.h"
#include "memory.h"
#include "conf.h"
#include "transpack.h"
#include "ym2610/2610intf.h"
#include "ym2610/ym2610.h"
#include "gw_malloc.h"
#include "neo_mem.h"
#include "gngeo_platform.h"

#ifndef HOST_BUILD
#include "gw_lcd.h"
#include "odroid_audio.h"
#include "gw_core_bridge.h"
#else
#include "odroid_audio.h"
#endif

#ifndef ODROID_AUDIO_VOLUME_MIN
#define ODROID_AUDIO_VOLUME_MIN 0
#endif

/* --- SDL surface helpers ------------------------------------------------ */

static SDL_Surface *alloc_surface(int w, int h)
{
    SDL_Surface *s = calloc(1, sizeof(*s));
    size_t nbytes;
    if (!s)
        return NULL;
    s->w = w;
    s->h = h;
    s->pitch = (Uint16)(w * 2);
    s->format = &s->format_storage;
    s->format->BytesPerPixel = 2;
    s->format->BitsPerPixel = 16;
    s->format->Rmask = 0xF800;
    s->format->Gmask = 0x07E0;
    s->format->Bmask = 0x001F;
    nbytes = (size_t)s->pitch * (size_t)h;
    s->pixels = ram_malloc(nbytes);
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    memset(s->pixels, 0, nbytes);
    s->owned_pixels = 1;
    s->clip_rect.x = 0;
    s->clip_rect.y = 0;
    s->clip_rect.w = (Uint16)w;
    s->clip_rect.h = (Uint16)h;
    return s;
}

#ifndef HOST_BUILD
/* Device: RAM_EMU is full (code+BSS ≈ 700 KiB / 724 KiB). Point the
 * GnGeo soft surface at the firmware LCD framebuffer instead of
 * allocating a 352×256 scratch (≈180 KiB) that will never fit. */
static SDL_Surface *alloc_lcd_surface(void)
{
    SDL_Surface *s = calloc(1, sizeof(*s));
    uint16_t *fb = lcd_get_active_buffer();
    if (!s || !fb) {
        free(s);
        return NULL;
    }
    s->w = 320;
    s->h = 240;
    s->pitch = 640;
    s->format = &s->format_storage;
    s->format->BytesPerPixel = 2;
    s->format->BitsPerPixel = 16;
    s->format->Rmask = 0xF800;
    s->format->Gmask = 0x07E0;
    s->format->Bmask = 0x001F;
    s->pixels = fb;
    s->owned_pixels = 0;
    s->clip_rect.x = 0;
    s->clip_rect.y = 0;
    s->clip_rect.w = 320;
    s->clip_rect.h = 240;
    return s;
}
#endif

int SDL_Init(Uint32 flags) { (void)flags; return 0; }
void SDL_Quit(void) {}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags; (void)depth; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    return alloc_surface(width, height);
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (!surface)
        return;
    /* pixels come from ram bump or LCD — not freed individually */
    free(surface);
}

int SDL_LockSurface(SDL_Surface *surface) { (void)surface; return 0; }
void SDL_UnlockSurface(SDL_Surface *surface) { (void)surface; }

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color)
{
    SDL_Rect r;
    Uint16 *p;
    int x, y;
    if (!dst || !dst->pixels)
        return -1;
    if (dstrect)
        r = *dstrect;
    else {
        r.x = 0; r.y = 0; r.w = (Uint16)dst->w; r.h = (Uint16)dst->h;
    }
    for (y = 0; y < r.h; y++) {
        p = (Uint16 *)((Uint8 *)dst->pixels + (r.y + y) * dst->pitch) + r.x;
        for (x = 0; x < r.w; x++)
            p[x] = (Uint16)color;
    }
    return 0;
}

int SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect,
                    SDL_Surface *dst, SDL_Rect *dstrect)
{
    SDL_Rect s, d;
    int row;
    if (!src || !dst)
        return -1;
    s = srcrect ? *srcrect : (SDL_Rect){0, 0, (Uint16)src->w, (Uint16)src->h};
    d = dstrect ? *dstrect : (SDL_Rect){0, 0, s.w, s.h};
    for (row = 0; row < s.h && (d.y + row) < dst->h; row++) {
        Uint16 *sp = (Uint16 *)((Uint8 *)src->pixels + (s.y + row) * src->pitch) + s.x;
        Uint16 *dp = (Uint16 *)((Uint8 *)dst->pixels + (d.y + row) * dst->pitch) + d.x;
        int w = s.w;
        if (d.x + w > dst->w)
            w = dst->w - d.x;
        if (w > 0)
            memcpy(dp, sp, (size_t)w * 2);
    }
    return 0;
}

Uint32 SDL_MapRGB(SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b)
{
    (void)fmt;
    return (Uint32)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect)
{
    if (!surface)
        return;
    if (!rect) {
        surface->clip_rect.x = 0;
        surface->clip_rect.y = 0;
        surface->clip_rect.w = (Uint16)surface->w;
        surface->clip_rect.h = (Uint16)surface->h;
    } else {
        surface->clip_rect = *rect;
    }
}

int SDL_SetColorKey(SDL_Surface *surface, Uint32 flag, Uint32 key)
{
    (void)surface; (void)flag; (void)key;
    return 0;
}

Uint32 SDL_GetTicks(void)
{
#ifdef HOST_BUILD
    /* Real time provided by host_platform when linked; stub fallback. */
#endif
    return 0;
}

void SDL_Delay(Uint32 ms) { (void)ms; }

void SDL_textout(SDL_Surface *surface, int x, int y, const char *text)
{
    (void)surface; (void)x; (void)y; (void)text;
}

int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }

SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags)
{
    (void)bpp; (void)flags;
    return alloc_surface(w, h);
}

/* --- screen ------------------------------------------------------------- */

char fps_str[32];
TRANS_PACK *tile_trans;

SDL_Surface *screen;
SDL_Surface *buffer, *sprbuf, *fps_buf, *scan, *fontbuf;
SDL_Rect visible_area;
int yscreenpadding;
Uint8 interpolation;
Uint8 nblitter;
Uint8 neffect;
Uint8 scale;
Uint8 fullscreen;

void init_rgb2yuv_table(void) {}

Uint8 get_effect_by_name(char *name) { (void)name; return 0; }
Uint8 get_blitter_by_name(char *name) { (void)name; return 0; }
void print_blitter_list(void) {}
void print_effect_list(void) {}
LIST *create_effect_list(void) { return NULL; }
LIST *create_blitter_list(void) { return NULL; }

void init_sdl(void) {}

int screen_init(void)
{
#ifdef HOST_BUILD
    /* Desktop: classic GnGeo 352×256 scratch in the host RAM pool. */
    buffer = alloc_surface(352, 256);
    screen = buffer;
    if (!buffer)
        return GN_FALSE;
    visible_area.x = 16;
    visible_area.y = 16;
    visible_area.w = 320;
    visible_area.h = 224;
    if (!conf.screen320) {
        visible_area.x = 24;
        visible_area.w = 304;
    }
#else
    /*
     * Device: draw straight into the 320×240 LCD FB. No 16px gutter —
     * that cropped the right edge 16px early vs host's 352-wide buffer.
     * Full 320×224 viewport, 8px letterbox top/bottom only.
     */
    conf.screen320 = 1;
    buffer = alloc_lcd_surface();
    screen = buffer;
    if (!buffer)
        return GN_FALSE;
    visible_area.x = 0;
    visible_area.y = 16;
    visible_area.w = 320;
    visible_area.h = 224;
#endif
    return GN_TRUE;
}

int screen_reinit(void) { return screen_init(); }
int screen_resize(int w, int h) { (void)w; (void)h; return GN_TRUE; }
void screen_update(void) {}
void screen_close(void) {}
void screen_fullscreen(void) {}
void sdl_set_title(char *name) { (void)name; }

#ifndef HOST_BUILD
void neo_bind_lcd_buffer(void)
{
    uint16_t *fb = lcd_get_active_buffer();
    if (buffer && fb)
        buffer->pixels = fb;
}

/* DMA2D solid fill (RGB565). Returns 0 on success. */
static int neo_dma2d_fill(uint16_t *dst, uint16_t w, uint16_t h, uint16_t dst_off)
{
    if (!dst || w == 0 || h == 0)
        return 0;
    if (dma2d_r2m_rgb565_start(0, (uint32_t)(uintptr_t)dst, w, h, dst_off) != 0)
        return -1;
    return dma2d_poll(50) == 0 ? 0 : -1;
}

/* DMA2D RGB565 blit with pitch 320. Returns 0 on success. */
static int neo_dma2d_blit(const uint16_t *src, uint16_t *dst,
                          uint16_t w, uint16_t h)
{
    const uint16_t pitch_skip = (uint16_t)(320 - w);

    if (!src || !dst || w == 0 || h == 0)
        return 0;
    if (dma2d_m2m_rgb565_start_ex((uint32_t)(uintptr_t)src,
                                  (uint32_t)(uintptr_t)dst,
                                  w, h, pitch_skip, pitch_skip) != 0)
        return -1;
    return dma2d_poll(50) == 0 ? 0 : -1;
}

static void neo_cpu_letterbox(uint16_t *fb, int dst_x, int dst_y, int vw, int vh)
{
    int y;

    if (dst_y > 0)
        memset(fb, 0, (size_t)dst_y * 320 * 2);
    if (dst_y + vh < 240)
        memset(fb + (dst_y + vh) * 320, 0,
               (size_t)(240 - dst_y - vh) * 320 * 2);
    for (y = dst_y; y < dst_y + vh; y++) {
        if (dst_x > 0)
            memset(fb + y * 320, 0, (size_t)dst_x * 2);
        if (dst_x + vw < 320)
            memset(fb + y * 320 + dst_x + vw, 0,
                   (size_t)(320 - dst_x - vw) * 2);
    }
}

/* Letterbox bars around a centered vw×vh crop. Prefer DMA2D R2M. */
static void neo_letterbox(uint16_t *fb, int dst_x, int dst_y, int vw, int vh)
{
    int ok = 1;
    const int bot_h = 240 - dst_y - vh;
    const int right_w = 320 - dst_x - vw;

    if (dst_y > 0)
        ok = ok && neo_dma2d_fill(fb, 320, (uint16_t)dst_y, 0) == 0;
    if (ok && bot_h > 0)
        ok = ok && neo_dma2d_fill(fb + (dst_y + vh) * 320, 320,
                                  (uint16_t)bot_h, 0) == 0;
    if (ok && dst_x > 0)
        ok = ok && neo_dma2d_fill(fb + dst_y * 320, (uint16_t)dst_x,
                                  (uint16_t)vh, (uint16_t)(320 - dst_x)) == 0;
    if (ok && right_w > 0)
        ok = ok && neo_dma2d_fill(fb + dst_y * 320 + dst_x + vw,
                                  (uint16_t)right_w, (uint16_t)vh,
                                  (uint16_t)(dst_x + vw)) == 0;
    if (!ok)
        neo_cpu_letterbox(fb, dst_x, dst_y, vw, vh);
}
#endif

/* Blit Neo Geo visible area into the active LCD RGB565 buffer (320x240). */
void neo_present_to_lcd(uint16_t *fb)
{
    int y;
    const int src_x = visible_area.x;
    const int src_y = visible_area.y;
    const int vw = visible_area.w;
    const int vh = visible_area.h;
    const int dst_x = (320 - vw) / 2;
    const int dst_y = (240 - vh) / 2;

    if (!buffer || !buffer->pixels || !fb)
        return;

#ifndef HOST_BUILD
    /*
     * Device draws into the LCD back-buffer. When the crop already matches
     * the panel (full 320 wide), only letterbox the 224→240 vertical gap.
     * Avoid a no-op memmove that used to shuffle a 304-wide gutter crop.
     */
    if (buffer->pixels == (void *)fb) {
        if (src_x != dst_x || src_y != dst_y) {
            if (dst_y < src_y) {
                for (y = 0; y < vh; y++) {
                    uint16_t *row = fb + (src_y + y) * 320 + src_x;
                    memmove(fb + (dst_y + y) * 320 + dst_x, row, (size_t)vw * 2);
                }
            } else {
                for (y = vh - 1; y >= 0; y--) {
                    uint16_t *row = fb + (src_y + y) * 320 + src_x;
                    memmove(fb + (dst_y + y) * 320 + dst_x, row, (size_t)vw * 2);
                }
            }
        }
        neo_letterbox(fb, dst_x, dst_y, vw, vh);
        return;
    }

    {
        const uint16_t *src = (const uint16_t *)buffer->pixels +
                              src_y * 320 + src_x;
        uint16_t *dst = fb + dst_y * 320 + dst_x;

        if (neo_dma2d_fill(fb, 320, 240, 0) != 0)
            memset(fb, 0, 320 * 240 * 2);
        if (neo_dma2d_blit(src, dst, (uint16_t)vw, (uint16_t)vh) != 0) {
            for (y = 0; y < vh; y++) {
                const Uint16 *s =
                    (const Uint16 *)((const Uint8 *)buffer->pixels +
                                     (src_y + y) * buffer->pitch) + src_x;
                memcpy(fb + (dst_y + y) * 320 + dst_x, s, (size_t)vw * 2);
            }
        }
        return;
    }
#endif

    memset(fb, 0, 320 * 240 * 2);
    for (y = 0; y < vh; y++) {
        const Uint16 *src = (const Uint16 *)((const Uint8 *)buffer->pixels +
                                             (src_y + y) * buffer->pitch) + src_x;
        Uint16 *dst = fb + (dst_y + y) * 320 + dst_x;
        memcpy(dst, src, (size_t)vw * 2);
    }
}

/* --- sound (pull model) ------------------------------------------------- */

/* Stereo scratch for one audio half — 18 kHz/60 ≈ 300 frames → 600 samples. */
#define BUFFER_LEN 1024
Uint16 *play_buffer;

static int audio_paused = 1;

int init_sdl_audio(void)
{
    neo_dtcm_ensure();
    if (!play_buffer) {
        play_buffer = dtc_malloc(BUFFER_LEN * sizeof(Uint16));
        if (!neo_alloc_ok(play_buffer)) {
            printf("neogeo: FATAL DTCM play_buffer\n");
            return GN_FALSE;
        }
        printf("neogeo: DTCM play_buffer=%p free=%u\n",
               (void *)play_buffer, (unsigned)dtc_get_free_size());
    }
    audio_paused = 0;
    return GN_TRUE;
}

void close_sdl_audio(void) { audio_paused = 1; }
void pause_audio(int on) { audio_paused = on; }

int neo_sound_gen_enabled(void)
{
    if (audio_paused || !conf.sound || NEO_DISABLE_YM2610)
        return 0;
    /* Volume 0: skip YM2610 + frame Z80 (big CPU win).
     * 68K→Z80 command path in memory.c still runs short slices. */
    if (odroid_audio_volume_get() == ODROID_AUDIO_VOLUME_MIN)
        return 0;
    return 1;
}

/* Generate `len` mono samples into dst (int16).
 * YM2610Update_stream writes interleaved stereo into play_buffer. */
void neo_render_audio(int16_t *dst, int len)
{
    int i;
    int n = len;

    if (!dst || n <= 0)
        return;
    if (!neo_sound_gen_enabled()) {
        memset(dst, 0, (size_t)n * sizeof(int16_t));
        return;
    }
    if (n > (BUFFER_LEN / 2))
        n = BUFFER_LEN / 2;

    YM2610Update_stream(n);
    for (i = 0; i < n; i++) {
        int32_t l = (int16_t)play_buffer[i * 2];
        int32_t r = (int16_t)play_buffer[i * 2 + 1];
        int32_t s = (l + r) >> 1;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        dst[i] = (int16_t)s;
    }
    for (; i < len; i++)
        dst[i] = 0;
}
