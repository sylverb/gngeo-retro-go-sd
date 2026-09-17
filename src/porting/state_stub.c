#include "state.h"
#include "config.h"
#include "gnutil.h"

SDL_Surface *state_img;
Uint8 state_version;

int mkstate_data(gzFile gzf, void *data, int size, int mode)
{
    (void)gzf; (void)data; (void)size; (void)mode;
    return GN_TRUE;
}

void neogeo_init_save_state(void) {}

int save_state(char *game, int slot)
{
    (void)game; (void)slot;
    return GN_FALSE;
}

int load_state(char *game, int slot)
{
    (void)game; (void)slot;
    return GN_FALSE;
}

SDL_Surface *load_state_img(char *game, int slot)
{
    (void)game; (void)slot;
    return NULL;
}

Uint32 how_many_slot(char *game)
{
    (void)game;
    return 0;
}
