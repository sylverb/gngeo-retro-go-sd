#ifndef GNEVENT_H
#define GNEVENT_H

#include "SDL.h"

typedef enum {
    GN_NONE = 0,
    GN_A,
    GN_B,
    GN_C,
    GN_D,
    GN_UP,
    GN_DOWN,
    GN_LEFT,
    GN_RIGHT,
    GN_START,
    GN_SELECT_COIN,
    GN_MENU_KEY,
    GN_HOTKEY1,
    GN_HOTKEY2,
    GN_HOTKEY3,
    GN_HOTKEY4,
    GN_MAX_KEY,
} GNGEO_BUTTON;

extern Uint8 joy_state[2][GN_MAX_KEY];

static inline int init_event(void) { return 1; }
static inline int handle_event(void) { return 0; }
static inline void reset_event(void) {}

void neo_set_input(Uint8 p1_buttons, Uint8 start, Uint8 coin);

#endif
