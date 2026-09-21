#!/usr/bin/env bash
# A same-box co-op pair that answers ONE question: which player index does the
# title's mission system run its trigger actions AS, on each side of the link?
#
# WHY THIS EXISTS (player issue #9, 2026-09-21). *"After the Client used a key item
# on the bike most key items were either missing or replaced with other key items."*
# The bike decides which part was placed from the item in the hands of the player at
# `missionUpdateContext + 0x10` (runtime/kernel/coop_items.cpp has the whole decode),
# so a part placed as the WRONG part means that index named the wrong Chuck. A solo
# run prints `mission update context player index is now 0` and never changes it,
# which is correct and says nothing — the question only has an answer with two
# players in one level.
#
# This is the cheapest form of that question: it needs no bike and no bike parts.
# Both instances jump into a level with mission triggers and print the index; if the
# host says 0 for ever while its partner is the one standing in the trigger, the
# index is not "who acted" and the bike path cannot be right.
#
# Two headless instances, two XenonLive identities (the default one hosts, the
# `XenonLive-host` profile joins on peer port 3075 — the part-3 recipe), the
# operator's own server, exactly as tools/coop_pair_reload.sh does it.
#
#   CASE=3 TIMEOUT=420 tools/coop_pair_items.sh
#
# CASE is how many DOWNs the host presses on the DebugJump screen: 1 = Case 0-2
# (outdoors, Still Creek), 3 = Case 0-4 (the safehouse garage, where the bike is).
#
# REFUSES TO RUN while another cz_runtime is alive: that is the operator's game.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/issue9}"
TIMEOUT="${TIMEOUT:-420}"
CASE="${CASE:-1}"
TRACE="${TRACE:-1}"
mkdir -p "$OUT"

if pgrep -x cz_runtime >/dev/null; then
    echo "coop_pair_items: a cz_runtime is already running (pid $(pgrep -x cz_runtime | head -1)) — not starting a pair beside it" >&2
    exit 2
fi

STAMP="$(date +%m%d_%H%M%S)"
HOSTLOG="$OUT/items_${STAMP}_host.log"
JOINLOG="$OUT/items_${STAMP}_joiner.log"

# The host: F2 once the menu is up, WAITJUMP parks until the DebugJump screen exists
# (it lands whenever it lands — anchoring on the EVENT is what makes this reproducible,
# gotcha 251), CASE DOWNs pick the case, A jumps. Then AutoChuck drives so the pair is
# not two statues: a walking Chuck enters trigger volumes, which is the whole point.
HOSTSEQ="NONE,START,NONE,F2,WAITJUMP"
for _ in $(seq 1 "$CASE"); do HOSTSEQ="$HOSTSEQ,DOWN"; done
HOSTSEQ="$HOSTSEQ,A"
for _ in $(seq 1 30); do HOSTSEQ="$HOSTSEQ,NONE"; done

echo "host   -> $HOSTLOG   (DebugJump, ${CASE} DOWN)"
echo "joiner -> $JOINLOG"

( cd "$ROOT/runtime/build" && env \
    CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_ITEM_TRACE="$TRACE" \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_COOP_JOIN_PROMPT=0 XLIVE_ALLOW_INSECURE=1 \
    CZ_AUTOCHUCK=EXPLORER CZ_ONLINE_LOG=1 \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$HOSTSEQ" \
    timeout "$TIMEOUT" ./cz_runtime > "$HOSTLOG" 2>&1 ) &
HOSTPID=$!

sleep 20   # the host's head start: its session must exist before the joiner searches

( cd "$ROOT/runtime/build" && env \
    CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_ITEM_TRACE="$TRACE" \
    XLIVE_DATA_DIR="$HOME/.config/XenonLive-host" XLIVE_ALLOW_INSECURE=1 CZ_XLIVE_PEER_PORT=3075 \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_XLIVE_JOIN=1 CZ_ONLINE_LOG=1 \
    CZ_AUTOCHUCK=EXPLORER \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="START,NONE,NONE,A,NONE,A,NONE,NONE,A,NONE,A,NONE" \
    timeout "$((TIMEOUT - 20))" ./cz_runtime > "$JOINLOG" 2>&1 ) &
JOINPID=$!

wait "$HOSTPID" || true
wait "$JOINPID" || true

for side in host joiner; do
    log="$HOSTLOG"; [ "$side" = joiner ] && log="$JOINLOG"
    echo "=== $side: link"
    grep -a "hosting session\|Accepted client\|LOGIN_STATE_CONNECTED\|Lost connection\|\[crash\]" "$log" | cut -c1-120 | head -8
    echo "=== $side: both Chucks present?"
    grep -a "\[pos\] player . appeared" "$log" | head -4
    echo "=== $side: the answer"
    grep -a "\[item\]" "$log" | head -40
done
