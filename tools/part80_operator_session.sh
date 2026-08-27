#!/bin/bash
# PART 80's OPERATOR SESSION — one yes/no question: IS THE AFTER-LOAD HITCH GONE?
#
# WHY. Part 79's session produced exactly one complaint — *"only felt hitches at the start
# right after loading"* — and it localised to two frames, 87.3 ms and 158.4 ms, landing 1.3
# and 1.4 seconds after a texture load burst. Both were the CROSS-FRAME STREAM STORE GROWING:
# `WaitAllFramesIdle` + `vkDeviceWaitIdle` + a host-visible allocation and MAP + freeing the
# old buffer, all on the pump inside ONE frame, measured at **71.7 ms for a 256 MB step**
# (§6dz). The store now starts at 512 MB — the size their own session grew to — instead of
# 128, so on the autonomous route the growth window's worst frame goes **90.9/90.3 ms ->
# 36.7/33.6** with zero growths, for +10 ms on a boot frame already at 235.
#
# **THE PRE-REGISTERED PREDICTION, stated before they play.** `stream store grows: 0`, and no
# felt hitch 1-2 seconds after a load. What would REFUTE it:
#
#   * a `stream store grown to ... MB on frame N` line appearing anyway — 512 was not enough
#     for this session, and the fix is a bigger start or growing in blocks;
#   * a hitch right after loading WITH no growth in the log — the part-79 attribution was
#     wrong, or there is a second cause at the same moment;
#   * the boot or first-load frame being noticeably worse — the +10 ms landed somewhere it
#     can be felt after all.
#
# **THE OTHER THING TO WATCH FOR IS A NON-EVENT.** Two spikes in their last session (50.0 and
# 60.3 ms, far from any load) were NOT growths and were NOT felt, which puts their threshold
# between 60 and 87 ms in a crowd. Those are expected to still be there and still unnoticed.
# If they now DO notice something in open play, that is new information and it is part 80's
# item 0.
#
# ALSO ARMED, and free: `CZ_VK_GPU_PASSES=1` with the pass extent census (§6dx).
#
# Usage:  tools/part80_operator_session.sh                 # GPU split + extent census
#         RES=2560x1440 tools/part80_operator_session.sh   # a different internal resolution
#         RES=desktop   tools/part80_operator_session.sh   # inherit it, un-pinned
#         PERSIST=128   tools/part80_operator_session.sh   # the CONTROL ARM: the old default
#         NOSAFE=1      tools/part80_operator_session.sh   # die like a player
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part80-operator"
mkdir -p "$OUT"

for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_auto 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing"; exit 2
done

TAG="p80_$(date +%m%d_%H%M)"
LOG="$OUT/$TAG.log"
TRACE="$OUT/$TAG.trace"
mkdir -p "$OUT/$TAG"

# The PRIMARY output's mode, not the first one xrandr happens to name.
prim="$(xrandr 2>/dev/null | awk '/ connected primary /{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){split($i,a,"+"); print a[1]; exit}}')"
[ -z "$prim" ] && prim="$(xrandr 2>/dev/null | awk '/ connected/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+x[0-9]+\+/){split($i,a,"+"); print a[1]; exit}}')"

RES="${RES:-3440x1440}"
extra=()
# THE CONTROL ARM, and it must be visible in the log rather than only in this shell
# (gotcha 151). PERSIST=128 restores the pre-part-79 start, i.e. the build that grew twice
# in their last session.
if [ -n "${PERSIST:-}" ]; then
    extra+=("CZ_VK_PERSIST_MB=$PERSIST")
    echo "  ** CONTROL ARM: CZ_VK_PERSIST_MB=$PERSIST (the shipping default is 512)"
fi
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
echo "  PART 80 SESSION — is the after-load hitch gone?"
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
echo "  WHAT I NEED FROM THIS RUN IS ONE YES/NO:"
echo "        LOAD A ZONE, THEN KEEP PLAYING FOR ~10 SECONDS AND WATCH."
echo "        Last session you felt a hitch about 1.3 seconds after loading,"
echo "        twice. Is it gone?"
echo
echo "  It was the cross-frame stream store doubling itself: a whole frame of"
echo "  71.7 ms spent waiting for the GPU, allocating 256 MB, mapping it and"
echo "  freeing the old one. It now starts big enough not to grow at all."
echo
echo "  The cost is ~10 ms added to the BOOT frame, which is already ~235 ms."
echo "  If the boot or the first load feels worse than last time, say so —"
echo "  that would mean I moved the hitch instead of removing it."
echo
echo "  Load more than one zone if you can. One clean load is a data point;"
echo "  three is an answer."
echo
echo "  Everything else is unchanged from last session, so the picture should"
echo "  still look identical. It did last time and that closed the item."
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
echo "  --- THE PART 80 QUESTION: this must read 0 growths ---"
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
