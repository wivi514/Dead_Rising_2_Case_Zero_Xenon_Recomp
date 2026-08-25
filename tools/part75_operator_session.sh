#!/bin/bash
# PART 75's VERIFICATION SESSION — the operator plays a crowd with the per-frame CPU/GPU
# profiler armed, so "is anything left" can be answered from data rather than from a feel.
#
# WHY THIS IS NOT `play_session.sh`. That one deliberately carries no profiler, because
# every frame-rate instrument here has a bill big enough to change the answer. This one is
# the opposite by request: the whole point is the phase breakdown, so it accepts the bill
# and SAYS SO. `CZ_VK_PROFILE` costs 2-4 ms a frame — about 3% of a 100 ms stutter frame
# and a fifth of a 12 ms one — so **the frame rate seen here is worse than the game the
# fix actually delivers.** Judge SHAPE from this run (which phase is big), not absolute fps.
#
# WHAT IT RECORDS
#   * `CZ_VK_FRAME_TRACE` — one line per presented frame with ALL TWENTY-ONE phase columns
#     (part 75 split `constants` five ways), plus the part-74 decomposition
#     `wall = CPUrec + fence + sleep + residual` (which SUM) alongside `GPU` (which
#     overlaps, and comes from the frame's OWN command-buffer timestamps).
#   * `F7` stamps the current frame into both the log and the trace. Human reaction is
#     200-500 ms, so a mark names a NEIGHBOURHOOD — the reader takes the worst frame in the
#     ~1 s before it, which is why every frame is written and not only the extremes.
#   * `CZ_SHADER_DUMP` into the persistent directory, because a run that reaches new ground
#     is the only way this cache grows and a missing shader is one silent log line.
#
# THE RESOLUTION IS CHECKED AND ANNOUNCED, and that is a part-75 lesson paid for in a lost
# afternoon: `WideMode()` is `9W > 16H` on the INTERNAL resolution, so the entire 21:9
# projection path — which is what part 75's fix removed the cost of — EXISTS at 3440x1440
# and does not exist at 2560x1440. A session run at 16:9 cannot show what the fix did.
#
# Usage:  tools/part75_operator_session.sh                 # profiler on, god mode on
#         RES=3440x1440 tools/part75_operator_session.sh   # pin it if the desktop drifted
#         NOSAFE=1 tools/part75_operator_session.sh        # die like a player
#         tools/part75_operator_session.sh CZ_VK_PATCH_IN_ARENA=1   # the pre-fix arm
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part75-operator"
mkdir -p "$OUT"

for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_auto 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing"; exit 2
done

TAG="p75_$(date +%m%d_%H%M)"
LOG="$OUT/$TAG.log"
TRACE="$OUT/$TAG.trace"
mkdir -p "$OUT/$TAG"

extra=()
[ -n "${RES:-}" ] && extra+=("CZ_VK_RES=$RES")
# God mode and no death sequence, but NOT "zombies ignore all humans": the crowd's own
# behaviour is part of the load being measured, and turning the AI off would change the
# very thing this session exists to profile. Staying alive is a harness concern; the
# zombies are the subject.
if [ -z "${NOSAFE:-}" ]; then
    extra+=(CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE")
fi

disp="$(xrandr 2>/dev/null | awk '/ connected/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){split($i,a,"+"); print a[1]; exit}}')"
echo "==================================================================="
echo "  PART 75 VERIFICATION SESSION — profiler ARMED"
echo
echo "  desktop:  ${disp:-unknown}"
if [ -n "${RES:-}" ]; then
    echo "  internal: $RES (pinned)"
elif [ "${disp:-}" != "3440x1440" ]; then
    echo "  ** THE DESKTOP IS NOT 3440x1440."
    echo "     WideMode() is 9W>16H on the internal resolution, so the whole 21:9"
    echo "     projection path — the thing part 75 made cheap — does not run at 16:9."
    echo "     Set the display back to 3440x1440, or re-launch with RES=3440x1440."
fi
echo
echo "  F7 :  MARK A STUTTER — press it the moment you feel one."
echo "        Stamped into the log AND the trace; I read backwards ~1 s from each mark."
echo "  F9 :  screenshot -> $OUT/$TAG"
echo "  F8 :  burst (every frame for 1 s) -> $OUT/$TAG"
echo
echo "  COST:  CZ_VK_PROFILE is 2-4 ms a frame. The game will feel slightly worse than"
echo "         it really is — read the SHAPE from this run, not the absolute fps."
echo
echo "  log:   $LOG"
echo "  trace: $TRACE"
echo "==================================================================="

( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
    CZ_VK_PROFILE=10 \
    "CZ_VK_FRAME_TRACE=$TRACE" \
    "CZ_CAPTURE_KEY=$OUT/$TAG" \
    "CZ_BURST_DUMP=$OUT/$TAG" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "${extra[@]}" "$@" \
    ./cz_runtime > "$LOG" 2>&1 )

echo
echo "  finished."
echo "  internal resolution:      $(grep -ao 'internal resolution [0-9x]*' "$LOG" | head -1)"
echo "  stutter marks (F7):       $(grep -ac '\*\* MARK ' "$LOG")"
echo "  frames traced:            $(( $(wc -l < "$TRACE" 2>/dev/null || echo 1) - 1 ))"
echo "  shaders the cache lacked: $(grep -ac 'no translated shader' "$LOG")"
echo "  slot mix-ups:             $(grep -ac 'PARALLEL GUARD SLOT MIX-UP' "$LOG")"
echo "  const memo stale:         $(grep -ac 'CONST MEMO STALE' "$LOG")"
echo
grep -a "^\[fps\]" "$LOG" | tail -10
