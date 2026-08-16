#!/bin/bash
# Part 47: the performance A/B driver, generalised over an ARBITRARY env-var arm.
#
# WHY THIS EXISTS. `tools/part46_perf_ab.sh` hard-coded its one arm (the pre-45 shader
# cache) into the loop body, so every subsequent performance question needed the script
# copied and edited. Part 47 has at least eight arms to run (docs/perf-plan-part47.md
# §7), so the arm is a parameter here: arm B is "the same binary with these environment
# variables set", which is exactly the same-binary control discipline the project already
# requires, just spelled once.
#
# Usage:
#   tools/part47_perf_ab.sh <tag> <RUNS> <VAR=VAL> [VAR=VAL ...]
# e.g.
#   tools/part47_perf_ab.sh norevalidate 3 CZ_VK_NO_TEX_REVALIDATE=1
#
# Arm A is always the plain default binary, so the SAME baseline runs interleave with
# every arm and can be pooled across questions.
#
# The alternation and the run count are not decoration: this workload's noise floor is
# 10-13% at one run a side (gotcha 229), and the arms alternate rather than running in
# blocks so thermal drift over the hour cannot be read as an arm difference.
#
# Read the output with tools/part47_perf_read.py -- MEDIANS and the pinned share, never
# means (gotchas 237/238) -- and for anything below ~10% read the CZ_VK_PROFILE phase
# shares out of the .log instead of the frame time at all (perf-plan-part47.md §6).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

ARM="${1:?usage: part47_perf_ab.sh <armtag> <runs> VAR=VAL [VAR=VAL ...]}"
RUNS="${2:?}"
shift 2
ARMENV=("$@")
[ ${#ARMENV[@]} -eq 0 ] && { echo "no arm env given"; exit 2; }

OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part47/perf}"
mkdir -p "$OUT"

# The unattended outdoor route: DebugJump to Case 0-2 (which spawns outside), then the
# title's own AI drives. Anchored to the WAITJUMP event rather than to a wall clock, so
# it reaches the same era on a fast and a slow boot (gotcha 251).
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE

echo "arm '$ARM' = ${ARMENV[*]}"
for i in $(seq 1 "$RUNS"); do
  for arm in base "$ARM"; do
    tag="${arm}_$i"
    [ -f "$OUT/$tag.stats" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $tag $(date +%H:%M:%S)"
    # `env` rather than assignment prefixes: an assignment prefix is recognised BEFORE
    # expansion, so a `${var:+NAME=value}` word is parsed as the COMMAND. See the part-46
    # script's note; six runs died in under a second that way.
    envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ"
          CZ_VK_PROFILE=30 "CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    [ "$arm" != base ] && envv+=("${ARMENV[@]}")
    ( cd "$ROOT/runtime/build" && \
      env "${envv[@]}" timeout 600 ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    if [ ! -s "$OUT/$tag.stats" ]; then
        echo "    !! $tag produced NO frame stats -- the run did not start; see $tag.log"
        continue
    fi
    # A run that never reached the outdoor era measures the safehouse, where the frame is
    # pinned to the pacing floor and every arm ties. Record the peak draw count so a tied
    # result can be told apart from a run that never got there.
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats" 2>/dev/null)
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))"
  done
done
echo "done; stats in $OUT"
