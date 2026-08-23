#!/bin/bash
# Part 68b — DOES THE STATIC WORLD KEEP ITS SHADOWS WITHOUT THE ACTORS? Three short arms.
#
# THE QUESTION, and it is a PRICE CHECK rather than a fix
# --------------------------------------------------------
# Part 67 places every occluder by its own object->world matrix, and for `palette` shaders
# — which blend several matrix entries with three PER-VERTEX weights — it uses entry 0
# with unit weight. That is right for the static world (99.5% of vertices on screen for
# the bank's busiest world shader) and wrong for a skinned actor: what enters the BLAS is
# the raw object-space mesh under ONE bone, not the pose the title drew, so the primary
# ray hits a wrong surface at roughly the right screen position.
#
# Part 68's first capture is that defect measured on the factor image itself: 100.5 edge
# pixels per 1000 in the crowd region against 10.6 on open road, isolated-pixel rate 0.35%
# and intermediate share 0.00% — localised to the actors, not acne, not noise. The
# operator's words were "when they are in front of a shadow the shadow doesn't work behind
# their silhouette, like SSR".
#
# CZ_VK_RT_NO_PALETTE=1 declines those draws. RTX Remix's answer to the same problem is to
# SKIN THEM PROPERLY on the GPU (dispatchSkinning, limitedBonesPerVertex=4) rather than
# exclude, and that is what the part-69 plan builds — see docs/rtx-remix-prior-art.md.
# So this session is not the fix. It answers the one question that decides how much the
# fix is worth:
#
#     does the STATIC world still have good shadows when the actors are out?
#
# If yes, the opaque half is sound and everything after is additive. If the world loses
# its shadows too, a lot of static geometry rides those shaders and the blend has to be
# done before anything else is worth doing.
#
# THE ARMS — 1 and 2 are the pair, and both carry the settle window so only ONE thing
# differs between them.
#   1 base    settle 120. The control.
#   2 nopal   settle 120 + palette declined. PASS = the actor-shaped holes and streaks
#             are gone AND the buildings/road still cast. `paletteArm=` must be non-zero.
#   3 look    arm 2 at RT HIGH with no debug — YOUR EYE, and an F9 please.
#
# Stand in the SAME place for arms 1 and 2 if you can — a crowd with a building shadow
# across it is the ideal spot, since that is where the artifact lives.
#
# ~30 s each, then quit; quitting one starts the next.
#
# Usage:  tools/part68_palette_session.sh          # all three
#         START=2 tools/part68_palette_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part68-palette"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null); do busy="$busy $p"; done
if [ -n "$busy" ]; then
    echo "!! cz_runtime already running (pid$busy); quit it first."; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
S="${START:-1}"

# The description is a positional parameter and never reaches `env` (gotcha 396).
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
    if [ "$(wc -c < "$OUT/$tag.log")" -lt 4096 ]; then
        echo "  !! ARM $tag DID NOT RUN — log is $(wc -c < "$OUT/$tag.log") bytes:"
        sed 's/^/     /' "$OUT/$tag.log"; echo "  !! stopping."; exit 3
    fi
    local n; n=$(grep -ac "FACTOR IMAGE" "$OUT/$tag.log")
    if [ "$n" -eq 0 ]; then
        echo "  !! ARM $tag produced NO factor readback — the pass never ran."
        grep -a "\[rtb\]" "$OUT/$tag.log" | tail -3 | sed 's/^/     /'
    else
        echo "  ARM $tag done — $n readings, last:"
        grep -a "FACTOR IMAGE" "$OUT/$tag.log" | tail -1 | sed 's/^/    /'
    fi
    grep -a "paletteArm" "$OUT/$tag.log" | tail -1 | sed 's/^/    /'
    echo
}

[ "$S" -le 1 ] && run base  "settle 120. The control — actors still in the structure." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=120
[ "$S" -le 2 ] && run nopal "settle 120, palette DECLINED. Holes gone? World still casting?" \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=120 CZ_VK_RT_NO_PALETTE=1
[ "$S" -le 3 ] && run look  "the same at RT HIGH, no debug. YOUR EYE, and an F9 please." \
    CZ_VK_RT_SHADOWS=3 CZ_VK_RT_DYN_SETTLE=120 CZ_VK_RT_NO_PALETTE=1

echo "==================================================================="
echo "  EVERY READING:"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-7s ' "$(basename "$f" .log)"
    grep -a "FACTOR IMAGE" "$f" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //' || echo "(none)"
done
echo
echo "  ENGAGEMENT (paletteArm must be non-zero in arms 2 and 3, zero in arm 1):"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-7s ' "$(basename "$f" .log)"
    grep -a "paletteArm" "$f" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' || echo "(none)"
done
echo
echo "  HOW MUCH GEOMETRY IT COSTS (tlasInst, arm 2 against arm 1):"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-7s ' "$(basename "$f" .log)"
    grep -a "tlasInst=" "$f" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' | cut -c1-95
done
echo "  PGMs and captures are in $OUT/"
