#!/bin/bash
# PART 80 — RECORD THE OPERATOR'S ROUTE INTO THE CROWD, so I can replay it myself.
#
# WHY. `part80-kickoff.md` §1's item 1 is parallel command recording, and its measurement
# rule is blunt: **8,000+ draws or not at all**. Below that the autonomous route is
# GPU-bound and a CPU saving reads as a dead null — part 79 spent a six-run campaign
# re-learning that (§6dw §3, gotchas 453 and 466). `tools/autoroute.sh` selects Case 0-2
# and tops out at ~6,200 draws, so every CPU item on the board is currently unmeasurable
# without an operator sitting there for every arm.
#
# The operator's survey (`part80_debugjump_probe.sh`) found DebugJump entries that spawn
# into 8,490-8,885 draws. This run records HOW they get there, precisely enough to
# transcribe into a `CZ_FAKE_PRESS_SEQ` recipe:
#
#   CZ_INPUT_TRACE=1   every pad state change, stamped in milliseconds and decoded into
#                      the recipe's own vocabulary (A, DOWN, LSUP, RSRIGHT...)
#   CZ_DEBUG_MENU=1    the DebugJump screen, and the `[debug] ... at Ns` line whose clock
#                      is the SAME epoch as the input trace — which is what lets the
#                      recipe be anchored on the screen landing rather than on boot,
#                      because boot depth is a distribution and not a constant (gotcha 75)
#   CZ_FPS_LOG=3       so the crowd shows up as a number while they are standing in it
#
# ZOMBIES IGNORE ALL HUMANS is armed at the operator's request. It is a debug tunable the
# title ships, and it is the right one for a ROUTE recording: without it the run's timing
# depends on whether Chuck gets grabbed, and a recipe transcribed from a run that had to
# fight its way through is a recipe that desynchronises the first time it does not.
# CHUCK GOD MODE and DISABLE DEATH SEQUENCE are armed for the same reason — the same three
# `tools/autoroute.sh` has used since part 72, so the replay and the recording agree.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part80-route}"
TAG="${TAG:-route}"
mkdir -p "$OUT"
for p in $(pgrep -x cz_runtime 2>/dev/null); do echo "!! cz_runtime already running (pid $p); refusing"; exit 2; done
STAMP="$(date +%m%d_%H%M%S)"
LOG="$OUT/rt_${STAMP}_${TAG}.log"
TRACE="$OUT/rt_${STAMP}_${TAG}.trace"

cat <<EOF
===================================================================
 PART 80 — RECORDING YOUR ROUTE INTO THE CROWD
===================================================================
  Armed: ZOMBIES IGNORE ALL HUMANS, CHUCK GOD MODE, DISABLE DEATH SEQUENCE
         (the same three the autonomous route uses, so a replay matches)

  Every button and stick you touch is now logged with a MILLISECOND
  timestamp and decoded into the names my replay understands. The
  DebugJump screen landing is stamped on the same clock, so I can
  anchor the recipe on THAT rather than on boot time -- boot depth
  varies from 24 s to 131 s, so anchoring on the clock alone would
  work once and never again.

  WHAT I NEED:
    1. Go to the crowd spawn you found. Take your time -- pauses are
       recorded exactly, so waiting is free and I will reproduce it.
    2. Once you are there, do whatever makes the draw count sit high:
       walk into the thick of it, turn the camera, stand still.
    3. Say out loud (or just remember) WHICH DebugJump entry you picked
       and how many DOWN presses it was -- I can see the presses, but
       not the names on the screen.
    4. Quit normally when you are happy.

  Anything simple and repeatable is better than anything clever: I have
  to be able to run this a dozen times unattended, three runs per arm.

  log:   $LOG
  trace: $TRACE
===================================================================
EOF

( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_FPS_CAP=500 CZ_FPS_LOG=3 \
    CZ_DEBUG_MENU=1 CZ_INPUT_TRACE=1 \
    "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS" \
    "CZ_VK_FRAME_TRACE=$TRACE" \
    "CZ_CAPTURE_KEY=$OUT/$TAG" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "$@" \
    ./cz_runtime > "$LOG" 2>&1 )

echo
echo "  finished.  $LOG"
echo "  internal resolution: $(grep -ao 'internal resolution [0-9x]*' "$LOG" | head -1)"
echo "  debug flags applied:"
grep -a "^\[debug\] " "$LOG" | grep -aE "GOD MODE|IGNORE ALL HUMANS|DEATH SEQUENCE" | sed 's/^/    /'
echo
echo "  --- YOUR INPUT, in order (this is what I transcribe) ---"
grep -a "^\[input\]" "$LOG" | sed 's/^/  /'
echo
echo "  --- THE ANCHOR: when the DebugJump screen actually landed ---"
grep -a "through frontend manager" "$LOG" | sed 's/^/  /'
echo
echo "  --- DRAW COUNT OVER TIME (3 s windows) ---"
awk '/^\[fps\]/{ t += 3
        if (match($0, /draws med [0-9]+ \([0-9]+\.\.[0-9]+\)/)) {
            s = substr($0, RSTART, RLENGTH)
            if (match($0, /median \([0-9.]+ ms\)/)) ms = substr($0, RSTART+8, RLENGTH-9)
            printf "  %5ds  %-32s  %s\n", t, s, ms } }' "$LOG"
echo
grep -a "^\[fps\]" "$LOG" | grep -aoE "draws med [0-9]+" | awk '{if($3>m)m=$3}END{print "  best sustained draws med: " m+0 "   (need >= 8,000 for a CPU item)"}'
