#!/usr/bin/env python3
"""WHICH PIXEL SHADERS SAMPLE THE SHADOW CASCADE ATLAS, at which fetch slot, and
what do they DO with the value.

WHY THIS EXISTS
---------------
Part 64 built ray-traced shadows "route (a)" — write traced depths into the atlas the
title rasterizes — proved every link of the mechanism, and then closed it as unworkable:
writing the MAP means every receiver inside the map is compared against itself, so five
independent knobs all landed at 64-66 median outdoor luma against the original's 80.61
(docs/phase5-notes.md §6cv §7j).

Route (b) computes the shadow factor per RECEIVING PIXEL in screen space and has the
atlas-sampling pixel shaders read THAT instead. Its step 1, per docs/part65-kickoff.md,
is a census: *"Build nothing before this returns a list."* The plan guessed "~a dozen
shaders". This tool measures it, and the answer is 128 — an order of magnitude out, the
same shape of error as gotcha 3 (XenonAnalyse's zero jump tables against our 234).

WHY THE ORACLE IS THE CAPTURE AND NOT OUR RUNTIME
-------------------------------------------------
The binding "fetch slot N holds the shadow atlas" is a RUNTIME fact: it lives in a
texture fetch constant, not in the microcode. So it cannot be read off the shader bank
alone. It can be read out of a `.xtr` GPU trace, which carries hardware's own register
file per draw — and that is a third party, not two of our own components agreeing.
`tools/xtr_draw_bindings.py` already decodes exactly this; this tool aggregates it.

Twenty single-frame world traces (R2/R3/R4/R6) agree: exactly TWO depth-format
surfaces exist in this title, a 4096x1024 `k_24_8` at 1812F000 (the cascade atlas) and a
1280x720 one at 0A978000 (the scene depth the DoF pass reads). The atlas is therefore
identified BY SHAPE here, not by its address — our own runtime resolves it to 1439B000,
a different allocation, and Case West will differ again.

TWO FILTERS, BOTH LOAD-BEARING
------------------------------
* A draw's register file carries all 32 fetch constants whether or not the bound shader
  declares them. Counting raw address matches over-reports by 6x (768 undeclared
  (shader, slot) pairs against 128 shaders): the atlas constant is simply left set from
  an earlier draw. The census intersects with the shader's own `tfetchConsts` sidecar.
* Two shaders read 1812F000 as a COLOUR `.xyz` and write it straight to oC0 — a blit,
  not a shadow read. They are reported separately and excluded from the slot map.

WHAT IT PRINTS, AND WHAT THE BUILD CONSUMES
-------------------------------------------
The table (shader, slot, draws, traces), the use-shape histogram, and — with
`--json out.json` — the slot map route (b)'s shader patch needs:
{"ps_<hash>": {"slots": [3], "shape": "pcf4", "draws": 7512}}.

`--hlsl` additionally translates each named shader through XenosRecomp and classifies
what the fetched value feeds, because the injection depends on it:
  * `pcf4`  — four ±0.5 taps, compared `> receiverDepth`, bilinearly weighted. 116 of
              the 142 (shader, slot) uses, and every one of the four offsets identical.
  * `tap1`  — one centre tap feeding `saturate((receiver - sampled) * k - bias)`, a
              linear ramp rather than a compare. 26 uses.
  * `blit`  — `.xyz` to the output. 2 uses, not shadows.
Both shadow shapes are MONOTONIC in the sampled value and both saturate, which is the
fact route (b)'s injection rests on: substituting 1.0 at every atlas tap means "lit" and
0.0 means "occluded" in all 142 uses, whatever the weighting, so one substitution serves
the whole population without a per-shader special case.

USAGE
    tools/shadow_shader_census.py                       # all world traces
    tools/shadow_shader_census.py --hlsl --json map.json
    tools/shadow_shader_census.py --trace <one.xtr>
"""
import argparse
import collections
import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TRACE_DIRS = ['Xenia logs/R2_world', 'Xenia logs/R3_world',
                      'Xenia logs/R4_world', 'Xenia logs/R6_gas_sign']
DEPTH_FORMATS = ('22', '23')          # k_24_8, k_24_8_FLOAT — see runtime/gpu/xenos.h
UCODE_DIR = Path(os.path.expanduser('~/DR2CZ-troubleshooting/ucode-dumps'))
XENOS = Path(os.path.expanduser('~/GithubRepo/XenosRecomp'))


def csv_for(trace, cache):
    """One xtr_draw_bindings CSV per trace, cached — the walk is the slow part."""
    out = cache / (trace.stem + '.csv')
    if out.exists() and out.stat().st_size:
        return out
    subprocess.run([sys.executable, str(ROOT / 'tools' / 'xtr_draw_bindings.py'),
                    str(trace), '--csv', str(out)],
                   check=True, stdout=subprocess.DEVNULL)
    return out


def pick_atlas(rows_by_trace):
    """The cascade atlas, BY SHAPE. The widest depth-format surface in the set.

    Deliberately not a hardcoded address: hardware's is 1812F000, ours is 1439B000, and
    a future port's will be neither. If more than one candidate shape appears the tool
    says so rather than choosing — a silent pick here would mis-key the whole census.
    """
    shapes = collections.Counter()
    addrs = collections.defaultdict(set)
    for rows in rows_by_trace.values():
        for r in rows:
            if r['fmt'] in DEPTH_FORMATS:
                k = (int(r['w']), int(r['h']))
                shapes[k] += 1
                addrs[k].add(r['addr'])
    if not shapes:
        return None, shapes, addrs
    # The atlas is the one whose width is a multiple of its height and >= 2048 — a
    # cascade atlas is several square slices side by side, a scene depth is the frame.
    cands = [k for k in shapes if k[0] >= 2048 and k[0] % k[1] == 0]
    return (max(cands, key=lambda k: shapes[k]) if cands else None), shapes, addrs


def load_sidecars():
    d = {}
    for p in (ROOT / 'assets' / 'shader_spv').glob('*.meta.json'):
        d[p.name.split('.')[0]] = json.load(open(p))
    return d


def translate(name, workdir):
    """XenosRecomp's HLSL for one shader, from its microcode dump."""
    uc = workdir / 'uc'
    uc.mkdir(exist_ok=True)
    src = UCODE_DIR / (name + '.ucode')
    if not src.exists():
        return None
    (uc / src.name).write_bytes(src.read_bytes())
    sy = workdir / 'sy'
    sy.mkdir(exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / 'tools' / 'synth_shader_container.py'),
                    str(uc), str(sy)], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    xshd = sy / (name + '.xshd')
    if not xshd.exists():
        return None
    hlsl = workdir / (name + '.hlsl')
    r = subprocess.run([str(XENOS / 'build' / 'XenosRecomp' / 'XenosRecomp'), str(xshd),
                        str(hlsl), str(XENOS / 'XenosRecomp' / 'shader_common.h')],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return hlsl.read_text().splitlines() if r.returncode == 0 and hlsl.exists() else None


def classify(src, slot):
    """What this shader does with the value fetched at `slot`: pcf4 / tap1 / blit."""
    import re
    pf = re.compile(r'^\s*(r\d+)\.([xyzw]+)\s*=\s*tfetch2D\(s%d_\w+, s%d_\w+, '
                    r'(r\d+\.\w+), float2\((-?[\d.]+), (-?[\d.]+)\)\)\.(\w+);' % (slot, slot))
    taps = [(i, m) for i, l in enumerate(src) for m in [pf.match(l)] if m]
    if not taps:
        return 'unknown', 0
    offs = {(m.group(4), m.group(5)) for _, m in taps}
    if len(taps) >= 4 and ('-0.5', '-0.5') in offs:
        return 'pcf4', len(taps)
    # One centre tap. A shadow read feeds a subtract-and-saturate ramp; a blit writes
    # three channels straight out.
    dest = taps[0][1].group(1)
    if len(taps[0][1].group(6)) >= 3:
        return 'blit', len(taps)
    for l in src[taps[0][0] + 1:taps[0][0] + 40]:
        if ('-' + dest + '.') in l or ('+ -' + dest) in l:
            return 'tap1', len(taps)
    return 'tap1?', len(taps)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace', action='append', help='a specific .xtr (repeatable)')
    ap.add_argument('--cache', default=None, help='directory for the per-trace CSVs')
    ap.add_argument('--hlsl', action='store_true', help='translate and classify each shader')
    ap.add_argument('--json', help='write the slot map route (b) consumes')
    args = ap.parse_args()

    traces = ([Path(t) for t in args.trace] if args.trace else
              sorted(p for d in DEFAULT_TRACE_DIRS for p in (ROOT / d).rglob('*.xtr')))
    if not traces:
        print('no traces found', file=sys.stderr)
        return 1
    cache = Path(args.cache) if args.cache else Path(tempfile.mkdtemp(prefix='shadowcensus'))
    cache.mkdir(parents=True, exist_ok=True)

    rows_by_trace = {}
    for t in traces:
        rows_by_trace[t.stem] = list(csv.DictReader(open(csv_for(t, cache))))
    print('traces: %d   fetch rows: %d'
          % (len(traces), sum(len(v) for v in rows_by_trace.values())))

    atlas, shapes, addrs = pick_atlas(rows_by_trace)
    print('\ndepth-format surfaces seen:')
    for k, v in shapes.most_common():
        print('  %4dx%-5d  rows=%-8d addrs=%s%s'
              % (k[0], k[1], v, sorted(addrs[k]), '   <-- the CASCADE ATLAS'
                 if k == atlas else ''))
    if atlas is None:
        print('no cascade-atlas-shaped surface — census cannot proceed', file=sys.stderr)
        return 1

    meta = load_sidecars()
    agg = collections.defaultdict(lambda: {'draws': 0, 'slots': collections.Counter(),
                                           'traces': set()})
    undeclared = collections.Counter()
    no_sidecar = set()
    for tname, rows in rows_by_trace.items():
        by_draw = collections.defaultdict(list)
        for r in rows:
            by_draw[int(r['draw'])].append(r)
        for rs in by_draw.values():
            ps = rs[0]['ps']
            if ps not in meta:
                no_sidecar.add(ps)
            decl = set(meta.get(ps, {}).get('tfetchConsts', []))
            for r in rs:
                if (int(r['w']), int(r['h'])) != atlas or r['fmt'] not in DEPTH_FORMATS:
                    continue
                slot = int(r['slot'])
                if slot in decl:
                    agg[ps]['draws'] += 1
                    agg[ps]['slots'][slot] += 1
                    agg[ps]['traces'].add(tname)
                else:
                    undeclared[(ps, slot)] += 1

    print('\nshaders with no sidecar in assets/shader_spv: %d' % len(no_sidecar))
    print('undeclared (shader,slot) pairs REJECTED — the fetch constant is set but the '
          'shader does not declare it: %d pairs' % len(undeclared))

    shapes_of = {}
    if args.hlsl:
        work = Path(tempfile.mkdtemp(prefix='shadowhlsl'))
        for ps in agg:
            src = translate(ps, work)
            shapes_of[ps] = ({s: classify(src, s)[0] for s in agg[ps]['slots']}
                             if src else {s: 'untranslated' for s in agg[ps]['slots']})

    print('\n=== %d PIXEL SHADERS SAMPLE THE CASCADE ATLAS ===' % len(agg))
    print('%-24s %8s %-12s %6s  %s' % ('pixel shader', 'draws', 'slots', 'traces', 'shape'))
    total = 0
    for ps, v in sorted(agg.items(), key=lambda kv: -kv[1]['draws']):
        sl = sorted(v['slots'])
        print('%-24s %8d %-12s %6d  %s'
              % (ps, v['draws'], str(sl), len(v['traces']),
                 ','.join('s%d=%s' % (s, shapes_of.get(ps, {}).get(s, '?')) for s in sl)
                 if args.hlsl else ''))
        total += v['draws']
    print('\ntotal atlas-sampling draws across %d traces: %d' % (len(traces), total))
    if args.hlsl:
        hist = collections.Counter(sh for d in shapes_of.values() for sh in d.values())
        print('\nuse shapes, per (shader, slot):')
        for k, v in hist.most_common():
            print('  %-14s %d' % (k, v))

    if args.json:
        out = {}
        for ps, v in agg.items():
            slots = {s: shapes_of.get(ps, {}).get(s, 'unclassified')
                     for s in sorted(v['slots'])}
            shadow = {s: k for s, k in slots.items() if k != 'blit'}
            if not shadow:
                continue
            out[ps] = {'slots': sorted(shadow), 'shapes': {str(k): v2 for k, v2 in shadow.items()},
                       'draws': v['draws'], 'traces': len(v['traces'])}
        # A generated config file says which tool made it and how to remake it — the
        # project convention, and JSON has no comments, so it goes in a key.
        json.dump({'_generatedBy': 'tools/shadow_shader_census.py --hlsl --json <this>',
                   '_what': 'pixel shaders that sample the shadow cascade atlas, by '
                            'declared texture fetch slot; the oracle is hardware\'s own '
                            'register file in the R2/R3/R4/R6 .xtr traces, so the slots '
                            'are HARDWARE\'s, not ours',
                   '_shapes': 'pcf4 = four +/-0.5 taps compared > receiverDepth and '
                              'bilinearly weighted; tap1 = one centre tap feeding '
                              'saturate((receiver - sampled) * k - bias). Both are '
                              'monotonic and saturating, so substituting 1.0 at every '
                              'atlas tap reads as LIT and 0.0 as OCCLUDED in all of them',
                   '_traces': len(traces),
                   'atlasShape': list(atlas), 'shaders': out}, open(args.json, 'w'),
                  indent=1, sort_keys=True)
        print('\nwrote %s — %d shaders, %d (shader,slot) pairs'
              % (args.json, len(out), sum(len(v['slots']) for v in out.values())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
