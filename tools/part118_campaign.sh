#!/bin/bash
# Part 118 campaign 1: the Havok worker count and the guard's non-temporal prefetch,
# against stock, on ONE binary, alternated, N runs an arm, read with
# tools/part116_guestcpu.py (the guest's CPU per frame and the wall, matched 250-draw
# bands). Arms:
#   A  stock                      (the same-binary control)
#   B  CZ_HAVOK_WORKERS=4         (main + 4 instead of main + 2)
#   C  B + CZ_VK_GUARD_NTA=1      (the guard's sweep kept out of L2/L3)
# Usage: tools/part118_campaign.sh <bin> [N]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:?usage: part118_campaign.sh <bin> [N]}"
N="${2:-3}"
export OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part118}"
export SOAK="${SOAK:-70}" RES="${RES:-1920x1080}" FPSLOG=10
for i in $(seq 1 "$N"); do
    BIN_SRC="$BIN" "$ROOT/tools/part80_crowdroute.sh" "c1A_stock$i"
    BIN_SRC="$BIN" "$ROOT/tools/part80_crowdroute.sh" "c1B_hk4_$i" CZ_HAVOK_WORKERS=4
    BIN_SRC="$BIN" "$ROOT/tools/part80_crowdroute.sh" "c1C_hk4nta$i" CZ_HAVOK_WORKERS=4 CZ_VK_GUARD_NTA=1
done
echo "C1 DONE"
