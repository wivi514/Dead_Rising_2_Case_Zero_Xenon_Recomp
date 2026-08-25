#!/bin/bash
# THE A/B FOR PART 76's SECOND ITEM — a `getenv` on the per-DRAW path.
#
# `Env()` in the renderer is a bare `getenv`, which in glibc is a linear scan over
# `environ` with a length-prefixed compare: **60.6 ns for a miss and 67.5 ns for a hit in
# a 100-121 entry environment**, measured on this machine. Two of them ran on EVERY DRAW,
# both as the FIRST operand of an `&&` so the cheap register test could not short-circuit
# them:
#
#   if (Env("CZ_CAPTURE_KEY") && (regs[kPaClClipCntl] & 0x3F))   // the clip-draw dump
#   if (!EnvOn("CZ_VK_NO_POLY_OFFSET") && ...)                   // the poly-offset counter
#
# At 6,000 draws that is ~0.8 ms a frame, in every run this project has ever made,
# for a dump that fires at most once in a session and a counter for an arm that is off.
# A third pair ran per RESOLVE (~40-100 a frame). All three are now function-local
# statics; nothing in this process calls `setenv`, so it is semantics-identical.
#
# THE TWO ARMS ARE TWO BINARIES AND THAT IS ADMISSIBLE HERE, unusually — the normal rule
# is that an old binary is never a single-variable arm (gotcha: an-old-binary-is-never-a-
# single-variable-arm), and it holds because an old binary lacks every fix since. These
# two are built from the SAME tree one commit apart with only these three hoists between
# them, so the objection does not apply; `git diff` is the evidence and it is three
# hunks. `CZ_CAPTURE_KEY` is set in both arms because a play session sets it and a HIT
# is the more expensive of the two getenv outcomes.
#
# Usage:  tools/part76_getenv_ab.sh [N]      # N runs per arm, default 3
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SP="${SP:?set SP to the directory holding cz_runtime_hoisted and cz_runtime_getenv}"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part76-getenv}"
RES="${RES:-3440x1440}"
SECS="${SECS:-60}"; PRESSMS="${PRESSMS:-5000}"
N="${1:-3}"
mkdir -p "$OUT"; CAP="$OUT/capdir"; mkdir -p "$CAP"
one() {
    local tag="$1" bin="$2"
    OUT="$OUT" SECS="$SECS" PRESSMS="$PRESSMS" BIN_SRC="$bin" \
        "$ROOT/tools/autoroute.sh" "$tag" \
        "CZ_VK_RES=$RES" "CZ_CAPTURE_KEY=$CAP" "CZ_BURST_DUMP=$CAP"
    echo "  [gate rc=$?]"
}
for i in $(seq 1 "$N"); do
    one "hoist$i" "$SP/cz_runtime_hoisted"
    one "getenv$i" "$SP/cz_runtime_getenv"
done
echo
echo "=== A/B"
python3 "$ROOT/tools/part76_band.py" "hoist=$OUT/auto_*_hoist*.log" "getenv=$OUT/auto_*_getenv*.log"
echo
echo "=== NULL — two runs of the SAME arm; everything below is the floor"
python3 "$ROOT/tools/part76_band.py" "a=$OUT/auto_*_hoist1.log" "b=$OUT/auto_*_hoist3.log"
