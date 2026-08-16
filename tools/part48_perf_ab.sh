#!/bin/bash
# Part 48: the performance A/B driver — `part47_perf_ab.sh` plus a PINNED BINARY.
#
# WHY THE BINARY IS A PARAMETER, and it is not a convenience. Part 47's driver ran
# `./cz_runtime`, which is the file ninja overwrites. A full A/B on this workload is
# three runs an arm at up to ten minutes each — the better part of an hour (gotcha 229) —
# and during that hour the next item cannot be built without silently changing the
# binary under the arms that have not run yet. Alternating the arms, which is what makes
# thermal drift unreadable as an arm difference, makes that worse rather than better: the
# swap would land BETWEEN two arms of the same comparison.
#
# So: snapshot the binary once, run every arm against the snapshot, and leave
# `./cz_runtime` free to be rebuilt. The snapshot lives in `runtime/build/` so the
# working directory — and therefore every asset path the runtime resolves — is identical
# to an ordinary run.
#
# Usage:
#   tools/part48_perf_ab.sh <tag> <RUNS> <VAR=VAL> [VAR=VAL ...]
# with the binary chosen by BIN (default: a snapshot named after the tag, made now):
#   BIN=cz_runtime_1b tools/part48_perf_ab.sh atomiccounters 3 CZ_PM4_ATOMIC_COUNTERS=1
#
# Read the frame times with tools/part47_perf_read.py (medians, a matched draw band, and
# the 16 ms-pinned share — all three read wrong by default). Read a change to the PM4
# WALK with tools/part48_walk_read.py instead, which quotes ns per packet: the arms do
# not submit the same command stream, so a matched draw band does not match a PM4
# workload and `outside` in milliseconds is not comparable across them
# (docs/perf-plan-part48.md §2).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

ARM="${1:?usage: part48_perf_ab.sh <armtag> <runs> VAR=VAL [VAR=VAL ...]}"
RUNS="${2:?}"
shift 2
ARMENV=("$@")
[ ${#ARMENV[@]} -eq 0 ] && { echo "no arm env given"; exit 2; }

OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part48/$ARM}"
mkdir -p "$OUT"

# Snapshot unless the caller named one. Copied, not hard-linked: ninja unlinks and
# recreates its output, but a hard link would survive that and quietly keep the OLD
# inode alive under a name that reads like the current build.
BIN="${BIN:-cz_runtime_$ARM}"
if [ ! -x "$ROOT/runtime/build/$BIN" ]; then
    cp "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($(cd "$ROOT" && git rev-parse --short HEAD))"
fi
echo "arm '$ARM' = ${ARMENV[*]}   binary=$BIN"

# The unattended outdoor route: DebugJump to Case 0-2, then the title's own AI drives.
# Anchored to the WAITJUMP event rather than to a wall clock (gotcha 251).
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE

for i in $(seq 1 "$RUNS"); do
  for arm in base "$ARM"; do
    tag="${arm}_$i"
    [ -f "$OUT/$tag.stats" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $tag $(date +%H:%M:%S)"
    # `env` rather than assignment prefixes: an assignment prefix is recognised BEFORE
    # expansion, so a `${var:+NAME=value}` word is parsed as the COMMAND (part 46).
    envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ"
          CZ_VK_PROFILE=30 "CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    [ "$arm" != base ] && envv+=("${ARMENV[@]}")
    ( cd "$ROOT/runtime/build" && \
      env "${envv[@]}" timeout 600 "./$BIN" > "$OUT/$tag.log" 2>&1 )
    if [ ! -s "$OUT/$tag.stats" ]; then
        echo "    !! $tag produced NO frame stats -- the run did not start; see $tag.log"
        continue
    fi
    # A run that never reached the outdoor era measures the safehouse, where the frame is
    # pinned to the pacing floor and every arm ties. Record the peak so a tied result can
    # be told apart from a run that never got there.
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats" 2>/dev/null)
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))"
  done
done
echo "done; stats in $OUT"
