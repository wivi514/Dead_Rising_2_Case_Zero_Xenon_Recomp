#!/usr/bin/env python3
"""WHAT IS ACTUALLY IN THE RAY-TRACING STRUCTURE, AND WHICH FILTER EATS THE REST?

WHY THIS EXISTS
---------------
Part 66 closed with the RT shadow defect LOCATED and everything downstream of it
exonerated by measurement: the injection into 126 shaders, the screen-space alignment
(both axes), the world reconstruction, the primary ray, the ray length, the origin bias
and the sun direction all read correct. What is left is the population.

`CZ_VK_RT_FACTOR_DEBUG=20` fires eight FIXED directions over the upper hemisphere with
the sun deliberately not involved and reads **97.3% fully open, mean 0.987** outdoors.
No direction above a receiver is occluded. So:

    THE TLAS IS EFFECTIVELY A GROUND PLANE — ~700 static opaque meshes a frame for a
    whole town, against `dyn=19.0M` (57% of every draw the collector sees) and
    `alpha=3.2M` (10%).

The part-67 hand-off asks two questions and insists both be answered OFFLINE, before
anything is built:

  (a) What ARE the accepted instances?  Is the accepted set a flat sheet?
  (b) Which filter eats the buildings?

THE ORACLE IS THE CAPTURE, NOT OUR RUNTIME
------------------------------------------
A `.xtr` world trace carries hardware's whole draw stream for one frame in order, with
the register file at every draw AND the actual bytes of the vertex buffers the GPU
fetched. That is everything `rtshadow::Collect` looks at, so the collector's filter
chain can be re-run offline against hardware's own frame — the same move that turned
part 65's shader census and part 66's draw-order census from "needs an instrumented
run" into files already on disc (gotcha 387).

This tool re-implements `rtshadow::Collect`'s chain, in its order, transcribed from
`runtime/gpu/vk_renderer.cpp`:

    SceneXformForm(c0..c3) == 2   the world's view-projection composite   -> `xform`
    RB_DEPTHCONTROL bit 2         depth-writing, i.e. an occluder         -> `nozw`
    RB_COLORCONTROL & 0x18        alpha test / alpha-to-mask              -> `alpha`
    attributes[0].format == 57    a float3 position at a direct slot      -> `posform`
    prim in {list, strip}, u16    the BLAS builder's geometry             -> `prim`
    fetch address/size in range                                           -> `range`
    endian in {0, 2}                                                       -> `endian`
    stream extent <= 50000        the part-64 bounds gate                 -> `bounds`

and then, for every draw it ACCEPTS and for every draw the `alpha` gate rejects, reads
the real vertex bytes out of the trace and computes the WORLD-SPACE bounding box. That
is legitimate because form 2 means the shader's c0..c3 is the pure view-projection: the
position stream is world-space by construction, which is also why our TLAS instances all
carry an identity transform.

WHAT IT FOUND — AND IT IS NEITHER (a) NOR (b) AS THEY WERE ASKED
----------------------------------------------------------------
No filter is eating the buildings. They are all collected, and they are all in the same
place:

    THE POSITION STREAMS ARE OBJECT-SPACE. Our BLASes hold untransformed local
    geometry and every TLAS instance carries an IDENTITY transform, so the whole town
    is piled on top of itself at the world origin.

`Collect` gates on `SceneXformForm(c0..c3) == 2` and §6cs read that as "the stream is
therefore world-space". It is not. c0..c3 is the camera's view-projection, and that is
the same matrix whether the shader feeds it a world position or an object position it
transformed one line earlier — which is what these shaders do, from a per-draw 4x3 at
`vc(8..10)` (see tools/rt_world_xform_census.py for the per-shader table).

The numbers, over the twenty traces: transformed by the camera composite alone, 11.7%
of accepted bounding boxes intersect the frustum they were drawn into; with the world
matrix applied first, 93.8%. Per VERTEX the gap is 0.0% against 61-98%. And 100% of the
accepted draws carry a NON-IDENTITY world translation, spread over x[-610, 124]
z[-681, 106] — the town.

`dyn` and `new` remain unmodellable here (a stream is `dynamic` once the guest has been
caught REWRITING it, which needs two frames and a trace has one), and they no longer
need to be: our runtime's 216..722 TLAS instances match this census's count of DISTINCT
position streams per frame, not its count of draws. The instances were collapsing
because the placement was never part of the identity.

USAGE
    tools/rt_tlas_census.py                      # every world trace
    tools/rt_tlas_census.py --trace <one.xtr>
    tools/rt_tlas_census.py --top 20             # the tallest rejected meshes, by bucket
"""
import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from xtr_draw_bindings import (BANKS, BE, DRAW_OPCODES, Memory, decompress,  # noqa: E402
                               fnv1a)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TRACE_DIRS = ['Xenia logs/R2_world', 'Xenia logs/R3_world',
                      'Xenia logs/R4_world', 'Xenia logs/R6_gas_sign']

# --- registers, transcribed from runtime/gpu/xenos.h ---------------------------------
ALU_CONST_BASE = 0x4000        # kAluConstantBase: 512 float4
FETCH_BASE = 0x4800            # kFetchConstantBase: vertex fetches are 2 dwords each
RB_DEPTHCONTROL = 0x2200
RB_COLORCONTROL = 0x2202       # bit 3 alpha test enable, bit 4 alpha-to-mask
RB_SURFACE_INFO = 0x2000
RB_DEPTH_INFO = 0x2002
RB_MODECONTROL = 0x2208
SQ_VS_CONST = 0x2307           # low 9 bits: the VS constant window's base float4
Z_WRITE = 1 << 2

PRIM_TRIANGLE_LIST = 4         # xenos::kTriangleList
PRIM_TRIANGLE_STRIP = 6        # xenos::kTriangleStrip
POS_FORMAT = 57                # k_32_32_32_FLOAT — the only position form the BLAS takes
BOUNDS_CAP = 50000.0           # CZ_VK_RT_BOUNDS_CAP's default

# The collector's chain, in the order Collect() applies it. `ok` is the accepted class.
BUCKETS = ['xform', 'nozw', 'alpha', 'posform', 'prim', 'range', 'endian', 'bounds',
           'nopos', 'ok']
WORLD_XFORM = ROOT / 'config' / 'rt_world_xform.json'


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


def is_169_perspective(m):
    """Is169Perspective from vk_renderer.cpp — the RAW projection form."""
    if m[12] != 0.0 or m[13] != 0.0 or m[14] != 1.0 or m[15] != 0.0:
        return False
    if any(m[i] != 0.0 for i in (1, 2, 3, 4, 6, 7, 8, 9)):
        return False
    if m[0] == 0.0 or m[5] == 0.0:
        return False
    return abs(abs(m[0] / m[5]) - 0.5625) <= 0.002


def scene_xform_form(m):
    """SceneXformForm from vk_renderer.cpp. 0 = not a scene transform, 1 = raw
    projection, 2 = the world's view-projection COMPOSITE (what Collect requires)."""
    if is_169_perspective(m):
        return 1

    def dot3(r, s):
        return sum(m[r * 4 + k] * m[s * 4 + k] for k in range(3))

    if abs(dot3(3, 3) - 1.0) > 0.004:          # unit view row; orthos/affines are 0
        return 0
    n0sq, n1sq = dot3(0, 0), dot3(1, 1)
    if not (n0sq > 0.0 and n1sq > 0.0):
        return 0
    n0, n1 = math.sqrt(n0sq), math.sqrt(n1sq)
    if abs(n0 / n1 - 0.5625) > 0.002:          # 9/16; the cube face cameras read 1.0
        return 0
    if abs(dot3(0, 3)) > 0.01 * n0 or abs(dot3(1, 3)) > 0.01 * n1:
        return 0
    f = dot3(2, 3)
    if f < 0.9 or f > 1.1:
        return 0
    for i in range(3):
        if abs(m[8 + i] - f * m[12 + i]) > 0.01:
            return 0
    return 2


def decode_vertex_fetch(regs, slot):
    """xenos::DecodeVertexFetch. The 24-bit size mask is load-bearing (see xenos.h)."""
    d0 = regs.get(FETCH_BASE + slot * 2)
    d1 = regs.get(FETCH_BASE + slot * 2 + 1)
    if d0 is None or d1 is None:
        return None
    return {'address': d0 & ~3, 'sizeDwords': (d1 >> 2) & 0xFFFFFF, 'endian': d1 & 3}


def _bounds_from_blob(blob, scan, stride_dw, offset_dw, endian):
    """The bounds gate's per-vertex rule over one contiguous span."""
    mn = [0.0, 0.0, 0.0]
    mx = [0.0, 0.0, 0.0]
    valid = 0
    step = stride_dw * 4
    at = offset_dw * 4
    for _ in range(scan):
        if at + 12 > len(blob):
            break
        c = blob[at:at + 12]
        at += step
        f = struct.unpack('>3f', c) if (endian & 3) == 0 else \
            struct.unpack('<3f', c[3::-1] + c[7:3:-1] + c[11:7:-1])
        if any(not math.isfinite(v) or abs(v) > 1e7 for v in f):
            continue
        if valid:
            for k in range(3):
                if f[k] < mn[k]:
                    mn[k] = f[k]
                elif f[k] > mx[k]:
                    mx[k] = f[k]
        else:
            mn = list(f)
            mx = list(f)
        valid += 1
    if not valid:
        return None, None, 0, scan, 0
    return mn, mx, valid, scan, 0


def stream_bounds(mem, addr, size_dwords, stride_dw, offset_dw, endian, cap_verts=4096):
    """The bounds gate's own scan, over hardware's bytes.

    Returns (mn, mx, valid, scanned, missing). `missing` counts vertices whose bytes the
    trace does not carry — a trace records the ranges the GPU touched, which for a big
    stream can arrive in several chunks or only in part, and reporting that as a small
    box would be exactly the silent-truncation failure this project keeps finding.

    The per-vertex validity rule is the runtime's: skip non-finite and |v| > 1e7
    components rather than vetoing the whole stream on them, because REAL meshes carry
    unreferenced padding slots and the first version of the runtime gate rejected 66,095
    real streams that way.
    """
    if not stride_dw:
        return None, None, 0, 0, 0
    verts = size_dwords // stride_dw
    scan = min(verts, cap_verts)
    # ONE read for the whole scanned span where the trace carries it contiguously —
    # which is the common case and ~100x the per-vertex path.
    span = (scan - 1) * stride_dw + offset_dw + 3 if scan else 0
    blob = mem.read(addr, span * 4) if span else None
    if blob is not None:
        return _bounds_from_blob(blob, scan, stride_dw, offset_dw, endian)
    # THE FALLBACK, when the trace carries the stream in pieces. `Memory.read` is a
    # linear scan over the trace's chunks, so this is capped and SAMPLED rather than
    # exhaustive — and the sample is reported, because a silently truncated bounds box
    # is exactly the "we measured a smaller world" failure this census exists to detect.
    step = max(1, scan // 256)
    mn = [0.0, 0.0, 0.0]
    mx = [0.0, 0.0, 0.0]
    valid = 0
    missing = 0
    for i in range(0, scan, step):
        dw = i * stride_dw + offset_dw
        if dw + 3 > size_dwords:
            break
        chunk = mem.read(addr + dw * 4, 12)
        if chunk is None:
            missing += 1
            continue
        # The guest bytes are big-endian; endian 0 and 2 are the only ones that reach
        # here and both leave a float3 as three big-endian dwords once unswapped.
        f = struct.unpack('>3f', chunk) if (endian & 3) == 0 else \
            struct.unpack('<3f', chunk[3::-1] + chunk[7:3:-1] + chunk[11:7:-1])
        if any(not math.isfinite(c) or abs(c) > 1e7 for c in f):
            continue
        for c in range(3):
            mn[c] = min(mn[c], f[c]) if valid else f[c]
            mx[c] = max(mx[c], f[c]) if valid else f[c]
        valid += 1
    if not valid:
        return None, None, 0, scan, missing
    return mn, mx, valid, scan, missing


def load_world_xform(path):
    """The per-shader object->world constant rows (tools/rt_world_xform_census.py).

    Entries are `kind@base` pairs, innermost first — the same string the runtime reads.
    """
    try:
        raw = json.load(open(path))['shaders']
    except Exception:
        return {}
    out = {}
    for name, ent in raw.items():
        stages = []
        for tok in ent['stages'].split(','):
            if tok in ('', 'camera'):
                continue
            kind, _, base = tok.partition('@')
            stages.append((int(base), kind))
        out[name] = stages
    return out


def affine(win, base):
    """A row-major 4x3 at VS constant float4s base..base+2, or None if unreadable."""
    m = []
    for r in range(3):
        for k in range(4):
            v = win[(base + r) * 4 + k] if (base + r) * 4 + k < len(win) else None
            if v is None or not math.isfinite(v):
                return None
            m.append(v)
    return m


def compose(outer, inner):
    """outer o inner, both row-major 4x3 affines."""
    out = []
    for r in range(3):
        for c in range(3):
            out.append(sum(outer[r * 4 + k] * inner[k * 4 + c] for k in range(3)))
        out.append(sum(outer[r * 4 + k] * inner[k * 4 + 3] for k in range(3))
                   + outer[r * 4 + 3])
    return out


def world_xform(win, stages):
    """The shader's whole object->world transform, innermost stage first.

    The PALETTE shape is applied as its entry 0 with unit weight, which is what the
    static world draws use in practice (composing the second stage takes the busiest
    palette shader from 81.3% of vertices on screen to 99.5%). It is counted separately
    at runtime so the approximation is never invisible.
    """
    m = None
    for base, _kind in stages:
        a = affine(win, base)
        if a is None:
            return None
        m = a if m is None else compose(a, m)
    return m or [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]


def xform_box(m, mn, mx):
    """The AABB of a box carried through a row-major 4x3."""
    c = [(mn[k] + mx[k]) * 0.5 for k in range(3)]
    e = [(mx[k] - mn[k]) * 0.5 for k in range(3)]
    o, r = [], []
    for i in range(3):
        o.append(sum(m[i * 4 + k] * c[k] for k in range(3)) + m[i * 4 + 3])
        r.append(sum(abs(m[i * 4 + k]) * e[k] for k in range(3)))
    return [o[k] - r[k] for k in range(3)], [o[k] + r[k] for k in range(3)]


def in_frustum(cam, mn, mx):
    """Does the box intersect the clip volume under the camera composite?"""
    out = [0] * 6
    for i in range(8):
        p = (mn[0] if i & 1 else mx[0], mn[1] if i & 2 else mx[1],
             mn[2] if i & 4 else mx[2], 1.0)
        cl = [sum(cam[r * 4 + k] * p[k] for k in range(4)) for r in range(4)]
        x, y, z, w = cl
        out[0] += x < -w
        out[1] += x > w
        out[2] += y < -w
        out[3] += y > w
        out[4] += z < 0
        out[5] += z > w
    return not any(o == 8 for o in out)


def load_sidecars(spv):
    d = {}
    for p in (ROOT / spv).glob('*.meta.json'):
        d[p.name.split('.')[0]] = json.load(open(p))
    return d


def walk_and_classify(path, sidecars):
    """Every draw in the trace, in stream order, run through Collect()'s chain."""
    data, _hdr = xtr.open_trace(str(path))
    mem = Memory()
    regs = {}
    bound = {0: None, 1: None}
    out = []
    cache = {}
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
                size = word(3) & 0xFFF
                blob = mem.read(word(1) & 0x3FFFFFFC, size * 4)
                for i in range(size):
                    if blob:
                        regs[base_reg + idx + i] = BE.unpack_from(blob, i * 4)[0]
                    else:
                        regs.pop(base_reg + idx + i, None)   # UNRECOVERABLE, not stale
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
            init = word(init_at)
            names = {}
            for stage, label in ((0, 'vs'), (1, 'ps')):
                b = bound[stage]
                if b is None:
                    continue
                code = b[1] if b[0] == 'inline' else mem.read(b[0], b[1] * 4)
                if code:
                    names[label] = '%s_%016x' % (label, fnv1a(code))
            di = regs.get(RB_DEPTH_INFO, 0)
            d = {
                'i': len(out),
                'vs': names.get('vs'),
                'ps': names.get('ps'),
                'prim': init & 0x3F,
                'index32': bool((init >> 11) & 1),
                'indexed': ((init >> 6) & 3) == 0,
                'count': init >> 16,
                'zwrite': bool(regs.get(RB_DEPTHCONTROL, 0) & Z_WRITE),
                'cc': regs.get(RB_COLORCONTROL, 0),
                'depthBase': di & 0xFFF,
                'pitch': regs.get(RB_SURFACE_INFO, 0) & 0x3FFF,
                'mode': regs.get(RB_MODECONTROL, 0) & 7,
            }
            classify(d, regs, mem, sidecars, cache)
            out.append(d)
    return out


def classify(d, regs, mem, sidecars, cache):
    """Collect()'s chain, in its order. Sets d['bucket'] and, where it can, d['mn'/'mx']."""
    d['bucket'] = None
    base4 = (regs.get(SQ_VS_CONST, 0)) & 0x1FF
    # The whole low window, because the object->world rows live at vc(4..10) and the
    # camera composite the chain gates on is only its first four rows.
    full = [regs.get(ALU_CONST_BASE + (base4 + r) * 4 + k)
            for r in range(16) for k in range(4)]
    d['win'] = [None if v is None else f32(v) for v in full]
    win = full[:16]
    if any(v is None for v in win):
        d['bucket'] = 'xform'          # the window is UNRECOVERABLE; not a scene draw
        d['xformUnknown'] = True
        return
    m = [f32(v) for v in win]
    d['form'] = scene_xform_form(m)
    if d['form'] != 2:
        d['bucket'] = 'xform'
        return
    if not d['zwrite']:
        d['bucket'] = 'nozw'
        return
    if d['cc'] & 0x18:
        d['bucket'] = 'alpha'
    meta = sidecars.get(d['vs'] or '')
    attrs = (meta or {}).get('attributes') or []
    if not attrs:
        d['bucket'] = d['bucket'] or 'posform'
        return
    pos = attrs[0]
    if (pos['location'] < 0 or pos['indirect'] or pos['format'] != POS_FORMAT
            or not pos['strideDwords'] or pos['fetchSlot'] >= 96):
        d['bucket'] = d['bucket'] or 'posform'
        return
    if d['prim'] not in (PRIM_TRIANGLE_LIST, PRIM_TRIANGLE_STRIP):
        d['bucket'] = d['bucket'] or 'prim'
        return
    if d['indexed'] and d['index32']:
        d['bucket'] = d['bucket'] or 'prim'
        return
    vf = decode_vertex_fetch(regs, pos['fetchSlot'])
    if vf is None or not vf['address'] or not vf['sizeDwords']:
        d['bucket'] = d['bucket'] or 'range'
        return
    if vf['endian'] not in (0, 2):
        d['bucket'] = d['bucket'] or 'endian'
        return
    d['stream'] = (vf['address'], vf['sizeDwords'], vf['endian'],
                   pos['strideDwords'], pos['offsetDwords'])
    ck = d['stream']
    if ck not in cache:
        cache[ck] = stream_bounds(mem, vf['address'], vf['sizeDwords'],
                                  pos['strideDwords'], pos['offsetDwords'],
                                  vf['endian'])
    mn, mx, valid, scan, missing = cache[ck]
    d['scanned'], d['valid'], d['missing'] = scan, valid, missing
    if mn is None:
        # No readable vertex at all. In the runtime this is `nopos`; here it is usually
        # "the trace does not carry these bytes", and the two must not share a number.
        d['bucket'] = d['bucket'] or ('nopos' if missing == 0 else 'unread')
        return
    d['mn'], d['mx'] = mn, mx
    extent = max(mx[c] - mn[c] for c in range(3))
    d['extent'] = extent
    if extent > BOUNDS_CAP:
        d['bucket'] = d['bucket'] or 'bounds'
        return
    d['bucket'] = d['bucket'] or 'ok'


def yhist(draws):
    """Vertical extents, in the buckets that separate 'a flat sheet' from 'a town'."""
    edges = [0.5, 1, 2, 4, 8, 16, 32, 64, 1e9]
    names = ['<0.5', '0.5-1', '1-2', '2-4', '4-8', '8-16', '16-32', '32-64', '>64']
    h = [0] * len(edges)
    for d in draws:
        dy = d['mx'][1] - d['mn'][1]
        for k, e in enumerate(edges):
            if dy < e:
                h[k] += 1
                break
    return names, h


def box(draws):
    mn = [min(d['mn'][c] for d in draws) for c in range(3)]
    mx = [max(d['mx'][c] for d in draws) for c in range(3)]
    return mn, mx


def report(name, draws, top):
    counts = collections.Counter(d['bucket'] for d in draws)
    verts = collections.Counter()
    for d in draws:
        verts[d['bucket']] += d['count']
    order = BUCKETS + [b for b in counts if b not in BUCKETS]
    print('\n%s — %d draws' % (name, len(draws)))
    print('    %-9s %7s %10s   %s' % ('bucket', 'draws', 'verts', 'what it is'))
    what = {
        'xform': 'c0-3 is not the world view-projection composite',
        'nozw': 'not depth-writing: not an occluder',
        'alpha': 'alpha test or alpha-to-mask (opaque-only BLAS)',
        'posform': 'no direct float3 position at attribute 0',
        'prim': 'not a triangle list/strip, or 32-bit indices',
        'range': 'vertex fetch address or size unusable',
        'endian': 'a swizzle the collector refuses rather than guesses',
        'bounds': 'stream extent > %g (the part-64 junk gate)' % BOUNDS_CAP,
        'nopos': 'no finite vertex in the stream',
        'unread': 'the TRACE does not carry these bytes (not a filter)',
        'ok': 'ACCEPTED — this is what the TLAS would hold',
    }
    for b in order:
        if counts.get(b):
            print('    %-9s %7d %10d   %s' % (b, counts[b], verts[b], what.get(b, '')))
    ok = [d for d in draws if d['bucket'] == 'ok' and 'mn' in d]
    if ok:
        mn, mx = box(ok)
        names, h = yhist(ok)
        print('    ACCEPTED as the BLAS holds them (untransformed): '
              'x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]'
              % (mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]))
        print('    ACCEPTED vertical extents: %s'
              % '  '.join('%s:%d' % (n, c) for n, c in zip(names, h) if c))
        placed = [d for d in ok if 'wmn' in d]
        if placed:
            wmn = [min(d['wmn'][k] for d in placed) for k in range(3)]
            wmx = [max(d['wmx'][k] for d in placed) for k in range(3)]
            raw = sum(1 for d in ok if in_frustum(d['win'][:16], d['mn'], d['mx']))
            put = sum(1 for d in placed
                      if in_frustum(d['win'][:16], d['wmn'], d['wmx']))
            print('    PLACED by the per-shader world matrix (%d of %d accepted): '
                  'x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]'
                  % (len(placed), len(ok), wmn[0], wmx[0], wmn[1], wmx[1],
                     wmn[2], wmx[2]))
            print('    boxes intersecting the frustum they were drawn into: '
                  'untransformed %d/%d (%.1f%%), placed %d/%d (%.1f%%)'
                  % (raw, len(ok), 100.0 * raw / len(ok), put, len(placed),
                     100.0 * put / len(placed)))
        miss = [d for d in ok if 'wmn' not in d]
        if miss:
            byvs = collections.Counter(d['vs'] for d in miss)
            print('    NOT PLACEABLE (no entry in the world-transform table): '
                  '%d draws over %d shaders: %s'
                  % (len(miss), len(byvs),
                     ' '.join('%s(%d)' % (k, v) for k, v in byvs.most_common(6))))
    for b in ('alpha', 'nozw'):
        rej = [d for d in draws if d['bucket'] == b and 'mn' in d]
        if rej:
            mn, mx = box(rej)
            names, h = yhist(rej)
            print('    %-6s world box:   x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]'
                  % (b, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]))
            print('    %-6s vertical extents: %s'
                  % (b, '  '.join('%s:%d' % (n, c) for n, c in zip(names, h) if c)))
    if top:
        tall = sorted((d for d in draws if 'mn' in d and d['bucket'] != 'ok'),
                      key=lambda d: d['mx'][1] - d['mn'][1], reverse=True)[:top]
        if tall:
            print('    tallest REJECTED meshes:')
            for d in tall:
                print('      #%-5d %-8s dy=%8.2f verts=%-6d %s'
                      % (d['i'], d['bucket'], d['mx'][1] - d['mn'][1], d['count'],
                         d['vs']))
    return counts, verts, ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace')
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--xform', default=str(WORLD_XFORM))
    args = ap.parse_args()

    traces = ([Path(args.trace)] if args.trace else
              sorted(p for d in DEFAULT_TRACE_DIRS for p in (ROOT / d).rglob('*.xtr')))
    if not traces:
        print('no traces found', file=sys.stderr)
        return 2
    xform = load_world_xform(args.xform)
    if not xform:
        print('no world-transform table at %s — run tools/rt_world_xform_census.py; '
              'the placement half of this census will be blank' % args.xform,
              file=sys.stderr)
    sidecars = load_sidecars(args.spv)
    if not sidecars:
        print('no shader sidecars under %s — build the cache first' % args.spv,
              file=sys.stderr)
        return 2

    total = collections.Counter()
    tverts = collections.Counter()
    all_ok = []
    for tp in traces:
        draws = walk_and_classify(tp, sidecars)
        for d in draws:
            if d['bucket'] != 'ok' or 'mn' not in d:
                continue
            ent = xform.get(d['vs'] or '')
            if ent is None:
                continue
            m = world_xform(d['win'], ent)
            if m is None:
                continue
            d['wmn'], d['wmx'] = xform_box(m, d['mn'], d['mx'])
        c, v, ok = report(tp.stem, draws, args.top)
        total.update(c)
        tverts.update(v)
        all_ok.extend(ok)

    print('\n' + '=' * 78)
    print('%d traces. Collect()\'s chain over hardware\'s own frames:' % len(traces))
    for b in BUCKETS + [x for x in total if x not in BUCKETS]:
        if total.get(b):
            print('    %-9s %8d draws %12d verts' % (b, total[b], tverts[b]))
    if all_ok:
        mn, mx = box(all_ok)
        names, h = yhist(all_ok)
        print('    ACCEPTED across all traces: %d draws, UNTRANSFORMED box '
              'x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]'
              % (len(all_ok), mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]))
        print('    vertical extents: %s'
              % '  '.join('%s:%d' % (n, c) for n, c in zip(names, h) if c))
        placed = [d for d in all_ok if 'wmn' in d]
        if placed:
            raw = sum(1 for d in all_ok if in_frustum(d['win'][:16], d['mn'], d['mx']))
            put = sum(1 for d in placed
                      if in_frustum(d['win'][:16], d['wmn'], d['wmx']))
            moved = sum(1 for d in placed
                        if max(abs(d['wmn'][k] - d['mn'][k]) for k in range(3)) > 0.01)
            wmn = [min(d['wmn'][k] for d in placed) for k in range(3)]
            wmx = [max(d['wmx'][k] for d in placed) for k in range(3)]
            print('    PLACED box: x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]  '
                  '(%d of %d accepted draws are placeable)'
                  % (wmn[0], wmx[0], wmn[1], wmx[1], wmn[2], wmx[2], len(placed),
                     len(all_ok)))
            print('    in the frustum: untransformed %.2f%%, placed %.2f%%; '
                  '%.1f%% of placeable draws MOVE'
                  % (100.0 * raw / len(all_ok), 100.0 * put / len(placed),
                     100.0 * moved / len(placed)))
            print('\nVERDICT: %s' % (
                'THE POSITION STREAMS ARE OBJECT-SPACE. An identity TLAS instance puts '
                'every one of these meshes at the world origin; the per-shader world '
                'matrix places them across the town and multiplies the fraction that '
                'lands in the frustum they were drawn into.'
                if put > raw * 2 else
                'the world matrix does not improve placement — this census disagrees '
                'with part 67 and the table or the chain is wrong.'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
