#!/bin/bash
# Part 66, session 5 — IS ANYTHING WORLD-LOCKED, AND IS THE SCENE OCCLUDED AT ALL?
#
# THE OPERATOR'S OBSERVATION IS THE REASON THIS EXISTS: "the ray might be tracing but it
# ain't doing it with the right behaviour — in all of them the shadow moved with me and
# the camera and nothing else produced shadow except what I would suppose is a skyline
# shadow." Every arm so far validated HIT-OR-MISS. None of them validated that the hit
# POSITION or the SUN DIRECTION live in the same world the geometry does — a correct
# skyline silhouette proves the camera ray is right in the tracer's space and says
# nothing about the sun vector, which comes from a different matrix entirely.
#
#   A  checker   a 4-unit CHECKERBOARD in world space, on the real hit points. This arm
#                has never validly run — under the old depth route it was fed a cleared
#                buffer. PASS = a grid PAINTED ON THE GROUND that stays PINNED where it
#                is as you walk and turn. FAIL = it swims with the camera, or the squares
#                are absurdly large / invisible, and our "world positions" are not world
#                positions. THIS IS THE ARM THAT ANSWERS YOUR OBSERVATION DIRECTLY, and
#                it is the one that wants your eye — walk a few steps and turn.
#
#   B  ao        occlusion over the upper hemisphere, EIGHT FIXED directions, the sun not
#                involved at all. Self-reading; ignore the picture. It splits the three
#                surviving explanations:
#                  shadowed HIGH here, ~0 along the sun -> the scene has occluders and
#                      the SUN DIRECTION is wrong
#                  shadowed LOW here too -> the TLAS has no occluders above its
#                      receivers; the collector's dyn/alpha filters dropped the cars,
#                      fences and containers that cast every shadow in that yard
#                  shadowed ~0 -> rays from a receiver hit nothing in ANY direction and
#                      every earlier arm was self-intersection
#
# ~30 s each, outdoors. For arm A, walk and turn — that is the whole measurement.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part66-world"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null); do busy="$busy $p"; done
if [ -n "$busy" ]; then
    echo "!! cz_runtime already running (pid$busy); quit it first."; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
S="${START:-1}"

run() {
    local tag="$1" desc="$2"; shift 2
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM $tag        $desc"
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 \
        "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        CZ_VK_RT_FACTOR_READBACK=60 \
        "CZ_VK_RT_FACTOR_PGM=$OUT/$tag" \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    # An arm that never launched must not read as an arm that showed nothing.
    if [ "$(wc -c < "$OUT/$tag.log")" -lt 4096 ]; then
        echo "  !! ARM $tag DID NOT RUN — log is $(wc -c < "$OUT/$tag.log") bytes:"
        sed 's/^/     /' "$OUT/$tag.log"; echo "  !! stopping."; exit 3
    fi
    local n; n=$(grep -ac "FACTOR IMAGE" "$OUT/$tag.log")
    if [ "$n" -eq 0 ]; then
        echo "  !! ARM $tag produced NO factor readback at all — the pass never ran."
        grep -a "\[rtb\]" "$OUT/$tag.log" | tail -3 | sed 's/^/     /'
    else
        echo "  ARM $tag done — $n readings, last:"
        grep -a "FACTOR IMAGE" "$OUT/$tag.log" | tail -1 | sed 's/^/    /'
    fi
    echo
}

[ "$S" -le 1 ] && run checker "WALK AND TURN. Does the grid stay PINNED to the ground?" \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=2
[ "$S" -le 2 ] && run ao      "hemisphere occlusion, sun not involved. Self-reading." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=20

echo "==================================================================="
echo "  EVERY READING, all arms:"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-10s ' "$(basename "$f" .log)"
    grep -a "FACTOR IMAGE" "$f" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //' || echo "(none)"
done
echo "  PGMs and captures are in $OUT/"
