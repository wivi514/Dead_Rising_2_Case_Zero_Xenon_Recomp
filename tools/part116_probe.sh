#!/bin/bash
# Perf plan part 116, items 0/1/4: PROFILE THE GUEST'S OWN 8.8 ms FOR THE FIRST TIME.
#
# WHY A NEW ONE RATHER THAN part109_probe.sh. That harness is the parent of this file and
# its method is kept whole (run the operator's crowd route, wait for the crowd as an EVENT,
# thread census, flat `perf record`, context-switch counts). Three things are different,
# each because part 110 or the plan asked for it:
#
#   1. THE EXECUTABLE IS ARCHIVED BESIDE THE perf.data (gotcha 550: three of part 109's
#      four captures were unreadable a day later because the binary had been rebuilt).
#      `perf report` is pointed at it with `--symfs`-free means: the archived copy is
#      the file the capture's mmap records name, so the copy is made BEFORE the run
#      under the same path the route copies to (`cz_runtime_crowd`), then moved.
#   2. A SECOND, SHORT capture with DWARF call stacks (`CG=1`), because the recompiled
#      TUs are built -O2 without frame pointers and `--call-graph fp` would return the
#      pump's own frames only. DWARF unwinding copies 16 KB of stack per sample, so it
#      is a 5-second window at 499 Hz, not the flat 30 s — enough for a per-CALLER
#      table of the top guest symbols, and nothing else.
#   3. THE RESOLUTION IS PINNED AT 1920x1080 by default, the plan's rule; the crowd
#      route's own default is 3440x1440. A guest-side question does not depend on it,
#      but every arm in this part must share one.
#
# The guest threads are NAMED now (the SetThreadName exception binds the guest's name to
# the host tid as of part 116), so the census and `perf report --sort comm` read
# `Main Thread` / `JobThreadN` rather than anonymous tids.
#
# NO CZ_VK_PROFILE (4-5 ms a frame, inverts the regime — gotcha 454). `perf` needs no
# instrument in the process.
#
# Usage:
#   tools/part116_probe.sh <tag> [ENV=VAL ...]
#   CG=1 tools/part116_probe.sh guest_cg          # add the DWARF call-graph window
#   PERF=0 tools/part116_probe.sh base1           # route + census only, no perf at all
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part116}"
TAG="${1:?usage: part116_probe.sh <tag> [ENV=VAL ...]}"; shift || true
PERF="${PERF:-1}"
PERF_SECS="${PERF_SECS:-30}"
CG="${CG:-0}"
CG_SECS="${CG_SECS:-5}"
CPU_SECS="${CPU_SECS:-15}"
DRAW_GATE="${DRAW_GATE:-8000}"
RES="${RES:-1920x1080}"
BIN_SRC="${BIN_SRC:-$ROOT/runtime/build/cz_runtime}"
# The soak must outlast the gate wait plus every sampling window, or `perf` records the
# route's tail instead of the crowd (part 109's lesson).
SOAK="${SOAK:-120}"
mkdir -p "$OUT"

maxf=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null || echo "?")
echo "=== $TAG  res $RES  scaling_max_freq $maxf kHz  soak ${SOAK}s  perf ${PERF_SECS}s once draws >= $DRAW_GATE"
echo "$maxf" > "$OUT/$TAG.maxfreq"
# Archive the binary FIRST, from its source, so a mid-campaign rebuild cannot change
# what this tag names. sha256 so a reader can match it to a perf.data's build-id.
cp -f "$BIN_SRC" "$OUT/$TAG.bin"
sha256sum "$OUT/$TAG.bin" | cut -c1-16 > "$OUT/$TAG.bin.sha"

( OUT="$OUT" RES="$RES" SOAK="$SOAK" BIN_SRC="$BIN_SRC" "$ROOT/tools/part80_crowdroute.sh" "$TAG" \
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

# TWO CONSECUTIVE WINDOWS at the gate, not one: the sweeps swing the view into the crowd
# and out again before the stationary soak, and a single window can read 9,141 then
# 5,800 (part 109).
reached=0
prev=0
d=""
for _ in $(seq 1 200); do
    kill -0 "$PID" 2>/dev/null || break
    d=$(grep -a "^\[fps\]" "$LOG" 2>/dev/null | grep -aoE "draws med [0-9]+" | tail -1 | awk '{print $3+0}')
    if [ "${d:-0}" -ge "$DRAW_GATE" ] && [ "$prev" -ge "$DRAW_GATE" ] && [ "${d:-0}" != "$prev" ]; then
        reached=1; break
    fi
    [ "${d:-0}" != "$prev" ] && prev="${d:-0}"
    sleep 2
done
if [ "$reached" != 1 ]; then echo "    !! never reached $DRAW_GATE draws (last=${d:-none}); sampling anyway"; fi
echo "    at the crowd $(date +%H:%M:%S), draws=${d:-?}"

# Named threads: tid -> comm, once, so a perf.data read later can be joined to the census
# even after the process is gone.
for t in /proc/$PID/task/*; do printf '%s %s\n' "$(basename "$t")" "$(cat "$t/comm" 2>/dev/null)"; done \
    > "$OUT/$TAG.threads" 2>/dev/null

python3 "$ROOT/tools/part50_thread_cpu.py" "$CPU_SECS" > "$OUT/$TAG.threadcpu" 2>&1
if [ "$PERF" = 1 ]; then
    perf record -F 999 -p "$PID" -o "$OUT/$TAG.perf.data" -- sleep "$PERF_SECS" \
        > "$OUT/$TAG.perf.log" 2>&1
    echo "    perf: $(du -h "$OUT/$TAG.perf.data" 2>/dev/null | cut -f1)"
    # A SYMFS for this capture, so `perf report/script --symfs` resolve symbols from
    # the ARCHIVED binary after the route has overwritten cz_runtime_crowd with the
    # next arm's. The build-id cache (`perf buildid-cache -a`) was tried first and
    # does NOT do this: perf reads the file at the recorded path, finds a build-id
    # mismatch and prints raw addresses rather than falling back to the cache. That
    # is the mechanism behind gotcha 550's "unreadable a day later". The symfs tree
    # mirrors the recorded absolute path onto the archive and /usr/lib64 onto itself.
    SYMFS="$OUT/$TAG.symfs"
    mkdir -p "$SYMFS$(dirname "$ROOT/runtime/build/cz_runtime_crowd")" "$SYMFS/usr"
    ln -sfn "$OUT/$TAG.bin" "$SYMFS$ROOT/runtime/build/cz_runtime_crowd"
    ln -sfn /usr/lib64 "$SYMFS/usr/lib64"
    echo "    read with: --symfs $SYMFS"
    if [ "$CG" = 1 ]; then
        perf record -F 499 --call-graph dwarf,16384 -p "$PID" -o "$OUT/$TAG.cg.perf.data" \
            -- sleep "$CG_SECS" > "$OUT/$TAG.cg.perf.log" 2>&1
        echo "    perf cg: $(du -h "$OUT/$TAG.cg.perf.data" 2>/dev/null | cut -f1)"
    fi
fi
# Context switches per thread: the second, independent instrument on spinning vs working.
for t in /proc/$PID/task/*; do
    tid=$(basename "$t")
    printf '%s %s %s %s %s\n' "$tid" "$(cat "$t/comm" 2>/dev/null)" \
        "$(awk '/^voluntary/{print $2}' "$t/status" 2>/dev/null)" \
        "$(awk '/^nonvoluntary/{print $2}' "$t/status" 2>/dev/null)" \
        "$(cat "$t/wchan" 2>/dev/null || echo -)"
done > "$OUT/$TAG.ctxsw" 2>/dev/null

wait $RUNNER
cat "$OUT/$TAG.route.out"
echo "artifacts in $OUT ($TAG.*)"
