/*
 * Desktop entry: init SDL, then jump into app_main_neogeo.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_compat.h"
#include "host_platform.h"

#ifndef HOST_SCALE
#define HOST_SCALE 2
#endif

extern void app_main_neogeo(uint8_t load_state, uint8_t start_paused, int8_t save_slot);
extern void neo_bios_set_dir(const char *dir);

static void usage(const char *argv0)
{
    printf("Usage: %s [options] [rom.gno]\n", argv0);
    printf("  --bios DIR   Directory (or neogeo.zip) with uni-bios.rom,\n");
    printf("               sfix.sfix, 000-lo.lo\n");
    printf("  Env: HOST_ROM, NEOGEO_BIOS (same as --bios)\n");
}

int main(int argc, char **argv)
{
    const char *title = "Neo Geo (GnGeo host)";
    const char *rom = getenv("HOST_ROM");
    const char *bios = getenv("NEOGEO_BIOS");
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) {
            bios = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            printf("unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            rom = argv[i];
        }
    }
    if (!rom || !rom[0])
        rom = "maglord.gno";

    if (bios && bios[0])
        neo_bios_set_dir(bios);

    if (host_platform_init(title, HOST_SCALE) != 0)
        return 1;

    gw_core_bridge_init();
    host_set_rom_path(rom);

    printf("host: Esc or close window to quit\n");
    printf("host: Arrows=D-pad  Z=B  X=A  Enter=Start  Shift=Select  A/S=Y/X\n");
    printf("host: F1=save state  F2=load state  (./host_saves/)\n");
    printf("host: ROM %s\n", rom);
    if (bios && bios[0])
        printf("host: BIOS dir %s\n", bios);
    else
        printf("host: BIOS search: --bios DIR, NEOGEO_BIOS, uni-bios-40/, "
               "neogeo.zip, bios/neogeo/\n");

    app_main_neogeo(0, 0, -1);

    host_platform_shutdown();
    return 0;
}
