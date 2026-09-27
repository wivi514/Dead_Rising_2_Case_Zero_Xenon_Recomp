#!/usr/bin/env bash
# Player issue #7, the real-hardware reproduction: THIS box hosts co-op with a SLOW
# level load (the reporter's Windows host took 14.5 s; this box normally ~1 s), and the
# second player rejoins from czamd on its normal build. The hypothesis: a slow host
# load leaves the joining client placed below the world at the first checkpoint. The
# host's fall watch ([pos]/[fall], compiled into this build) shows player 1 (the joiner)
# as the host sees him; czamd's own cz_runtime.log carries the client-side crash.
#
# Windowed and audible on purpose — the OPERATOR drives the host (start a fresh co-op
# game to Still Creek) and rejoins from czamd. Detached so it outlives this shell.
#
#   tools/issue7_slowhost.sh          # SLOW=400 -> ~16 s load
#   SLOW=250 tools/issue7_slowhost.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/issue7}"
SLOW="${SLOW:-400}"
mkdir -p "$OUT"

if pgrep -x cz_runtime >/dev/null; then
    echo "issue7_slowhost: a cz_runtime is still running (pid $(pgrep -x cz_runtime | head -1)) — close both games first" >&2
    exit 2
fi

STAMP="$(date +%m%d_%H%M%S)"
LOG="$OUT/slowhost_${STAMP}.log"
echo "host log -> $LOG   (zone opens sleep ${SLOW} ms each; ~$(( SLOW * 40 / 1000 )) s load)"
echo "  drive the host to a fresh co-op Still Creek start, then rejoin from czamd"

setsid nohup env -C "$ROOT/runtime/build" \
    DISPLAY="${DISPLAY:-:0}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}" \
    CZ_VKDRAW=1 \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_XLIVE_HOST=1 XLIVE_ALLOW_INSECURE=1 \
    CZ_SLOW_ZONE_OPEN_MS="$SLOW" CZ_ONLINE_LOG=3 CZ_NET_LOG=1 \
    ./cz_runtime > "$LOG" 2>&1 < /dev/null &
disown || true
sleep 3
if pgrep -x cz_runtime >/dev/null; then
    echo "launched (pid $(pgrep -x cz_runtime | head -1)); window should be up shortly"
else
    echo "FAILED to launch — tail of the log:"; tail -20 "$LOG"
fi
echo "$LOG"
