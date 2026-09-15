#!/usr/bin/env python3
"""Print the DISPLAY GAMMA RAMP a Xenia `.xtr` capture recorded — hardware's own curve.

WHY THIS EXISTS (2026-09-15, the operator: "interiors are really dark compared to the
Xbox 360"). Every single-frame capture carries one `GammaRamp` record (xtr.py's command
11): the 256-entry 10:10:10 table the title loaded into the display controller through
the DC_LUT registers, plus the 128-entry PWL table. Xenia APPLIES that ramp when it
presents; this runtime has never read a DC_LUT register (grep pm4.cpp / vk_renderer.cpp
for DC_LUT: nothing), so whatever the ramp does to the picture, ours does not. This
tool prints the curve so the question "what does hardware do to the frame after the
tone map" is answered from the capture rather than argued.

    python3 tools/xtr_gamma_ramp.py "Xenia logs/R2_world/w4_bathroom/w4_bathroom.xtr"
    python3 tools/xtr_gamma_ramp.py <a.xtr> --csv > ramp.csv      # 256 rows: in, r, g, b

The 256-entry table is what the 8-bit front buffer goes through; entry i (0..255) is
the 10-bit output for input i. Identity would be i * 1023 / 255. The three interior and
exterior R2/R4 captures read the SAME table, and it is not identity: it sits BELOW the
line at the dark end (input 32/255 -> 66/1023 = 16.5/255) — an exponent of ~1.15-1.3.
Read that together with the front buffer's format before drawing a conclusion: a
k_8_8_8_8_GAMMA resolve is PWL-encoded (brightened) on write, and the ramp then takes
part of that back; skipping both is not the same as skipping one.
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xtr  # noqa: E402


def read_ramp(path):
    data = open(path, 'rb').read()
    n = len(data)
    for off, cid in xtr.walk(data, n):
        if cid != 11:
            continue
        _t, rw, enc, ln = struct.unpack_from('<4I', data, off)
        payload = data[off + 16:off + 16 + ln]
        if enc == 1:
            import cramjam
            payload = bytes(cramjam.snappy.decompress_raw(payload))
        if len(payload) < 1024:
            raise SystemExit(f'{path}: GammaRamp payload is {len(payload)} bytes, expected >= 1024')
        table = struct.unpack_from('<256I', payload, 0)
        pwl = struct.unpack_from('<384I', payload, 1024) if len(payload) >= 2560 else None
        return rw, table, pwl
    raise SystemExit(f'{path}: no GammaRamp record')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('xtr')
    ap.add_argument('--csv', action='store_true', help='print all 256 rows as CSV')
    args = ap.parse_args()
    rw, table, pwl = read_ramp(args.xtr)
    r = [(v >> 20) & 0x3FF for v in table]
    g = [(v >> 10) & 0x3FF for v in table]
    b = [v & 0x3FF for v in table]
    if args.csv:
        print('in,r,g,b')
        for i in range(256):
            print(f'{i},{r[i]},{g[i]},{b[i]}')
        return
    ident = all(abs(r[i] - i * 1023 / 255) < 2.5 for i in range(256))
    print(f'{os.path.basename(args.xtr)}: GammaRamp rw={rw}, 256-entry table '
          f'{"IS" if ident else "is NOT"} identity, r==g==b: {r == g == b}')
    print('   in   out/1023  identity  ratio  exponent')
    for i in (1, 2, 4, 8, 16, 32, 48, 64, 96, 128, 160, 192, 224, 255):
        ideal = i * 1023 / 255
        exp = math.log(r[i] / 1023) / math.log(i / 255) if 0 < r[i] and 0 < i < 255 else float('nan')
        print(f'  {i:3d}   {r[i]:4d}      {ideal:6.1f}   {r[i] / ideal:5.2f}   {exp:5.2f}')
    if pwl:
        pairs = [(x & 0xFFFF, x >> 16) for x in pwl]
        print(f'  PWL table: {len(pairs) // 3} entries x rgb of (base, delta); first '
              f'{pairs[:3]}, last {pairs[-3:]}')


if __name__ == '__main__':
    main()
