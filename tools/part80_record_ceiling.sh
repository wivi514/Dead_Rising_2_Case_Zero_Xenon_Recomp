#!/bin/bash
# PART 80 — HOW MUCH OF `record` IS THE DRIVER? The arithmetic before the work.
#
# `part80-kickoff.md` §1's item 1 is parallel command recording, its pre-registered kill is
# 1.5 ms at the operator's load, and it is the riskiest item on the board. Three plans in a
# row have priced it off a SHARE of the pump — "`DoDraw` plus the driver, ~40%" — and a
# share is not the quantity the decision turns on. A secondary-command-buffer recorder can
# only take away the time spent inside the driver's recording entry points: the pump still
# has to walk PM4, decode the fetch constants, upload the streams and look up the pipeline,
# because it has to do all of that to know WHAT to record.
#
# `record` is 29.0% of the pump and 625 ns a draw on this machine. That scope also holds the
# vertex-fetch decode, the rectangle-list expansion, the index-range arithmetic and the
# state-cache comparisons. Reading the code and estimating the split is exactly the move
# gotcha 470 charges for — part 79 sized the stream-store fix from arithmetic it had not
# done and shipped a half-fix that the next session had to replace.
#
# THE PROBE. `CZ_VK_NO_DRIVER_RECORD=1` skips every `vkCmd*` in the record path and nothing
# else. All decode, uploads, cache updates and counters still run; the frame is built
# completely and never told to the driver. The difference in `record`'s NANOSECONDS PER DRAW
# between the two arms is the ceiling — an upper bound, since a real recorder also pays for
# capture, for re-establishing state at each range boundary, and for scheduling.
#
# READ `record` AND NOTHING ELSE. The probe arm draws nothing, so its frame time is a
# statement about an empty GPU. This is a phase measurement, not an A/B.
#
# BOTH ARMS CARRY `CZ_VK_PROFILE`, which costs 2-4 ms a frame and inverts the regime
# (gotcha 454). That is fine here and only here: the quantity is a per-draw phase cost, both
# arms pay the same instrument, and the nested-scope clock reads land in `record`'s residual
# in both. It would NOT be fine for a frame-time claim.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part80-crowd}"
N="${N:-2}"
mkdir -p "$OUT"

for i in $(seq 1 "$N"); do
  for arm in null skip; do
    extra=()
    [ "$arm" = skip ] && extra=(CZ_VK_NO_DRIVER_RECORD=1)
    # ALTERNATED, not blocked: a machine that warms up or a background job that starts
    # halfway through would otherwise land entirely on one arm.
    SOAK=45 tools/part80_crowdroute.sh "ceil_${arm}$i" CZ_VK_PROFILE=10 "${extra[@]}" \
      || echo "  RUN ${arm}$i REJECTED (not counted)"
  done
done

echo
echo "=== record, NANOSECONDS PER DRAW — the only column this probe can speak to ==="
for arm in null skip; do
  echo "--- $arm"
  for f in "$OUT"/crowd_*_ceil_${arm}*.log; do
    [ -e "$f" ] || continue
    # The LAST profiler window of the run: the earlier ones include the menu and the walk,
    # and the item is priced at the crowd. Same window choice in both arms.
    grep -a "^\[vkprof\] record " "$f" | tail -1 | sed "s|^|  $(basename "$f"): |"
  done
done
echo
echo "  ceiling = (null ns/draw) - (skip ns/draw), times the draw count you intend to"
echo "  measure at. At the operator's 9,300 draws, multiply by 9.3 to get microseconds."
echo "  Their GPU headroom is 3.06 ms and the item's pre-registered kill is 1.5 ms."
