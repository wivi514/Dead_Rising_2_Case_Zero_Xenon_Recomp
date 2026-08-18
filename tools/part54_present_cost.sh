#!/bin/bash
# Part 54: PRICE THE PRESENT PATH, WINDOWED, AT BOTH RESOLUTIONS.
#
# WHY THIS EXISTS. Part 53 closed with an arithmetic claim and no measurement: "the
# present readback is the scale SQUARED in bytes, 3.5 MB/frame at 1x and 14.1 at 2x, and
# at 2x it is the single largest fixed per-frame cost in the renderer." That is a
# multiplication, not a number anyone read off a running game — and the plan's §7
# swapchain item was PROMOTED on the strength of it. Every part of this project that
# built before it measured has repriced or killed the item afterwards, so this measures
# it first.
#
# IT MUST BE WINDOWED, AND THAT IS THE WHOLE POINT. `Host_PresentPixels` returns
# immediately when there is no window (`g_active` is false), so the `readback` phase
# reads 0.0% on every headless run this project has ever taken — including the ones part
# 53 used to declare item 1.3 done. The cost being priced here EXISTS ONLY WITH A WINDOW,
# and it is three copies of the frame, not one:
#
#   1. GPU:    vkCmdCopyImageToBuffer, colour image -> host-visible buffer (device->host)
#   2. pump:   Host_PresentPixels memcpy into g_pixelsBack, under g_frameMutex
#   3. window: SDL_UpdateTexture from g_pixelsFront, under the SAME mutex (host->device)
#
# Only (2) is charged to `readback`. (3) is on the window thread and is visible only in
# per-thread CPU, which is why this samples /proc as well as the profiler — the same
# reason part 53 had to: a cost that MOVES between threads is invisible to any instrument
# that reads one of them (gotcha 344).
#
# THE ROUTE is the recon's: DebugJump to the outdoor level, AutoChuck driving, so the
# sample lands in a crowd with no operator. The arms differ in ONE variable.
#
# WINSIZE=WxH RESIZES THE WINDOW IN BOTH ARMS, AND IT IS NOT OPTIONAL FOR A SWAPCHAIN
# NUMBER. The first campaign run from this script left the window at its default and
# measured a ~1088x612 drawable; the operator plays MAXIMIZED at 2560x1417. Those are not
# the same experiment, because the two arms scale differently with the window: the
# readback path's CPU cost is the INTERNAL resolution and is independent of the window,
# while the swapchain arm's blit destination IS the window. Say which window size a
# present number was measured at, the same way part 53 has to say which internal
# resolution (gotcha 348's shape, one variable over).
#
# Usage:  tools/part54_present_cost.sh <tag> [1|2]        # resolution scale
#         SECS=300 tools/part54_present_cost.sh p54_2x 2
#         WINSIZE=2560x1417 tools/part54_present_cost.sh p54_big 2
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part54}"
TAG="${1:-present}"
SCALE="${2:-1}"
SECS="${SECS:-330}"
ROAM_SECS="${ROAM_SECS:-60}"
SAMPLE_SECS="${SAMPLE_SECS:-60}"
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
envv=(CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
      CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE
      CZ_VK_PROFILE=15 "CZ_VK_RES_SCALE=$SCALE")
if [ -n "${WINSIZE:-}" ]; then
    envv+=("CZ_WINDOW_RESIZE_AT=20:${WINSIZE/x/x}")
fi
# ENVX="VAR=1" is how an ARM is added without a second script -- the control for a
# present-path change has to be the same binary, the same route and the same event gate.
if [ -n "${ENVX:-}" ]; then for kv in $ENVX; do envv+=("$kv"); done; fi

echo "=== $TAG (scale ${SCALE}x${ENVX:+, $ENVX}) $(date +%H:%M:%S)"
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

# Gate on the EVENT, not a clock (gotcha 75): this boot's depth in fixed wall time has
# always been a distribution, and a fixed sleep would sample a different place each run.
jumped=0
for _ in $(seq 1 150); do
    kill -0 "$PID" 2>/dev/null || break
    grep -aq "requested DebugJump through frontend manager" "$LOG" 2>/dev/null && { jumped=1; break; }
    sleep 2
done
[ "$jumped" = 1 ] || echo "    !! the DebugJump was never serviced -- this sample is NOT outdoors"
echo "    outdoors at $(date +%H:%M:%S); roaming ${ROAM_SECS}s before sampling"
sleep "$ROAM_SECS"

python3 "$ROOT/tools/part50_thread_cpu.py" "$SAMPLE_SECS" > "$OUT/$TAG.threadcpu" 2>&1

kill "$PID" 2>/dev/null
wait $RUNNER 2>/dev/null

echo
echo "--- [vkprof] windows (readback is the pump's share of the present copy) ---"
grep -a "^\[vkprof\] " "$LOG" | tail -8
echo "--- per-thread CPU ---"
tail -25 "$OUT/$TAG.threadcpu"
