#!/bin/bash
# Part 65 — THE LADDER. Four ~40-second arms that split the factor pass into its links.
#
# WHERE THIS STANDS. The operator's verdict on the last build: "there is just 0 shadow
# except the power line in main menu, and they are not ray traced, they are a leftover
# of normal shadow". So route (b) still produces nothing visible, and the earlier
# before/after stills were NOT evidence — they were two frames whose SHADOW setting was
# unknown, which makes them inadmissible (the A/B rule in CLAUDE.md, and gotcha 386).
#
# WHAT IS ACTUALLY ESTABLISHED:
#   * the poison arm DARKENS the world -> our factor image is read by all 126 patched
#     shaders and drives the title's own shadow term. The injection is proven.
#   * with a real factor the frame is unchanged -> the factor is ~1.0 (lit) EVERYWHERE.
#
# So the fault is in the pass that COMPUTES the factor, and it has three links in
# series: the DEPTH READ, the WORLD RECONSTRUCTION, and the RAY. A frame looks
# identical under all three failures, so no amount of looking can separate them. These
# arms each turn one link into a picture you can read in five seconds.
#
# RUN THEM IN ORDER AND STOP AT THE FIRST FAILURE — everything after a broken link is
# meaningless. Each arm needs ~40 seconds: get outdoors, set SHADOW to RT LOW, look,
# F9, quit.
#
#   arm 1  depth      PASS = the WORLD GOES BLACK and only the SKY stays lit.
#                     FAIL = nothing changes -> we are not reading the scene depth.
#   arm 2  checker    PASS = a regular CHECKERBOARD painted on the ground and walls
#                     that stays PINNED to the world as you walk and turn.
#                     FAIL = no pattern at all, or one that swims with the camera, or
#                     squares so huge/tiny the screen is one flat colour
#                     -> the inverse view-projection (or the world scale) is wrong.
#   arm 3  rays       PASS = HEAVY over-shadowing, most of the world dark and filthy.
#                     That is what a shadow ray with NO bias is supposed to do — it
#                     hits the surface it started on. FAIL = still fully lit -> the
#                     rays miss regardless of bias, and the fault is the TLAS or the
#                     ray construction.
#   arm 4  normal     the shipped path again, for reference after the three above.
#
# Usage:  tools/part65_ladder_session.sh          # all four
#         START=3 tools/part65_ladder_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part65-ladder"
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

run() {
    local tag="$1"; local mode="$2"
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM $tag   (CZ_VK_RT_FACTOR_DEBUG=$mode)"
    echo "  Outdoors, F4 -> SHADOW -> RT LOW, look, F9, quit."
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 \
        CZ_VK_RT_SHADOWS=1 \
        "CZ_VK_RT_FACTOR_DEBUG=$mode" \
        "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag done."
    grep -a "\[rtb\] passes=" "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    echo
}

S="${START:-1}"
# CZ_VK_RT_SHADOWS=1 IS SET HERE ON PURPOSE, unlike the earlier sessions: these arms do
# not need the panel to be live, and pinning the tier removes "was the row on RT?" as a
# way to misread the result — which is exactly how the last two stills were misread.
[ "$S" -le 1 ] && { echo; echo "  ARM 1 — DEPTH. PASS = black world, lit sky."; run depth 1; }
[ "$S" -le 2 ] && { echo; echo "  ARM 2 — WORLD CHECKER. PASS = a grid PINNED to the world."; run checker 2; }
[ "$S" -le 3 ] && { echo; echo "  ARM 3 — UNBIASED RAYS. PASS = heavy over-shadowing."; run rays 3; }
[ "$S" -le 4 ] && { echo; echo "  ARM 4 — the shipped path, for reference."; run normal 0; }
echo "  all arms done. Everything is in $OUT/"
