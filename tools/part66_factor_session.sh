#!/bin/bash
# Part 66, session 3 — READ THE FACTOR IMAGE ITSELF. Five short arms, and you do not
# have to judge ANY of them by eye: each one prints a histogram of our own shadow-factor
# image to the log and the script prints them all at the end.
#
# WHY THIS REPLACES THE LADDER. Every instrument route (b) has had reads the factor
# THROUGH the 126 patched shaders and then through the title's own lighting, where the
# whole range between "fully lit" and "fully shadowed" is about a tenth of the frame's
# luma. Three sessions went into interpreting a faint answer. The factor is OUR image —
# reading it directly splits the problem exactly in half:
#
#     mostly LIT  -> the rays are not hitting anything. Fault is the TLAS or the ray.
#     mostly SHADOWED -> the factor is right; the fault is downstream, in the injection.
#
# ARM A IS THE INSTRUMENT'S OWN POSITIVE CONTROL and it gates the rest. Poison writes
# all-shadow unconditionally, so its histogram MUST read ~100% shadowed. If it does not,
# the readback is lying and nothing after it means anything (gotcha 30: a test that has
# never failed has not been shown capable of failing).
#
# What each arm must print:
#   A  poison       shadowed ~100%          <- gates everything below
#   B  stripes X    shadowed ~50%, and the PICTURE is eight vertical bands
#   C  stripes Y    shadowed ~50%, and the PICTURE is eight horizontal bands.
#                   B and C are ONE result: part 65's control was horizontal-only and
#                   passed for three sessions while every row was 427 px out of place.
#   D  primary hit  THE GATE. shadowed = the share of screen the rays found. Anything
#                   near 0% means the rays miss; the world should be most of the frame.
#   E  real factor  what the shadow term actually is. Compare with D.
#
# ~30 s each: be outdoors and standing still, look around a little, then quit. Quitting
# one starts the next. You do NOT need to press F9 and you do NOT need to judge arms A,
# D or E by eye — only B and C have a picture worth your opinion.
#
# Usage:  tools/part66_factor_session.sh          # all five
#         START=3 tools/part66_factor_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part66-factor"
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

[ "$S" -le 1 ] && run poison  "INSTRUMENT CONTROL. Must print shadowed ~100%." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_POISON=1
[ "$S" -le 2 ] && run stripeX "eight VERTICAL bands; must print shadowed ~50%." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=14
[ "$S" -le 3 ] && run stripeY "eight HORIZONTAL bands, same spacing; shadowed ~50%." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=19
[ "$S" -le 4 ] && run hit17   "THE GATE. shadowed = the share of screen the rays found." \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_FACTOR_DEBUG=17
[ "$S" -le 5 ] && run real    "the shipped path; what the shadow term actually is." \
    CZ_VK_RT_SHADOWS=1

echo "==================================================================="
echo "  EVERY READING, all arms:"
for f in "$OUT"/*.log; do
    [ -e "$f" ] || continue
    printf '  %-10s ' "$(basename "$f" .log)"
    grep -a "FACTOR IMAGE" "$f" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //' || echo "(none)"
done
echo "  PGMs and captures are in $OUT/"
