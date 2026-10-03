# Changelog

## [v0.0.4]

Still WIP: occasional sound popping (set volume to 0 to mute emu audio).

### Added

- Pause-menu options **System** (AES / MVS) and **Region** (Japan / USA /
  Europe), persisted via core settings. AES/MVS drives `STATUS_A`/`STATUS_B` and
  `REG_BRDFIX`; region is stamped into UniBIOS backup RAM / memcard (`V2`
  header). Confirm with A to soft-reset and apply (clears 68k RAM so UniBIOS
  re-reads prefs; STATUS is set before BIOS boot — was stuck MVS/`0xFF`).

### Changed

- Default UniBIOS boot remains AES + Europe (CONSOLE); MVS is opt-in.

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
- mslug5 softlock: 68k master cycle counter could wrap after a long frame-wait
  busy-loop; `m68k_run` then no-op'd forever (watchdog reset did not clear
  cycles). Rebase cycles before overflow and on CPU reset.
- Intermittent silent boot: defer `audio_start_playing` until after ROM/flash
  load so SAI/DMA is not left unfed during a long `boot_game()`. Hard-restart
  SAI (stop/init/clear/start) after load and on every SleepWake.
- Cache 512 KiB cart S1 (`kof2000` / `kof2003`): stream convert tile-by-tile
  into QSPI instead of staging the whole SFIX in RAM_EMU (`OOM staging sfix`).
- UniBIOS defaults to CONSOLE: present AES on `REG_STATUS_A`/`STATUS_B` (FBNeo
  `0x3F & ~0x18`) and treat `REG_BRDFIX` as a no-op when a cart S ROM exists.
  `kof2003` AES splash omits “SNK forever” — restore those board SFIX rows.

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
