#!/bin/bash
# Part 69 follow-up — ONE ARM: put the ACTORS into the ray structure.
#
# WHY THIS ARM EXISTS, and why it is one arm and not a session
# ------------------------------------------------------------
# The part-69 pair both ran at CZ_VK_RT_DYN_SETTLE=120, which excludes any stream the
# persist store has caught being rewritten in the last 120 frames. A zombie's vertex
# buffer is rewritten EVERY frame, so it never settles: the actors were not in the ray
# structure at all in either arm. The primary ray from a pixel on a zombie therefore
# passes straight through it and lands on the ground behind, and the factor computed back
# there is painted onto the zombie — which is precisely the operator's report closing that
# session, "the shadows... passing through thing it shouldn't".
#
# CZ_VK_RT_DYN_SETTLE=0 admits everything immediately. Until part 69 that was a DIAGNOSTIC
# ONLY: a stream that changes every frame got a new content guard, therefore a new BLAS
# key, therefore a new BLAS every frame, and with no per-BLAS eviction it climbed to
# CZ_VK_RT_BLAS_MB and flushed the lot. Items 1 and 2 of docs/rt-remix-plan.md exist to
# remove exactly that: identity no longer depends on content, and a changed mesh is
# refitted in place. Headlessly it holds — blas=4687, built=4703, flushes=0 over 6,145 RT
# passes on the outdoor route — but it has never been seen in a picture.
#
# THE CONTROL IS THE SHOT YOU ALREADY TOOK. This is the same binary in the same place with
# ONE variable changed against the `bake` arm of part69_rt_geometry_session.sh, so the two
# are directly comparable and there is nothing to re-run.
#
# WHAT TO LOOK FOR
#   * Do the zombies and Chuck now have shadows ATTACHED to them, moving with them?
#   * Does the shadow stop showing through walls and through the actors themselves?
#   * Stand where you stood for the `bake` arm if you can, and F9 there.
#
# WHAT TO WATCH FOR GOING WRONG — say so even if the picture looks better:
#   * a stutter or a slow slide as the structure fills (thousands more BLASes);
#   * shadows lagging a frame or two behind a running zombie (the refit budget).
#
# ~60 s. Usage:  tools/part69_dyn0_arm.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part69-rt-geometry"
TAG="dyn0"
mkdir -p "$OUT/$TAG"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null); do busy="$busy $p"; done
if [ -n "$busy" ]; then
    echo "!! cz_runtime already running (pid$busy); quit it first."; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"

echo "==================================================================="
echo "  ARM dyn0     THE ACTORS GO IN. Same place as the bake arm, F9 please."
echo "==================================================================="
( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 \
    "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
    "CZ_CAPTURE_KEY=$OUT/$TAG" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=0 \
    CZ_VK_RT_FACTOR_READBACK=60 "CZ_VK_RT_FACTOR_PGM=$OUT/$TAG" \
    CZ_FPS_LOG=1 \
    ./cz_runtime > "$OUT/$TAG.log" 2>&1 )

if [ "$(wc -c < "$OUT/$TAG.log")" -lt 4096 ]; then
    echo "  !! ARM $TAG DID NOT RUN — log is $(wc -c < "$OUT/$TAG.log") bytes:"
    sed 's/^/     /' "$OUT/$TAG.log"; exit 3
fi

echo
echo "  DID THE ACTORS GET IN? `dyn=` must be 0 and `settledIn=` large:"
grep -a "tlasInst=" "$OUT/$TAG.log" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' | cut -c1-110
echo
echo "  DID IT STAY BOUNDED? flushes=0 is the gate items 1 and 2 exist for:"
printf '    BLAS pool flush lines: %s\n' "$(grep -ac 'over its cap' "$OUT/$TAG.log")"
echo
echo "  ITEM 3 (outOfRange MUST be 0):"
grep -a "baked=" "$OUT/$TAG.log" | tail -1 | sed 's/^\[rt\] [a-zA-Z ]*: //' | cut -c1-110
echo
echo "  THE FACTOR READBACK — against the bake arm's 18.5% shadowed:"
grep -a "FACTOR IMAGE" "$OUT/$TAG.log" | tail -1 | sed 's/\[rtb\] FACTOR IMAGE //'
echo
echo "  FRAME RATE (the bake arm had none logged; this is the first cost reading):"
grep -a "\[fps\]" "$OUT/$TAG.log" | tail -3
echo
echo "  Captures and PGMs in $OUT/$TAG/"
