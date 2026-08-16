#!/bin/bash
# Part 46 operator session 3 — the UI fix candidate, and the trees settled.
#
# ARM 1 "fix" — the two things this session is proposing, together:
#   * the DYNAMIC-STREAM GUARD. Session 2 refuted "raise the bound": at 256 KB the HUD
#     still dropped out, while part 45's unlimited arm fixed it, which places the UI
#     buffer above 256 KB where buying exactness by SIZE costs 121+ MB/frame. So
#     exactness is now EARNED instead: a stream this store has caught changing is hashed
#     exactly from then on, everything else keeps the cheap sampled guard. Measured cost
#     on the outdoor route: 101-116 streams/frame, 0.5 MB/frame.
#   * A2M mode 1 (the flat 0.5 threshold), which the operator's own near-matched A/B put
#     ahead of the dither: isolated-pixel share 0.71% against 4.17%.
#   PLAY THIS LONGEST. The HUD defect needs an accumulated session; a short run cannot
#   refute it, and session 2's arm 3 only failed after ~2,500 frames.
#
# ARM 2 "fix_off" — the SAME binary with CZ_VK_NO_DYNAMIC_GUARD=1. The control. If the
#   HUD survives arm 1 and breaks here, the guard policy is the mechanism; if it breaks
#   in both, it is not, and that is worth knowing before anything is made default.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part46-operator3"
run () {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo; echo "==================================================================="
    echo "  ARM: $tag        (quit the game window to move to the next arm)"
    echo "  captures -> $OUT/$tag     (press F9 for one)"
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_CAPTURE_KEY="$OUT/$tag" \
        CZ_SHADER_DUMP="$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_WAIT_TRACE=1 CZ_VK_PROFILE=20 CZ_VK_FRAME_STATS="$OUT/$tag.stats" \
        CZ_SHADER_SPV="$ROOT/assets/shader_spv_a2m" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "$@" ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag finished. log: $OUT/$tag.log"
}
run fix
run fix_off CZ_VK_NO_DYNAMIC_GUARD=1
echo; echo "Both arms done. Captures in $OUT/{fix,fix_off}."
