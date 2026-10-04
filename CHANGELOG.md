# Changelog

## [v0.0.5]

### Added

- Nothing.

### Changed

- Nothing.

### Fixed

- Move PVC code/data to .ro to free some ram to fit PVC data (fix Metal Slug 5 & other PVC games).

### Install

Release assets (attached to this GitHub release):

- `gngeo-vx.x.x.zip` — SD install archive. Unzip onto the **root** of the SD
  card (creates `/cores/gngeo.bin` and `/cores/gngeo.ro`).

Also required on the SD card (not in the zip):

- ROMs: `/roms/neogeo/*.gno` (XIP dumps via `tools/make_gno_xip.py`) and/or
  unencrypted MAME `/roms/neogeo/*.zip`. Encrypted MAME sets must be decrypted
  first with `tools/decrypt_neogeo_zip` (e.g. `mslug5_dec.zip`).
- BIOS (shared): `/bios/neogeo/uni-bios.rom` (or `uni-bios_4_0.rom` / `sp-s2.sp1`),
  `sfix.sfix` (or `sfix.sfx`), and `000-lo.lo`.

Firmware must match the ABI recorded in `SDK_VERSION` in this repository
(`FIRMWARE_ABI_VERSION=2` at this release).
