#!/usr/bin/env python3
"""WHICH WAY IS THE SUN — asked of HARDWARE, not of our own latch.

WHY THIS EXISTS
---------------
Part 69 closed with the sun as the live lead: our RT shadow route derives the sun's
direction by decomposing a cascade-pass matrix it captures at draw time, and the census
of that derived vector disagreed with itself between runs — `(-0.364 0.546 -0.755)` in
the operator's windowed sessions against `(-0.371 0.557 +0.743)` recorded from headless
ones. Two components agreeing to 2% while the third flips sign is not a day cycle, and
`docs/part69-night-plan.md` §3 item 1 asks for an ORACLE before either value is trusted:
"establish WHICH is right before touching anything".

Our own runtime cannot be that oracle — it is the thing in dispute (the standing rule:
an oracle must be something you did not write). But the `.xtr` captures can, and they
already hold the answer twice over, in the SAME draw's constant file:

  * `pc(23)` is a unit direction the title uploads to its own world shaders. Part 27's
    constant table (`docs/phase5-notes.md` §6bp) read it as `-0.371391 0.557086 0.742781`
    from `w1_spawn` and matched our runtime's own upload of it to four decimal places.
    It is the title SAYING where the sun is.
  * `pc(28..31)`, `pc(32..35)`, `pc(36..39)` are the three shadow-cascade matrices the
    same draw dp4s against the world position. Their DEPTH row's gradient is the
    direction light travels, so its negation is a second, independent statement of the
    same thing — derived from a matrix rather than read from a labelled constant.

If those two agree, hardware has answered the question and neither of our two candidate
values gets a vote.

WHAT IT ALSO CHECKS, AND WHY THAT IS THE POINT
----------------------------------------------
The runtime does not read either of those. It captures the cascade RENDER matrix (a
shadow-pass draw's `c0-3`) and recovers a direction by unprojecting the light volume's
near and far centres and negating the difference. That method is reproduced here EXACTLY,
applied to hardware's own matrices, so the method can be tested apart from the matrix:

  * run it on the SAMPLING matrices (`pc(28..31)` etc.) — if it reproduces `pc(23)` the
    arithmetic is sound and any disagreement in the runtime is about WHICH matrix gets
    captured;
  * run it on the shadow pass's own `c0-3` (the render matrix, found by the same
    RB_SURFACE_INFO pitch-1040 test the runtime uses) — if THAT disagrees with `pc(23)`,
    the render matrix is not interchangeable with the sampling one and the runtime is
    decomposing the wrong thing.

Twenty single-frame traces are read rather than one, because a single frame cannot
distinguish "the sun" from "a value that happened to look like one" (gotcha 3 and the
standing census rule).

THE 0.00-DEGREE HEADLINE HAS A BUILT-IN NEGATIVE CONTROL, which matters because a tool
that always printed 0.00 would look exactly the same (gotcha 30). The pitch-1040 pass in
every trace also carries two matrices that are NOT the sun, and the tool reports them at
**54.86 and 137.97 degrees** from the constant in the same run that reports the sun at
0.00. So the comparison demonstrably resolves disagreement; it is not a saturated
statistic.

USAGE
    tools/xtr_sun_oracle.py "Xenia logs"/R*/*/*.xtr
    tools/xtr_sun_oracle.py <trace.xtr> --verbose      # per-draw, not just the summary
"""
import argparse
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xtr  # noqa: E402
from xtr_draw_bindings import (BANKS, DRAW_OPCODES, Memory, decompress,  # noqa: E402
                               fnv1a)

BE = struct.Struct(">I")
ALU_BASE = 0x4000
SQ_VS_CONST = 0x2307
SQ_PS_CONST = 0x2308
RB_SURFACE_INFO = 0x2000
SET, UNSET = 'set', 'unset'


def f32(word):
    return struct.unpack('>f', struct.pack('>I', word))[0]


def vec4(regs, prov, base):
    """Four consecutive registers as floats, or None if any is not `set`."""
    out = []
    for c in range(4):
        if prov.get(base + c) is not SET:
            return None
        out.append(f32(regs[base + c]))
    return out


def mat4(regs, prov, base):
    """Sixteen consecutive registers as four rows, or None if any is missing."""
    rows = []
    for r in range(4):
        v = vec4(regs, prov, base + r * 4)
        if v is None:
            return None
        rows.append(v)
    return rows


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    if n < 1e-12:
        return None
    return [x / n for x in v]


def invert4(m):
    """Gauss-Jordan on a 4x4 given as four rows. Returns rows, or None if singular."""
    a = [list(m[r]) + [1.0 if c == r else 0.0 for c in range(4)] for r in range(4)]
    for col in range(4):
        piv = max(range(col, 4), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col]) < 1e-12:
            return None
        a[col], a[piv] = a[piv], a[col]
        d = a[col][col]
        a[col] = [x / d for x in a[col]]
        for r in range(4):
            if r == col:
                continue
            f = a[r][col]
            if f:
                a[r] = [x - f * y for x, y in zip(a[r], a[col])]
    return [row[4:] for row in a]


def runtime_method(m):
    """THE RUNTIME'S OWN DERIVATION, transcribed from vk_renderer.cpp's LatchSun.

    Unproject clip (0,0,0,1) and (0,0,1,1) through the inverse; light travels p0->p1,
    so the direction TOWARD the sun is the negation of the difference. Returns
    (direction, volume length) or (None, None).
    """
    inv = invert4(m)
    if inv is None:
        return None, None
    p = []
    for z in (0.0, 1.0):
        c = [0.0, 0.0, z, 1.0]
        o = [sum(inv[i][k] * c[k] for k in range(4)) for i in range(3)]
        w = sum(inv[3][k] * c[k] for k in range(4))
        if abs(w) > 1e-12:
            o = [x / w for x in o]
        p.append(o)
    d = [p[1][i] - p[0][i] for i in range(3)]
    ln = math.sqrt(sum(x * x for x in d))
    if ln < 1e-6:
        return None, None
    return [-x / ln for x in d], ln


def depth_row_method(m):
    """The other reading of the same matrix: the DEPTH row's gradient.

    Row 2 dp4'd against (x,y,z,1) gives the light-space depth, so its xyz is the
    gradient of depth in world space — i.e. the direction light travels — and its
    negation points at the sun. Independent of rows 0 and 1 entirely, which is what
    makes it a cross-check on the unprojection rather than a restatement of it.
    """
    g = norm(m[2][:3])
    if g is None:
        return None
    return [-x for x in g]


def is_ortho(m):
    """Row 3 == (0,0,0,1): the projection has no perspective divide."""
    return (abs(m[3][0]) < 0.05 and abs(m[3][1]) < 0.05 and abs(m[3][2]) < 0.05
            and abs(m[3][3] - 1.0) < 0.01)


def is_unit(v):
    return v is not None and abs(math.sqrt(sum(x * x for x in v[:3])) - 1.0) < 0.01


def angle_deg(a, b):
    if a is None or b is None:
        return None
    d = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
    return math.degrees(math.acos(d))


def fmt(v):
    return 'none' if v is None else '(%+.4f %+.4f %+.4f)' % tuple(v[:3])


def scan(path, verbose):
    """Replay one trace's register file and pull every sun statement out of it."""
    data, hdr = xtr.open_trace(path)
    mem = Memory()
    regs, prov = {}, {}
    bound = {0: None, 1: None}
    result = {'trace': Path(path).stem, 'const': [], 'sample': [], 'render': []}

    def write(reg, value, how):
        regs[reg] = value
        prov[reg] = how

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
                write(reg if one else reg + i, word(1 + i), SET)
            continue
        if ptype == 1:
            if count >= 3:
                write(header & 0x7FF, word(1), SET)
                write((header >> 11) & 0x7FF, word(2), SET)
            continue
        if ptype != 3:
            continue
        opcode = (header >> 8) & 0x7F
        if opcode == 0x2D and count >= 2:
            base_reg = BANKS.get((word(1) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(1) & 0x7FF
                for i in range(2, count):
                    write(base_reg + idx + i - 2, word(i), SET)
        elif opcode in (0x55, 0x56) and count >= 2:
            idx = word(1) & 0xFFFF
            for i in range(2, count):
                write(idx + i - 2, word(i), SET)
        elif opcode == 0x2F and count >= 4:
            base_reg = BANKS.get((word(2) >> 16) & 0xFF)
            if base_reg is not None:
                idx = word(2) & 0x7FF
                src = word(1) & 0x3FFFFFFC
                size = word(3) & 0xFFF
                blob = mem.read(src, size * 4)
                if blob:
                    for i in range(size):
                        write(base_reg + idx + i, BE.unpack_from(blob, i * 4)[0], SET)
                else:
                    for i in range(size):
                        write(base_reg + idx + i, 0, 'UNRECOVERABLE')
        elif opcode == 0x27 and count >= 3:
            bound[word(1) & 3] = (word(1) & ~3, word(2) & 0xFFFF)
        elif opcode == 0x2B and count >= 3:
            size = word(2) & 0xFFFF
            bound[word(1) & 3] = ('inline', b''.join(
                struct.pack('>I', word(3 + i)) for i in range(min(size, count - 3))))
        elif opcode in DRAW_OPCODES and count >= 2:
            surf = regs.get(RB_SURFACE_INFO, 0)
            shadow_pass = (surf & 0x3FFF) == 1040 and ((surf >> 16) & 3) == 0
            ps_base = regs.get(SQ_PS_CONST, 256) & 0x1FF
            if SQ_PS_CONST not in prov:
                ps_base = 256
            vs_base = regs.get(SQ_VS_CONST, 0) & 0x1FF

            if shadow_pass:
                # THE RENDER MATRIX: c0-3 of a cascade draw, exactly what the runtime
                # captures. The runtime additionally requires the row-3 ortho test and
                # a world-space position stream; the ortho test is applied here, the
                # stream test is not (this tool has no scene-pass oracle), so a few
                # object-space composites can appear and are reported rather than hidden.
                m = mat4(regs, prov, ALU_BASE + vs_base * 4)
                if m is not None and is_ortho(m):
                    d, ln = runtime_method(m)
                    if d is not None:
                        result['render'].append((d, depth_row_method(m), ln))
                continue

            # THE TITLE'S OWN STATEMENT, and the sampling matrices beside it.
            base = ALU_BASE + ps_base * 4
            c23 = vec4(regs, prov, base + 23 * 4)
            if is_unit(c23):
                result['const'].append(c23[:3])
                for k, first in enumerate((28, 32, 36)):
                    m = mat4(regs, prov, base + first * 4)
                    if m is None or not is_ortho(m):
                        continue
                    d, ln = runtime_method(m)
                    result['sample'].append((k, d, depth_row_method(m), ln))
            if verbose and is_unit(c23):
                print('  draw ps=%s c23=%s' % (
                    fnv1a(b'') if bound[1] is None else '', fmt(c23)))
    return result


def cluster(vs, tol_deg=2.0, lens=None):
    """Group directions to ~2 degrees, the same quantisation the runtime's census uses.

    `lens` carries each entry's light-volume length so the clusters can be reported with
    the volumes that produced them: the runtime's own census prints a volume beside every
    direction, and comparing those numbers is how a captured matrix is identified as the
    same object hardware drew rather than merely a similar-looking one.
    """
    out = []
    for i, v in enumerate(vs):
        ln = lens[i] if lens else None
        for c in out:
            if angle_deg(c[0], v) is not None and angle_deg(c[0], v) < tol_deg:
                c[1] += 1
                if ln is not None:
                    c[2].append(ln)
                break
        else:
            out.append([v, 1, [ln] if ln is not None else []])
    out.sort(key=lambda c: -c[1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('traces', nargs='+')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    rows = []
    for t in args.traces:
        try:
            r = scan(t, args.verbose)
        except Exception as e:
            print('%-34s  UNREADABLE: %s' % (Path(t).stem, e), file=sys.stderr)
            continue
        rows.append(r)
        cons = cluster(r['const'])
        samp = cluster([s[1] for s in r['sample'] if s[1]])
        sdep = cluster([s[2] for s in r['sample'] if s[2]])
        rend = cluster([s[0] for s in r['render'] if s[0]],
                       lens=[s[2] for s in r['render'] if s[0]])
        print('\n=== %s' % r['trace'])
        print('  title CONSTANT pc(23)          %-28s over %d draws%s'
              % (fmt(cons[0][0]) if cons else 'ABSENT', len(r['const']),
                 '' if len(cons) <= 1 else '  [%d distinct!]' % len(cons)))
        print('  sampling matrix, depth row     %-28s over %d matrices%s'
              % (fmt(sdep[0][0]) if sdep else 'ABSENT', len(r['sample']),
                 '' if len(sdep) <= 1 else '  [%d distinct]' % len(sdep)))
        print('  sampling matrix, RUNTIME method%-28s'
              % (' ' + fmt(samp[0][0]) if samp else ' ABSENT'))
        if cons and sdep:
            print('    -> constant vs depth row:      %.2f deg'
                  % angle_deg(cons[0][0], sdep[0][0]))
        if cons and samp:
            print('    -> constant vs runtime method: %.2f deg'
                  % angle_deg(cons[0][0], samp[0][0]))
        if rend:
            for d, n, ls in rend[:4]:
                vol = ('' if not ls else '  volumes %s'
                       % '/'.join('%.1f' % x for x in sorted(set(round(y, 1)
                                                                 for y in ls))[:6]))
                print('  RENDER matrix (pitch-1040 c0-3), RUNTIME method %s  x%d%s%s'
                      % (fmt(d), n,
                         '' if not cons else
                         '   %.2f deg from the constant' % angle_deg(cons[0][0], d),
                         vol))
        else:
            print('  RENDER matrix (pitch-1040 c0-3): none in this trace')

    # THE VERDICT, over every trace at once. One trace cannot separate "the sun" from
    # "a plausible-looking constant"; twenty at different places and times can.
    print('\n' + '=' * 72)
    allc = cluster([v for r in rows for v in r['const']])
    print('pc(23) over all %d traces: %d distinct directions' % (len(rows), len(allc)))
    for v, n, _ in allc:
        print('   %s  x%d' % (fmt(v), n))
    agree = [angle_deg(cluster(r['const'])[0][0], cluster([s[2] for s in r['sample']
                                                           if s[2]])[0][0])
             for r in rows if r['const'] and any(s[2] for s in r['sample'])]
    if agree:
        print('constant vs cascade depth row, worst disagreement over %d traces: '
              '%.3f deg' % (len(agree), max(agree)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
