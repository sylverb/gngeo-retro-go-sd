#include "event.h"
#include "memory.h"
#include "emu.h"
#include "config.h"
#include "gnutil.h"

Uint8 joy_state[2][GN_MAX_KEY];

void neo_set_input(Uint8 p1_buttons, Uint8 start, Uint8 coin)
{
    /* p1_buttons: bit0=A bit1=B bit2=C bit3=D bit4=UP bit5=DOWN bit6=LEFT bit7=RIGHT */
    memory.intern_p1 = 0xFF;
    if (p1_buttons & 0x10) memory.intern_p1 &= ~0x01; /* up */
    if (p1_buttons & 0x20) memory.intern_p1 &= ~0x02; /* down */
    if (p1_buttons & 0x40) memory.intern_p1 &= ~0x04; /* left */
    if (p1_buttons & 0x80) memory.intern_p1 &= ~0x08; /* right */
    if (p1_buttons & 0x01) memory.intern_p1 &= ~0x10; /* A */
    if (p1_buttons & 0x02) memory.intern_p1 &= ~0x20; /* B */
    if (p1_buttons & 0x04) memory.intern_p1 &= ~0x40; /* C */
    if (p1_buttons & 0x08) memory.intern_p1 &= ~0x80; /* D */

    memory.intern_p2 = 0xFF;
    memory.intern_start = 0xFF;
    memory.intern_coin = 0xFF;
    if (start) memory.intern_start &= ~0x01; /* P1 start */
    if (coin)  memory.intern_coin  &= ~0x01; /* P1 coin */
}
