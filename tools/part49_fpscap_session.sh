#!/bin/bash
# Part 49 operator session — THE 60 fps CAP.
#
# WHAT IS BEING ASKED. The title ships at 30 fps because its own D3D present interval is
# 2 (two 60 Hz vblanks per present). `CZ_FPS_CAP=60` selects interval 1, which is the
# value the game's OWN "vsync 1" configuration produces — a mode it already supports,
# not a defeat of its pacing. Our vblank cadence is untouched at 16 ms and the title
# still waits for exactly the interval it asked for; it just asks for one.
#
# THE TWO QUESTIONS, and the first one is the one that matters:
#
#   1. DOES THE GAME RUN AT DOUBLE SPEED? Animations, walking, zombies, the world clock,
#      the cinematics. If the engine derived its delta time from the vblank COUNT rather
#      than from the timebase, everything would run 2x and the frame rate would be a lie.
#      A headless check says it does not -- world units travelled per WALL second are
#      unchanged with the stick held forward -- but that measures Chuck walking and not
#      animation, physics or audio, so YOUR EYES ARE THE REAL CHECK HERE.
#   2. Is it actually smoother where you play, and does anything else look wrong?
#
# WHY THIS IS WORTH DOING BEYOND THE SMOOTHNESS. Part 48 put the frame ON the two-vblank
# floor: at >=5,000 draws, 4,452 frames of 12,009 sat at exactly 32 ms and another 3,570
# at 33. A CPU saving below 33 ms therefore measures as EXACTLY ZERO (gotchas 237/238),
# which is why the last three parts had to argue from phase milliseconds instead of frame
# rate. Raising the cap makes frame time a usable instrument again.
#
# TWO ARMS, CHAINED. Quit one and the next starts.
#
#   cap60   the new mode. Judge the SPEED OF THE WORLD first, the smoothness second.
#   cap30   the shipped default, same binary, for comparison. Play it second so you have
#           the 60 fps arm fresh in mind when you look at it.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part49-operator"
mkdir -p "$OUT"

# Refuse to start if another run is alive -- a leftover headless run cost a session in
# part 48, and `pgrep -x cz_runtime` could not see it because Linux truncates `comm` to
# 15 characters, so `cz_runtime_envperpacket` did not match. Match on a PREFIX.
others=$(pgrep -a . 2>/dev/null | awk '$2 ~ /^cz_runtime/ {print $1" "$2}')
if [ -n "$others" ]; then
    echo "!! another cz_runtime is already running -- refusing to start, because it would"
    echo "   contend for the CPU and every frame rate below would be wrong:"
    echo "$others" | sed 's/^/     /'
    echo "   kill its PROCESS GROUP (kill -9 -<pgid>), not the game."
    exit 2
fi

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag        $*"
    echo "  captures -> $OUT/$tag     (press F9 for one)"
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_VK_PROFILE=20 "CZ_VK_FRAME_STATS=$OUT/$tag.stats" \
        CZ_SWAPQ_TRACE=1 \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_a2m" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  done. log: $OUT/$tag.log"
}

run cap60 CZ_FPS_CAP=60
run cap30 CZ_NOOP=1

echo
echo "Read them with:"
echo "  grep -a 'fps cap' $OUT/cap60.log | head -2      # it engaged, and off what"
echo "  grep -a 'swapq:'  $OUT/cap60.log | tail -2      # due should be tick+1, not tick+2"
echo "  grep -aE '^\\[vkprof\\] [0-9]+\\.[0-9] fps' $OUT/{cap60,cap30}.log | tail"
