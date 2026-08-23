#!/usr/bin/env python3
"""WHERE EACH VERTEX SHADER KEEPS ITS WORLD MATRIX.

WHY THIS EXISTS
---------------
Part 66 established, by measurement, that the ray-tracing structure is effectively a
GROUND PLANE: `CZ_VK_RT_FACTOR_DEBUG=20` reads 97.3% of receivers fully open over the
whole upper hemisphere, so no sun vector could ever have produced a shadow. Part 67's
census found why, and it is not a filter throwing geometry away:

    THE POSITION STREAMS ARE OBJECT-SPACE. Every mesh enters our BLAS untransformed
    and every TLAS instance carries an IDENTITY transform, so the whole town is piled
    on top of itself at the world origin.

Measured over the twenty `.xtr` world traces, 46,820 structurally-accepted draws:
transformed by the camera composite at `vc(0..3)` alone, **11.7%** of their bounding
boxes intersect the frustum they were drawn into; with the per-draw matrix at
`vc(8..10)` applied first, **93.8%** do. Per-VERTEX the gap is starker still — 0.0%
against 61-98%. And 100% of those draws carry a NON-IDENTITY world translation, spread
over x[-610, 124] z[-681, 106], which is the town.

`rtshadow::Collect` gates on `SceneXformForm(c0..c3) == 2`, and §6cs read that as "the
position stream is therefore world-space". It is not: c0..c3 is the camera's
view-projection, which is the same matrix whether the shader feeds it a world position
or an object position it transformed a line earlier. The test proves what c0..c3 IS, not
what is fed to it (gotcha: an input's SPACE is a property of the shader, not of the
matrix it is finally multiplied by).

WHY A TABLE, RATHER THAN "ALWAYS USE vc(8..10)"
-----------------------------------------------
Because the bank has more than one shape. Two exist in the microcode:

    direct   r1.x = dot(vc(8),  pos4)        one row-major 4x3 at vc(N..N+2)
             r1.y = dot(vc(9),  pos4)
             r1.z = dot(vc(10), pos4)
             oPos = mul(vc(0..3), r1)

    palette  a matrix BLENDED from vc(N + a0) entries by three per-vertex weights,
             then a SECOND 4x3 at vc(4..6), then the camera composite. The bank's
             busiest world shader (`vs_b677dc3457f5b41a`, 2,658 of the gas-station
             frame's 4,512 accepted draws) is this shape, and composing its second
             stage takes it from 81.3% of vertices on screen to 99.5%.

Guessing one shape for all of them would silently misplace the other, and a misplaced
occluder is not a visible error — it is a shadow in the wrong place, which is exactly
the class of defect this feature has spent four parts failing to see. So the shapes are
READ OUT OF THE MICROCODE, per shader, and anything that matches neither is NAMED rather
than defaulted (gotcha 5: an unsupported case fails loudly with its identifier).

HOW
---
Each `vs_*.ucode` is wrapped by `synth_shader_container.py` and translated by XenosRecomp
exactly as `build_shader_spv.sh` does it — the same translator the runtime's cache is
built with, so the HLSL read here is the code that actually runs. The position dataflow
is then walked BACKWARDS from `oPos`, one stage at a time, to the register that was
assigned from `iPositionN.xyz`.

OUTPUT
    config/rt_world_xform.json   {vs_hash: {"stages": [[base, kind], ...]}}
    with the stages ordered INNERMOST FIRST, i.e. the order they are applied to the
    object-space position.

It is also a GATE: exit 1 when the shader cache holds a vertex shader this table does
not cover, because a shader with no entry is a mesh the runtime silently declines to
place — the same shape of silent gap as the ten missing play-cache modules of gotcha 390.

USAGE
    tools/rt_world_xform_census.py                       # the standard dump directory
    tools/rt_world_xform_census.py --ucode DIR -o FILE
    tools/rt_world_xform_census.py --verbose             # per-shader lines
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
XENOS = Path.home() / 'GithubRepo' / 'XenosRecomp'
DEFAULT_UCODE = Path.home() / 'DR2CZ-troubleshooting' / 'ucode-dumps'

ASSIGN = re.compile(r'^\s*(r\d+)\.([xyzw]+)\s*=\s*(.+);\s*$')
POSINIT = re.compile(r'^\s*(r\d+)\.xyz\s*=\s*i(?:Position|POSITION)\d+\.xyz\s*;\s*$')
OPOS = re.compile(r'^\s*oPos\.([xyzw])\s*=\s*dot\(vc\((\d+)\)[.\w]*,\s*(r\d+)[.\w]*\)\s*;\s*$')
DOT_VC = re.compile(r'^dot\(vc\((\d+)\)[.\w]*,\s*(r\d+)[.\w]*\)$')
DOT_RR = re.compile(r'^dot\((r\d+)[.\w]*,\s*(r\d+)[.\w]*\)$')
VC_INDEXED = re.compile(r'vc\((\d+)\s*\+\s*a0\)')

# --- THE PALETTE BLEND'S OWN INPUTS (the Remix plan's item 3) -------------------------
# `palette@N` says WHERE the matrices are; these three say which per-vertex bytes select
# and weight them. All three are read out of the translated microcode for the same reason
# the stage bases are: a guessed component is a silently misplaced occluder.
DEPFETCH = re.compile(
    r'^\s*(r\d+)\.([xyzw]+)\s*=\s*XeVfetchDep\('
    r'(\d+)u,\s*[^,]+,\s*(\d+)u,\s*(\d+)u,\s*(-?\d+),\s*(\d+)u,\s*(\d+)u,\s*(\d+)u'
    r'\)\.([xyzw]+)\s*;\s*$')
A0_FROM = re.compile(r'a0\s*=\s*\(int\)clamp\(floor\((r\d+)\.([xyzw])\s*\+\s*0\.5\)')
WEIGHTED_ROW = re.compile(
    r'^\s*r\d+\.\w+\s*=\s*(r\d+)\.([xyzw])\2*\s*\*\s*vc\((\d+)\s*\+\s*a0\)')


LOCDECL = re.compile(r'\[\[vk::location\((\d+)\)\]\]\s+in\s+float4\s+(i\w+)\s*:')
TFETCH_TC = re.compile(
    r'^\s*(r\d+)\.([xyzw]+)\s*=\s*tfetchTexcoord\([^,]+,\s*(i\w+),\s*\d+\)'
    r'\.([xyzw]+)\s*;\s*$')
PLAIN_IN = re.compile(r'^\s*(r\d+)\.([xyzw]+)\s*=\s*(i\w+)\.([xyzw]+)\s*;\s*$')


def blend_inputs(text, base, meta):
    """The per-vertex weight/index bytes a `palette@base` stage blends with.

    Returns (descriptor, note). The descriptor is the compact string the runtime reads:

        slot : strideDwords : weightOffsetDw : indexOffsetDw : bytes

    where `bytes` are the components of the two 8_8_8_8 dwords, one digit per influence,
    IN THE ORDER THE SHADER APPLIES THEM. That order is not decoration: the weights are
    normalised bytes that need not sum to one, so applying them to the wrong indices is a
    mesh placed between two props rather than on either.

    The canonical form this title uses — verified rather than assumed, and anything that
    does not match is NAMED so the runtime declines it instead of guessing (gotcha 5):

        * both inputs are dependent fetches on ONE slot, `k_8_8_8_8`, same stride;
        * the weight fetch is num_format NORMALISED and the index fetch INTEGER, which is
          the shader's own declaration of which is which;
        * each influence pairs weight byte k with index byte k;
        * the three `vc(base + a0)` rows read per influence are base, base+1, base+2.
    """
    lines = text.split('\n')
    # (reg, component) -> the vertex-buffer component it was assigned from.
    #
    # TWO SHAPES REACH HERE and they are the same data bound two different ways, which is
    # why the descriptor below records the buffer layout rather than the binding. Twelve
    # of this bank's eighteen palette shaders take the blend inputs as DECLARED vertex
    # attributes (`iTexCoord0`/`iTexCoord1`, i.e. ordinary bound streams) and six take
    # them through a DEPENDENT fetch (`XeVfetchDep`, the slot read with the vertex index).
    # Both end up reading offsets 6 and 7 of the same 8-dword stride, because it is one
    # interleaved vertex buffer either way. Reading only the dependent form — which the
    # first version of this did — would have declined two thirds of the population while
    # reporting that it had covered it.
    loc = {name: int(n) for n, name in LOCDECL.findall(text)}
    by_loc = {}
    for a in (meta.get('attributes') or []):
        if a.get('location', -1) >= 0 and not a.get('indirect'):
            by_loc[a['location']] = a
    dep = {}
    for ln in lines:
        m = DEPFETCH.match(ln)
        if m:
            reg, dst, slot, stride, off, fmt, sgn, numint, src = (
                m.group(1), m.group(2), m.group(3), m.group(5), m.group(6),
                m.group(7), m.group(8), m.group(9), m.group(10))
            if len(dst) != len(src):
                continue
            for d, sc in zip(dst, src):
                dep[(reg, d)] = {'slot': int(slot), 'stride': int(stride),
                                 'off': int(off), 'fmt': int(fmt), 'sgn': int(sgn),
                                 'int': int(numint), 'comp': 'xyzw'.index(sc)}
            continue
        m = TFETCH_TC.match(ln) or PLAIN_IN.match(ln)
        if not m:
            continue
        reg, dst, name, src = m.groups()
        a = by_loc.get(loc.get(name, -1))
        if a is None or len(dst) != len(src):
            continue
        for d, sc in zip(dst, src):
            dep[(reg, d)] = {'slot': a['fetchSlot'], 'stride': a['strideDwords'],
                             'off': a['offsetDwords'], 'fmt': a['format'],
                             'sgn': a['signed'], 'int': a['integer'],
                             'comp': 'xyzw'.index(sc)}

    influences = []          # (weight source, index source, vc row) in shader order
    cur = None
    for ln in lines:
        m = A0_FROM.match(ln.strip())
        if m:
            cur = (m.group(1), m.group(2))
            continue
        m = WEIGHTED_ROW.match(ln)
        if m and cur is not None:
            wreg, wcomp, row = m.group(1), m.group(2), int(m.group(3))
            influences.append((( wreg, wcomp), cur, row))
    if not influences:
        return None, 'no weighted vc(N + a0) row follows an a0 assignment'

    # Group by influence (one a0 assignment feeds three consecutive rows).
    by_inf = {}
    order = []
    for w, i, row in influences:
        k = (w, i)
        if k not in by_inf:
            by_inf[k] = set()
            order.append(k)
        by_inf[k].add(row)
    rows = set()
    for k in order:
        rows |= by_inf[k]
    if rows != {base, base + 1, base + 2}:
        return None, ('the palette rows are %s, not three consecutive from %d'
                      % (sorted(rows), base))

    fetches = {}
    bytes_ = []
    for w, i in order:
        dw, di = dep.get(w), dep.get(i)
        if dw is None or di is None:
            return None, 'a weight or index component is not a dependent-fetch result'
        if dw['fmt'] != 6 or di['fmt'] != 6:
            return None, ('the blend inputs are formats %d/%d, not k_8_8_8_8'
                          % (dw['fmt'], di['fmt']))
        if dw['int'] or not di['int']:
            return None, 'the normalised/integer pair does not identify weights vs indices'
        if dw['comp'] != di['comp']:
            return None, ('influence pairs weight byte %d with index byte %d'
                          % (dw['comp'], di['comp']))
        if dw['slot'] != di['slot'] or dw['stride'] != di['stride']:
            return None, 'the two blend fetches disagree on slot or stride'
        fetches.setdefault('slot', dw['slot'])
        fetches.setdefault('stride', dw['stride'])
        fetches.setdefault('w', dw['off'])
        fetches.setdefault('i', di['off'])
        if (dw['slot'], dw['stride'], dw['off'], di['off']) != (
                fetches['slot'], fetches['stride'], fetches['w'], fetches['i']):
            return None, 'influences read different fetch descriptors'
        bytes_.append(dw['comp'])
    return ('%d:%d:%d:%d:%s' % (fetches['slot'], fetches['stride'], fetches['w'],
                                fetches['i'], ''.join(str(b) for b in bytes_))), None


def translate(ucode_dir, out_dir):
    """synth + XenosRecomp, the same pair build_shader_spv.sh uses."""
    synth = out_dir / 'synth'
    hl = out_dir / 'hlsl'
    hl.mkdir(parents=True, exist_ok=True)
    vs = out_dir / 'vsuc'
    vs.mkdir(parents=True, exist_ok=True)
    n = 0
    for p in sorted(Path(ucode_dir).glob('vs_*.ucode')):
        shutil.copy(p, vs / p.name)
        n += 1
    if not n:
        return None, 'no vs_*.ucode under %s' % ucode_dir

    r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'synth_shader_container.py'),
                        str(vs), str(synth)], capture_output=True, text=True)
    if r.returncode:
        return None, 'synth_shader_container failed: %s' % r.stderr.strip()[:400]
    failed = []
    for x in sorted(synth.glob('*.xshd')):
        b = x.stem
        r = subprocess.run([str(XENOS / 'build' / 'XenosRecomp' / 'XenosRecomp'), str(x),
                            str(hl / (b + '.hlsl')),
                            str(XENOS / 'XenosRecomp' / 'shader_common.h')],
                           capture_output=True, text=True)
        if r.returncode:
            failed.append(b)
    return (hl, synth, failed), None


def last_assign(lines, reg, comp, before):
    """The line that last wrote reg.comp before line `before` — the dataflow step."""
    for i in range(before - 1, -1, -1):
        m = ASSIGN.match(lines[i])
        if m and m.group(1) == reg and comp in m.group(2):
            return i, m.group(3).strip()
    return None, None


def classify(text):
    """The position dataflow, walked backwards from oPos. Returns (stages, note)."""
    lines = text.split('\n')
    pos_reg = None
    for ln in lines:
        m = POSINIT.match(ln)
        if m:
            pos_reg = m.group(1)
            break
    if pos_reg is None:
        return None, 'no register is assigned from iPosition*.xyz'

    opos = {}
    for i, ln in enumerate(lines):
        m = OPOS.match(ln)
        if m:
            opos[m.group(1)] = (i, int(m.group(2)), m.group(3))
    if set(opos) != set('xyzw'):
        return None, 'oPos is not four dot(vc(N), r) lines'
    bases = [opos[c][1] for c in 'xyzw']
    srcs = {opos[c][2] for c in 'xyzw'}
    if len(srcs) != 1 or bases != [bases[0] + k for k in range(4)]:
        return None, 'the camera composite is not four consecutive vc() rows on one reg'

    src = srcs.pop()
    at = min(opos[c][0] for c in 'xyzw')
    stages = []
    for _ in range(8):                      # a bounded walk; 8 is far past any real chain
        if src == pos_reg:
            return stages, None
        steps = [last_assign(lines, src, c, at) for c in 'xyz']
        if any(s[0] is None for s in steps):
            return None, 'register %s has no producer for one of xyz' % src
        rhs = [s[1] for s in steps]
        mv = [DOT_VC.match(r) for r in rhs]
        if all(mv):
            ks = [int(m.group(1)) for m in mv]
            ts = {m.group(2) for m in mv}
            if len(ts) == 1 and ks == [ks[0] + k for k in range(3)]:
                stages.insert(0, [ks[0], 'direct'])
                src = ts.pop()
                at = min(s[0] for s in steps)
                continue
            return None, 'a 4x3 stage is not three consecutive vc() rows on one reg'
        mr = [DOT_RR.match(r) for r in rhs]
        if all(mr):
            # A matrix built in REGISTERS: the palette blend. Its constants are the
            # `vc(N + a0)` reads that produced those registers; the base is the lowest.
            mats = {m.group(1) for m in mr}
            ts = {m.group(2) for m in mr}
            if len(ts) != 1:
                return None, 'a register-built matrix reads more than one source reg'
            first = min(s[0] for s in steps)
            idx = set()
            for mat in mats:
                j = first
                for _hop in range(16):
                    j, r = last_assign(lines, mat, 'x', j)
                    if j is None:
                        break
                    idx.update(int(g) for g in VC_INDEXED.findall(r))
                    if '+ %s' % mat not in r:
                        break
            if not idx:
                return None, 'a register-built matrix reads no vc(N + a0) constants'
            stages.insert(0, [min(idx), 'palette'])
            src = ts.pop()
            at = first
            continue
        return None, 'a stage is neither dot(vc(N), r) nor dot(r, r)'
    return None, 'the position chain did not reach iPosition in 8 stages'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ucode', default=str(DEFAULT_UCODE))
    ap.add_argument('--hlsl', help='a directory of already-translated .hlsl (skips XenosRecomp)')
    ap.add_argument('-o', '--out', default=str(ROOT / 'config' / 'rt_world_xform.json'))
    ap.add_argument('--cache', default=str(ROOT / 'assets' / 'shader_spv'),
                    help='the shader cache to gate coverage against; exit 1 on a gap')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    tmp = None
    if args.hlsl:
        hl, synth, failed = Path(args.hlsl), Path(args.hlsl), []
    else:
        tmp = tempfile.mkdtemp(prefix='rtxf')
        got, err = translate(args.ucode, Path(tmp))
        if err:
            print(err, file=sys.stderr)
            return 2
        hl, synth, failed = got
    if failed:
        print('XenosRecomp could not translate %d shader(s): %s'
              % (len(failed), ' '.join(failed)))

    table = {}
    shapes = {}
    bad = []
    nb = []      # palette shaders whose blend inputs could not be read
    for p in sorted(hl.glob('vs_*.hlsl')):
        stages, note = classify(open(p).read())
        if stages is None:
            bad.append((p.stem, note))
            continue
        # ONE representation, a compact string, because two would drift: the runtime's
        # reader and this tool's census line both parse `kind@base` pairs.
        key = ','.join('%s@%d' % (k, b) for b, k in stages) or 'camera'
        entry = {'stages': key}
        # The blend descriptor, for exactly the stages that need one. Its absence is what
        # makes the runtime fall back to entry 0 with unit weight, and part 68's census of
        # hardware's own index streams says that approximation is wrong for essentially
        # every palette draw: over 2,786 of them in the gas-station trace, NOT ONE
        # references a single matrix and the median is 19 distinct entries. So a palette
        # shader whose blend cannot be read here is a mesh placed on the wrong prop, and
        # it is named rather than defaulted.
        for b, k in stages:
            if k != 'palette':
                continue
            meta = {}
            mp = synth / (p.stem + '.meta.json')
            if mp.is_file():
                meta = json.load(open(mp))
            desc, note = blend_inputs(open(p).read(), b, meta)
            if desc:
                entry['blend'] = desc
            else:
                nb.append((p.stem, 'palette@%d: %s' % (b, note)))
            break
        table[p.stem] = entry
        shapes[key] = shapes.get(key, 0) + 1
        if args.verbose:
            print('%-24s %-24s %s' % (p.stem, key, entry.get('blend', '')))

    print('\n%d vertex shaders classified, %d not:' % (len(table), len(bad)))
    nblend = sum(1 for v in table.values() if 'blend' in v)
    npal = sum(1 for v in table.values() if 'palette' in v['stages'])
    print('    %d of %d palette shaders carry a readable blend descriptor' % (nblend, npal))
    for n, note in nb:
        print('    NO BLEND: %-22s %s' % (n, note))
    for key in sorted(shapes, key=lambda k: -shapes[k]):
        print('    %-34s %4d shaders' % (key, shapes[key]))
    if bad:
        # NAMED, never defaulted: a shader whose transform we cannot read must not be
        # placed at the origin on a guess. The runtime skips these and counts them.
        print('\n  UNCLASSIFIED — the runtime will decline to place these, and count it:')
        for n, note in bad:
            print('    %-24s %s' % (n, note))

    # THE COVERAGE GATE, and it exists because gotcha 390 is the same defect one level
    # up: a shader present in the cache but absent from this table is a mesh the runtime
    # DECLINES to place, silently, in every session — the exact shape of the ten missing
    # play-cache modules that cost three parts. Exit 1 rather than a line in a log.
    missing = []
    known = set(table) | {n for n, _ in bad}
    cache = Path(args.cache)
    if cache.is_dir():
        missing = sorted(p.stem for p in cache.glob('vs_*.spv') if p.stem not in known)
        print('shader cache %s: %d vertex shaders, %d covered by this table'
              % (cache, len(list(cache.glob('vs_*.spv'))),
                 len([p for p in cache.glob('vs_*.spv') if p.stem in known])))
        for m in missing:
            print('    NOT IN THE TABLE: %s — its microcode is not in %s'
                  % (m, args.ucode))

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # Written one shader per LINE, because runtime/gpu/vk_renderer.cpp reads it with the
    # same deliberately small JSON reader the shader sidecars use: it finds the shader's
    # own key and then the next "stages" string. Multi-line pretty printing would still
    # parse, but a one-line entry makes that contract obvious to whoever edits either end.
    with open(out, 'w') as f:
        f.write('{\n')
        f.write(' "_comment": "Generated by tools/rt_world_xform_census.py. Each entry '
                'names the VS constant rows carrying that shader\'s object->world '
                'transform, INNERMOST STAGE FIRST, as kind@base pairs. `direct` is a '
                'row-major 4x3 at vc(base..base+2); `palette` is a per-vertex blend '
                'blended per vertex. A palette stage also carries "blend": '
                '"slot:strideDwords:weightOffsetDw:indexOffsetDw:bytes", the dependent '
                'fetch the per-vertex weights and matrix indices come through, with one '
                'byte digit per influence IN THE ORDER THE SHADER APPLIES THEM. '
                'Regenerate after any change to the shader bank.",\n')
        f.write(' "shaders": {\n')
        items = sorted(table.items())
        for i, (k, v) in enumerate(items):
            blend = (', "blend": "%s"' % v['blend']) if 'blend' in v else ''
            f.write('  "%s": {"stages": "%s"%s}%s\n'
                    % (k, v['stages'], blend, ',' if i + 1 < len(items) else ''))
        f.write(' },\n')
        f.write(' "unclassified": [%s]\n'
                % ', '.join('"%s"' % n for n, _ in sorted(bad)))
        f.write('}\n')
    print('\nwrote %s (%d shaders)' % (out, len(table)))
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
