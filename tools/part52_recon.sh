#!/bin/bash
# Part 52 recon: build the frame budget at SYMBOL level, with the instruments taken off.
#
# WHY. Parts 50 and 51 both repriced the plan's top item by measuring it before building
# anything, and part 51 found the reason it keeps happening: every budget this project has
# written is a PHASE budget, and a phase is a scope somebody already suspected. `perf` over
# the process names the top cost in every thread in symbols — the guest's included — and
# on our own pump it disagreed with the phase table (`DoDraw` 9.84% of the thread while a
# CONTENT GUARD was 16.79%, and a phase called `streams` reading 0.03 ms while `GuardFold`
# was the second-biggest symbol in the process). Gotcha 340.
#
# It also found the trap in reading such a profile: `perf` charges INLINED code to its
# container, and the biggest symbol on the pump (`DoSwapImpl`, 19.4%) has the frame-stats
# instrument inlined into it. So the recon is run as an A/B on the INSTRUMENTS themselves:
#
#   stats     CZ_VK_FRAME_STATS on   — what every profile this project has ever taken saw
#   nostats   nothing on             — what the player's frame actually contains
#
# The difference between those two symbol tables IS the instrument's footprint, measured
# rather than reasoned, and the `nostats` table is the only honest input to a budget.
#
# THREE THINGS ARE COLLECTED PER RUN, and the second is the one part 51 could not do:
#   1. a flat cycles profile (which instructions run);
#   2. a DWARF call-graph profile over a shorter window (WHO CALLS the hot leaf) — this is
#      what turns "GuardFold is 17%" into "the texture guard is N% and the stream guard is
#      M%", which are different items with different fixes;
#   3. per-thread CPU from /proc, so the symbol shares can be converted into milliseconds
#      of a frame rather than left as percentages of an unknown denominator.
#
# GATING ON AN EVENT, NOT A CLOCK. The `nostats` arm cannot poll a frame-stats file for the
# draw count, so both arms gate on a LOG event plus a fixed roam, and which event matters.
# The first version of this gated on "EXPLORER engaged" and fired at 6-9 SECONDS: AutoChuck
# takes control of Chuck on the first level the AI can reach, long before the DebugJump
# screen exists, so that gate was a fixed wall clock wearing an event's clothes — exactly
# what WAITJUMP was built to avoid, since this boot's depth in fixed time has always been a
# distribution (gotcha 75). The real event is the jump being SERVICED, which the same run
# logged at 28 s and other boots have logged as late as 131 s.
#
# Usage:  tools/part52_recon.sh <tag> [stats|nostats] [profile]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part52}"
TAG="${1:-nostats}"
MODE="${2:-nostats}"
PROF="${3:-}"
SECS="${SECS:-420}"
FLAT_SECS="${FLAT_SECS:-40}"
DWARF_SECS="${DWARF_SECS:-20}"
ROAM_SECS="${ROAM_SECS:-100}"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

LOG="$OUT/$TAG.log"
envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
      CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE)
[ "$MODE" = stats ] && envv+=("CZ_VK_FRAME_STATS=$OUT/$TAG.stats")
[ -n "$PROF" ] && envv+=(CZ_VK_PROFILE=30)
# ENVX="VAR=1 VAR2=1" adds arbitrary arms to the run. This is what makes the recon
# reusable as the A/B harness for an ITEM rather than only as a survey: the control arm
# for a change measured in symbol shares has to be the same binary, the same route and
# the same event gate, and re-deriving all three in a second script is how two arms drift
# apart (gotcha 51 — the control is the old configuration run NOW, under the same
# conditions). Used for item 1.0 as ENVX=CZ_PM4_NO_SHADER_MEMO=1.
if [ -n "${ENVX:-}" ]; then
    for kv in $ENVX; do envv+=("$kv"); done
fi
# NO_DWARF=1 skips the call-graph pass. The flat profile is what an item's A/B reads;
# the DWARF pass is for attribution and costs ~500 MB and 20 s a run.
[ -n "${NO_DWARF:-}" ] && DWARF_SECS=0

echo "=== $TAG ($MODE${PROF:+ +profile}) $(date +%H:%M:%S)"
( cd "$ROOT/runtime/build" && env "${envv[@]}" timeout "$SECS" ./cz_runtime > "$LOG" 2>&1 ) &
RUNNER=$!

PID=""
for _ in $(seq 1 120); do
    for p in $(pgrep -f cz_runtime 2>/dev/null); do
        c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
        case "$c" in cz_runtime*) PID=$p ;; esac
    done
    [ -n "$PID" ] && break
    sleep 1
done
[ -z "$PID" ] && { echo "!! no cz_runtime appeared"; wait $RUNNER; exit 3; }
echo "    pid=$PID"

# The EVENT: the DebugJump request being SERVICED by the frontend manager. That is the
# moment the outdoor level starts loading, and it is what WAITJUMP itself waits for. Then
# a fixed roam so the sample lands in a crowd rather than at the spawn point. Both arms use
# the same event and the same roam, which is what makes them comparable.
jumped=0
for _ in $(seq 1 300); do
    kill -0 "$PID" 2>/dev/null || break
    if grep -q "requested DebugJump through frontend manager" "$LOG" 2>/dev/null; then
        jumped=1; break
    fi
    sleep 2
done
[ "$jumped" = 1 ] || echo "    !! the DebugJump was never serviced -- this sample is NOT outdoors"

echo "    outdoors at $(date +%H:%M:%S); roaming ${ROAM_SECS}s before sampling"
sleep "$ROAM_SECS"

python3 "$ROOT/tools/part50_thread_cpu.py" 20 > "$OUT/$TAG.threadcpu" 2>&1
perf record -F 999 -p "$PID" -o "$OUT/$TAG.flat.perf.data" -- sleep "$FLAT_SECS" \
    > "$OUT/$TAG.perf.log" 2>&1
# DWARF unwinding is expensive and writes a lot, so it gets a shorter window. It is the
# only way to attribute a hot LEAF to its callers here: the build is -O2 without frame
# pointers, so an fp walk would be fiction rather than merely imprecise.
if [ "$DWARF_SECS" != 0 ]; then
perf record -F 499 --call-graph dwarf,16384 -p "$PID" -o "$OUT/$TAG.cg.perf.data" \
    -- sleep "$DWARF_SECS" >> "$OUT/$TAG.perf.log" 2>&1
fi

kill "$PID" 2>/dev/null
wait $RUNNER 2>/dev/null
echo "    done: $(du -sh "$OUT/$TAG.flat.perf.data" "$OUT/$TAG.cg.perf.data" 2>/dev/null | tr '\n' ' ')"
