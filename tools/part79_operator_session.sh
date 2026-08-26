#!/bin/bash
# PART 79's OPERATOR SESSION — the texture flush stops waiting, and this is the ONLY place
# the item can be priced.
#
# WHY IT NEEDS THEM. `FlushTextureUploads` submitted AND WAITED; it now submits into a
# three-slot ring and waits only on the slot it is about to reuse. On my autonomous route the
# mechanism is decisively gone — the flush goes 999-1138 us to 106-114 us, −89.8%, with three
# runs an arm agreeing to 0.7 ms — **and the run is a NULL on frame time**, because that route
# is GPU-bound at 6,200 draws and the pump simply moved its blocking to the frame fence
# (median fence 0.699 -> 1.136 ms, `phase5-notes.md` §6dw §3, gotcha 466).
#
# The operator's load is the opposite regime and it is where the number lives:
#
#   * their flush count is **27x mine per second** — 1,841 flushes in 150 s against my 67 in
#     148 — because the batch flushes once per FRAME and their uploads are spread across play
#     where mine are concentrated into one DebugJump load (§6dt §3);
#   * their fence is **0.00 ms at every band from 3,000 draws up** and their GPU headroom is
#     1.93-2.38 ms (§6dv §2), so a CPU saving there converts nearly 1:1.
#
# **The pre-registered prediction, stated before they play.** Part 77 measured 1,092.5 ms of
# `vkQueueWaitIdle` in a 150 s session. That should now be ~0. Spread over ~1,841 flushes on
# ~17% of frames it is ~0.59 ms on the frames it touches — about 5% of an 11.5 ms frame there
# and nothing on the rest. **So the honest expectation is that they will NOT feel it**, and
# what makes this run worth doing is the counter, not the impression. What would REFUTE the
# reading is `texture flush` still reading ~1,000 us a flush, a non-zero slot-stall count, or
# the fence rising to absorb the saving the way mine did.
#
# ALSO ARMED, and free: `CZ_VK_GPU_PASSES=1` carries the PASS EXTENT CENSUS added in part 79
# (§6dx). Their post chain is a larger share of their GPU frame than of mine (14.4% against
# 9.9%, §6dv §4) and nobody has seen it at their resolution.
#
# THE PICTURE IS THE OTHER HALF. This change cannot alter a pixel by construction, and the
# gate says so — `STILL=1` era medians put the control arm INSIDE the null on all three
# statistics while the broken build reads 36,799x. But this project has twice shipped a
# defect only the operator's eye could see (§6bo, part 60) with every automatic check green,
# so their verdict is collected explicitly.
#
# Usage:  tools/part79_operator_session.sh                 # GPU split + extent census
#         RES=2560x1440 tools/part79_operator_session.sh   # a different internal resolution
#         RES=desktop   tools/part79_operator_session.sh   # inherit it, un-pinned
#         NOSAFE=1      tools/part79_operator_session.sh   # die like a player
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part79-operator"
mkdir -p "$OUT"

for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_auto 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing"; exit 2
done

TAG="p79_$(date +%m%d_%H%M)"
LOG="$OUT/$TAG.log"
TRACE="$OUT/$TAG.trace"
mkdir -p "$OUT/$TAG"

# The PRIMARY output's mode, not the first one xrandr happens to name.
prim="$(xrandr 2>/dev/null | awk '/ connected primary /{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){split($i,a,"+"); print a[1]; exit}}')"
[ -z "$prim" ] && prim="$(xrandr 2>/dev/null | awk '/ connected/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){split($i,a,"+"); print a[1]; exit}}')"

RES="${RES:-3440x1440}"
extra=()
if [ "$RES" != "desktop" ]; then
    extra+=("CZ_VK_RES=$RES")
fi
# God mode and no death sequence, but NOT "zombies ignore all humans": the crowd's own
# behaviour is the load being measured, and turning the AI off would change the very thing
# this session exists to profile.
if [ -z "${NOSAFE:-}" ]; then
    extra+=(CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE")
fi

echo "==================================================================="
echo "  PART 79 SESSION — the texture flush no longer waits"
echo
xrandr 2>/dev/null | grep -w connected | sed 's/^/  display:  /'
echo "  primary:  ${prim:-unknown}"
if [ "$RES" = "desktop" ]; then
    echo "  internal: from the desktop (UN-PINNED — say which it chose when quoting a number)"
else
    echo "  internal: $RES (pinned)"
    [ "$RES" != "${prim:-}" ] && echo "            (note: that is not the primary's mode — deliberate, but say so)"
fi
echo
echo "  Quit with the game's own menu when you have had enough — the split prints at exit."
echo
echo "  F7 :  MARK A STUTTER — press it the moment you feel one."
echo "        Stamped into the log AND the trace; I read backwards ~1 s from each mark."
echo "  F9 :  screenshot -> $OUT/$TAG        (free until pressed, since part 76)"
echo "  F8 :  burst, every frame for 1 s -> $OUT/$TAG"
echo
echo "  WHAT I MOST WANT FROM THIS RUN: a big crowd, held for a while, and whether"
echo "        the PICTURE is identical to last session anywhere you look."
echo
echo "  Part 79 stopped the texture-upload flush from waiting for the GPU. Last"
echo "  session that wait was 1,092.5 ms of your 150 seconds. On my own route the"
echo "  mechanism is gone (-89.8%) but the frame time did not move at all, because"
echo "  my route is GPU-bound and the pump just waits somewhere else instead."
echo "  YOUR crowd is CPU-bound with ~2 ms of GPU headroom, so this is the only"
echo "  place the change can show up."
echo
echo "  BE PREPARED FOR IT TO FEEL THE SAME. The arithmetic says ~0.6 ms on about"
echo "  one frame in six — real, but well under what an eye resolves. The counter"
echo "  is the measurement here; your impression is a bonus."
echo
echo "  Nothing about this change can alter a pixel. If the picture differs anywhere"
echo "  from the last session, that is a defect and I want to know exactly where."
echo
echo "  COST:  CZ_VK_PROFILE is 2-4 ms a frame AND IT INVERTS THE REGIME (gotcha 454)."
echo "         This session is armed with CZ_VK_GPU_PASSES instead, which is free, and"
echo "         CZ_VK_FRAME_TRACE, which is one line a frame."
echo
echo "  log:   $LOG"
echo "  trace: $TRACE"
echo "==================================================================="

( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
    CZ_VK_GPU_PASSES=1 \
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
echo "  stale present slots:      $(grep -ac 'present slot describes frame' "$LOG")"
echo
echo "  --- THE PART 79 ITEM, and the two lines that decide it ---"
grep -a "texture upload ring" "$LOG" | sed 's/^/  /'
grep -a "texture flush:" "$LOG" | tail -1 | sed 's/^/  /'
grep -a "texture upload batch:" "$LOG" | tail -1 | sed 's/^/  /'
grep -a "immediate submits:" "$LOG" | tail -1 | sed 's/^/  /'
echo
echo "  --- THE GPU SPLIT AND THE PASS EXTENT CENSUS (part 79 §6dx) ---"
sed -n '/GPU per-region split/,/(a region.s time is/p' "$LOG" | sed 's/^/  /' 
echo
python3 "$ROOT/tools/part76_regime.py" "$TRACE" 2>/dev/null || true
echo
grep -a "^\[fps\]" "$LOG" | tail -10
