#!/usr/bin/env python3
"""HARDWARE's per-draw truth out of a `.xtr`: which shaders, which textures, what bytes.

WHY THIS EXISTS
---------------
Phase C part 26 chased a large flat-white ground patch through seven hypotheses and
refuted every one of them — the tone map, a missing texture, constant texture
coordinates, the white dummy (all four heaps poisoned), the clear colour, the EDRAM
surface format, and a texture decoding flat. Every one of those is about an INPUT, and
the inputs are all correct. The operator then confirmed the surface renders correctly on
Xenia, so the defect is ours and the remaining question needs a comparison against
something that gets it right.

`runtime/gpu/vk_renderer.cpp`'s `CZ_VK_DRAW_CENSUS` answers "what did WE bind to each
draw". This is the same question asked of the hardware stream, so the two can be diffed:

  * bound VERTEX and PIXEL shader, named by the SAME FNV-1a hash the runtime and
    `tools/build_shader_spv.sh` use, so a draw here and a draw there have one name;
  * every texture fetch constant the draw could sample — address, extent, format, tiling
    — decoded exactly as `xenos::DecodeTextureFetch` decodes it;
  * the index count, which is how a surface is identified when you cannot click on it.

And, uniquely, THE BYTES. A trace carries `MemoryRead` records with the actual contents
the GPU sampled, so `--dump-texture` writes out the texture hardware really had. That is
the one thing no amount of instrumenting our own runtime can produce: if our upload and
the capture's bytes differ, the defect is in our read; if they match, it is in our
shading, and the last input is eliminated.

WHAT IT ASSUMES, AND WHY EACH ASSUMPTION IS SAFE
------------------------------------------------
The register decode (type-0/1/3 packets, SET_CONSTANT banks, SET_CONSTANT2) is the same
subset `tools/xtr_resolve_census.py` replays and that `runtime/gpu/pm4.cpp` implements —
bank 1 lands the fetch constants at 0x4800, which the two PM4 capture oracles gate. The
shader hash is FNV-1a over the microcode as BIG-ENDIAN bytes, self-tested below against
our own dump filenames: run with `--self-test` and it hashes `~/DR2CZ-troubleshooting/
ucode-dumps` and reports how many reproduce their own names.

NOTE ON XENIA'S DUMPS, which cost an hour: `dump_shaders` writes `.ucode.bin` files
DWORD-SWAPPED relative to the guest's big-endian bytes, so hashing them directly gives a
name that matches nothing. Swap before hashing. The self-test is what proved the function
was right and therefore that the data had to be the problem.

USAGE
    xtr_draw_bindings.py <trace.xtr> [--min-verts N] [--csv out.csv]
                         [--dump-texture ADDR --out DIR] [--self-test]
"""
import argparse
import collections
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402

BE = struct.Struct(">I")
DRAW_OPCODES = (0x22, 0x36)          # DRAW_INDX, DRAW_INDX_2
FETCH_BASE = 0x4800                  # SET_CONSTANT bank 1, as runtime/gpu/pm4.cpp maps it
BANKS = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}


def fnv1a(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def decompress(payload, enc, declen):
    if enc == 0:
        return payload
    import cramjam
    return bytes(cramjam.snappy.decompress_raw(payload))[:declen]


class Memory:
    """The guest memory the trace carries, as {base: bytes} sorted for lookup."""

    def __init__(self):
        self.chunks = []

    def add(self, base, data):
        self.chunks.append((base, base + len(data), data))

    def read(self, addr, length):
        for lo, hi, d in reversed(self.chunks):     # later writes win
            if lo <= addr and addr + length <= hi:
                return d[addr - lo:addr - lo + length]
        return None


def decode_fetch(regs, slot):
    """The six dwords of a texture fetch constant, as xenos::DecodeTextureFetch reads them."""
    d = [regs.get(FETCH_BASE + slot * 6 + i) for i in range(6)]
    if d[0] is None or d[1] is None or d[2] is None:
        return None
    if (d[0] & 3) != 2:                              # type 2 = texture
        return None
    addr = (d[1] >> 12) << 12
    if not addr:
        return None
    return {
        'slot': slot,
        'addr': addr,
        'w': (d[2] & 0x1FFF) + 1,
        'h': ((d[2] >> 13) & 0x1FFF) + 1,
        'fmt': d[1] & 0x3F,
        'tiled': bool((d[0] >> 31) & 1),
        'pitch': (d[0] >> 22) & 0x1FF,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace', nargs='?')
    ap.add_argument('--min-verts', type=int, default=0)
    ap.add_argument('--csv')
    ap.add_argument('--dump-texture')
    ap.add_argument('--out', default='.')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()

    if args.self_test:
        import glob
        ok = bad = 0
        for p in glob.glob(os.path.expanduser('~/DR2CZ-troubleshooting/ucode-dumps/*.ucode')):
            want = os.path.basename(p).split('.')[0].split('_')[1]
            ok, bad = ((ok + 1, bad) if '%016x' % fnv1a(open(p, 'rb').read()) == want
                       else (ok, bad + 1))
        print('hash self-test: %d reproduce their filename, %d do not' % (ok, bad))
        return 0 if bad == 0 else 1

    data, hdr = xtr.open_trace(args.trace)
    print('%s  version %s  title %s  %.1f MB'
          % (args.trace, hdr['version'], hdr['title'], hdr['size'] / 1e6))

    mem = Memory()
    regs = {}
    bound = {0: None, 1: None}        # stage -> (addr, dwords)
    draws = []
    want = int(args.dump_texture, 16) if args.dump_texture else None
    dumped = 0

    for off, cmd in xtr.walk(data, len(data)):
        if cmd in (xtr.CMD_MEMORY_READ, xtr.CMD_MEMORY_WRITE):
            base, enc, elen, dlen = struct.unpack_from('<IIII', data, off + 4)
            try:
                mem.add(base, decompress(data[off + 20:off + 20 + elen], enc, dlen))
            except Exception as e:                    # a bad record must not kill the walk
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
        if ptype == 1:
            if count >= 3:
                regs[header & 0x7FF] = word(1)
                regs[(header >> 11) & 0x7FF] = word(2)
            continue
        if ptype != 3:
            continue
        opcode = (header >> 8) & 0x7F

        if opcode == 0x2D and count >= 2:             # SET_CONSTANT
            base_reg = BANKS.get((word(1) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(1) & 0x7FF
                for i in range(2, count):
                    regs[base_reg + idx + i - 2] = word(i)
        elif opcode in (0x55, 0x56) and count >= 2:   # SET_CONSTANT2, absolute index
            idx = word(1) & 0xFFFF
            for i in range(2, count):
                regs[idx + i - 2] = word(i)
        elif opcode == 0x27 and count >= 3:           # IM_LOAD: stage in low 2 bits
            stage = word(1) & 3
            addr = word(1) & ~3
            bound[stage] = (addr, word(2) & 0xFFFF)
        elif opcode == 0x2B and count >= 3:           # IM_LOAD_IMMEDIATE: inline microcode
            stage = word(1) & 3
            size = word(2) & 0xFFFF
            code = b''.join(struct.pack('>I', word(3 + i))
                            for i in range(min(size, count - 3)))
            bound[stage] = ('inline', code)
        elif opcode in DRAW_OPCODES and count >= 2:
            # The initiator is body dword 1 for DRAW_INDX (dword 0 is viz-query info) and
            # body dword 0 for DRAW_INDX_2 — the same split runtime/gpu/pm4.cpp makes, and
            # getting it wrong reads the viz-query word as a vertex count, which silently
            # reports every draw as tiny.
            init_at = 2 if opcode == 0x22 else 1
            if count <= init_at:
                continue
            init = word(init_at)
            idx_count = init >> 16
            prim = init & 0x3F
            names = {}
            for stage, label in ((0, 'vs'), (1, 'ps')):
                b = bound[stage]
                if b is None:
                    continue
                if b[0] == 'inline':
                    names[label] = '%s_%016x' % (label, fnv1a(b[1]))
                else:
                    code = mem.read(b[0], b[1] * 4)
                    if code:
                        names[label] = '%s_%016x' % (label, fnv1a(code))
            tex = [t for t in (decode_fetch(regs, s) for s in range(32)) if t]
            draws.append((len(draws), idx_count, names, tex, prim))
            if want:
                for t in tex:
                    if t['addr'] == want:
                        # SIZE BY FORMAT. Asking for w*h*4 of a DXT1 overruns the
                        # record and the bounds check silently returns nothing, which
                        # reads as "the trace does not carry this texture" when it
                        # carries it perfectly.
                        bpp = {18: 0.5, 19: 1.0, 20: 1.0, 6: 4.0, 2: 1.0,
                               22: 4.0, 26: 8.0}.get(t['fmt'], 4.0)
                        blob = mem.read(t['addr'], int(t['w'] * t['h'] * bpp))
                        if blob:
                            p = os.path.join(args.out, 'tex_%08X_%ux%u_f%u.bin'
                                             % (t['addr'], t['w'], t['h'], t['fmt']))
                            open(p, 'wb').write(blob)
                            dumped += 1

    print('draws: %d   distinct shader pairs: %d'
          % (len(draws), len({(d[2].get('vs'), d[2].get('ps')) for d in draws})))
    if want:
        print('dumped %d copies of texture %08X to %s' % (dumped, want, args.out))

    shown = 0
    for i, n, names, tex, prim in sorted(draws, key=lambda d: -d[1]):
        if n < args.min_verts or shown >= 12:
            continue
        shown += 1
        print('  draw %-5d verts=%-6d prim=%u vs=%s ps=%s'
              % (i, n, prim, names.get('vs', '?'), names.get('ps', '?')))
        for t in tex[:8]:
            print('      s%-2d %08X %4ux%-4u fmt=%-3u tiled=%u pitchBlk=%u'
                  % (t['slot'], t['addr'], t['w'], t['h'], t['fmt'], t['tiled'],
                     t['pitch']))

    if args.csv:
        with open(args.csv, 'w') as f:
            f.write('draw,verts,vs,ps,slot,addr,w,h,fmt\n')
            for i, n, names, tex, prim in draws:
                for t in tex:
                    f.write('%d,%d,%s,%s,%d,%08X,%d,%d,%d\n'
                            % (i, n, names.get('vs', ''), names.get('ps', ''),
                               t['slot'], t['addr'], t['w'], t['h'], t['fmt']))
        print('wrote %s' % args.csv)
    return 0


if __name__ == '__main__':
    sys.exit(main())
