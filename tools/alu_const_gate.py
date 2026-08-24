#!/usr/bin/env python3
"""Gate on the ALU-constant lists the runtime's gather trusts (perf item C, part 72).

WHY THIS EXISTS
---------------
`CZ_VK_CONST_GATHER=1` (ON; it is OFF by default since part 74) means the renderer copies only the
constant registers each shader's sidecar says it reads. **A register missing from a list is
never copied**, so the shader reads whatever the bump arena left in that slot — garbage,
not a stale value, and the symptom is a wrong constant in one shader with everything else
correct. That is the hardest defect class in this renderer to see.

So the list is load-bearing for CORRECTNESS, and the runtime's own verify arm
(`CZ_VK_VERIFY_CONST_GATHER=1`) can only check that the gather COPIED what the list names
— it cannot check that the list names everything the shader reads. Nothing at run time can:
the missing register is exactly the one nobody looks at.

This checks the list against the shader's own translated HLSL, which is the substrate the
list was built from, and against the cache the runtime will load. It is cheap, it is
offline, and it fails loudly:

  * every `.spv` in the cache has a sidecar carrying `aluConsts` and `aluDynamic`
    (a sidecar predating part 72 has neither, and the runtime then full-copies — safe, but
    it means the item is silently not running and only a count would ever say so);
  * every literal `vc(N)`/`pc(N)` in the HLSL appears in the list — re-parsed here rather
    than trusted, so a bug in the writer cannot hide behind the writer;
  * every shader with a dynamic (`a0`-relative) read is marked dynamic, because those
    cannot be bounded by any list;
  * no list names a register >= 256.

    python3 tools/alu_const_gate.py                       # the stock cache
    python3 tools/alu_const_gate.py --dir assets/shader_spv_clip_a2m --hlsl-dir <synth>
"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
_spec = importlib.util.spec_from_file_location(
    'alu_const_census', Path(__file__).resolve().parent / 'alu_const_census.py')
_census = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_census)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default='assets/shader_spv')
    ap.add_argument('--hlsl-dir', help='re-parse the HLSL and cross-check every list')
    args = ap.parse_args()

    d = Path(args.dir)
    spvs = sorted(d.glob('*.spv'))
    if not spvs:
        print('no .spv in %s' % d, file=sys.stderr)
        return 2

    bad = []
    missing_meta = missing_key = dynamic = zero = 0
    used = []
    for sp in spvs:
        name = sp.name[:-4]
        mp = d / (name + '.meta.json')
        if not mp.exists():
            bad.append('%s: no sidecar' % name)
            missing_meta += 1
            continue
        meta = json.load(open(mp))
        if 'aluConsts' not in meta or 'aluDynamic' not in meta:
            # NOT a hard failure on its own — the runtime full-copies — but it means the
            # item is not running for this shader, and that must be visible.
            missing_key += 1
            continue
        lst = meta['aluConsts']
        if any((not isinstance(r, int)) or r < 0 or r >= 256 for r in lst):
            bad.append('%s: list names a register outside 0..255' % name)
        if meta['aluDynamic']:
            dynamic += 1
        else:
            used.append(len(lst))
            if not lst:
                zero += 1
        if args.hlsl_dir:
            hp = Path(args.hlsl_dir) / (name + '.hlsl')
            if not hp.exists():
                bad.append('%s: no HLSL to cross-check against' % name)
                continue
            _stage, lits, dyn = _census.census_file(str(hp))
            miss = sorted(set(lits) - set(lst))
            if miss:
                bad.append('%s: HLSL reads %s but the list omits them'
                           % (name, miss[:8]))
            if bool(dyn) != bool(meta['aluDynamic']):
                bad.append('%s: dynamic flag is %s, HLSL says %s'
                           % (name, meta['aluDynamic'], bool(dyn)))

    print('%s: %d modules, %d dynamic (full-copy fallback), %d without the keys'
          % (d.name, len(spvs), dynamic, missing_key))
    # A shader reading ZERO constants is its own category and the item's biggest single
    # win — the correct copy is nothing at all. It is called out because the first version
    # of the runtime treated "empty list" as "unknown list" and full-copied them, which is
    # the feature not applying to exactly the shaders it helps most, silently.
    if zero:
        print('  %d shader(s) read NO ALU constants — those copy ZERO bytes' % zero)
    if used:
        used.sort()
        print('  registers per gathering shader: median %d, p90 %d, MAX %d of 256'
              % (used[len(used) // 2],
                 used[min(len(used) - 1, int(0.9 * len(used)))], used[-1]))
        print('  worst-case gather %d bytes against 4096 (%.1f%% of the full window)'
              % (used[-1] * 16, 100.0 * used[-1] / 256.0))
    if not args.hlsl_dir:
        print('  (no --hlsl-dir: the lists were NOT cross-checked against the shaders —'
              ' this run only checked that they exist and are in range)')
    for b in bad[:20]:
        print('  ** %s' % b)
    if bad:
        print('%d defect(s)' % len(bad), file=sys.stderr)
        return 1
    if missing_meta:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
