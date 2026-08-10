#!/usr/bin/env python3
"""Does HARDWARE have the shader-versus-constant dimension disagreement that we have?

WHY THIS EXISTS
---------------
Our renderer declines ~14,670 cube fetches a run to the 1x1 white dummy because the
shader indexes the cube descriptor array while the guest's own fetch constant describes a
2D surface (`runtime/gpu/vk_renderer.cpp`, "the SHADER and the FETCH CONSTANT disagree").
That decline is the confirmed mechanism behind the operator's white glass and blown-out
bathroom window (docs/open-items.md 00f/00g).

Part 26 read the round-2 captures and reported "414 of 414 cube-declared draws on
hardware read stack depth 5 and dimension 3 — no disagreements at all", i.e. that we
manufacture the disagreement ourselves. That claim selected draws by whether a
CUBE-DECLARING SHADER was bound and then looked at the slots whose constants already read
cube — which cannot find a disagreement, because a disagreeing slot reads 2D and is
therefore not in the population being counted. **It is gotcha 25 in a new disguise: a
filter that cannot match the thing it is testing for.**

This asks the question the way the runtime asks it, per fetch rather than per draw:

    for every draw, for the vertex and pixel shader actually bound,
      for every fetch slot THE SHADER declares (from our own sidecar, the same
      `tfetchConsts`/`tfetchDims` arrays the renderer binds from),
        does the guest's fetch constant at that slot declare the same dimension?

Hardware and our runtime then answer the same question over the same shaders, and the
answer is directly comparable to the runtime's own counters.

USAGE
    xtr_cube_agreement.py <trace.xtr> [--spv assets/shader_spv] [--examples N]
"""
import argparse
import collections
import json
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from xtr_draw_bindings import (BANKS, DRAW_OPCODES, Memory, decode_fetch,  # noqa: E402
                               decompress, fnv1a)

BE = struct.Struct(">I")
DIMNAME = {0: '1D', 1: '2D', 2: '3D', 3: 'cube'}


def load_sidecars(root):
    """hash -> (tfetchConsts, tfetchDims), for every shader in our SPIR-V cache."""
    out = {}
    for p in Path(root).glob('*.meta.json'):
        try:
            d = json.load(open(p))
        except Exception:
            continue
        consts = d.get('tfetchConsts') or []
        dims = d.get('tfetchDims') or []
        # POSITIONAL arrays, exactly as the renderer treats them: a sidecar whose two
        # arrays are different lengths carries no usable dimension and the renderer falls
        # back to 2D. Mirror that here rather than guessing, or this tool would report
        # agreement the runtime does not have.
        if len(dims) != len(consts):
            dims = [1] * len(consts)
        out[p.name.split('.')[0].split('_')[1]] = (consts, dims)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--examples', type=int, default=6)
    args = ap.parse_args()

    meta = load_sidecars(args.spv)
    print('%d sidecars' % len(meta))

    data, hdr = xtr.open_trace(args.trace)
    mem = Memory()
    regs = {}
    bound = {0: None, 1: None}
    agree = collections.Counter()          # (shaderDim, constDim) -> fetches
    unknown_shaders = collections.Counter()
    examples = []
    draws = 0

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
            # LOAD_ALU_CONSTANT — see the note in xtr_draw_bindings.py. It can target
            # BANK 1, the fetch constants, so leaving it out could hide exactly the
            # cube descriptors this tool exists to count.
            base_reg = BANKS.get((word(2) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(2) & 0x7FF
                blob = mem.read(word(1) & 0x3FFFFFFC, (word(3) & 0xFFF) * 4)
                if blob:
                    for i in range(word(3) & 0xFFF):
                        regs[base_reg + idx + i] = BE.unpack_from(blob, i * 4)[0]
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
            for stage, label in ((0, 'vs'), (1, 'ps')):
                b = bound[stage]
                if b is None:
                    continue
                code = b[1] if b[0] == 'inline' else mem.read(b[0], b[1] * 4)
                if not code:
                    continue
                name = '%016x' % fnv1a(code)
                m = meta.get(name)
                if m is None:
                    unknown_shaders[label + '_' + name] += 1
                    continue
                consts, dims = m
                for slot, sdim in zip(consts, dims):
                    # The renderer's own bound: slots >= 16 are skipped there, so skipping
                    # them here keeps the two populations identical.
                    if slot >= 16:
                        continue
                    t = decode_fetch(regs, slot)
                    if t is None:
                        continue
                    agree[(sdim, t['dim'])] += 1
                    if sdim != t['dim'] and len(examples) < args.examples:
                        examples.append(
                            'draw %d verts=%d %s_%s slot=%d shaderDim=%s constDim=%s '
                            'addr=%08X %ux%u fmt=%u depth=%u | %s'
                            % (draws, nverts, label, name, slot, DIMNAME[sdim],
                               DIMNAME[t['dim']], t['addr'], t['w'], t['h'], t['fmt'],
                               t['depth'],
                               ' '.join('%08X' % (d or 0) for d in t['dwords'])))

    print('%s: %d draws' % (os.path.basename(args.trace), draws))
    if unknown_shaders:
        print('shaders bound but NOT in our cache: %d distinct, %d binds'
              % (len(unknown_shaders), sum(unknown_shaders.values())))
    total = sum(agree.values())
    print('\nfetches by (dimension the SHADER declares, dimension the CONSTANT declares):')
    for (s, c), n in sorted(agree.items(), key=lambda kv: -kv[1]):
        print('  shader %-4s  constant %-4s  %9d  %6.2f%%%s'
              % (DIMNAME.get(s, s), DIMNAME.get(c, c), n, 100.0 * n / max(total, 1),
                 '   <-- DISAGREEMENT' if s != c else ''))
    bad = sum(n for (s, c), n in agree.items() if s != c)
    print('\n%d of %d fetches disagree (%.2f%%)' % (bad, total, 100.0 * bad / max(total, 1)))
    for e in examples:
        print('  ' + e)
    return 0


if __name__ == '__main__':
    sys.exit(main())
