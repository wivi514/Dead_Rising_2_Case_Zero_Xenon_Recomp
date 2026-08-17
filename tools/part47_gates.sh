#!/bin/bash
# Part 47: the standing gates, in one command, because a PERFORMANCE part runs them more
# often than any other kind.
#
# WHY THIS EXISTS. `docs/perf-plan-part47.md` §6 requires the picture gates and the PM4
# oracles after EVERY item, and several of its items touch code the picture depends on:
# the texture cache's revalidation cadence, the command processor's register-write path,
# and the command buffer's vertex/index bindings. Those are exactly the changes whose
# failure mode is a plausible wrong picture rather than an error, and the cost of asking
# is a couple of minutes. Running them from memory, one at a time, is how one gets
# skipped.
#
# Exit 1 if any gate fails. Nothing here is optional and nothing here is slow except the
# title-screen run, which is 120 s.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
FAIL=0
note() { printf '\n=== %s\n' "$*"; }
bad()  { printf '!!! FAIL: %s\n' "$*"; FAIL=1; }

note "link gate (--smoke)"
./runtime/build/cz_runtime --smoke >/dev/null 2>&1 || bad "--smoke"

note "recompilation gate: every switch-shaped bctr lowered"
python3 tools/find_unlowered_switches.py >/dev/null || bad "find_unlowered_switches.py"

note "shader cache: per-slot dimensions agree between our ucode parse and DXC"
python3 tools/shader_dim_census.py >/dev/null || bad "shader_dim_census.py"

# The two PM4 boundary oracles. Exit 1 means our parser would DESYNC on a real hardware
# stream -- the failure the bulk register-write path of §2.1 could plausibly cause and
# that nothing in the picture would show.
B1="Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
if [ -f "$B1" ]; then
    note "PM4 oracle 1/2: packet lengths (24.5 M packets)"
    python3 tools/pm4_packet_lengths.py "$B1" >/dev/null || bad "pm4_packet_lengths.py"
    note "PM4 oracle 2/2: indirect-buffer walks (28,726 buffers)"
    python3 tools/pm4_indirect_walks.py "$B1" >/dev/null || bad "pm4_indirect_walks.py"
else
    echo "    (skipped: $B1 not present)"
fi

# The PICTURE, against hardware's own screenshot of the same screen. The title backdrop
# is the cheap static repro established in part 46 (§6ca): 120 s, no input, and E3 is a
# photograph of it. The threshold is +0.70 -- part 45's liveness fix moved this gate
# +0.687 -> +0.710, so it is demonstrably able to fail.
note "picture gate: title backdrop vs capture E3 (120 s run)"
CAP=$(mktemp -d "${TMPDIR:-/tmp}/cz-part47-gate.XXXXXX")
# `env`, not assignment prefixes. A QUOTED word like "CZ_CAPTURE_KEY=$CAP" is not
# recognised as an assignment prefix -- quoting removes that -- so bash parses it as the
# COMMAND and everything after it as arguments. The part-46 A/B script carries the same
# note because six of its runs died that way in under a second, and this script repeated
# the mistake on its first outing: "CZ_CAPTURE_KEY=/tmp/...: No such file or directory".
# FIVE captures, not one, and this is a part-49 repair of the gate itself. The title
# backdrop is an ANIMATED Still Creek scene and E3 is a photograph of ONE phase of it,
# so a single frame is one sample of a moving target (gotcha 133). Measured, five
# captures per arm in one run:
#
#   60 fps default   0.854  0.740  0.875  0.777  0.688
#   CZ_FPS_CAP=30    0.876  0.649  0.828  0.851  0.710
#
# The spread WITHIN one configuration is ~0.23, the two configurations overlap almost
# entirely -- and **the 30 fps arm produced 0.649, below this gate's own +0.70
# threshold**. So the single-sample form could fail for no reason at all, and had been
# passing at ~0.87 only because its one F9 happened to land on a favourable phase.
#
# The statistic is the BEST of the five. E3 is one phase; the question this gate asks is
# "can our renderer reproduce that phase", so a run that matches it at any sampled
# moment has answered yes. A median would answer a different and less useful question --
# "how close are we on average across phases the reference does not show".
( cd runtime/build && env CZ_NO_WINDOW=1 CZ_VKDRAW=1 "CZ_CAPTURE_KEY=$CAP" \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=NONE,NONE,NONE,F9,F9,F9,F9,F9,NONE \
    timeout 180 ./cz_runtime > "$CAP/run.log" 2>&1 )
# A shader the cache lacks is ONE log line and a silent counter, so it is checked here
# rather than left to be noticed (CLAUDE.md, "the check that costs nothing").
n=$(grep -c "no translated shader" "$CAP/run.log")
[ "$n" = 0 ] || bad "no translated shader x$n  (see $CAP/run.log)"
# `capture_NNNNNN.ppm`, NOT any .ppm: a CZ_CAPTURE_KEY press also writes every resolve
# SNAPSHOT of the frame as `fNNNNNN_snap_*.ppm`, and those sort first. Comparing a
# 32x1 luminance-reduction surface against E3 would fail the gate for the wrong reason.
shots=$(ls "$CAP"/capture_*.ppm 2>/dev/null)
if [ -z "$shots" ]; then
    bad "the capture run produced no frame (see $CAP/run.log)"
else
    best=""; bestc="0"; passed=0
    for shot in $shots; do
        python3 tools/frame_signature.py \
            --ref "Xenia logs/E_screenshots/E3_title_background_stillcreek.png" "$shot" \
            > "$CAP/$(basename "$shot").sig" 2>&1
        c=$(grep -oE 'identity +corr \+[0-9.]+' "$CAP/$(basename "$shot").sig" \
            | grep -oE '[0-9.]+$' | head -1)
        [ -z "$c" ] && c=0
        # "LAYOUT AGREES" is the tool's own pass verdict: best orientation is the
        # identity AND its correlation cleared +0.70. Anything else -- NO MATCH,
        # INCONCLUSIVE, or a transform -- is a failure, so the test is for the pass
        # string rather than against a list of failure strings a future version could
        # add to.
        grep -q "LAYOUT AGREES" "$CAP/$(basename "$shot").sig" && passed=$((passed + 1))
        awk -v a="$c" -v b="$bestc" 'BEGIN{exit !(a>b)}' && { bestc="$c"; best="$shot"; }
        printf '    %-28s corr +%s\n' "$(basename "$shot")" "$c"
    done
    cp "$CAP/$(basename "$best").sig" "$CAP/signature.txt" 2>/dev/null
    echo "  best of $(echo "$shots" | wc -l): +$bestc ($(basename "$best")), $passed agreed on layout"
    grep -q "LAYOUT AGREES" "$CAP/signature.txt" \
        || bad "E3 picture gate: not one of $(echo "$shots" | wc -l) sampled frames \
reported LAYOUT AGREES (see $CAP/*.sig)"
fi

printf '\n'
if [ "$FAIL" = 0 ]; then echo "ALL GATES CLEAN   (capture in $CAP)"; else echo "GATES FAILED   (capture in $CAP)"; fi
exit "$FAIL"
