# Changelog

## [v0.0.2]

Initial port of gngeo emulator. It is WIP and has occasionnal sound popping. You can disable sound emulation by setting volume to 0 in retro-go-sd. Sometimes sound is not running, restart game and it should play sound.

### Added

- Savestates.

### Changed

- Nothing.

### Fixed

- Improvement of performances.

### Install

Release assets (attached to this GitHub release):

- `neogeo-vx.x.x.zip` — SD install archive. Unzip onto the **root** of the SD
  card (creates `/cores/neogeo.bin`).

Also required on the SD card (not in the zip):

- ROMs: `/roms/neogeo/*.gno` — XIP dumps only (`tools/make_gno_xip.py` after
  `gngeo --dump`; classic compressed dumps are not supported on device).
- BIOS (shared): `/bios/neogeo/uni-bios.rom` (or `uni-bios_4_0.rom` / `sp-s2.sp1`),
  `sfix.sfix` (or `sfix.sfx`), and `000-lo.lo`.

Firmware must match the ABI recorded in `SDK_VERSION` in this repository
(`FIRMWARE_ABI_VERSION=2` at this release).
