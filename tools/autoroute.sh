#!/bin/bash
# THE AUTONOMOUS ROUTE — DebugJump to Case 0-2, then 30 s of camera turning.
#
# The operator's instruction (2026-08-23): *"If it require a run just debug jump to case
# 0-2 and once you are in game move the camera to right or left for 30 second to try to
# reproduce stutter."* That is a standing authorisation to run the game myself ON THIS
# ROUTE, and this script is the only place that route is written down.
#
# WHAT THE ROUTE IS. `F2` opens the shipped DebugJump screen (needs CZ_DEBUG_MENU=1),
# `DOWN` selects **Case 0-2**, which spawns outdoors. **`WAITJUMP` is the load-bearing
# part**: the DebugJump request is HELD until the frontend exists and lands whenever it
# lands — 27 s on one boot, 131 s on another — so the barrier parks the sequence until the
# screen is actually up and starts the remaining intervals from that moment. A fixed-time
# recipe is a fit to one afternoon (gotchas 75, 251). Then a stick entry HOLDS for its
# whole interval, so RSRIGHT/RSLEFT turn the camera continuously.
#
# **IT RUNS WINDOWED, and that is not a detail.** `Host_PresentPixels` returns immediately
# when there is no window, so the `readback` phase has read 0.0% on every headless run in
# this project's history — while windowed it is 8.1-8.7% at 720p and 16.4-22.6% at 1440p.
# A headless performance run here would be measuring a renderer that does not exist.
#
# **IT IS A LIGHTER LOAD THAN THE OPERATOR'S** — ~7,431 draws against their 9,750 — so no
# number from it is a claim about their frame (gotcha 356). It is a claim about this route.
#
# Usage:
#   tools/autoroute.sh <tag> [KEY=VALUE ...]        # one arm
#   SECS=90 tools/autoroute.sh base                 # longer after arrival
#   STILL=1 tools/autoroute.sh flicker             # hold the view (SKY FLICKER hunts)
#   TIMEOUT=300 SECS=150 tools/autoroute.sh soak    # both, for a deliberately long run
#   tools/autoroute.sh null                         # then again — that pair IS the floor
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part72-auto}"
TAG="${1:?usage: autoroute.sh <tag> [ENV=VAL ...]}"; shift || true
# THE RUN'S TIME BUDGET — cut in part 74 on the operator's instruction, because the old
# shape wasted most of every run. The sequence finishes turning at ~150 s and the process
# then sat until a FIXED 450 s timeout, so **~60% of every run was a parked camera**
# (gotcha 438) — dead wall time that also contaminates any run mean with one frozen pose.
#
# The timeout is now derived from the work instead of being a constant: arrival is ~8 s of
# boot delay + the WAITJUMP wait (~27 s typical) + 7 menu entries at the 8 s interval
# = ~90 s, then the turn block, then a shutdown margin. The 8 s interval is deliberately
# NOT shortened: CZ_FAKE_START_MS is both the boot delay and the per-entry interval, and
# each entry taps for only 150 ms inside it — the DebugJump edge was a 150 ms race that
# part 54 had to fix, so squeezing the cadence trades wall time for route reliability.
#
# Windows are 5 s rather than 10 s, which recovers the sample count a shorter run would
# otherwise lose: the frame time on this route is stationary once outdoors, so more, shorter
# windows are strictly better than fewer long ones for a banded median.
# STILL=1 — STAND STILL after arrival instead of turning the camera.
#
# The turn block exists for the STUTTER hunt: the operator's authorisation was "move the
# camera to right or left for 30 second to try to reproduce stutter". For the SKY FLICKER it
# is actively harmful — their words: *"remove the move with camera to left and right because
# it just makes the flicker harder to catch; the left and right is for catching stutter"*. A
# swinging camera changes which half of the sky is bright, which is the very thing the eye
# has to watch for. So a flicker run holds the view.
STILL="${STILL:-0}"
# PRESSMS — how fast the menu is walked, separate from the boot delay since part 74. The
# DebugJump navigation is seven fixed entries and at the old 8 s cadence that was 56 seconds
# of waiting per run, most of it after the screen had already changed.
PRESSMS="${PRESSMS:-3000}"
SECS="${SECS:-60}"          # gameplay time AFTER arrival, including the turn block
TIMEOUT="${TIMEOUT:-$(( 60 + (10 * PRESSMS / 1000) + SECS ))}"   # boot + menu + play
FPSLOG="${FPSLOG:-5}"
FPS="${FPS:-500}"
mkdir -p "$OUT"
# NEVER `pgrep -f` HERE OR IN ANY WAITER AROUND THIS SCRIPT. `pgrep -f "autoroute.sh"`
# matches the WAITING SHELL's own command line, so an `until ! pgrep -f ...; do sleep; done`
# loop can never exit and three of them deadlocked in part 75. Match the process NAME
# (`pgrep -x cz_runtime_auto`) — that is the only thing that is actually the game.
for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_auto 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing"; exit 2
done
# BIN_SRC lets an OLD BINARY run this exact route — the arm part 74 needs to ask "how does
# the game fare against before the RT era". It is deliberately the only thing that changes:
# the binary is copied into this tree's runtime/build, so it resolves the SAME
# assets/shader_spv cache and the SAME assets/save/cz_settings.txt by the same relative
# paths. An arm that also swapped the shader cache or the resolution would be measuring
# three things (gotcha 415: CZ_VK_WIDE=0 was not the wide-culling arm because it also
# dropped 26% of the pixels).
BIN=cz_runtime_auto
BIN_SRC="${BIN_SRC:-$ROOT/runtime/build/cz_runtime}"
[ -x "$BIN_SRC" ] || { echo "!! no such binary: $BIN_SRC"; exit 2; }
cp -f "$BIN_SRC" "$ROOT/runtime/build/$BIN"
if [ "$BIN_SRC" = "$ROOT/runtime/build/cz_runtime" ]; then
    HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
else
    # Name the binary's OWN provenance, not this tree's HEAD, or the log lies about what ran.
    HEAD="$(cd "$(dirname "$BIN_SRC")" && git rev-parse --short HEAD 2>/dev/null || echo external) [$BIN_SRC]"
fi
STAMP="$(date +%m%d_%H%M%S)"
LOG="$OUT/auto_${STAMP}_${TAG}.log"

# The press sequence. Each entry is one 8 s interval (CZ_FAKE_PRESS_SEQ's own cadence), so
# the turn block is built by repeating stick entries rather than by a timer — and it
# ALTERNATES right and left, because the operator said "right or left" and a single
# direction would spin the camera through the same arc repeatedly instead of sweeping the
# scene both ways.
SEQ="F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE"
turns=$(( (SECS * 1000 + PRESSMS - 1) / PRESSMS ))
for i in $(seq 1 "$turns"); do
    if [ "$STILL" = "1" ]; then SEQ="$SEQ,NONE"
    elif [ $(( i % 2 )) -eq 1 ]; then SEQ="$SEQ,RSRIGHT"; else SEQ="$SEQ,RSLEFT"; fi
done
# POSTSEQ — extra press entries appended AFTER the turn block, comma separated.
#
# Added in part 76 so a GATE can drive `F9` and `F8` on this route without rewriting the
# route: `CZ_FAKE_PRESS_SEQ` is built here and here only, and a caller that overrode the
# whole variable would be testing a different journey while claiming to test this one. The
# entries land after the camera work, i.e. standing in the crowd, which is where a capture
# or a burst is worth taking anyway.
[ -n "${POSTSEQ:-}" ] && SEQ="$SEQ,$POSTSEQ"
SEQ="$SEQ,NONE"

desc=$([ "$STILL" = "1" ] && echo "STANDING STILL" || echo "camera turning")
echo "=== $TAG  ($HEAD)  ${turns}x${PRESSMS}ms $desc  -> $LOG"
( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 "CZ_FPS_CAP=$FPS" "CZ_FPS_LOG=$FPSLOG" \
    CZ_DEBUG_MENU=1 "CZ_FAKE_START_MS=8000" "CZ_FAKE_PRESS_MS=$PRESSMS" \
    "CZ_FAKE_PRESS_SEQ=$SEQ" \
    "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
    "$@" timeout "$TIMEOUT" "./$BIN" > "$LOG" 2>&1 )

# THE ROUTE'S OWN GATE. A run that never reached Case 0-2 produces a perfectly formed log
# full of title-screen frames, and its medians would be a fact about the menu. The draw
# count is what says it got there (gotcha 78: this recipe MANUFACTURES progress, so it is
# never a gate configuration — but it still has to prove it arrived).
peak=$(grep -a "^\[fps\]" "$LOG" | grep -aoE "draws med [0-9]+" | awk '{if($3>m)m=$3}END{print m+0}')
echo "  peak windowed draws med: $peak"
if [ "${peak:-0}" -lt 5000 ]; then
    echo "  ** DID NOT REACH THE OUTDOOR WORLD (need >=5000). This log is NOT reportable."
    grep -aE "WAITJUMP|requested DebugJump|EXPLORER" "$LOG" | tail -4 | sed 's/^/     /'
    exit 3
fi
grep -aE "WAITJUMP|requested DebugJump" "$LOG" | tail -2 | sed 's/^/  /'
echo "  OK"
