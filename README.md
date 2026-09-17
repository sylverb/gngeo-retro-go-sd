# Neo Geo (GnGeo) — Retro-Go SD core

GPL-licensed Neo Geo AES/MVS emulator core for
[Game & Watch Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd).

| | |
|--|--|
| Packed binary | `neogeo.bin` → `/cores/neogeo.bin` |
| ROMs | `/roms/neogeo/*.gno` (XIP tiles — see below) |
| BIOS | `/bios/neogeo/` (shared, not inside each `.gno`) |
| Entry | `app_main_neogeo` |
| Audio | YM2610 @ 18000 Hz mono |
| Video | Soft LSPC → RGB565 320×224 centered on 320×240 |

Based on [GnGeo](https://github.com/pepone42/gngeo) with a Musashi/gwenesis
68000 core. Early unencrypted titles are the validation target.

## Build

```bash
make            # → neogeo.bin (device)
make docker
make host       # → neogeo_host (desktop SDL2)
```

## BIOS files (device + host)

Put these on the SD card (same names work on host):

```text
/bios/neogeo/uni-bios.rom   # or uni-bios_4_0.rom / sp-s2.sp1
/bios/neogeo/sfix.sfix       # or sfix.sfx
/bios/neogeo/000-lo.lo       # sprite zoom table
```

Also accepted: `/retro-go/bios/neogeo/`. External BIOS **overrides** any
BIOS/sfix/000-lo baked into a `.gno`, so every game shares one UniBIOS.

## Host preview

```bash
# XIP .gno (uncompressed tiles). BIOS can stay out of the file:
python3 tools/make_gno_xip.py maglord.gno -o host_roms/maglord.gno

make host
./neogeo_host --bios uni-bios-40 host_roms/maglord.gno
# or: NEOGEO_BIOS=uni-bios-40 ./neogeo_host host_roms/maglord.gno
```

Controls: arrows = D-pad, `Z`/`X` = B/A, `A`/`S` = Y/X (C/D), Enter = Start,
Shift = Select (coin). Esc quits.

## Prepare an XIP `.gno` (device)

The device maps the `.gno` in OSPI flash. Classic `gngeo --dump` files compress
sprite tiles (type 1); this core only accepts **uncompressed** regions (type 0).

```bash
gngeo -i /path/to/roms --dump maglord
python3 tools/make_gno_xip.py maglord.gno -o maglord.gno
# BIOS is optional in the .gno when /bios/neogeo is present on the SD card.
# --bios still works if you want a self-contained file.
```

```text
/cores/neogeo.bin
/roms/neogeo/maglord.gno
/bios/neogeo/uni-bios.rom
/bios/neogeo/sfix.sfix
/bios/neogeo/000-lo.lo
```

Controls: D-pad, A/B/X/Y = A/B/C/D, Start = start, Select = coin.

## Layout

```text
Makefile                 CORE_NAME=neogeo pack metadata
src/main_neogeo.c        Retro-Go frame loop
src/porting/             SDL stubs, gno XIP loader, Musashi glue
src/gngeo/               Vendored GnGeo + m68k + mamez80 + ym2610
tools/make_gno_xip.py    Dump → XIP .gno (+ optional BIOS inject)
sdk/                     Retro-Go SD core SDK
```

## Licence

GnGeo is GPL; this core is GPL. See `src/gngeo/COPYING`.
