#!/bin/bash
# THE A/B FOR PART 78 ITEM 1 — what the ALL_COMMANDS image barriers were costing.
#
# THE TWO ARMS ARE ONE BINARY AND ONE VARIABLE:
#
#   fix   the shipping behaviour — each image barrier's stage and access masks are derived
#         from the layouts it moves between (`LayoutMasks`).
#   ctl   CZ_VK_WIDE_BARRIERS=1 — every barrier goes back to
#         `ALL_COMMANDS -> ALL_COMMANDS` with `MEMORY_READ | MEMORY_WRITE`, i.e. this
#         renderer through part 77.
#
# WHY THIS ITEM EXISTS AT ALL: part 78's per-region GPU split (`CZ_VK_GPU_PASSES=1`)
# measured 137.6 layout transitions a frame costing 0.930 ms of an 8.49 ms GPU frame —
# 11.0% — because this renderer oscillates the same two full-size EDRAM images between
# attachment and transfer layouts 49 times a frame. Nothing had ever priced them.
#
# WHICH READER, AND WHY IT IS NOT `part75_ab_report.py`. A barrier is on EVERY presented
# frame, menu included, so the menu window is not a control channel for this change — it is
# a second measurement of it (gotcha 452). `part76_band.py` prints both and asserts
# neither. **The primary evidence is the GPU-side split**, which is what the change acts
# on; the wall-clock band table says how much of that reaches the frame, and on this route
# the frame is GPU-bound (§6dr), so it should reach most of it.
#
# THE METHOD is `docs/part76-kickoff.md` §5: `CZ_VK_RES` pinned in both arms, runs
# ALTERNATED so a drifting machine drifts through both, the route gate read on a finished
# log, and a NULL comparison of two same-arm runs quoted beside the real one.
#
# `CZ_VK_GPU_PASSES=1` is carried in BOTH arms. It is an instrument with a bill, so it must
# be either in both or in neither, and having it in both is what makes the split itself
# comparable — which is the number this item is actually about.
#
# Usage:  tools/part78_barrier_ab.sh [N]        # N runs per arm, default 3
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part78-ab}"
RES="${RES:-3440x1440}"
SECS="${SECS:-60}"
PRESSMS="${PRESSMS:-5000}"
N="${1:-3}"
mkdir -p "$OUT"

# A RUN THAT FAILED THE ROUTE GATE IS RENAMED, NOT MERELY REPORTED. The first pass of this
# A/B printed "DID NOT REACH THE OUTDOOR WORLD" for one of six runs and then globbed its log
# into the comparison anyway, where it contributed 26 extra MENU windows to one arm and none
# to the other. `part76-kickoff.md` §5 says a failed run is dropped BY NAME; a message on the
# terminal is not a drop.
one() {   # one() <tag> [extra env ...]
    local tag="$1"; shift
    local rc=0
    OUT="$OUT" SECS="$SECS" PRESSMS="$PRESSMS" \
        "$ROOT/tools/autoroute.sh" "$tag" \
        "CZ_VK_RES=$RES" CZ_VK_GPU_PASSES=1 "$@" || rc=$?
    echo "  [gate rc=$rc]"
    if [ "$rc" != 0 ]; then
        local f
        f=$(ls -t "$OUT"/auto_*_"$tag".log 2>/dev/null | head -1)
        [ -n "$f" ] && mv "$f" "$f.rejected" &&
            echo "  ** rejected by the route gate -> $(basename "$f").rejected"
    fi
}

for i in $(seq 1 "$N"); do
    one "p78bar_fix$i"
    one "p78bar_ctl$i" CZ_VK_WIDE_BARRIERS=1
done

echo
echo "=== ENGAGEMENT: the barrier form each run actually used (gotcha 151)"
for f in "$OUT"/auto_*_p78bar_*.log; do
    [ -e "$f" ] || continue
    printf '  %s\n' "$(basename "$f")"
    grep -a "^\[vk\]   image barriers: " "$f" | tail -1 | sed 's/^/    /'
done

echo
echo "=== THE GPU-SIDE SPLIT — the quantity this change acts on, per run"
for f in "$OUT"/auto_*_p78bar_*.log; do
    [ -e "$f" ] || continue
    printf '  %s\n' "$(basename "$f")"
    grep -a "GPU per-region split\|pass-begin barriers\|resolve barriers" "$f" | tail -3 |
        sed 's/^/    /'
done

echo
echo "=== A/B (part76_band.py: a per-frame change, so the menu is a SECOND MEASUREMENT)"
python3 "$ROOT/tools/part76_band.py" \
    "fix=$OUT/auto_*_p78bar_fix*.log" "ctl=$OUT/auto_*_p78bar_ctl*.log"

echo
echo "=== NULL — two runs of the SAME arm. Everything below this line is the floor."
python3 "$ROOT/tools/part76_band.py" \
    "fixA=$OUT/auto_*_p78bar_fix1.log" "fixB=$OUT/auto_*_p78bar_fix3.log"
