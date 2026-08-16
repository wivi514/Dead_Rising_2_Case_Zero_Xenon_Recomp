#!/bin/bash
# Part 46 operator session 2: three arms, CHAINED — quitting one starts the next.
#
# WHAT CHANGED SINCE SESSION 1, and why. Session 1 answered the tree question (the A2M
# coverage arm removes the hard black plates) and raised a new one (it replaces them with
# a screen-door dither: canopy isolated-pixel share 0.14% -> 5.59% against hardware's
# ~0%). It also had a hole I put there: no CZ_VK_PROFILE and no CZ_VK_FRAME_STATS, so
# the operator's "around 20 fps" had no measurement behind it at all. Both are wired on
# every arm here, and every arm also prints the stream-guard SIZE HISTOGRAM that item
# 00c's fix has been waiting on for two parts.
#
# ARM 1 "a2m_mode1" — the NEW candidate. One flat alpha threshold at 0.5 for A2M draws.
#                     Gives up hardware's soft edge but puts the SILHOUETTE where a
#                     coverage mask averaged over a resolve puts it, and needs no sample
#                     grid — so no stipple. Headless on the menu tree it scores
#                     isolated 0.20% (dither 1.13%, default 0.18%, hardware 0.00%) while
#                     recovering about half the tonal gain (p05/p95 0.306 against the
#                     dither's 0.324 and hardware's 0.326).
# ARM 2 "a2m_mode2" — the dither, i.e. exactly what session 1's a2m arm did. Here so the
#                     two can be judged against each other in the same sitting rather
#                     than across sessions.
# ARM 3 "ui_guard"  — DEFAULT shaders, with the cross-frame stream store's content guard
#                     exact up to 256 KB instead of 16 KB. This is the open item 00c /
#                     00k candidate: the UI text layer is one big vertex buffer the guest
#                     sub-allocates per draw, so it lands above the old bound and a few
#                     edited glyph quads get sampled over — which is the HUD block going
#                     missing. PLAY THIS ONE LONGEST: the defect needs an accumulated
#                     session, so a short run cannot refute it.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part46-operator2"

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
        CZ_VK_PROFILE=20 \
        CZ_VK_FRAME_STATS="$OUT/$tag.stats" \
        "$@" ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag finished. log: $OUT/$tag.log"
}

A2M="$ROOT/assets/shader_spv_a2m"
run a2m_mode1 CZ_SHADER_SPV="$A2M" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1
run a2m_mode2 CZ_SHADER_SPV="$A2M" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=2
run ui_guard  CZ_VK_STREAM_GUARD_BYTES=262144

echo
echo "All three arms done. Captures in $OUT/{a2m_mode1,a2m_mode2,ui_guard}."
