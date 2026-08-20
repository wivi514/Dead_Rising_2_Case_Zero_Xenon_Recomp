#!/bin/bash
# Part 58 operator session — the clip-plane SHIFT ladder, four short arms, chained.
#
# WHAT THIS SESSION DECIDES. Part 57 left two slicing residuals: the cut is see-through
# (the gore that seals it is missing) and a thin slab sometimes doubles. Part 57's
# hypothesis — our dot uses the wrong SPACE — is REFUTED offline: all 88 captured planes
# are unit view-space planes under the scene projection (tools/clip_plane_space.py), so
# our raw-oPos dot is exactly hardware's. What was NEVER measured is the margin: how far
# from the clip boundary the gore geometry sits. CZ_VK_CLIP_BIAS could not measure it —
# it ROTATES the plane (part 57's eps=0.01 moved the boundary literal meters, which is
# why the whole body un-clipped). CZ_VK_CLIP_SHIFT (new) translates the boundary by true
# view-space METERS.
#
# HOW TO READ THE LADDER:
#   * see-through cut HEALS at +0.02 and worsens at -0.02  -> the gore sits within ~2 cm
#     of the boundary and our clip errs at precision scale — the fix hunt moves to the
#     dot's arithmetic.
#   * NO CHANGE at +/-0.02 while +/-0.05 visibly moves the cut -> the gore is lost by a
#     NON-CLIP mechanism (the four-pass depth/stencil interlock) and the clip branch is
#     CLOSED for good.
#   * the +/-0.05 arms are the ladder's own positive controls: both halves' kept sets
#     grow by 5 cm each at +0.05, so a ~10 cm doubled band MUST appear at every cut
#     (and a ~10 cm gap at -0.05). If they do not, the arm never engaged — say so.
#
# THE ARMS, in launch order (all on the clip cache, SAFE on, F9 captures wired):
#
#   arm 1  shift_null   (~2 min)  shift 0.00 — today's baseline, same session, same
#                                 spot. Cut 2-3 zombies, F9 each result: the see-through
#                                 and any doubling, as they are RIGHT NOW.
#   arm 2  shift_p05    (~1 min)  +0.05 m — the positive control. EXPECTED: a clearly
#                                 doubled ~10 cm band at every cut. F9 one cut.
#   arm 3  shift_p02    (~2 min)  +0.02 m — the probe. Cut several. Is the cut still
#                                 see-through? F9 each.
#   arm 4  shift_m02    (~2 min)  -0.02 m — the probe's other side. EXPECTED if the
#                                 margin story is right: see-through clearly WORSE.
#
# Same spot each arm if possible (the gas station street worked in part 57). Quit the
# game normally when an arm is done; the next starts itself.
#
# Usage:  tools/part58_operator_session.sh          # all four arms, chained
#         START=3 tools/part58_operator_session.sh  # skip to arm N
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part58-operator"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -f cz_runtime 2>/dev/null); do
    c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
    case "$c" in cz_runtime*) busy="$busy  $p $c"$'\n' ;; esac
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

CLIP="$ROOT/assets/shader_spv_clip_a2m"   # a2m foliage fix + the clip-plane epilogue
SAFE_FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS"

run() {
    local tag="$1"; local shift_m="$2"; shift 2
    mkdir -p "$OUT/$tag"
    echo "==================================================================="
    echo "  ARM: $tag   (CZ_VK_CLIP_SHIFT=$shift_m)"
    echo "  F9 = capture -> $OUT/$tag"
    echo "  Quit the game normally when you are done; the next arm starts itself."
    echo "==================================================================="
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FPS_CAP=500 CZ_FPS_LOG=10 \
        "CZ_SHADER_SPV=$CLIP" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_VK_CLIP_SHIFT=$shift_m" \
        "CZ_DEBUG_FLAGS=$SAFE_FLAGS" \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_BURST_DUMP=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "$@" \
        ./cz_runtime > "$OUT/$tag.log" 2>&1 )
    echo "  ARM $tag finished. log: $OUT/$tag.log"
    grep -ac "no translated shader" "$OUT/$tag.log" | sed 's/^/  shaders the cache lacked: /'
    grep -a  "user clip plane 0 published" "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    grep -a  "user clip plane SHIFTED"     "$OUT/$tag.log" | tail -1 | sed 's/^/  /'
    echo
}

S="${START:-1}"

if [ "$S" -le 1 ]; then
cat <<'B1'

  ARM 1 — SHIFT 0.00: today's baseline (~2 min).
  Cut 2-3 zombies in half at your usual spot. F9 every result: the see-through
  cut and any doubled slab, exactly as they are today, in THIS session's light.
B1
run shift_null 0.0
fi

if [ "$S" -le 2 ]; then
cat <<'B2'

  ARM 2 — SHIFT +0.05 m: the positive control (~1 min).
  EXPECTED AND CORRECT: every cut shows a clearly DOUBLED band, ~10 cm of body
  present on both halves. That doubling IS the pass — it proves the shift arm
  engages. If the cuts look the same as arm 1, the arm is broken: say so.
  One or two cuts, F9, quit.
B2
run shift_p05 0.05
fi

if [ "$S" -le 3 ]; then
cat <<'B3'

  ARM 3 — SHIFT +0.02 m: the probe (~2 min).
  Cut several zombies. THE QUESTION: is the cut still see-through, or does the
  gore/flesh now seal it? F9 every cut, healed or not.
B3
run shift_p02 0.02
fi

if [ "$S" -le 4 ]; then
cat <<'B4'

  ARM 4 — SHIFT -0.02 m: the probe's other side (~2 min).
  If the margin story is right the see-through should be clearly WORSE here
  (a visible gap at every cut). F9 a couple of cuts.
B4
run shift_m02 -0.02
fi

echo "  all arms done. Everything is in $OUT/"
