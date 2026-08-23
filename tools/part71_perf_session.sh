#!/bin/bash
# PART 71'S CHAINED PERFORMANCE SESSION — four arms, one sitting, the operator drives,
# quitting one arm starts the next.
#
# WHY THIS EXISTS, and why it is the FIRST thing part 71 does rather than an experiment:
# the frame at the operator's soak has not been measured since part 58, and thirteen parts
# have shipped since (the fov slider, the settings panel, the wide culling fix, RT stage 2
# and four RT sub-features, part 70's per-draw sun probe). Every item in
# `docs/perf-state-parked.md` §2 is priced against a ~10.5 ms baseline that no longer
# exists. A recorded estimate has the same shelf life as a recorded measurement
# (gotchas 13, 50/51/86: the control is the old binary run NOW, not its remembered
# number). So arm 1 is not an experiment, it is a denominator.
#
# THE ARMS, one variable each against the shipped default:
#
#   base        the shipped default WITH part 71's per-draw hook fold. The re-baseline.
#   nofold      CZ_VK_NO_HOOK_FOLD=1 — the five per-draw census/collect hooks and the
#               per-FETCH `NoteAtlasFetch` decode called on every draw exactly as parts
#               59-70 left them. This arm is ALSO the pre-part-71 build, so its median is
#               what compares with part 58's 11.8-12.3 ms; `base` minus this is the fold.
#   noclip      CZ_SHADER_SPV=assets/shader_spv_a2m — the stock (no user-clip-plane)
#               shader cache. Six plane dots per vertex in all 104 VS plus a per-draw
#               plane-block zero+publish; Night Run 1 headlessly measured +0.20 ms / +3.0%
#               at ~5,000 draws and ~0 at 2,500, so it is a suspect and not a conclusion.
#   nogamefov   CZ_NO_GAME_FOV=1 — the guest-side fov substitution off, which takes part
#               62's wide-mode over-widen (k=1.34 in tan space, ~1.8x the culled volume)
#               with it. THIS IS THE TURN-STUTTER ARM.
#
# WHY `CZ_VK_WIDE=0` IS **NOT** THE WIDE ARM HERE, though the plan named it. This
# operator's `cz_settings.txt` is 3440x1440, so `CZ_VK_WIDE=0` would force the internal
# resolution to 2560x1440 — 26% fewer pixels. That is a RESOLUTION change wearing a
# culling change's label, and a frame-time delta measured across it would be mostly GPU.
# `CZ_NO_GAME_FOV=1` removes the over-widen at a constant pixel count, which is the
# question. It does change the draw SET (the game culls to its own narrower frustum), so
# by this project's own admissibility rule its frame time is not comparable with the other
# arms' — read it for the TURN statistics and the felt verdict, not for a headline.
#
# WHY `CZ_VK_RT=0` IS NOT AN ARM, though the plan named it as the crude bound on parts
# 59-70's per-draw hooks. It does not bound them. `ps.moduleRt` is populated from
# `assets/shader_spv_*_rt` with no reference to `rtEnabled`, and `NoteAtlasFetch`'s guard
# was `ps.moduleRt` alone — so `CZ_VK_RT=0` leaves every one of those 9.5 M-per-100 s
# fetch decodes in place and only makes two already-false predicates false slightly
# sooner. `CZ_VK_NO_HOOK_FOLD` is the arm that actually brackets the cost. Corrected in
# `docs/perf-plan-part71.md` §1.4 in place.
#
# EVERY ARM PROVES IT ENGAGED OR THE HARNESS REFUSES TO REPORT IT (gotcha 408). The
# summary at the end prints ENGAGED/**NOT ENGAGED** per arm from a line in the arm's own
# log, and exits non-zero if any arm failed. An arm that silently ran as the default is
# how part 69 shipped a null it had never measured.
#
# THE SOAK, then a TURN BLOCK. Their own framing for the soak (part 55): "soak at the spot
# that hit the cpu the most so we just get 2x 3minutes soak instead of hours of testing in
# unstable environment with autochuck". The turn block is new in part 71 and exists
# because the stutter they reported is the one performance problem on this port that has
# only ever been FELT: a median cannot see it, so `CZ_FPS_LOG` now also prints p99, the
# worst frame and the share of frames above 2x the window median, and 30 s of continuous
# turning at the end of each arm gives those three numbers a place to live.
#
# DELIBERATELY NOT ON: `CZ_VK_PROFILE` (2-4 ms/frame) and `CZ_VK_FRAME_STATS` (1.9-3.3),
# either of which would change the thing being judged (gotcha 337). `CZ_FPS_LOG` is one
# counter and one clock read per presented frame.
#
# GOD MODE, NO DEATH SEQUENCE and ZOMBIES IGNORE ALL HUMANS are held in EVERY arm through
# `CZ_DEBUG_FLAGS` rather than the F4 menu, so they are identical and automatic and cannot
# differ between arms. Standing still in the heaviest place is exactly where Chuck dies,
# and a zombie GRAB rotates the camera, which changes the draw set — the one thing a soak
# exists to hold still.
#
# Usage:  tools/part71_perf_session.sh
#         ORDER=nofold,base tools/part71_perf_session.sh     # any order, any subset
#         SECS=120 tools/part71_perf_session.sh              # a shorter soak
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part71-perf"
ORDER="${ORDER:-base,nofold,noclip,nogamefov}"
SECS="${SECS:-180}"
FPS="${FPS:-500}"
FLAGS="${FLAGS:-CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS}"
mkdir -p "$OUT"

busy=""
for p in $(pgrep -x cz_runtime 2>/dev/null) $(pgrep -x cz_runtime_p71 2>/dev/null); do
    busy="$busy  $p $(cat "/proc/$p/comm" 2>/dev/null)"$'\n'
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

# ONE BINARY FOR ALL ARMS, snapshotted so nothing rebuilt between them can be blamed.
BIN=cz_runtime_p71
cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
echo "snapshot: $BIN <- cz_runtime ($HEAD)"

STAMP="$(date +%m%d_%H%M)"

# ---- PREFLIGHT, before an operator spends twenty minutes on it -----------------------
#
# THE NAME DIFF, not the count. Part 65 found the cache this harness selects holding 439
# modules against the stock 449 for three parts — the ten ABSENT rather than stale, so
# every draw bound to one printed `no translated shader` and was SKIPPED. A skipped draw
# is both a wrong picture and a faster frame, which is the worst possible confound for a
# performance A/B: it would have read as a win (gotcha 390).
pf=0
for d in shader_spv_clip_a2m shader_spv_a2m; do
    if [ ! -d "$ROOT/assets/$d" ]; then echo "!! missing cache assets/$d"; pf=1; fi
done
if [ "$pf" = 0 ]; then
    if ! diff -q <(cd "$ROOT/assets/shader_spv_clip_a2m" && ls *.spv) \
                 <(cd "$ROOT/assets/shader_spv_a2m" && ls *.spv) >/dev/null; then
        echo "!! the two caches this session switches between hold DIFFERENT shader sets."
        echo "   Fix that first — a skipped draw reads as a faster frame."
        exit 3
    fi
    echo "preflight: both caches $(ls "$ROOT/assets/shader_spv_a2m"/*.spv | wc -l) modules, name sets identical"
fi
# The settings this session inherits. Quoted because a measurement has a CONFIGURATION as
# well as a workload, and because `CZ_NO_GAME_FOV` is inert at fov=0 and `aspect`/`res_*`
# decide whether wide mode is on at all — i.e. whether the turn arm can move anything.
if [ -f "$ROOT/assets/save/cz_settings.txt" ]; then
    echo "preflight: cz_settings.txt ->" \
         "$(grep -av '^#' "$ROOT/assets/save/cz_settings.txt" | tr '\n' ' ')"
    # The substitution is live when `fovAdj != 0 || wideK != 1.0` (camera_fov.cpp), so
    # the arm is inert only when the slider is at zero AND the internal resolution is
    # 16:9. Checking one of the two would have declared the arm dead at fov=0 on a 21:9
    # display, which is exactly this operator's configuration.
    fovv=$(grep -a '^fov=' "$ROOT/assets/save/cz_settings.txt" | cut -d= -f2)
    rw=$(grep -a '^res_w=' "$ROOT/assets/save/cz_settings.txt" | cut -d= -f2)
    rh=$(grep -a '^res_h=' "$ROOT/assets/save/cz_settings.txt" | cut -d= -f2)
    wide=0
    [ -n "${rw:-}" ] && [ -n "${rh:-}" ] && [ $((rw * 9)) -gt $((rh * 16)) ] && wide=1
    if [ "${fovv:-0}" = "0" ] && [ "$wide" = 0 ]; then
        echo "  !! fov=0 AND a 16:9 internal resolution — the guest-side substitution is"
        echo "     not running at all, so 'nogamefov' cannot move anything. Drop it with"
        echo "     ORDER=base,nofold,noclip, or set the fov slider off zero."
    else
        echo "  the fov substitution WILL be live (fov=${fovv:-0}, wide=$wide) — the"
        echo "  'nogamefov' arm has something to turn off."
    fi
fi

arm_desc() {
    case "$1" in
      base)      echo "the shipped default — part 71's per-draw hook fold ON" ;;
      nofold)    echo "CZ_VK_NO_HOOK_FOLD=1 — the pre-part-71 per-draw hooks and per-fetch decode" ;;
      noclip)    echo "CZ_SHADER_SPV=assets/shader_spv_a2m — the STOCK cache, no user clip planes" ;;
      nogamefov) echo "CZ_NO_GAME_FOV=1 — the guest-side fov substitution and the wide over-widen OFF" ;;
      *)         echo "UNKNOWN ARM" ;;
    esac
}

run_arm() {
    local arm="$1" n="$2" total="$3"
    local tag="p71_${STAMP}_${n}_${arm}"
    local extra=()
    case "$arm" in
      base)      ;;
      nofold)    extra+=(CZ_VK_NO_HOOK_FOLD=1) ;;
      noclip)    extra+=("CZ_SHADER_SPV=$ROOT/assets/shader_spv_a2m") ;;
      nogamefov) extra+=(CZ_NO_GAME_FOV=1) ;;
      *) echo "!! unknown arm '$arm'"; return 1 ;;
    esac
    # The a2m foliage configuration rides in EVERY arm — it is what the operator plays
    # (part 46), so a run without it is not their game. `noclip` overrides only the
    # DIRECTORY, above, and the two caches carry byte-identical name sets (449 each).
    local cache=(CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1)
    [ "$arm" != noclip ] && cache+=("CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m")
    cat <<BANNER

===================================================================
  ARM $n of $total:  $arm
  $(arm_desc "$arm")

  res:   from your cz_settings.txt (unchanged — every arm the same)
  cap:   CZ_FPS_CAP=$FPS      instruments: CZ_FPS_LOG only
  debug: $FLAGS
  log:   $OUT/$tag.log

  >>> 1. Go to THE SAME heaviest spot in every arm.
  >>> 2. STAND STILL for ~$((SECS / 60)) min. Do not walk — a soak is worth ~16x a walk here.
  >>> 3. THEN, for the last ~30 s, TURN THE CAMERA continuously —
  >>>    slow full circles, both directions. This is the stutter test;
  >>>    say out loud afterwards whether it stuttered in this arm.
  >>> 4. QUIT. The next arm starts by itself.
===================================================================

BANNER
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 \
        "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=10 \
        "CZ_CAPTURE_KEY=$OUT/$tag" \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=$FLAGS" \
        "${cache[@]}" "${extra[@]}" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    echo "  arm $n ($arm) finished."
}

# THE ENGAGEMENT GATE. Each arm names ONE line in its own log that can only be there if
# the variable took effect — not the variable itself, which is what part 69 got wrong.
engaged() {
    local arm="$1" f="$2"
    case "$arm" in
      base)
        # The fold reports how many draws it skipped. Zero means it never engaged.
        grep -aq "hook fold: [1-9][0-9]* of" "$f" && \
        ! grep -aq "CZ_VK_NO_HOOK_FOLD" "$f" ;;
      nofold)
        # Both halves: the arm announced itself AND nothing was folded away.
        grep -aq "CZ_VK_NO_HOOK_FOLD: the pre-part-71" "$f" && \
        grep -aq "hook fold: 0 of" "$f" ;;
      noclip)
        grep -aq "shader cache: .*shader_spv_a2m$" "$f" ;;
      nogamefov)
        # TWO-SIDED, because "the variable was read" and "the substitution stopped" are
        # different claims and only the second is the arm. `[fovgame] ... ACTIVE` is the
        # substitution's own first-fire announcement; its ABSENCE is the arm engaging.
        grep -aq "CZ_NO_GAME_FOV=1 — the guest-side fov substitution is OFF" "$f" && \
        ! grep -aq "game-side fov ACTIVE" "$f" ;;
      *) return 1 ;;
    esac
}

n=1
IFS=',' read -ra arms <<< "$ORDER"
for a in "${arms[@]}"; do run_arm "$a" "$n" "${#arms[@]}"; n=$((n + 1)); done

echo
echo "==================================================================="
echo "  ALL ARMS DONE — $HEAD"
fail=0
for a in "${arms[@]}"; do
    f=$(ls "$OUT"/p71_"$STAMP"_*_"$a".log 2>/dev/null | head -1)
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
    # A measurement has a MACHINE and a CONFIGURATION as well as a workload (gotcha 359).
    grep -a "^\[threads\]" "$f" | tail -2
    grep -a "internal resolution\|swapchain \|shader cache:" "$f" | tail -3
    grep -a "hook fold:\|const memo:\|flat cache grows:" "$f" | tail -3
    # The fov substitution's own line, in EVERY arm — it is the control for `nogamefov`
    # and a silent control is not one.
    grep -a "game-side fov ACTIVE\|CZ_NO_GAME_FOV=1" "$f" | tail -1
    grep -a "RT shadow variant cache\|rt shadows" "$f" | tail -1
    echo "  [fps] windows (the last 3 are the TURN block):"
    grep -a "^\[fps\]" "$f" | tail -24
done

echo
echo "  shaders the caches lacked: $(grep -ac 'no translated shader' "$OUT"/p71_"$STAMP"_*.log 2>/dev/null | paste -sd+ | bc 2>/dev/null || echo '?')"
echo
echo "  READ IT WITH — and always arm-against-its-own-control, never against a"
echo "  remembered number (gotcha 364):"
for a in "${arms[@]:1}"; do
    echo "    python3 tools/part54_fps_bins.py $OUT/p71_${STAMP}_1_${arms[0]}.log --arm $OUT/p71_${STAMP}_*_${a}.log"
done
echo
echo "  Match windows on 'draws med'; DISCARD any whose (min..max) spread is wide —"
echo "  that window straddled two places and its median is not a place at all."
echo "  The TAIL table is the turn-stutter answer; the frame-time bands are not."
exit $fail
