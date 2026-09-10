#!/usr/bin/env python3
"""Does the phase profiler's table agree with `perf`? Exit 1 when a phase's NAME lies.

WHY THIS EXISTS, and it is the third time, not the first.

`CZ_VK_PROFILE` prints a table of named phases that reads like a decomposition of the
frame. It is not one. A `ProfScope` measures a REGION OF CODE, and the regions do not
cover the functions they are named after. The worked example is one function:

  * **part 22** closed the stream cache because `streams` read 0.0%;
  * **part 55** re-opened it from a `perf` SYMBOL profile and found `UploadStream` at
    13.1% of the pump thread;
  * **gotcha 343** was written about exactly this;
  * **part 109** read `streams 0.3%` and wrote "item 2 is almost certainly dead on price
    before a line is written" — four hours before its own symbol profile said the symbol
    was 9.4% of the pump, the third-largest thing on the critical path. It retracted the
    paragraph in place the same night.

Three parts, one function, one instrument. Reading the code again does not fix that; a
CHECK that fails does. So this reads both instruments off the same run and prints them
side by side against a hand-maintained phase -> symbol map, and exits 1 on any phase
that disagrees with the symbols implementing the subsystem it is named after by more
than a factor of `--factor` (default 2).

WHAT THE MAP MEANS, because this is the whole judgement in the file. It maps a phase to
the symbols that implement THE SUBSYSTEM THE PHASE IS NAMED AFTER — not to the symbols
that happen to run inside the scope. That is deliberate: the failure mode is a reader
seeing `streams 0.3%` and concluding the stream path is cheap, and a checker that
compared the scope to itself could never catch it. A phase whose name names a subsystem
is a claim about that subsystem, and this is the test of the claim.

THE TWO DENOMINATORS, which have to be reconciled or the comparison is noise:

  * a phase's percentage is of the WINDOW'S WALL TIME (see the `pct` lambda in
    vk_renderer.cpp);
  * a symbol's percentage here is of ITS OWN THREAD'S CPU (part53_symbols.py exists
    because `perf report --tid=` does not renormalise and every hand reading of it had
    to).

The profiler prints `pump thread: N% on CPU` every window, which is exactly the ratio
between them, so symbol shares are multiplied by it and both columns end up as a share
of wall. On this workload N is 96-98% and the correction is small — but it is printed,
because an unstated denominator is how a factor-of-thirty error survives three parts.

SYMBOLS AND STALE BINARIES. `perf` resolves a DSO by build-id and REFUSES a file at the
recorded path whose build-id has moved on — which is every archived capture in this
project, because the route script re-links `cz_runtime_crowd` on the next build. The
symptom is 89% `[unknown]` on the pump thread and it looks exactly like a profile with
no symbols at all. Pass `--binary` with the matching executable and this builds a symfs
(a symlink farm, so libc and the driver still resolve) and points `perf script` at it.
`perf buildid-list -i <perf.data>` prints the build-id the capture wants;
`readelf -n <bin> | grep -i 'build id'` prints what a candidate has.

Usage:
    tools/phase_vs_perf.py <perf.data> <profiled log> [--binary BIN] [--factor 2]
    tools/phase_vs_perf.py --self-test        # the positive control, on part 109's archive

THE POSITIVE CONTROL IS NOT OPTIONAL (gotcha 30). A checker that has never failed has
not been shown capable of failing, and this one's whole job is to fail on a case that
already happened. `--self-test` points it at part 109's archived artifacts and REQUIRES
that `streams` be flagged; if it is not, the checker is broken and says so.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# --- the map, and every row is a judgement about what a phase's NAME claims -----------
#
# Each entry: phase name in the log -> (regexes matching the symbols that implement the
# subsystem it is named after, why this row exists).
#
# `record` and `other` are deliberately NOT here. They name a region of DoDraw rather
# than a subsystem, `DoDraw` is one symbol covering both, and no honest split of that
# symbol between them exists — pretending one does would put a fabricated row next to
# five measured ones.
PHASE_SYMBOLS = {
    "streams": (
        [r"UploadStream", r"PersistFind"],
        True,
        "the scope wraps only the CopySwapped; the flat-cache lookup, the content "
        "guard and the cross-frame store are the subsystem and are charged to `record`",
    ),
    "textures": (
        [r"UploadTextureUncached", r"UploadTexture\b", r"TexFind", r"DecodeTextureFetch"],
        True,
        "the untile/upload is scoped; the cache lookup (`TexFind`) and the fetch decode "
        "are not — they are charged to `other`/`otherFetch`",
    ),
    # ADVISORY, and the reason is a limit of THIS TOOL rather than of the profiler.
    # `constVsPatch` is not a call: `SceneXformForm` and the two patch helpers are inline
    # code in DoDraw, so at -O2 their cycles land in the `DoDraw` SYMBOL and no symbol
    # regex can claim them back. The symbol column here is therefore a LOWER BOUND for
    # this row, and a phase reading HIGHER than it is the expected consequence, not a
    # finding. Splitting DoDraw by source line is what prices the inlined half
    # (tools/part55_srcline.py); this row exists to show the gap, not to gate on it.
    "constants": (
        [r"CopyConstWindow", r"__memset_avx2"],
        False,
        "the gathers and the shared-block zero are real symbols; the fov/wide projection "
        "PATCH is inlined into DoDraw, so the symbol column under-counts this row",
    ),
}

# The PM4 walk is not a ProfScope at all — it is `walk` minus the phases, printed on the
# `pump` line as `[pm4 N]`. Included because it is 22% of the pump and a table that
# accounts for it only by subtraction is exactly the shape this file distrusts.
PM4_SYMBOLS = [r"WriteRegisterRun", r"ExecutePacket", r"ExecuteLinear"]

# How the pump thread is identified. Not "the second busiest thread" — that was a
# guess that happened to hold, and on this runtime the busiest thread is the guest's.
# These three symbols exist nowhere else in the process.
PUMP_MARKERS = [r"ExecutePacket", r"WriteRegisterRun", r"DoDraw"]

SCRIPT_LINE = re.compile(
    r"^\s*(\S+)\s+(\d+)\s+[\d.]+:\s+(\d+)\s+\S+:\s+[0-9a-f]+\s+(.*?)\s+\((.*)\)\s*$")

# The phase table. One regex per column read, because the line is long and a single
# monster pattern that stops matching after a format change fails SILENTLY — which is
# the defect class this whole file is about.
FPS_LINE = re.compile(
    r"\[vkprof\]\s+[\d.]+ fps \(([\d.]+) ms/frame, (\d+) draws/frame\)")
DRAW_BLOCK = re.compile(
    r"draw ([\d.]+)% \[constants ([\d.]+) \(vs ([\d.]+) \[copy ([\d.]+) patch ([\d.]+)\] "
    r"ps ([\d.]+) shared ([\d.]+)\) streams ([\d.]+) textures ([\d.]+) record ([\d.]+) "
    r"other ([\d.]+)\]")
PM4_LINE = re.compile(r"\[vkprof\] pump \d+ ticks .*?\[pm4 ([\d.-]+)\]")
CPU_LINE = re.compile(r"\[vkprof\]\s+pump thread: ([\d.]+)% on CPU")
# Part 110's own coverage line, if the log came from a build that has it.
COV_LINE = re.compile(r"\[vkprof\]\s+COVERAGE: phases ([\d.]+)%")


def build_symfs(binary, recorded_path):
    """A symlink farm so `perf script` finds OUR binary and the system's libraries.

    --symfs prefixes every DSO path, so a symfs holding only the executable would lose
    libc and the driver — and `__memset_avx2` and `__memcmp_avx2` are real rows in this
    table. Symlinking the system roots in keeps them.
    """
    d = tempfile.mkdtemp(prefix="phase_vs_perf.symfs.")
    for top in ("usr", "lib64", "lib", "bin", "opt", "etc"):
        p = os.path.join("/", top)
        if os.path.exists(p):
            os.symlink(p, os.path.join(d, top))
    dst = os.path.join(d, recorded_path.lstrip("/"))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    os.symlink(os.path.abspath(binary), dst)
    return d


def recorded_main_dso(perf_data):
    """The path `perf` recorded for the game's own executable, from the build-id list."""
    out = subprocess.run(["perf", "buildid-list", "-i", perf_data],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and "cz_runtime" in parts[1]:
            return parts[1].strip()
    return None


def perf_symbols(perf_data, binary=None):
    """{tid: Counter(function -> cycles)} with symbols resolved, plus the pump's tid."""
    argv = ["perf", "script", "-i", perf_data]
    tmp = None
    if binary:
        rec = recorded_main_dso(perf_data)
        if not rec:
            sys.exit("!! no cz_runtime DSO in the build-id list; wrong perf.data?")
        tmp = build_symfs(binary, rec)
        argv += ["--symfs=" + tmp]
    try:
        out = subprocess.run(argv, capture_output=True, text=True).stdout
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    per = collections.defaultdict(collections.Counter)
    for line in out.splitlines():
        m = SCRIPT_LINE.match(line)
        if not m:
            continue
        sym = re.sub(r"\+0x[0-9a-f]+$", "", m.group(4))
        per[int(m.group(2))][sym] += int(m.group(3))
    if not per:
        sys.exit("!! no samples parsed out of perf script")
    # The pump is the thread carrying the command processor, not the busiest one.
    best, bestscore = None, 0
    for tid, c in per.items():
        score = sum(v for s, v in c.items()
                    if any(re.search(p, s) for p in PUMP_MARKERS))
        if score > bestscore:
            best, bestscore = tid, score
    if best is None:
        sys.exit("!! no thread carries ExecutePacket/WriteRegisterRun/DoDraw — the "
                 "binary's symbols did not resolve. Pass --binary with the executable "
                 "whose build-id matches (perf buildid-list -i <perf.data>).")
    return per, best


def share(counter, patterns):
    """That thread's share of cycles in any symbol matching any pattern, as a percent."""
    total = sum(counter.values())
    hit = sum(v for s, v in counter.items()
              if any(re.search(p, s) for p in patterns))
    return 100.0 * hit / total if total else 0.0


def read_log(path, floor):
    """Mean phase shares over the log's crowd windows, and the pump's CPU duty.

    A window is kept only at >= `floor` draws: a run-wide mean over every window would
    average the menus with the crowd, and this project has made that mistake often
    enough to have a tool named after it (tools/part109_band.py).
    """
    rows, pm4, duty, cov = [], [], [], []
    cur = None
    for line in open(path, errors="replace"):
        m = FPS_LINE.search(line)
        if m:
            d = DRAW_BLOCK.search(line)
            cur = None
            if d and int(m.group(2)) >= floor:
                cur = {
                    "ms": float(m.group(1)), "draws": int(m.group(2)),
                    "draw": float(d.group(1)), "constants": float(d.group(2)),
                    "streams": float(d.group(8)), "textures": float(d.group(9)),
                    "record": float(d.group(10)), "other": float(d.group(11)),
                }
                rows.append(cur)
            continue
        if cur is None:
            continue
        m = PM4_LINE.search(line)
        if m:
            pm4.append(float(m.group(1)))
        m = CPU_LINE.search(line)
        if m:
            duty.append(float(m.group(1)))
        m = COV_LINE.search(line)
        if m:
            cov.append(float(m.group(1)))
    if not rows:
        sys.exit(f"!! no CZ_VK_PROFILE windows at >= {floor} draws in {path}")
    mean = lambda xs: sum(xs) / len(xs) if xs else 0.0
    out = {k: mean([r[k] for r in rows]) for k in rows[0]}
    out["windows"] = len(rows)
    out["pm4"] = mean(pm4)
    out["duty"] = mean(duty) if duty else 100.0
    out["coverage"] = mean(cov) if cov else None
    return out


def run(perf_data, log, binary, factor, floor):
    per, pump = perf_symbols(perf_data, binary)
    counter = per[pump]
    total = sum(sum(c.values()) for c in per.values())
    phases = read_log(log, floor)
    duty = phases["duty"] / 100.0

    print(f"perf : {perf_data}")
    print(f"log  : {log}   ({phases['windows']} windows >= {floor} draws, "
          f"{phases['draws']:.0f} draws/frame, {phases['ms']:.2f} ms/frame)")
    print(f"pump : tid {pump}, {100.0 * sum(counter.values()) / total:.1f}% of the "
          f"process's cycles, {phases['duty']:.1f}% on CPU")
    print(f"       symbol shares below are of the PUMP THREAD's cpu, rescaled by that "
          f"duty so both columns are shares of WALL")
    print()
    print(f"  {'phase':<12} {'table':>8} {'symbols':>8} {'ratio':>7}  verdict")
    print(f"  {'-'*12} {'-'*8} {'-'*8} {'-'*7}  {'-'*7}")

    bad, notes = [], []
    rows = list(PHASE_SYMBOLS.items()) + [
        ("pm4 (walk)", (PM4_SYMBOLS, True,
                        "not a ProfScope at all: the `pump` line prints it as `walk` "
                        "minus the phases, so an error in any phase lands here"))]
    for name, (pats, strict, why) in rows:
        table = phases["pm4"] if name.startswith("pm4") else phases[name]
        sym = share(counter, pats) * duty
        if table <= 0.0005 and sym <= 0.0005:
            ratio, verdict = 1.0, "ok"
        elif table <= 0.0005:
            ratio, verdict = float("inf"), "LIES"
        else:
            ratio = sym / table
            verdict = "LIES" if (ratio > factor or ratio < 1.0 / factor) else "ok"
        if verdict == "LIES" and not strict:
            verdict = "note"
        if verdict == "LIES":
            bad.append((name, table, sym, ratio, why))
        elif verdict == "note":
            notes.append((name, table, sym, ratio, why))
        r = "  inf" if ratio == float("inf") else f"{ratio:6.2f}x"
        print(f"  {name:<12} {table:7.2f}% {sym:7.2f}% {r}  {verdict}")

    if phases["coverage"] is not None:
        print(f"\n  the build's own COVERAGE line reads {phases['coverage']:.1f}% "
              f"— compare it with the sum of the table above")

    if notes:
        print()
        for name, table, sym, ratio, why in notes:
            r = "inf" if ratio == float("inf") else f"{ratio:.1f}x"
            print(f"  note: `{name}` {table:.2f}% vs {sym:.2f}% ({r}) — {why}. "
                  f"ADVISORY: this row does not gate.")

    print()
    if bad:
        print(f"!! {len(bad)} phase(s) disagree with their symbols by more than "
              f"{factor}x. A phase names a SCOPE, not a subsystem (gotcha 343):")
        for name, table, sym, ratio, why in bad:
            r = "inf" if ratio == float("inf") else f"{ratio:.1f}x"
            print(f"   - `{name}` reads {table:.2f}% and its symbols are {sym:.2f}% "
                  f"({r}) — {why}")
        return 1
    print("every mapped phase agrees with its symbols to within "
          f"{factor}x.")
    return 0


def self_test():
    """The positive control: part 109's archived crowd run must flag `streams`.

    The two artifacts come from different runs of the same era (there is no capture in
    the archive carrying both `perf` and `CZ_VK_PROFILE`, which is itself a finding and
    is why the probe now takes them together). That mismatch cannot manufacture the
    failure it is testing for: the disagreement is a factor of THIRTY and no difference
    in resolution or load moves a phase by that much.
    """
    tb = os.path.expanduser("~/DR2CZ-troubleshooting")
    perf_data = f"{tb}/part109/unkres.perf.data"
    binary = f"{tb}/part109/unkres.bin"
    logs = sorted(f for f in os.listdir(f"{tb}/part80-crowd")
                  if "prof1080" in f and f.endswith(".log"))
    if not (os.path.exists(perf_data) and os.path.exists(binary) and logs):
        sys.exit("!! part 109's archive is not where this expects it; self-test cannot "
                 f"run (wanted {perf_data}, {binary}, {tb}/part80-crowd/*prof1080*.log)")
    log = f"{tb}/part80-crowd/{logs[-1]}"
    print("=== POSITIVE CONTROL (gotcha 30): part 109's archived crowd artifacts.")
    print("=== `streams` MUST be flagged. If it is not, this checker is broken.\n")
    # 2,000-draw floor: the 1080p profiled runs are a different, lighter era than the
    # 3440x1440 perf capture, and holding out for 8,000 there would select no windows.
    rc = run(perf_data, log, binary, 2.0, 2000)
    print()
    if rc == 1:
        print("SELF-TEST PASSED: the checker fails on the case that already happened.")
        return 0
    print("SELF-TEST FAILED: the known lie was not detected. Do not trust a pass from "
          "this tool until it does.")
    return 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("perf_data", nargs="?")
    ap.add_argument("log", nargs="?")
    ap.add_argument("--binary", help="the executable whose build-id matches the capture")
    ap.add_argument("--factor", type=float, default=2.0)
    ap.add_argument("--floor", type=int, default=8000, help="crowd draw floor")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(self_test())
    if not (a.perf_data and a.log):
        ap.error("need <perf.data> and <log>, or --self-test")
    sys.exit(run(a.perf_data, a.log, a.binary, a.factor, a.floor))


if __name__ == "__main__":
    main()
