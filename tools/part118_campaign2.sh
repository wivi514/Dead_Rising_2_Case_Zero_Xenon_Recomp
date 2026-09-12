#!/bin/bash
# Part 118 campaign 2: thread placement. Same-binary arms, alternated, N runs an arm,
# read with tools/part116_guestcpu.py. All arms carry CZ_HAVOK_WORKERS=4 if campaign 1
# kept it (pass HK=2 to drop it).
#   A  no pin                          (the control)
#   B  CZ_GUEST_PIN=1                  (Main + Draw on their own physical cores)
#   C  CZ_GUEST_PIN=2                  (also cz-pump and cz-draw)
# Usage: tools/part118_campaign2.sh <bin> [N]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:?usage: part118_campaign2.sh <bin> [N]}"
N="${2:-3}"
HK="${HK:-4}"
export OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part118}"
export SOAK="${SOAK:-70}" RES="${RES:-1920x1080}" FPSLOG=10
for i in $(seq 1 "$N"); do
    BIN_SRC="$BIN" "$ROOT/tools/part80_crowdroute.sh" "c2A_nopin$i" CZ_HAVOK_WORKERS=$HK
    BIN_SRC="$BIN" "$ROOT/tools/part80_crowdroute.sh" "c2B_pin1_$i" CZ_HAVOK_WORKERS=$HK CZ_GUEST_PIN=1
    BIN_SRC="$BIN" "$ROOT/tools/part80_crowdroute.sh" "c2C_pin2_$i" CZ_HAVOK_WORKERS=$HK CZ_GUEST_PIN=2
done
echo "C2 DONE"
