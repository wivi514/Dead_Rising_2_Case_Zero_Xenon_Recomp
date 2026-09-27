#!/usr/bin/env bash
# The HOST leg of player issue #9's deciding measurement — one command, played by a human.
#
# WHY THIS EXISTS. Issue #9 (the guest places a bike part, it lands on the floor and the
# HOST drops what he was holding) has had four candidate mechanisms refuted, each because
# it was inferred from a call graph that dead-ends. Part 10 read the response chain end to
# end and left exactly TWO surviving leads, and the whole of what separates them is two
# log lines on the HOST during ONE placement by the guest. Everything needed to produce
# those lines is an environment variable, and the previous three sessions were all set up
# by hand — which is how the joiner ran a build without the arms once already.
#
# The joiner's matching launcher is C:\cz\play.bat on czwin (patched 2026-09-27;
# play.bat.pre-part10 is the original). BOTH MACHINES MUST BE ON THE SAME COMMIT — the
# joiner's copy of the trace has to exist or its half of the log is empty.
#
# THE TWO LINES, and what each value means (docs/coop-part10-kickoff.md section 1 is the
# full version):
#
#   [place] STATE 34 ... ctx+0x10 = N     whose Chuck the response runs for.
#       N == 1 -> the response is correct; the prop lookup is the remaining suspect.
#       N == 0 -> it is running for the LOCAL Chuck, which IS the reported symptom, and
#                 the defect is that one field of one class-0x6B batch record.
#
#   [place] PROPFIND ... N live pool entries match ... for PROPCMD 17
#       N == 1 -> the lookup was exact; that lead is refuted on one line.
#       N >= 2 -> the lookup is a coin toss and the list says which prop the title took
#                 and whose hand each candidate was in.
#
# THE CANDIDATE FIX IS DELIBERATELY OFF. Run once without it: the un-armed log is what
# says which mechanism is real. FIX=1 arms it for a second placement, FIX=2 observes only.
#
#   tools/issue9_host.sh              # the measurement
#   FIX=1 tools/issue9_host.sh        # then, if the PROPFIND line showed 2+ candidates
#
# Everything else is play_session.sh's business (F8/F9, the log path, the shader dump),
# and this only adds environment to it — so a session recorded here is comparable with
# every other operator session.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIX="${FIX:-0}"
CENSUS_MS="${CENSUS_MS:-3000}"

cat <<BANNER
=== issue #9 HOST leg ===================================================
  commit      : $(cd "$ROOT" && git rev-parse --short HEAD)
  place fix   : CZ_COOP_PLACE_FIX=$FIX  $( [ "$FIX" = 0 ] && echo "(control — the measurement run)" || echo "(ARMED: this is no longer a clean measurement)" )
  what to do  : host, let the joiner in, then have the GUEST carry one bike
                part to the Case 0-4 bike and press the interact button.
  ONE placement is the whole test. Five is not more informative.
  afterwards  : grep -aE '\[place\] (STATE|PROPFIND|REMOVE)' on both logs
=========================================================================
BANNER

exec "$ROOT/tools/play_session.sh" \
    CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 XLIVE_ALLOW_INSECURE=1 CZ_ONLINE_LOG=3 \
    CZ_ITEM_TRACE=1 CZ_COOP_PICKUP_TRACE=1 \
    CZ_COOP_PLACE_TRACE=1 "CZ_COOP_POOL_CENSUS_MS=$CENSUS_MS" "CZ_COOP_PLACE_FIX=$FIX" \
    "$@"
