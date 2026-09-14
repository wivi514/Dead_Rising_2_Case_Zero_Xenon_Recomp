#!/usr/bin/env bash
# A same-box co-op pair in which the HOST RELOADS THE LEVEL with the joiner attached —
# and the host's load is made SLOW on purpose.
#
# WHY THIS EXISTS (player issue #7, 2026-09-14). A Windows host reported that "loading
# into Still Creek in co-op makes the client fall through the map and crash". The
# host's F9 report says: a live session, a level load that took 14.5 s on the host, and
# after it the partner's marker pointing DOWN through the host's feet; the client's
# link died a few seconds later. The operator could not reproduce it on the two test
# machines — where the host (this box) loads a level in about a second and the joiner
# (the laptop) is the SLOW side. This script builds the shape nobody had run: a joiner
# that finishes loading long before the host does.
#
# Two headless instances, two XenonLive identities (the default one hosts, the
# `XenonLive-host` profile — "Chuck Greene" — joins on peer port 3075, the part-3
# recipe), the operator's own server. The host takes the DebugJump route to Case 0-2,
# the joiner searches and joins (CZ_XLIVE_JOIN=1 opens GameSelect itself; the two A's
# are its dialog and its save slot), and after RELOAD_AT seconds the host presses F2
# again and jumps to a case — a level reload with a client in the session. Every zone
# archive open on the host sleeps CZ_SLOW_ZONE_OPEN_MS (forty opens a load), so a
# 300 ms setting is a ~12 s load, the reporter's shape.
#
# What to read afterwards: `[pos]`/`[fall]` lines in BOTH logs (the fall watch is on
# every build; slot 1 is the joiner's own Chuck on both machines), the sync points
# (`SYNCPOINT_TYPE_*`, from CZ_ONLINE_LOG=3), and any `[crash]` block.
#
#   SLOW=300 RELOAD_AT=260 tools/coop_pair_reload.sh
#
# REFUSES TO RUN while another cz_runtime is alive: that is the operator's game, and a
# headless pair beside it contaminates both (and the joiner would find THEIR session).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/issue7}"
SLOW="${SLOW:-300}"
RELOAD_AT="${RELOAD_AT:-260}"     # seconds after launch: the host's second F2
TIMEOUT="${TIMEOUT:-540}"
mkdir -p "$OUT"

if pgrep -x cz_runtime >/dev/null; then
    echo "coop_pair_reload: a cz_runtime is already running (pid $(pgrep -x cz_runtime | head -1)) — not starting a pair beside it" >&2
    exit 2
fi

STAMP="$(date +%m%d_%H%M%S)"
HOSTLOG="$OUT/pair_${STAMP}_host.log"
JOINLOG="$OUT/pair_${STAMP}_joiner.log"

# The host's presses: 8 s each. F2 after the menu is up (docs/coop-plan.md, part 3's
# item 4 — F2 at 8 s lands in the title->menu transition and is lost), WAITJUMP parks
# until the DebugJump screen exists, DOWN picks Case 0-2, A jumps. Then hold with NONE
# until RELOAD_AT, then F2 again (from gameplay; WAITJUMP is one-shot, so a plain NONE
# gives the screen its interval), DOWN, DOWN, A: the reload to Case 0-3.
HOLD=$(( (RELOAD_AT - 7 * 8) / 8 ))
[ "$HOLD" -lt 1 ] && HOLD=1
HOSTSEQ="NONE,START,NONE,F2,WAITJUMP,DOWN,A"
for _ in $(seq 1 "$HOLD"); do HOSTSEQ="$HOSTSEQ,NONE"; done
HOSTSEQ="$HOSTSEQ,F2,NONE,DOWN,DOWN,A,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE"

echo "host   -> $HOSTLOG   (zone opens sleep ${SLOW} ms; reload F2 at ~${RELOAD_AT} s)"
echo "joiner -> $JOINLOG"

( cd "$ROOT/runtime/build" && env \
    CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_COOP_JOIN_PROMPT=0 XLIVE_ALLOW_INSECURE=1 \
    CZ_SLOW_ZONE_OPEN_MS="$SLOW" CZ_ONLINE_LOG=3 \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$HOSTSEQ" \
    timeout "$TIMEOUT" ./cz_runtime > "$HOSTLOG" 2>&1 ) &
HOSTPID=$!

# Give the host its head start so its session exists by the time the joiner's search
# runs (the search retries every 12 s regardless).
sleep 20

( cd "$ROOT/runtime/build" && env \
    CZ_NO_WINDOW=1 CZ_VKDRAW=1 \
    XLIVE_DATA_DIR="$HOME/.config/XenonLive-host" XLIVE_ALLOW_INSECURE=1 CZ_XLIVE_PEER_PORT=3075 \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_XLIVE_JOIN=1 CZ_ONLINE_LOG=3 \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="START,NONE,NONE,A,NONE,A,NONE,NONE,A,NONE,A,NONE" \
    timeout "$((TIMEOUT - 20))" ./cz_runtime > "$JOINLOG" 2>&1 ) &
JOINPID=$!

wait "$HOSTPID" || true
wait "$JOINPID" || true

echo "=== host: session / join / loads"
grep -a -n "hosting session\|Accepted client\|Pending client IS\|requested DebugJump\|WAITJUMP\|audio.Prologue.txt\|closesocket\|Open -> Error\|\[crash\]" "$HOSTLOG" | cut -c1-140 | head -40
echo "=== host: fall watch"
grep -a "\[fall\]\|\[pos\] player 1 appeared" "$HOSTLOG" | head -30
echo "=== joiner: session / loads"
grep -a -n "LOGIN_STATE\|audio.Prologue.txt\|FINALIZE_START_LEVEL\|READY_FOR_PLAY\|SHUTDOWN_LEVEL\|GameplayFlow::Enter\|\[crash\]\|Lost connection" "$JOINLOG" | grep -v "recved event" | cut -c1-140 | head -60
echo "=== joiner: fall watch"
grep -a "\[fall\]\|\[pos\] player . appeared" "$JOINLOG" | head -40
