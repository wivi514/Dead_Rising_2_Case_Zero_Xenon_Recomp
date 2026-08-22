#!/bin/bash
# Part 65 operator session — RT SHADOWS, ROUTE (B): the screen-space traced factor.
#
# WHAT CHANGED SINCE THE SESSION YOU RAN IN PART 64. That session showed route (a):
# ray-traced depths written INTO the title's own shadow atlas. Your eleven words —
# "shadow squares following where the player is and normal shadow still on" — named the
# mechanism three headless statistics had missed, and it is structural: writing the MAP
# means every receiver inside it is compared against itself, and there is no
# receiver-side offset to apply. Route (a) is closed (docs/phase5-notes.md §6cv §7j).
#
# ROUTE (B) computes the shadow per RECEIVING PIXEL instead: the ray starts at that
# pixel's own surface and is pushed off it before it goes anywhere, so the same defect
# cannot happen. A census over twenty hardware GPU traces found the 126 pixel shaders
# that sample the atlas and the 140 slots they sample it at; those taps now read our
# factor image, through a second shader cache the runtime loads beside the normal one.
#
# WHAT THIS SESSION DECIDES — three questions, in order, and arm 1 gates the rest:
#
#   1. DOES THE SUBSTITUTION REACH THE FRAME AT ALL? Arm 1 poisons the factor to
#      all-shadow. Every surface that takes a sun shadow must go dark. If the picture
#      does NOT change when you switch the SHADOW row to an RT value, stop and say so —
#      nothing below means anything.
#   2. IS IT CORRECT? Arm 2 is the real thing, toggled LIVE from the panel on one scene.
#      THE QUESTION IS SHAPE, not brightness: do the white vans and the quarantine bus
#      stay BRIGHT (they went uniformly grey under route (a))? Do shadows sit UNDER the
#      things that cast them? Is there acne — a dirty stippled look on lit surfaces?
#   3. DOES THE LADDER MEAN ANYTHING? Arm 2 again: RT LOW is half-resolution and one
#      ray (hard edges), RT MEDIUM full-resolution one ray, RT HIGH four rays across the
#      sun's disc — RT HIGH should be the only one with SOFT shadow edges.
#
# HOW TO DRIVE IT. F4 opens the debug panel; the SHADOW row now reads
#   LOW / MEDIUM / HIGH / RT LOW / RT MEDIUM / RT HIGH
# and it is LIVE — change it standing in one place, outdoors, and watch. Left/Right on
# the row. The raster tier is remembered underneath, so stepping back down to HIGH
# returns exactly what you had.
#
# WHAT IS DELIBERATELY NOT SET: CZ_VK_RT_SHADOWS. The env arm WINS over the panel row
# and would freeze it, which would remove the whole point of arms 2 and 3.
#
# Quit the game normally when an arm is done; the next starts itself.
#
# Usage:  tools/part65_operator_session.sh
#         START=2 tools/part65_operator_session.sh    # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part65-operator"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

# The cache a play session selects (a2m foliage + the clip-plane epilogue). Its RT
# sibling is found automatically by appending _rt, so one change per experiment holds:
# turning RT on does not also change the foliage or the slicing.
CLIP="$ROOT/assets/shader_spv_clip_a2m"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"

for d in "$CLIP" "${CLIP}_rt"; do
    n=$(ls "$d"/*.spv 2>/dev/null | wc -l)
    echo "  cache $(basename "$d"): $n modules"
    [ "$n" -gt 0 ] || { echo "!! missing shader cache $d — build it, see tools/patch_rt_shadow_hlsl.py"; exit 2; }
done

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag"
    echo "  F4 = debug panel (SHADOW row).  F9 = capture -> $OUT/$tag"
    echo "  Quit the game normally when you are done; the next arm starts itself."
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
        CZ_VK_PROFILE=10 \
        "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_BURST_DUMP=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag finished. log: $OUT/$tag.log"
    # THE ENGAGEMENT LINES, printed for you rather than left in a 200 MB log. Part 64
    # shipped a build whose A/B measured a perfect fix and was the feature silently
    # switched off; these three lines are what makes that impossible to miss.
    grep -a "RT shadow variant cache"  "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -a "\[rtb\] factor pass ready" "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -a "\[rtb\] TOTAL"             "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -a "\[rtb\] passes="           "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -ac "no translated shader"     "$OUT/$tag.log" | sed 's/^/  shaders the cache lacked: /'
    echo
}

S="${START:-1}"

if [ "$S" -le 1 ]; then
cat <<'B1'

  ARM 1 — THE POSITIVE CONTROL (~1 minute). CZ_VK_RT_FACTOR_POISON=1.
  Get outdoors, stand still, F4, and step the SHADOW row from HIGH to RT LOW.

  EXPECTED AND CORRECT: the world goes DARK — every surface that takes a sun
  shadow is shadowed, because the factor is forced to "occluded" everywhere.
  That darkening IS the pass. Step back to HIGH and it must come back.

  IF NOTHING CHANGES: the substitution never reached the frame. Say so and stop;
  arms 2 and 3 cannot mean anything. (The log lines printed after the arm will
  say whether the variant cache loaded and whether the factor pass ran.)
  F9 one shot of each state if you can.
B1
run poison CZ_VK_RT_FACTOR_POISON=1
fi

if [ "$S" -le 2 ]; then
cat <<'B2'

  ARM 2 — THE REAL THING, toggled live (~5-10 minutes). No poison, no env arm.

  Stand outdoors somewhere with strong shadows — the military camp, the street
  by the gas station, anywhere with the white vans or the quarantine bus in
  frame, since those are what went uniformly grey under route (a).

  Step the SHADOW row: HIGH -> RT LOW -> RT MEDIUM -> RT HIGH -> back to HIGH.
  F9 at each. WHAT I NEED FROM YOU IS SHAPE, in your own words:

    * do LIT surfaces stay lit? (the route (a) failure was every lit surface
      going grey — if you see that again, route (b) has it too and say so)
    * do the shadows sit UNDER the things that cast them, or offset from them?
    * is there ACNE — a dirty stippled or crawling look on flat lit ground?
    * do shadows DETACH from their casters (a gap at the base of a wall or a
      lamppost)? That is the opposite error and it needs the other knob.
    * RT HIGH vs RT MEDIUM: are the shadow EDGES softer on HIGH?
    * anything that is simply missing: zombies and Chuck cast NO RT shadow by
      design in this build (they are not in the ray structure) — expected, but
      tell me how bad it looks.

  Also owed from part 60 and cheap to fold in here: how do the raster tiers LOW
  and HIGH compare to each other on this same scene?
B2
run live
fi

if [ "$S" -le 3 ]; then
cat <<'B3'

  ARM 3 — EXAGGERATED SOFTNESS (~1 minute). Four rays over a 5x wider sun disc.

  Only worth running if arm 2 looked broadly right. Switch the SHADOW row to
  RT HIGH and look at a long shadow's edge: it should be obviously, even
  excessively soft, and stepped in a few discrete levels rather than smooth.
  That stepping is expected — the title's own 2x2 filter gives us five levels.

  IF IT IS IDENTICAL TO ARM 2's RT HIGH, the cone/ray path is not engaging.
B3
run softmax CZ_VK_RT_RAYS=4 CZ_VK_RT_CONE=0.1
fi

echo "  all arms done. Everything is in $OUT/"
echo
echo "  If arm 2 showed shadows offset from their casters, the two knobs are"
echo "  CZ_VK_RT_FACTOR_BIAS (along the sun) and CZ_VK_RT_FACTOR_CAMBIAS (toward"
echo "  the camera); both default to a fraction of the cascade's own depth extent."
