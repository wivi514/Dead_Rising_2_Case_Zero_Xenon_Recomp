#!/usr/bin/env bash
# The HOST leg of co-op part 13 — the guest who arrives dressed and renders invisible
# or without a torso (docs/coop-plan.md section 13).
#
# WHAT THE ROUND IS. There is nothing to do. Host, let the guest in, and LOOK AT HIM.
# The check runs by itself every 2 s once both players are dressed, and the repair
# fires by itself for a piece that has been missing for three sweeps. The round is
# over when you have seen the guest — correct or not — and the log has said which.
#
# WHY THERE IS NO ARM TO SET. Both halves ship ON. This launcher exists only to keep
# CZ_OUTFIT_TRACE on (per-PART, not per-frame: it prints the file the requester asked
# for and whether the model and texture were created, which is the depth the check
# points at when a repair fails) and to leave issue #9's per-pickup traces OFF.
#
# THE PREDICTED LINES, stated before the run so the run can refute them:
#
#   [outfit] co-op clothing check is running (local player 0, 2 dressed players, ...)
#   then ONE of:
#   [outfit] player 1 (the other machine) has all N of the pieces he is wearing
#   [outfit] PLAYER 1 (the other machine) IS MISSING CLOTHING — ... renders with holes
#   [outfit]     chest 'X' has no model — its streaming budget in this session is N KB
#   [outfit]     asking for player 1's chest ('X') again — attempt 1 of 3
#
# ANY OF THESE REFUTES THE CHECK ITSELF, and none of them is a defect in the game:
# the line about PLAYER 0 (this machine) missing clothing — player 0 is the host's own
# Chuck and he is visibly correct, so that means the reading is wrong; or `is dressed
# but is NOT registered with the clothing manager`, which means the load records
# cannot be found for him at all.
#
# THE CONTROL, and it is worth the second join if there is time:
#   CZ_COOP_OUTFIT_REPAIR=0 tools/part13_outfit_host.sh
# Same evening, same two machines: the check still reports, nothing is repaired. That
# is what separates "the repair fixed him" from "he was going to arrive anyway".
#
# The joiner's launcher is C:\cz\play.bat on czwin and needs no change — both halves
# are on by default there too.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cat <<BANNER
=== co-op part 13 HOST leg — the guest's clothing =======================
  commit      : $(cd "$ROOT" && git rev-parse --short HEAD)
  check       : ON      (CZ_COOP_OUTFIT_CHECK=0 turns it off)
  repair      : ${CZ_COOP_OUTFIT_REPAIR:-ON}   (CZ_COOP_OUTFIT_REPAIR=0 is the control)
  what to do  : host, let the guest in, and LOOK AT HIM. Nothing to press.
  afterwards  : grep -a '\[outfit\]' on both logs
=========================================================================
BANNER

exec "$ROOT/tools/play_session.sh" \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 XLIVE_ALLOW_INSECURE=1 CZ_ONLINE_LOG=3 \
    CZ_OUTFIT_TRACE=1 \
    "$@"
