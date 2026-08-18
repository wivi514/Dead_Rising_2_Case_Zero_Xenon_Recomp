#!/bin/bash
# THE CHAINED SWAPCHAIN A/B — the operator drives, both arms, quitting one starts the next.
#
# THE QUESTION. The measurement says CZ_VK_SWAPCHAIN is worth 21-29% of the frame at
# 2560x1440 internal into a maximised 2560x1417 window. The operator played it and said
# "still feels pretty much the same framerate wise". Those are not reconciled, and no
# instrument in this project can reconcile them, because what their report is implicitly
# compared against is a frame rate from another DAY — which is exactly the control gotcha
# 51 forbids. The only fix is both arms, same session, same machine, same route.
#
# THE OPERATOR'S OWN OBJECTION SHAPED THIS SCRIPT, and it is the reason it asks for a SOAK
# rather than a walk. In their words: AutoChuck "isn't a predetermined route and zombie
# spawns are not always the same, so it won't be 100% accurate especially if in a run it
# stays in the military zone and one go on the main street." That is right, it applies to a
# human-driven session too, and binning by draw count only half fixes it: a draw count
# controls for the QUANTITY of work and not its COMPOSITION, so a 3,000-draw military zone
# and a 3,000-draw main street are matched by the bin and are not the same workload.
#
# Three things follow, and all three are in here:
#
#   1. SOAK, DO NOT WALK. Stand still in one heavy place for the whole window. Part 52
#      established this as the best measurement this project has ever taken — one draw bin
#      held 7,773 frames against a walk's ~1,300, giving a significance of +211 against a
#      walk's +13. A soak removes route variance because there is no route.
#   2. THE SAME PLACE IN BOTH ARMS. The arms are chained precisely so the operator can quit
#      one and walk straight back to the same spot; nothing else here is worth as much.
#   3. THE DRAW COUNT IS ON EVERY LINE, with its min and max. `[fps]` now carries
#      `draws med N (min..max)`, so a window that straddled two places is visible as a wide
#      spread rather than averaged into a plausible median. Match windows on it; discard
#      any window whose spread is wide.
#
# GOD MODE IS ON IN BOTH ARMS, AND NOT THROUGH THE MENU. The operator's second objection:
# "if you want me to do a soak launch the game on debug or else I'll get killed by zombies"
# — standing still in the heaviest place is exactly where Chuck dies. The obvious answer,
# the F4 host-rendered debug menu, is the ONE thing the swapchain arm cannot draw, so it
# would have to be set by hand in one arm and not the other.
#
# `CZ_DEBUG_FLAGS` is the better answer and it already existed: it names entries from the
# title's own debug-bool table by label, and the PUMP re-asserts them every frame because
# the game clears them on a level load. So god mode is identical, automatic and counted in
# both arms — no operator fiddling, and no way for the arms to differ in it.
#
# "ZOMBIES IGNORE ALL HUMANS" IS IN, AND THE OPERATOR'S REASON IS BETTER THAN THE ONE THIS
# SCRIPT ORIGINALLY GAVE FOR LEAVING IT OUT. The first version excluded it as "a different
# workload wearing the same name". Their answer: "zombies will not grab me so it won't move
# the camera" -- and a grab moves the CAMERA, which changes the draw set, which is the one
# thing a soak exists to hold still. That is a measurement argument, not a convenience one,
# and it outranks workload fidelity: the crowd is still there and still rendered, it simply
# does not reach in and rotate the thing being measured.
#
# It is held in BOTH arms, so it cannot bias the comparison; what it costs is that the
# scene is not exactly ordinary play, which is stated here rather than discovered later.
#
# WHAT IS DELIBERATELY NOT ON: the profiler (2-4 ms/frame) and frame stats (1.9-3.3), both
# of which would change the thing being judged, and the second of which would additionally
# force the readback to keep running in the swapchain arm and measure nothing at all
# (gotcha 350). `CZ_FPS_LOG` is one counter and one clock read per frame.
#
# Usage:  tools/part54_chained_ab.sh              # swapchain first, then readback
#         ORDER=readback,swapchain tools/part54_chained_ab.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part54-operator"
RES="${RES:-2560x1440}"
FPS="${FPS:-500}"
ORDER="${ORDER:-swapchain,readback}"
FLAGS="${FLAGS:-CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS}"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

STAMP="$(date +%m%d_%H%M)"
run_arm() {
    local arm="$1" n="$2"
    local tag="p54ab_${STAMP}_${n}_${arm}"
    local extra=()
    # Inverted at the close of part 54: the swapchain is the default, so it is the
    # READBACK arm that needs a flag now.
    [ "$arm" = readback ] && extra+=(CZ_VK_NO_SWAPCHAIN=1)
    cat <<BANNER

===================================================================
  ARM $n of 2:  $arm
  res:   $RES internal, window MAXIMISED
  cap:   CZ_FPS_CAP=$FPS
  debug: $FLAGS  (held on by the pump, both arms -- no menu needed)
  log:   $OUT/$tag.log

  >>> STAND STILL IN THE HEAVIEST PLACE YOU CAN FIND, for 2-3 min.
  >>> Do NOT walk a route -- a soak is worth ~16x a walk here, and
  >>> it is the only way round zombie spawns differing between runs.
  >>> Then QUIT, and the next arm starts automatically.
  >>> Go back to THE SAME SPOT in the second arm.
===================================================================

BANNER
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_WINDOW_MAXIMIZED=1 \
        "CZ_VK_RES=$RES" "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=10 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_a2m" CZ_VK_A2M_ANY_SURFACE=1 \
        CZ_VK_A2M_MODE=1 \
        CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=$FLAGS" \
        "${extra[@]}" ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  arm $n ($arm) finished."
}

n=1
IFS=',' read -ra arms <<< "$ORDER"
for a in "${arms[@]}"; do run_arm "$a" "$n"; n=$((n + 1)); done

echo
echo "==================================================================="
echo "  BOTH ARMS DONE. Windows, with the draw count on every line so"
echo "  they can be matched rather than averaged:"
for f in "$OUT"/p54ab_"$STAMP"_*.log; do
    echo
    echo "--- $(basename "$f")"
    grep -a "window drawable\|\[vk\] swapchain " "$f" | tail -2
    grep -a "CZ_DEBUG_FLAGS" "$f" | tail -3
    grep -a "^\[fps\]" "$f" | tail -20
done
echo
echo "  Match windows on 'draws med', DISCARD any whose (min..max) spread"
echo "  is wide -- that window straddled two places and its median is not"
echo "  a place at all."
echo "  shaders the cache lacked: $(grep -ac 'no translated shader' "$OUT"/p54ab_"$STAMP"_*.log | paste -sd+ | bc 2>/dev/null || echo '?')"
