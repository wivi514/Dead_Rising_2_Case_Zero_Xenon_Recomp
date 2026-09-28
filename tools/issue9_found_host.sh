#!/usr/bin/env bash
# The HOST leg of player issue #9's SECOND deciding measurement — the FOUND flag.
#
# WHY THIS EXISTS, AND WHY IT IS NOT `issue9_host.sh`. That script asks part 10's
# question (whose Chuck does the PLACEMENT response run for) and that question is
# ANSWERED and FIXED — `CZ_COOP_RESPONSE_PLAYER` is on by default and the operator
# verified it by eye. This is the other half: a part the GUEST picks up is never
# marked FOUND, so placing it does nothing. `issue9_host.sh` is left exactly as it
# was, because its text is the record of what part 10 asked for.
#
# WHAT THE ROUND IS. Two minutes, and it produces BOTH arms in ONE log, one process,
# one binary — the shape that has now won twice in this issue:
#
#     the HOST picks up one bike part.
#     the GUEST picks up a DIFFERENT bike part.
#     NOBODY PLACES ANYTHING.
#
# Placing is not part of the test and makes the log harder to read. One pickup each
# is the whole thing.
#
# WHAT IT IS MEASURING (docs/coop-plan.md section 12 is the derivation). A bike part
# is marked FOUND when `cMissionObjectiveGiveItemToNPC` — the PREREQUISITE of the
# mission `Prologue<Part>Objective` — sees the item in an inventory. Measured, that
# test looks in exactly ONE inventory: `GetUserPlayer(world->0x7C, world->0x80)`, the
# LOCAL player, which on the host is player 0. So a part in the guest's hands should
# be invisible to it.
#
# THE PREDICTED LINES, stated before the run so the run can refute them:
#
#   [obj] ... ITEM_NAME "<the part YOU picked up>":     answer 0 -> 1, index 0
#   [obj] ... ITEM_NAME "<the part the GUEST picked>":  answer 0, index 0, and it
#                                                      NEVER becomes 1
#   [mw] CHANGED ... Prologue<YourPart>Objective : state(+08) 0 -> N
#        ... and NO such line for the guest's part's objective
#
# ANY OF THESE REFUTES IT: the answer never reaching 1 for your own part; the index
# printed being anything other than 0; or the guest's objective advancing anyway.
#
# THE CANDIDATE FIX IS OFF. Run once without it — the un-armed log is what says the
# mechanism is real. Then `FIX=1` for a second round, where the whole test is whether
# `[found] RESCUED` appears for the guest's part and the screen turns green.
#
#   tools/issue9_found_host.sh          # the measurement
#   FIX=1 tools/issue9_found_host.sh    # then, only after the first round agreed
#
# BOTH MACHINES MUST BE ON THE SAME COMMIT, and the joiner needs the same two arms —
# the joiner's launcher is C:\cz\play.bat on czwin and its patch is in
# docs/coop-part12-kickoff.md section 3. Without them the joiner's half of the log is
# empty, which has already happened once in this investigation.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIX="${FIX:-0}"
MW_MS="${MW_MS:-500}"

cat <<BANNER
=== issue #9 HOST leg — the FOUND flag ==================================
  commit      : $(cd "$ROOT" && git rev-parse --short HEAD)
  found fix   : CZ_COOP_FOUND_ANY_PLAYER=$FIX  $( [ "$FIX" = 0 ] && echo "(control — the measurement run)" || echo "(ARMED: this is no longer a clean measurement)" )
  what to do  : host, let the joiner in, both go to the Case 0-4 bike area.
                YOU pick up ONE bike part. The GUEST picks up a DIFFERENT one.
                DO NOT PLACE ANYTHING. Then quit.
  afterwards  : grep -aE '\[obj\]|\[mw\] (CHANGED|LISTS)|\[found\]' on both logs
=========================================================================
BANNER

exec "$ROOT/tools/play_session.sh" \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 XLIVE_ALLOW_INSECURE=1 CZ_ONLINE_LOG=3 \
    CZ_ITEM_TRACE=1 CZ_COOP_PICKUP_TRACE=1 \
    CZ_COOP_OBJTRACE=1 "CZ_COOP_MISSIONWATCH=$MW_MS" \
    "CZ_COOP_FOUND_ANY_PLAYER=$FIX" \
    "$@"
