#!/usr/bin/env bash
# PART 70 — IS THERE ONE ENORMOUS OCCLUDER? The suspect mode 18 cannot see.
#
# WHY THIS EXISTS
# ---------------
# `docs/part69-night-plan.md` §2 named this and then shelved it: "IF THE RECEIVER IS A
# PLANE — find the mesh that is standing in front of the camera... The likeliest mechanism
# is ONE ENORMOUS ADMITTED MESH." It was set aside because `CZ_VK_RT_FACTOR_DEBUG=18`
# rendered a correct depth image of the world, which was read as exonerating the occluder
# population.
#
# **Mode 18 cannot exonerate it.** Mode 18 traces the PRIMARY ray, camera through pixel,
# and reports the closest hit — so it images only what the camera can see. A large
# occluder ABOVE the scene, or behind the camera, contributes nothing to that image and
# still blocks every shadow ray fired toward the sun. The signature that produces is the
# reported one: a flat slab of shadow whose boundary is the mesh's own edge — a straight
# line in screen space that bends at no surface it crosses, because it belongs to the
# occluder and not to the receiver.
#
# The gate that should catch such a mesh screens the stream's OBJECT-space extent against
# `CZ_VK_RT_BOUNDS_CAP` (default 50,000) in a town that fits in ~1,100, so a mesh a
# hundred times the town's height passes. It is also blind to a small mesh with a large
# SCALE in its instance transform, which is the same hazard.
#
# So two things run together here, and the second is the point:
#   * the cap sweep — 50000 (default) / 5000 / 1000 / 200 — watching the shadowed share
#     and `tlasInst` together. A cap at which the slab goes away while `tlasInst` barely
#     moves names the population without any new code;
#   * the per-mesh WORLD-extent census this binary now prints
#     (`largest admitted meshes by WORLD extent`), which NAMES the members instead of a
#     threshold. A threshold that works is a workaround; the named streams are the
#     finding, and they are what a Case West port would need.
#
# PRE-REGISTERED: if the largest admitted mesh is of the order of the town (~1,100 units)
# or smaller, there is no enormous occluder and this hypothesis is dead — which is worth
# the same twenty minutes as confirming it.
#
# Diagnostic only: counters and a PGM read by a tool.
set -u
OUT=${1:-$HOME/DR2CZ-troubleshooting/part70-bounds}
SECS=${SECS:-240}
mkdir -p "$OUT"
[ -x runtime/build/cz_runtime ] || { echo "build first" >&2; exit 1; }
ROUTE="F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE"

for CAP in 50000 5000 1000 200; do
    tag="cap$CAP"
    mkdir -p "$OUT/$tag"
    echo "=== $tag"
    ( cd runtime/build && env CZ_VK_RT_BOUNDS_CAP=$CAP \
        CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_VK_RT_SHADOWS=1 \
        CZ_VK_RT_DYN_SETTLE=0 CZ_VK_RT_FACTOR_READBACK=64 \
        CZ_VK_RT_FACTOR_PGM="$OUT/$tag" \
        CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$ROUTE" \
        timeout $SECS ./cz_runtime ) > "$OUT/$tag.log" 2>&1
    if ! grep -qa "requested DebugJump" "$OUT/$tag.log"; then
        echo "    *** never reached the world — not reported"; continue
    fi
    grep -ao "tlasInst=[0-9]* blas=[0-9]*[^|]*" "$OUT/$tag.log" | tail -1 \
        | cut -c1-130 | sed 's/^/    /'
    grep -a "FACTOR IMAGE" "$OUT/$tag.log" | tail -2 | cut -c1-140 | sed 's/^/    /'
    grep -a -A9 "largest admitted meshes" "$OUT/$tag.log" | tail -9 | sed 's/^/    /'
done
