#!/usr/bin/env bash
# The HOST leg of player issue #11's two-machine round (docs/coop-plan.md
# "Player issue #11").
#
# WHY THIS EXISTS. CZ_COOP_ITEM_SYNC is the repair for the DECISION half of #9: the
# host scored the guest's gas can as BIKE FORKS out of its own drifted copy of the
# guest's bag. Its receive half and the found-flag rescue cannot run on one machine
# (the same-box pair does not join in either arm), so the only test is two machines,
# and both must carry the arm or the far side never publishes.
#
# THE ROUND:
#     the GUEST picks up the gas can and at least one other part.
#     the HOST picks up nothing (or a different part).
#     the GUEST swaps to the gas can and places it IMMEDIATELY.
#
# THE PREDICTED LINES on THIS (host) log, stated before the run:
#   [itemsync] publisher running: ...                      (the driver is alive)
#   [itemsync] <- player 1 holds 5F8D0521 (GasolineCanister)
#   [found] RESCUED BY THE OWNER: "GasolineCanister" ...   (or the copy-based RESCUED)
#   [itemsync] placement by player 1 (remote here): ... -> raising D4AF6D06 (GasCanPlaced)
#              ... or "both machines agree"
# and the HUD ticks the GAS CAN, not the forks. A guest who never carried the forks
# must never tick the forks.
# REFUTED BY: "NO FRESH held-item" (the channel is not arriving), no RESCUED line and
# the gas can not ticking (no listener), or the forks ticking anyway.
#
# The joiner's launcher is C:\cz\play.bat on czwin and needs CZ_COOP_ITEM_SYNC=1 too.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cat <<BANNER
=== issue #11 HOST leg — item sync ======================================
  commit      : $(cd "$ROOT" && git rev-parse --short HEAD)
  arm         : CZ_COOP_ITEM_SYNC=1 (both machines)
  what to do  : host, let the joiner in, both go to the bike.
                GUEST picks up the gas can + another part, swaps to the
                gas can and places it at once. Host picks up nothing.
  afterwards  : grep -aE '\[itemsync\]|\[found\]|\[respfix\]' on both logs
=========================================================================
BANNER

exec "$ROOT/tools/play_session.sh" \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 XLIVE_ALLOW_INSECURE=1 CZ_ONLINE_LOG=3 \
    CZ_COOP_ITEM_SYNC=1 CZ_ITEM_TRACE=1 CZ_COOP_PICKUP_TRACE=1 CZ_COOP_CONDITION_ANY_PLAYER=1 CZ_COOP_SPAWN_CARRIED=3 CZ_COOP_TRACKER_ALL=1 \
    "$@"
