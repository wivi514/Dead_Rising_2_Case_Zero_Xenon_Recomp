#!/bin/bash
# Part 117 gates on the two-core pump (CZ_PUMP_SPLIT=1) — every standing gate the part-116
# close ran, PLUS the barrier gate, because the split moves every vkCmd* from one thread to
# another and "it records the same commands" is a claim until synchronization validation
# has read 0 with the poison reading 30 (gotcha 30).
#
# Runs one thing at a time (two game processes would contaminate each other's numbers and
# the gates themselves), prints one line per gate, and exits non-zero if any failed.
#
# Usage:  ARM="CZ_PUMP_SPLIT=1 CZ_WAITANY_WAKE=1" tools/part117_gates.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
ARM="${ARM:-CZ_PUMP_SPLIT=1 CZ_WAITANY_WAKE=1}"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part117/gates}"
mkdir -p "$OUT"
FAIL=0
bad() { printf '!!! FAIL: %s\n' "$*"; FAIL=1; }
echo "=== part 117 gates, arm: $ARM  ($(date +%H:%M))"

echo "--- 1. part47_gates.sh (smoke, switches, shader dims, PM4 oracles, E3 picture, no translated shader)"
env $ARM tools/part47_gates.sh > "$OUT/part47.out" 2>&1
tail -3 "$OUT/part47.out"
grep -q "ALL GATES CLEAN" "$OUT/part47.out" || bad "part47_gates.sh"

echo "--- 2. A5 kernel-call diff (boot to the title, renderer on, split on)"
( cd runtime/build && env $ARM CZ_NO_WINDOW=1 CZ_VKDRAW=1 timeout 150 ./cz_runtime > "$OUT/a5.log" 2>&1 )
python3 tools/kernel_call_diff.py --xenia "Xenia logs/A5_highfreq_boot/cz_run5.log" \
    --ours "$OUT/a5.log" --include-high-frequency > "$OUT/a5.diff" 2>&1
A5RC=$?
tail -2 "$OUT/a5.diff"
[ "$A5RC" = 0 ] || bad "A5 exit $A5RC"
n=$(grep -c "no translated shader" "$OUT/a5.log"); [ "$n" = 0 ] || bad "A5 run: no translated shader x$n"

echo "--- 3. synchronization validation on the outdoor route (0 hazards), then the poison (30)"
env $ARM CZ_VK_SYNC_VALIDATION=1 CZ_VK_RES=1280x720 PRESSMS=9000 SECS=45 TIMEOUT=420 \
    OUT="$OUT/sync" tools/autoroute.sh syncsplit > "$OUT/sync.out" 2>&1
SL=$(ls -t "$OUT"/sync/*.log 2>/dev/null | head -1)
hz=$(grep -ac "SYNC-HAZARD" "$SL" 2>/dev/null); hz=${hz:-0}
peak=$(grep -a "^\[fps\]" "$SL" | grep -aoE "draws med [0-9]+" | awk '{if($3>m)m=$3} END{print m+0}')
echo "    hazards: $hz   peak draws med: $peak   ($SL)"
[ "$hz" = 0 ] || bad "sync validation: $hz hazards"
env $ARM CZ_VK_SYNC_VALIDATION=1 CZ_VK_BARRIER_POISON=1 CZ_VK_RES=1280x720 PRESSMS=9000 SECS=45 TIMEOUT=420 \
    OUT="$OUT/syncpoison" tools/autoroute.sh syncpoison > "$OUT/syncpoison.out" 2>&1
PL=$(ls -t "$OUT"/syncpoison/*.log 2>/dev/null | head -1)
phz=$(grep -ac "SYNC-HAZARD" "$PL" 2>/dev/null); phz=${phz:-0}
echo "    poison hazards: $phz   ($PL)"
[ "$phz" -ge 20 ] || bad "sync validation poison control produced $phz hazards (expected ~30): the gate is not watching"

echo "--- 4. truncated=0 on the crowd-route logs of this part (all arms)"
tr=$(grep -ah "truncated=" "$HOME"/DR2CZ-troubleshooting/part117/crowd_*.log 2>/dev/null | grep -avc "truncated=0" )
echo "    ring-trace lines with truncated!=0: ${tr:-0}"

echo "--- 5. explorer soak, 10 min, split on, CZ_WAIT_TRACE=1"
( cd runtime/build && env $ARM CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE CZ_WAIT_TRACE=1 CZ_FPS_LOG=30 \
    timeout 600 ./cz_runtime > "$OUT/soak.log" 2>&1 )
grep -aE "WAITJUMP|EXPLORER engaged|changed the state away|pressing B" "$OUT/soak.log" | head -6
grep -a "guest fault\|CORRUPT STREAM" "$OUT/soak.log" | head -3
grep -aq "guest fault\|CORRUPT STREAM" "$OUT/soak.log" && bad "soak: a fault"
grep -a "^\[fps\]" "$OUT/soak.log" | tail -2 | cut -c1-200
grep -a "^\[split\] per" "$OUT/soak.log" | tail -1
n=$(grep -c "no translated shader" "$OUT/soak.log"); [ "$n" = 0 ] || bad "soak: no translated shader x$n"

printf '\n'
if [ "$FAIL" = 0 ]; then echo "PART 117 GATES CLEAN ($(date +%H:%M))"; else echo "PART 117 GATES FAILED ($(date +%H:%M))"; fi
exit "$FAIL"
