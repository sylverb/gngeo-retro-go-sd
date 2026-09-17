/* Minimal SDL stand-in for GnGeo on Retro-Go SD. */
#ifndef SDL_H
#define SDL_H

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "SDL_types.h"
#include "SDL_endian.h"

#define SDL_SWSURFACE 0
#define SDL_HWSURFACE 0
#define SDL_FULLSCREEN 0
#define SDL_RESIZABLE 0
#define SDL_INIT_VIDEO 0
#define SDL_INIT_AUDIO 0
#define SDL_INIT_JOYSTICK 0
#define SDL_INIT_TIMER 0

#define SDL_ALPHA_OPAQUE 255

typedef struct SDL_Rect {
    Sint16 x, y;
    Uint16 w, h;
} SDL_Rect;

typedef struct SDL_Color {
    Uint8 r, g, b, unused;
} SDL_Color;

typedef struct SDL_PixelFormat {
    Uint8 BytesPerPixel;
    Uint8 BitsPerPixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
    Uint8 Rshift, Gshift, Bshift, Ashift;
} SDL_PixelFormat;

typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h;
    Uint16 pitch;
    void *pixels;
    SDL_Rect clip_rect;
    int refcount;
    /* private */
    SDL_PixelFormat format_storage;
    int owned_pixels;
} SDL_Surface;

typedef struct SDL_AudioSpec {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint32 size;
    void (*callback)(void *userdata, Uint8 *stream, int len);
    void *userdata;
} SDL_AudioSpec;

typedef struct SDL_Joystick SDL_Joystick;

typedef enum {
    SDL_NOEVENT = 0,
    SDL_KEYDOWN,
    SDL_KEYUP,
    SDL_QUIT,
    SDL_JOYAXISMOTION,
    SDL_JOYBUTTONDOWN,
    SDL_JOYBUTTONUP,
    SDL_JOYHATMOTION,
    SDL_VIDEORESIZE,
    SDL_USEREVENT
} SDL_EventType;

typedef struct SDL_Event {
    Uint8 type;
    int padding;
} SDL_Event;

enum {
    SDLK_UNKNOWN = 0,
    SDLK_ESCAPE,
    SDLK_RETURN,
    SDLK_UP,
    SDLK_DOWN,
    SDLK_LEFT,
    SDLK_RIGHT,
    SDLK_a,
    SDLK_b,
    SDLK_c,
    SDLK_d,
    SDLK_LAST = 512
};

#define SDL_HAT_CENTERED  0
#define SDL_HAT_UP        1
#define SDL_HAT_RIGHT     2
#define SDL_HAT_DOWN      4
#define SDL_HAT_LEFT      8
#define SDL_HAT_RIGHTUP   3
#define SDL_HAT_RIGHTDOWN 6
#define SDL_HAT_LEFTUP    9
#define SDL_HAT_LEFTDOWN  12

#define AUDIO_S16SYS 0x8010

/*
 * On the desktop host build we link real libSDL2 for the window/audio
 * frontend. Rename the GnGeo soft-SDL stand-ins so they do not collide.
 */
#ifdef HOST_BUILD
#define SDL_Init              gngeo_SDL_Init
#define SDL_Quit              gngeo_SDL_Quit
#define SDL_CreateRGBSurface  gngeo_SDL_CreateRGBSurface
#define SDL_FreeSurface       gngeo_SDL_FreeSurface
#define SDL_LockSurface       gngeo_SDL_LockSurface
#define SDL_UnlockSurface     gngeo_SDL_UnlockSurface
#define SDL_FillRect          gngeo_SDL_FillRect
#define SDL_BlitSurface       gngeo_SDL_BlitSurface
#define SDL_SetClipRect       gngeo_SDL_SetClipRect
#define SDL_MapRGB            gngeo_SDL_MapRGB
#define SDL_GetError          gngeo_SDL_GetError
#define SDL_Delay             gngeo_SDL_Delay
#define SDL_GetTicks          gngeo_SDL_GetTicks
#define SDL_PollEvent         gngeo_SDL_PollEvent
#define SDL_SaveBMP           gngeo_SDL_SaveBMP
#define SDL_OpenAudio         gngeo_SDL_OpenAudio
#define SDL_CloseAudio        gngeo_SDL_CloseAudio
#define SDL_PauseAudio        gngeo_SDL_PauseAudio
#define SDL_LockAudio         gngeo_SDL_LockAudio
#define SDL_UnlockAudio       gngeo_SDL_UnlockAudio
#define SDL_WM_SetCaption     gngeo_SDL_WM_SetCaption
#define SDL_SetVideoMode      gngeo_SDL_SetVideoMode
#define SDL_NumJoysticks      gngeo_SDL_NumJoysticks
#define SDL_JoystickOpen      gngeo_SDL_JoystickOpen
#define SDL_JoystickClose     gngeo_SDL_JoystickClose
#define SDL_JoystickNumAxes   gngeo_SDL_JoystickNumAxes
#define SDL_JoystickNumButtons gngeo_SDL_JoystickNumButtons
#define SDL_JoystickNumHats   gngeo_SDL_JoystickNumHats
#define SDL_textout           gngeo_SDL_textout
#endif

/* Implemented in gngeo_platform.c */
int SDL_Init(Uint32 flags);
void SDL_Quit(void);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, SDL_Rect *srcrect,
                    SDL_Surface *dst, SDL_Rect *dstrect);
void SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect);
Uint32 SDL_MapRGB(SDL_PixelFormat *fmt, Uint8 r, Uint8 g, Uint8 b);
char *SDL_GetError(void);
void SDL_Delay(Uint32 ms);
Uint32 SDL_GetTicks(void);
int SDL_PollEvent(SDL_Event *event);
int SDL_SaveBMP(SDL_Surface *surface, const char *file);

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_CloseAudio(void);
void SDL_PauseAudio(int pause_on);
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);

void SDL_WM_SetCaption(const char *title, const char *icon);
SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags);

int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int index);
void SDL_JoystickClose(SDL_Joystick *joystick);
int SDL_JoystickNumAxes(SDL_Joystick *joystick);
int SDL_JoystickNumButtons(SDL_Joystick *joystick);
int SDL_JoystickNumHats(SDL_Joystick *joystick);

/* text helper used by video.c */
void SDL_textout(SDL_Surface *surface, int x, int y, const char *text);

#endif
