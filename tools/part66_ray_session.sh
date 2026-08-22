#!/bin/bash
# Part 66, session 4 — WHICH LINK OF THE SHADOW RAY IS WRONG. Four arms, ~30 s each,
# and NONE of them needs your eye: every one prints a histogram of our factor image and
# the script prints them all at the end. Just be outdoors, standing still, then quit.
#
# WHERE SESSION 3 LEFT IT. Everything except one link is now proven:
#   poison      100.0% shadowed   the readback instrument is honest
#   stripes X   49.8%, 8 vertical bands, rows flat
#   stripes Y   50.0%, 8 horizontal bands, cols flat   -> alignment correct, BOTH axes
#   primary     85.2% shadowed, and the mask is a SKYLINE SILHOUETTE matching the frame
#                                 -> the receiver is found, upright and in the right place
#   real         0.9% shadowed    -> the ray from that receiver toward the sun hits nothing
#
# So: the TLAS has the world in it, we find the right receiving point, our image reaches
# the shaders correctly — and the occlusion ray escapes. These four arms say why.
#
#   A  down   ray fired STRAIGHT DOWN from the receiver, 10x length, sun ignored.
#             A point ON the ground firing down MUST hit the ground. Expect ~100%
#             shadowed. THIS IS THE DISCRIMINATOR: if it is high the shadow-ray path
#             works and the SUN DIRECTION is the fault; if it is ~0% the path itself is
#             broken and the direction is irrelevant.
#   B  flip   the real path with the sun NEGATED. The direction comes from the light
#             matrix z axis and is then negated on an argument about which way light
#             travels; if that argument is backwards every ray fires at the sky, which
#             is exactly what 0.9% looks like. If B is high, the sign was wrong.
#   C  long   the real path with a 2000-unit ray instead of the cascade's 116. Distinguishes
#             "the ray escapes" from "the ray is too short to reach the occluder".
#   D  unbias the real path with no origin offset at all. If the bias was lifting every
#             origin clear of its own geometry this goes heavily shadowed.
#
# Exactly one of these should move, and which one it is names the fix.
#
# Usage:  tools/part66_ray_session.sh          # all four
#         START=3 tools/part66_ray_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part66-ray"
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

[ "$S" -le 1 ] && run down   "straight down, 10x. MUST be heavily shadowed." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=7
[ "$S" -le 2 ] && run flip   "the real path with the sun NEGATED." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_SUN_FLIP=1
[ "$S" -le 3 ] && run long   "the real path with a 2000-unit ray." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_RAY_LEN=2000
[ "$S" -le 4 ] && run unbias "the real path with no origin offset." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_BIAS=0.0001 CZ_VK_RT_FACTOR_CAMBIAS=0.0001

echo "==================================================================="
echo "  EVERY READING, all arms:"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-10s ' "$(basename "$f" .log)"
    grep -a "FACTOR IMAGE" "$f" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //' || echo "(none)"
done
echo "  PGMs and captures are in $OUT/"
