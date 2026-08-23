#!/bin/bash
# Part 67 — THE OCCLUDERS ARE PLACED. Five short arms, and four of the five need no eye
# at all: each prints a histogram of our own shadow-factor image and the script reads
# them back for you at the end.
#
# WHAT PART 67 FOUND, OFFLINE, SO THIS SESSION IS A CHECK AND NOT A SEARCH
# ------------------------------------------------------------------------
# Part 66 ended with the ray tracer exonerated end to end and the structure it traces
# "effectively a ground plane" — no direction above a receiver occluded, so no sun vector
# could ever have produced a shadow. The census found why, and it is not a filter
# throwing geometry away:
#
#     THE POSITION STREAMS ARE OBJECT-SPACE. Every mesh went into the BLAS in its own
#     local frame and every TLAS instance carried an IDENTITY transform, so the whole
#     town was piled on top of itself at the world origin.
#
# Measured against hardware's own frames (the twenty `.xtr` world traces, 46,820 accepted
# draws): untransformed, 11.7% of those meshes' boxes intersect the frustum they were
# drawn into; placed by their own world matrix, 98.6%. Every TLAS instance now carries
# that matrix.
#
# THE ARMS COME IN PAIRS, and the pair IS the result
# ---------------------------------------------------
#   1 place   hemisphere occlusion, sun deliberately EXCLUDED. Part 66 read
#             mean 0.987, 97.3% fully open. PASS = the mean falls clearly below that.
#   2 pile    the SAME arm with CZ_VK_RT_OBJ_XFORM=0 — the part-66 renderer, same
#             binary. PASS = it reproduces ~0.987 / ~97.3%. If arm 1 moved and arm 2
#             did not, the placement is what moved it and nothing else can have.
#   3 real    the shipped shadow path. Part 66 read 0.9% shadowed. PASS = clearly more.
#   4 realpile  arm 3 with the placement off. PASS = back to ~0.9%.
#   5 look    RT HIGH, no debug mode. THE ONLY ARM THAT NEEDS YOUR EYE: are there
#             shadows, are they under the things that should cast them, and do they
#             stay put when you move? Take an F9 somewhere with a building and a
#             clear patch of ground.
#
# The script also prints, per arm, the collector's own placement line —
#   [rt] ... placed=N (palette=N) declined: ... world box x[..] y[..] z[..]
# — and that world box is the engagement counter: Still Creek runs roughly
# x[-940 390] z[-720 370]. A box a couple of units across means the placement is NOT
# engaged and every reading below it is a reading of the part-66 renderer.
#
# ~30 s each: be OUTDOORS and standing still, look around a little, then quit. Quitting
# one starts the next.
#
# Usage:  tools/part67_placement_session.sh          # all five
#         START=3 tools/part67_placement_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part67-placement"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null); do busy="$busy $p"; done
if [ -n "$busy" ]; then
    echo "!! cz_runtime already running (pid$busy); quit it first."; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"
S="${START:-1}"

# NOTE the quoting on every assignment below. GNU `env` treats ANY argument containing
# `=` as an assignment, so a human description written with an `=` in it is silently
# absorbed and one written without it becomes the command — which killed two arms of a
# part-66 session while the script printed "done" (gotcha 396). The description is a
# positional parameter here and never reaches `env`.
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
    grep -a "world box" "$OUT/$tag.log" | tail -1 | sed 's/^/    /'
    echo
}

[ "$S" -le 1 ] && run place    "hemisphere occlusion, PLACED. Part 66 read mean 0.987." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=20
[ "$S" -le 2 ] && run pile     "the same, placement OFF — must reproduce 0.987." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=20 CZ_VK_RT_OBJ_XFORM=0
[ "$S" -le 3 ] && run real     "the shipped path, PLACED. Part 66 read 0.9% shadowed." \
    CZ_VK_RT_SHADOWS=1
[ "$S" -le 4 ] && run realpile "the shipped path, placement OFF — must be back to 0.9%." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_OBJ_XFORM=0
[ "$S" -le 5 ] && run look     "RT HIGH (tier 3), no debug. YOUR EYE: are the shadows under the things that cast them? F9 please." \
    CZ_VK_RT_SHADOWS=3

echo "==================================================================="
echo "  EVERY READING, all arms:"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-10s ' "$(basename "$f" .log)"
    grep -a "FACTOR IMAGE" "$f" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //' || echo "(none)"
done
echo
echo "  PLACEMENT (the engagement counter — a town-sized box, or it is not engaged):"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-10s ' "$(basename "$f" .log)"
    grep -a "world box" "$f" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' || echo "(none)"
done
echo "  PGMs and captures are in $OUT/"
