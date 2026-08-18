#!/bin/bash
# Part 53 item 1.1 (parallel content guards): does moving the guards off the pump
# shorten the FRAME, and what does it cost the machine?
#
# Same shape as `part52_item_campaign.sh` and for the same reasons — one pinned binary,
# arms selected by environment only, alternated rather than blocked, and a NULL arm that
# cannot move the statistic so every effect is quoted as a multiple of the floor
# (gotcha 331). Read that script's header for the argument; only what differs is
# restated here.
#
# WHAT DIFFERS. This item has a real runtime switch on both halves it touches, so unlike
# part 52's campaign the control arm restores the WHOLE item rather than one of four:
#
#   base    the pool on (the shipped default), 4 workers
#   noitem  CZ_VK_NO_PARALLEL_GUARD=1 — both guards fold inline on the pump, which is
#           what this runtime did for fifty-two parts. The old configuration RUN NOW.
#   null    `base` again — the noise floor.
#
# ~~THE CAP IS RAISED IN EVERY ARM.~~ TRUE WHEN THIS RAN, AND INVERTED AT THE CLOSE OF THE
# SAME PART. When this campaign ran, the runtime default was 60 fps and this route sat on
# that cap for most of its length, so both arms read 16.2 ms whatever the change was worth
# and `CAP=120` is what made it readable. The default is now 500 (a 1 ms vblank period),
# so `CAP=120` LOWERS the ceiling instead of raising it — see the note above the value.
# What this reports is still a CPU SAVING and not a frame rate a player sees; the
# player-facing number comes from the operator's soak, which is not capped.
#
# AND THE COST THIS ITEM HAS THAT PART 52'S DID NOT. Strategy (b) spends idle cores to
# shorten one thread, so the process's TOTAL cpu goes UP even when the frame gets
# shorter. `tools/part50_thread_cpu.py` is sampled in every arm for exactly that reason —
# a saving reported without its bill is half a measurement.
#
# Usage:  tools/part53_item_campaign.sh [runs]      # 3 runs ≈ 50 min
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part53/frame}"
mkdir -p "$OUT"

BIN=cz_runtime_p53
if [ ! -x "$ROOT/runtime/build/$BIN" ]; then
    cp "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($(cd "$ROOT" && git rev-parse --short HEAD))"
fi

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

#
# !! READ BEFORE RE-RUNNING, as of the close of part 53: THE RUNTIME DEFAULT IS NOW 500,
# not 60. `CAP=120` below therefore CONSTRAINS the run rather than lifting it — it sets a
# 4 ms vblank period where the default is 1 ms, which coarsens the frame-time ladder and
# binds the light draw bins at 125 fps. The value is left as it was so the campaign
# already recorded with it stays reproducible; a FRESH campaign should pass `CAP=500`.
CAP="${CAP:-120}"
declare -A ARMS=( [noitem]="CZ_VK_NO_PARALLEL_GUARD=1" )
ORDER=(base noitem null)
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE
RUNS="${1:-3}"
SECS="${SECS:-330}"

for i in $(seq 1 "$RUNS"); do
  for arm in "${ORDER[@]}"; do
    tag="${arm}_$i"
    [ -f "$OUT/$tag.done" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $tag $(date +%H:%M:%S)"
    envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ" "CZ_FPS_CAP=$CAP"
          "CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    [ -n "${ARMS[$arm]:-}" ] && envv+=(${ARMS[$arm]})
    ( cd "$ROOT/runtime/build" && \
      env "${envv[@]}" timeout "$SECS" "./$BIN" > "$OUT/$tag.log" 2>&1 ) &
    RUNNER=$!
    # Sample the whole process's per-thread CPU once the route is outdoors, so the
    # campaign carries the item's BILL as well as its benefit.
    ( for _ in $(seq 1 150); do
        grep -q "requested DebugJump through frontend manager" "$OUT/$tag.log" 2>/dev/null && break
        sleep 2
      done
      sleep 90
      P=""
      for p in $(pgrep -f "$BIN" 2>/dev/null); do
          c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
          case "$c" in cz_runtime*) P=$p ;; esac
      done
      [ -n "$P" ] && python3 "$ROOT/tools/part50_thread_cpu.py" 20 > "$OUT/$tag.threadcpu" 2>&1 ) &
    wait $RUNNER
    wait
    if [ ! -s "$OUT/$tag.stats" ]; then
        echo "    !! $tag produced NO frame stats — see $tag.log"; continue
    fi
    echo "COMPLETE $(date +%s)" > "$OUT/$tag.done"
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats")
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))  $(grep -a 'process total' "$OUT/$tag.threadcpu" 2>/dev/null)"
  done
done
echo "campaign done; artifacts in $OUT"
echo "read with: python3 tools/frame_perf_bins.py $OUT/base_*.stats -- $OUT/noitem_*.stats"
echo "the null:  python3 tools/frame_perf_bins.py $OUT/base_*.stats -- $OUT/null_*.stats"
