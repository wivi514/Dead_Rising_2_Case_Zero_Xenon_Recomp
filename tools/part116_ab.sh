#!/bin/bash
# Part 116 A/B: two BINARIES (or one binary and an env arm), three runs a side, alternated,
# on BOTH kinds of arm the guest question needs:
#   normal        the wall is the pump; read the `guest main/draw` columns
#   nodd          CZ_VK_NO_DODRAW=1 — the per-draw renderer deleted, the wall IS the guest
#                 floor (8.8 ms in part 110); read the wall
# Reader: tools/part116_guestcpu.py <A logs> -- <B logs>, per kind.
#
# Usage:
#   tools/part116_ab.sh <tagA> <binA> <tagB> <binB> [N=3] [KINDS="normal nodd"]
#   ENVB="CZ_VK_TEXMEMO=1 CZ_VK_SCOPED_SHARED_ZERO=1" tools/part116_ab.sh base BIN bundle BIN
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TA="$1"; BA="$2"; TB="$3"; BB="$4"
N="${N:-3}"
KINDS="${KINDS:-normal nodd}"
ENVA="${ENVA:-}"; ENVB="${ENVB:-}"
for i in $(seq 1 "$N"); do
    for kind in $KINDS; do
        extra=""
        [ "$kind" = nodd ] && extra="CZ_VK_NO_DODRAW=1"
        PERF=0 BIN_SRC="$BA" "$ROOT/tools/part116_probe.sh" "${TA}_${kind}${i}" $extra $ENVA
        PERF=0 BIN_SRC="$BB" "$ROOT/tools/part116_probe.sh" "${TB}_${kind}${i}" $extra $ENVB
    done
done
echo "AB DONE $TA vs $TB"
