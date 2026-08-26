#!/bin/bash
# PART 79 ITEM 1's A/B: what the wait inside `FlushTextureUploads` was costing.
#
# THE TWO ARMS ARE ONE BINARY AND ONE VARIABLE:
#
#   fix   the shipping behaviour — the flush records into one of three upload slots,
#         submits it with that slot's fence and does NOT wait; it then advances and waits
#         only on the slot it is about to reuse, which by then retired flushes ago.
#   ctl   CZ_VK_TEX_FLUSH_WAIT=1 — the flush takes the literal pre-part-79 path,
#         `RunImmediate`: allocate a command buffer, submit, `vkQueueWaitIdle`, free.
#
# WHY THIS READER. The flush runs at most once per frame and only on a frame that uploaded
# a texture — 67 frames of ~8,000 on this route, and ~17% of frames at the operator's load
# (§6dt §3). A draw-banded median is therefore a fact about the frames the change cannot
# touch, and would report a real saving as noise (gotcha 452). `part77_tex_report.py`'s
# population is FRAMES WITH AN UPLOAD and its control channel is the frames without one,
# which this change genuinely cannot reach.
#
# THE PRIMARY EVIDENCE IS NOT THE WALL CLOCK, for the same reason part 78's was not: the
# quantity the change acts on is measured directly. `texture flush: N flushes, X ms total`
# is printed by both arms and it is the tightest number in the comparison; the frame-time
# table says how much of it reaches the frame.
#
# **AND THIS ROUTE UNDERSTATES THE ITEM BY CONSTRUCTION.** It concentrates ~2,350 uploads
# into one DebugJump load, so it pays the wait 67 times; the operator's play spreads the
# same uploads over 1,841 flushes. The per-flush number generalises, the run total does not
# (gotcha 356, and §6dt is the same shape one item earlier).
#
# THE METHOD is `docs/part76-kickoff.md` §5: `CZ_VK_RES` pinned in both arms, runs
# ALTERNATED, the route gate read on a FINISHED log and a failed run dropped BY NAME, and a
# NULL comparison of two same-arm runs quoted beside the real one.
#
# Usage:  tools/part79_flush_ab.sh [runs-per-arm]     (default 3)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-3}"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part79-flush}"
RES="${RES:-3440x1440}"
SECS="${SECS:-60}"
mkdir -p "$OUT"
STAMP="${STAMP:-$(date +%m%d_%H%M%S)}"
echo "=== part 79 flush A/B: $N runs an arm, ALTERNATED, res pinned $RES"
for i in $(seq 1 "$N"); do
    for arm in fix ctl; do
        tag="p79flush_${arm}_$i"
        extra=""
        [ "$arm" = "ctl" ] && extra="CZ_VK_TEX_FLUSH_WAIT=1"
        # NEVER send this to /dev/null — the route gate only speaks on stdout.
        SECS="$SECS" "$ROOT/tools/autoroute.sh" "$tag" \
            "CZ_VK_RES=$RES" "CZ_VK_FRAME_TRACE=$OUT/${STAMP}_${arm}_$i.trace" $extra \
            2>&1 | tee "$OUT/${STAMP}_${arm}_$i.gate"
        rc=${PIPESTATUS[0]}
        echo "$rc" > "$OUT/${STAMP}_${arm}_$i.rc"
        L=$(ls -t "$HOME/DR2CZ-troubleshooting/part72-auto/"*"$tag.log" | head -1)
        # A FAILED RUN IS DROPPED BY NAME, not merely reported (part 78's own A/B globbed
        # one in after printing that it had failed).
        if [ "$rc" != 0 ]; then
            mv "$L" "$L.rejected" && echo "  ** rejected by the route gate"
        else
            grep -a "texture upload ring\|texture flush:\|texture upload batch\|texture uploads over\|immediate submits" \
                "$L" > "$OUT/${STAMP}_${arm}_$i.attr" || true
        fi
    done
done

echo
echo "=== ENGAGEMENT: which flush path each run actually took (gotcha 151)"
for f in "$OUT/${STAMP}"_*.attr; do
    [ -e "$f" ] || continue
    printf '  %s\n' "$(basename "$f")"
    sed 's/^/    /' "$f"
done

echo
echo "=== A/B (part77_tex_report.py: population is FRAMES WITH AN UPLOAD)"
python3 "$ROOT/tools/part77_tex_report.py" "$OUT" "$STAMP"
