#ifndef GNO_FLASH_H
#define GNO_FLASH_H

#include "SDL_types.h"

void neo_bios_set_dir(const char *dir);
int gno_flash_load(const char *path);
const char *gno_flash_last_error(void);
Uint8 *neo_vector_patch(void);
void neo_vector_use_bios(void);
void neo_vector_use_game(void);

#endif
