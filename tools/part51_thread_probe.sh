#!/bin/bash
# Part 51 item 0: WHAT IS THE 93%-BUSY GUEST THREAD ACTUALLY DOING?
#
# WHY THIS EXISTS. `tools/part50_thread_cpu.py` established that this process uses 2.46
# of 16 cores and that the busiest thread is not ours -- it is a GUEST thread at 93.2% of
# one core, i.e. the recompiled title simulating. Every remaining item in
# `docs/perf-plan-part50.md` makes OUR graphics pump's work smaller, and if that guest
# thread is the real limiter then all of them buy nothing. But a CPU percentage cannot
# tell WORKING from SPINNING (gotcha 338): a guest thread spinning on a lock burns exactly
# 100% of a core and looks identical to one doing useful arithmetic.
#
# WHAT SEPARATES THEM IS THE INSTRUCTION POINTER, and we have symbols for every guest
# function (`sub_XXXXXXXX`, RelWithDebInfo), so a flat `perf` profile answers it directly:
#
#   spinning  -> a handful of symbols take nearly all samples, and the same few addresses
#                repeat; typically one of our kernel HLE waits or a guest retry loop.
#   working   -> the samples spread over hundreds of guest functions, the way a game's
#                simulation frame does.
#
# It samples the whole process, not one thread, for two reasons: `perf report --per-thread`
# splits it afterwards for free, and the same recording prices OUR pump's symbols on the
# same time base -- which is the honest way to compare the two threads' work, where the
# per-phase profiler can only ever see inside the pump (and costs 2-4 ms doing it, §6cg).
#
# Deliberately NOT profiled: no CZ_VK_PROFILE. Part 50 measured that instrument at 8-18%
# of the frame, and this run is asking what the machine does in play, not in measurement.
#
# Usage: part51_thread_probe.sh [tag]        # default tag "base"
# Artifacts land in ~/DR2CZ-troubleshooting/part51/.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part51}"
TAG="${1:-base}"
SECS="${SECS:-420}"          # whole run
PERF_SECS="${PERF_SECS:-30}" # sampling window, once the route is outdoors
CPU_SECS="${CPU_SECS:-25}"   # /proc thread-CPU window
DRAW_GATE="${DRAW_GATE:-4000}"
EXTRA_ENV="${EXTRA_ENV:-}"
mkdir -p "$OUT"

# Same guard as part50_campaign.sh, and for the same reason: two instances at once
# measures contention. Matched on /proc/PID/comm by PREFIX -- `pgrep -f` matches this
# script's own launcher, and `pgrep -x` cannot see a name past 15 characters.
busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

STATS="$OUT/$TAG.stats"; LOG="$OUT/$TAG.log"
rm -f "$STATS" "$LOG"
echo "=== $TAG $(date +%H:%M:%S)  (run ${SECS}s, perf ${PERF_SECS}s once draws > $DRAW_GATE)"
( cd "$ROOT/runtime/build" && \
  env CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER \
      CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE \
      "CZ_VK_FRAME_STATS=$STATS" $EXTRA_ENV \
      timeout "$SECS" ./cz_runtime > "$LOG" 2>&1 ) &
RUNNER=$!

# Find the game's pid (the `timeout` child), then wait for the route to be OUTDOORS.
# Anchoring on the DRAW COUNT rather than on a wall clock is the same rule WAITJUMP
# exists for: this boot's depth in fixed time has always been a distribution (gotcha 75).
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

reached=0
for _ in $(seq 1 300); do
    kill -0 "$PID" 2>/dev/null || break
    d=$(awk 'NR>1 {v=$2} END {print v+0}' "$STATS" 2>/dev/null)
    if [ "${d:-0}" -gt "$DRAW_GATE" ]; then reached=1; break; fi
    sleep 2
done
if [ "$reached" != 1 ]; then
    echo "    !! never reached $DRAW_GATE draws (last=${d:-none}); sampling anyway"
fi
echo "    outdoors at $(date +%H:%M:%S), draws=${d:-?}"

# 1. Per-thread CPU over its own window, so the perf profile can be read against a known
#    utilisation rather than assuming part 50's numbers still hold on this route today.
python3 "$ROOT/tools/part50_thread_cpu.py" "$CPU_SECS" > "$OUT/$TAG.threadcpu" 2>&1
tail -20 "$OUT/$TAG.threadcpu"

# 2. The flat profile. No call graph: at -O2 without frame pointers an fp walk is fiction,
#    and dwarf unwinding at 999 Hz over 37 threads writes gigabytes. The question here is
#    which INSTRUCTIONS run, and that needs no stack.
perf record -F 999 -p "$PID" -o "$OUT/$TAG.perf.data" -- sleep "$PERF_SECS" \
    > "$OUT/$TAG.perf.log" 2>&1
echo "    perf: $(du -h "$OUT/$TAG.perf.data" 2>/dev/null | cut -f1)"

# 3. And the kernel's own answer to "is it blocked": a thread that is genuinely computing
#    context-switches involuntarily (its timeslice expires); one waiting on a futex or a
#    sleep switches VOLUNTARILY, thousands of times a second. This is a second, independent
#    instrument on the same question and it costs nothing.
for t in /proc/$PID/task/*; do
    tid=$(basename "$t")
    printf '%s %s %s %s\n' "$tid" \
        "$(awk '/^voluntary/{print $2}' "$t/status" 2>/dev/null)" \
        "$(awk '/^nonvoluntary/{print $2}' "$t/status" 2>/dev/null)" \
        "$(cat "$t/wchan" 2>/dev/null || echo -)"
done > "$OUT/$TAG.ctxsw" 2>/dev/null

wait $RUNNER
frames=$(( $(wc -l < "$STATS" 2>/dev/null || echo 1) - 1 ))
echo "    done: frames=$frames peak draws=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$STATS")"
echo "artifacts in $OUT ($TAG.*)"
