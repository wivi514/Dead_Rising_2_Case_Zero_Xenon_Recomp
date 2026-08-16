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
#   envpkt   CZ_PM4_ENV_PER_PACKET=1 — undoes part 48's biggest walk find: `ExecutePacket`
#            was calling `getenv` once per TYPE-3 PACKET, ~29,000 times a frame, for a
#            diagnostic that has been off in every run this project has ever measured.
#            That sits inside the PM4 walk, which is 16.6 ms of your frame and the largest
#            single term in it, and YOUR PACKET MIX IS NOT OURS (144 ns/packet against our
#            110-113, 7.8 register dwords per packet against 9.4), so the headless number
#            would not transfer even if we had one — the A/B for it was killed to free the
#            machine for this session. If you are short of time, quit after `fold`.
#
# (The per-thread PM4 census counters, part 48's item 1b, are in all three arms. They
#  measured at only ~3 ns per packet headlessly against a predicted 20-40, so they are not
#  worth an arm of your time.)
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

# REFUSE TO START IF ANOTHER RUN IS ALIVE. This cost the operator a session: a headless
# A/B run survived a kill because Linux truncates a process's `comm` to 15 characters, so
# `cz_runtime_envperpacket` appears as `cz_runtime_envp` and `pgrep -x cz_runtime` — the
# check that said "machine free" — could not match it. The operator noticed by HEARING the
# other run's audio, which is not an instrument this project can rely on.
#
# Two runs sharing the CPU is not a small error here: the whole point of an operator
# session is a real frame rate on their machine, and a contended arm reads slower for a
# reason that has nothing to do with what it is testing. Match on a PREFIX of `comm`, and
# on the truncation length, so no snapshot binary name can slip past.
others=$(pgrep -a . 2>/dev/null | awk '$2 ~ /^cz_runtime/ {print $1" "$2}')
if [ -n "$others" ]; then
    echo "!! another cz_runtime is already running -- refusing to start, because it would"
    echo "   contend for the CPU and every frame rate below would be wrong:"
    echo "$others" | sed 's/^/     /'
    echo "   kill its PROCESS GROUP (kill -9 -<pgid>), not the game: a driver script"
    echo "   restarts the next arm the moment you kill one of its runs."
    exit 2
fi

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
run envpkt  CZ_PM4_ENV_PER_PACKET=1

echo
echo "Read them with:"
echo "  python3 tools/part47_perf_read.py $OUT   # frame time, medians, pinned share"
echo "  python3 tools/part48_walk_read.py $OUT   # the PM4 walk, ns per packet"
echo "  grep -a '^\\[vkprof\\] pm4 opcodes' $OUT/part48.log | tail -4   # your packet mix"
echo "  grep -a '^\\[vkprof\\] other '      $OUT/part48.log | tail -4"
