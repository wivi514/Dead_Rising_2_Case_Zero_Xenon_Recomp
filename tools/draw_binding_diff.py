#!/usr/bin/env python3
"""Diff OUR per-draw bindings against HARDWARE's, for the same place.

WHY THIS EXISTS
---------------
"Is each texture associated with the right draw?" is the question behind almost every
picture defect in this port, and until now answering it meant reading two differently
shaped text files side by side and transcribing columns by hand. Part 27 did exactly that
for one draw and it took most of a session; there are 2,000-5,000 draws in a frame.

Both halves now exist in one vocabulary:

  * OURS      — `CZ_CAPTURE_KEY=<dir>` writes `capture_f<frame>.census`, one line per
                draw, on an operator's F9 press;
  * HARDWARE  — `tools/xtr_draw_bindings.py --csv` over the matching Xenia `.xtr`.

WHAT IT MATCHES ON, AND WHY NOT ON ADDRESSES
--------------------------------------------
A draw is identified by **(vertex shader, pixel shader, vertex count)**. That triple is
stable across the two stacks — the shaders are the guest's own microcode hashed the same
way on both sides, and the vertex count is a property of the mesh — while nothing else is:
the two runs are different sessions, so every guest ADDRESS differs, and comparing
addresses would report 100% disagreement on a perfectly correct frame.

So what is compared per fetch slot is the texture's SHAPE — extent, format, dimension and
stack depth — which is a fingerprint of *which asset* is bound without depending on where
it happens to live. A slot where hardware has a 512x512 DXT1 and we have a 4x4 8888 is
bound to the wrong thing; a slot where both say 512x512 DXT1 is almost certainly right.

It cannot prove two same-shaped textures are the same IMAGE. That needs the bytes, which
`xtr_draw_bindings.py --dump-texture` extracts and `CZ_VK_TEX_DUMP` writes for our side —
this narrows the search to the slots worth doing that for.

A signature can appear many times in a frame (a crowd is one mesh drawn 200 times). Draws
are therefore grouped by signature and the SETS of slot shapes are compared, so 200
identical zombies are one row and not 200.

USAGE
    draw_binding_diff.py <ours.census> <hardware.csv> [--max N] [--all]
"""
import argparse
import collections
import csv
import json
import re
import sys
from pathlib import Path

# `draw 825 verts=25234 prim=6 vs=36ee... ps=ad65... mask=F blend=00010001  s0=0DC01000 ...`
DRAW_RE = re.compile(
    r'^draw (\d+) verts=(\d+) prim=(\d+) vs=([0-9a-f]+) ps=([0-9a-f]+)')
SLOT_RE = re.compile(
    r's(\d+)=([0-9A-F]+) (\d+)x(\d+) fmt=(\d+) dim=(\d+) depth=(\d+)')


def load_ours(path):
    """signature -> {slot: shape}, plus a count of draws per signature."""
    out = collections.defaultdict(dict)
    seen = collections.Counter()
    truncated = 0
    for line in open(path):
        m = DRAW_RE.match(line)
        if not m:
            continue
        # A census line is capped at 2 KB and marks its own overflow. A truncated line
        # looks like a draw that binds fewer textures, which is indistinguishable from a
        # real disagreement — so they are counted and reported rather than compared
        # (gotcha 109).
        if 'TRUNCATED' in line:
            truncated += 1
            continue
        sig = (m.group(4), m.group(5), int(m.group(2)))
        seen[sig] += 1
        for s in SLOT_RE.finditer(line):
            out[sig].setdefault(int(s.group(1)), set()).add(
                (int(s.group(3)), int(s.group(4)), int(s.group(5)), int(s.group(6)),
                 int(s.group(7))))
    return out, seen, truncated


def load_hardware(path):
    out = collections.defaultdict(dict)
    seen = collections.Counter()
    per_draw = collections.defaultdict(dict)
    sig_of = {}
    for r in csv.DictReader(open(path)):
        if r.get('dim') in (None, '', 'None'):
            sys.exit('%s has no dim column — regenerate it with the current '
                     'tools/xtr_draw_bindings.py (it also gained the LOAD_ALU_CONSTANT '
                     'replay, so a stale CSV is wrong in two ways)' % path)
        sig = (r['vs'].removeprefix('vs_'), r['ps'].removeprefix('ps_'), int(r['verts']))
        sig_of[r['draw']] = sig
        per_draw[r['draw']][int(r['slot'])] = (int(r['w']), int(r['h']), int(r['fmt']),
                                               int(r['dim']), int(r['depth']))
    for draw, slots in per_draw.items():
        sig = sig_of[draw]
        seen[sig] += 1
        for slot, shp in slots.items():
            out[sig].setdefault(slot, set()).add(shp)
    return out, seen


def shape(v):
    """One slot's shape, or the several a signature was seen with."""
    if not v:
        return '-- not bound --'
    return ' | '.join('%dx%-4d fmt=%-3d dim=%d depth=%d' % t for t in sorted(v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ours', help='capture_f<frame>.census from CZ_CAPTURE_KEY')
    ap.add_argument('hardware', help='CSV from tools/xtr_draw_bindings.py --csv')
    ap.add_argument('--max', type=int, default=25, help='disagreeing signatures to print')
    ap.add_argument('--all', action='store_true', help='also list the agreeing ones')
    ap.add_argument('--spv', default='assets/shader_spv')
    args = ap.parse_args()

    # name -> the fetch slots that shader declares, from the same sidecars the renderer
    # binds from.
    sidecars = {}
    for f in Path(args.spv).glob('*.meta.json'):
        try:
            sidecars[f.name.split('.')[0]] = json.load(open(f)).get('tfetchConsts') or []
        except Exception:
            pass

    ours, ourN, truncated = load_ours(args.ours)
    hw, hwN = load_hardware(args.hardware)

    common = set(ours) & set(hw)
    print('%s  vs  %s' % (args.ours, args.hardware))
    print('  draw signatures: %d ours, %d hardware, %d in BOTH' % (len(ours), len(hw),
                                                                   len(common)))
    print('  draws covered:   %d ours, %d hardware' % (sum(ourN.values()),
                                                       sum(hwN.values())))
    if truncated:
        print('  %d of our census lines were TRUNCATED and are excluded' % truncated)
    if not common:
        print('  NO SIGNATURE IN COMMON — are these the same place? A capture and a '
              'census of two different spots share almost nothing, and that is the '
              'first thing to rule out before reading any disagreement below.')
        return 1

    # COMPARE ONLY THE SLOTS THE SHADER DECLARES, and this is not a detail.
    #
    # The two sides populate their slot lists by different rules: our census lists the
    # slots `bindTextures` walks, i.e. the ones the sidecar's `tfetchConsts` names, while
    # `xtr_draw_bindings.py` lists every one of the 32 fetch constants that happens to
    # decode as a texture — and the fetch constant file is persistent, so slots a previous
    # draw set are still populated whether or not this shader reads them. Compared
    # naively, 328 of 405 shared signatures "disagreed", every one of them because
    # hardware listed slots we correctly never looked at.
    #
    # That is the second time in one session that two populations built by different
    # membership rules produced a false result (gotcha 264 was the first, on the same
    # subject). The rule here is the RUNTIME's rule, because the question is what the
    # runtime bound.
    declared = {}
    for sig in common:
        s = set()
        for stage, h in (('vs', sig[0]), ('ps', sig[1])):
            m = sidecars.get('%s_%s' % (stage, h))
            if m:
                s |= {c for c in m if c < 16}
        declared[sig] = s

    # AND EXCLUDE SIGNATURES THAT BIND MORE THAN ONE TEXTURE PER SLOT WITHIN A FRAME.
    #
    # (vertex shader, pixel shader, vertex count) identifies a MESH, not a draw. The UI
    # renderer draws forty different elements through one shader pair at the same vertex
    # count and binds a different atlas to each, so grouping by signature collapses forty
    # distinct bindings into one and whichever came last wins. Compared that way the top
    # five "disagreements" in the slot-machine frame were all that one shader pair, and all
    # of them were an artifact of the grouping.
    #
    # A signature whose slot took several shapes on either side is therefore AMBIGUOUS and
    # excluded, with the count reported: it is not evidence of agreement and it is not
    # evidence of a defect. Narrowing it further needs a per-draw key the two stacks share,
    # which they do not have -- so this is the honest boundary of what a signature diff can
    # say, and saying so is better than a confident wrong answer.
    agree = disagree = nodecl = ambiguous = 0
    rows = []
    for sig in common:
        slots = sorted(declared[sig])
        if any(len(ours[sig].get(s, ())) > 1 or len(hw[sig].get(s, ())) > 1
               for s in slots):
            ambiguous += 1
            continue
        if not slots:
            # No sidecar, or a shader that samples nothing. Not comparable, and counted
            # rather than silently folded into "agree".
            nodecl += 1
            continue
        bad = [s for s in slots if ours[sig].get(s) != hw[sig].get(s)]
        if bad:
            disagree += 1
            rows.append((ourN[sig] + hwN[sig], sig, slots, bad))
        else:
            agree += 1
    print('  comparing only the fetch slots the SHADER declares (%d sidecars loaded)'
          % len(sidecars))
    print('  of the %d shared signatures: %d agree on every declared slot, %d DISAGREE, '
          '%d declare none, %d ambiguous (one signature, several textures)'
          % (len(common), agree, disagree, nodecl, ambiguous))
    # Ordered by how much of the frame the signature accounts for, because a disagreement
    # on a 200-draw crowd mesh matters more than one on a single 4-vertex quad, and an
    # unordered list buries the first behind the second.
    for _, sig, slots, bad in sorted(rows, key=lambda r: -r[0])[:args.max]:
        print('\n  vs=%s ps=%s verts=%d   (%d draws ours, %d hardware)'
              % (sig[0], sig[1], sig[2], ourN[sig], hwN[sig]))
        for s in slots:
            mark = '   <-- DIFFERENT' if s in bad else ''
            print('    s%-2d ours %-28s hw %-28s%s'
                  % (s, shape(ours[sig].get(s)), shape(hw[sig].get(s)), mark))
    if args.all:
        for sig in sorted(common, key=lambda g: -(ourN[g] + hwN[g])):
            if declared[sig] and all(ours[sig].get(s) == hw[sig].get(s)
                                     for s in declared[sig]):
                print('  AGREE vs=%s ps=%s verts=%d  (%d slots)'
                      % (sig[0], sig[1], sig[2], len(ours[sig])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
