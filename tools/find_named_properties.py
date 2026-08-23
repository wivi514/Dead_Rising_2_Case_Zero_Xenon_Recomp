#!/usr/bin/env python3
"""Enumerate every data-driven NAMED PROPERTY the engine registers, out of the image.

WHY THIS EXISTS
---------------
This engine's tunables are data-driven named properties: a class binds each of its
fields to a string at construction time through one of a handful of universal binders,
and the asset data then addresses those fields by name. Part 61 found the camera FOV
that way and part 62 used it to fix the 21:9 flank pop-in.

Part 72 needed the opposite answer — *is there an ASPECT scalar anywhere?* — and that is
a question a grep cannot settle, because a name that is never registered is not a
tunable however often the string appears, and a name that IS registered may sit in a
class nobody would grep for. The live instrument (`CZ_FOV_PROP_TRACE=1`) only reports
what a run actually constructs, so a null from it is a fact about the route, not about
the game (gotcha 3: a zero is a detection failure until you have shown the detector can
see). This scans the whole `.text` instead, so a null is a null over the entire image.

The answer it gave: 2,056 registration sites, 1,966 names recovered, and exactly ONE
`Aspect` in the game — `+0x24` on `cZombieSpawnRegion`, alongside X/Y/Width/Height, i.e.
a 2D spawn box. Sixty-odd camera configs register FOV and nothing aspect-shaped. That
killed the plan's cheapest route for the wide-culling item in one pass.

HOW IT WORKS. Scan `.text` for `bl <binder>`, then walk backwards up to sixteen
instructions reconstructing `lis`/`addi` pairs to recover r4 (the name pointer) and r5
(`&this->field`, as a displacement off the object register). Names that do not decode as
short printable C strings are dropped and COUNTED rather than skipped silently, so the
recovery rate is visible — a scanner that quietly recovered 40% of the sites would
answer "no aspect property exists" with exactly the same confidence as one that
recovered 96%.

The binders were found by following the registration calls in a class whose properties
were already known; if a port of another title in this engine finds a sixth, add it.

    python3 tools/find_named_properties.py                    # the whole list
    python3 tools/find_named_properties.py --grep -i aspect   # ask a question
"""
import argparse
import re
import struct
import sys

BASE = 0x82000000
# The universal property binders. B2's three thunks differ only in the element count
# they pass (1/2/4 floats), so a name bound through them is still one property.
BINDERS = {
    0x8233E1F8: 'B1',
    0x8233E3F8: 'B1b',
    0x82375518: 'B2',
    0x82395FF0: 'B2/1',
    0x82396000: 'B2/2',
    0x82396010: 'B2/4',
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', default='assets/game/default_image.bin')
    ap.add_argument('--text-base', type=lambda s: int(s, 16), default=0x82150000)
    ap.add_argument('--text-size', type=lambda s: int(s, 16), default=0x873564)
    ap.add_argument('--grep', help='regex; print only names matching it')
    ap.add_argument('-i', '--ignore-case', action='store_true')
    args = ap.parse_args()

    try:
        img = open(args.image, 'rb').read()
    except OSError as e:
        sys.exit(f"{e}\n(build it with tools/xex_image_dump — see CLAUDE.md)")

    t0, t1 = args.text_base, args.text_base + args.text_size

    def cstr(va):
        o = va - BASE
        if o < 0 or o >= len(img):
            return None
        e = img.find(b'\0', o)
        if e < 0:
            return None
        s = img[o:e]
        if not s or len(s) > 63:
            return None
        if not all(32 <= c < 127 for c in s):
            return None
        return s.decode('ascii')

    sites, named = 0, []
    for va in range(t0, t1, 4):
        ins = struct.unpack_from('>I', img, va - BASE)[0]
        if (ins >> 26) != 18 or (ins & 3) != 1:      # not `bl`
            continue
        li = ins & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        tgt = (va + li) & 0xFFFFFFFF
        if tgt not in BINDERS:
            continue
        sites += 1
        # Walk back for the argument setup. `regs` tracks only lis/addi chains, which is
        # all these call sites use to form a string address; `fld` catches the
        # `addi r5, <object reg>, imm` that names the field offset.
        regs, fld = {}, None
        for k in range(16, 0, -1):
            p = va - 4 * k
            if p < t0:
                continue
            i2 = struct.unpack_from('>I', img, p - BASE)[0]
            op, rt, ra = i2 >> 26, (i2 >> 21) & 31, (i2 >> 16) & 31
            imm = i2 & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:                       # lis
                regs[rt] = (simm << 16) & 0xFFFFFFFF
            elif op == 14:                                 # addi
                if ra in regs:
                    regs[rt] = (regs[ra] + simm) & 0xFFFFFFFF
                elif rt == 5:                              # &this->field
                    fld = simm
        name = cstr(regs[4]) if 4 in regs else None
        if name:
            named.append((va, BINDERS[tgt], name, fld))

    pat = None
    if args.grep:
        pat = re.compile(args.grep, re.I if args.ignore_case else 0)
    shown = 0
    for va, b, n, f in named:
        if pat and not pat.search(n):
            continue
        shown += 1
        print(f"{va:08X} {b:5s} {n!r} field={'+0x%x' % f if f is not None else '?'}")
    # The recovery rate is part of the result, not diagnostics: a null answer from this
    # tool is only as strong as the fraction of sites it could read.
    print(f"\n{sites} registration call sites, {len(named)} names recovered "
          f"({100.0 * len(named) / sites:.1f}%)", file=sys.stderr)
    if pat:
        print(f"{shown} matched /{args.grep}/", file=sys.stderr)


if __name__ == '__main__':
    main()
