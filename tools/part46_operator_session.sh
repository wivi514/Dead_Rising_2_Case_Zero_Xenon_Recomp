#!/bin/bash
# Part 46 operator session: two arms, CHAINED — quitting arm 1 starts arm 2.
#
# WHY CHAINED. The operator drives; the only thing that costs them is a second trip to
# the terminal between arms, and a picture A/B is worth nothing unless both arms are
# actually played. Quitting the first window starts the second automatically.
#
# ARM 1 "default"  — the game exactly as it stands. The control.
# ARM 2 "a2m"      — the ALPHA-TO-MASK coverage arm (part 46, phase5-notes §6ca):
#                    assets/shader_spv_a2m + CZ_VK_A2M_ANY_SURFACE=1.
#                    This is a DIAGNOSTIC, not a candidate default. It supplies the
#                    coverage the tree canopies are missing, so the hard black shards
#                    should break up into feathery foliage you can see sky through —
#                    but because this title's foliage is on a 2x MSAA surface we do not
#                    sample-expand, the dither lands at PIXEL granularity, so expect
#                    visible stipple/graininess on the leaves. The question this arm
#                    asks is "are the shards gone", not "does it look better overall".
#
# Both arms carry the same instruments, all cheap:
#   CZ_SHADER_DUMP  — any run that reaches new ground captures the microcode for free.
#                     NEVER under /tmp; that is a tmpfs and it is how eleven cache
#                     entries lost their microcode.
#   CZ_CAPTURE_KEY  — F9 writes the picture, the per-draw census, the pose and every
#                     resolve snapshot of one frame, into this arm's own directory.
#   CZ_WAIT_TRACE   — names any infinite wait outlasting 5 s, in case the part-43
#                     sledgehammer freeze recurs.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part46-operator"

run () {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo
    echo "==================================================================="
    echo "  ARM: $tag        (quit the game window to move to the next arm)"
    echo "  captures -> $OUT/$tag     (press F9 for one)"
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
        CZ_CAPTURE_KEY="$OUT/$tag" \
        CZ_SHADER_DUMP="$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_WAIT_TRACE=1 \
        "$@" ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag finished. log: $OUT/$tag.log"
}

run default
run a2m CZ_SHADER_SPV="$ROOT/assets/shader_spv_a2m" CZ_VK_A2M_ANY_SURFACE=1

echo
echo "Both arms done. Captures in $OUT/{default,a2m}."
