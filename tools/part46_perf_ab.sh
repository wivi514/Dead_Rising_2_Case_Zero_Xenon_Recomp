#!/bin/bash
# Part 46 item 2: the operator's reported PERFORMANCE REGRESSION, as a one-variable
# same-binary A/B over the shader cache.
#
# WHY THIS EXISTS. The operator reported "the performance degraded with all the fix you
# did in the last few days" and nobody had measured it. The three suspects, in the order
# they were introduced, are part 41's per-fetch samplers, part 44/45's mip uploads and
# part 45's interpolant-liveness fix. Only the last one has a preserved control arm --
# `assets/shader_spv_pre45` is the whole pre-fix cache -- so it is the only suspect that
# can be turned into a ONE-VARIABLE A/B without a rebuild, and it is also the biggest
# a-priori suspect because it added interpolants to 217 of 333 pixel shaders.
#
# The alternation and the run count are not decoration. This workload's noise floor is
# 10-13% at one run a side (gotcha 229), so a single pair is a coin flip; and the arms
# alternate rather than running in blocks so that thermal drift over the hour cannot be
# read as an arm difference.
#
# Read the OUTPUT with medians and the share of frames pinned to a 16 ms multiple, never
# with means: a mean on this title measures its own two-vblank pacing floor and not the
# change (gotchas 237/238). tools/frame_perf_bins.py reports means, so it is the wrong
# reader on its own -- see docs/measurement.md.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HOME/DR2CZ-troubleshooting/part46/perf}"
RUNS="${2:-3}"
mkdir -p "$OUT"

# The unattended outdoor route: DebugJump to Case 0-2 (which spawns outside), then the
# title's own AI drives. Anchored to the WAITJUMP event rather than to a wall clock, so
# it reaches the same era on a fast and a slow boot (gotcha 251).
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE

for i in $(seq 1 "$RUNS"); do
  for arm in fixed pre45; do
    tag="${arm}_$i"
    [ -f "$OUT/$tag.stats" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $tag $(date +%H:%M:%S)"
    # `env` rather than assignment prefixes: an assignment prefix is recognised BEFORE
    # expansion, so a `${var:+NAME=value}` word is parsed as the COMMAND and every
    # assignment after it becomes an argument. The first version of this script did that
    # and all six runs exited in under a second with "CZ_FAKE_START_MS=8000: command not
    # found" -- which the peak-draws line below is what caught.
    envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ"
          CZ_VK_PROFILE=30 "CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    [ "$arm" = pre45 ] && envv+=("CZ_SHADER_SPV=$ROOT/assets/shader_spv_pre45")
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
