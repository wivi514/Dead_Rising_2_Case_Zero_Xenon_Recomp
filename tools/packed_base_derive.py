#!/usr/bin/env python3
"""Derive the packed layout of SMALL textures (mipAddr=0) — including LEVEL 0's offset.

WHY THIS EXISTS (part 59, the gas-sign R6 trace). A whole class of tiny far-LOD
textures (79 distinct in one street frame: 8x8..64x16, all DXT, all packed_mips=1)
carries `mipAddr=0`: the entire chain, base included, lives inside ONE shared tile at
the base address. `packed_mip_derive.py` skips the class (`if not mipAddr`), and the
runtime both skips their mip upload (the `t.mipAddress &&` gate) and reads level 0
from block (0,0) — which, if the packed layout offsets the BASE too, is not the base
at all but the tile's tail region. The R6 letters texture is the worked case: bytes at
(0,0) are 68.8% black "garbage" on hardware AND on our runtime (byte-identical), yet
hardware's distant sign shows clean red GAS letters — so hardware is reading them
from somewhere else in the tile.

METHOD — no reference needed. The classic derivation scores a tail level against a
downsample of its parent; with the base itself packed there is no parent. But a mip
CHAIN is self-consistent: level N+1 is a 2x box downsample of level N. So brute-force
(L0 offset, L1 offset) JOINTLY over all block-aligned positions in the tile, score
each pair by |downsample(L0 slice) - L1 slice|, then verify the winner with L2 (and
report margins). A wrong hypothesis cannot win three levels of consistency by luck
against ~700x900 competing pairs.

USAGE
    packed_base_derive.py <trace.xtr> [--max N]     # derive over the mipAddr=0 class
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from tex_decode import dxt1_block, dxt5_block, tiled2d  # noqa: E402
from packed_mip_derive import (Memory, decompress, swap16, texel_grid,  # noqa: E402
                               downsample, score, grid_sd, BLOCK_BYTES,
                               FMT_DXT1, FMT_DXT5, BANKS, FETCH_BASE,
                               DRAW_OPCODES, BE)


def collect_small(path):
    """Every distinct packed-mips DXT fetch with mipAddr == 0 that any draw bound."""
    data, hdr = xtr.open_trace(path)
    mem = Memory()
    regs = {}
    seen = {}
    for off, cmd in xtr.walk(data, len(data)):
        if cmd in (xtr.CMD_MEMORY_READ, xtr.CMD_MEMORY_WRITE):
            base, enc, elen, dlen = struct.unpack_from('<IIII', data, off + 4)
            try:
                mem.add(base, decompress(data[off + 20:off + 20 + elen], enc, dlen))
            except Exception:
                pass
            continue
        if cmd != xtr.CMD_PACKET_START:
            continue
        count = struct.unpack_from('<I', data, off + 8)[0]
        if not count:
            continue
        header = BE.unpack_from(data, off + 12)[0]

        def word(i):
            return BE.unpack_from(data, off + 12 + 4 * i)[0]

        ptype = header >> 30
        if ptype == 0:
            reg = header & 0x7FFF
            one = (header >> 15) & 1
            for i in range(count - 1):
                regs[reg if one else reg + i] = word(1 + i)
            continue
        if ptype != 3:
            continue
        opcode = (header >> 8) & 0x7F
        if opcode == 0x2D and count >= 2:
            base_reg = BANKS.get((word(1) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(1) & 0x7FF
                for i in range(2, count):
                    regs[base_reg + idx + i - 2] = word(i)
        elif opcode in (0x55, 0x56) and count >= 2:
            idx = word(1) & 0xFFFF
            for i in range(2, count):
                regs[idx + i - 2] = word(i)
        elif opcode in DRAW_OPCODES:
            for slot in range(16):
                d = [regs.get(FETCH_BASE + slot * 6 + i) for i in range(6)]
                if d[0] is None or d[1] is None or d[2] is None or d[5] is None:
                    continue
                if (d[0] & 3) != 2:
                    continue
                fmt = d[1] & 0x3F
                if fmt not in BLOCK_BYTES:
                    continue
                if not ((d[5] >> 11) & 1):
                    continue
                if (d[5] >> 12) << 12:              # mipAddr != 0 -> the other tool's case
                    continue
                if ((d[5] >> 9) & 3) != 1:
                    continue
                t = {
                    'addr': (d[1] >> 12) << 12,
                    'w': (d[2] & 0x1FFF) + 1,
                    'h': ((d[2] >> 13) & 0x1FFF) + 1,
                    'fmt': fmt,
                    'tiled': bool((d[0] >> 31) & 1),
                    'mipMax': (d[4] >> 6) & 0xF if d[4] is not None else 0,
                }
                if t['mipMax'] < 1 or not t['tiled']:
                    continue
                seen[(t['addr'], t['fmt'], t['w'], t['h'])] = t
    return mem, list(seen.values())


def slice_grid(tile, x0, y0, w, h):
    return [row[x0 * 4:x0 * 4 + w] for row in tile[y0 * 4:y0 * 4 + h]]


def derive(mem, t):
    fmt = t['fmt']
    bpu = BLOCK_BYTES[fmt]
    raw = mem.read(t['addr'], 32 * 32 * bpu)
    if raw is None:
        return None, 'tile bytes not in trace'
    tile = texel_grid(fmt, swap16(raw), 32, 32, 32)
    if tile is None:
        return None, 'tile short read'

    lw0, lh0 = t['w'], t['h']
    luW0, luH0 = (lw0 + 3) // 4, (lh0 + 3) // 4
    lw1, lh1 = max(1, lw0 >> 1), max(1, lh0 >> 1)
    luW1, luH1 = (lw1 + 3) // 4, (lh1 + 3) // 4
    if lw1 < 4 or lh1 < 4:
        return None, 'level 1 below one block'

    # Joint (L0, L1) search on chain consistency.
    best = None
    l1_slices = []
    for y1 in range(0, 33 - luH1):
        for x1 in range(0, 33 - luW1):
            l1_slices.append((x1, y1, slice_grid(tile, x1, y1, lw1, lh1)))
    for y0 in range(0, 33 - luH0):
        for x0 in range(0, 33 - luW0):
            g0 = slice_grid(tile, x0, y0, lw0, lh0)
            want1 = downsample(g0, lw1, lh1)
            if max(grid_sd(want1)) < 4.0:
                continue                     # a flat candidate matches anything
            for x1, y1, g1 in l1_slices:
                if x1 == x0 and y1 == y0:
                    continue
                s = score(g1, want1, lw1, lh1)
                if best is None or s < best[0]:
                    best = (s, (x0, y0), (x1, y1), g1)
    if best is None:
        return None, 'no candidate with contrast'
    s01, off0, off1, g1 = best

    # Verify with level 2 if the chain has one at >= one block.
    lw2, lh2 = max(1, lw0 >> 2), max(1, lh0 >> 2)
    l2 = None
    if t['mipMax'] >= 2 and lw2 >= 4 and lh2 >= 4:
        want2 = downsample(g1, lw2, lh2)
        luW2, luH2 = (lw2 + 3) // 4, (lh2 + 3) // 4
        cands = []
        for y2 in range(0, 33 - luH2):
            for x2 in range(0, 33 - luW2):
                cands.append((score(slice_grid(tile, x2, y2, lw2, lh2),
                                    want2, lw2, lh2), x2, y2))
        cands.sort()
        l2 = (cands[0][1], cands[0][2], cands[0][0],
              (cands[1][0] - cands[0][0]) if len(cands) > 1 else 0.0)
    return {'t': t, 'score01': s01, 'off0': off0, 'off1': off1, 'l2': l2}, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--max', type=int, default=0)
    a = ap.parse_args()
    mem, texs = collect_small(a.trace)
    print(f'{a.trace}: {len(texs)} distinct packed-mips DXT textures with mipAddr=0')
    if a.max:
        texs = texs[:a.max]
    votes = {}
    skipped = {}
    for t in texs:
        r, why = derive(mem, t)
        if r is None:
            skipped[why] = skipped.get(why, 0) + 1
            continue
        key = (t['w'], t['h'], t['fmt'])
        l2s = ''
        if r['l2']:
            l2s = f"  L2@({r['l2'][0]},{r['l2'][1]}) s={r['l2'][2]:.1f} m={r['l2'][3]:.1f}"
        print(f"  {t['addr']:08X} {t['w']}x{t['h']} fmt={t['fmt']}: "
              f"L0@{r['off0']} L1@{r['off1']} s01={r['score01']:.1f}{l2s}")
        votes.setdefault(key, {}).setdefault((r['off0'], r['off1']), 0)
        votes[key][(r['off0'], r['off1'])] += 1
    print('\nVOTES by extent (L0 offset, L1 offset):')
    for key, v in sorted(votes.items()):
        rank = sorted(v.items(), key=lambda kv: -kv[1])
        print(f'  {key[0]}x{key[1]} fmt={key[2]}: ' +
              '  '.join(f'{o0}+{o1} x{n}' for (o0, o1), n in rank[:4]))
    for why, n in sorted(skipped.items()):
        print(f'  skipped {n}: {why}')


if __name__ == '__main__':
    main()
