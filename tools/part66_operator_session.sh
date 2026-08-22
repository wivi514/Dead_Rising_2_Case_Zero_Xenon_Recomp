#!/bin/bash
# Part 66 — THE GATE, THEN THE PICTURE. RT shadows, route (b), primary-ray receiver.
#
# WHAT CHANGED SINCE THE BUILD YOU LAST PLAYED, in one paragraph. Part 65 established
# that our shadow factor reaches the screen (poison darkens the world) but computes
# "lit" everywhere, and its ladder narrowed that to "everything the pass SAMPLES comes
# back empty". Part 66 found why, offline, against hardware: **this title has no scene
# Z prepass.** Walked in stream order, all twenty `.xtr` world traces show the FIRST
# draw of the scene pass already sampling the shadow atlas, with zero depth-writing
# draws before it and ~5,200 after it. So the depth buffer the pass reconstructs from
# is at its clear value every single time it runs, and no trigger could have fixed it.
# The receiver now comes from a PRIMARY RAY into the same TLAS the shadow ray uses,
# which needs no depth buffer and therefore has no "too early".
#
# HOW TO READ THIS SESSION. Arm 1 is a GATE and the rest are meaningless without it.
# Part 65 handed over three builds without a met gate and spent three of your sessions
# discovering that; this script refuses to be that again — if arm 1 does not do what it
# says, STOP, and the log plus the frame-stats file are the whole answer.
#
#   arm 0  A5 kernel gate, headless, ~90 s, no window. Nothing for you to do.
#   arm 1  GATE — "does the primary ray find the world?"  (CZ_VK_RT_FACTOR_DEBUG=17)
#          PASS = the WORLD IS BLACK and only the SKY is lit.
#          FAIL = the frame looks normal -> the rays are not hitting the scene, and
#                 nothing after this can work. Say so and stop.
#   arm 2  how FAR is each hit                            (CZ_VK_RT_FACTOR_DEBUG=18)
#          PASS = a smooth gradient, darker near the camera and brighter into the
#                 distance — a depth image made entirely of rays. It separates "the
#                 rays hit something" from "the rays hit the right thing": a TLAS full
#                 of junk at the origin still passes arm 1 and reads flat black here.
#   arm 3  THE PICTURE — the shipped path, RT LOW.
#          The questions are about SHAPE, because that is what a number cannot answer:
#            * do LIT surfaces stay lit (route (a) died by greying everything)?
#            * do shadows sit UNDER their casters, or float away from them?
#            * is there acne — speckle or stripes on surfaces that should be clean?
#            * do characters and foliage look wrong? They are NOT in the ray structure,
#              so they cast no shadow and RECEIVE the shadowing of the ground behind
#              them. Expected; I want to know how bad it looks.
#   arm 4  RT HIGH — four rays over the sun's disc, the first soft shadow either route
#          could produce. Same questions, plus: is the penumbra visible, and is the
#          five-level banding objectionable?
#   arm 5  the depth-buffer source, as the same-binary CONTROL. This is part 65's build
#          exactly. Expect NO shadows. If arm 3 shows shadows and arm 5 does not, the
#          census's finding is confirmed from the picture as well as from the traces.
#   arm 6  RT OFF — the shipped default, for the side-by-side, and the LOW-vs-HIGH
#          raster shadow LOOK verdict that has been owed since part 60.
#
# In every arm: get outdoors, F4 -> SHADOW -> the tier named, look around for ~30 s,
# F9 for a still, then quit. Each arm's quit starts the next.
#
# Usage:  tools/part66_operator_session.sh          # all arms in order
#         START=3 tools/part66_operator_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part66"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
S="${START:-0}"

# One arm. `tag` names the log, the still directory and the frame-stats file that
# tools/part65_luma_read.py reads; the extra env is per-arm.
run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM $tag        $*"
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 \
        "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_VK_FRAME_STATS=$OUT/fs_$tag.txt" \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag done."
    grep -a "\[rtb\] passes=" "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -a "factor pass fires" "$OUT/$tag.log" | sed 's/^/  /'
    echo
}

if [ "$S" -le 0 ]; then
    echo "  ARM 0 — the A5 kernel gate, headless. ~90 s, nothing to do."
    ( cd "$ROOT/runtime/build" && CZ_NO_WINDOW=1 CZ_NO_AUDIO_OUT=1 \
        timeout 90 ./cz_runtime > "$OUT/a5.log" 2>&1 )
    python3 "$ROOT/tools/kernel_call_diff.py" \
        --xenia "$ROOT/Xenia logs/A5_highfreq_boot/cz_run5.log" \
        --ours "$OUT/a5.log" --include-high-frequency | tail -12
    echo "  ARM 0 exit $? (0 = the gate holds)"
    echo
fi

[ "$S" -le 1 ] && run gate17 "GATE — PASS = BLACK WORLD, LIT SKY." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=17
[ "$S" -le 2 ] && run dist18 "PASS = a distance GRADIENT, dark near, bright far." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=18
[ "$S" -le 3 ] && run rtlow  "THE PICTURE — RT LOW. Shape questions in the header." \
    CZ_VK_RT_SHADOWS=1
[ "$S" -le 4 ] && run rthigh "RT HIGH — four rays, soft shadow." \
    CZ_VK_RT_SHADOWS=3
[ "$S" -le 5 ] && run depthsrc "CONTROL — the depth-buffer source. Expect NO shadows." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_SOURCE=depth
[ "$S" -le 6 ] && run off "RT OFF — the shipped default, plus the raster LOW-vs-HIGH look."

echo "  all arms done. Everything is in $OUT/"
echo
echo "  the luma table (arm 'off' is the control):"
python3 "$ROOT/tools/part65_luma_read.py" "$OUT" --control fs_off.txt || true
