#!/bin/bash
# PART 79 ITEM 1's PICTURE GATE — and its positive control.
#
# WHY NOT `CZ_VK_VALIDATION=1`. §6ds §9 / gotcha 458: the guest texture heap is a bindless,
# update-after-bind descriptor array and the validation layer does not track those images'
# layouts at all. It reported clean on a build with ~1,400 textures never copied to the GPU
# and a fully black screen. It is structurally blind to exactly the class of defect a change
# to WHEN a texture image is written can produce, which is what this change is.
#
# WHAT COULD GO WRONG HERE, stated so the gate can be judged against it. The wait was what
# guaranteed that a staging segment and a command buffer were free before being reused. If
# the ring is wrong — too few slots, a fence waited on the wrong slot, a cursor that does not
# follow the slot — the symptom is a texture uploaded from bytes that the CPU has already
# overwritten with a LATER texture's. That is a wrong picture, not an API error, and no
# counter in this runtime would say so.
#
# THE ARMS, all one binary, all `CZ_VK_FRAME_STATS`:
#   fix1, fix2   the shipping ring. Their disagreement is the NULL.
#   ctl          CZ_VK_TEX_FLUSH_WAIT=1 — the pre-part-79 flush, i.e. the known-correct
#                picture. `fix` differing from `ctl` by more than the null is the failure.
#   break        CZ_VK_TEX_BATCH_BREAK=1 — the POSITIVE CONTROL. It skips the pre-submit
#                flush, so frames draw against images whose copies were never submitted.
#                §6ds measured it at 43,211x the null on coverage. A gate that cannot tell
#                this build from the working one is not a gate (gotcha 30).
#
# Usage:  tools/part79_picture_gate.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part79-picture}"
RES="${RES:-1280x720}"
SECS="${SECS:-45}"
mkdir -p "$OUT"
STAMP="${STAMP:-$(date +%m%d_%H%M%S)}"

one() {   # one() <tag> [extra env ...]
    local tag="$1"; shift
    local rc=0
    SECS="$SECS" "$ROOT/tools/autoroute.sh" "p79pic_$tag" \
        "CZ_VK_RES=$RES" "CZ_VK_FRAME_STATS=$OUT/${STAMP}_$tag.txt" "$@" || rc=$?
    echo "  [gate rc=$rc]"
    # The BREAK arm is EXPECTED to fail the route gate on draw count if it renders nothing
    # — its log is still the measurement, so it is kept rather than rejected. Every other
    # arm is dropped by name on a gate failure.
    if [ "$rc" != 0 ] && [ "$tag" != "break" ]; then
        mv "$OUT/${STAMP}_$tag.txt" "$OUT/${STAMP}_$tag.txt.rejected" 2>/dev/null
        echo "  ** rejected by the route gate"
    fi
}

one fix1
one ctl  CZ_VK_TEX_FLUSH_WAIT=1
one fix2
one break CZ_VK_TEX_BATCH_BREAK=1

echo
echo "=== NULL = the two fix runs; ARMS = the known-correct flush, and the broken one"
python3 "$ROOT/tools/frame_era_medians.py" \
    --null "$OUT/${STAMP}_fix1.txt" "$OUT/${STAMP}_fix2.txt" \
    --arm  "$OUT/${STAMP}_ctl.txt" \
    --arm  "$OUT/${STAMP}_break.txt"
