#!/bin/bash
# Part 65 follow-up — ONE arm. Did fixing the sun capture give route (b) shadows?
#
# WHAT SESSION 2 ESTABLISHED, so this run does not re-ask it:
#   * the POISON arm darkened the world -> the substitution reaches the frame and
#     drives the shadow term. Route (b)'s injection point is PROVEN. Not re-run.
#   * with the real factor there were NO shadows at all — not wrong ones, absent
#     ones, from zombies, objects and buildings alike.
#
# THE CAUSE, from the same logs: the captured sun read (-0.010, 1.000, 0.020) with a
# 3587.7-unit light volume — straight down, over a town §6cu measured at ~1,100 units
# across. That is a top-down ortho, not a sun. Route (a) consumed the matrix at each
# cascade RESOLVE; route (b) read it at DRAW time and never consumed, so it became
# last-write-wins over the whole frame and picked up something drawn after the
# cascades. Two effects compound into "no shadows": a vertical sun puts every shadow
# directly under its caster, and the bias derived from that volume (0.0015 x 3587.7 =
# 5.4 world units) lifts the ray origin clear of a zombie or a van before the ray is
# cast.
#
# THE FIX: route (b) now latches its own sun matrix at the moment a shadow-atlas
# RESOLVE happens — the pairing route (a) had for free — which excludes anything that
# never resolves into the cascade atlas. And a DIRECTION CENSUS now prints every
# distinct direction the latch saw, because every cascade of one sun must agree on
# direction however much their volumes differ. In the menu era it reads 1 distinct
# over 29,224 latches; gameplay has never been measured.
#
# WHAT TO DO (~3-5 minutes):
#   Get outdoors to the same gas-station street. F4, SHADOW row:
#   HIGH -> RT LOW -> RT MEDIUM -> RT HIGH -> back to HIGH, F9 at each.
#
#   THE QUESTION IS SIMPLY: ARE THERE SHADOWS NOW?
#     * shadows under the van, the lamppost, the buildings, Chuck
#     * do they point the right way for that low orange sun (long, away from it)?
#     * zombies cast none by design in this build — expected
#     * acne (stippled dirt on lit ground) or detachment (a gap at a wall's base)
#       are the two knobs' failures and both are fixable; say which you see
#
# The line that decides the diagnosis either way is printed after the arm:
#   [rtb] sun directions latched (N latches, K distinct ...)
# K = 1 means the capture is clean. K > 1 names the intruder by its direction.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part65-operator"
mkdir -p "$OUT/sunfix"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"

cat <<'B'
===================================================================
  ARM: sunfix — the sun capture is latched at the cascade resolve now.
  F4 = debug panel (SHADOW row).  F9 = capture.
  HIGH -> RT LOW -> RT MEDIUM -> RT HIGH -> HIGH, F9 at each.
  THE QUESTION: are there shadows now, and do they point the right way?
  (Zombie attacks: the F4 Quickie Menu has IGNORE HUMANS / ATTACKS /
   GRAPPLES as three separate toggles.)
===================================================================
B
( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
    CZ_VK_PROFILE=10 \
    "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
    "CZ_CAPTURE_KEY=$OUT/sunfix" \
    "CZ_BURST_DUMP=$OUT/sunfix" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    ./cz_runtime > "$OUT/sunfix.log" 2>&1 )

echo "  finished. log: $OUT/sunfix.log"
grep -a "rtb\] sun directions" -A 9 "$OUT/sunfix.log" | tail -10 | sed 's/^/  /'
grep -a "\[rtb\] TOTAL"   "$OUT/sunfix.log" | tail -1 | sed 's/^/  /'
grep -a "\[rtb\] passes="  "$OUT/sunfix.log" | tail -1 | sed 's/^/  /'
grep -ac "no translated shader" "$OUT/sunfix.log" | sed 's/^/  shaders the cache lacked: /'
