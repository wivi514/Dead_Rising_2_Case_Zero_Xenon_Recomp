#!/bin/bash
# Part 51 operator session — the pump's SLEEP, on the only machine that can price it.
#
# THE ONE QUESTION: is the game faster, and does anything about the pacing feel worse?
# The second half matters here in a way it did not for parts 47 and 48. Those made the
# renderer's WORK smaller, which cannot change when anything happens; this changes how
# often the command processor looks at the ring, and pacing is something a player feels
# before any counter shows it. So the thing to ask for is not only "is it faster" but
# **"is it smoother or more uneven than the other arm"** — stutter, hitching, input lag.
#
# WHAT WAS MEASURED HEADLESSLY, so the operator's report can agree or disagree with
# something specific rather than with a hope (§6ch §4):
#
#   * the pump sleeps a fixed 1 ms before every ring walk, and the walk stops ~3.1 times
#     a frame whatever the tick is, so the tick sets only how long each of those three
#     stops lasts;
#   * 87-100% of those sleeps are immediately followed by a walk that finds real work,
#     while the title's Draw Thread spins at 93% of a core on the read pointer only that
#     walk advances;
#   * at 100 us the measured latency bound falls 3.17 -> 0.47 ms/frame, and the positive
#     control at 4 ms puts it at 12.17 — linear in the tick, as the model says.
#
# TWO ARMS, CHAINED: quit the first and the second starts. Arm A is part 51's tick; arm B
# is the shipped 1 ms, in the SAME BINARY, so this is a same-binary A/B and not a
# comparison against a remembered afternoon (gotchas 50/51/86).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part51-operator"
mkdir -p "$OUT"

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag        $*"
    echo "  captures -> $OUT/$tag     (press F9 for one)"
    echo "  Play the SAME route in both arms, for roughly the same time, and"
    echo "  finish at the gas station you have named as the worst spot."
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_VK_PROFILE=20 "CZ_VK_FRAME_STATS=$OUT/$tag.stats" \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_a2m" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  done. log: $OUT/$tag.log"
}

# Part 51's tick first, because it is the arm whose FEEL needs judging as well as its
# frame time, and a first impression is worth more than a second one.
run tick100 CZ_PM4_TICK_US=100
echo
echo "  Arm A finished. Starting arm B (the shipped 1 ms tick)."
echo
run tick1000 CZ_PM4_TICK_US=1000

cat <<'EOF'

Both arms recorded. Read them with:
  tools/part47_perf_read.py ~/DR2CZ-troubleshooting/part51-operator tick100 tick1000
  tools/part51_tick_read.py ~/DR2CZ-troubleshooting/part51-operator

Ask the operator, in this order:
  1. Which arm was FASTER?
  2. Which arm was SMOOTHER — and were there hitches in either that the other lacked?
  3. Anything different about how the controls felt?
Question 2 is the one this change could plausibly fail, and no counter here can answer it.
EOF
