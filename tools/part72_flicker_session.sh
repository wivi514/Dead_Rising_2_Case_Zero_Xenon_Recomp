#!/bin/bash
# PART 72 — THE SKY FLICKER: is it the constant gather, and did the fix close it?
# Three short arms, same route each time, the operator drives.
#
# THE REPORT (2026-08-23, operator, during the pipeline session's two arms): *"Sky flicker
# from half the screen switching from right to left depending of moment."* Not seen — or
# not noticed — in the two stand-still soak arms before it, which is expected either way:
# those arms do not move, and this one travels through the world.
#
# WHY A NUMBER CANNOT ANSWER THIS. `CZ_VK_VERIFY_CONST_GATHER` read **0 disagreements over
# 17,948,265 checks** in the same build, and it was right to: the gather DOES copy what the
# shader's list names. The defect candidate is what happens to the memo slot afterwards,
# which is outside that verifier's scope. A verifier's scope is not the feature's blast
# radius, and this is what the operator's eye is for.
#
# THE CANDIDATE, found by inspection rather than by the instrument. A memo slot being
# topped up already holds PATCHED c0..c3 (the fov slider and the 21:9 wide patch rewrite
# them on the miss path). The gather writes only the registers the NEW shader reads, so a
# shader reading only SOME of c0..c3 left a window part raw and part patched — and
# `SceneXformForm` reads all sixteen floats to decide whether it is a scene projection at
# all, so such a window can be mis-recognised and then scaled on the wrong rows. **This
# title renders in left/right 640-wide tiles**, and tile 0 misses the memo while tile 1
# hits it, which is a mechanism for one half of the screen differing from the other and for
# which half it is to change with what else was drawn between them. Fixed in `a55df20`.
#
# THE ARMS, in the order that answers the most with the fewest runs:
#
#   nogather  CZ_VK_NO_CONST_GATHER=1 — the gather off entirely, and as of `be1d9d7` this
#             really is the pre-part-72 renderer. **IF IT STILL FLICKERS HERE, THE GATHER
#             IS NOT THE CAUSE** and everything after it is about something else. This is
#             first precisely because it is the arm that can exonerate the suspect.
#   fixed     the shipped default, gather ON, with a55df20. If nogather was clean and this
#             is clean, the fix closed it.
#   prefix    gather ON with CZ_VK_GATHER_NO_C0_REFRESH=1 — the fix REVERTED, in the same
#             binary. This is the positive control: it must bring the flicker BACK. Without
#             it, "the flicker stopped" is indistinguishable from "we did not trigger it
#             this time", which on an intermittent visual defect is the whole difficulty.
#
# **THE THIRD ARM IS THE ONE PEOPLE SKIP.** An intermittent defect that disappears after a
# fix is the single easiest thing in this project to declare fixed wrongly (gotcha 30 in
# its visual form), and the operator has already said the flicker depends on the moment.
#
# ROUTE, all three arms the same: play from the title through the load to the heavy spot,
# LOOK AT THE SKY, and turn so the horizon sweeps across both halves of the screen. ~2 min.
#
# Usage:  tools/part72_flicker_session.sh
#         ORDER=nogather tools/part72_flicker_session.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part72-flicker"
ORDER="${ORDER:-nogather,fixed,prefix}"
FPS="${FPS:-500}"
FLAGS="${FLAGS:-CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS}"
mkdir -p "$OUT"

for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_p72f 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing to start a second"; exit 2
done
BIN=cz_runtime_p72f
cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
echo "snapshot: $BIN <- cz_runtime ($HEAD)"
STAMP="$(date +%m%d_%H%M)"
[ -f "$ROOT/assets/save/cz_settings.txt" ] && echo "preflight: cz_settings.txt ->" \
    "$(grep -av '^#' "$ROOT/assets/save/cz_settings.txt" | tr '\n' ' ')"

arm_desc() {
    case "$1" in
      nogather) echo "CZ_VK_NO_CONST_GATHER=1 — gather OFF. Flicker here = NOT the gather" ;;
      fixed)    echo "the shipped default — gather ON with the c0..c3 refresh fix" ;;
      prefix)   echo "CZ_VK_GATHER_NO_C0_REFRESH=1 — the fix REVERTED. Must bring it BACK" ;;
      *)        echo "UNKNOWN ARM" ;;
    esac
}
run_arm() {
    local arm="$1" n="$2" total="$3"
    local tag="p72f_${STAMP}_${n}_${arm}"
    local extra=()
    case "$arm" in
      nogather) extra+=(CZ_VK_NO_CONST_GATHER=1) ;;
      fixed)    ;;
      prefix)   extra+=(CZ_VK_GATHER_NO_C0_REFRESH=1) ;;
      *) echo "!! unknown arm '$arm'"; return 1 ;;
    esac
    cat <<BANNER

===================================================================
  ARM $n of $total:  $arm
  $(arm_desc "$arm")

  log: $OUT/$tag.log

  >>> Same route every arm: title -> load -> your heavy spot.
  >>> LOOK AT THE SKY and turn so the horizon sweeps both halves
  >>> of the screen. ~2 min. Then QUIT and say whether it flickered.
===================================================================

BANNER
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=10 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=$FLAGS" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
        "${extra[@]}" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    echo "  arm $n ($arm) finished — did the sky flicker? (F9 in the moment captures it)"
}

engaged() {
    local arm="$1" f="$2"
    case "$arm" in
      nogather) grep -aq "CZ_VK_NO_CONST_GATHER=1 — the full 256-register window" "$f" &&
                grep -aq "const gather: 0.0% of window copies gathered" "$f" ;;
      fixed)    grep -aq "const gather: [1-9]" "$f" &&
                ! grep -aq "CZ_VK_NO_CONST_GATHER" "$f" &&
                ! grep -aq "CZ_VK_GATHER_NO_C0_REFRESH" "$f" ;;
      prefix)   grep -aq "CZ_VK_GATHER_NO_C0_REFRESH=1" "$f" &&
                grep -aq "const gather: [1-9]" "$f" ;;
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
    f=$(ls "$OUT"/p72f_"$STAMP"_*_"$a".log 2>/dev/null | head -1)
    echo; echo "--- $a  ($(basename "${f:-MISSING}"))"
    if [ -z "$f" ] || ! engaged "$a" "$f"; then
        echo "  ** NOT ENGAGED — this arm's verdict is NOT reportable."; fail=1
        [ -n "$f" ] || continue
    else
        echo "  ENGAGED."
    fi
    grep -a "const gather:" "$f" | tail -1 | sed 's/^/    /'
    grep -a "CZ_VK_GATHER_NO_C0_REFRESH\|CZ_VK_NO_CONST_GATHER" "$f" | tail -1 | sed 's/^/    /'
done
cat <<'READ'

  THE VERDICT IS YOURS, NOT A COUNTER'S. Read it in this order:
   1. nogather flickered too  -> the gather is EXONERATED. Stop; it is something else,
                                 and the two arms after it say nothing about it.
   2. nogather clean, fixed clean, prefix FLICKERED -> the diagnosis and the fix are both
                                 confirmed, and that third arm is what makes it a result
                                 rather than a hope.
   3. nogather clean, fixed clean, prefix ALSO clean -> we did not trigger it this run.
                                 The fix is UNCONFIRMED, not confirmed. Re-run, or find a
                                 place that reproduces it reliably first.
   4. fixed still flickered    -> the c0..c3 refresh was not the mechanism. The next
                                 suspect is the gather list itself for the sky shader.
READ
exit $fail
