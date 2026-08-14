#!/usr/bin/env python3
"""Derive the PACKED MIP TAIL layout empirically, from hardware's own texture bytes.

WHY THIS EXISTS (part 41, item 2)
---------------------------------
Part 39 uploads each texture's mip chain but stops at the packed tail: the levels
small enough to share one 32x32-unit tile, at sub-tile offsets the runtime does not
know how to compute ("mip: PACKED TAIL REACHED" — declined and counted, never
guessed, gotcha 5). The tail is exactly what a DISTANT surface samples, so the
missing levels are a far-field defect: the sampler clamps at the last uploaded
level and distant surfaces sample a level far too detailed for their footprint.

The classic 360 layout packs all levels whose texel extent fits a threshold into
one tile at fixed offsets — but a remembered table is exactly what part 39's method
exists to replace (and what gotcha 308 punishes: a wrong constant that is silently
wrong). So this tool carries no table. It BRUTE-FORCES the offsets against ground
truth:

  For every packed-mips texture a hardware trace carries bytes for, walk the
  unpacked chain with the accumulation rule part 39 verified, then decode the
  ENTIRE shared tile to texels once and try every block-aligned offset (x0, y0)
  for each tail level, scoring the candidate slice against the 2x box downsample
  of the level above — in TEXEL space, on luma AND alpha, because the earlier
  endpoint-only scorer went blind exactly where the tail matters (a 4x4 level is
  one block, whose two endpoints have no variance to discriminate with — the same
  shape as gotcha 287's junk-scorer, which this tool's first draft repeated).

The output is a per-(level extent) table of winning offsets with votes, margins
and disagreements — a disagreement or a weak margin is printed, never smoothed
over. If the vote is unanimous the table can be transcribed into UploadTexture's
chain walk; if it is not, the layout hypothesis is wrong and the tool has said so.

USAGE
    packed_mip_derive.py "Xenia logs/R4_world"/*/*.xtr [--max-per-trace N]
    packed_mip_derive.py trace.xtr --verbose      # per-texture detail
"""

import argparse
import collections
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from tex_decode import dxt1_block, dxt5_block, tiled2d  # noqa: E402

BE = struct.Struct(">I")
DRAW_OPCODES = (0x22, 0x36)
FETCH_BASE = 0x4800
BANKS = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}

# The two formats that carry nearly every world texture in this title. Formats are
# added here only with a matching decoder — an unknown format is SKIPPED AND
# COUNTED, never guessed at.
FMT_DXT1 = 18
FMT_DXT5 = 20
BLOCK_BYTES = {FMT_DXT1: 8, FMT_DXT5: 16}
DECODER = {FMT_DXT1: dxt1_block, FMT_DXT5: dxt5_block}


def decompress(payload, enc, declen):
    if enc == 0:
        return payload
    import cramjam
    return bytes(cramjam.snappy.decompress_raw(payload))[:declen]


def swap16(b):
    d = bytearray(b)
    d[0::2], d[1::2] = d[1::2], d[0::2]
    return bytes(d)


class Memory:
    """Trace memory records, newest-wins — same caveats as xtr_draw_bindings.Memory
    (a record is a snapshot with a TIME; textures read from disc are safe, surfaces
    the GPU renders inside the frame are not — mip chains are the former)."""

    def __init__(self):
        self.chunks = []

    def add(self, base, data):
        self.chunks.append((base, base + len(data), data))

    def read(self, addr, length):
        for lo, hi, d in reversed(self.chunks):
            if lo <= addr and addr + length <= hi:
                return d[addr - lo:addr - lo + length]
        return None


def texel_grid(fmt, data, luW, luH, pitch, x0=0, y0=0):
    """Decode a tiled block region to a texel grid of (luma, alpha) pairs.

    Block (x, y) of the level is read at tile-space (x0+x, y0+y) with the given
    unit pitch — the same addressing the runtime's untiler uses. `data` must
    already be swap16'd (Xenos DXT payloads are big-endian 16-bit words).
    Returns rows of (luma, alpha), 4*luH by 4*luW, or None on a short read.
    """
    bpu = BLOCK_BYTES[fmt]
    decode = DECODER[fmt]
    l2b = bpu.bit_length() - 1
    out = [[None] * (4 * luW) for _ in range(4 * luH)]
    for by in range(luH):
        for bx in range(luW):
            off = tiled2d(x0 + bx, y0 + by, pitch, l2b) * bpu
            if off + bpu > len(data):
                return None
            for t, ((r, g, b), a) in enumerate(decode(data[off:off + bpu])):
                out[by * 4 + t // 4][bx * 4 + t % 4] = \
                    (0.299 * r + 0.587 * g + 0.114 * b, float(a))
    return out


def downsample(g, ow, oh):
    """Box downsample a texel grid to ow x oh (the level below's extent)."""
    h, w = len(g), len(g[0])
    sy, sx = h / oh, w / ow
    out = []
    for y in range(oh):
        row = []
        for x in range(ow):
            ys = range(int(y * sy), max(int(y * sy) + 1, int((y + 1) * sy)))
            xs = range(int(x * sx), max(int(x * sx) + 1, int((x + 1) * sx)))
            l = a = n = 0.0
            for yy in ys:
                for xx in xs:
                    l += g[yy][xx][0]
                    a += g[yy][xx][1]
                    n += 1
            row.append((l / n, a / n))
        out.append(row)
    return out


def grid_sd(g):
    """The larger of the luma and alpha standard deviations — the contrast the
    scorer has available. A flat reference cannot discriminate offsets."""
    for ch in (0, 1):
        flat = [v[ch] for row in g for v in row]
        mean = sum(flat) / len(flat)
        yield (sum((v - mean) ** 2 for v in flat) / len(flat)) ** 0.5


def score(cand, want, lw, lh):
    """Mean |difference| per texel over luma and alpha, over the level's extent."""
    s = 0.0
    n = 0
    for y in range(lh):
        for x in range(lw):
            s += abs(cand[y][x][0] - want[y][x][0]) + \
                 abs(cand[y][x][1] - want[y][x][1])
            n += 1
    return s / n


def collect_textures(path):
    """One walk of the trace: memory + every distinct packed-mips DXT fetch that any
    draw bound."""
    data, hdr = xtr.open_trace(path)
    mem = Memory()
    regs = {}
    seen = {}
    for off, cmd in xtr.walk(data, len(data)):
        if cmd in (xtr.CMD_MEMORY_READ, xtr.CMD_MEMORY_WRITE):
            base, enc, elen, dlen = struct.unpack_from('<IIII', data, off + 4)
            try:
                mem.add(base, decompress(data[off + 20:off + 20 + elen], enc, dlen))
            except Exception as e:
                print('  [memory record at %08X undecodable: %s]' % (base, e),
                      file=sys.stderr)
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
                if not ((d[5] >> 11) & 1):            # packed_mips
                    continue
                mipAddr = (d[5] >> 12) << 12
                if not mipAddr:
                    continue
                if ((d[5] >> 9) & 3) != 1:            # 2D only, as the runtime walks
                    continue
                t = {
                    'addr': (d[1] >> 12) << 12,
                    'w': (d[2] & 0x1FFF) + 1,
                    'h': ((d[2] >> 13) & 0x1FFF) + 1,
                    'fmt': fmt,
                    'tiled': bool((d[0] >> 31) & 1),
                    'mipMax': (d[4] >> 6) & 0xF if d[4] is not None else 0,
                    'mipAddr': mipAddr,
                }
                if t['mipMax'] < 1 or not t['tiled']:
                    continue
                seen[(t['addr'], t['fmt'], t['w'], t['h'], t['mipAddr'])] = t
    return mem, list(seen.values())


def derive_one(mem, t, verbose):
    """Walk one texture's chain to the tail, then brute-force each tail level.

    Returns (results, why_skipped): results is a list of per-level dicts, or None
    if the texture cannot inform (bytes missing, unpacked rule broken, no tail).
    """
    fmt = t['fmt']
    bpu = BLOCK_BYTES[fmt]

    # Level 0 reference grid, from the base address at the fetch pitch rule.
    luW0 = (t['w'] + 3) // 4
    luH0 = (t['h'] + 3) // 4
    pitch0 = (luW0 + 31) & ~31
    raw = mem.read(t['addr'], pitch0 * ((luH0 + 31) & ~31) * bpu)
    if raw is None:
        return None, 'base bytes not in trace'
    ref = texel_grid(fmt, swap16(raw), luW0, luH0, pitch0)
    if ref is None:
        return None, 'base short read'

    # The unpacked chain: accumulate full-tile footprints exactly as the runtime
    # does, keeping the reference grid one octave down at each step.
    chainOff = 0
    level = 1
    tail_off = None
    while level <= t['mipMax'] and level < 16:
        lw = max(1, t['w'] >> level)
        lh = max(1, t['h'] >> level)
        luW = (lw + 3) // 4
        luH = (lh + 3) // 4
        lPitch = (luW + 31) & ~31
        lRows = (luH + 31) & ~31
        if max(lw, lh) <= 16:
            tail_off = chainOff
            break
        raw = mem.read(t['mipAddr'] + chainOff, lPitch * lRows * bpu)
        if raw is None:
            return None, 'level %d bytes not in trace' % level
        g = texel_grid(fmt, swap16(raw), luW, luH, lPitch)
        if g is None:
            return None, 'level %d short read' % level
        want = downsample(ref, lw, lh)
        s = score(g, want, lw, lh)
        if s > 48.0:
            return None, 'level %d diverges from its parent' % level
        ref = g
        chainOff += lPitch * lRows * bpu
        level += 1
    if tail_off is None:
        return None, 'no tail level inside mipMax'

    raw = mem.read(t['mipAddr'] + tail_off, 32 * 32 * bpu)
    if raw is None:
        return None, 'tail tile bytes not in trace'
    # The whole shared tile, decoded ONCE — a candidate offset is then a slice,
    # which is what makes the brute force affordable.
    tile = texel_grid(fmt, swap16(raw), 32, 32, 32)
    if tile is None:
        return None, 'tail tile short read'

    results = []
    while level <= t['mipMax'] and level < 16:
        lw = max(1, t['w'] >> level)
        lh = max(1, t['h'] >> level)
        luW = (lw + 3) // 4
        luH = (lh + 3) // 4
        if lw < 4 or lh < 4:
            # Below one block the offset cannot be block-aligned at all; deriving
            # those needs sub-block addressing and is out of scope on purpose.
            break
        want = downsample(ref, lw, lh)
        sd = max(grid_sd(want))
        cands = []
        for y0 in range(0, 33 - luH):
            for x0 in range(0, 33 - luW):
                cand = [row[x0 * 4:x0 * 4 + lw]
                        for row in tile[y0 * 4:y0 * 4 + lh]]
                cands.append((score(cand, want, lw, lh), x0, y0))
        cands.sort()
        best_s, bx, by = cands[0]
        margin = (cands[1][0] - best_s) if len(cands) > 1 else 0.0
        results.append({'lw': lw, 'lh': lh, 'fmt': fmt, 'off': (bx, by),
                        'score': best_s, 'margin': margin, 'sd': sd,
                        'level': level})
        if verbose:
            print('  %08X %ux%u fmt=%u L%u (%ux%u tx): best (%2u,%2u) score %.2f '
                  'margin %.2f sd %.1f'
                  % (t['addr'], t['w'], t['h'], fmt, level, lw, lh, bx, by,
                     best_s, margin, sd))
        # Descend: the accepted candidate becomes the next reference. Using the
        # WINNER is deliberate — a wrong winner poisons the level below, which
        # shows up as scattered votes there, i.e. the failure is visible.
        ref = [row[bx * 4:bx * 4 + lw] for row in tile[by * 4:by * 4 + lh]]
        level += 1
    return results, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('traces', nargs='+')
    ap.add_argument('--max-per-trace', type=int, default=0,
                    help='cap textures examined per trace (0 = all)')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    votes = collections.defaultdict(collections.Counter)
    weak = flat = skipped = informative = 0
    skip_reasons = collections.Counter()

    for path in args.traces:
        mem, textures = collect_textures(path)
        print('%s: %d distinct packed-mips DXT textures' % (path, len(textures)))
        if args.max_per_trace:
            textures = textures[:args.max_per_trace]
        for t in textures:
            res, why = derive_one(mem, t, args.verbose)
            if res is None:
                skipped += 1
                skip_reasons[re.sub(r'\d+', 'N', why)] += 1
                continue
            for r in res:
                # A flat reference cannot discriminate offsets; a weak margin means
                # the winner is not distinguishable from the runner-up. Both are
                # counted OUT rather than averaged in.
                if r['sd'] < 4.0:
                    flat += 1
                    continue
                if r['margin'] < 1.0:
                    weak += 1
                    continue
                informative += 1
                votes[(r['lw'], r['lh'], r['fmt'])][r['off']] += 1

    print()
    print('%d informative level votes, %d weak-margin, %d flat-reference, '
          '%d textures skipped' % (informative, weak, flat, skipped))
    for reason, n in skip_reasons.most_common():
        print('  skipped %3d: %s' % (n, reason))
    print()
    print('THE TABLE (level texel extent -> winning block offset in the shared tile):')
    print('%-6s %-6s %-5s %-12s %s' % ('lw', 'lh', 'fmt', 'offset', 'votes'))
    for (lw, lh, fmt), c in sorted(votes.items(), key=lambda kv: (-kv[0][0], -kv[0][1])):
        total = sum(c.values())
        top, n = c.most_common(1)[0]
        others = ' '.join('(%u,%u)x%d' % (o[0], o[1], m)
                          for o, m in c.most_common()[1:4])
        flag = '' if n == total else '   DISAGREES: ' + others
        print('%-6u %-6u %-5u (%2u,%2u)      %d/%d%s'
              % (lw, lh, fmt, top[0], top[1], n, total, flag))
    return 0


if __name__ == '__main__':
    sys.exit(main())
