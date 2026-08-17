#!/bin/bash
# Part 52 operator session — four CPU items shipped, and nobody has looked at them yet.
#
# THE QUESTION: is it faster where you play, and does anything look or feel wrong?
#
# WHY THIS SESSION MATTERS MORE THAN USUAL. Part 52 measured everything headlessly and
# then discovered the headless route has run out of headroom: it now sits ON the frame cap
# for most of its length, so a frame-time A/B there reads zero whatever the change was
# worth (`phase5-notes.md` §6ci §5c). **The operator's frame is heavier than the headless
# one** — historically ~2x, closer in part 51 — so their route may still be below the cap,
# which makes their machine the only place these items can show as frame rate rather than
# as headroom.
#
# WHAT SHIPPED, so the report can agree or disagree with something specific:
#
#   * `BindShader` went from 14.16% of the pump thread to 0.00% — it re-hashed every
#     shader's whole microcode on every load, ~1,300-2,200 times a frame. Worth ~2.4 ms at
#     6,000-7,000 draws headlessly, and that was a LOWER bound.
#   * the pipeline lookup went 110-112 -> 38-43 ns/draw.
#   * ten hot counter sites stopped building a std::string per call.
#
# WHAT COULD HAVE GONE WRONG, and what to look at:
#
#   The memo is a CACHE ON SHADER IDENTITY. Its failure mode is not a crash and not a
#   missing object — it is the WRONG SHADER on something, which reads as a surface with
#   the wrong material: plastic-looking skin, a wall lit like metal, foliage drawn opaque.
#   The design the plan specified would have done exactly that, silently; it was caught
#   and replaced (gotcha 342). The shipped one compares the whole microcode, and a 600 s
#   headless roam under `CZ_PM4_VERIFY_SHADER_HASH=1` reported ZERO disagreements — but
#   that roam is one route and you go places it does not.
#
#   **So: if anything looks like the wrong MATERIAL rather than the wrong geometry, press
#   F9 and say where.** That is the one class this part could have introduced.
#
# INSTRUMENTS, and what is deliberately NOT here.
#
#   `CZ_VK_PROFILE=20` is on: it costs 2-4 ms a frame and it is the only source of the
#   phase split, which is what makes an operator report comparable with the headless
#   numbers. `CZ_VK_FRAME_STATS` is deliberately OFF — part 51 measured it at 1.86-3.32 ms
#   a frame on this machine, and it was on in every performance run this project ever
#   recorded. Leaving it off costs the per-frame draw-bin analysis and buys ~3 ms of the
#   frame you are actually judging, which for a session about FEEL is the right trade.
#   Quote nothing from here as an absolute frame time without saying the profiler was on.
#
# Usage:  tools/part52_operator_session.sh          # one arm, the current build
#         ARM=ab tools/part52_operator_session.sh   # two arms chained: quit one, the
#                                                   # next starts. Arm B restores the
#                                                   # pre-part-52 shader path in the SAME
#                                                   # binary (gotchas 50/51/86).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part52-operator"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

A2M="$ROOT/assets/shader_spv_a2m"

run() {
    local tag="$1"; shift
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag"
    echo "  F9  = screenshot -> $OUT/$tag"
    echo "  F2  = the shipped DebugJump screen (at the title menu)"
    echo "  F4  = the Case Zero debug submenus (Left goes back)"
    echo "  Quit the game normally when you are done."
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_VK_PROFILE=20 \
        "CZ_SHADER_SPV=$A2M" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag finished. log: $OUT/$tag.log"
    grep -c "no translated shader" "$OUT/$tag.log" \
        | sed 's/^/  shaders the cache lacked: /'
    grep "shader memo" "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -E "^\[vkprof\] [0-9]" "$OUT/$tag.log" | tail -3 | sed 's/^/  /'
}

if [ "${ARM:-single}" = ab ]; then
    run part52on
    echo; echo "  Arm A finished. Starting arm B — the pre-part-52 shader path."; echo
    run part52off CZ_PM4_NO_SHADER_MEMO=1
else
    run part52
fi
