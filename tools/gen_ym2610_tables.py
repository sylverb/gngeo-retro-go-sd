#!/usr/bin/env python3
"""Generate src/gngeo/ym2610/ym2610_tables.{c,h} with host libm."""
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENV_BITS = 10
ENV_LEN = 1 << ENV_BITS
ENV_STEP = 128.0 / ENV_LEN
SIN_BITS = 10
SIN_LEN = 1 << SIN_BITS
TL_RES_LEN = 256
TL_TAB_LEN = 13 * 2 * TL_RES_LEN

def main():
    tl_tab = [0] * TL_TAB_LEN
    for x in range(TL_RES_LEN):
        m = (1 << 16) / math.pow(2, (x + 1) * (ENV_STEP / 4.0) / 8.0)
        m = math.floor(m)
        n = int(m) >> 4
        n = (n >> 1) + 1 if (n & 1) else (n >> 1)
        n <<= 2
        tl_tab[x * 2 + 0] = n
        tl_tab[x * 2 + 1] = -n
        for i in range(1, 13):
            v = tl_tab[x * 2] >> i
            tl_tab[x * 2 + 0 + i * 2 * TL_RES_LEN] = v
            tl_tab[x * 2 + 1 + i * 2 * TL_RES_LEN] = -v

    sin_tab = [0] * SIN_LEN
    for i in range(SIN_LEN):
        m = math.sin(((i * 2) + 1) * math.pi / SIN_LEN)
        o = 8 * math.log(1.0 / abs(m)) / math.log(2)
        o = o / (ENV_STEP / 4)
        n = int(2.0 * o)
        n = (n >> 1) + 1 if (n & 1) else (n >> 1)
        sin_tab[i] = n * 2 + (0 if m >= 0.0 else 1)

    out_c = ROOT / "src/gngeo/ym2610/ym2610_tables.c"
    out_h = ROOT / "src/gngeo/ym2610/ym2610_tables.h"
    out_h.write_text(
        "/* Precomputed YM2610 tl_tab / sin_tab (see ym2610_tables.c). */\n"
        "#ifndef YM2610_TABLES_H\n#define YM2610_TABLES_H\n\n"
        f"#define YM2610_TL_TAB_LEN  ({TL_TAB_LEN})\n"
        f"#define YM2610_SIN_LEN     ({SIN_LEN})\n\n"
        "extern const signed int ym2610_tl_tab[YM2610_TL_TAB_LEN];\n"
        "extern const unsigned int ym2610_sin_tab[YM2610_SIN_LEN];\n\n"
        "#endif\n"
    )
    lines = [
        "/* Auto-generated FM tables — do not edit by hand.",
        " * Built with host libm so G&W does not use crude sin/log stubs.",
        " * Regenerate: python3 tools/gen_ym2610_tables.py",
        " */",
        '#include "ym2610_tables.h"',
        "",
        f"const signed int ym2610_tl_tab[{TL_TAB_LEN}] = {{",
    ]
    for i in range(0, TL_TAB_LEN, 8):
        chunk = ", ".join(str(v) for v in tl_tab[i : i + 8])
        comma = "," if i + 8 < TL_TAB_LEN else ""
        lines.append(f"  {chunk}{comma}")
    lines += ["};", "", f"const unsigned int ym2610_sin_tab[{SIN_LEN}] = {{"]
    for i in range(0, SIN_LEN, 8):
        chunk = ", ".join(str(v) for v in sin_tab[i : i + 8])
        comma = "," if i + 8 < SIN_LEN else ""
        lines.append(f"  {chunk}{comma}")
    lines += ["};", ""]
    out_c.write_text("\n".join(lines))
    print(f"wrote {out_c.relative_to(ROOT)} ({out_c.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
