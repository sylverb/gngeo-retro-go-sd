# Neo Geo (GnGeo) core for Retro-Go SD.
#
#   make / make docker  → neogeo.bin
# ROMs: /roms/neogeo/*.gno (XIP — see tools/make_gno_xip.py)

PROJECT_KIND ?= core

CORE_NAME  := neogeo
CORE_ENTRY := app_main_neogeo
ROM_DIRNAME := neogeo

# Hot LSPC + Z80 .text in ITCM (see ld/neogeo_core.ld). BUILD_DIR must
# stay build/core so EXCLUDE_FILE object paths match.
CORE_LDSCRIPT := ld/neogeo_core.ld

GNGEO := src/gngeo
PORT  := src/porting

CORE_C_SOURCES := \
$(PORT)/gngeo_platform.c \
$(PORT)/neo_mem.c \
$(PORT)/gno_flash.c \
$(PORT)/neo_frame.c \
$(PORT)/event.c \
$(PORT)/conf_stub.c \
$(PORT)/state_stub.c \
$(PORT)/gnutil_stub.c \
$(PORT)/lib_stubs.c \
$(PORT)/m68k_interf.c \
$(GNGEO)/m68k/m68kcpu.c \
$(GNGEO)/emu.c \
$(GNGEO)/video.c \
$(GNGEO)/memory.c \
$(GNGEO)/timer.c \
$(GNGEO)/pd4990a.c \
$(GNGEO)/frame_skip.c \
$(GNGEO)/list.c \
$(GNGEO)/transpack.c \
$(GNGEO)/neocrypt.c \
$(GNGEO)/mame_layer.c \
$(GNGEO)/mamez80_interf.c \
$(GNGEO)/mamez80/z80.c \
$(GNGEO)/ym2610/ym2610.c \
$(GNGEO)/ym2610/2610intf.c \
src/main_neogeo.c \
src/neogeo_i18n.c

CORE_C_INCLUDES := \
-I$(PORT) \
-I$(GNGEO) \
-I$(GNGEO)/m68k \
-I$(GNGEO)/mamez80 \
-I$(GNGEO)/ym2610 \
-Isrc

# Program ROMs in .gno / UniBIOS dumps are LE word order — Musashi fetch
# helpers use READ_WORD_ROM without SWAP16 (see memory.h, !USE_GENERATOR68K).
CORE_C_DEFS := \
-DHAVE_CONFIG_H \
-DUSE_MAMEZ80 \
-DUSE_MUSASHI68K \
-DBUILD_TABLES \
-DTABLES_FULL

# Profiling / audio A/B (same ym2610.c on device and `make host`):
#   make CORE_C_DEFS+=-DNEO_DISABLE_YM2610=1   → silence YM synth
#   make CORE_C_DEFS+=-DNEO_YM_EARLYOUT=0      → stock YM loop (no early-outs)
#   make CORE_C_DEFS+=-DNEO_DISABLE_VIDEO=1    → no draw/present (audio A/B)
# Host BIOS scout / frame profiler:
#   ./neogeo_host --bios-trace rom.gno
#   ./neogeo_host --prof rom.gno
#   NEO_BIOS_TRACE_EVERY=120 NEO_BIOS_TRACE_VERBOSE=1 ./neogeo_host --bios-trace rom.gno
#   NEO_PROF_EVERY=120 ./neogeo_host --prof rom.gno
# Next perf leads (if still short): stub draw_tile_50/25; ADPCM early-out; Z80 66667
#CORE_C_DEFS += -DNEO_DISABLE_YM2610=1
#CORE_C_DEFS += -DNEO_DISABLE_VIDEO=1

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build/$(PROJECT_KIND)

ifeq ($(PROJECT_KIND),core)
CORE_C_DEFS += \
-DPROJECT_KIND_CORE=1 \
-DCOVERFLOW=1 \
-DCHEAT_CODES=0

PACKED_BIN  := $(CORE_NAME).bin
PAD_LOGO    := src/assets/pad.png
HEADER_LOGO := src/assets/header.png
else
$(error Neo Geo template builds PROJECT_KIND=core only)
endif

OPT ?= -Os

include $(GNW_CORE_SDK)/Makefile

PACK_CORE := $(GNW_CORE_SDK)/tools/pack_core.py
CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

.PHONY: pack
pack: $(TARGET_BIN) $(PAD_LOGO) $(HEADER_LOGO)
	$(V)$(ECHO) [ PACK CORE ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_CORE) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--system-name "Neo Geo" --dirname $(ROM_DIRNAME) \
		--extensions "gno" \
		--core-name "Neo Geo" \
		--version "$(CORE_VERSION)" \
		--cheat-ext "" \
		--pad-logo $(PAD_LOGO) \
		--header-logo $(HEADER_LOGO) \
		--out $(PACKED_BIN)

all: pack

.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-CORE_VERSION
print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)
print-TARGET_ELF:
	@echo $(TARGET_ELF)
print-TARGET_MAP:
	@echo $(BUILD_DIR)/$(CORE_NAME)_core.map
print-CORE_VERSION:
	@echo $(CORE_VERSION)

clean::
	$(V)rm -f $(PACKED_BIN)

.PHONY: docker docker_pull docker_shell

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker:
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE) "PROJECT_KIND=$(PROJECT_KIND)"
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc) PROJECT_KIND=$(PROJECT_KIND)

docker_pull:
	$(V)$(ECHO) "[ PULL ]" $(DOCKER_IMAGE)
	$(V)docker pull $(DOCKER_IMAGE)

docker_shell:
	$(DOCKER_RUN) bash

include host/Makefile.host
