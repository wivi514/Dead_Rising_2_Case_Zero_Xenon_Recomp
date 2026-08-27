#!/bin/bash
# PART 81 ITEM 0's PICTURE GATE — and its positive control.
#
# WHY A PICTURE GATE AT ALL, when both steps "cannot change behaviour". Step 1 calls the
# same driver functions with the same arguments through a different pointer; step 2 hands
# the same (binding, buffer, offset) triples to the driver in fewer calls. Both of those
# are ARGUMENTS, and this project's standard is a measurement. `CZ_VK_VERIFY_BIND_BATCH=1`
# already checks the triples in-process (0 of 173.8 M disagreed), but it checks what this
# renderer BELIEVES it issued; only a picture can check what came out.
#
# WHAT COULD GO WRONG, stated so the gate can be judged against it. A wrong `firstBinding`
# lands a real stream in the wrong binding — a plausible-looking mesh built out of another
# mesh's attributes. It is a wrong picture, not an API error, and `CZ_VK_ORDER_GATE` would
# pass it: that gate hashes the pipeline and the vertex RANGE, not which buffer landed in
# which slot.
#
# THE ARMS, all one binary, all `CZ_VK_FRAME_STATS`:
#   fix1, fix2   the shipping pair. Their disagreement is the NULL.
#   ctl          CZ_VK_NO_BIND_BATCH=1 CZ_VK_NO_DEVICE_PFN=1 — the code as it is today,
#                i.e. the known-correct picture.
#   poison       CZ_VK_VERIFY_BIND_BATCH_POISON=1 — the POSITIVE CONTROL. It shifts every
#                issued vertex offset by 16 bytes, so every draw reads its attributes from
#                the wrong place. A gate that cannot tell this build from the working one
#                is not a gate (gotcha 30).
#
# STILL=1 IS THE FRAMING, and it is not optional. Part 79's picture gate first read a
# 33.7x false alarm on `meanLuma` that was pure COMPOSITION — the arms' era median draw
# counts differed by ~900 on a route whose luma ramps 28 -> 79. With the camera held, both
# control runs came inside the null on all three statistics. A null pair agreeing to 0.05%
# is one sample of a floor that is really ~1.5% (gotcha 465).
#
# Usage:  tools/part81_picture_gate.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part81-picture}"
RES="${RES:-1280x720}"
SECS="${SECS:-45}"
export STILL="${STILL:-1}"
mkdir -p "$OUT"
STAMP="${STAMP:-$(date +%m%d_%H%M%S)}"

one() {   # one() <tag> [extra env ...]
    local tag="$1"; shift
    local rc=0
    SECS="$SECS" "$ROOT/tools/autoroute.sh" "p81pic_$tag" \
        "CZ_VK_RES=$RES" "CZ_VK_FRAME_STATS=$OUT/${STAMP}_$tag.txt" "$@" || rc=$?
    echo "  [gate rc=$rc]"
    # The POISON arm may fail the route gate — it is expected to render nonsense, and its
    # log is still the measurement, so it is kept. Every other arm is dropped by name on a
    # gate failure rather than left for a glob to pick up (`part76-kickoff.md` §5).
    if [ "$rc" != 0 ] && [ "$tag" != "poison" ]; then
        mv "$OUT/${STAMP}_$tag.txt" "$OUT/${STAMP}_$tag.txt.rejected" 2>/dev/null
        echo "  ** rejected by the route gate"
    fi
}

one fix1
one ctl    CZ_VK_NO_BIND_BATCH=1 CZ_VK_NO_DEVICE_PFN=1
one fix2
one poison CZ_VK_VERIFY_BIND_BATCH_POISON=1

echo
echo "=== NULL = the two fix runs; ARMS = today's code, and the poisoned one"
python3 "$ROOT/tools/frame_era_medians.py" \
    --null "$OUT/${STAMP}_fix1.txt" "$OUT/${STAMP}_fix2.txt" \
    --arm  "$OUT/${STAMP}_ctl.txt" \
    --arm  "$OUT/${STAMP}_poison.txt"
