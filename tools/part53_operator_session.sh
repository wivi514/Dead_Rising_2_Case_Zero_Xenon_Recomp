#!/bin/bash
# Part 53 operator session — the guards moved off the pump, and nobody has looked at it.
#
# THE QUESTION: is it faster where you play, and does anything look or feel wrong?
#
# WHY THIS ONE IS DIFFERENT FROM EVERY PREVIOUS OPERATOR SESSION. Part 53 is the first
# change this port has made that does not make work smaller — it MOVES work onto cores
# that were idle. Both content guards (the vertex/index stream store's and the texture
# cache's) now fold on four worker threads, filed a frame ahead from the working set the
# pump saw last frame. So this session has to answer two things, not one:
#
#   1. is the frame shorter where you play?
#   2. what did it COST? The process went 2.53 -> 2.68 cores headlessly, and 33.2 points
#      of core appeared on the workers where 13.1 left the pump. That is expected — but
#      "expected" is not "measured on your machine", so both arms sample per-thread CPU.
#
# WHAT SHIPPED, so the report can agree or disagree with something specific:
#
#   * item 1.1 — `GuardFold` went from 25.87% of the pump thread to 0.86%. Headlessly
#     that is -12.5% of frame time at 6,000-7,000 draws against a null of +0.1%, and
#     +10.0% more frames delivered in the same wall time.
#   * item 1.3 — the present readback stopped making a redundant 3.5 MB copy every frame.
#     `readback` went from 5.5-7.3% of the frame to 0.0%, ~0.78 ms.
#
# WHAT COULD HAVE GONE WRONG, and what to look at:
#
#   The guard is what decides whether a mesh or a texture the game reuses at the same
#   address is STILL the same. Pre-hashing it moves that read up to a frame earlier, and
#   the new thing it admits is a ONE-FRAME STALE buffer — the part-46 defect class. That
#   is not a crash and not a missing object; it reads as:
#
#     * the HUD showing the previous value (ammo, health, a timer that lags by a frame);
#     * an animated mesh that stutters or snaps rather than moving smoothly;
#     * a texture that flicks to a different one for a single frame.
#
#   Headlessly it happens to 0.0002-0.0021% of guards, and most of those are harmless.
#   **If you see any of the three, press F9 and say where.** That is the one class this
#   part could have introduced.
#
#   Item 1.3 changes which buffer the window is fed from, not what is in it, so a defect
#   there would be a whole frame wrong or torn rather than one object wrong.
#
# INSTRUMENTS. `CZ_VK_PROFILE` is on in both modes. `CZ_VK_FRAME_STATS` is on ONLY in the
# A/B mode — it costs 1.86-3.32 ms a frame on this machine, so paying it in a single-arm
# session would be instrumenting the very thing being judged (gotcha 7), while in a
# two-arm session it rides in both, inflates both equally, and buys the median by DRAW BIN
# which is the statistic that actually reads on this title (gotcha 237).
#
# Quote nothing from here as an absolute frame time without saying which were on.
#
# Usage:  tools/part53_operator_session.sh          # one arm, the current build
#         ARM=ab tools/part53_operator_session.sh   # two arms chained: quit one, the
#                                                   # next starts. Arm B restores BOTH
#                                                   # part-53 items in the SAME binary
#                                                   # (gotchas 50/51/86).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part53-operator"
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

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag"
    echo "  F9  = screenshot -> $OUT/$tag   (and it MARKS the soak in the stats)"
    echo "  F2  = the shipped DebugJump screen (at the title menu)"
    echo "  F4  = the Case Zero debug submenus (Left goes back)"
    echo "  Quit the game normally when you are done."
    echo "==================================================================="
    local stats=()
    [ "${ARM:-single}" = ab ] && stats=("CZ_VK_FRAME_STATS=$OUT/$tag.stats")
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "CZ_VK_PROFILE=${PROF:-20}" "${stats[@]}" \
        "CZ_SHADER_SPV=$A2M" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 ) &
    local runner=$!

    # THE BILL, sampled where it matters. This item is the first one that COSTS cpu, so a
    # report of its benefit without its price is half a measurement (gotcha 344). The
    # sample is triggered by the operator's own F9 rather than by a wall clock, because
    # the number that matters is the one during the SOAK and nobody can predict when that
    # starts (gotcha 251 — an event, not a clock). /proc only; it never stops the game.
    (
      for _ in $(seq 1 3600); do
          kill -0 $runner 2>/dev/null || exit 0
          grep -q "\[vk\] capture: wrote" "$OUT/$tag.log" 2>/dev/null && break
          sleep 1
      done
      sleep 5
      P=""
      for p in $(pgrep -f cz_runtime 2>/dev/null); do
          c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
          case "$c" in cz_runtime*) P=$p ;; esac
      done
      [ -n "$P" ] && python3 "$ROOT/tools/part50_thread_cpu.py" 20 > "$OUT/$tag.threadcpu" 2>&1
    ) &

    wait $runner
    wait
    echo "  ARM $tag finished. log: $OUT/$tag.log"
    grep -c "no translated shader" "$OUT/$tag.log" \
        | sed 's/^/  shaders the cache lacked: /'
    grep -a "guard prehash" "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -a "PARALLEL GUARD SLOT MIX-UP" "$OUT/$tag.log" | head -1 | sed 's/^/  *** /'
    grep -aE "^\[vkprof\] [0-9]" "$OUT/$tag.log" | tail -3 | sed 's/^/  /'
    grep -a "process total" "$OUT/$tag.threadcpu" 2>/dev/null | sed 's/^/  /'
}

if [ "${ARM:-single}" = ab ]; then
    cat <<'BANNER'

  ###################################################################
  #  TWO ARMS, CHAINED. Quit arm A and arm B starts by itself.      #
  #                                                                 #
  #  ARM A  the new build: guards on four workers, no readback copy #
  #  ARM B  both of those switched OFF — the runtime as it was      #
  #         before today, same binary                               #
  #                                                                 #
  #  THE BEST THING YOU CAN DO IS THE SOAK YOU INVENTED LAST TIME:  #
  #  go to the heaviest place you know, stand still, and hold for   #
  #  ~3 minutes in EACH arm. It gave a bin with 7,773 comparable    #
  #  frames where a walk gives ~1,300, and it is the only           #
  #  measurement here that is not sitting on the frame cap.         #
  #                                                                 #
  #  PRESS F9 WHEN THE SOAK STARTS. It marks the soak in the stats  #
  #  file AND it triggers the per-thread CPU sample, which this     #
  #  time is half the result: the change spends idle cores to       #
  #  shorten the busy one, so its cost has to be measured too.      #
  #                                                                 #
  #  Both arms carry ~3 ms of instrument, so neither is as smooth   #
  #  as the shipping build. Judge the DIFFERENCE, not the feel.     #
  ###################################################################

BANNER
    run part53on
    echo; echo "  Arm A finished. Starting arm B — the pre-part-53 runtime."; echo
    run part53off CZ_VK_NO_PARALLEL_GUARD=1 CZ_VK_PRESENT_STAGING=1
    cat <<EOM

  Both arms done. Read them with:
    python3 tools/frame_perf_bins.py --a $OUT/part53off.stats --b $OUT/part53on.stats

  Unlike part 52's A/B, arm B restores the WHOLE part — both items have runtime switches
  — so this measures part 53 rather than one of its items. Headlessly the split is
  ~2.2 ms of guard and ~0.8 ms of readback.

  ORDER CONFOUND, stated because one run an arm cannot remove it: arm A ran first, on a
  cooler machine. Thermal drift would move every phase together, though, and the
  prediction here is specific — the \`record\` GUARD column and \`readback\` collapse and
  little else moves — so the two are distinguishable.
EOM
else
    run part53
fi
