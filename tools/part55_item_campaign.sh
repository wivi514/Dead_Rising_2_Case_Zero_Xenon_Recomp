#!/bin/bash
# Part 55's frame-time campaign: does an item shorten the FRAME, and what does it cost?
#
# Same shape as `part53_item_campaign.sh` and for the same reasons — one pinned binary,
# arms selected by environment only, alternated rather than blocked, and a NULL arm that
# cannot move the statistic so every effect is quoted as a multiple of the floor
# (gotcha 331). Read that script's header for the argument; only what differs is
# restated here.
#
# WHAT DIFFERS, and both are corrections to its predecessor.
#
#  1. **The cap is the shipped default (500), not 120.** Part 53's campaign passed
#     `CAP=120` to lift a 60 fps ceiling that was binding every bin; the default moved to
#     500 at the close of the same part, so passing 120 now LOWERS the ceiling and
#     coarsens the frame-time ladder. `CAP=` here is the shipped value and should stay
#     that way unless a run is deliberately reproducing an old campaign.
#
#  2. **The arms are given on the command line**, because part 55 has several items in
#     one family (the flat caches) and writing a script per arm is how two arms drift
#     apart. Every arm is one environment assignment against the SAME binary, and the
#     shipped default is always `base`:
#
#       tools/part55_item_campaign.sh 3 nofl=CZ_VK_NO_FLAT_CACHE=1
#
#     runs base / nofl / null, three rounds, alternated.
#
# AND THE WARNING THAT OUTRANKS EVERYTHING THIS SCRIPT PRODUCES (gotcha 355): this route's
# best-populated draw band is 2,500-3,000 while the operator plays at 6,700-7,300. Six
# campaigns in part 54 all sampled the light end and the headline was over-generalised by
# a factor of six. Read the bins, quote the draw count, and treat an operator soak as the
# number that decides.
#
# Usage:  tools/part55_item_campaign.sh [runs] [arm=ENV=VAL]...
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part55/frame}"
mkdir -p "$OUT"

BIN="${BIN:-cz_runtime_p55}"
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

CAP="${CAP:-500}"
# Arms from the command line: `name=VAR=VALUE`. `base` (the shipped default) and `null`
# (base again, the noise floor) are always present and always first and last.
declare -A ARMS=()
ORDER=(base)
for a in "${@:2}"; do
    name="${a%%=*}"; kv="${a#*=}"
    ARMS[$name]="$kv"
    ORDER+=("$name")
done
ORDER+=(null)
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
for arm in "${ORDER[@]}"; do
    [ "$arm" = base ] && continue
    echo "read with: python3 tools/frame_perf_bins.py $OUT/base_*.stats -- $OUT/${arm}_*.stats"
done
