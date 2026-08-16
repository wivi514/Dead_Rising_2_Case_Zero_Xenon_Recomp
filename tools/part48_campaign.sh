#!/bin/bash
# Part 48: all three open A/Bs as ONE campaign against ONE pinned binary.
#
# WHY ONE CAMPAIGN AND NOT THREE. `part48_perf_ab.sh` runs `base` alongside its arm, so
# three separate invocations would run the baseline three times — nine wasted runs of ten
# minutes each. The baseline is the same binary with no variables set in all three
# comparisons, so it is measured once and pooled, which is exactly what part 47's driver
# comment says the base arm is for. 3 base + 3x3 arm = 12 runs, about two hours.
#
# The arms, each undoing exactly ONE default so the comparison has one variable:
#
#   atomiccounters  CZ_PM4_ATOMIC_COUNTERS=1     part 48 item 1b — the PM4 census
#                                                counters back to four `lock xadd`s a
#                                                packet. Read as ns/PACKET.
#   streamcacheclear CZ_VK_STREAM_CACHE_CLEAR=1  part 48 item 2b — the per-frame stream
#                                                cache back to clear-and-refill.
#   nobindcache     CZ_VK_NO_BUFFER_BIND_CACHE=1 part 47's vertex/index bind cache, which
#                                                is OWED: it is the one part-47 change
#                                                never A/B'd on its own, and `record`
#                                                came out ~1 ms WORSE on the part-47 arm
#                                                in both the headless and the operator's
#                                                data. If it is a loss, delete it.
#
# THE ARMS ALTERNATE WITH THE BASELINE within each round rather than running in blocks, so
# thermal drift over two hours cannot be read as an arm difference. The round is the outer
# loop for the same reason.
#
# Read with:
#   tools/part47_perf_read.py <dir>   frame time by draw bin, medians, 16 ms-pinned share
#   tools/part48_walk_read.py <dir>   the PM4 walk, ns per packet (the ONLY admissible
#                                     statistic for the walk — the arms do not submit the
#                                     same command stream)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part48/campaign}"
mkdir -p "$OUT"

BIN=cz_runtime_p48
if [ ! -x "$ROOT/runtime/build/$BIN" ]; then
    cp "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($(cd "$ROOT" && git rev-parse --short HEAD))"
fi

declare -A ARMS=(
  [atomiccounters]="CZ_PM4_ATOMIC_COUNTERS=1"
  [streamcacheclear]="CZ_VK_STREAM_CACHE_CLEAR=1"
  [nobindcache]="CZ_VK_NO_BUFFER_BIND_CACHE=1"
)
ORDER=(base atomiccounters streamcacheclear nobindcache)
SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE
RUNS="${1:-3}"

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
      env "${envv[@]}" timeout 600 "./$BIN" > "$OUT/$tag.log" 2>&1 )
    if [ ! -s "$OUT/$tag.stats" ]; then
        echo "    !! $tag produced NO frame stats -- see $tag.log"; continue
    fi
    peak=$(awk 'NR>1 && $2>m {m=$2} END {print m+0}' "$OUT/$tag.stats" 2>/dev/null)
    echo "    peak draws=$peak  frames=$(( $(wc -l < "$OUT/$tag.stats") - 1 ))"
  done
done
echo "campaign done; stats in $OUT"
