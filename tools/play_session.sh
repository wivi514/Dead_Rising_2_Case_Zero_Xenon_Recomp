#!/bin/bash
# A PLAY session — no profiler, no frame stats, no debug menu, and the cap raised out of
# the way. The operator asked for this after seeing `CZ_FPS_CAP=120` in a measurement run.
#
# WHAT "UNCAPPED" MEANS HERE, because it is not what it sounds like. There is deliberately
# no uncapped mode: `CZ_FPS_CAP` moves the vblank PERIOD with the title's own present
# interval pinned at 2, and interval 0 (present immediately) was tried and overflows the
# flip queue in 10 runs out of 10 (`gpu/vd.cpp`). The knob takes 20..500, and 500 gives a
# 1 ms period and a 2 ms ceiling — 500 fps, which nothing in this game approaches, so it
# never binds. That is as close to uncapped as this runtime goes.
#
# THE COST OF THE TOP SETTING, said out loud: the period is also the guest's vblank ISR
# cadence, so 500 fires it 1000 times a second against 125 at the 60 fps default. It
# measured 0.0% of the pump at a 4 ms period and is untested at 1 ms. If the game feels
# WORSE rather than better, that is the first thing to suspect -- fall back with
# FPS=250 (2 ms period) or FPS=120 (4 ms), which is what the measurement runs used.
#
# THE ONE INSTRUMENT LEFT IS `CZ_FPS_LOG`, and it is here because every other frame-rate
# instrument this project owns has a bill big enough to change the answer -- 2-4 ms for
# `CZ_VK_PROFILE`, 1.9-3.3 for `CZ_VK_FRAME_STATS` (gotcha 337). This is one counter and
# one clock read per presented frame, about one part in a million of a frame, and it
# reports the MEDIAN as well as the mean because a mean on this title measures the pacing
# floor rather than the change (gotcha 237). Without it a play session produces no number
# at all and "it fares well" cannot be checked.
#
# F9 still writes a screenshot -- it does not need the debug menu, only CZ_CAPTURE_KEY --
# so a picture defect can still be captured where it is seen.
#
# Usage:  tools/play_session.sh            # the top setting
#         FPS=250 tools/play_session.sh    # fall back a rung
#         PLAIN=1 tools/play_session.sh    # the a2m foliage cache off, i.e. stock shaders
#         RES=2560x1440 tools/play_session.sh   # internal resolution scale
#         NOSWAP=1 tools/play_session.sh          # the pre-part-54 readback present
#         tools/play_session.sh CZ_VK_CONST_GATHER=1   # trailing KEY=VALUE pairs pass
#                                                      # straight through, same convention
#                                                      # as tools/autoroute.sh
#
# RES is an INTEGER multiple of the title's own 1280x720 and nothing else -- 2560x1440,
# 3840x2160, 5120x2880. The guest's geometry, viewports and scissors are its own numbers
# and are untouched; what scales is the rasterisation target it draws into, so the same
# triangles are sampled at more points. The window stays where it is and the bigger image
# is filtered down into it, which is supersampling -- drag the window bigger, or maximise
# it, to see the resolution on screen instead of only in the sampling.
#
# It costs the present readback SQUARED in bytes: 3.5 MB/frame at 1x, 14.7 at 2x. Read
# `readback` in CZ_VK_PROFILE before blaming anything else for a frame time here.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/play"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

TAG="play_$(date +%m%d_%H%M)"
FPS="${FPS:-500}"
mkdir -p "$OUT/$TAG"

# The alpha-to-mask foliage cache is the configuration the operator CHOSE in part 46, not
# a debug arm, so it rides in a play session. PLAIN=1 takes it off.
extra=()
[ -n "${RES:-}" ] && extra+=("CZ_VK_RES=$RES")
# SWAP=1 -- present through a real Vulkan swapchain (part 54, plan §7) instead of reading
# the frame back into host memory and handing SDL a texture. Measured at -8.3% of the frame
# at 720p and -31.4% at 1440p, and it is in a PLAY session because what it is missing is a
# human verdict: the numbers and the E3 correlation both say it is right, and neither of
# them can say how it FEELS. The one thing it costs is the host-rendered F4 debug overlay,
# which this session does not enable anyway; the title's own F2 screen is the game's and is
# unaffected.
# NOSWAP=1 restores the readback present path -- the swapchain became the DEFAULT at the
# operator's decision at the close of part 54, so the knob that needs to exist is the one
# that takes it OFF.
[ -n "${NOSWAP:-}" ] && extra+=(CZ_VK_NO_SWAPCHAIN=1)

# SAFE=1 — god mode, no death sequence, and ZOMBIES IGNORE ALL HUMANS, held on by the pump.
#
# The operator asked for this while trying to take captures: "need you to launch it with
# zombie ignore human so I can take capture without dying". It is the same argument the
# part-54 soak harness makes and it applies to CAPTURE work at least as strongly — lining up
# a shot means standing still in a place full of zombies, which is exactly where Chuck dies,
# and a zombie GRAB rotates the camera, which for an F8 burst destroys the draw-list half of
# the answer.
#
# `CZ_DEBUG_FLAGS` names entries in the title's own debug-bool table by label and the pump
# re-asserts them every frame, because the game clears them on a level load. That is why
# this does not need the F4 menu and cannot silently lapse mid-session.
#
# NOT the default: a play session is meant to be the game as it ships, and the whole point
# of the operator's picture verdicts is that they are judgements about what a player sees.
# This is for capture and measurement sessions, and it announces itself below.
SAFE_FLAGS="${FLAGS:-CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS}"
if [ -n "${SAFE:-}" ]; then
    extra+=(CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=$SAFE_FLAGS")
fi
if [ -z "${PLAIN:-}" ]; then
    extra+=("CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1)
fi

echo "==================================================================="
echo "  PLAY SESSION — no profiler, no frame stats, no debug menu"
echo "  cap:  CZ_FPS_CAP=$FPS   (vblank period $((1000/(2*FPS))) ms, ceiling $((1000/FPS)) ms)"
echo "  res:  ${RES:-1280x720}"
if [ -n "${NOSWAP:-}" ]; then
    echo "  swap: CZ_VK_NO_SWAPCHAIN=1 -- the readback present path (the pre-part-54 arm)"
else
    echo "  swap: swapchain present (the default since part 54; NOSWAP=1 is the arm)"
fi
echo "  fps:  one line every 10 s, mean AND median"
if [ -n "${SAFE:-}" ]; then
    echo "  safe: $SAFE_FLAGS"
    echo "        (held on by the pump every frame -- the game clears them on a level load)"
fi
echo "  F8 :  BURST — every frame for 1 s -> $OUT/$TAG  (for a FLICKER; stand still)"
echo "  F9 :  screenshot -> $OUT/$TAG"
echo "  log:  $OUT/$TAG.log"
echo "==================================================================="

( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 \
    "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=10 \
    "CZ_CAPTURE_KEY=$OUT/$TAG" \
    "CZ_BURST_DUMP=$OUT/$TAG" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    "${extra[@]}" "$@" \
    ./cz_runtime > "$OUT/$TAG.log" 2>&1 )

echo
echo "  finished. frame rate over the session:"
grep -a "^\[fps\]" "$OUT/$TAG.log" | tail -40
echo
echo "  bursts recorded:          $(grep -ac 'burst .* DONE' "$OUT/$TAG.log")  ($(ls "$OUT/$TAG"/burst*_*.ppm 2>/dev/null | wc -l) frames)"
echo "  read a burst with:        python3 tools/burst_read.py $OUT/$TAG"
echo "  shaders the cache lacked: $(grep -ac 'no translated shader' "$OUT/$TAG.log")"
echo "  slot mix-ups:             $(grep -ac 'PARALLEL GUARD SLOT MIX-UP' "$OUT/$TAG.log")"
