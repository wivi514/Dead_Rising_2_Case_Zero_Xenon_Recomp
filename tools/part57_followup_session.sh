#!/bin/bash
# Part 57 follow-up — ONE launch, three answers.
#
#   1. THE GAS SIGN (your #1): walk to the far spot where the sign breaks, press F9,
#      then STAND STILL ~10 seconds. A watcher fires live_texdump within seconds of
#      the press (process_vm_readv, the game never notices) and pulls the tiny far-LOD
#      textures out of guest memory — the last untested input for the black letters.
#      The F9 also photographs whether the sign is black TODAY (yesterday's draw-ID
#      run could not take pictures).
#   2. THE DECAL FLICKER: one or two F8 bursts at a decal WITH THE CAMERA MOVING.
#      The census now carries each draw's vertex-buffer ADDRESS (va=), which is what
#      separates "the game ping-pongs two buffers" from "one buffer is rewritten
#      under our walk" — the question the first bursts could not answer.
#   3. THE SLICING RESIDUALS: slice a zombie or two. This launch carries
#      CZ_VK_CLIP_BIAS (default +0.01), pushing every clip plane slightly OUTWARD.
#      If the see-through cut now seals (the gore plug survives), the boundary-error
#      mechanism is confirmed; if nothing changes, it is refuted. Either answer is
#      progress. BIAS=0.03 or BIAS=-0.01 re-runs the sweep.
#
# Usage:  tools/part57_followup_session.sh            # clip cache, bias +0.01
#         BIAS=0 tools/part57_followup_session.sh     # the no-bias control
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part57-operator"
TAG="followup_$(date +%H%M)"
mkdir -p "$OUT/$TAG"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"
BIAS="${BIAS:-0.01}"
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"

echo "==================================================================="
echo "  PART 57 FOLLOW-UP  (clip cache, CZ_VK_CLIP_BIAS=$BIAS)"
echo "  1. far GAS sign: F9, then STAND STILL ~10 s (a texture dump fires itself)"
echo "  2. flickering decal: F8 while MOVING the camera (once or twice)"
echo "  3. slice a zombie: does the cut still show through? F9 it either way"
echo "  Quit normally when done. Everything -> $OUT/$TAG"
echo "==================================================================="

( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
    "CZ_CAPTURE_KEY=$OUT/$TAG" \
    "CZ_BURST_DUMP=$OUT/$TAG" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "CZ_VK_CLIP_BIAS=$BIAS" \
    "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
    ./cz_runtime > "$OUT/$TAG.log" 2>&1 ) &
runner=$!

# The texture-dump watcher: fire live_texdump within seconds of EVERY F9, while the
# operator is still standing on the spot — a dump taken minutes later reads recycled
# streaming memory and supports any theory (gotcha 285). One dump per capture, keyed
# on the census file appearing.
(
  seen=""
  while kill -0 $runner 2>/dev/null; do
      for c in "$OUT/$TAG"/capture_f*.census; do
          [ -e "$c" ] || continue
          case " $seen " in *" $c "*) continue ;; esac
          seen="$seen $c"
          sleep 2   # let the census finish writing
          python3 "$ROOT/tools/live_texdump.py" "$OUT/$TAG" texdump "$c" \
              >> "$OUT/$TAG.texdump.log" 2>&1
      done
      sleep 1
  done
) &
watcher=$!

wait $runner
kill $watcher 2>/dev/null
echo
echo "  finished. log: $OUT/$TAG.log"
grep -a  "user clip plane BIASED" "$OUT/$TAG.log" | tail -1 | sed 's/^/  /'
grep -ac "burst .* DONE" "$OUT/$TAG.log" | sed 's/^/  bursts: /'
ls "$OUT/$TAG" | grep -c "^texdump" | sed 's/^/  texture dump dirs: /'
grep -ac "no translated shader" "$OUT/$TAG.log" | sed 's/^/  cache misses: /'
