#!/bin/bash
# PART 77's A/B: the image-memory pool, measured on the TEXTURE frames it can reach.
#
# WHY THIS SCRIPT AND NOT `part75_ab_report.py` OR `part76_band.py`. Both of those band the
# frame by DRAW COUNT, which is the right axis for a throughput item. This is a HITCH item:
# the change removes ~145 us of `vkAllocateMemory` per texture upload and does **nothing at
# all** on a frame that uploads none. Banding by draws would dilute a 100 ms effect on 3% of
# frames into a rounding error on the median of the other 97% — which is how a real fix
# reads as zero (gotcha 452: classify the change, then pick the reader).
#
# So the population is FRAMES THAT UPLOADED A TEXTURE (`texUploads > 0` in the frame trace),
# and the statistics are the ones a hitch is felt as: the run's worst frame, the p99, and
# the total decode time the run spent. `texDecUs` is in the trace UNCONDITIONALLY — it is a
# plain counter, not a ProfScope — so this needs no `CZ_VK_PROFILE` and therefore does not
# pay the +1.5 to +5.5 ms that instrument costs (gotcha 454).
#
# The control arm is `CZ_VK_NO_TEX_MEMPOOL=1`: the same binary, one dedicated
# `vkAllocateMemory` per texture, i.e. the renderer through part 76.
#
# Usage:  tools/part77_tex_ab.sh [runs-per-arm]     (default 3)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-3}"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part77-tex}"
RES="${RES:-3440x1440}"
SECS="${SECS:-60}"
mkdir -p "$OUT"
STAMP="$(date +%m%d_%H%M%S)"
echo "=== part 77 texture A/B: $N runs an arm, ALTERNATED, res pinned $RES"
for i in $(seq 1 "$N"); do
    for arm in fix ctl; do
        tag="p77tex_${arm}_$i"
        extra=""
        [ "$arm" = "ctl" ] && extra="CZ_VK_NO_TEX_MEMPOOL=1"
        # NEVER send this to /dev/null — the route gate only speaks on stdout, and part 75
        # aggregated a control run that never left the menu because of exactly that.
        SECS="$SECS" "$ROOT/tools/autoroute.sh" "$tag" \
            "CZ_VK_RES=$RES" "CZ_VK_FRAME_TRACE=$OUT/${STAMP}_${arm}_$i.trace" $extra \
            2>&1 | tee "$OUT/${STAMP}_${arm}_$i.gate"
        rc=${PIPESTATUS[0]}
        echo "$rc" > "$OUT/${STAMP}_${arm}_$i.rc"
        # The decode attribution is printed at exit; keep it beside the trace.
        L=$(ls -t "$HOME/DR2CZ-troubleshooting/part72-auto/"*"$tag.log" | head -1)
        grep -a "decode split\|CreateImage x\|image memory:\|texture uploads over\|DECODING" \
            "$L" > "$OUT/${STAMP}_${arm}_$i.attr" || true
    done
done
echo "=== traces in $OUT (stamp $STAMP); read with tools/part77_tex_report.py"
