#!/bin/bash
# PART 76's OPERATOR SESSION — the per-frame CPU/GPU profiler, on the operator's request.
#
# WHAT IT IS FOR, beyond "see how it feels". Part 76 took 4.15 ms of CPU and 1.66 ms of GPU
# out of every frame and the autonomous route then came out **GPU-BOUND** — GPU 10.55 ms of
# a 10.59 ms wall, fence 2.99 ms (`phase5-notes.md` §6dq §7). That route sits at 6,000-7,000
# draws and the operator plays at ~9,750. GPU cost scales with PIXELS and CPU with DRAWS, so
# the two regimes need not agree, and **which one their frame is in decides the whole of
# part 77's board.** This session answers it from their own play.
#
# DIFFERENCES FROM `part75_operator_session.sh`, which it otherwise reuses:
#
#   1. **THE DISPLAY PICK IS FIXED.** That script takes the FIRST connected output with a
#      geometry. With two monitors attached that is not necessarily the one being played on
#      — here it reads DP-1 at 3012x1694 while the primary is HDMI-A-1 at 3440x1440 — so its
#      "the desktop is not 3440x1440" warning fires on the wrong screen and its silence
#      would be equally wrong. It prefers the PRIMARY output now, and prints every output so
#      a wrong guess is visible rather than silent.
#   2. **THE INTERNAL RESOLUTION IS PINNED BY DEFAULT.** `WideMode()` is `9W > 16H` on the
#      internal resolution, so an entire renderer path exists at 21:9 and not at 16:9; a
#      resolution that arrives from the environment is a variable nobody declared (gotcha
#      447). `RES=` overrides, `RES=desktop` restores the old inherit-it behaviour.
#   3. **`CZ_CAPTURE_KEY` AND `CZ_BURST_DUMP` ARE FREE NOW.** Part 75's harness carried a
#      warning that they cost the present readback; part 76 made the readback armed by the
#      PRESS, so F8 and F9 cost nothing until they are pressed (gotcha 450).
#   4. It prints the CPU/GPU verdict at the end instead of leaving it to a later read.
#
# THE BILL, said out loud: `CZ_VK_PROFILE` is 2-4 ms a frame and `CZ_VK_FRAME_TRACE` adds a
# line of I/O per frame. **The frame rate seen here is worse than the game really is.** Judge
# SHAPE from this run — which phase is big, whether the CPU is waiting — never absolute fps.
# `tools/play_session.sh` is the run for an honest frame rate.
#
# Usage:  tools/part76_operator_session.sh                 # profiler on, god mode on
#         RES=2560x1440 tools/part76_operator_session.sh   # a different internal resolution
#         RES=desktop   tools/part76_operator_session.sh   # inherit it, un-pinned
#         NOSAFE=1      tools/part76_operator_session.sh   # die like a player
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part76-operator"
mkdir -p "$OUT"

for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_auto 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing"; exit 2
done

TAG="p76_$(date +%m%d_%H%M)"
LOG="$OUT/$TAG.log"
TRACE="$OUT/$TAG.trace"
mkdir -p "$OUT/$TAG"

# The PRIMARY output's mode, not the first one xrandr happens to name.
prim="$(xrandr 2>/dev/null | awk '/ connected primary /{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){split($i,a,"+"); print a[1]; exit}}')"
[ -z "$prim" ] && prim="$(xrandr 2>/dev/null | awk '/ connected/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){split($i,a,"+"); print a[1]; exit}}')"

RES="${RES:-3440x1440}"
extra=()
if [ "$RES" != "desktop" ]; then
    extra+=("CZ_VK_RES=$RES")
fi
# God mode and no death sequence, but NOT "zombies ignore all humans": the crowd's own
# behaviour is the load being measured, and turning the AI off would change the very thing
# this session exists to profile.
if [ -z "${NOSAFE:-}" ]; then
    extra+=(CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE")
fi

echo "==================================================================="
echo "  PART 76 SESSION — per-frame CPU/GPU profiler ARMED"
echo
xrandr 2>/dev/null | grep -w connected | sed 's/^/  display:  /'
echo "  primary:  ${prim:-unknown}"
if [ "$RES" = "desktop" ]; then
    echo "  internal: from the desktop (UN-PINNED — say which it chose when quoting a number)"
else
    echo "  internal: $RES (pinned)"
    [ "$RES" != "${prim:-}" ] && echo "            (note: that is not the primary's mode — deliberate, but say so)"
fi
echo
echo "  F7 :  MARK A STUTTER — press it the moment you feel one."
echo "        Stamped into the log AND the trace; I read backwards ~1 s from each mark."
echo "  F9 :  screenshot -> $OUT/$TAG        (free until pressed, since part 76)"
echo "  F8 :  burst, every frame for 1 s -> $OUT/$TAG"
echo
echo "  WHAT I MOST WANT FROM THIS RUN: a big crowd, held for a while."
echo "  The question is whether YOUR frame is waiting on the GPU or on the CPU at your"
echo "  draw count — the autonomous route went GPU-bound after part 76 and yours may not"
echo "  have. Standing in a heavy place beats walking through several."
echo
echo "  COST:  CZ_VK_PROFILE is 2-4 ms a frame. The game will feel slightly worse than it"
echo "         really is — read the SHAPE from this run, not the absolute fps."
echo
echo "  log:   $LOG"
echo "  trace: $TRACE"
echo "==================================================================="

( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
    CZ_VK_PROFILE=10 \
    "CZ_VK_FRAME_TRACE=$TRACE" \
    "CZ_CAPTURE_KEY=$OUT/$TAG" \
    "CZ_BURST_DUMP=$OUT/$TAG" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "${extra[@]}" "$@" \
    ./cz_runtime > "$LOG" 2>&1 )

echo
echo "  finished."
echo "  internal resolution:      $(grep -ao 'internal resolution [0-9x]*' "$LOG" | head -1)"
echo "  stutter marks (F7):       $(grep -ac '\*\* MARK ' "$LOG")"
echo "  frames traced:            $(( $(wc -l < "$TRACE" 2>/dev/null || echo 1) - 1 ))"
echo "  shaders the cache lacked: $(grep -ac 'no translated shader' "$LOG")"
echo "  slot mix-ups:             $(grep -ac 'PARALLEL GUARD SLOT MIX-UP' "$LOG")"
echo "  const memo stale:         $(grep -ac 'CONST MEMO STALE' "$LOG")"
echo "  stale present slots:      $(grep -ac 'present slot describes frame' "$LOG")"
echo
python3 "$ROOT/tools/part76_regime.py" "$TRACE" 2>/dev/null || true
echo
grep -a "^\[fps\]" "$LOG" | tail -10
