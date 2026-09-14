#!/usr/bin/env bash
# A same-box co-op pair that drives the JOINER through the MENUS to the friends
# screen and presses X on the focused friend — the JOIN FRIENDS row end to end,
# with no operator and no second machine.
#
# WHY THIS EXISTS (2026-09-14). The operator reported that "join a friend" from
# the main menu "acts like it randomly searches a game", and asked whether Case
# West had logs of the title's own friend-join to implement it properly. No
# capture anywhere holds one (Xenia has no Live layer; Case West's runtime has no
# friend-join of its own), so the title's own path was run HERE instead, under
# CZ_COOP_FRIENDS_NATIVE=1, and this is the harness that ran it fifteen times in
# an afternoon. docs/coop-plan.md "Part 7" is the record.
#
# Two headless instances, two XenonLive identities on the operator's own server:
# the default profile (wivi514) HOSTS by the DebugJump route to Case 0-2; the
# `XenonLive-host` profile (Chuck Greene) JOINS on peer port 3075. The two
# accounts were made friends on 2026-09-14 for this test (the friends screen
# lists friends only; a joinable friend's row is what X acts on).
#
# The joiner's presses are 8 s apart, and the title screen takes ~30 s to accept
# START, so the sequence is: START (lost), NONE, NONE, A (= press start), NONE,
# DOWN, A (JOIN CO-OP GAME -> JoinGame), NONE, DOWN, A (JOIN FRIENDS -> the
# friends screen), NONE, NONE, X (join the focused friend), then whatever the
# experiment wants (JOINSEQ overrides the whole thing).
#
#   tools/coop_pair_friends.sh                        # the proven filtered-search join
#   NATIVE=1 tools/coop_pair_friends.sh               # the title's own path + the arms
#   JOINSEQ=... HEAD=60 TIMEOUT=480 tools/coop_pair_friends.sh
#
# What to read afterwards: the joiner's `[coop]` lines (the friends screen, the
# search-by-id, the invite machine's states, the GameInvites dispatches), its
# `LIVE_STATE_*` transitions and `LOGIN_STATE_CONNECTED`; the host's `Pending
# client IS confirmed!`. REFUSES TO RUN beside another cz_runtime: that is the
# operator's game, and the joiner would find THEIR session.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/coop-native}"
HEAD="${HEAD:-60}"; TIMEOUT="${TIMEOUT:-420}"
JOINSEQ="${JOINSEQ:-START,NONE,NONE,A,NONE,DOWN,A,NONE,DOWN,A,NONE,NONE,X,NONE,A,NONE,A,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE}"
NATIVE_ARMS=""
[ "${NATIVE:-0}" = 1 ] && NATIVE_ARMS="CZ_COOP_FRIENDS_NATIVE=1 CZ_COOP_FRIENDS_INVITEINFO=1 CZ_SCREEN_TRACE=1 CZ_GUEST_LOG=1 CZ_GUEST_DIAG=1"
mkdir -p "$OUT"
if pgrep -x cz_runtime >/dev/null; then
    echo "coop_pair_friends: a cz_runtime is already running — not starting a pair beside it" >&2
    exit 2
fi
STAMP="$(date +%m%d_%H%M%S)"
HOSTLOG="$OUT/pair_${STAMP}_host.log"; JOINLOG="$OUT/pair_${STAMP}_joiner.log"
HOSTSEQ="NONE,START,NONE,F2,WAITJUMP,DOWN,A"
for _ in $(seq 1 40); do HOSTSEQ="$HOSTSEQ,NONE"; done
echo "host   -> $HOSTLOG"
echo "joiner -> $JOINLOG (seq $JOINSEQ)"
( cd "$ROOT/runtime/build" && env CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_COOP_JOIN_PROMPT=0 XLIVE_ALLOW_INSECURE=1 \
    CZ_ONLINE_LOG=3 CZ_NET_LOG=1 CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$HOSTSEQ" \
    timeout "$TIMEOUT" ./cz_runtime > "$HOSTLOG" 2>&1 ) &
HP=$!
sleep "$HEAD"
( cd "$ROOT/runtime/build" && env CZ_NO_WINDOW=1 CZ_VKDRAW=1 \
    XLIVE_DATA_DIR="$HOME/.config/XenonLive-host" XLIVE_ALLOW_INSECURE=1 CZ_XLIVE_PEER_PORT=3075 \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 $NATIVE_ARMS CZ_ONLINE_LOG=3 CZ_NET_LOG=1 \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$JOINSEQ" \
    timeout "$((TIMEOUT - HEAD))" ./cz_runtime > "$JOINLOG" 2>&1 ) &
JP=$!
wait "$HP"; wait "$JP"
echo "=== host"
grep -a -n "hosting session\|Accepted client\|Pending client IS\|requested DebugJump\|\[crash\]" "$HOSTLOG" | cut -c1-160 | head
echo "=== joiner"
grep -a -n "\[coop\]\|\[xlive\] XSession\|search found\|session search:\|state transition to LIVE\|LOGIN_STATE\|Join Session\|guest fault" "$JOINLOG" \
    | grep -v "recved event" | cut -c1-200 | head -80
