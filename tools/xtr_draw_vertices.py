#!/usr/bin/env python3
"""HARDWARE's VERTEX DATA for a named draw, decoded the way our renderer decodes ours.

WHY THIS EXISTS
---------------
The white ground patch (docs/open-items.md 00f/00g) survived seven refuted hypotheses and
then a full input comparison against the round-2 captures: same vertex shader hash, same
pixel shader hash, same vertex count, same bound textures, same texture CONTENTS, same
render state — and a different picture. That comparison covered every input except one.

**The vertex data is the last uncompared input**, and there is already an anomaly on file
for this shader: `vs_36eef2c94b4a065c` declares two float2 attributes, one at fetch slot
94 dword offset 0 and one at slot 93 dword offset 1, which our own `CZ_VK_DRAW_PROBE`
reported as decoding identically. Two attributes that should be different texture
coordinate sets and are not is exactly what would texture a ground mesh with one set
where the material wanted two.

A single-frame `.xtr` carries a `MemoryRead` with the ACTUAL BYTES of every vertex buffer
the GPU fetched, so hardware's streams are available and this prints them in the same
shape `CZ_VK_DRAW_PROBE` prints ours: per attribute, per vertex, components decoded by
format. The two transcripts can then be read side by side.

WHAT IT SHARES WITH THE RUNTIME, DELIBERATELY
---------------------------------------------
The vertex fetch constant decode (`address = dword0 & ~3`, `size = (dword1 >> 2) &
0xFFFFFF` dwords, `endian = dword1 & 3`), the attribute layout (our own sidecar's
`attributes` array, which is what the renderer binds from), the endian unswap and the
per-format component count are all transcribed from `runtime/gpu/xenos.h` and
`runtime/gpu/vk_renderer.cpp`. That is the point: if the two decodes agreed only by
coincidence the comparison would mean nothing, and if one of them is wrong it is wrong on
both sides and the diff still shows the data.

The 24-bit mask on the size field is load-bearing — see the comment on `DecodeVertexFetch`
in `xenos.h`, where dropping it turned an 85-dword stream into 67 million.

USAGE
    xtr_draw_vertices.py <trace.xtr> --vs <hash> [--ps <hash>] [--min-verts N]
                         [--verts N] [--draws N] [--spv assets/shader_spv]
"""
import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from xtr_draw_bindings import (BANKS, DRAW_OPCODES, Memory, decompress,  # noqa: E402
                               fnv1a)

BE = struct.Struct(">I")
FETCH_BASE = 0x4800

# Dwords per element, transcribed from VertexFormatDwords in vk_renderer.cpp.
FMT_DWORDS = {6: 1, 7: 1, 16: 1, 25: 1, 31: 1, 33: 1, 36: 1,
              26: 2, 32: 2, 37: 2, 57: 3, 38: 4}
FMT_F32 = (36, 37, 57, 38)


def unswap(buf, endian):
    """CopySwapped from vk_renderer.cpp, over a bytes object."""
    out = bytearray(buf)
    if endian & 3 == 1:
        for i in range(0, len(out) - 1, 2):
            out[i], out[i + 1] = out[i + 1], out[i]
    elif endian & 3 == 2:
        for i in range(0, len(out) - 3, 4):
            out[i:i + 4] = out[i:i + 4][::-1]
    elif endian & 3 == 3:
        for i in range(0, len(out) - 3, 4):
            out[i:i + 4] = out[i + 2:i + 4] + out[i:i + 2]
    return bytes(out)


def decode_vertex_fetch(regs, slot):
    d0 = regs.get(FETCH_BASE + slot * 2)
    d1 = regs.get(FETCH_BASE + slot * 2 + 1)
    if d0 is None or d1 is None:
        return None
    return {'address': d0 & ~3, 'sizeDwords': (d1 >> 2) & 0xFFFFFF,
            'endian': d1 & 3, 'type': d0 & 3}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--vs', required=True, help='vertex shader hash, 16 hex digits')
    ap.add_argument('--ps', help='optional pixel shader hash to narrow the draw')
    ap.add_argument('--min-verts', type=int, default=0)
    ap.add_argument('--verts', type=int, default=6, help='vertices to print per attribute')
    ap.add_argument('--draws', type=int, default=1, help='how many matching draws to print')
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--vc', default='0,1,2,3,4,5,6,8,9,10',
                    help='vertex-shader ALU float4 registers to print')
    ap.add_argument('--pc', default='0,1,2,3,255',
                    help='pixel-shader ALU float4 registers to print (window 256+n)')
    args = ap.parse_args()

    side = Path(args.spv) / ('vs_%s.meta.json' % args.vs)
    attrs = json.load(open(side))['attributes']
    print('%s: %d attributes' % (side.name, len(attrs)))

    data, hdr = xtr.open_trace(args.trace)
    mem = Memory()
    regs = {}
    bound = {0: None, 1: None}
    unknown = set()   # registers the trace cannot reconstruct
    draws = 0
    printed = 0

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
        if ptype == 1:
            if count >= 3:
                regs[header & 0x7FF] = word(1)
                regs[(header >> 11) & 0x7FF] = word(2)
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
        elif opcode == 0x2F and count >= 4:
            # LOAD_ALU_CONSTANT — see the note in xtr_draw_bindings.py. This title sets
            # its shader constants from MEMORY, 620 packets to 36 SET_CONSTANTs in
            # w1_spawn, so without this every constant printed below is a zero that
            # means nothing.
            #
            # AND WHEN THE SOURCE BYTES ARE NOT IN THE TRACE, THE REGISTER BECOMES
            # UNKNOWN RATHER THAN STALE. 81 of w1_spawn's 620 loads read an address the
            # capture never recorded — the title cycles its constant buffers through many
            # addresses and Xenia records only the ranges it saw sampled. Leaving the
            # previous value in place would print a number that looks like hardware's
            # answer and is a leftover from some earlier draw, which is exactly the
            # failure this comparison exists to avoid. pc(253..255) on the ground draw is
            # that case: it printed (0,0,0,0) for a register the shader uses as its
            # literal 0 and 1, i.e. a value the shader could not have run with.
            base_reg = BANKS.get((word(2) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(2) & 0x7FF
                size = word(3) & 0xFFF
                blob = mem.read(word(1) & 0x3FFFFFFC, size * 4)
                for i in range(size):
                    if blob:
                        regs[base_reg + idx + i] = BE.unpack_from(blob, i * 4)[0]
                        unknown.discard(base_reg + idx + i)
                    else:
                        unknown.add(base_reg + idx + i)
        elif opcode == 0x27 and count >= 3:
            bound[word(1) & 3] = (word(1) & ~3, word(2) & 0xFFFF)
        elif opcode == 0x2B and count >= 3:
            size = word(2) & 0xFFFF
            bound[word(1) & 3] = ('inline', b''.join(
                struct.pack('>I', word(3 + i)) for i in range(min(size, count - 3))))
        elif opcode in DRAW_OPCODES and count >= 2:
            init_at = 2 if opcode == 0x22 else 1
            if count <= init_at:
                continue
            draws += 1
            nverts = word(init_at) >> 16
            if nverts < args.min_verts or printed >= args.draws:
                continue
            names = {}
            for stage, label in ((0, 'vs'), (1, 'ps')):
                b = bound[stage]
                if b is None:
                    continue
                code = b[1] if b[0] == 'inline' else mem.read(b[0], b[1] * 4)
                if code:
                    names[label] = '%016x' % fnv1a(code)
            if names.get('vs') != args.vs:
                continue
            if args.ps and names.get('ps') != args.ps:
                continue
            printed += 1
            print('\ndraw %d  verts=%d  vs=%s ps=%s'
                  % (draws, nverts, names.get('vs'), names.get('ps')))
            # THE ALU CONSTANTS, in the same registers `CZ_VK_DRAW_PROBE` prints. They
            # are an INPUT to the draw exactly as the textures and the vertex streams
            # are, and part 26's "same shader, same textures, same state, different
            # picture" comparison never covered them. Bank 0 lands at 0x4000, four dwords
            # per float4, with the pixel shader's window at float4 256+n — the layout
            # `vk_renderer.cpp` reads and `pm4.cpp` writes.
            # THE WINDOW THE GUEST NAMES, not the 0/256 default. `SQ_VS_CONST` (0x2307)
            # and `SQ_PS_CONST` (0x2308) each carry a base float4 in their low 9 bits,
            # and this title does move them (our runtime counts 24 draws a run that do).
            # Reading `pc(n)` at a fixed 256 when the guest said otherwise compares two
            # different registers and calls the difference a finding.
            vsc, psc = regs.get(0x2307), regs.get(0x2308)
            print('  SQ_VS_CONST=%s (base=%s)  SQ_PS_CONST=%s (base=%s)'
                  % ('%08X' % vsc if vsc is not None else '?',
                     vsc & 0x1FF if vsc is not None else '?',
                     '%08X' % psc if psc is not None else '?',
                     psc & 0x1FF if psc is not None else '?'))
            for label, spec, base4 in (('vc', args.vc, (vsc or 0) & 0x1FF),
                                       ('pc', args.pc, (psc or 0x100) & 0x1FF)):
                for r in [int(x) for x in spec.split(',') if x.strip()]:
                    at = BANKS[0] + (base4 + r) * 4
                    vals = [regs.get(at + k) for k in range(4)]
                    if any(at + k in unknown for k in range(4)):
                        print('  %s(%3d) = UNRECOVERABLE — last written by a '
                              'LOAD_ALU_CONSTANT whose source bytes the trace does not '
                              'carry' % (label, r))
                        continue
                    if all(v is None for v in vals):
                        print('  %s(%3d) = never written in this trace' % (label, r))
                        continue
                    print('  %s(%3d) = %s'
                          % (label, r, ' '.join(
                              '%12.4f' % struct.unpack('<f', struct.pack('<I', v))[0]
                              if v is not None else '           ?' for v in vals)))
            for a in attrs:
                vf = decode_vertex_fetch(regs, a['fetchSlot'])
                if vf is None:
                    print('  loc%-3d slot=%-3d  FETCH CONSTANT NOT SET IN THIS TRACE'
                          % (a['location'], a['fetchSlot']))
                    continue
                print('  loc%-3d slot=%-3d fmt=%-3d int=%d stride=%-2d off=%-2d  '
                      'addr=%08X size=%d dwords endian=%d'
                      % (a['location'], a['fetchSlot'], a['format'], a['integer'],
                         a['strideDwords'], a['offsetDwords'], vf['address'],
                         vf['sizeDwords'], vf['endian']))
                comps = FMT_DWORDS.get(a['format'], 1)
                line = '      '
                # READ PER VERTEX, NOT THE WHOLE STREAM. A trace records the ranges the
                # GPU actually touched, so a 65,852-dword stream can arrive as several
                # `MemoryRead` chunks (or as only the part that was sampled) and asking
                # for all of it at once fails the bounds check and reads as "the trace
                # does not carry this buffer". It carries it; it just does not carry it
                # in one piece. Four attributes of five reported NOT IN THE TRACE that way.
                for v in range(args.verts):
                    dw = v * a['strideDwords'] + a['offsetDwords']
                    if dw + comps > vf['sizeDwords']:
                        break
                    chunk = mem.read(vf['address'] + dw * 4, comps * 4)
                    if chunk is None:
                        line += ' v%d(not in the trace)' % v
                        continue
                    raw = unswap(chunk, vf['endian'])
                    if a['format'] in FMT_F32:
                        vals = ','.join('%.5f' % struct.unpack_from('<f', raw, k * 4)[0]
                                        for k in range(comps))
                    else:
                        vals = ','.join('%08X' % struct.unpack_from('<I', raw, k * 4)[0]
                                        for k in range(comps))
                    line += ' v%d(%s)' % (v, vals)
                print(line)
    if not printed:
        print('no draw matched vs=%s%s over %d draws'
              % (args.vs, ' ps=%s' % args.ps if args.ps else '', draws))
    return 0


if __name__ == '__main__':
    sys.exit(main())
