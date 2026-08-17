#!/bin/bash
# Part 52: do the shipped items shorten the frame? One pinned binary, two arms and a null.
#
# WHY THIS EXISTS RATHER THAN AN A/B PER ITEM. Part 52's items are each worth 0.3-2.3 ms
# of an ~18-20 ms frame, and this route's frame-time noise floor is 8-18% by draw band at
# one run a side (part 50 §6cg). An item worth under ~1 ms is INVISIBLE in frame time here
# and has to be settled on a per-unit statistic instead — for item 1.0 that is
# `BindShader`'s share of the pump thread in a `perf` profile, which is decisive in one
# run an arm because it measures the quantity being removed rather than its effect.
#
# So the division of labour is:
#   * MECHANISM, per item, one run an arm: `tools/part52_recon.sh` with `ENVX=<control>`.
#     That is where an item is proved to have engaged (gotcha 151).
#   * FRAME TIME, once, for the items TOGETHER: this script. Pooling them is not
#     sloppiness, it is the only way the sum clears the floor.
#
# THE ARMS, one binary, environment only:
#
#   base    everything part 52 shipped, on
#   noitem  CZ_PM4_NO_SHADER_MEMO=1 — the pre-part-52 shader path restored. This is the
#           control arm, and it is the OLD CONFIGURATION RUN NOW rather than a number
#           remembered from before the change (gotchas 50/51/86).
#   null    `base` again. An arm that CANNOT move the statistic measures the floor, and
#           every effect below is quoted as a multiple of it (gotcha 331). Without it a
#           7% difference is unreadable, because 7% is what this route does on its own.
#
# Note the control arm can only restore the ONE item that has a runtime switch. The
# `Count` -> `COUNT` conversion (item 2.1) has no arm by construction — it is a container
# choice at 10 call sites — so it rides in both arms and this campaign under-reports the
# part's total by whatever that item is worth. Said out loud because a campaign that
# silently measures less than it claims is the defect this project keeps finding in its
# own instruments.
#
# Usage:  tools/part52_item_campaign.sh [runs]      # 3 runs ≈ 50 min
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part52/frame}"
mkdir -p "$OUT"

# A PINNED binary, copied once. Rebuilding mid-campaign is the classic way two arms stop
# being two states of one program (the A/B admissibility rule in CLAUDE.md).
BIN=cz_runtime_p52
if [ ! -x "$ROOT/runtime/build/$BIN" ]; then
    cp "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($(cd "$ROOT" && git rev-parse --short HEAD))"
fi

# /proc/PID/comm by prefix — `pgrep -f` matches this script's own launcher and `pgrep -x`
# cannot see a name past 15 characters. Both traps cost a part each.
busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

# THE CAP IS RAISED IN EVERY ARM, AND THAT IS THE POINT.
#
# Part 51's tick change plus part 52's items took the headless outdoor route to the 60 fps
# CAP for most of its length — 61.8 fps at 4,135 draws and 57.3 at 5,796 with BOTH
# instruments running. A capped frame cannot report a CPU saving: both arms sit on 16.2 ms
# and the A/B reads zero whatever the change was worth. That is not a null result, it is
# an unmeasurable one, and quoting it as "no change" would be the same error as reading a
# mean off this title's vblank floor (gotcha 237).
#
# So the campaign runs at CZ_FPS_CAP=120 in EVERY arm, which lifts the ceiling above the
# work without changing anything else. The number it produces is therefore the CPU
# SAVING, not the frame rate a player sees — at the shipped 60 fps cap on this route the
# honest answer for most bands is "already at the cap, the saving buys headroom rather
# than frames". Say which of the two is being quoted, every time.
#
# The player-facing question is answered on the OPERATOR's machine and route, which is
# heavier than this one, and by the symbol shares — not here.
CAP="${CAP:-120}"
declare -A ARMS=( [noitem]="CZ_PM4_NO_SHADER_MEMO=1" )
ORDER=(base noitem null)
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE
RUNS="${1:-3}"
SECS="${SECS:-330}"

for i in $(seq 1 "$RUNS"); do
  # Alternated rather than blocked, so a machine that warms or throttles over the hour
  # does not become one arm's advantage.
  for arm in "${ORDER[@]}"; do
    tag="${arm}_$i"
    [ -f "$OUT/$tag.done" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $tag $(date +%H:%M:%S)"
    envv=(CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ" "CZ_FPS_CAP=$CAP"
          "CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    [ -n "${ARMS[$arm]:-}" ] && envv+=(${ARMS[$arm]})
    ( cd "$ROOT/runtime/build" && \
      env "${envv[@]}" timeout "$SECS" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    # A growing file read mid-run is a complete file that ends early (gotcha 339), so the
    # driver writes its own completion marker and the reader checks THAT.
    if [ ! -s "$OUT/$tag.stats" ]; then
        echo "    !! $tag produced NO frame stats — see $tag.log"; continue
    fi
    echo "COMPLETE $(date +%s)" > "$OUT/$tag.done"
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats")
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))"
  done
done
echo "campaign done; artifacts in $OUT"
echo "read with: python3 tools/frame_perf_bins.py $OUT/base_*.stats -- $OUT/noitem_*.stats"
