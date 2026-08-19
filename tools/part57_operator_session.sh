#!/bin/bash
# Part 57 operator session — four arms, chained: quit one and the next starts.
#
# WHAT THIS SESSION IS FOR, in the operator's priority order from part 56:
#
#   1. THE DISTANCE DEFECTS (gas sign / canopy / bunting): identify WHICH draw paints
#      the sign, with the draw-ID pass — attribution by inference has been wrong twice
#      (gotchas 291, 302), so this session points at the pixel instead.
#   2. THE DECAL FLICKER: the F8 burst now records a PER-DRAW CENSUS beside the pixels
#      (built this part), so a decal seen blinking can finally be asked "was your draw
#      issued on the dark frames?" — the question part 56 could not answer.
#   3. ZOMBIE SLICING: user clip planes are now computed by the vertex shaders in a new
#      cache (Vulkan has no fixed-function planes). Two 30-second poison arms prove the
#      plumbing before any zombie is asked to test it.
#
# THE ARMS, in the order they launch:
#
#   arm 1  clip_poison   (~30 s)  clip cache + a poison plane that clips EVERYTHING.
#                                 SUCCESS = the picture VANISHES (menus included) as
#                                 soon as anything draws. If the game looks normal, the
#                                 clip chain is broken and arm 4 would be a null test.
#                                 Vulkan validation is ON in this arm (it is short).
#                                 Just boot, look, quit.
#   arm 2  poison_null   (~20 s)  SAME poison on the stock cache. SUCCESS = the game is
#                                 completely NORMAL (the poison only acts through the
#                                 new shader epilogue). Boot, look, quit.
#   arm 3  distance_flicker       the long arm — stock shaders, draw-ID on F9, SAFE on:
#                                 * stand where the GAS SIGN is broken at distance
#                                   (your capture_012174 spot), STAND STILL, press F9
#                                   twice a few seconds apart. The screen flashes dark
#                                   for one frame per press — that IS the capture.
#                                 * same at the canopy/bunting spot if different.
#                                 * find a flickering decal: press F8 and KEEP THE
#                                   CAMERA MOVING for the whole second (the flicker
#                                   needs motion). Two or three bursts. Then one more
#                                   burst STANDING STILL at the same decal, as the
#                                   control. Each burst writes ~260 MB + a census.
#   arm 4  clip_slice             the clip+a2m cache, SAFE on: cut zombies in half
#                                 (machete/sword/katana), several of them. The question:
#                                 do the halves now SEPARATE — one upper half, one lower
#                                 half — instead of two whole doubled bodies? F9 at every
#                                 result, good or bad. Also glance around the world: all
#                                 104 vertex shaders changed in this cache, so anything
#                                 NEWLY wrong anywhere is worth an F9 and a sentence.
#
# Usage:  tools/part57_operator_session.sh          # all four arms, chained
#         START=3 tools/part57_operator_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part57-operator"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

A2M="$ROOT/assets/shader_spv_a2m"
CLIP="$ROOT/assets/shader_spv_clip_a2m"   # a2m foliage fix + the clip-plane epilogue
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag"
    echo "  F9 = capture -> $OUT/$tag     F8 = burst -> $OUT/$tag"
    echo "  Quit the game normally when you are done; the next arm starts itself."
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_BURST_DUMP=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag finished. log: $OUT/$tag.log"
    grep -ac "no translated shader" "$OUT/$tag.log" | sed 's/^/  shaders the cache lacked: /'
    grep -a  "user clip plane"      "$OUT/$tag.log" | tail -3 | sed 's/^/  /'
    grep -a  "CLIP_POISON"          "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -ac "burst .* DONE"        "$OUT/$tag.log" | sed 's/^/  bursts recorded: /'
    grep -a  "VALIDATION"           "$OUT/$tag.log" | grep -avc "topology-08773" \
        | sed 's/^/  validation lines that are not the known topology baseline: /' || true
    echo
}

S="${START:-1}"

if [ "$S" -le 1 ]; then
cat <<'B1'

  ARM 1 — CLIP POISON on the clip cache (~30 seconds).
  EXPECTED AND CORRECT: the picture DISAPPEARS (black/empty, menus too) as soon
  as the renderer draws. That vanishing IS the pass. If the game looks normal,
  say so — that means the clip chain is inert and arm 4 cannot mean anything.
  Boot, look for a few seconds, quit.
B1
run clip_poison "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    CZ_VK_CLIP_POISON=1 CZ_VK_VALIDATION=1
fi

if [ "$S" -le 2 ]; then
cat <<'B2'

  ARM 2 — the SAME poison on the stock cache (~20 seconds).
  EXPECTED AND CORRECT: the game is completely NORMAL. Boot, look, quit.
B2
run poison_null "CZ_SHADER_SPV=$A2M" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    CZ_VK_CLIP_POISON=1
fi

if [ "$S" -le 3 ]; then
cat <<'B3'

  ARM 3 — DISTANCE DEFECTS + DECAL FLICKER (stock shaders, SAFE on, draw-ID on F9).
    * GAS sign, far, where it is broken: STAND STILL, F9 twice, a few seconds apart.
      (One dark flash per press is the capture working, not a bug.)
    * Canopy / bunting viewpoint too if it is a different spot.
    * A flickering decal: F8 while MOVING the camera, two or three times.
      Then ONE burst standing still at the same decal — the control.
B3
run distance_flicker "CZ_SHADER_SPV=$A2M" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    CZ_VK_DRAW_ID=1 "CZ_DEBUG_FLAGS=$SAFE_FLAGS"
fi

if [ "$S" -le 4 ]; then
cat <<'B4'

  ARM 4 — ZOMBIE SLICING on the clip-plane cache (SAFE on).
    Cut several zombies in half. The question: do the halves SEPARATE now —
    an upper half and a lower half — instead of two whole doubled bodies?
    F9 at every result, good or bad. And glance around the world for anything
    NEWLY wrong (every vertex shader changed in this cache).
B4
run clip_slice "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "CZ_DEBUG_FLAGS=$SAFE_FLAGS"
fi

echo "  all arms done. Everything is in $OUT/"
