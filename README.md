# Neo Geo (GnGeo) — Retro-Go SD core

GPL-licensed Neo Geo AES/MVS emulator core for
[Game & Watch Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd).

| | |
|--|--|
| Packed binary | `gngeo.bin` → `/cores/gngeo.bin` |
| Cold sidecar | `gngeo.ro` → `/cores/gngeo.ro` (required — init/savestate/tables) |
| ROMs | `/roms/neogeo/*.{zip,neo,gno}` (see below) |
| BIOS | `/bios/neogeo/` (shared, not inside each `.gno`) |
| Entry | `app_main_neogeo` |
| Audio | YM2610 @ 18000 Hz mono |
| Video | Soft LSPC → RGB565 320×224 centered on 320×240 |

Based on [GnGeo](https://github.com/pepone42/gngeo) with a Musashi/gwenesis
68000 core. Early unencrypted titles are the validation target.

## Build

```bash
make            # → gngeo.bin + gngeo.ro (device)
make docker
make host       # → gngeo_host (desktop SDL2)
```

Copy **both** `gngeo.bin` and `gngeo.ro` to `/cores/` on the SD. The `.ro`
sidecar holds cold `.text`/`.rodata` (linked at `0xCAFE…`, mapped into QSPI
and rebased at boot — same pattern as Zelda 3’s `zelda3.ro`).

## BIOS files (device + host)

Put these on the SD card (same names work on host):

```text
/bios/neogeo/uni-bios.rom   # or uni-bios_4_0.rom / sp-s2.sp1
/bios/neogeo/sfix.sfix       # or sfix.sfx
/bios/neogeo/000-lo.lo       # sprite zoom table
```

Also accepted: `/retro-go/bios/neogeo/`. External BIOS **overrides** any
BIOS/sfix/000-lo baked into a `.gno`, so every game shares one UniBIOS.

In-game pause menu: **System** (AES / MVS) and **Region** (Japan / USA /
Europe) — saved for the core. Press A on the option to soft-reset and apply.
Default is AES + Europe (UniBIOS CONSOLE).

## Host preview

```bash
# XIP .gno (uncompressed tiles). BIOS can stay out of the file:
python3 tools/make_gno_xip.py maglord.gno -o host_roms/maglord.gno

make host
./gngeo_host --bios uni-bios-40 host_roms/maglord.gno
# or: NEOGEO_BIOS=uni-bios-40 ./gngeo_host host_roms/maglord.gno
```

Unencrypted MAME `.zip` and TerraOnion `.neo` also work
(`./gngeo_host blazstar.zip`, `./gngeo_host neotris_beta2.neo`).

### Encrypted sets (CMC42/50, SMA, PVC, PCM2)

Decrypt needs ~100+ MiB RAM — do it once on a PC, then feed the plain zip
to host or the device:

```bash
make decrypt-zip    # → tools/decrypt_neogeo_zip (+ cmc42/cmc50.xor)
./tools/decrypt_neogeo_zip --list
./tools/decrypt_neogeo_zip mslug5.zip -o mslug5_dec.zip
./tools/decrypt_neogeo_zip kof2002.zip -o kof2002_dec.zip
./gngeo_host mslug5_dec.zip
# SD: /roms/neogeo/<game>_dec.zip
```

Supports the encrypted MAME sets in `init_func_table` plus PVC siblings
(`svc`, `kof2003`, `samsho5`, `samsh5sp`). SMA/PVC/`fix_bank` handlers are
picked from **chip names inside the zip** (not the `.zip` filename). Requires
the `zip` CLI for the output archive.

Controls: arrows = D-pad, `X`/`Z`/`S`/`A` = Neo A/B/C/D, Enter = Start,
Shift = Select (coin). Esc quits.

## Prepare an XIP `.gno` (device)

The device maps the `.gno` in OSPI flash. Classic `gngeo --dump` files compress
sprite tiles (type 1); this core only accepts **uncompressed** regions (type 0).

Some .gno files for gngeo are available [here](https://github.com/steward-fu/website/releases/tag/neogeo), these files has to be converted using tools/make_gno_xip.py script.

```bash
python3 tools/make_gno_xip.py maglord.gno -o maglord.gno
# BIOS is optional in the .gno when /bios/neogeo is present on the SD card.
# --bios still works if you want a self-contained file.
```

```text
/cores/gngeo.bin
/cores/gngeo.ro
/roms/neogeo/maglord.gno
/bios/neogeo/uni-bios.rom
/bios/neogeo/sfix.sfix
/bios/neogeo/000-lo.lo
```

Controls: D-pad, A/B/X/Y = A/B/C/D, Start = start, Select = coin.

## Layout

```text
Makefile                 CORE_NAME=gngeo pack metadata
src/main_neogeo.c        Retro-Go frame loop
src/porting/             SDL stubs, gno XIP loader, Musashi glue
src/gngeo/               Vendored GnGeo + m68k + mamez80 + ym2610
tools/make_gno_xip.py         Dump → XIP .gno (+ optional BIOS inject)
tools/decrypt_neogeo_zip      Encrypted MAME zip → plain zip (mslug5)
tools/data/cmc50.xor          CMC50 tables for decrypt tool
tools/data/cmc42.xor          CMC42 tables for decrypt tool
sdk/                          Retro-Go SD core SDK
```

## Licence

GnGeo is GPL; this core is GPL. See `src/gngeo/COPYING`.
