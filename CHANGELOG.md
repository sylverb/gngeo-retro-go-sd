# Changelog

## [v0.0.4]

Still WIP: occasional sound popping (set volume to 0 to mute emu audio).
If sound does not start, restart the game.

### Added

- Nothing.

### Changed

- Nothing.

### Fixed

- Refuse to boot a ROM that does not fit in the remaining QSPI flash cache
  (clear error instead of failing mid-cache).
- Decrypted 2 MiB P sets (`bangbead_dec`, `ganryu_dec`, …): skip MAME
  `CONTINUE` half-swap when the zip is already memory-order (was double-swapped
  → white screen). Program cache key bumped to `p2`.
- `decrypt_neogeo_zip`: mirror short CMC50 M1 dumps to 512 KiB before decrypt
  (fixes kof2000 match music / garbage ADPCM banks).
- `decrypt_neogeo_zip`: write CMC50 M1 as plain 512 KiB (not the 0x90000
  runtime banking image) so Z80 banks match FBNeo/neo_zip — fixes kof2003
  looping music / missing SFX and other CMC50 audio.

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
