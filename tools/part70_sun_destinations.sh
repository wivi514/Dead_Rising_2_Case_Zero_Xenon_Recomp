#!/usr/bin/env bash
# PART 70 — IS THE SUN'S DIRECTION A DEFECT OR A DAY CYCLE? Ask the game's own
# destinations.
#
# WHY THIS EXISTS
# ---------------
# Part 69 recorded the sun's Z component flipping sign "between headless and windowed
# runs" and made it the live lead, on the strength of a census that read one distinct
# vector per run:
#
#     windowed (operator: bake, nobake, dyn0)   sun=(-0.364 0.546 -0.755)
#     headless (v_final, seq, m18_settle0)      sun=(-0.371 0.557 +0.743)
#
# The label was the confound. Re-reading all 36 archived RT logs, ZERO of them contain a
# `requested DebugJump` line: every one is an operator run that loaded THEIR save, while
# every "headless" run reaches the world through the DebugJump screen and spawns at
# Case 0-2. So the two arms differed in the place and the story time as well as in the
# window, and a game with a day cycle is entitled to two different suns.
#
# This settles it without an operator. The DebugJump screen lists several destinations;
# each is a different point in the story, so each should carry its own sun. If the sun
# tracks the DESTINATION, it moves because the game moved it, our decomposition is right,
# and the part-69 lead is dead. If every destination gives the same vector and only the
# operator's save differs, the lead survives and the next question is what their save has
# that a fresh spawn does not.
#
# The clincher is in the same log either way: `NoteGuestSun` reads the TITLE'S OWN sun
# constant (pixel c23) and cross-checks it against the title's own cascade matrix, and
# the runtime REFUSES a block whose two halves disagree. A nonzero rejection count would
# mean our reading of the title is wrong; zero means the title is telling us this.
#
# Diagnostic only — counters and log lines, no claim about how the frame looks.
set -u
OUT=${1:-$HOME/DR2CZ-troubleshooting/part70-sun}
SECS=${SECS:-240}
mkdir -p "$OUT"
[ -x runtime/build/cz_runtime ] || { echo "build first" >&2; exit 1; }

# The DebugJump screen: F2 opens it, START is the entry WAITJUMP repeats until the
# frontend exists, then N x DOWN selects the Nth destination and A takes it. One DOWN is
# `Case 0-2`, the route CLAUDE.md documents; the others are what this sweep is for.
for n in 0 1 2 3; do
    downs=""
    for ((i = 0; i < n; i++)); do downs="${downs}DOWN,"; done
    seq="F2,START,WAITJUMP,NONE,${downs}A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE"
    echo "=== destination $n  (${n} x DOWN)"
    ( cd runtime/build && env CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
        CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=0 \
        CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$seq" \
        timeout $SECS ./cz_runtime ) > "$OUT/dest$n.log" 2>&1
    # An arm that never reached the world measures the MENU, whose sun is a different
    # light entirely — so it is refused rather than reported (gotcha 408's shape).
    if ! grep -qa "requested DebugJump" "$OUT/dest$n.log"; then
        echo "    *** never reached the world — not reported"
        continue
    fi
    # -A9 on each header, then drop anything that belongs to the OTHER table: the two
    # censuses sit next to each other and a wide window prints the second one twice.
    echo "  the CASCADE decomposition latched:"
    grep -a -A9 "sun directions latched" "$OUT/dest$n.log" \
        | grep -a "volume" | sed 's/^/    /'
    echo "  the TITLE'S OWN constant said:"
    grep -a -A9 "TITLE'S OWN sun" "$OUT/dest$n.log" \
        | grep -av "volume" | grep -a "^\[rtb\]" | cut -c1-160 | sed 's/^/    /'
done
