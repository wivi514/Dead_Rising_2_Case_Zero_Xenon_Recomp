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
#   tools/autoroute.sh null                         # then again — that pair IS the floor
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part72-auto}"
TAG="${1:?usage: autoroute.sh <tag> [ENV=VAL ...]}"; shift || true
SECS="${SECS:-90}"          # gameplay time AFTER arrival, including the turn block
FPS="${FPS:-500}"
mkdir -p "$OUT"
for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_auto 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing"; exit 2
done
BIN=cz_runtime_auto
cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
STAMP="$(date +%m%d_%H%M%S)"
LOG="$OUT/auto_${STAMP}_${TAG}.log"

# The press sequence. Each entry is one 8 s interval (CZ_FAKE_PRESS_SEQ's own cadence), so
# the turn block is built by repeating stick entries rather than by a timer — and it
# ALTERNATES right and left, because the operator said "right or left" and a single
# direction would spin the camera through the same arc repeatedly instead of sweeping the
# scene both ways.
SEQ="F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE"
turns=$(( (SECS + 7) / 8 ))
for i in $(seq 1 "$turns"); do
    if [ $(( i % 2 )) -eq 1 ]; then SEQ="$SEQ,RSRIGHT"; else SEQ="$SEQ,RSLEFT"; fi
done
SEQ="$SEQ,NONE"

echo "=== $TAG  ($HEAD)  ${turns}x8s of camera turning  -> $LOG"
( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=10 \
    CZ_DEBUG_MENU=1 "CZ_FAKE_START_MS=8000" "CZ_FAKE_PRESS_SEQ=$SEQ" \
    "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
    "$@" timeout $((360 + SECS)) "./$BIN" > "$LOG" 2>&1 )

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
