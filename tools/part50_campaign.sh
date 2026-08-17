#!/bin/bash
# Part 50: the CPU performance campaign, one pinned binary, one shared baseline.
#
# WHY A CAMPAIGN AND NOT ONE A/B PER ITEM. `docs/perf-plan-part50.md` §7 requires every
# item to have a same-binary arm, and each arm must be compared against a baseline of the
# same binary. Running them separately measures the baseline once per item, which is the
# waste part 48's driver was written to avoid — and worse, it measures it at a different
# time of day for each, so thermal drift reads as an arm difference.
#
# THE ARMS, each undoing exactly ONE default so the comparison has one variable:
#
#   nofiller   CZ_PM4_NO_FILLER_RUNS=1     item 1a — a full `ExecutePacket` call per
#                                          type-2 filler dword, which is ~30% of every
#                                          packet this title walks. Read as ns/PACKET.
#
# THE NULL CONTROL IS `base` AGAINST ITSELF. Gotcha 331: an arm that CANNOT move the
# statistic measures the floor, and part 48 believed a fake 8% for an hour without one.
# Here the honest null is not another flag — it is two runs of the SAME configuration,
# because the title's own AI drives the route and no two runs walk the same stream. Three
# base runs give three pairwise nulls, and `part50_read.sh` prints them next to the arm
# difference. An arm difference inside the base spread is not a result.
#
# THE ARMS ALTERNATE WITH THE BASELINE within each round rather than running in blocks,
# so drift over the campaign cannot be read as an arm difference.
#
# Read with:
#   tools/part48_walk_read.py <dir>   the walk, ns per packet — the ONLY admissible
#                                     statistic for it, and it checks the packet MIX
#   tools/part47_perf_read.py <dir>   frame time by draw bin (usable again since part 49)
#   tools/part48_draw_read.py <dir>   ns per draw, narrow band, --null required
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part50/campaign}"
mkdir -p "$OUT"

# A PINNED binary, copied once. The campaign takes over half an hour and this repository
# is under active edit for the whole of it; rebuilding `cz_runtime` mid-campaign would
# silently make round 3 a different program from round 1.
BIN=cz_runtime_p50
if [ ! -x "$ROOT/runtime/build/$BIN" ]; then
    cp "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($(cd "$ROOT" && git rev-parse --short HEAD))"
fi

# Guarded, because two instances at once measures contention and it has happened twice in
# two parts — the second time because a guard that existed was bypassed "just this once".
#
# Matched on /proc/PID/comm, and NOT on `pgrep -f`. Two traps meet here and this script
# fell into the second on its first outing:
#   - `pgrep -x cz_runtime` cannot see a longer snapshot name, because Linux truncates
#     `comm` to 15 characters — that cost part 49 real time, hence the prefix test;
#   - but `pgrep -f` matches the whole COMMAND LINE, which includes the shell that
#     launched this script and every agent tool call that mentions the binary. It matched
#     its own launcher and refused to run at all.
# `comm` is the executable's name and nothing else, so a shell whose arguments merely
# mention `cz_runtime` cannot be mistaken for the game.
busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"
    printf '%s' "$busy"
    exit 2
fi

declare -A ARMS=(
  [nofiller]="CZ_PM4_NO_FILLER_RUNS=1"
)
ORDER=(base nofiller)
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE
RUNS="${1:-3}"
SECS="${SECS:-330}"

for i in $(seq 1 "$RUNS"); do
  for arm in "${ORDER[@]}"; do
    tag="${arm}_$i"
    [ -f "$OUT/$tag.stats" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $tag $(date +%H:%M:%S)"
    envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ"
          CZ_VK_PROFILE=30 "CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    [ "$arm" != base ] && envv+=(${ARMS[$arm]})
    ( cd "$ROOT/runtime/build" && \
      env "${envv[@]}" timeout "$SECS" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    if [ ! -s "$OUT/$tag.stats" ]; then
        echo "    !! $tag produced NO frame stats -- see $tag.log"; continue
    fi
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats" 2>/dev/null)
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))"
  done
done
echo "campaign done; stats in $OUT"
