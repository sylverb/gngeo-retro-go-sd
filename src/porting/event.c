#include "event.h"
#include "memory.h"
#include "emu.h"
#include "config.h"
#include "gnutil.h"
#include "neo_settings.h"

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

    /*
     * REG_STATUS_A ($320001): FBNeo MVS=0x3F, AES=0x3F&~0x18 (bits 3–4).
     * REG_STATUS_B bit15 via intern_start bit7: MVS=1, AES=0.
     * Driven by the persistent System option (AES → UniBIOS CONSOLE).
     * Select shares COIN and also lowers intern_start bit 1 (UniBIOS 1.3+).
     */
    if (neo_settings_is_aes()) {
        memory.intern_coin = (Uint8)(0x3F & ~0x18);
        memory.intern_start = 0x0F; /* bit7 clear = AES */
    } else {
        memory.intern_coin = 0x3F;
        memory.intern_start = 0x8F; /* bit7 set = MVS */
    }
    if (coin)
        memory.intern_coin &= (Uint8)~0x01; /* P1 coin */
    if (start)
        memory.intern_start &= (Uint8)~0x01; /* P1 start */
    if (coin)
        memory.intern_start &= (Uint8)~0x02; /* P1 select */
}
