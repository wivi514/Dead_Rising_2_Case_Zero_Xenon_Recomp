#!/usr/bin/env python3
"""The ALU constant usage census — the gate on perf item C (copy only what is read).

WHY THIS EXISTS. `docs/perf-state-parked.md` item C proposes copying only the ALU
constants a shader actually reads instead of the full 256-float4 window per stage per
draw (~28 MB/frame at soak load, the half the memo does not reach). Its own text
gates it: "Measure the distribution of max-constant-used across the modules before
writing any runtime code — if the median shader reads 200 of 256, the item is
worthless." The `.meta.json` sidecars carry no ALU usage, so the census did not exist.

WHERE THE NUMBERS COME FROM. Not the SPIR-V — DXC folds constant indices into raw
buffer-device-address offsets, which cannot be told apart from other address math
without a full dataflow walk. The XenosRecomp-generated HLSL is the honest substrate:
every constant read is a literal `vc(N)` / `pc(N)` macro call, and every dynamic
(`a0`-relative) read is textually `vc(BASE + a0)`. Regenerate the HLSL from the ucode
dumps with the first two steps of tools/build_shader_spv.sh (synth + XenosRecomp, no
DXC needed):

    python3 tools/synth_shader_container.py <ucode_dir> <work>
    for x in <work>/*.xshd; do XenosRecomp "$x" "${x%.xshd}.hlsl" shader_common.h; done
    python3 tools/alu_const_census.py <work>

TWO SIZES ARE REPORTED PER MODULE, because they price two different designs:
  * n_used — distinct literal registers read: prices a GATHER copy (a per-shader list).
  * span   — (max - min + 1): prices a RANGE copy (one memcpy of the used window).
A module with dynamic indexing is priced as FULL COPY for its stage (the `a0` bound is
run-time data — bone counts — and the HLSL cannot bound it), and is counted separately.

WHAT THIS CANNOT SAY. The census weights every module equally; the frame weights them
by draw count. A 90% median saving here is an upper bound on the copy-bytes saving,
not a frame-time prediction — the run-time counter for that is item C's to build.
"""
import sys, re, json, glob, os, statistics

CALL = re.compile(r"\b(vc|pc)\(([^)]+)\)")

def census_file(path):
    stage = "vs" if os.path.basename(path).startswith("vs_") else "ps"
    lits, dyn = set(), []
    with open(path, "r", errors="replace") as f:
        for line in f:
            if line.lstrip().startswith("#define"):
                continue  # the macro's own definition, not a use
            for m in CALL.finditer(line):
                arg = m.group(2).strip()
                if re.fullmatch(r"\d+", arg):
                    lits.add(int(arg))
                elif arg != "INDEX":
                    dyn.append(arg)
    return stage, sorted(lits), sorted(set(dyn))

def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))] if xs else 0

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    files = sorted(glob.glob(os.path.join(d, "*.hlsl")))
    if not files:
        print(f"no .hlsl in {d} — regenerate per the docstring", file=sys.stderr)
        return 1
    rows = []
    for p in files:
        stage, lits, dyn = census_file(p)
        n = len(lits)
        span = (lits[-1] - lits[0] + 1) if lits else 0
        rows.append(dict(name=os.path.basename(p)[:-5], stage=stage, n_used=n,
                         lo=lits[0] if lits else 0, hi=lits[-1] if lits else 0,
                         span=span, dynamic=dyn))
    for stage in ("vs", "ps"):
        S = [r for r in rows if r["stage"] == stage]
        dyn = [r for r in S if r["dynamic"]]
        static = [r for r in S if not r["dynamic"]]
        print(f"\n== {stage.upper()}: {len(S)} modules, {len(dyn)} with a0-relative "
              f"(dynamic) indexing -> full copy for those")
        bases = {}
        for r in dyn:
            for b in r["dynamic"]:
                bases[b] = bases.get(b, 0) + 1
        if bases:
            print("   dynamic forms: " +
                  ", ".join(f"{k} x{v}" for k, v in sorted(bases.items())))
        if not static:
            continue
        for key, label in (("n_used", "distinct registers read (gather-copy size)"),
                           ("span", "min..max span (range-copy size)")):
            xs = [r[key] for r in static]
            print(f"   {label}, static modules only:")
            print(f"     median {statistics.median(xs):.0f} of 256 | "
                  f"p25 {pct(xs,25)}  p75 {pct(xs,75)}  p90 {pct(xs,90)}  max {max(xs)}")
        hi255 = sum(1 for r in static if r["hi"] >= 250)
        print(f"   static modules whose top register is >= 250: {hi255} of "
              f"{len(static)} (a range copy from 0 saves nothing for these; "
              f"a gather or lo..hi copy still can)")
    out = os.path.join(d, "alu_const_census.json")
    with open(out, "w") as f:
        json.dump(rows, f, indent=1)
    print(f"\nper-module detail: {out}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
