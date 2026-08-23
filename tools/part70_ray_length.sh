#!/usr/bin/env bash
# PART 70 — IS THE SHADOW RAY TOO SHORT? A pre-registered sweep with a numeric gate.
#
# WHY THIS EXISTS
# ---------------
# The defect that survived parts 67-69 is a shadow boundary that runs as ONE STRAIGHT
# LINE across a shipping container, tyres, cars, a chain-link fence and the ground,
# bending at none of them, plus a horizontal cut through Chuck's chest. Part 70 closed
# the sun, part 69 showed the primary ray resolves the real world, and parts 67-69 fixed
# the occluder set. What is left is the ray itself.
#
# Reading `rt_factor.hlsl`: the shadow ray's `TMax` is `pc.sun.w`, which defaults to the
# CASCADE'S DEPTH EXTENT — 88.5 world units in the operator's last run, and hardware's
# own cascade volumes are 63.4/67.1/89.7. The town is ~1,100 units across and one
# frame's world box measured x[-610, 324] z[-681, 107]. **A ray that stops at 88 units
# cannot find an occluder 200 units away.**
#
# And the SHAPE that produces is the observed one. The set of receiver points whose
# fixed-length ray just clears a given occluder is a PLANE in world space, and a world
# plane projects to a straight line in screen space that bends at nothing it crosses —
# because the cut-off is a property of the RAY, not of the surface receiving it. A
# constant-height cut through a standing character is the same statement.
#
# `docs/part69-night-plan.md` §3 ranked the ray length THIRD, reasoning that a short ray
# would leave "everything past it lit — the opposite signature". That is the wrong
# geometry: a short ray does not fail far from the camera, it fails far from the
# OCCLUDER, and it fails along a plane. Part 67 also "exonerated" the length, but against
# a pile of geometry at the world origin, which is no test (gotcha 172).
#
# THE PRE-REGISTERED PREDICTION, so a run can refute it:
#   * the shadowed share RISES monotonically with ray length across 88 -> 3000;
#   * the straight edges in the dumped factor move outward or disappear.
# If the share is flat across a 34x range, the ray length is not the limiter and this
# hypothesis is dead — which is worth the same twenty minutes.
#
# Diagnostic only: the share is a counter and the PGM is read by a tool. Whether the
# frame LOOKS right still goes through the operator.
set -u
OUT=${1:-$HOME/DR2CZ-troubleshooting/part70-raylen}
SECS=${SECS:-240}
mkdir -p "$OUT"
[ -x runtime/build/cz_runtime ] || { echo "build first" >&2; exit 1; }
ROUTE="F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE"

for L in 0 300 1000 3000; do
    tag="len$L"
    [ "$L" = 0 ] && tag="lendefault"
    mkdir -p "$OUT/$tag"
    echo "=== $tag  (CZ_VK_RT_RAY_LEN=${L:-default})"
    ( cd runtime/build && env $( [ "$L" != 0 ] && echo CZ_VK_RT_RAY_LEN=$L ) \
        CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_VK_RT_SHADOWS=1 \
        CZ_VK_RT_DYN_SETTLE=0 CZ_VK_RT_FACTOR_READBACK=64 \
        CZ_VK_RT_FACTOR_PGM="$OUT/$tag" \
        CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$ROUTE" \
        timeout $SECS ./cz_runtime ) > "$OUT/$tag.log" 2>&1
    if ! grep -qa "requested DebugJump" "$OUT/$tag.log"; then
        echo "    *** never reached the world — not reported"; continue
    fi
    # The arm must PROVE it engaged: len= on the trace line is the value the shader was
    # handed, not the value we asked for (gotcha 408).
    grep -ao "len=[0-9.]* bias=[0-9./]*" "$OUT/$tag.log" | tail -1 | sed 's/^/    /'
    grep -a "FACTOR IMAGE" "$OUT/$tag.log" | tail -3 | cut -c1-150 | sed 's/^/    /'
done

echo
echo "################ THE SHAPE, not just the share ################"
for L in lendefault len300 len1000 len3000; do
    [ -d "$OUT/$L" ] || continue
    echo "--- $L"
    tools/rt_factor_pgm_read.py "$OUT/$L" 2>&1 | tail -20 | sed 's/^/  /'
done
