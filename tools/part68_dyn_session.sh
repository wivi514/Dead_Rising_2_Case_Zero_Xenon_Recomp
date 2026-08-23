#!/bin/bash
# Part 68 — IS THE `dyn` LATCH EATING THE STATIC WORLD? Four arms, ~30 s each.
#
# WHAT PART 67'S SESSION ESTABLISHED, so this one is a single question
# ---------------------------------------------------------------------
# The placement fix works and is measured: the AO probe went mean 0.987 -> 0.650 and the
# shipped path 0.8% -> 14.6% shadowed, with the placement-off arms reproducing part 66's
# numbers exactly. And the placement is CORRECT — projecting our placed geometry through
# the camera hardware itself used lands it on Chuck, on every zombie, on the lamp posts
# and the kerb in hardware's own frame (tools/rt_placement_render.py).
#
# What the operator saw was therefore not a misplacement. The mode-20 dump shows the ray
# structure, rendered from the player's camera, as a FLAT PLAIN WITH DISTANT BUILDINGS —
# no vans, no wrecked cars, no fence. The primary ray sails past the foreground and lands
# on the ground behind it, and the factor computed there is painted onto the near
# object's pixels: "misaligned", "seen through walls", and world-locked, which is exactly
# what was reported.
#
# THE ONE SUSPECT LEFT is the persist store's `dynamic` latch. It is set the first time
# the guest rewrites a stream in place and it never unlatches, so a static van whose
# vertex buffer the streaming system recycled once is excluded from the ray structure for
# the rest of the run. It is 41% of everything the collector sees:
#
#     collected=14,897,391   dyn=10,532,974   alpha=3,024,035
#
# CZ_VK_RT_DYN_SETTLE=N asks the question the flag was standing in for: has this stream
# been STILL for N frames? A zombie rewritten every frame never settles. A building
# rewritten at load settles in a second — and its guard is stable by then, so its BLAS
# key is stable and it costs one build.
#
# THE ARMS
#   1 off      the shipped default. The control, and it must reproduce part 67:
#              ~14-15% shadowed, and the foreground still missing.
#   2 s120     settle = 120 frames (~2 s). THE ARM. PASS = the vans, cars and fences
#              start casting, and `settledIn=` in the log is not zero.
#   3 s30      settle = 30 frames, a shorter window. More geometry, more BLAS churn.
#   4 look     arm 2 at RT HIGH with no debug mode — YOUR EYE, and an F9 please.
#
# NOT AN ARM: CZ_VK_RT_DYN_SETTLE=0. It admits streams that change every frame, each of
# which gets a new content guard, a new BLAS key and a new BLAS — and there is no
# per-BLAS eviction, so it climbs to CZ_VK_RT_BLAS_MB and flushes everything. It exists
# for a deliberate experiment, not for a play session.
#
# Read in the log, per arm — the script prints all three:
#   settledIn=  did the arm engage at all
#   world box   x[-940 390]-ish, or the placement is not engaged and nothing counts
#   FACTOR IMAGE  the shadowed share
#
# ~30 s each, OUTDOORS, standing still, then quit. Quitting one starts the next.
#
# Usage:  tools/part68_dyn_session.sh          # all four
#         START=2 tools/part68_dyn_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part68-dyn"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null); do busy="$busy $p"; done
if [ -n "$busy" ]; then
    echo "!! cz_runtime already running (pid$busy); quit it first."; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
S="${START:-1}"

# The description is a positional parameter and never reaches `env` — GNU `env` eats any
# argument containing an `=` as an assignment, which silently killed two arms of a
# part-66 session while the harness printed "done" (gotcha 396).
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
    grep -a "settledIn" "$OUT/$tag.log" | tail -1 | sed 's/^/    /'
    echo
}

[ "$S" -le 1 ] && run off   "the shipped default. Must reproduce part 67: ~14-15% shadowed." \
    CZ_VK_RT_SHADOWS=1
[ "$S" -le 2 ] && run s120  "settle 120 frames. THE ARM: do the vans and cars start casting?" \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=120
[ "$S" -le 3 ] && run s30   "settle 30 frames. More geometry, more BLAS churn." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=30
[ "$S" -le 4 ] && run look  "settle 120 at RT HIGH, no debug. YOUR EYE, and an F9 please." \
    CZ_VK_RT_SHADOWS=3 CZ_VK_RT_DYN_SETTLE=120

echo "==================================================================="
echo "  EVERY READING, all arms:"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-8s ' "$(basename "$f" .log)"
    grep -a "FACTOR IMAGE" "$f" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //' || echo "(none)"
done
echo
echo "  ENGAGEMENT (settledIn must be non-zero in arms 2-4, and the world box a town):"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-8s ' "$(basename "$f" .log)"
    grep -a "settledIn" "$f" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' || echo "(none)"
done
echo
echo "  BLAS COST (arm 3 is the one that can run away):"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-8s ' "$(basename "$f" .log)"
    grep -a "tlasInst=" "$f" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' | cut -c1-110
done
echo "  PGMs and captures are in $OUT/"
