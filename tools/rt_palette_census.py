#!/usr/bin/env python3
"""HOW MANY MATRICES DOES A PALETTE-BLENDED DRAW ACTUALLY USE?

WHY THIS EXISTS
---------------
Part 68 closed with the RT occluders correctly PLACED and the remaining defect in the
POPULATION, and the largest half of that population is the `palette` shader shape:
`vs_b677dc3457f5b41a` alone carries 2,658 of one frame's 4,512 accepted draws, and
declining the whole class (`CZ_VK_RT_NO_PALETTE=1`) costs 60% of the world's occluders.
The collector places such a draw by taking palette ENTRY 0 with unit weight, which is
exact for a mesh that only ever uses entry 0 and wrong for anything that blends.

`docs/rt-remix-plan.md` item 3 says the shape of the fix turns on one number, and that
the number is answerable offline from captures already on disc:

    read the dependent fetch's actual per-vertex palette INDICES for a real draw and
    count how many distinct palette entries it references.

    ONE  => the world is not really skinned; entry 0 (or whichever single entry) is
            exact, the artifact is confined to genuinely animated actors, and the fix is
            to SKIN THOSE.
    MANY => the world is batched under a matrix palette — several props per vertex
            buffer, each vertex indexed to its own matrix — and the placement is PER
            VERTEX, so the fix is to bake world positions into the BLAS vertex data and
            leave the instance transform at the outer stage.

Part 68 already killed the cheap shortcut that would have avoided this question:
comparing palette entry 0 against entry 1 in the constant window separates nothing
(63.4% and 69.8% of accepted draws have a distinct second entry over two traces). That
test could not tell "this mesh blends" from "vc(base+3..) holds unrelated constants for a
shader that only ever reads entry 0". Reading the INDICES answers it directly, because an
index is only ever produced by the vertex data itself.

WHAT IT READS, AND WHY THAT IS THE GROUND TRUTH
-----------------------------------------------
The blend inputs come through a DEPENDENT fetch — `XeVfetchDep(95, r0.x, ...)` in the
translated microcode, i.e. an ordinary vertex fetch constant read with the vertex index.
Both halves are already declared in the shader's own sidecar as `indirect` attributes on
the same slot, distinguished by their `integer` flag:

    offsetDwords 6, format 6 (8_8_8_8), integer 0  ->  the three WEIGHTS, 0..1
    offsetDwords 7, format 6 (8_8_8_8), integer 1  ->  the three INDICES, 0..255

so nothing here is guessed from a name: the layout is the shader's own declaration, and
the bytes are hardware's own, out of the `.xtr` traces. A trace records the memory the GPU
touched, so a stream can arrive only in part — vertices whose bytes are absent are counted
as `missing` and never folded into a distinct-entry count, because "this draw uses one
matrix" and "we could only read one vertex" must not share a number (gotcha 25).

USAGE
    tools/rt_palette_census.py                        # every world trace
    tools/rt_palette_census.py --trace FILE --top 20  # one trace, per-draw detail
    tools/rt_palette_census.py --shader vs_b677...    # one shader only
"""
import argparse
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rt_tlas_census as C  # noqa: E402

# Formats whose element is one dword of four bytes — the only shape this title's blend
# inputs use. Anything else is NAMED rather than decoded on a guess (gotcha 5).
FMT_8_8_8_8 = 6


def dep_fetches(meta):
    """The shader's own declaration of its dependent fetches, split weights vs indices.

    `integer` is the discriminator and it is the shader's, not ours: a weight is a
    normalised 0..1 value and an index is delivered as its own integer, which is exactly
    what `num_format_all` says (and what this port's USCALED/SSCALED binding honours).
    """
    w = idx = None
    for a in meta.get('attributes') or []:
        if not a.get('indirect'):
            continue
        if a.get('format') != FMT_8_8_8_8:
            continue
        if a.get('integer'):
            idx = a
        else:
            w = a
    return w, idx


def read_indices(mem, vf, attr, vert_count):
    """Per-vertex index bytes for one draw. Returns (list of 4-byte tuples, missing).

    ONE bulk read for the whole span where the trace carries it contiguously, with a
    capped per-vertex fallback when it does not — the same two-path shape (and the same
    reason) as `stream_bounds`: `Memory.read` is a linear scan over the trace's chunks,
    so a per-vertex loop over a 4,000-vertex stream in every one of a few thousand draws
    does not finish.
    """
    stride = attr['strideDwords'] * 4
    at0 = attr['offsetDwords'] * 4
    endian = vf['endian'] & 3

    def unpack(b):
        # The guest bytes are big-endian; under 8-in-32 (endian 2) the runtime uploads
        # the stream dword-swapped and the fetch then reads byte 0 of the little-endian
        # dword as component x. Reversing the dword here reproduces exactly what the
        # shader sees, and leaving it alone is the endian-0 case.
        d = b[::-1] if endian == 2 else b
        return (d[0], d[1], d[2], d[3])

    if not vert_count:
        return [], 0
    span = (vert_count - 1) * stride + at0 + 4
    blob = mem.read(vf['address'], span)
    if blob is not None:
        return [unpack(blob[i * stride + at0:i * stride + at0 + 4])
                for i in range(vert_count)], 0
    step = max(1, vert_count // 256)
    out = []
    missing = 0
    for i in range(0, vert_count, step):
        off = i * stride + at0
        if off + 4 > vf['sizeDwords'] * 4:
            break
        b = mem.read(vf['address'] + off, 4)
        if not b or len(b) < 4:
            missing += 1
            continue
        out.append(unpack(b))
    return out, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--trace')
    ap.add_argument('--spv', default='assets/shader_spv')
    ap.add_argument('--shader', help='restrict to one vs_ hash')
    ap.add_argument('--top', type=int, default=0, help='print this many per-draw lines')
    args = ap.parse_args()

    traces = ([Path(args.trace)] if args.trace else
              sorted(p for d in C.DEFAULT_TRACE_DIRS for p in (C.ROOT / d).rglob('*.xtr')))
    if not traces:
        print('no traces found', file=sys.stderr)
        return 2
    sidecars = C.load_sidecars(args.spv)
    if not sidecars:
        print('no shader sidecars under %s — build the cache first' % args.spv,
              file=sys.stderr)
        return 2
    xform = C.load_world_xform(str(C.WORLD_XFORM))

    # Which shaders the transform census called `palette`, so this census asks its
    # question of exactly the population the runtime approximates.
    palette_shaders = {k for k, v in xform.items() if 'palette' in str(v)}
    if args.shader:
        palette_shaders &= {args.shader}
    print('%d palette shaders in %s' % (len(palette_shaders), C.WORLD_XFORM.name))

    hist = collections.Counter()      # distinct entry count -> draws
    per_shader = collections.defaultdict(collections.Counter)
    verts_by_bucket = collections.Counter()
    no_dep = collections.Counter()
    unread = 0
    printed = 0
    for tp in traces:
        draws, mem = C.walk_and_classify(tp, sidecars, with_mem=True)
        for d in draws:
            if d.get('bucket') != 'ok' or not d.get('vs'):
                continue
            if d['vs'] not in palette_shaders:
                continue
            meta = sidecars.get(d['vs']) or {}
            w, idx = dep_fetches(meta)
            if idx is None:
                no_dep[d['vs']] += 1
                continue
            # The dependent fetch reads its OWN slot's fetch constant, which the walk
            # already has in `regs` — but `walk_and_classify` does not hand `regs` back
            # per draw, so the position stream's own slot is used when they coincide
            # (they do here: the blend data is interleaved into the same buffer at a
            # different dword offset, which is what the sidecar's shared strideDwords
            # says). A shader that split them across slots would be NAMED, not guessed.
            pos = (meta.get('attributes') or [{}])[0]
            if idx['fetchSlot'] != pos.get('fetchSlot'):
                no_dep[d['vs']] += 1
                continue
            addr, size_dw, endian, stride_dw, _off = d['stream']
            vf = {'address': addr, 'sizeDwords': size_dw, 'endian': endian}
            if idx['strideDwords'] != stride_dw:
                no_dep[d['vs']] += 1
                continue
            verts = size_dw // stride_dw if stride_dw else 0
            rows, missing = read_indices(mem, vf, idx, min(verts, 4096))
            if not rows:
                unread += 1
                continue
            # Bytes 1..3 are the three influences this bank's blend uses (byte 0 is not
            # referenced by any of the three a0 assignments); count both so a shader that
            # does use byte 0 cannot hide.
            d3 = {r[1] for r in rows} | {r[2] for r in rows} | {r[3] for r in rows}
            d4 = d3 | {r[0] for r in rows}
            hist[len(d3)] += 1
            per_shader[d['vs']][len(d3)] += 1
            verts_by_bucket[len(d3)] += len(rows)
            if printed < args.top:
                printed += 1
                print('  %-22s %5d verts  distinct(bytes1-3)=%-3d %s  '
                      'distinct(all4)=%d  missing=%d'
                      % (d['vs'], len(rows), len(d3),
                         sorted(d3)[:8], len(d4), missing))

    total = sum(hist.values())
    print('\n%d palette draws read across %d traces (%d unread)' % (total, len(traces),
                                                                    unread))
    if no_dep:
        print('  NOT ASKED (no matching dependent fetch on the position slot):')
        for k, v in sorted(no_dep.items(), key=lambda kv: -kv[1]):
            print('    %-22s %d draws' % (k, v))
    if not total:
        return 1
    print('\ndistinct palette entries referenced, per draw:')
    for n in sorted(hist):
        print('    %3d entr%s  %6d draws (%5.1f%%)  %10d verts'
              % (n, 'y ' if n == 1 else 'ies', hist[n], 100.0 * hist[n] / total,
                 verts_by_bucket[n]))
    one = hist.get(1, 0)
    print('\n%.1f%% of palette draws reference exactly ONE matrix.' % (100.0 * one / total))
    print('\nVERDICT: %s' % (
        'THE WORLD IS NOT SKINNED. Almost every palette draw indexes a single matrix, '
        'so the entry-0 approximation is wrong only in WHICH entry it picks — reading '
        'the draw\'s own index and using that entry places the static world exactly, '
        'and only the genuinely multi-matrix draws need a per-vertex blend.'
        if one >= total * 0.8 else
        'THE PLACEMENT IS PER VERTEX. A large share of palette draws reference several '
        'matrices, so no single instance transform can place them: the blended world '
        'positions have to be baked into the BLAS vertex data, with the instance left '
        'at the outer stage.'))
    for vs in sorted(per_shader, key=lambda k: -sum(per_shader[k].values()))[:8]:
        c = per_shader[vs]
        print('    %-22s %6d draws  one-matrix %5.1f%%  max distinct %d'
              % (vs, sum(c.values()), 100.0 * c.get(1, 0) / sum(c.values()), max(c)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
