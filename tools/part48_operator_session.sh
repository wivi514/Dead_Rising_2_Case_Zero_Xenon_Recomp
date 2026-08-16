#!/bin/bash
# Part 48 operator session — the PERFORMANCE work, on the only machine and route that
# can judge it.
#
# THE TWO QUESTIONS, unchanged from part 47 and the second still matters most:
#   1. Is it faster where you play?
#   2. Does any texture or surface ever look STALE?
#
# THREE ARMS, CHAINED — quit one and the next starts. Play roughly the same route and
# roughly the same length of time in each, and stand in the gas-station spot you named as
# worst for frame rate in at least one of them.
#
#   part48   everything part 48 has built. This is the one whose PICTURE needs judging.
#   fold     CZ_VK_GUARD_FOLD_SERIAL=1 — undoes ONLY part 47's four-lane guard fold.
#            This is action zero: the fold is the one change that landed after your last
#            session and has never been measured on your machine. Headlessly it takes
#            `record` from 1,636 to 1,198 ns/draw; on the 81.65 MB/frame of guard hashing
#            your session showed, it predicts ~6.8 ms, i.e. 42.8 -> ~36 ms.
#   pmcount  CZ_PM4_ATOMIC_COUNTERS=1 — undoes part 48's per-thread PM4 census counters.
#            OPTIONAL, and last on purpose: it is the only item aimed at your PM4 walk,
#            which is 16.6 ms of your frame and the largest single term in it, and your
#            packet mix is not ours (144 ns/packet against our 110-113, 7.8 register
#            dwords per packet against 9.4), so the headless number does not transfer.
#            If you are short of time, quit after `fold` — the first two answer the plan.
#
# WHY THE INSTRUMENTS ARE NOT OPTIONAL. Part 46's first operator session shipped without
# CZ_VK_PROFILE and CZ_VK_FRAME_STATS and its "around 20 fps" had no measurement behind it
# at all -- a whole session wasted. The headless route understates this draw path by about
# a factor of two, so a win measured there is not the conservative direction.
#
# NEW THIS PART, and it is the reason `fold` and `pmcount` are worth your time: the
# profiler now prints the PM4 OPCODE CENSUS and a split of `other`. Nothing has ever
# described what your 16.6 ms of command-processor walk is walking -- the counters existed
# since phase 4 and were read by nothing.
#
# A2M mode 1 and the a2m shader cache are on, since that is the tree setting your own A/B
# preferred (part 46, §6cc).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part48-operator"
mkdir -p "$OUT"

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag        $*"
    echo "  captures -> $OUT/$tag     (press F9 for one)"
    echo "  Play the same route in every arm, for roughly the same time."
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

run part48 CZ_NOOP=1
run fold    CZ_VK_GUARD_FOLD_SERIAL=1
run pmcount CZ_PM4_ATOMIC_COUNTERS=1

echo
echo "Read them with:"
echo "  python3 tools/part47_perf_read.py $OUT   # frame time, medians, pinned share"
echo "  python3 tools/part48_walk_read.py $OUT   # the PM4 walk, ns per packet"
echo "  grep -a '^\\[vkprof\\] pm4 opcodes' $OUT/part48.log | tail -4   # your packet mix"
echo "  grep -a '^\\[vkprof\\] other '      $OUT/part48.log | tail -4"
