#ifndef NEO_PVC_H
#define NEO_PVC_H

#include "SDL.h"

/* NEO-PVC cart protection (mslug5, svc, kof2003). Call after ROM bind. */
void neo_pvc_reset(void);
void neo_pvc_install(void);
int neo_pvc_active(void);
int neo_pvc_wanted(void);
/* Call after battery SRAM is allocated (end of boot_game AHB phase). */
int neo_pvc_ensure_ram(void);

/* Savestate: 0 if PVC not used; else PVC_RAM_BYTES and a live buffer. */
Uint16 *neo_pvc_ram_ptr(void);
Uint32 neo_pvc_ram_bytes(void);

Uint16 neo_pvc_read_word(Uint32 addr);
Uint8 neo_pvc_read_byte(Uint32 addr);
void neo_pvc_write_word(Uint32 addr, Uint16 data);
void neo_pvc_write_byte(Uint32 addr, Uint8 data);

/* Game-name hooks after ROM bind (fix bank type + PVC, etc.). */
void neo_game_special_init(const char *name);

/*
 * Map a zip member basename to a canonical short name (mslug5, kof2003, …).
 * Matches MAME chip names (e.g. 268-p1cr.p1) and decrypt_neogeo_zip output
 * (mslug5-p1.p1). Returns a static string, or NULL if unknown.
 */
const char *neo_game_match_member(const char *member_basename);

#endif
