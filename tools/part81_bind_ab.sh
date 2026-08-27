#!/bin/bash
# PART 81 §1.3 — the three-arm campaign for item 0, pre-registered in `perf-plan-part81.md`.
#
# WHAT IS BEING MEASURED. Two independent changes to the driver call path, measured
# together and attributed separately:
#
#   step 1  the device command table — `vkGetDeviceProcAddr` once, then call through stored
#           pointers, so every `vkCmd*` skips the loader's trampoline. Ceiling 0.12-0.35
#           ms/frame; may land under the floor, and the plan says in advance what to do
#           then (keep it, report a NULL — it is strictly less work for identical
#           behaviour, and the reason to keep it is code, not performance).
#   step 2  the vertex bind batch — one `vkCmdBindVertexBuffers` per contiguous RUN of
#           changed bindings instead of one per binding. §1.0's census measured the runs
#           before this was written: 1.742 -> 0.468 calls a draw, 0.616 ms/frame at 9,300.
#
# THE ARMS, and each item is compared against its OWN control in the SAME block — never
# against a number from a previous session, which is a mistake this project has made and
# paid for:
#
#   both      steps 1 and 2 on — the shipping candidate
#   no-pfn    CZ_VK_NO_DEVICE_PFN=1  — the loader trampoline, attributing step 1
#   no-batch  CZ_VK_NO_BIND_BATCH=1  — one call per binding, attributing step 2
#
# ALTERNATED, three runs an arm, one binary, on the crowd route with the resolution pinned.
# Alternation matters because thermal and desktop state drift over an hour and a block of
# three consecutive runs of one arm would carry that drift as if it were the arm
# (gotchas 50/51/86 — the control is the other arm run NOW).
#
# PRE-REGISTERED KILL: combined, below 0.30 ms at the crowd load across three runs an arm,
# ship neither step. That is below this route's ±2.9% noise floor (±0.38 ms at 13 ms) and
# the honest report is a null.
#
# Read with:
#   tools/part80_trace_band.py both=<out>/both*.trace no-batch=<out>/no-batch*.trace
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part81-bind}"
REPS="${REPS:-3}"
mkdir -p "$OUT"

run () {  # run <arm> <rep> [ENV...]
    local arm="$1" rep="$2"; shift 2
    local tag="p81_${arm}${rep}"
    echo "--- $arm rep $rep"
    OUT="$OUT" "$ROOT/tools/part80_crowdroute.sh" "$tag" \
        "CZ_VK_FRAME_TRACE=$OUT/${arm}${rep}.trace" "$@"
    # A run that missed the route is renamed .rejected by the route script — but its TRACE
    # is not, and a stale trace would be globbed straight back into the comparison. Part 78
    # lost an A/B to exactly that (`part76-kickoff.md` §5), so the trace goes with it.
    if [ $? -ne 0 ]; then
        mv -f "$OUT/${arm}${rep}.trace" "$OUT/${arm}${rep}.trace.rejected" 2>/dev/null
        echo "    (trace renamed .rejected too)"
    fi
}

for r in $(seq 1 "$REPS"); do
    run both     "$r"
    run no-pfn   "$r" CZ_VK_NO_DEVICE_PFN=1
    run no-batch "$r" CZ_VK_NO_BIND_BATCH=1
done

# THE PROFILER PAIR — the DIRECT evidence, and it is a far tighter statistic than frame
# time. Part 78's barrier classes reproduced to 0.001 ms/frame where the frame time needed
# three runs a side. `record` ns/draw is what both steps act on; the frame time is the
# consequence. One run an arm is enough here because the statistic is a per-draw mean over
# millions of draws rather than a distribution over frames.
#
# READ ONLY THE SUB-SCOPES. The profiler costs ~807 ns a draw at this load (18.6 scopes at
# ~21.7 ns) and therefore distorts every phase SHARE — `part81-kickoff.md` §0b point three.
if [ "${PROFILE:-1}" = "1" ]; then
    echo
    echo "=== the profiler pair: record ns/draw, the direct evidence"
    for arm in both no-pfn no-batch; do
        extra=""
        [ "$arm" = "no-pfn" ] && extra="CZ_VK_NO_DEVICE_PFN=1"
        [ "$arm" = "no-batch" ] && extra="CZ_VK_NO_BIND_BATCH=1"
        echo "--- profile $arm"
        OUT="$OUT" "$ROOT/tools/part80_crowdroute.sh" "p81prof_$arm" \
            CZ_VK_PROFILE=10 $extra
    done
fi

echo
echo "=== traces in $OUT"
ls -la "$OUT"/*.trace 2>/dev/null
echo
echo "read with:"
echo "  tools/part80_trace_band.py both=$OUT/both*.trace no-batch=$OUT/no-batch*.trace"
echo "  tools/part80_trace_band.py both=$OUT/both*.trace no-pfn=$OUT/no-pfn*.trace"
