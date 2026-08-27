#!/bin/bash
# PART 80 — THE DEBUGJUMP PROBE. Find a spawn that lands in a BIGGER CROWD.
#
# WHY THIS EXISTS. Every autonomous performance run in this project goes through
# `tools/autoroute.sh`, which selects **Case 0-2** on the shipped DebugJump screen because
# that is the entry the operator named in part 72. It reaches ~6,200 draws a frame. The
# operator's own play reaches **9,000-12,000**, and `part80-kickoff.md` §1 says a CPU item
# must be measured at 8,000+ draws or not at all — because below that this route is
# GPU-bound and a CPU saving reads as a dead null (§6dw §3, gotchas 453 and 466).
#
# So the route itself is the blocker, and the operator offered to look for a better entry:
# *"I'll try to look if there is a better debug jump that could make you spawn in a crowd."*
# This launcher exists to make that search CHEAP TO READ AFTERWARDS. It is not a measurement
# run — it is a survey, and what it has to produce is a NAME plus a DRAW COUNT.
#
# WHAT IT ARMS, and why each one is nearly free:
#   CZ_DEBUG_MENU=1     the shipped DebugJump screen (F2 at the title) — the whole point
#   CZ_FPS_LOG=3        a [fps] window every 3 s rather than the usual 10, because the
#                       operator will be moving between places quickly and a 10-second
#                       window would average two different locations into one number
#   CZ_VK_FRAME_TRACE   one line a frame, so the draw count has a TIME AXIS and a spawn
#                       can be located afterwards even if nobody wrote down when it was
#   CZ_SHADER_DUMP      free, and any new area is exactly where a missing shader lives
#
# DELIBERATELY NOT ARMED: `CZ_VK_PROFILE` (2-4 ms a frame and it inverts the regime,
# gotcha 454) and `CZ_VK_GPU_PASSES` (free, but this run is not about the GPU). A survey
# that costs frame rate would make every draw count it reports a fact about the instrument.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part80-debugjump}"
TAG="${TAG:-probe}"
mkdir -p "$OUT"
for p in $(pgrep -x cz_runtime 2>/dev/null); do echo "!! cz_runtime already running (pid $p); refusing"; exit 2; done
STAMP="$(date +%m%d_%H%M%S)"
LOG="$OUT/dj_${STAMP}_${TAG}.log"
TRACE="$OUT/dj_${STAMP}_${TAG}.trace"

cat <<EOF
===================================================================
 PART 80 — DEBUGJUMP SURVEY.  Looking for a spawn IN A CROWD.
===================================================================
  WHY: the autonomous route (Case 0-2) tops out at ~6,200 draws a frame and is
       GPU-bound there, so a CPU change measures as ZERO on it. Your own play
       runs at 9,000-12,000 draws, which is where the next item lives. If some
       other DebugJump entry spawns into a crowd, I can measure it myself
       instead of needing you for every arm.

  HOW:  at the TITLE screen press  F2  to open the DebugJump screen.
        Try the entries. After each one, stand still a moment so the frame
        window settles, then F2/back out and try the next.

  WHAT I NEED BACK: which entry names put you in the thickest crowd. The log
        records the draw count every 3 seconds with a timestamp, so I can match
        a name to a number afterwards -- but only you can see the names.

  A GOOD ONE IS >= 8,000 draws sustained. Case 0-2 gives ~6,200, so anything
  near that is not an improvement.

  F9 screenshots if you want to show me a place.  Quit normally when done.

  log:   $LOG
  trace: $TRACE
===================================================================
EOF

( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_FPS_CAP=500 CZ_FPS_LOG=3 \
    CZ_DEBUG_MENU=1 \
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
echo "  shaders the cache lacked: $(grep -ac 'no translated shader' "$LOG")"
echo
echo "  --- DRAW COUNT OVER TIME (3 s windows) — find the peaks, they are the crowds ---"
awk '/^\[fps\]/{
        t += 3
        if (match($0, /draws med [0-9]+ \([0-9]+\.\.[0-9]+\)/)) {
            s = substr($0, RSTART, RLENGTH)
            if (match($0, /median \([0-9.]+ ms\)/)) ms = substr($0, RSTART+8, RLENGTH-9)
            printf "  %5ds  %-32s  %s\n", t, s, ms
        }
     }' "$LOG"
echo
echo "  --- PEAK ---"
grep -a "^\[fps\]" "$LOG" | grep -aoE "draws med [0-9]+" | awk '{if($3>m)m=$3}END{print "  best sustained draws med: " m+0 "   (Case 0-2 is ~6,200; >=8,000 is a better route)"}'
