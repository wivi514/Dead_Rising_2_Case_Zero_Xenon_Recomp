#!/bin/bash
# Part 46 operator session 4 — testing CZ_VK_GUARD_BUDGET.
#
# THE ONE QUESTION: does the UI stay good with the CHEAP variant of the guard policy?
#
# The default policy (the one confirmed in session 3: "Ui stay good the whole time")
# makes every stream it catches changing exact forever, and that cost 35-48 MB/frame in
# the operator's session and +22.7% of a mid-crowd frame. CZ_VK_GUARD_BUDGET=1 keeps
# exactness unbudgeted only for streams PROVEN to need it -- the ones the cheap sampled
# guard has actually been caught missing a change on -- and puts everything speculative
# on a per-frame toll. Headless it reads ~11-14 MB/frame against ~18.
#
# The risk it carries, stated plainly: a stream only becomes "proven" while it is being
# hashed exactly, and under a budget it may not be hashed exactly at the moment it
# changes. If that happens to the UI buffer the HUD will drop out again. THAT is what
# this session is for, and it is why the variant is not the default.
#
# A2M mode 1 is on, since that is the tree setting the operator's own A/B preferred.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part46-operator4"
mkdir -p "$OUT/budget"
echo "==================================================================="
echo "  ARM: budget   (CZ_VK_GUARD_BUDGET=1 + A2M mode 1)"
echo "  captures -> $OUT/budget     (press F9 for one)"
echo "  Watch the HUD -- especially the top-left LV/PP/LIFE/\$ block."
echo "==================================================================="
( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_CAPTURE_KEY="$OUT/budget" \
    CZ_SHADER_DUMP="$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    CZ_WAIT_TRACE=1 CZ_VK_PROFILE=20 CZ_VK_FRAME_STATS="$OUT/budget.stats" \
    CZ_SHADER_SPV="$ROOT/assets/shader_spv_a2m" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    CZ_VK_GUARD_BUDGET=1 \
    ./cz_runtime > "$OUT/budget.log" 2>&1 )
echo "  done. log: $OUT/budget.log"
