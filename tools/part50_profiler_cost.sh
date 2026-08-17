#!/bin/bash
# Part 50: WHAT DOES `CZ_VK_PROFILE` ITSELF COST? Three runs with it off, to be compared
# against the campaign's three `base` runs, which had it on.
#
# WHY THIS EXISTS. Part 50 found that a `ProfScope` reads the clock twice and that the
# reads land in the residual of the scope AROUND them — so `other`'s 206 ns/draw residual,
# which `docs/perf-plan-part50.md` §4 calls "the highest-yield-per-hour item in the
# document", may be the profiler measuring itself. That would make it not an item at all.
#
# But the consequence is larger than one item, and it is the reason this is its own
# script. EVERY number in that plan's budget — 28.3 ms at 5,000-7,000 draws, 35.7 fps,
# the whole per-draw table — was read out of a run with `CZ_VK_PROFILE` set, because that
# is the only way to get the phase split. If the profiler costs a measurable share of the
# frame, then the frame the operator actually plays is FASTER than the one this project
# has been quoting, and the target is closer than it looks.
#
# THE MEASUREMENT IS POSSIBLE ONLY BECAUSE `CZ_VK_FRAME_STATS` IS INDEPENDENT of
# `CZ_VK_PROFILE`: the per-frame `msec` column exists in both arms, so frame time can be
# compared between a profiled run and an unprofiled one. Nothing else in the report can —
# a phase share obviously cannot be read from an arm that does not compute it.
#
# Read with tools/part47_perf_read.py over the campaign directory, which bands by draw
# count; `noprof_*` and `base_*` are the two arms.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part50/campaign}"
mkdir -p "$OUT"

BIN=cz_runtime_p50   # the SAME pinned binary the campaign used, or this is two variables
if [ ! -x "$ROOT/runtime/build/$BIN" ]; then
    echo "!! $BIN is missing — it is the campaign's pinned binary and this arm must use it"
    exit 2
fi

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE
RUNS="${1:-3}"
SECS="${SECS:-330}"
for i in $(seq 1 "$RUNS"); do
    tag="noprof_$i"
    [ -f "$OUT/$tag.stats" ] && { echo "skip $tag (already present)"; continue; }
    echo "=== $tag $(date +%H:%M:%S)"
    ( cd "$ROOT/runtime/build" && \
      env CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER \
          CZ_FAKE_START_MS=8000 "CZ_FAKE_PRESS_SEQ=$SEQ" \
          "CZ_VK_FRAME_STATS=$OUT/$tag.stats" \
          timeout "$SECS" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats" 2>/dev/null)
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))"
done
echo "done; compare noprof_* against base_* in $OUT"
