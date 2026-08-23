#!/usr/bin/env python3
"""Write each shader's ALU-constant usage into its .meta.json — perf item C's input.

WHY THIS EXISTS
---------------
The renderer copies the guest's WHOLE 256-float4 ALU constant window per stage per draw:
4,096 bytes each, ~28 MB/frame at the old soak load and more at the new one. The constant
memo (part 52) removes the copies whose contents did not change, but it reaches only ~61%
of pixel windows and **2.9% of vertex windows**, because the guest rewrites a world matrix
per object. `perf-state-parked.md` item C proposes copying only the registers the shader
actually READS, and gated itself on a census before any runtime code was written.

That census exists (`tools/alu_const_census.py`, Night Run 1, all 439 modules) and its
verdict was: a RANGE copy is dead (318 of 335 pixel shaders read a register >= 250 next to
their low ones — the c255 tonemap cluster — so the span is the whole window), but a GATHER
is alive and large: **median 9 of 256 registers for VS, 27 for PS**, ~430 bytes instead of
4,096. Twenty-two vertex shaders index `a0`-relatively (the bone palette) and must keep the
full copy; no pixel shader does.

A census is a distribution. The RUNTIME needs the per-shader list, and the sidecar is where
this project already keeps per-shader facts the runtime cannot re-derive cheaply
(`tfetchConsts`, `tfetchDims`, `attributes`, `interpolators`). So this writes it there, at
cache-build time, from the same substrate and the same parser the census used — it IMPORTS
`census_file` rather than re-implementing it, because a sidecar that disagreed with the
census would be a silent wrong answer in exactly the place nobody looks.

WHY THE HLSL AND NOT THE SPIR-V. DXC folds constant indices into raw buffer-device-address
offsets, indistinguishable from other address math without a full dataflow walk. The
XenosRecomp HLSL states every read literally as `vc(N)` / `pc(N)`, and every dynamic read
as `vc(BASE + a0)`.

**THIS FILE IS LOAD-BEARING FOR CORRECTNESS, not just for speed.** A register the list
omits is a register the runtime will not copy, so the shader reads whatever was left in
that arena slot — garbage, not a stale value. That is safe only because the shader provably
never reads it, which makes this parse the thing the whole item rests on. Two consequences,
both deliberate:

  * `dynamic` is recorded and the runtime falls back to the FULL copy for those shaders.
    An `a0`-relative read can land anywhere, so no list can bound it.
  * **the runtime ships a verify arm** (`CZ_VK_VERIFY_CONST_GATHER=1`) that does both
    copies and compares, plus a poison arm that drops a register from the list and must
    make the verifier fire. A census this load-bearing is not trusted on its reputation.

Usage — one shader, in the cache build (tools/build_shader_spv.sh calls this), or a whole
directory to backfill an existing cache:

    python3 tools/alu_const_sidecar.py <name.hlsl> <name.meta.json>
    python3 tools/alu_const_sidecar.py --dir <cache_dir> --hlsl <synth_dir>
"""
import argparse
import importlib.util
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
_spec = importlib.util.spec_from_file_location(
    'alu_const_census', Path(__file__).resolve().parent / 'alu_const_census.py')
_census = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_census)


def annotate(hlsl, meta_path):
    """Merge this shader's constant usage into its sidecar. Returns (n_used, dynamic)."""
    _stage, lits, dyn = _census.census_file(hlsl)
    try:
        meta = json.load(open(meta_path))
    except Exception as e:
        raise SystemExit('cannot read %s: %s' % (meta_path, e))
    meta['aluConsts'] = lits
    meta['aluDynamic'] = bool(dyn)
    # The dynamic EXPRESSIONS, not just the flag. They are what a future reader needs to
    # decide whether a tighter bound is possible (the bone palette's `vc(8+a0)` is bounded
    # by the palette size, which the blend descriptor already knows) and they cost nothing
    # to carry.
    if dyn:
        meta['aluDynamicExprs'] = dyn
    json.dump(meta, open(meta_path, 'w'), indent=1)
    open(meta_path, 'a').write('\n')
    return len(lits), bool(dyn)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('hlsl', nargs='?')
    ap.add_argument('meta', nargs='?')
    ap.add_argument('--dir', help='a built cache directory to backfill')
    ap.add_argument('--hlsl-dir', help='the synth directory holding the .hlsl files')
    args = ap.parse_args()

    if args.dir:
        if not args.hlsl_dir:
            sys.exit('--dir needs --hlsl-dir (the synth work directory)')
        n = miss = dyn = 0
        used = []
        for mp in sorted(Path(args.dir).glob('*.meta.json')):
            name = mp.name[:-len('.meta.json')]
            hp = Path(args.hlsl_dir) / (name + '.hlsl')
            if not hp.exists():
                # NAMED, not skipped: a sidecar with no HLSL beside it is a module the
                # runtime would silently full-copy forever, and the count is the only
                # thing that would ever say so.
                miss += 1
                print('no HLSL for %s' % name)
                continue
            k, d = annotate(str(hp), str(mp))
            used.append(k)
            dyn += int(d)
            n += 1
        used.sort()
        print('%d sidecars annotated, %d without HLSL, %d dynamic (full-copy fallback)'
              % (n, miss, dyn))
        if used:
            print('registers used: median %d, p90 %d, max %d of 256'
                  % (used[len(used) // 2], used[min(len(used) - 1, int(0.9 * len(used)))],
                     used[-1]))
        return 1 if miss else 0

    if not args.hlsl or not args.meta:
        sys.exit('need <name.hlsl> <name.meta.json>, or --dir with --hlsl-dir')
    k, d = annotate(args.hlsl, args.meta)
    print('%s: %d registers%s' % (os.path.basename(args.meta), k,
                                  ', DYNAMIC (full copy)' if d else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
