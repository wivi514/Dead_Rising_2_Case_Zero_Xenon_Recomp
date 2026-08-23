#!/bin/bash
# PART 72'S CHAINED SESSION — what does the 21:9 culling over-widen ACTUALLY cost?
# Four arms, one sitting, the operator drives, quitting one arm starts the next.
#
# WHY THIS EXISTS. `docs/perf-plan-part72.md` §1 opens with the wide-culling over-widen
# priced at "+1,930 draws of 9,817, ≈4.8 ms of 28 — the best-priced item in the plan". That
# number came from part 71's `CZ_NO_GAME_FOV=1` arm, and part 72 found it is an UPPER
# BOUND rather than the item's value: the arm turns off the WHOLE fov substitution, and the
# substitution does two things — it widens the game's frustum horizontally (part 62's fix,
# which stops the flanks popping in, and which is being KEPT) and vertically (the waste,
# which is the item). The configuration a horizontal-only fix would reach has the arm's
# vertical and today's horizontal, so its frustum is a strict subset of today's and a
# strict superset of the arm's, and the recoverable draws are strictly fewer than 1,930.
# Two models put it near half. That is a factor of two on the plan's first item, decided by
# a model — which is exactly what this project's evidence rules say not to do.
#
# So `CZ_VK_VCULL_CENSUS=1` measures it instead. For every world draw it projects the
# position stream's own object-space box by the FINAL projection and counts the draws whose
# box lands entirely outside the clip volume in Y. Those draws produce no pixel. They are
# the CEILING on what any vertical-cull fix can recover, and unlike the arm difference they
# carry no horizontal confound. `phase5-notes.md` §6de.
#
# THE ARMS. Two of them are CONTROLS and are not measurements — the harness says so in
# their banner, and their numbers exist only to show the census can move.
#
#   census      CZ_VK_VCULL_CENSUS=1. THE MEASUREMENT. Soak at the heaviest spot.
#   nofov       + CZ_NO_GAME_FOV=1. THE SEMANTIC CONTROL: the over-widen is gone, so the
#               vertical waste must FALL SHARPLY. If it does not, the census is measuring
#               something other than the over-widen and its headline is worthless.
#   scalelow    + CZ_VK_VCULL_SCALE=0.02. MECHANICAL CONTROL: a tiny clip bound must drive
#               the count UP toward every off-centre draw.
#   scalehigh   + CZ_VK_VCULL_SCALE=50. MECHANICAL CONTROL, the other direction: a huge
#               clip bound must drive the count to ZERO.
#
# AFTER PART 72's FIRST SITTING, RUN ONLY TWO OF THEM: `ORDER=census,nofov`. The two
# mechanical arms already fired correctly (5,653 at 0.02, 0 at 50, monotone both ways) and
# the predicate they exercise is unchanged and has an OFFLINE gate that covers it —
# repeating them spends operator time confirming something already confirmed. What the
# first sitting actually caught was that the boxes were never PLACED (the census projected
# object space by the camera matrix and read 98.1% of the world off-screen); that is fixed,
# and the census now REFUSES to print a headline when its own on-screen invariant fails, so
# a second failure names itself instead of hiding in a plausible number.
# `docs/part72-fix-plan.md` §4.
#
# The two mechanical arms need ~30 s each, not a soak — they are asking whether the
# predicate fires at all, and that is answered by the first `[vcull]` dump.
#
# WHY BOTH DIRECTIONS. A count that is stuck at zero and a count that is stuck at "every
# draw" both look like a plausible answer from one arm. Only a monotone response in both
# directions shows the predicate is reading the geometry rather than a constant
# (gotcha 30). The predicate additionally has an OFFLINE gate that runs in a second and
# was confirmed capable of failing:
#
#     clang++ -O1 -o /tmp/vp tools/vcull_predicate_test.cpp -lm && /tmp/vp
#
# FRAME TIMES FROM THIS SESSION ARE WORTHLESS BY CONSTRUCTION and the harness refuses to
# print them. The census does a map lookup and ~100 flops per world draw on the pump
# thread, ~9,800 times a frame (gotcha 7). `CZ_FPS_LOG` is still on, but ONLY for its draw
# counts — the census reports wasted draws per frame and the fraction needs a denominator
# from the same run, because "1,000 wasted" means something different at 9,800 draws than
# at 2,500 (gotcha 417: this workload's conclusions flip when the draw band moves).
#
# EVERY ARM PROVES IT ENGAGED OR THE HARNESS REFUSES TO REPORT IT (gotcha 408), from a line
# the FEATURE prints and not from the variable being set. Those gates have their own gate:
# `SELFTEST=1 tools/part72_vcull_session.sh` runs twelve cases — four clean arms and eight
# deliberate breakages — and exits non-zero if any gate accepts a log it should refuse.
#
# THE OTHER THING OWED THIS SITTING is the pipeline-cache attribution run, which is 90
# seconds and a separate harness — run it first or last, it does not interact:
#
#     ORDER=cold,warm,nocache tools/part71_pipeline_session.sh
#
# GOD MODE, NO DEATH SEQUENCE and ZOMBIES IGNORE ALL HUMANS ride in every arm through
# CZ_DEBUG_FLAGS, for part 71's reason: standing still in the heaviest place is exactly
# where Chuck dies, and a zombie grab rotates the camera, which changes the draw set — the
# one thing a soak exists to hold still.
#
# Usage:  tools/part72_vcull_session.sh
#         ORDER=census,nofov tools/part72_vcull_session.sh    # any order, any subset
#         SECS=120 tools/part72_vcull_session.sh              # a shorter soak
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part72-vcull"
ORDER="${ORDER:-census,nofov}"   # see the note above: the mechanical arms are done
SECS="${SECS:-180}"
FPS="${FPS:-500}"
FLAGS="${FLAGS:-CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS}"
mkdir -p "$OUT"

# The self-test is a pure unit test of the gates below: it must not refuse to run because
# the game is up, and it must not touch the build directory. Everything between here and
# the SELFTEST block is skipped for it.
busy=""
for p in $(: ${SELFTEST:+skip}; [ -n "${SELFTEST:-}" ] || pgrep -x cz_runtime 2>/dev/null) \
         $([ -n "${SELFTEST:-}" ] || pgrep -x cz_runtime_p72 2>/dev/null); do
    busy="$busy  $p $(cat "/proc/$p/comm" 2>/dev/null)"$'\n'
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

# ONE BINARY FOR ALL ARMS, snapshotted so nothing rebuilt between them can be blamed.
BIN=cz_runtime_p72
HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
if [ -z "${SELFTEST:-}" ]; then
    cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($HEAD)"
fi
STAMP="$(date +%m%d_%H%M)"

# ---- PREFLIGHT ----------------------------------------------------------------------
#
# THE PREDICATE'S OWN GATE FIRST. It costs a second and it is the only check in this
# session that can catch a sign error in the thing being measured — a sign error there
# does not crash, does not look wrong, and reports a plausible number that no amount of
# operator time can audit (gotcha 425).
if command -v clang++ >/dev/null 2>&1; then
    if clang++ -O1 -o "/tmp/vcull_predicate_$$" "$ROOT/tools/vcull_predicate_test.cpp" -lm 2>/dev/null \
       && "/tmp/vcull_predicate_$$" >/dev/null 2>&1; then
        echo "preflight: vcull predicate gate PASSES (13/13)"
    else
        echo "!! the vcull predicate gate FAILS — the census's arithmetic is wrong."
        echo "   Fix that before spending operator time:"
        echo "     clang++ -O1 -o /tmp/vp tools/vcull_predicate_test.cpp -lm && /tmp/vp"
        rm -f "/tmp/vcull_predicate_$$"
        exit 3
    fi
    rm -f "/tmp/vcull_predicate_$$"
fi

# THE NAME DIFF, not the count (gotcha 390). This session does not switch caches, but it
# selects one, and part 65 found the selected cache ten modules short for three parts —
# every draw bound to a missing shader is SKIPPED, which is both a wrong picture and a
# smaller draw count, and the draw count is this session's entire result.
if [ -d "$ROOT/assets/shader_spv_clip_a2m" ] && [ -d "$ROOT/assets/shader_spv" ]; then
    if ! diff -q <(cd "$ROOT/assets/shader_spv_clip_a2m" && ls *.spv) \
                 <(cd "$ROOT/assets/shader_spv" && ls *.spv) >/dev/null; then
        echo "!! the play cache and the stock cache hold DIFFERENT shader sets."
        echo "   Fix that first — a skipped draw shrinks the very number being measured."
        exit 3
    fi
    echo "preflight: play cache $(ls "$ROOT/assets/shader_spv_clip_a2m"/*.spv | wc -l) modules, name set matches stock"
fi

# THE CONFIGURATION, quoted with the result (gotcha 419). This session is ENTIRELY about
# wide mode: at a 16:9 internal resolution there is no over-widen, the census must read
# ~zero, and the sitting answers nothing.
if [ -f "$ROOT/assets/save/cz_settings.txt" ]; then
    echo "preflight: cz_settings.txt ->" \
         "$(grep -av '^#' "$ROOT/assets/save/cz_settings.txt" | tr '\n' ' ')"
    rw=$(grep -a '^res_w=' "$ROOT/assets/save/cz_settings.txt" | cut -d= -f2)
    rh=$(grep -a '^res_h=' "$ROOT/assets/save/cz_settings.txt" | cut -d= -f2)
    wide=0
    [ -n "${rw:-}" ] && [ -n "${rh:-}" ] && [ $((rw * 9)) -gt $((rh * 16)) ] && wide=1
    if [ "$wide" = 0 ]; then
        echo "  !! the internal resolution is 16:9 — THERE IS NO OVER-WIDEN TO MEASURE."
        echo "     Set a wider resolution in the settings panel, or this sitting answers"
        echo "     nothing at all. (k = 9W/16H = 1.0 means the substitution is inert.)"
        exit 4
    fi
    k=$(awk -v w="$rw" -v h="$rh" 'BEGIN{printf "%.4f", (9*w)/(16*h)}')
    echo "  wide mode IS on: ${rw}x${rh}, k = 9W/16H = $k — the frustum is over-widened"
    echo "  vertically by that factor, and that is what the census prices."
fi

arm_desc() {
    case "$1" in
      census)    echo "CZ_VK_VCULL_CENSUS=1 — THE MEASUREMENT" ;;
      nofov)     echo "+ CZ_NO_GAME_FOV=1 — SEMANTIC CONTROL, the waste must FALL SHARPLY" ;;
      scalelow)  echo "+ CZ_VK_VCULL_SCALE=0.02 — MECHANICAL CONTROL, the count must RISE" ;;
      scalehigh) echo "+ CZ_VK_VCULL_SCALE=50 — MECHANICAL CONTROL, the count must go to ZERO" ;;
      *)         echo "UNKNOWN ARM" ;;
    esac
}
arm_secs() {
    # The controls answer "can the predicate move", which the first dump settles. Only the
    # two census arms need a soak, and only they are compared with each other.
    case "$1" in scalelow|scalehigh) echo 40 ;; *) echo "$SECS" ;; esac
}
arm_task() {
    case "$1" in
      scalelow|scalehigh)
        echo ">>> Get to ANY outdoor spot with a crowd, stand for ~40 s, quit."
        echo "    >>> This is a CONTROL. Its number is not a result." ;;
      *)
        echo ">>> 1. Go to THE SAME heaviest spot in both census arms."
        echo "    >>> 2. STAND STILL for ~$((SECS / 60)) min. Do not walk."
        echo "    >>> 3. QUIT. The next arm starts by itself." ;;
    esac
}

run_arm() {
    local arm="$1" n="$2" total="$3"
    local tag="p72_${STAMP}_${n}_${arm}"
    local extra=(CZ_VK_VCULL_CENSUS=1)
    case "$arm" in
      census)    ;;
      nofov)     extra+=(CZ_NO_GAME_FOV=1) ;;
      scalelow)  extra+=(CZ_VK_VCULL_SCALE=0.02) ;;
      scalehigh) extra+=(CZ_VK_VCULL_SCALE=50) ;;
      *) echo "!! unknown arm '$arm'"; return 1 ;;
    esac
    cat <<BANNER

===================================================================
  ARM $n of $total:  $arm
  $(arm_desc "$arm")

  res:   from your cz_settings.txt (unchanged — every arm the same)
  cap:   CZ_FPS_CAP=$FPS   instruments: CZ_FPS_LOG + the vcull census
  debug: $FLAGS
  log:   $OUT/$tag.log

  >>> THE FRAME WILL BE SLOWER IN EVERY ARM AND THAT IS EXPECTED —
  >>> the census costs work per draw. No frame time from this
  >>> session is quotable; the COUNTS are the result.

  $(arm_task "$arm")
===================================================================

BANNER
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 \
        "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=10 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=$FLAGS" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
        "${extra[@]}" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    echo "  arm $n ($arm) finished."
}

# THE ENGAGEMENT GATE. Each arm names lines in its OWN log that can only be there if the
# variable took effect. Every one of these is two-sided where a one-sided test would pass
# for the wrong reason: "the census is armed" is not "the control also engaged".
engaged() {
    local arm="$1" f="$2"
    # Common to all four: the census announced itself AND produced a dump with world
    # frames in it. "Armed but the run never reached world geometry" prints
    # `no world frames seen`, and that is a failed arm, not a zero.
    grep -aq "CZ_VK_VCULL_CENSUS=1 — the vertical-waste census is ARMED" "$f" || return 1
    grep -aq "^\[vcull\] [1-9][0-9]* world frames" "$f" || return 1
    case "$arm" in
      census)
        # Two-sided: neither control may have been on. The fov substitution must be LIVE
        # (its own first-fire line), or there is no over-widen to measure.
        ! grep -aq "CZ_VK_VCULL_SCALE" "$f" && \
        grep -aq "game-side fov ACTIVE" "$f" ;;
      nofov)
        # Two-sided: the arm announced itself AND the substitution never fired.
        grep -aq "CZ_NO_GAME_FOV=1 — the guest-side fov substitution is OFF" "$f" && \
        ! grep -aq "game-side fov ACTIVE" "$f" ;;
      scalelow)
        grep -aq "CZ_VK_VCULL_SCALE=0.02 — the clip bound is scaled" "$f" ;;
      scalehigh)
        grep -aq "CZ_VK_VCULL_SCALE=50 — the clip bound is scaled" "$f" ;;
      *) return 1 ;;
    esac
}

# THE GATES' OWN GATE. A gate that has never been shown capable of refusing is not a
# gate (gotcha 30), and part 71's rule is that every one is tested against a deliberate
# breakage BEFORE an operator sees it. Keeping the cases here rather than in a scratch
# file is what stops them drifting from the gate they test.
#
#     SELFTEST=1 tools/part72_vcull_session.sh
#
if [ -n "${SELFTEST:-}" ]; then
    d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
    ARMED='[vcull] CZ_VK_VCULL_CENSUS=1 — the vertical-waste census is ARMED; diagnostic'
    FRAMES='[vcull] 1200 world frames  scene draws/frame 9800  TESTED 9000 (91.8%)  untestable 800  near-plane 40'
    FOVON='[fovgame] game-side fov ACTIVE at the camera param getter: base+10 deg, factor 1.3438'
    FOVOFF='[fov] CZ_NO_GAME_FOV=1 — the guest-side fov substitution is OFF; the game culls to its own 16:9 frustum'
    SLOW='[vcull] CZ_VK_VCULL_SCALE=0.02 — the clip bound is scaled; this is the MECHANICAL control, not a measurement'
    SHIGH='[vcull] CZ_VK_VCULL_SCALE=50 — the clip bound is scaled; this is the MECHANICAL control, not a measurement'
    mk() { local f="$d/$1"; shift; printf '%s\n' "$@" > "$f"; }
    mk g_census     "$ARMED" "$FOVON" "$FRAMES"
    mk g_nofov      "$ARMED" "$FOVOFF" "$FRAMES"
    mk g_scalelow   "$ARMED" "$FOVON" "$SLOW" "$FRAMES"
    mk g_scalehigh  "$ARMED" "$FOVON" "$SHIGH" "$FRAMES"
    # One breakage per failure mode this session can actually suffer.
    mk b_notarmed      "$FOVON" "$FRAMES"
    mk b_noworld       "$ARMED" "$FOVON" '[vcull] no world frames seen'
    mk b_censusfovoff  "$ARMED" "$FOVOFF" "$FRAMES"
    mk b_censusscaled  "$ARMED" "$FOVON" "$SLOW" "$FRAMES"
    mk b_nofovstillon  "$ARMED" "$FOVOFF" "$FOVON" "$FRAMES"
    mk b_nofovnovar    "$ARMED" "$FRAMES"
    pass=0; bad=0
    t() { engaged "$2" "$d/$3"; local got=$?
          if [ "$got" = "$4" ]; then echo "  ok    $1"; pass=$((pass+1))
          else echo "  FAIL  $1 (got $got want $4)"; bad=$((bad+1)); fi; }
    echo "SHOULD ENGAGE:"
    t "census    clean"             census    g_census     0
    t "nofov     clean"             nofov     g_nofov      0
    t "scalelow  clean"             scalelow  g_scalelow   0
    t "scalehigh clean"             scalehigh g_scalehigh  0
    echo "SHOULD REFUSE (deliberate breakages):"
    t "census    never armed"       census    b_notarmed      1
    t "census    no world frames"   census    b_noworld       1
    t "census    ran with fov OFF"  census    b_censusfovoff  1
    t "census    ran SCALED"        census    b_censusscaled  1
    t "nofov     substitution ON"   nofov     b_nofovstillon  1
    t "nofov     var never read"    nofov     b_nofovnovar    1
    t "scalelow  got the 50 arm"    scalelow  g_scalehigh     1
    t "scalehigh got the 0.02 arm"  scalehigh g_scalelow      1
    echo; echo "$pass passed, $bad failed"
    exit $bad
fi

n=1
IFS=',' read -ra arms <<< "$ORDER"
for a in "${arms[@]}"; do run_arm "$a" "$n" "${#arms[@]}"; n=$((n + 1)); done

echo
echo "==================================================================="
echo "  ALL ARMS DONE — $HEAD"
fail=0
for a in "${arms[@]}"; do
    f=$(ls "$OUT"/p72_"$STAMP"_*_"$a".log 2>/dev/null | head -1)
    echo
    echo "--- $a  ($(basename "${f:-MISSING}"))"
    if [ -z "$f" ] || ! engaged "$a" "$f"; then
        echo "  ** NOT ENGAGED — this arm ran as something other than what it claims."
        echo "     Its numbers are NOT reportable. (gotcha 408)"
        fail=1
        [ -n "$f" ] || continue
    else
        echo "  ENGAGED."
    fi
    grep -a "internal resolution\|shader cache:" "$f" | tail -2
    grep -a "game-side fov ACTIVE\|CZ_NO_GAME_FOV=1" "$f" | tail -1
    # THE RESULT. The last dump is the one with the most frames behind it.
    echo "  [vcull] last dump:"
    grep -a "^\[vcull\]" "$f" | tail -6 | sed 's/^/    /'
    # THE INVARIANT'S VERDICT, hoisted so it cannot be missed in the scroll. A refusal is
    # not a failed run — it is the census declining to publish a number it cannot stand
    # behind, and the offender lines beside it say why.
    if grep -aq "REFUSING TO REPORT" "$f"; then
        echo "  ** THE CENSUS REFUSED TO REPORT. Its on-screen invariant failed, so its"
        echo "     vertical figure is NOT item 1's price. The offenders it named:"
        grep -a "offender " "$f" | head -6 | sed 's/^/       /'
    fi
    # THE DENOMINATOR, from the same run (gotcha 417): "1,000 wasted" means something
    # different at 9,800 draws than at 2,500, and these arms will not sit at the same load.
    echo "  [fps] windows (for the DRAW COUNTS — the frame times are inflated by the census):"
    grep -a "^\[fps\]" "$f" | tail -4 | sed 's/^/    /'
done

echo
echo "  shaders the caches lacked: $(grep -ac 'no translated shader' "$OUT"/p72_"$STAMP"_*.log 2>/dev/null | paste -sd+ | bc 2>/dev/null || echo '?')"
cat <<'READ'

  HOW TO READ IT
  --------------
  1. CONTROLS FIRST, and if either fails, stop — the headline means nothing.
     scalelow  must read a MUCH HIGHER vertical-waste count than `census`.
     scalehigh must read ~ZERO.
     A count that does not move in both directions is a constant, not a measurement.
  2. THE SEMANTIC CONTROL. `nofov` must read a MUCH LOWER count than `census`. If it
     does not, the census is counting draws that are off-screen for some other reason
     and it is not pricing the over-widen.
  3. THEN THE HEADLINE: `census`'s "ENTIRELY OFF-SCREEN VERTICALLY: N draws/frame".
     That N is the CEILING on what a horizontal-only culling fix can recover. Multiply
     by the soak's ~2.5 us/draw for the millisecond figure, and quote the run's own
     draws/frame beside it.
  4. THE PRE-REGISTERED DECISION (perf-plan-part72 §1): below 700 draws recovered
     (~1.75 ms) this item is not worth a picture risk and should be killed. Above it,
     the choice is route (b) — hooking the engine's cull, which has no debug surface —
     or route (c), a smaller k, which is a PICTURE decision and the operator's call.
  5. Watch `untestable` as well as the headline. If it is a large share of the scene
     draws, the census is pricing a fraction of the population and N is a floor as
     well as a ceiling — say so rather than quoting N alone (gotcha 25).
READ
exit $fail
