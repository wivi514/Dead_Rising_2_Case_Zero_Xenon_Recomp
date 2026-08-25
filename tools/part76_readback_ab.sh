#!/bin/bash
# THE A/B FOR PART 76 ITEM 1 — what the every-frame present readback was costing.
#
# THE TWO ARMS ARE ONE BINARY AND ONE VARIABLE. Both carry `CZ_CAPTURE_KEY` and
# `CZ_BURST_DUMP`, because that is `tools/play_session.sh`'s configuration and the whole
# item is that naming those two variables used to force a whole-frame readback on every
# frame:
#
#   fix   the shipping behaviour — the readback is armed by an F9/F8 PRESS, not by the
#         variable being set, so with no press it never runs.
#   ctl   CZ_VK_PRESENT_ALWAYS=1 — the pre-part-76 behaviour, readback every frame.
#
# NOTHING ELSE DIFFERS, and in particular no key is pressed in either arm: a capture writes
# a 15 MB PPM and would land in one arm's frame times and not the other's.
#
# THE METHOD IS `docs/part76-kickoff.md` §5, which is the list of ways part 75 got this
# wrong. In particular:
#   * `CZ_VK_RES` IS PINNED in both arms. The desktop here is 2560x1440 and the operator
#     plays at 3440x1440 — and that is not merely a pixel count, `WideMode()` is
#     `9W > 16H` so a whole renderer path exists at one and not the other. The readback
#     copies width*height*4 bytes, so this item's SIZE is the resolution.
#   * runs ALTERNATE, so a machine that drifts through the session drifts through both.
#   * the route gate is read on a finished log and a failed run is dropped BY NAME.
#   * a NULL comparison — two runs of the same arm — is reported beside the real one, so
#     the difference is quoted against the floor rather than against zero.
#
# Usage:  tools/part76_readback_ab.sh [N]        # N runs per arm, default 3
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part76-ab}"
RES="${RES:-3440x1440}"
SECS="${SECS:-60}"
# 5 s rather than autoroute's 3: the SLOWER arm polls fewer times inside the 150 ms press
# window and so misses the DebugJump more often, which would leave the control arm's
# surviving runs its luckiest ones (kickoff §5). A slower menu walk costs wall time and
# buys a comparable gate failure rate.
PRESSMS="${PRESSMS:-5000}"
N="${1:-3}"
mkdir -p "$OUT"
CAP="$OUT/capdir"; mkdir -p "$CAP"

one() {   # one() <tag> [extra env ...]
    local tag="$1"; shift
    OUT="$OUT" SECS="$SECS" PRESSMS="$PRESSMS" \
        "$ROOT/tools/autoroute.sh" "$tag" \
        "CZ_VK_RES=$RES" "CZ_CAPTURE_KEY=$CAP" "CZ_BURST_DUMP=$CAP" "$@"
    echo "  [gate rc=$?]"
}

for i in $(seq 1 "$N"); do
    one "fix$i"
    one "ctl$i" CZ_VK_PRESENT_ALWAYS=1
done

echo
echo "=== the readback counters, per run (the arm has to be shown to have engaged)"
for f in "$OUT"/auto_*_fix*.log "$OUT"/auto_*_ctl*.log; do
    [ -e "$f" ] || continue
    printf '%s\n' "  $(basename "$f")"
    grep -a "^\[vk\]   readback: " "$f" | sed 's/^/    /'
done

echo
echo "=== A/B (part75_ab_report.py: menu window as the machine-state fingerprint)"
python3 "$ROOT/tools/part75_ab_report.py" \
    "fix=$OUT/auto_*_fix*.log" "ctl=$OUT/auto_*_ctl*.log"

echo
echo "=== NULL — two runs of the SAME arm. Everything below this line is the floor."
python3 "$ROOT/tools/part75_ab_report.py" \
    "fixA=$OUT/auto_*_fix1.log" "fixB=$OUT/auto_*_fix3.log"
