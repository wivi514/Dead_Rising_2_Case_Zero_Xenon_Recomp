#!/bin/bash
# Perf plan part 109, item 0: WHERE DOES THE CROWD FRAME GO ON THE OPERATOR'S OWN BOX?
#
# WHY A NEW ONE RATHER THAN part107_standin_probe.sh. That harness is the same shape and
# it is the parent of this file, but it pins two things this question cannot inherit:
# `taskset -c 0-3,8-11` at a 3.2 GHz cap (the Ryzen 3 STAND-IN — a machine that is not
# the target any more) and `RES=1920x1080 CZ_NO_WINDOW=1`. The part-109 target is the
# operator's own 8c/16t at 4654 MHz **presenting through a real swapchain at 3440x1440**,
# because that is the configuration §4.1's baseline was measured in and a decomposition
# taken in a different one cannot be subtracted from it (gotcha 50/51/86: the control is
# the configuration run NOW). Headless drops the present path entirely, which is exactly
# the kind of windowed-only cost part 76 found reading 0.0% headlessly (gotcha 445).
#
# WHAT IT DOES, unchanged from its parent because the method is sound:
#   1. run the operator's crowd route and wait for an EVENT — the [fps] windows reporting
#      >= 8,000 draws — not a wall clock (gotcha 75);
#   2. per-thread CPU over a window: who is busy;
#   3. a flat `perf record -F 999` of the whole process: which SYMBOLS on which THREAD,
#      read per thread and renormalised by tools/part53_symbols.py, and split by SOURCE
#      LINE by tools/part55_srcline.py — the two readers that make a symbol an item;
#   4. voluntary/involuntary context switches per thread: the second, independent
#      instrument on "spinning or working".
#
# NO CZ_VK_PROFILE. The phase profiler costs 4.2-5.0 ms a frame on this box (§4.1) and
# inverts the regime (gotcha 454). `perf` needs no instrument in the process at all, which
# is the whole reason part 51 moved to it. Run the phase table as a separate arm.
#
# Usage:
#   tools/part109_probe.sh <tag> [ENV=VAL ...]
#   RES=1920x1080 tools/part109_probe.sh crowd1080     # the resolution control
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part109}"
TAG="${1:?usage: part109_probe.sh <tag> [ENV=VAL ...]}"; shift || true
PERF_SECS="${PERF_SECS:-30}"
CPU_SECS="${CPU_SECS:-15}"
DRAW_GATE="${DRAW_GATE:-8000}"
RES="${RES:-3440x1440}"
# The soak has to outlast the sampling window or `perf` records the route's tail instead
# of the crowd: 15 s of thread CPU + 30 s of perf + slack.
SOAK="${SOAK:-90}"
mkdir -p "$OUT"

maxf=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null || echo "?")
echo "=== $TAG  res $RES  scaling_max_freq $maxf kHz  soak ${SOAK}s  perf ${PERF_SECS}s once draws >= $DRAW_GATE"
echo "$maxf" > "$OUT/$TAG.maxfreq"

( OUT="$OUT" RES="$RES" SOAK="$SOAK" "$ROOT/tools/part80_crowdroute.sh" "$TAG" \
      CZ_FPS_LOG=10 "$@" > "$OUT/$TAG.route.out" 2>&1 ) &
RUNNER=$!

# The game's pid, by /proc comm PREFIX (never `pgrep -f`: it matches this script; never
# `pgrep -x`: the name is 16 characters and the kernel keeps 15).
PID=""
for _ in $(seq 1 90); do
    for p in $(pgrep cz_runtime 2>/dev/null); do
        c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
        case "$c" in cz_runtime_crow*) PID=$p ;; esac
    done
    [ -n "$PID" ] && break
    sleep 1
done
[ -z "$PID" ] && { echo "!! no cz_runtime_crowd appeared"; wait $RUNNER; exit 3; }
LOG=""
for _ in $(seq 1 30); do LOG=$(ls -t "$OUT"/crowd_*_"$TAG".log 2>/dev/null | head -1); [ -n "$LOG" ] && break; sleep 1; done
echo "    pid=$PID  log=$LOG"

reached=0
for _ in $(seq 1 200); do
    kill -0 "$PID" 2>/dev/null || break
    d=$(grep -a "^\[fps\]" "$LOG" 2>/dev/null | grep -aoE "draws med [0-9]+" | tail -1 | awk '{print $3+0}')
    if [ "${d:-0}" -ge "$DRAW_GATE" ]; then reached=1; break; fi
    sleep 2
done
if [ "$reached" != 1 ]; then echo "    !! never reached $DRAW_GATE draws (last=${d:-none}); sampling anyway"; fi
echo "    at the crowd $(date +%H:%M:%S), draws=${d:-?}"

python3 "$ROOT/tools/part50_thread_cpu.py" "$CPU_SECS" > "$OUT/$TAG.threadcpu" 2>&1
perf record -F 999 -p "$PID" -o "$OUT/$TAG.perf.data" -- sleep "$PERF_SECS" \
    > "$OUT/$TAG.perf.log" 2>&1
echo "    perf: $(du -h "$OUT/$TAG.perf.data" 2>/dev/null | cut -f1)"
for t in /proc/$PID/task/*; do
    tid=$(basename "$t")
    printf '%s %s %s %s\n' "$tid" \
        "$(awk '/^voluntary/{print $2}' "$t/status" 2>/dev/null)" \
        "$(awk '/^nonvoluntary/{print $2}' "$t/status" 2>/dev/null)" \
        "$(cat "$t/wchan" 2>/dev/null || echo -)"
done > "$OUT/$TAG.ctxsw" 2>/dev/null

wait $RUNNER
cat "$OUT/$TAG.route.out"
echo "artifacts in $OUT ($TAG.*)"
