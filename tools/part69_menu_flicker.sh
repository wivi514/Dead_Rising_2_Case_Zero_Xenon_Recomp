#!/bin/bash
# Part 69 follow-up — IS THE MAIN-MENU ZOMBIE FLICKER OURS? Two 20-second arms.
#
# THE REPORT: "zombie flicker from showing to not showing but only in main menu".
#
# WHY THIS IS NOT OBVIOUSLY PART 69's, AND WHY IT MIGHT BE
# --------------------------------------------------------
# Everything part 69 added lives inside `rtshadow` and is gated on `Active()`, which is
# false with RT off — except ONE thing. `PersistUsage()` adds
# VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR to the CROSS-FRAME
# STREAM STORE, the buffer the RASTER path draws every world mesh out of, and it does so
# whenever the device took the RT path — including in the menu with RT shadows off.
#
# A usage flag can change a buffer's memory requirements, so the first check was free: the
# store reports `memory type 3, heap 1` in both the part-68 and part-69 binaries, so the
# flag did NOT move it. That weakens the hypothesis without killing it, and the honest way
# to settle it is the arm below rather than more reading.
#
# THE ARMS — both boot to the main menu and sit there. No gameplay.
#   1 og   CZ_VK_RT=0. The device never takes the RT path, so `rtEnabled` is false and
#          EVERYTHING part 69 added is inert, INCLUDING the usage flag. This is the
#          tightest same-binary control that exists for this part.
#   2 rt   the default. Same twenty seconds at the same menu.
#
# READ IT AS: flicker in BOTH -> not ours, it is a pre-existing defect that wants its own
# sighting (there is already an unresolved "decal flicker" carry-over). Flicker in arm 2
# ONLY -> it is ours and `rtEnabled` is implicated, which is a very small search.
#
# Please watch the zombies in the menu backdrop for ~20 s in each arm and say which arms
# flickered. Quitting one starts the next.
#
# Usage:  tools/part69_menu_flicker.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part69-menu-flicker"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null); do busy="$busy $p"; done
if [ -n "$busy" ]; then
    echo "!! cz_runtime already running (pid$busy); quit it first."; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"

run() {
    local tag="$1" desc="$2" want="$3"; shift 3
    echo "==================================================================="
    echo "  ARM $tag        $desc"
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 \
        "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    if [ "$(wc -c < "$OUT/$tag.log")" -lt 4096 ]; then
        echo "  !! ARM $tag DID NOT RUN — log is $(wc -c < "$OUT/$tag.log") bytes:"
        sed 's/^/     /' "$OUT/$tag.log"; exit 3
    fi
    # THE ENGAGEMENT CHECK, and it exists because the first version of this script did
    # not have one. `CZ_VK_RT=0` was written into the arm's DESCRIPTION and never passed
    # to `env`, so the control ran with the RT device fully enabled and would have been
    # read as a control. An arm that cannot be shown to have engaged is not an arm
    # (gotcha 151) — and the sibling of this trap is already named in
    # part69_rt_geometry_session.sh's own header (gotcha 396), which is what makes
    # falling into it twice worth a hard failure rather than a printed line.
    if ! grep -qa "$want" "$OUT/$tag.log"; then
        echo "  !! ARM $tag DID NOT ENGAGE — expected a log line matching:"
        echo "     $want"
        echo "     what the log says instead:"
        grep -a "^\[vk\] RT" "$OUT/$tag.log" | head -3 | sed 's/^/       /'
        echo "  !! stopping rather than reporting a control that is not one."
        exit 4
    fi
    printf '    ENGAGED: '
    grep -a "^\[vk\] RT" "$OUT/$tag.log" | tail -1 | cut -c1-92
    printf '    store:   '
    grep -a "cross-frame stream store" "$OUT/$tag.log" | tail -1 | cut -c1-72
    echo
}

# arg 1 = tag, 2 = description, 3 = the log line that PROVES the arm engaged, rest = env
run og "the RT device is never created. Everything part 69 added is inert." \
    "RT: supported but OFF" CZ_VK_RT=0
run rt "the default, RT shadows on. Same menu, same twenty seconds." \
    "RT: device created WITH ray query" CZ_VK_RT_SHADOWS=1

echo "==================================================================="
echo "  Which arms flickered? BOTH = not ours. Arm 2 only = ours, and the"
echo "  search is small — rtEnabled gates one thing outside the RT code."
echo "  Logs in $OUT/"
