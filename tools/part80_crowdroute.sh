#!/bin/bash
# THE CROWD ROUTE — the operator's own route into a 9,300-draw crowd, replayed unattended.
#
# WHY THIS EXISTS AND WHY IT REPLACES `autoroute.sh` FOR CPU WORK.
#
# `part80-kickoff.md` §1's item 1 is parallel command recording, and its measurement rule is
# blunt: **8,000+ draws or not at all**. Below that the autonomous route is GPU-bound and a
# CPU saving reads as a dead null — part 79 spent a six-run campaign re-learning exactly
# that (`phase5-notes.md` §6dw §3, gotchas 453 and 466). `autoroute.sh` selects the
# DebugJump screen's Case 0-2 and tops out at **~6,200 draws**, so every CPU item on the
# board was unmeasurable without an operator sitting through three runs per arm — an hour
# of their evening per A/B, which is gotcha 190 in its most expensive form.
#
# The operator broke that open by playing the route once themselves with the newly
# timestamped `CZ_INPUT_TRACE`, and `tools/part80_transcribe_route.py` turned it into
# `config/part80_crowd_route.seq`. It is THEIR route, not a synthesised one:
#
#   START            open the main menu from the title
#   A, B             the accidental detour into the start-game screen and straight back out
#                    — KEPT DELIBERATELY. It is two presses and about four seconds, and
#                    removing it would make this a route nobody has ever run, on the theory
#                    that the menu state after A-then-B equals the state before A. That is
#                    probably true, it is not measured, and a route that silently differs
#                    from the recorded one is what this whole file exists to avoid.
#   UP, A            up to the DebugJump entry — which lives IN THE MAIN MENU here and is
#                    reached with the PAD, not through the host's F2 bridge. That is why
#                    this route has no `[debug] ... through frontend manager` line and no
#                    WAITJUMP: there is no host-side screen request to wait on.
#   RIGHT, A         pick the destination and confirm
#   ~14 s of walking then ~45 s of camera sweeps, all ANALOG — see below
#
# It settles at **9,300-9,700 draws a frame** and holds there, which is the operator's own
# crowd load and half again what `autoroute.sh` reaches.
#
# THE ROUTE IS ANALOG, AND THAT WAS NOT OPTIONAL. The first transcription used the eight
# cardinal stick names and the operator watched it and said: *"the character goes forward
# the whole time while I was often slightly to the left so it runs into the sheriff office
# building instead of middle of street."* The trace agrees — over the 14.5-second walk the
# Y axis is pinned at 32767 while X drifts between -5,467 and +3,993. That is steering, and
# `LSUP` is (0, 32767). So `CZ_FAKE_PRESS_SEQ` grew `LS<x>/<y>` entries, and `+` to hold
# both sticks at once, because they turn the camera WHILE walking and the camera decides
# the draw set.
#
# THE ONE FRAGILE NUMBER, named so it is checked rather than trusted: the leading silence.
# The replay's clock starts at the title's FIRST INPUT POLL; the transcription's clock
# started at PROCESS START, and the gap between them is however long this boot took — a
# distribution, not a constant (gotcha 75). Everything after the first press is anchored to
# the press before it and is therefore sound; only the lead-in is a fit to one afternoon.
# `LEADIN=N` overrides it, and the draw gate at the bottom is what says whether it worked.
#
# IT DOES NOT LAND ON THE SAME SPOT TWICE, AND THAT IS IRREDUCIBLE. The operator watched
# the first replay and said: *"Not exactly the same spot but pretty close. But we cannot fix
# this — it is because the random zombie spawn placing zombies on the way. So we'll have to
# settle for this. Still way better than the roadblock debug spawn."* The crowd is spawned
# with randomness, zombies get in Chuck's way, and identical inputs therefore produce a
# slightly different position. No amount of transcription fidelity fixes that; it is the
# title's own non-determinism, not a defect in the recipe.
#
# WHAT THAT MEANS FOR EVERY MEASUREMENT TAKEN HERE, and it is the whole reading rule:
#
#   * **Band by draw count. Never compare matched frame indices.** Two runs land in
#     slightly different places, so frame N of one is not frame N of the other. This is the
#     same conclusion part 26 reached for picture A/Bs by a different road — exact frame
#     matching outdoors selects for stasis and yields 0 of 12,174 frames (gotcha 254) —
#     and `tools/part76_band.py`'s matched 250-draw bands are already the right reader.
#   * **Run the NULL first, on this route, with the runs interleaved.** The spawn variance
#     is a real source of run-to-run spread and nobody has measured it here yet. A change
#     smaller than that spread is not a result (gotcha 229).
#   * **Three runs an arm, alternated.** As every campaign since part 20.
#
# THIS MANUFACTURES PROGRESS (gotcha 78) and is never a gate configuration.
#
# Usage:
#   tools/part80_crowdroute.sh <tag> [ENV=VAL ...]
#   SOAK=120 tools/part80_crowdroute.sh base          # longer stationary soak
#   LEADIN=25000 tools/part80_crowdroute.sh base      # a slower boot than the recording
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part80-crowd}"
TAG="${1:?usage: part80_crowdroute.sh <tag> [ENV=VAL ...]}"; shift || true
mkdir -p "$OUT"
# NEVER `pgrep -f` here: it matches the waiting shell's own command line and three waiters
# deadlocked on that in part 75. Match the process NAME.
# THE NAME IS TRUNCATED TO 15 CHARACTERS AND `pgrep -x` COMPARES AGAINST THE TRUNCATION.
# `cz_runtime_crowd` is 16, so `pgrep -x cz_runtime_crowd` printed a warning and matched
# NOTHING, every time, since the day this script was written — the guard never guarded. It
# is a candidate cause of gotcha 483 (runs that exit ~3 s after start with no error and no
# `[fps]` line): `cp -f` over a binary a previous run still has mapped, plus two processes
# sharing one pipeline-cache file, is exactly that shape. `pgrep -x` on the TRUNCATED name
# is the fix; still never `pgrep -f`, which matches the waiting shell's own command line and
# deadlocked three waiters in part 75.
for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_crow 2>/dev/null); do
    echo "!! a cz_runtime is already running (pid $p); refusing"; exit 2
done

SEQFILE="${SEQFILE:-$ROOT/config/part80_crowd_route.seq}"
[ -r "$SEQFILE" ] || { echo "!! no route file: $SEQFILE"; exit 2; }
BASE_SEQ="$(grep -a '^CZ_FAKE_PRESS_SEQ=' "$SEQFILE" | tail -1 | cut -d= -f2-)"
[ -n "$BASE_SEQ" ] || { echo "!! $SEQFILE has no CZ_FAKE_PRESS_SEQ= line"; exit 2; }

# The recorded lead-in, read out of the route file rather than duplicated here — a constant
# copied into two places is a constant that will disagree with itself.
REC_LEADIN="$(printf '%s' "$BASE_SEQ" | sed -n 's/^NONE@\([0-9]*\),.*/\1/p')"
LEADIN="${LEADIN:-$REC_LEADIN}"
SEQ="$(printf '%s' "$BASE_SEQ" | sed "s/^NONE@${REC_LEADIN},/NONE@${LEADIN},/")"

# SOAK — extra seconds STANDING STILL at the end, in the crowd. This is where a frame time
# is actually measured: the sweeps get Chuck's view into the thick of it and then the load
# is stationary, which is what makes two arms comparable at all (a turning camera changes
# the draw set every frame — gotcha 247).
SOAK="${SOAK:-60}"
SEQ="$SEQ,NONE@$((SOAK * 1000)),NONE"

FPSLOG="${FPSLOG:-5}"
RES="${RES:-3440x1440}"   # PINNED. autoroute takes it from the DESKTOP, and a mid-run mode
                          # change once made one A/B read -33% and 0% at the same time.

# Timeout derived from the recipe's own length rather than fixed: a constant timeout is how
# part 74 found that 60% of every autoroute run was a parked camera (gotcha 438).
RECIPE_MS=$(printf '%s' "$SEQ" | tr ',' '\n' | sed -n 's/.*@\([0-9]*\)$/\1/p' \
            | awk '{s+=$1} END {print s+0}')
TIMEOUT="${TIMEOUT:-$(( 40 + RECIPE_MS / 1000 ))}"

BIN=cz_runtime_crowd
BIN_SRC="${BIN_SRC:-$ROOT/runtime/build/cz_runtime}"
[ -x "$BIN_SRC" ] || { echo "!! no such binary: $BIN_SRC"; exit 2; }
cp -f "$BIN_SRC" "$ROOT/runtime/build/$BIN"
if [ "$BIN_SRC" = "$ROOT/runtime/build/cz_runtime" ]; then
    HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
else
    # Name the BINARY's provenance, not this tree's HEAD, or the log lies about what ran.
    HEAD="$(cd "$(dirname "$BIN_SRC")" && git rev-parse --short HEAD 2>/dev/null || echo external) [$BIN_SRC]"
fi
STAMP="$(date +%m%d_%H%M%S)"
LOG="$OUT/crowd_${STAMP}_${TAG}.log"

echo "=== $TAG  ($HEAD)  lead-in ${LEADIN}ms + route + ${SOAK}s soak  timeout ${TIMEOUT}s"
echo "    -> $LOG"
( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 "CZ_FPS_CAP=500" "CZ_FPS_LOG=$FPSLOG" "CZ_VK_RES=$RES" \
    CZ_DEBUG_MENU=1 "CZ_FAKE_START_MS=100" "CZ_FAKE_PRESS_SEQ=$SEQ" \
    "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
    "$@" timeout "$TIMEOUT" "./$BIN" > "$LOG" 2>&1 )

# THE ROUTE'S OWN GATE, and its threshold is the ITEM's threshold rather than "did it get
# outdoors". A run that lands in the world at 6,000 draws has arrived somewhere and is
# still useless for a CPU item, so the gate sits where the measurement rule sits.
#
# A FAILED RUN IS RENAMED, not merely complained about. Part 78 lost an A/B because a run
# that missed its route printed a loud message and was then globbed straight back into the
# comparison: a message on the terminal is not a drop (`part76-kickoff.md` §5).
peak=$(grep -a "^\[fps\]" "$LOG" | grep -aoE "draws med [0-9]+" | awk '{if($3>m)m=$3}END{print m+0}')
sustained=$(grep -a "^\[fps\]" "$LOG" | grep -aoE "draws med [0-9]+" | awk '$3>=8000' | wc -l)
echo "  peak windowed draws med: $peak    windows at >=8000 draws: $sustained"
if [ "${peak:-0}" -lt 8000 ] || [ "$sustained" -lt 4 ]; then
    echo "  ** DID NOT REACH/HOLD THE CROWD (need peak >=8000 and >=4 windows there)."
    awk '/^\[fps\]/{ t += '"$FPSLOG"'
        if (match($0, /draws med [0-9]+/)) printf "     %4ds  %s\n", t, substr($0, RSTART, RLENGTH) }' "$LOG" | tail -14
    mv -f "$LOG" "${LOG%.log}.rejected"
    echo "  (renamed to ${LOG%.log}.rejected so no glob can pick it up)"
    exit 3
fi
echo "  OK"
