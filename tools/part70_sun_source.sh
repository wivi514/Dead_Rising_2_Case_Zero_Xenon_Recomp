#!/usr/bin/env bash
# PART 70 — THE SUN'S DIRECTION, AND WHERE IT COMES FROM.
#
# WHY THIS EXISTS
# ---------------
# Part 69 closed with the sun as the live lead: our RT shadow route derives the sun by
# decomposing a cascade matrix it captures at draw time, and that derived vector read
# `(-0.364 0.546 -0.755)` in the operator's windowed sessions against `(-0.371 0.557
# +0.743)` in a headless one. `tools/xtr_sun_oracle.py` then asked HARDWARE, which
# answers with two independent statements in the same draw's constant file — the title's
# own `pc(23)` and its own cascade matrix — that agree with each other to 0.00 degrees in
# all twenty `.xtr` captures and both say +Z.
#
# So the runtime now reads the title's constant (`CZ_VK_RT_SUN_SRC=guest`, the default)
# and the decomposition is retained as the control arm (`=cascade`). This script runs the
# pair and prints what each one LATCHED, with the frame ranges — because "two directions
# over a run" has two explanations, a light that moved and a selection that flipped, and
# only an ORDER separates them.
#
# Diagnostic only: every reading below is a counter or a log line. No claim about how the
# frame LOOKS is made here — that goes through the operator, standing rule.
set -u
OUT=${1:-$HOME/DR2CZ-troubleshooting/part70-sun}
SECS=${SECS:-300}
mkdir -p "$OUT"
BIN=runtime/build/cz_runtime
[ -x "$BIN" ] || { echo "no $BIN — build first" >&2; exit 1; }

ROUTE="F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE"

run () {                       # run <tag> <proof-line> [VAR=VAL ...]
    local tag=$1 proof=$2; shift 2
    echo "=== $tag  ($*)"
    ( cd runtime/build && env "$@" \
        CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_VK_RT_SHADOWS=1 \
        CZ_VK_RT_DYN_SETTLE=0 CZ_VK_RT_FACTOR_READBACK=64 \
        CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ="$ROUTE" \
        timeout $SECS ./cz_runtime ) > "$OUT/$tag.log" 2>&1
    # THE ARM MUST PROVE IT ENGAGED. A control whose variable never reached `env` ran
    # with the feature on for a whole operator session in part 69 (gotcha 408), so an
    # arm that cannot show its own line is REFUSED rather than reported.
    if ! grep -qa -- "$proof" "$OUT/$tag.log"; then
        echo "  *** ARM DID NOT ENGAGE — no line matching '$proof'. Its numbers are"
        echo "      NOT reported, because an arm that did not engage measures the"
        echo "      other arm (gotcha 408)."
        return
    fi
    grep -a "requested DebugJump\|WAITJUMP released" "$OUT/$tag.log" | sed 's/^/    /'
    grep -ao "tlasInst=[0-9]* .*singular=[0-9]*" "$OUT/$tag.log" | tail -1 \
        | cut -c1-150 | sed 's/^/    /'
    grep -a "FACTOR IMAGE" "$OUT/$tag.log" | tail -1 | cut -c1-150 | sed 's/^/    /'
}

run cascade "CZ_VK_RT_SUN_SRC=cascade" CZ_VK_RT_SUN_SRC=cascade
run guest   "src=guest-c23"

echo
echo "################ WHAT EACH RUN LATCHED, WITH ITS ORDER ################"
for t in cascade guest; do
    [ -e "$OUT/$t.log" ] || continue
    echo "--- $t"
    grep -a -A10 "sun directions latched" "$OUT/$t.log" | grep -a "^\[rtb\]" \
        | sed 's/^/  /'
    grep -a -A10 "TITLE'S OWN sun" "$OUT/$t.log" | grep -a "^\[rtb\]" | sed 's/^/  /'
done
echo
echo "Read it like this:"
echo "  * clusters with DISJOINT frame ranges  = the sun MOVED (a day cycle, no defect)"
echo "  * clusters with OVERLAPPING ranges     = the selection FLIPPED (our defect)"
echo "  * 'blocks REJECTED for c23-vs-cascade disagreement' non-zero = the title's own"
echo "    two statements disagree in OUR runtime, which they never do on hardware"
