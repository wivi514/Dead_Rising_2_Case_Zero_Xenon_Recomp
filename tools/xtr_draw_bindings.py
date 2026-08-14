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
    """The guest memory the trace carries, as {base: bytes} sorted for lookup.

    EVERY CHUNK CARRIES THE WALK POSITION IT ARRIVED AT, and reads can report it.
    A trace's memory records are SNAPSHOTS taken at a moment, not a live view: Xenia
    dumps the bytes behind a resource the first time the GPU reads it, and never again.
    So for an address the title RESOLVES INTO during the traced frame, the only snapshot
    is the one taken BEFORE the resolve — i.e. the previous frame's contents.

    That cost part 32 the whole of §6bc's hardware oracle. `--dump-texture 1812F000`
    on `w1_spawn` returns 16 MB that reads as a shadow-cascade atlas 96.5% populated,
    and is in fact the previous frame's COMPOSITED SCENE — the HUD text is legible in
    it. Detiled and viewed, "8 KILLED" is readable across the middle of the "atlas".
    The number was quoted as hardware's shadow map for a whole part.
    """

    def __init__(self):
        self.chunks = []

    def add(self, base, data, seq=None):
        self.chunks.append((base, base + len(data), data, seq))

    def read(self, addr, length):
        for lo, hi, d, _ in reversed(self.chunks):  # later writes win
            if lo <= addr and addr + length <= hi:
                return d[addr - lo:addr - lo + length]
        return None

    def sources(self, addr, length):
        """Every chunk that could serve this read, newest last, as (seq, base, len)."""
        return [(seq, lo, hi - lo) for lo, hi, _, seq in self.chunks
                if lo <= addr and addr + length <= hi]


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
        # The DIMENSION and the STACK DEPTH, decoded exactly as the runtime decodes them
        # (dword5 bits 9..10, dword2 bits 26..31 stored minus one — both LOCATED BY
        # CENSUS in part 25, not from documentation). They are here because part 27's
        # question is whether hardware's constant for a given slot says what ours says:
        # we decline ~14,670 cube fetches a run for a shader/constant disagreement, and
        # the capture is the only thing that can say whether hardware has one too.
        'dim': (d[5] >> 9) & 3 if d[5] is not None else None,
        'depth': ((d[2] >> 26) & 0x3F) + 1,
        # THE MIP CHAIN, which this project had never read from either side. A Xenos
        # fetch constant names TWO addresses: dword1's base address, which holds level 0
        # only, and dword5's separate MIP ADDRESS, which holds levels 1..n. Which of
        # those levels the sampler may use is clamped by dword4's mip_min_level /
        # mip_max_level. A streaming title raises mip_min_level while the big levels are
        # still on disc, so a fetch whose mipMin is above zero is the guest SAYING "do
        # not read level 0, it is not resident" — and a renderer that uploads one level
        # and ignores the clamp reads exactly the memory the guest just disclaimed.
        # These four fields sit beside `dim` because they share dword5's layout:
        # dimension at bits 9..10 (measured in part 25) puts packed_mips at 11 and the
        # mip address at 12..31, which is the layout that makes both readings consistent.
        'mipMin': (d[4] >> 2) & 0xF if d[4] is not None else None,
        'mipMax': (d[4] >> 6) & 0xF if d[4] is not None else None,
        'mipAddr': ((d[5] >> 12) << 12) if d[5] is not None else None,
        'packedMips': (d[5] >> 11) & 1 if d[5] is not None else None,
        'dwords': d,
    }


def tiled_footprint(t, bpp):
    """How many bytes this texture actually OCCUPIES, which is not w*h*bpp.

    A tiled Xenos surface is stored in 32x32-unit macro tiles, so both its pitch and
    its row count are rounded up to 32 units — the same rule
    runtime/gpu/vk_renderer.cpp's untiler applies. Sizing a dump at `w * h * bpp`
    therefore SHORT-READS every tiled texture whose height is not a multiple of the
    tile: part 39's 256x64 DXT1 sign wrote 8 KB of a 16 KB footprint and rendered the
    right half magenta, which reads as a decode failure rather than a truncated dump.
    (The md5 pairing it was used for still held, because our own live_texdump.py sizes
    itself the same wrong way and both sides truncated identically — but that is luck,
    not a property of the method, and live_texdump.py wants the same fix.)
    """
    unit = 4 if t['fmt'] in (18, 19, 20, 26) else 1        # DXT block edge, in texels
    uw = (t['w'] + unit - 1) // unit
    uh = (t['h'] + unit - 1) // unit
    pitch = t['pitch'] * 32 // unit if t['pitch'] else ((uw + 31) & ~31)
    if not t['tiled']:
        return int(pitch * uh * bpp * unit * unit)
    return int(((pitch + 31) & ~31) * ((uh + 31) & ~31) * bpp * unit * unit)


ALPHA_FUNCS = ('NEVER', 'LESS', 'EQUAL', 'LEQUAL', 'GREATER', 'NOTEQUAL', 'GEQUAL',
               'ALWAYS')


def describe_colorcontrol(cc, aref):
    """RB_COLORCONTROL in words. Bits 0..2 compare func, 3 alpha-test enable, 4
    alpha-to-mask enable — the same decode runtime/gpu/vk_renderer.cpp makes, so a
    hardware draw and one of ours can be read in one vocabulary."""
    bits = []
    if cc & 0x8:
        bits.append('ALPHATEST=%s ref=%.3f'
                    % (ALPHA_FUNCS[cc & 7], struct.unpack('<f', struct.pack('<I', aref))[0]))
    if cc & 0x10:
        bits.append('ALPHA_TO_MASK')
    return 'RB_COLORCONTROL=%08X%s' % (cc, ('  ' + ' '.join(bits)) if bits else '')


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
    seq = 0
    # Every RESOLVE destination the trace issues, and the walk position of the FIRST
    # one. A dumped texture whose address is in here was PRODUCED inside the traced
    # frame, so any memory snapshot taken before that position is the previous frame's
    # contents at that address, not the surface the draw sampled.
    resolve_dests = {}
    dump_provenance = []

    for off, cmd in xtr.walk(data, len(data)):
        seq += 1
        if cmd in (xtr.CMD_MEMORY_READ, xtr.CMD_MEMORY_WRITE):
            base, enc, elen, dlen = struct.unpack_from('<IIII', data, off + 4)
            try:
                mem.add(base, decompress(data[off + 20:off + 20 + elen], enc, dlen),
                        seq)
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
        elif opcode == 0x2F and count >= 4:           # LOAD_ALU_CONSTANT: from MEMORY
            # THE DOMINANT CONSTANT PATH IN THIS TITLE, and it was missing here for a
            # whole part. `w1_spawn.xtr` carries 620 of these against 36 SET_CONSTANTs,
            # so a replay without it reads most of the ALU constant file as ZERO — and
            # zero is indistinguishable from "the guest wrote zero". It was caught
            # because the ground pixel shader uses `c255.w` as its literal 1.0 and the
            # replay said c255 was all zeros, which would make the shader compute
            # nonsense: an impossible value is the only kind of absence a replay
            # announces by itself (gotcha 25).
            base_reg = BANKS.get((word(2) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(2) & 0x7FF
                src = word(1) & 0x3FFFFFFC
                size = word(3) & 0xFFF
                blob = mem.read(src, size * 4)
                if blob:
                    for i in range(size):
                        regs[base_reg + idx + i] = BE.unpack_from(blob, i * 4)[0]
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
            # A RESOLVE is a draw issued while RB_MODECONTROL's edram_mode is 6
            # (kCopy) — the same test tools/xtr_resolve_census.py makes. Recorded here
            # only so the texture dump can say whether the bytes it is about to write
            # predate the resolve that produced them.
            if (regs.get(0x2208, 0) & 7) == 6:            # RB_MODECONTROL, 6 = kCopy
                d = regs.get(0x2319, 0) & 0xFFFFFFFC     # RB_COPY_DEST_BASE
                if d:
                    resolve_dests.setdefault(d, seq)
            tex = [t for t in (decode_fetch(regs, s) for s in range(32)) if t]
            # RB_COLORCONTROL, verbatim, because it is the register that says how a
            # cutout happens. Carried raw rather than pre-decoded so a reading nobody
            # has thought of yet is still recoverable from the CSV.
            #
            # 0x2202, NOT 0x2205 (part 40). This tool shipped reading 0x2205 — which is
            # RB_BLENDCONTROL1 — and part 39's item-0t refutation ("hardware enables
            # neither the alpha test nor alpha-to-mask across 40,703 draws") was a
            # census of that wrong register. At 0x2202 every value carries the 0xAA
            # alpha-to-mask offset signature in its top byte, and the R4 traces enable
            # the alpha test on hundreds of draws per frame — the foliage cutout part 38
            # went looking for. See kRbColorControl in runtime/gpu/xenos.h.
            draws.append((len(draws), idx_count, names, tex, prim,
                          regs.get(0x2202, 0), regs.get(0x210E, 0),
                          regs.get(0x2201, 0), regs.get(0x2200, 0)))
            if want:
                for t in tex:
                    if t['addr'] == want:
                        # SIZE BY FORMAT. Asking for w*h*4 of a DXT1 overruns the
                        # record and the bounds check silently returns nothing, which
                        # reads as "the trace does not carry this texture" when it
                        # carries it perfectly.
                        bpp = {18: 0.5, 19: 1.0, 20: 1.0, 6: 4.0, 2: 1.0,
                               22: 4.0, 26: 8.0}.get(t['fmt'], 4.0)
                        length = tiled_footprint(t, bpp)
                        blob = mem.read(t['addr'], length)
                        if blob:
                            p = os.path.join(args.out, 'tex_%08X_%ux%u_f%u.bin'
                                             % (t['addr'], t['w'], t['h'], t['fmt']))
                            open(p, 'wb').write(blob)
                            dumped += 1
                            dump_provenance.append(
                                (t['addr'], length, t['w'], t['h'], t['fmt'],
                                 mem.sources(t['addr'], length)))

    print('draws: %d   distinct shader pairs: %d'
          % (len(draws), len({(d[2].get('vs'), d[2].get('ps')) for d in draws})))
    stale = False
    if want:
        print('dumped %d copies of texture %08X to %s' % (dumped, want, args.out))
        # THE GATE. A trace's memory records are snapshots with a TIME, and this tool
        # serves whichever one happens to contain the range. If the address is also a
        # RESOLVE DESTINATION in the same trace and every covering snapshot predates the
        # first resolve to it, the bytes just written are what was at that address
        # BEFORE the surface was produced — a different picture entirely, and one that
        # looks perfectly plausible. Exit 2 rather than a warning, because the failure
        # mode is a confident wrong number: §6bc quoted 16 MB of the previous frame's
        # composited scene as hardware's shadow atlas.
        stale = False
        for addr, length, w, h, fmt, srcs in dump_provenance[-1:]:
            made_at = resolve_dests.get(addr)
            newest = max((q for q, _, _ in srcs if q is not None), default=None)
            print('  %08X %ux%u fmt=%u: %d memory snapshot(s) cover it%s'
                  % (addr, w, h, fmt, len(srcs),
                     '' if newest is None else ', newest at walk position %d' % newest))
            if made_at is None:
                print('  and the trace issues no RESOLVE to this address, so the bytes '
                      'are guest memory the title uploaded — a sound oracle.')
                continue
            print('  BUT the trace RESOLVES INTO %08X, first at walk position %d.'
                  % (addr, made_at))
            if newest is not None and newest < made_at:
                stale = True
                print('  *** EVERY SNAPSHOT PREDATES THAT RESOLVE. The bytes written '
                      'are what was at this address BEFORE the surface was produced — '
                      'in this title, typically the previous frame\'s composited scene. '
                      'They are NOT hardware\'s copy of the surface, and the capture '
                      'cannot supply it: a surface the GPU produces inside the traced '
                      'frame is never snapshotted again. ***')
    shown = 0
    for i, n, names, tex, prim, cc, aref, bl, dc in sorted(draws, key=lambda d: -d[1]):
        if n < args.min_verts or shown >= 12:
            continue
        shown += 1
        print('  draw %-5d verts=%-6d prim=%u vs=%s ps=%s  %s'
              % (i, n, prim, names.get('vs', '?'), names.get('ps', '?'),
                 describe_colorcontrol(cc, aref) +
                 '  RB_BLENDCONTROL0=%08X RB_DEPTHCONTROL=%08X' % (bl, dc)))
        for t in tex[:8]:
            print('      s%-2d %08X %4ux%-4u fmt=%-3u dim=%s depth=%u tiled=%u pitchBlk=%u '
                  'mip=%s..%s mipAddr=%08X packed=%s'
                  % (t['slot'], t['addr'], t['w'], t['h'], t['fmt'], t['dim'],
                     t['depth'], t['tiled'], t['pitch'], t['mipMin'], t['mipMax'],
                     t['mipAddr'] or 0, t['packedMips']))

    if args.csv:
        with open(args.csv, 'w') as f:
            f.write('draw,verts,vs,ps,slot,addr,w,h,fmt,dim,depth,'
                    'mipMin,mipMax,mipAddr,packedMips,colorControl,alphaRef,'
                    'blendControl0,depthControl\n')
            for i, n, names, tex, prim, cc, aref, bl, dc in draws:
                for t in tex:
                    f.write('%d,%d,%s,%s,%d,%08X,%d,%d,%d,%s,%d,%s,%s,%08X,%s,%08X,%08X,%08X,%08X\n'
                            % (i, n, names.get('vs', ''), names.get('ps', ''),
                               t['slot'], t['addr'], t['w'], t['h'], t['fmt'],
                               t['dim'], t['depth'], t['mipMin'], t['mipMax'],
                               t['mipAddr'] or 0, t['packedMips'], cc, aref, bl, dc))
        print('wrote %s' % args.csv)
    # The staleness gate exits non-zero AFTER the rest of the output, so a caller who
    # wanted the census as well still gets it. A warning buried in a long listing is a
    # warning people learn to skip; an exit code is one a script cannot.
    return 2 if stale else 0


if __name__ == '__main__':
    sys.exit(main())
