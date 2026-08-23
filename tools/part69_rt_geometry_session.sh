#!/bin/bash
# Part 69 — THE REMIX PLAN'S ITEMS 0-3, MEASURED. Five arms; two of them need no eye.
#
# WHAT SHIPPED, AND WHAT EACH ARM IS FOR
# --------------------------------------
# `docs/rt-remix-plan.md` items 0-3, all four now in the binary and all four with a
# same-binary control arm:
#
#   item 0  the BLAS reads its vertices in place out of the persist store
#           (CZ_VK_RT_NO_DIRECT_BUFFERS=1 restores the staging copy)
#   item 1  the BLAS key stops being a function of the mesh's CONTENT
#           (CZ_VK_RT_STABLE_KEY=0 restores the part-68 key)
#   item 2  a mesh whose bytes moved is REFITTED in place instead of rebuilt
#           (CZ_VK_RT_NO_REFIT=1 restores the rebuild, and the FAST_TRACE build flags)
#   item 3  a palette-blended mesh has its blended world positions BAKED into the BLAS,
#           with the instance carrying only the outer stage
#           (CZ_VK_RT_NO_BAKE=1 restores palette entry 0 with unit weight)
#
# Item 3 is the one with a measured motivation. Part 69's census of hardware's own vertex
# data (`tools/rt_palette_census.py`, over the gas-station trace) reads ZERO of 2,786
# palette draws referencing a single matrix, median 19 distinct entries. So "entry 0" was
# not an approximation that is right for the static world and wrong for actors, which is
# what §6cx assumed: it places every batched prop on top of whichever prop happens to be
# bone 0, and that shape is 60% of the world's occluders.
#
# THE ARMS
#   0 a5      the kernel gate, OWED SINCE PART 67. Boot to the title, sit ~60 s, quit.
#             No RT, nothing to look at — it is a log for tools/kernel_call_diff.py.
#   1 dyn0    CZ_VK_RT_DYN_SETTLE=0 with everything on. THIS ARM NEEDS NO EYE: the gate
#             is `flushes=0` with `blas=` steady and `refit=` non-zero, read from the log.
#             Please just ROAM for two or three minutes — the more of the town the better.
#   2 old     the SAME configuration with items 1 and 2 backed out. The positive control:
#             this one is expected to climb and FLUSH, and if it does not then the growth
#             items 1 and 2 exist to stop was never happening and they are unpriced.
#   3 bake    the shipped default, RT LOW, factor readback. The control for the picture.
#   4 nobake  arm 3 with CZ_VK_RT_NO_BAKE=1 — palette entry 0, i.e. the part-68 renderer.
#             Stand in the SAME place as arm 3 if you can. A crowd with a building shadow
#             across it, or any street with several props batched together.
#   5 look    RT HIGH, no debug, everything on. YOUR EYE, and an F9 please.
#
# WHAT TO WATCH FOR IN ARMS 3/4/5, because a number cannot answer it: are the shadows
# under the things that cast them, and does a shadow boundary now BEND over a van's roof
# and down its side instead of running as one straight line across wall, van and ground?
#
# ~60 s each for 0-2, ~30 s for 3-5. Quitting one starts the next.
#
# Usage:  tools/part69_rt_geometry_session.sh          # all six
#         START=3 tools/part69_rt_geometry_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part69-rt-geometry"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null); do busy="$busy $p"; done
if [ -n "$busy" ]; then
    echo "!! cz_runtime already running (pid$busy); quit it first."; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
S="${START:-0}"

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
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    if [ "$(wc -c < "$OUT/$tag.log")" -lt 4096 ]; then
        echo "  !! ARM $tag DID NOT RUN — log is $(wc -c < "$OUT/$tag.log") bytes:"
        sed 's/^/     /' "$OUT/$tag.log"; echo "  !! stopping."; exit 3
    fi
    grep -a "tlasInst=" "$OUT/$tag.log" | tail -1 | cut -c1-120 | sed 's/^/    /'
    grep -a "verts direct=" "$OUT/$tag.log" | tail -1 | cut -c1-120 | sed 's/^/    /'
    grep -a "baked=" "$OUT/$tag.log" | tail -1 | cut -c1-120 | sed 's/^/    /'
    echo
}

[ "$S" -le 0 ] && run a5 "the kernel gate, owed since part 67. Title screen, ~60 s, quit." \
    CZ_VK_RT_SHADOWS=0

# --- the two log-only arms: does refit stop the growth, and was there growth to stop? ---
[ "$S" -le 1 ] && run dyn0 "settle 0, everything on. ROAM 2-3 min. Gate: flushes=0." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=0
[ "$S" -le 2 ] && run old "settle 0, items 1+2 BACKED OUT. Expected to flush. Same roam." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=0 CZ_VK_RT_STABLE_KEY=0 CZ_VK_RT_NO_REFIT=1

# --- the picture pair for item 3 -------------------------------------------------------
picture() {
    local tag="$1" desc="$2"; shift 2
    run "$tag" "$desc" CZ_VK_RT_FACTOR_READBACK=60 "CZ_VK_RT_FACTOR_PGM=$OUT/$tag" "$@"
    grep -a "FACTOR IMAGE" "$OUT/$tag.log" | tail -1 | sed 's/^/    /'
}
[ "$S" -le 3 ] && picture bake "the shipped default. The control for the picture." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=120
[ "$S" -le 4 ] && picture nobake "the same with palette ENTRY 0 — the part-68 renderer." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=120 CZ_VK_RT_NO_BAKE=1

[ "$S" -le 5 ] && run look "RT HIGH, no debug, everything on. YOUR EYE, and an F9 please." \
    CZ_VK_RT_SHADOWS=3 CZ_VK_RT_DYN_SETTLE=120

echo "==================================================================="
echo "  ITEM 2's GATE — arm dyn0 must hold flushes=0 with blas= steady;"
echo "  arm old is the positive control and is EXPECTED to flush:"
for t in dyn0 old; do
    [ -e "$OUT/$t.log" ] || continue
    printf '  %-7s ' "$t"
    grep -a "tlasInst=" "$OUT/$t.log" | tail -1 \
        | sed 's/^\[rt\] [a-zA-Z ]*: //' | cut -c1-100
    printf '          BLAS pool flush lines: %s\n' \
        "$(grep -ac 'over its cap' "$OUT/$t.log")"
done
echo
echo "  ITEM 0 and ITEM 2's engagement (direct= must dominate staged=; refit= non-zero):"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-7s ' "$(basename "$f" .log)"
    grep -a "verts direct=" "$f" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' | cut -c1-100
done
echo
echo "  ITEM 3's engagement (baked= non-zero, and outOfRange MUST be 0):"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-7s ' "$(basename "$f" .log)"
    grep -a "baked=" "$f" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' | cut -c1-100
done
echo
echo "  THE FACTOR READBACK, arm bake against arm nobake:"
for t in bake nobake; do
    [ -e "$OUT/$t.log" ] || continue
    printf '  %-7s ' "$t"
    grep -a "FACTOR IMAGE" "$OUT/$t.log" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //' \
        || echo "(none)"
done
echo
echo "  THE KERNEL GATE (owed since part 67):"
echo "    python3 tools/kernel_call_diff.py \\"
echo "        --xenia 'Xenia logs/A5_highfreq_boot/cz_run5.log' \\"
echo "        --ours '$OUT/a5.log' --include-high-frequency"
echo
echo "  PGMs and captures are in $OUT/"
