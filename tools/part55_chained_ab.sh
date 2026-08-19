#!/bin/bash
# THE CHAINED CONTAINER A/B — the operator drives, both arms, quitting one starts the next.
#
# THE QUESTION. Part 55 replaced five `std::unordered_map`/`std::map` lookups on the pump
# thread with flat open-addressed tables. `tools/part55_srcline.py` says they were roughly
# a QUARTER of that thread; a roaming campaign says the frame is 12.0% shorter at >= 6,000
# draws with no overlap between the arms. Neither of those is the number that matters.
#
# WHY THIS REPLACES THE ROAMING CAMPAIGN, in the operator's words: "I'll do your campaign
# with soak at the spot that hit the cpu the most so we just get 2x 3minutes soak instead
# of hours of testing in unstable environment with autochuck." That is the right call and
# it is this project's own conclusion three parts running:
#
#   * gotcha 355 — an A/B measures the LOAD IT SAMPLED. Every roaming campaign here lands
#     its best-populated bin at 2,500-3,000 draws while the operator plays at 6,700-7,300,
#     and part 54's headline was over-generalised by a factor of six because of it.
#   * part 52 — a soak held 7,773 frames in ONE draw bin against a walk's ~1,300, giving a
#     significance of +211 against a walk's +13. That is ~16x the measurement for a
#     fifteenth of the wall time.
#   * AutoChuck is not a route. Zombie spawns differ, the AI opens the map, and a run that
#     stays in the military zone is not the run that walks the main street — so binning by
#     draw count matches the QUANTITY of work and not its COMPOSITION.
#
# THE ARMS, and there is exactly one difference between them:
#
#   flat   the shipped default — five flat tables (per-frame stream cache, shader table,
#          cross-frame store index, texture cache, and the two texture censuses)
#   maps   CZ_VK_NO_FLAT_CACHE=1 — `std::unordered_map` and `std::map`, i.e. what this
#          renderer used for fifty-four parts. The old configuration RUN NOW (gotcha 51).
#
# THE INTERNAL RESOLUTION IS 1280x720 HERE AND THAT IS DELIBERATE. Part 54's chained A/B
# ran at 2560x1440 because it was measuring a PRESENT path whose cost scales with pixels.
# This item is pure CPU on the pump thread, and at 2x the GPU is much closer to being the
# limiter — which is exactly the mechanism that collapsed part 54's saving at high load,
# CPU time taken off the pump being absorbed by a longer fence wait rather than converted
# into frames. Measuring a CPU item at a resolution that makes the GPU the limiter would
# understate it and the run could not say so. `RES=2560x1440 tools/part55_chained_ab.sh`
# asks the other question on purpose.
#
# GOD MODE AND "ZOMBIES IGNORE ALL HUMANS" ARE HELD IN BOTH ARMS, through `CZ_DEBUG_FLAGS`
# rather than the F4 menu, so they are identical and automatic and cannot differ between
# the arms. Standing still in the heaviest place is exactly where Chuck dies, and a zombie
# GRAB moves the camera — which changes the draw set, which is the one thing a soak exists
# to hold still. Part 54's harness established both; the reasoning is in its header.
#
# DELIBERATELY NOT ON: the profiler (2-4 ms/frame) and frame stats (1.9-3.3 ms), either of
# which would change the thing being judged. `CZ_FPS_LOG` is one counter and one clock read
# per presented frame and it carries the draw count on every line, which is what makes two
# windows matchable rather than merely averageable.
#
# Usage:  tools/part55_chained_ab.sh                    # flat first, then maps
#         ORDER=maps,flat tools/part55_chained_ab.sh    # the other order
#         RES=2560x1440 SECS_HINT=180 tools/part55_chained_ab.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part55-operator"
RES="${RES:-1280x720}"
FPS="${FPS:-500}"
ORDER="${ORDER:-flat,maps}"
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

# ONE BINARY FOR BOTH ARMS, snapshotted so nothing rebuilt between them can be blamed.
BIN=cz_runtime_p55ab
cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
echo "snapshot: $BIN <- cz_runtime ($(cd "$ROOT" && git rev-parse --short HEAD))"

STAMP="$(date +%m%d_%H%M)"
run_arm() {
    local arm="$1" n="$2"
    local tag="p55ab_${STAMP}_${n}_${arm}"
    local extra=()
    # Four arm names, one variable each, all against the SAME binary:
    #   flat / maps      -- the container item (CZ_VK_NO_FLAT_CACHE)
    #   vram / ram       -- where the GEOMETRY buffers live (CZ_VK_VRAM_STREAMS)
    #   memo / nomemo    -- the ALU constant memo (CZ_VK_NO_CONST_MEMO)
    # `flat`, `ram` and `memo` are all names for the shipped default, so each pair
    # compares exactly one change against what ships and nothing else.
    [ "$arm" = maps ]    && extra+=(CZ_VK_NO_FLAT_CACHE=1)
    [ "$arm" = vram ]    && extra+=(CZ_VK_VRAM_STREAMS=1)
    [ "$arm" = nomemo ]  && extra+=(CZ_VK_NO_CONST_MEMO=1)
    cat <<BANNER

===================================================================
  ARM $n of 2:  $arm
  $( case "$arm" in
       maps) echo "CZ_VK_NO_FLAT_CACHE=1 -- std::unordered_map / std::map" ;;
       vram) echo "CZ_VK_VRAM_STREAMS=1 -- geometry buffers in VIDEO memory" ;;
       ram)  echo "the shipped default -- geometry buffers in system RAM" ;;
       nomemo) echo "CZ_VK_NO_CONST_MEMO=1 -- all 8 KB of constants copied every draw" ;;
       memo) echo "the shipped default -- the ALU constant memo (~35% of half-copies served)" ;;
       *)    echo "the shipped default -- five flat open-addressed tables" ;;
     esac )
  res:   $RES internal, window MAXIMISED
  cap:   CZ_FPS_CAP=$FPS
  debug: $FLAGS  (held on by the pump, both arms -- no menu needed)
  log:   $OUT/$tag.log

  >>> STAND STILL IN THE HEAVIEST PLACE YOU CAN FIND, for ~3 min.
  >>> Do NOT walk -- a soak is worth ~16x a walk here.
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
        "${extra[@]}" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    echo "  arm $n ($arm) finished."
}

n=1
IFS=',' read -ra arms <<< "$ORDER"
for a in "${arms[@]}"; do run_arm "$a" "$n"; n=$((n + 1)); done

echo
echo "==================================================================="
echo "  BOTH ARMS DONE."
for f in "$OUT"/p55ab_"$STAMP"_*.log; do
    echo
    echo "--- $(basename "$f")"
    # The thread budget FIRST: a parallel measurement has a machine as well as a
    # workload, and naming only one is naming none (gotcha 359).
    grep -a "^\[threads\]" "$f" | tail -3
    grep -a "VIDEO MEMORY\|system RAM" "$f" | head -2
    grep -a "const memo" "$f" | tail -2
    grep -a "window drawable\|\[vk\] swapchain " "$f" | tail -2
    grep -a "^\[fps\]" "$f" | tail -22
done
echo
echo "  Match windows on 'draws med', DISCARD any whose (min..max) spread"
echo "  is wide -- that window straddled two places and its median is not"
echo "  a place at all."
echo "  read with: python3 tools/part54_fps_bins.py $OUT/p55ab_${STAMP}_*_flat.log -- $OUT/p55ab_${STAMP}_*_maps.log"
echo "  shaders the cache lacked: $(grep -ac 'no translated shader' "$OUT"/p55ab_"$STAMP"_*.log | paste -sd+ | bc 2>/dev/null || echo '?')"
