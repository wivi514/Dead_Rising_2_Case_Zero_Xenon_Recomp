#!/bin/bash
# Part 47 operator session — the PERFORMANCE work, on the only machine and route that
# can judge it.
#
# THE ONE QUESTION: is the game faster where the operator plays, and does it still look
# right?
#
# TWO ARMS, CHAINED. Quit the first and the second starts. Arm A is the part-47 default;
# arm B turns off exactly the three part-47 changes and nothing else, so it is the
# pre-part-47 renderer in the SAME BINARY:
#
#   CZ_VK_TEX_GUARD_EVERY_FETCH=1  the content guard on every fetch, not once a frame
#   CZ_PM4_NO_BULK_REGS=1          the command processor's per-dword register writes
#   CZ_VK_NO_BUFFER_BIND_CACHE=1   vertex/index binds re-issued per draw
#
# WHY THE INSTRUMENTS ARE NOT OPTIONAL HERE. Part 46's first operator session shipped
# without CZ_VK_PROFILE and CZ_VK_FRAME_STATS, and its "around 20 fps" had no measurement
# behind it at all -- a whole session wasted. `docs/part47-kickoff.md` makes wiring them
# into every operator launch a standing rule, and the reason is sharper than tidiness:
# **the headless route understates the operator's draw path by about a factor of two**
# (28.7 ms at 5,241 draws against their 53.9 at 5,080), so a win measured here is not
# conservative and has to be confirmed on their configuration.
#
# A2M mode 1 and the a2m shader cache are on, since that is the tree setting their own
# A/B preferred (part 46, §6cc).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part47-operator"
mkdir -p "$OUT"

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag        $*"
    echo "  captures -> $OUT/$tag     (press F9 for one)"
    echo "  Play the same route in both arms, for roughly the same time."
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_WAIT_TRACE=1 CZ_VK_PROFILE=20 "CZ_VK_FRAME_STATS=$OUT/$tag.stats" \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_a2m" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  done. log: $OUT/$tag.log"
}

# The part-47 default first, because it is the one whose PICTURE needs judging as well as
# its speed: if a texture ever looks stale, that is the once-per-frame guard cadence and
# it is the thing to report.
run part47 CZ_NOOP=1
run pre47 CZ_VK_TEX_GUARD_EVERY_FETCH=1 CZ_PM4_NO_BULK_REGS=1 \
          CZ_VK_NO_BUFFER_BIND_CACHE=1

echo
echo "Read both with:"
echo "  grep -a '^\\[vkprof\\] [0-9]' $OUT/{part47,pre47}.log"
echo "  grep -a 'texture guard\\|binds skipped' $OUT/{part47,pre47}.log"
