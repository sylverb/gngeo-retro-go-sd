# Changelog

## [v0.0.3]

Still WIP: occasional sound popping (set volume to 0 to mute emu audio).
If sound does not start, restart the game.

### Added

- MAME Neo Geo `.zip` ROMs on device and host (QSPI region cache, auto-detect).
- Offline `tools/decrypt_neogeo_zip` for encrypted CMC42/50 / SMA / PVC / PCM2
  sets → plain zip (`make decrypt-zip`).
- Runtime PVC and SMA bank handlers so decrypted sets boot (mslug5, svc,
  kof2003, mslug3, garou, kof99, kof2000, …).

### Changed

- Updated Neo Geo header logo.

### Fixed

- Nothing.

### Install

Release assets (attached to this GitHub release):

- `neogeo-vx.x.x.zip` — SD install archive. Unzip onto the **root** of the SD
  card (creates `/cores/neogeo.bin` and `/cores/neogeo.ro`).

Also required on the SD card (not in the zip):

- ROMs: `/roms/neogeo/*.gno` (XIP dumps via `tools/make_gno_xip.py`) and/or
  unencrypted MAME `/roms/neogeo/*.zip`. Encrypted MAME sets must be decrypted
  first with `tools/decrypt_neogeo_zip` (e.g. `mslug5_dec.zip`).
- BIOS (shared): `/bios/neogeo/uni-bios.rom` (or `uni-bios_4_0.rom` / `sp-s2.sp1`),
  `sfix.sfix` (or `sfix.sfx`), and `000-lo.lo`.

Firmware must match the ABI recorded in `SDK_VERSION` in this repository
(`FIRMWARE_ABI_VERSION=2` at this release).
