#!/bin/bash
# Part 51: does the graphics pump's SLEEP cost frame time? One pinned binary, three arms.
#
# WHY. The pump loop sleeps a fixed 1 ms before every ring walk. At this title's ~3.0
# ticks a frame that is ~3 ms of every frame with the pump off the CPU -- 10-18% of the
# wall clock in the `pump` line, printed since part 18 and never questioned -- while the
# title's Draw Thread spins at 93% of a core on the read pointer that only that walk
# advances (finding 38). Nothing in `docs/perf-plan-part50.md` accounts for it: every item
# there makes the pump's WORK smaller, and a sleep is not work.
#
# THE ARMS, one variable each, all from the same binary:
#
#   base    the shipped 1 ms tick
#   fast    CZ_PM4_TICK_US=100    ten times finer
#   slow    CZ_PM4_TICK_US=4000   four times coarser -- THE POSITIVE CONTROL, and the
#                                 arm that makes this an experiment rather than a hope.
#                                 If the sleep is on the critical path, `slow` must be
#                                 WORSE by roughly the same mechanism `fast` is better
#                                 by. If all three read the same across a 40x range of
#                                 tick periods, the sleep is not on the critical path
#                                 and the item is dead -- which is a result, not a
#                                 failure (gotcha 30: an arm that cannot fail has not
#                                 tested anything).
#
# THE NULL CONTROL IS `base` AGAINST ITSELF, as in part 50: the title's own AI drives the
# route, so no two runs walk the same stream and the honest floor is two runs of one
# configuration. Part 50 measured that floor at 8-18% on frame time by draw band, which
# is why the mechanism statistics (`sleep %`, the sleep-before-progress bound, ticks per
# frame) matter as much as the milliseconds: they move by construction if the arm engaged
# at all, and an arm that cannot be shown to have engaged proves nothing (gotcha 151).
#
# PROFILED AND UNPROFILED RUNS ARE BOTH NEEDED AND MUST NOT BE MIXED. `CZ_VK_PROFILE`
# costs 2-4 ms a frame (part 50 §6cg §6) but is the only source of the `pump` line, so:
#   MODE=profile   one run an arm, for the MECHANISM (does the sleep actually shrink?)
#   MODE=frame     three runs an arm, no profiler, for the frame TIME
# Never quote a frame time from the profile runs.
#
# Usage:  MODE=profile tools/part51_tick_campaign.sh        # ~17 min, run this FIRST
#         MODE=frame   tools/part51_tick_campaign.sh 3      # ~50 min
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${MODE:-profile}"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part51/$MODE}"
mkdir -p "$OUT"

BIN=cz_runtime_p51
if [ ! -x "$ROOT/runtime/build/$BIN" ]; then
    cp "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($(cd "$ROOT" && git rev-parse --short HEAD))"
fi

# /proc/PID/comm by prefix -- `pgrep -f` matches this script's own launcher and `pgrep -x`
# cannot see a name past 15 characters. Both traps cost a part each.
busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

declare -A ARMS=( [fast]="CZ_PM4_TICK_US=100" [slow]="CZ_PM4_TICK_US=4000" )
ORDER=(base fast slow)
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE
RUNS="${1:-1}"
SECS="${SECS:-330}"

for i in $(seq 1 "$RUNS"); do
  for arm in "${ORDER[@]}"; do
    tag="${arm}_$i"
    [ -f "$OUT/$tag.stats" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $MODE $tag $(date +%H:%M:%S)"
    envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ"
          "CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    [ "$MODE" = profile ] && envv+=(CZ_VK_PROFILE=30)
    [ "$arm" != base ] && envv+=(${ARMS[$arm]})
    ( cd "$ROOT/runtime/build" && \
      env "${envv[@]}" timeout "$SECS" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    # A growing file read mid-run is a complete file that ends early (gotcha 339), so
    # the driver writes its own completion marker and the reader checks THAT.
    if [ ! -s "$OUT/$tag.stats" ]; then
        echo "    !! $tag produced NO frame stats -- see $tag.log"; continue
    fi
    echo "COMPLETE $(date +%s)" > "$OUT/$tag.done"
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats")
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))"
    [ "$MODE" = profile ] && grep -h "critical path" "$OUT/$tag.log" | tail -3
  done
done
echo "campaign done ($MODE); artifacts in $OUT"
