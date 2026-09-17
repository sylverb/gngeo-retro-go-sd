#!/usr/bin/env python3
"""Convert a GnGeo .gno dump into an XIP-safe .gno for Retro-Go SD.

Classic `gngeo --dump` writes sprite tiles as zlib block-compressed
(type=1). The H7B0 core memory-maps the whole .gno and only accepts
uncompressed regions (type=0). This tool:

  1. Rewrites every type=1 region as raw type=0.
  2. Optionally injects Neo Geo BIOS regions from a neogeo.zip / directory
     when they are missing. Prefer keeping BIOS out of the .gno and putting
     files in /bios/neogeo on the SD (or --bios on the host).

Usage:
  # After: gngeo -i /path/to/roms --dump maglord
  python3 tools/make_gno_xip.py maglord.gno -o maglord_xip.gno

  # Optional: bake BIOS into the file (self-contained; SD /bios/neogeo still wins)
  python3 tools/make_gno_xip.py maglord.gno -o maglord_xip.gno \\
      --bios /path/to/neogeo.zip

Region IDs match src/gngeo/roms.h.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zipfile
import zlib
from pathlib import Path

MAGIC = b"gnodmpv1"

REGION_AUDIO_CPU_BIOS = 0
REGION_AUDIO_CPU_CARTRIDGE = 1
REGION_AUDIO_DATA_1 = 3
REGION_AUDIO_DATA_2 = 4
REGION_FIXED_LAYER_BIOS = 5
REGION_FIXED_LAYER_CARTRIDGE = 6
REGION_MAIN_CPU_BIOS = 7
REGION_MAIN_CPU_CARTRIDGE = 8
REGION_SPRITES = 9
REGION_SPR_USAGE = 10
REGION_GAME_FIX_USAGE = 11
REGION_ZOOM_TABLE = 12

HAS_CUSTOM_CPU_BIOS = 0x1
HAS_CUSTOM_SFIX_BIOS = 0x4

REGION_NAMES = {
    REGION_AUDIO_CPU_BIOS: "audio_cpu_bios",
    REGION_AUDIO_CPU_CARTRIDGE: "cpu_z80",
    REGION_AUDIO_DATA_1: "adpcma",
    REGION_AUDIO_DATA_2: "adpcmb",
    REGION_FIXED_LAYER_BIOS: "bios_sfix",
    REGION_FIXED_LAYER_CARTRIDGE: "game_sfix",
    REGION_MAIN_CPU_BIOS: "bios_m68k",
    REGION_MAIN_CPU_CARTRIDGE: "cpu_m68k",
    REGION_SPRITES: "tiles",
    REGION_SPR_USAGE: "spr_usage",
    REGION_GAME_FIX_USAGE: "gfix_usage",
    REGION_ZOOM_TABLE: "zoom_lo",
}

# Prefer UniBIOS, then stock arcade BIOS files found in MAME neogeo.zip.
BIOS_CPU_CANDIDATES = (
    "uni-bios.rom",
    "uni-biosn.rom",
    "uni-bios_4_0.rom",
    "uni-bios_3_3.rom",
    "uni-bios_3_2.rom",
    "uni-bios_3_1.rom",
    "uni-bios_3_0.rom",
    "uni-bios_2_3.rom",
    "uni-bios_2_2.rom",
    "uni-bios_2_1.rom",
    "uni-bios_2_0.rom",
    "uni-bios_1_3.rom",
    "uni-bios_1_2.rom",
    "uni-bios_1_1.rom",
    "uni-bios_1_0.rom",
    "sp-s2.sp1",
    "sp-s.sp1",
    "usa_2slt.bin",
    "sp-e.sp1",
    "asia-s3.rom",
)
BIOS_SFIX_CANDIDATES = ("sfix.sfx", "sfix.sfix")
BIOS_LO_CANDIDATES = ("000-lo.lo",)


def convert_sfix_chars(data: bytes) -> bytes:
    """Mirror GnGeo convert_all_char (little-endian) for board SFIX."""
    if len(data) < 0x20000:
        data = data + bytes(0x20000 - len(data))
    src = bytearray(data[:0x20000])
    out = bytearray(0x20000)
    si = 0
    di = 0
    while si + 32 <= len(src):
        for _ in range(8):
            out[di] = src[si + 16]
            out[di + 1] = src[si + 24]
            out[di + 2] = src[si]
            out[di + 3] = src[si + 8]
            di += 4
            si += 1
        si += 24
    return bytes(out)


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def read_region(data: bytes, off: int) -> tuple[int, int, int, bytes, int]:
    """Return (size, lid, type, payload_bytes, next_offset)."""
    if off + 6 > len(data):
        raise ValueError(f"truncated region header at {off}")
    size = u32(data, off)
    lid = data[off + 4]
    typ = data[off + 5]
    p = off + 6
    if typ == 0:
        end = p + size
        if end > len(data):
            raise ValueError(f"type0 region {lid} overruns file")
        return size, lid, typ, data[p:end], end

    if typ != 1:
        raise ValueError(f"unsupported region type {typ} (id={lid})")

    if p + 4 > len(data):
        raise ValueError("truncated type1 block_size")
    block_size = u32(data, p)
    p += 4
    if block_size == 0 or size % block_size != 0:
        raise ValueError(
            f"bad block_size={block_size} for region {lid} size={size}"
        )
    nb_block = size // block_size
    table_bytes = nb_block * 4
    if p + table_bytes + 4 > len(data):
        raise ValueError("truncated type1 offset table")
    offsets = [u32(data, p + i * 4) for i in range(nb_block)]
    p += table_bytes
    cmp_size = u32(data, p)
    p += 4
    # Compressed stream follows; length is cmp_size (sum of compressed payloads
    # only — not including per-block length words). Absolute offsets in the
    # table point at each (len32 + payload) pair.
    out = bytearray(size)
    for i, abs_off in enumerate(offsets):
        if abs_off + 4 > len(data):
            raise ValueError(f"bad block offset {abs_off}")
        clen = u32(data, abs_off)
        cstart = abs_off + 4
        cend = cstart + clen
        if cend > len(data):
            raise ValueError(f"compressed block {i} overruns file")
        raw = zlib.decompress(data[cstart:cend])
        if len(raw) != block_size:
            raise ValueError(
                f"block {i}: expected {block_size} bytes, got {len(raw)}"
            )
        out[i * block_size : (i + 1) * block_size] = raw
    # Advance past the compressed area using cmp_size (as GnGeo does).
    end = p + cmp_size
    # Some dumps leave padding; prefer max(end, last block end).
    if offsets:
        last = offsets[-1]
        last_clen = u32(data, last)
        end = max(end, last + 4 + last_clen)
    return size, lid, typ, bytes(out), end


def parse_gno(data: bytes) -> tuple[str, int, list[tuple[int, int, bytes]]]:
    if len(data) < 21 or data[:8] != MAGIC:
        raise ValueError("not a gnodmpv1 file")
    name = data[8:16].decode("ascii", errors="replace").rstrip(" ")
    flags = u32(data, 16)
    nb_sec = data[20]
    off = 21
    regions: list[tuple[int, int, bytes]] = []
    for _ in range(nb_sec):
        size, lid, typ, payload, off = read_region(data, off)
        if len(payload) != size:
            raise ValueError(f"region {lid}: size mismatch")
        if typ == 1:
            print(f"  expanded type1 region {lid} ({REGION_NAMES.get(lid, '?')}) "
                  f"{size} bytes")
        regions.append((lid, size, payload))
    return name, flags, regions


def write_gno(name: str, flags: int, regions: list[tuple[int, int, bytes]]) -> bytes:
    out = bytearray()
    out += MAGIC
    out += f"{name[:8]:<8}".encode("ascii")[:8]
    out += struct.pack("<I", flags)
    out += bytes([len(regions)])
    for lid, size, payload in regions:
        if len(payload) != size:
            raise ValueError(f"region {lid}: payload length != size")
        out += struct.pack("<I", size)
        out += bytes([lid, 0])  # always type 0
        out += payload
    return bytes(out)


def _load_named(path: Path, names: tuple[str, ...]) -> bytes | None:
    if path.is_dir():
        for n in names:
            f = path / n
            if f.is_file():
                print(f"  bios: {f}")
                return f.read_bytes()
        return None
    if path.suffix.lower() == ".zip" or zipfile.is_zipfile(path):
        with zipfile.ZipFile(path, "r") as zf:
            lower = {n.lower(): n for n in zf.namelist()}
            for n in names:
                key = n.lower()
                if key in lower:
                    print(f"  bios: {path}!{lower[key]}")
                    return zf.read(lower[key])
        return None
    # Single file: use if name matches any candidate
    if path.is_file() and path.name.lower() in {n.lower() for n in names}:
        print(f"  bios: {path}")
        return path.read_bytes()
    return None


def inject_bios(
    regions: list[tuple[int, int, bytes]],
    flags: int,
    bios_path: Path,
) -> tuple[list[tuple[int, int, bytes]], int]:
    have = {lid for lid, _, _ in regions}
    out = list(regions)
    if REGION_MAIN_CPU_BIOS not in have:
        raw = _load_named(bios_path, BIOS_CPU_CANDIDATES)
        if not raw:
            raise SystemExit(
                f"missing MAIN_CPU_BIOS and could not find any of "
                f"{BIOS_CPU_CANDIDATES} under {bios_path}"
            )
        out.append((REGION_MAIN_CPU_BIOS, len(raw), raw))
        flags |= HAS_CUSTOM_CPU_BIOS
        print(f"  injected bios_m68k ({len(raw)} bytes)")
    if REGION_FIXED_LAYER_BIOS not in have:
        raw = _load_named(bios_path, BIOS_SFIX_CANDIDATES)
        if not raw:
            raise SystemExit(
                f"missing FIXED_LAYER_BIOS and could not find any of "
                f"{BIOS_SFIX_CANDIDATES} under {bios_path}"
            )
        raw = convert_sfix_chars(raw)
        out.append((REGION_FIXED_LAYER_BIOS, len(raw), raw))
        flags |= HAS_CUSTOM_SFIX_BIOS
        print(f"  injected+converted bios_sfix ({len(raw)} bytes)")
    if REGION_ZOOM_TABLE not in have:
        raw = _load_named(bios_path, BIOS_LO_CANDIDATES)
        if not raw:
            print("  warning: 000-lo.lo not found — sprites may zoom incorrectly")
        else:
            out.append((REGION_ZOOM_TABLE, len(raw), raw))
            print(f"  injected zoom_lo ({len(raw)} bytes)")
    return out, flags


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="input .gno from gngeo --dump")
    ap.add_argument("-o", "--output", type=Path, required=True, help="output XIP .gno")
    ap.add_argument(
        "--bios",
        type=Path,
        help="neogeo.zip or directory with uni-bios.rom / sfix.sfix / 000-lo.lo "
        "(optional; core prefers /bios/neogeo on device)",
    )
    args = ap.parse_args()

    data = args.input.read_bytes()
    print(f"reading {args.input} ({len(data)} bytes)")
    name, flags, regions = parse_gno(data)
    print(f"  game={name!r} flags=0x{flags:08x} sections={len(regions)}")

    have = {lid for lid, _, _ in regions}
    need_bios = (
        REGION_MAIN_CPU_BIOS not in have
        or REGION_FIXED_LAYER_BIOS not in have
        or REGION_ZOOM_TABLE not in have
    )
    if need_bios and args.bios:
        regions, flags = inject_bios(regions, flags, args.bios)
    elif need_bios:
        missing = []
        if REGION_MAIN_CPU_BIOS not in have:
            missing.append("bios_m68k")
        if REGION_FIXED_LAYER_BIOS not in have:
            missing.append("bios_sfix")
        if REGION_ZOOM_TABLE not in have:
            missing.append("000-lo.lo")
        print(
            f"  note: dump has no {', '.join(missing)}; "
            f"core will load them from /bios/neogeo (or --bios on host)"
        )

    out = write_gno(name, flags, regions)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(f"wrote {args.output} ({len(out)} bytes, {len(regions)} type0 regions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
