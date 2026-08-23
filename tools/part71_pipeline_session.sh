#!/bin/bash
# PART 71's SECOND SESSION — the stutter, and the one question that decides the whole plan.
#
# WHAT THE FIRST SESSION FOUND. Four soak arms, and `>2x med` read **0.0% in the steady
# state of every one of them** — the stutter is not in the soak at all. It is entirely in
# the first ~50 seconds, and the worst single frames were:
#
#     arm 1 base       3891.85 ms   (plus a 1026 ms)      <- cold shader set
#     arm 3 noclip      807.46 ms   (plus 400 and 282)    <- cold shader set
#     arm 2 nofold      343.82 ms
#     arm 4 nogamefov   290.61 ms
#
# which is the operator's felt ranking exactly, and 1 and 3 are precisely the two arms
# that loaded a shader set the driver had not compiled before. **This renderer created
# every pipeline with `VK_NULL_HANDLE` as the cache from phase 5 until part 71** — 503-545
# of them per session, compiled on the PUMP THREAD the moment a new draw state is first
# seen. Part 71 added a `VkPipelineCache` seeded from and written back to disk.
#
# THE HYPOTHESIS HAS BEEN INFERRED THREE TIMES BEFORE AND NEVER MEASURED. The creation
# site's own comment says so, and says it once FAILED a pre-registered prediction. Its
# timer was gated on `CZ_VK_PROFILE`, which costs 2-4 ms a frame and is therefore off in
# every session whose stutter anyone has ever reported. That timer is now unconditional
# (two clock reads per pipeline, ~500 a run) and prints a TOP-FRAME table:
#
#     [vk]   pipeline creation: N pipelines, X ms total, worst single Y ms @frame F
#     [vk]     frame  12345:  3810.2 ms building 7 pipeline(s)
#
# **That table is the whole point of this session.** If the frame that took 3,891 ms shows
# up in it, the diagnosis is settled; if it does not, the pipeline theory dies for the
# fourth time and the tail statistic goes looking somewhere else. Either outcome is worth
# the session, which is what makes it a measurement rather than a demonstration.
#
# THE FOURTH ARM ANSWERS A DIFFERENT AND LARGER QUESTION: **is 28 ms at 3440x1440 CPU- or
# GPU-limited?** Everything in `docs/perf-state-parked.md` §2 is a CPU item, and if the
# GPU is the limiter at the operator's real resolution then all of it is worth zero and
# the plan needs rewriting. `CZ_VK_RES=1720x720` is a quarter of the pixels at the SAME
# aspect ratio — so the guest's geometry, culling and draw set are bit-identical and only
# the rasterisation target changes. Frame time barely moves -> CPU-bound, the plan stands.
# Frame time falls with the pixels -> GPU-bound, and the plan is wrong.
#
# Usage:  tools/part71_pipeline_session.sh
#         ORDER=warm tools/part71_pipeline_session.sh        # any subset
#         SECS=90 tools/part71_pipeline_session.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part71-pipeline"
ORDER="${ORDER:-nocache,cold,warm,lowres}"
FPS="${FPS:-500}"
FLAGS="${FLAGS:-CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS}"
# THE CACHE FILE THIS SESSION CONTROLS, pinned rather than left at the default path, so
# `cold` is provably cold and `warm` is provably reading what `cold` wrote. Leaving it at
# the default would make the pair depend on whatever earlier runs happened to leave there.
PCF="$OUT/session_pipeline_cache.bin"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_p71p 2>/dev/null); do
    busy="$busy  $p $(cat "/proc/$p/comm" 2>/dev/null)"$'\n'
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

BIN=cz_runtime_p71p
cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
echo "snapshot: $BIN <- cz_runtime ($HEAD)"
STAMP="$(date +%m%d_%H%M)"

run_arm() {
    local arm="$1" n="$2" total="$3"
    local tag="p71pipe_${STAMP}_${n}_${arm}"
    local extra=() what="" ask=""
    case "$arm" in
      nocache)
        extra+=(CZ_VK_NO_PIPELINE_CACHE=1)
        what="CZ_VK_NO_PIPELINE_CACHE=1 — every pipeline compiled from scratch (pre-part-71)"
        ask="Play from the title through the load to your heavy spot. EXPECT STUTTER." ;;
      cold)
        # Deleted HERE and not at the top, so re-running a subset still gets a cold arm.
        rm -f "$PCF"
        extra+=("CZ_VK_PIPELINE_CACHE_FILE=$PCF")
        what="the pipeline cache, DELETED first — this run pays the compiles and writes them"
        ask="Same route. Expect the same stutter as the first arm; this arm is the SEED." ;;
      warm)
        extra+=("CZ_VK_PIPELINE_CACHE_FILE=$PCF")
        what="the pipeline cache, WARM — reading what the 'cold' arm just wrote"
        ask="Same route. **THIS IS THE FIX** — say whether the post-load stutter is gone." ;;
      lowres)
        extra+=("CZ_VK_PIPELINE_CACHE_FILE=$PCF" "CZ_VK_RES=1720x720")
        what="CZ_VK_RES=1720x720 — a QUARTER of the pixels, same aspect, same draw set"
        ask="Go to the heavy spot and STAND STILL ~$((SECS_DEF / 60)) min. It will look soft; ignore that." ;;
      *) echo "!! unknown arm '$arm'"; return 1 ;;
    esac
    cat <<BANNER

===================================================================
  ARM $n of $total:  $arm
  $what

  >>> $ask
  >>> Then QUIT. The next arm starts by itself.

  log: $OUT/$tag.log
===================================================================

BANNER
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 \
        "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=5 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=$FLAGS" \
        "${extra[@]}" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    echo "  arm $n ($arm) finished."
}
SECS_DEF="${SECS:-120}"

# Engagement, from a line the FEATURE prints — never from the variable (gotcha 408).
engaged() {
    local arm="$1" f="$2"
    case "$arm" in
      nocache)  grep -aq "CZ_VK_NO_PIPELINE_CACHE=1 — every pipeline" "$f" && \
                grep -aq "pipeline creation:.*\[no pipeline cache\]" "$f" ;;
      # Two-sided: cold must seed from NOTHING and must write something back. A cache that
      # loads and is never written is the silent failure that looks exactly like success.
      cold)     grep -aq "pipeline cache: 0 bytes seeded.*COLD" "$f" && \
                grep -aq "pipeline cache: [1-9][0-9]* bytes written" "$f" ;;
      warm)     grep -aq "pipeline cache: [1-9][0-9]* bytes seeded" "$f" && \
                ! grep -aq "COLD" "$f" ;;
      lowres)   grep -aq "internal resolution 1720x720" "$f" ;;
      *) return 1 ;;
    esac
}

n=1
IFS=',' read -ra arms <<< "$ORDER"
for a in "${arms[@]}"; do run_arm "$a" "$n" "${#arms[@]}"; n=$((n + 1)); done

echo
echo "==================================================================="
echo "  ALL ARMS DONE — $HEAD"
fail=0
for a in "${arms[@]}"; do
    f=$(ls "$OUT"/p71pipe_"$STAMP"_*_"$a".log 2>/dev/null | head -1)
    echo
    echo "--- $a  ($(basename "${f:-MISSING}"))"
    if [ -z "$f" ] || ! engaged "$a" "$f"; then
        echo "  ** NOT ENGAGED — not reportable. (gotcha 408)"
        fail=1
        [ -n "$f" ] || continue
    else
        echo "  ENGAGED."
    fi
    grep -a "pipeline cache:" "$f" | tail -2
    grep -a "internal resolution 1720x720\|internal resolution 3440x1440 from" "$f" | tail -1
    echo "  THE TABLE THIS SESSION EXISTS FOR:"
    grep -a "pipeline creation:" "$f" | tail -1
    grep -a "^\[vk\]     frame " "$f" | head -12
    echo "  the five worst [fps] windows by 'worst':"
    grep -a "^\[fps\]" "$f" | sed 's/.*worst \([0-9.]*\) ms.*/\1 &/' | sort -rn | head -5 | cut -d' ' -f2-
done
echo
echo "  READ IT AS: does the top-frame table account for the [fps] 'worst' frames?"
echo "  If yes, pipeline creation IS the stutter and 'warm' should be visibly cleaner."
echo "  If the biggest [fps] worst has no matching pipeline frame, the theory is DEAD"
echo "  for the fourth time and that is the finding."
echo
echo "  lowres vs warm at a matched draw band answers CPU-vs-GPU:"
echo "    python3 tools/part54_fps_bins.py $OUT/p71pipe_${STAMP}_*_warm.log --arm $OUT/p71pipe_${STAMP}_*_lowres.log --band 500"
exit $fail
