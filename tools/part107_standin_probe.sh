#!/bin/bash
# Perf plan part 107, item 0: WHERE DOES THE CROWD FRAME GO ON THE 4-CORE STAND-IN?
#
# WHY THIS EXISTS. Every decomposition this project holds was taken on 8 physical cores at
# stock clock, where a spinning guest thread is free and the pump has a core to itself.
# The target is a Ryzen 3 3100 (4c/8t, ~0.7x per thread), and perf-plan-part107 §0.4 says
# the item an 8-core measurement can never find is CONTENTION: which threads share a
# core, and what a busy-waiting sibling costs the pump. So the profile has to be taken
# UNDER THE MASK (taskset to four cores + their SMT siblings) AND under the clock cap the
# operator set (3.2 GHz, checked and printed below, not assumed — a number taken at stock
# and filed as the stand-in's is gotcha 13's shape).
#
# WHAT IT DOES. Runs the operator's crowd route (tools/part80_crowdroute.sh, 1080p, mirror
# on) pinned to the mask, waits for the [fps] windows to report >= 8,000 draws — an event,
# not a wall clock (gotcha 75) — and then, inside the stationary soak:
#   1. per-thread CPU over a window (tools/part50_thread_cpu.py): who is busy;
#   2. a flat `perf record -F 999` of the whole process for PERF_SECS: which SYMBOLS on
#      which THREAD (tools/part53_symbols.py reads it per thread, renormalised);
#   3. the kernel's context-switch counts per thread: voluntary switches say a thread
#      sleeps, involuntary say it computes. The second, independent instrument on
#      "spinning or working" (part 51).
# NO CZ_VK_PROFILE: the phase profiler costs 2-4 ms a frame and inverts the regime
# (gotcha 454). Run that as a separate arm if the phase table is wanted.
#
# Usage:
#   tools/part107_standin_probe.sh <tag> [ENV=VAL ...]
#   CPUS=0-15 tools/part107_standin_probe.sh c8_base            # the unmasked control
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part107}"
TAG="${1:?usage: part107_standin_probe.sh <tag> [ENV=VAL ...]}"; shift || true
CPUS="${CPUS:-0-3,8-11}"
PERF_SECS="${PERF_SECS:-30}"
CPU_SECS="${CPU_SECS:-15}"
DRAW_GATE="${DRAW_GATE:-8000}"
mkdir -p "$OUT"

maxf=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null || echo "?")
echo "=== $TAG  mask $CPUS  scaling_max_freq $maxf kHz  perf ${PERF_SECS}s once draws >= $DRAW_GATE"
echo "$maxf" > "$OUT/$TAG.maxfreq"

( OUT="$OUT" RES=1920x1080 taskset -c "$CPUS" "$ROOT/tools/part80_crowdroute.sh" "$TAG" \
      CZ_NO_WINDOW=1 "CZ_VK_FRAME_TRACE=$OUT/$TAG.trace" CZ_FPS_LOG=10 "$@" \
      > "$OUT/$TAG.route.out" 2>&1 ) &
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
