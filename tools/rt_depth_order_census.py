#!/usr/bin/env python3
"""WHEN, IN A HARDWARE FRAME, IS THE SCENE DEPTH BUFFER ACTUALLY FULL?

WHY THIS EXISTS
---------------
Route (b) computes a ray-traced shadow factor per receiving pixel by reconstructing the
world position from the SCENE DEPTH BUFFER. The renderer fires that pass at the title's
own first shadow-atlas-sampling draw, and the argument for that trigger — written into
`runtime/gpu/rt_factor.hlsl`'s header — is:

    "This title issues a real Z PREPASS — 233,155 depth-only draws against 148,150
     colour-mode ones over a boot (§6u) — so by the time its own first shadow-sampling
     draw is recorded, the depth buffer holds the finished scene depth."

Part 65's debug ladder then measured the sampled depth as **exactly 1.0 everywhere**
(mode 8's dither says it does not vary; mode 9's mean says the value is 1.0), which is
precisely what a freshly CLEARED depth buffer reads. Those two statements cannot both be
true, and the ladder could not tell them apart from inside the shader: a broken
descriptor and a genuinely-cleared buffer produce the same uniform 1.0.

THE ORACLE IS THE CAPTURE, NOT OUR RUNTIME
------------------------------------------
A `.xtr` world trace carries hardware's whole draw stream for one frame IN ORDER, with
the register file at every draw. So the question "had anything written the scene depth
surface before the first atlas-sampling draw?" is answerable offline, against hardware,
with no instrumented run at all — the same move that turned part 65's shader census from
"needs a runtime probe" into twenty files already on disc (gotcha 387).

The §6u count that the trigger rests on is a count over a WHOLE BOOT, which says nothing
about ORDER inside a frame. This tool asks the ordering question directly.

WHAT IT MEASURES, PER TRACE
---------------------------
Walking the draws in stream order, for each draw it records the depth surface it renders
into (RB_DEPTH_INFO's EDRAM tile base), whether it can write depth at all
(RB_DEPTHCONTROL's z-write bit), its colour write mask (RB_COLOR_MASK) and its edram
mode (RB_MODECONTROL), plus whether the bound pixel shader DECLARES a fetch slot holding
the cascade atlas.

Then, for the first atlas-sampling draw, it prints how many earlier draws wrote depth
into THAT SAME depth surface. That number is the whole finding:

  * hundreds  -> a real scene Z prepass precedes the shadow draws and the trigger is
                 sound; the uniform 1.0 is a defect in the sampling path.
  * ~zero     -> the depth buffer the factor pass reads is still at its clear value when
                 the pass fires, the trigger is the defect, and no amount of fixing the
                 descriptor will move the picture.

The atlas is identified BY SHAPE, exactly as tools/shadow_shader_census.py identifies it
(the widest depth-format fetch whose width is a multiple of its height and >= 2048), so
the tool does not depend on hardware's 1812F000 or on our runtime's 1439B000.

THE DECLARED-SLOT FILTER IS LOAD-BEARING, for the same reason it is in the census: a
draw's register file carries all 32 fetch constants whether the bound shader reads them
or not, so counting raw address matches over-reports by ~6x. A draw counts as
atlas-sampling only if the atlas sits in a slot the shader's own sidecar declares.

USAGE
    tools/rt_depth_order_census.py                    # every world trace
    tools/rt_depth_order_census.py --trace <one.xtr>
    tools/rt_depth_order_census.py --verbose          # the per-draw timeline too
"""
import argparse
import collections
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from xtr_draw_bindings import (BANKS, BE, DRAW_OPCODES, Memory, decode_fetch,  # noqa: E402
                               decompress, fnv1a)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TRACE_DIRS = ['Xenia logs/R2_world', 'Xenia logs/R3_world',
                      'Xenia logs/R4_world', 'Xenia logs/R6_gas_sign']
DEPTH_FORMATS = (22, 23)             # k_24_8, k_24_8_FLOAT — runtime/gpu/xenos.h

RB_SURFACE_INFO = 0x2000
RB_DEPTH_INFO = 0x2002               # base:12 (EDRAM tiles), format:1 @16
RB_DEPTHCONTROL = 0x2200
RB_MODECONTROL = 0x2208              # low 3 bits: 4 = kColorDepth, 5 = kDepth, 6 = kCopy
RB_COLOR_MASK = 0x2104

# RB_DEPTHCONTROL, the layout runtime/gpu/vk_renderer.cpp's DepthState() reads.
Z_ENABLE = 1 << 1
Z_WRITE = 1 << 2


def load_sidecars():
    d = {}
    for p in (ROOT / 'assets' / 'shader_spv').glob('*.meta.json'):
        d[p.name.split('.')[0]] = json.load(open(p))
    return d


def walk_trace(path):
    """Every draw in the trace, in stream order, with the state that decides ordering."""
    data, _hdr = xtr.open_trace(str(path))
    mem = Memory()
    regs = {}
    bound = {0: None, 1: None}
    out = []
    seq = 0
    for off, cmd in xtr.walk(data, len(data)):
        seq += 1
        if cmd in (xtr.CMD_MEMORY_READ, xtr.CMD_MEMORY_WRITE):
            base, enc, elen, dlen = struct.unpack_from('<IIII', data, off + 4)
            try:
                mem.add(base, decompress(data[off + 20:off + 20 + elen], enc, dlen), seq)
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
        if opcode == 0x2D and count >= 2:                  # SET_CONSTANT
            base_reg = BANKS.get((word(1) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(1) & 0x7FF
                for i in range(2, count):
                    regs[base_reg + idx + i - 2] = word(i)
        elif opcode in (0x55, 0x56) and count >= 2:        # SET_CONSTANT2
            idx = word(1) & 0xFFFF
            for i in range(2, count):
                regs[idx + i - 2] = word(i)
        elif opcode == 0x2F and count >= 4:                # LOAD_ALU_CONSTANT
            base_reg = BANKS.get((word(2) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(2) & 0x7FF
                blob = mem.read(word(1) & 0x3FFFFFFC, (word(3) & 0xFFF) * 4)
                if blob:
                    for i in range(word(3) & 0xFFF):
                        regs[base_reg + idx + i] = BE.unpack_from(blob, i * 4)[0]
        elif opcode == 0x27 and count >= 3:                # IM_LOAD
            bound[word(1) & 3] = (word(1) & ~3, word(2) & 0xFFFF)
        elif opcode == 0x2B and count >= 3:                # IM_LOAD_IMMEDIATE
            size = word(2) & 0xFFFF
            bound[word(1) & 3] = ('inline', b''.join(
                struct.pack('>I', word(3 + i)) for i in range(min(size, count - 3))))
        elif opcode in DRAW_OPCODES and count >= 2:
            init_at = 2 if opcode == 0x22 else 1
            if count <= init_at:
                continue
            b = bound[1]
            ps = None
            if b is not None:
                if b[0] == 'inline':
                    ps = 'ps_%016x' % fnv1a(b[1])
                else:
                    code = mem.read(b[0], b[1] * 4)
                    if code:
                        ps = 'ps_%016x' % fnv1a(code)
            di = regs.get(RB_DEPTH_INFO, 0)
            dc = regs.get(RB_DEPTHCONTROL, 0)
            out.append({
                'i': len(out),
                'ps': ps,
                'verts': word(init_at) >> 16,
                'depthBase': di & 0xFFF,
                'depthFmt': (di >> 16) & 1,
                'zwrite': bool(dc & Z_WRITE),
                'ztest': bool(dc & Z_ENABLE),
                'mode': regs.get(RB_MODECONTROL, 0) & 7,
                'colorMask': regs.get(RB_COLOR_MASK, 0) & 0xF,
                'surfacePitch': regs.get(RB_SURFACE_INFO, 0) & 0x3FFF,
                'fetches': [t for t in (decode_fetch(regs, s) for s in range(32)) if t],
            })
    return out


def pick_atlas_addr(draws):
    """The cascade atlas's address, by SHAPE: the widest depth-format fetch whose width
    is a multiple of its height and at least 2048. Same rule as shadow_shader_census."""
    shapes = collections.Counter()
    for d in draws:
        for t in d['fetches']:
            if t['fmt'] in DEPTH_FORMATS and t['w'] >= 2048 and t['w'] % t['h'] == 0:
                shapes[(t['addr'], t['w'], t['h'])] += 1
    if not shapes:
        return None, shapes
    return max(shapes, key=lambda k: shapes[k]), shapes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    # The world captures nest one directory per viewpoint, and R6 keeps its .xtr at the
    # top level; rglob covers both rather than the tool silently answering from one file.
    traces = ([Path(args.trace)] if args.trace else
              sorted(p for d in DEFAULT_TRACE_DIRS for p in (ROOT / d).rglob('*.xtr')))
    if not traces:
        print('no traces found', file=sys.stderr)
        return 2
    sidecars = load_sidecars()

    verdicts = []
    for tp in traces:
        draws = walk_trace(tp)
        atlas, shapes = pick_atlas_addr(draws)
        if atlas is None:
            print('%-28s  %5d draws  — no cascade-shaped depth fetch; skipped'
                  % (tp.stem, len(draws)))
            continue
        atlas_addr = atlas[0]

        # Atlas-sampling draws, filtered to slots the bound shader actually DECLARES.
        def samples_atlas(d):
            if not d['ps']:
                return False
            decl = sidecars.get(d['ps'], {}).get('tfetchConsts')
            if decl is None:
                return False
            return any(t['addr'] == atlas_addr and t['slot'] in decl
                       for t in d['fetches'])

        shadow = [d for d in draws if samples_atlas(d)]
        if not shadow:
            print('%-28s  %5d draws  — no declared atlas fetch; skipped'
                  % (tp.stem, len(draws)))
            continue
        first = shadow[0]
        surf = (first['depthBase'], first['surfacePitch'])

        # Everything BEFORE it that could have written that same depth surface.
        before = [d for d in draws[:first['i']]
                  if (d['depthBase'], d['surfacePitch']) == surf and d['zwrite']
                  and d['mode'] != 6]
        before_prepass = [d for d in before if d['colorMask'] == 0 or d['mode'] == 5]
        after = [d for d in draws[first['i']:]
                 if (d['depthBase'], d['surfacePitch']) == surf and d['zwrite']
                 and d['mode'] != 6]
        cover_before = sum(d['verts'] for d in before)
        cover_after = sum(d['verts'] for d in after)

        verdicts.append((tp.stem, len(before), len(after)))
        print('%-28s  %5d draws | atlas %08X %ux%u | first shadow draw at #%d (%s)'
              % (tp.stem, len(draws), atlas[0], atlas[1], atlas[2], first['i'],
                 first['ps']))
        print('    its depth surface: EDRAM tile base %u, pitch %u, mode %d'
              % (first['depthBase'], first['surfacePitch'], first['mode']))
        print('    depth-WRITING draws into that surface BEFORE it: %d  (%d of them '
              'colour-masked-off, i.e. a Z prepass) — %d verts'
              % (len(before), len(before_prepass), cover_before))
        print('    depth-WRITING draws into that surface FROM it on : %d — %d verts'
              % (len(after), cover_after))
        print('    atlas-sampling draws in the frame: %d, first..last #%d..#%d'
              % (len(shadow), shadow[0]['i'], shadow[-1]['i']))
        if args.verbose:
            for d in draws[:first['i'] + 1]:
                print('      #%-5d %-20s base=%-4d zw=%d mask=%X mode=%d verts=%d'
                      % (d['i'], d['ps'] or '-', d['depthBase'], d['zwrite'],
                         d['colorMask'], d['mode'], d['verts']))

    if verdicts:
        tot_b = sum(v[1] for v in verdicts)
        tot_a = sum(v[2] for v in verdicts)
        print('\n%d traces answered. Depth-writing draws into the shadow draws\' own '
              'depth surface: %d BEFORE the first shadow draw, %d from it on.'
              % (len(verdicts), tot_b, tot_a))
        print('VERDICT: %s' % (
            'a Z PREPASS EXISTS — the depth is full when the first shadow draw records, '
            'so the factor pass\'s trigger is sound.' if tot_b > tot_a else
            'THERE IS NO SCENE Z PREPASS BEFORE THE SHADOW DRAWS — the depth buffer is '
            'still at its clear value when the factor pass fires, which is exactly the '
            'uniform 1.0 part 65\'s ladder measured.'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
