#!/bin/bash
# PART 72's ITEM-C SESSION — is the constant gather CORRECT, and what is it worth?
# Four arms, one sitting, the operator drives, quitting one arm starts the next.
#
# WHY THIS EXISTS. Part 72 shipped perf item C: the renderer now copies only the ALU
# constant registers each shader's sidecar says it READS, instead of the whole 256-float4
# window per stage per draw. Median 26 of 256 over the 449 modules, maximum 56 — 896 bytes
# against 4,096.
#
# THE CORRECTNESS ARMS COME FIRST AND THEY ARE 40 SECONDS EACH, on purpose. A register
# missing from a list is never copied, so the shader reads whatever the bump arena left in
# that slot — garbage, not a stale value, and the symptom is one shader subtly wrong with
# everything else correct. That is the hardest defect class in this renderer to see, and
# there is no reason to spend six minutes of soak before finding out whether the thing is
# even right. If arm 2 fails, stop: nothing measured afterwards means anything.
#
#   verify    CZ_VK_VERIFY_CONST_GATHER=1 + CZ_VK_ORDER_GATE=1. Both verifiers armed,
#             both must read ZERO. ~40 s anywhere outdoors.
#   poison    + CZ_VK_GATHER_POISON=1 + CZ_VK_ORDER_POISON=0. TWO POSITIVE CONTROLS in one
#             run: each verifier must now report a NON-ZERO count. A zero here means that
#             verifier is blind and its zero in arm 1 meant nothing (gotcha 30).
#             **THE PICTURE MAY BE WRONG IN THIS ARM** — the gather poison really does drop
#             a register. That is the point. The order poison does NOT touch rendering; it
#             transposes a local copy used only for the hash.
#   gather    the shipped default. THE A/B's treatment arm. ~3 min soak, heavy spot.
#   nogather  (no variable) — the whole window copied; the pre-part-72 renderer AND the part-74 default.
#             THE A/B's control arm. ~3 min soak, SAME spot.
#
# WHAT TO READ. The two soak arms carry `CZ_FPS_LOG` and nothing else, so their medians are
# comparable at a matched draw band. The two short arms' frame times are worthless by
# construction (the verifier does BOTH copies and compares) and the harness does not print
# them.
#
# EVERY ARM PROVES IT ENGAGED OR THE HARNESS REFUSES TO REPORT IT (gotcha 408), from a line
# the FEATURE prints. `SELFTEST=1 tools/part72_itemc_session.sh` runs the gate cases —
# clean arms and deliberate breakages — and exits non-zero if any gate accepts a log it
# should refuse.
#
# Usage:  tools/part72_itemc_session.sh
#         ORDER=verify,poison tools/part72_itemc_session.sh    # correctness only
#         SECS=120 tools/part72_itemc_session.sh
#
# **PART 74 FLIPPED THE DEFAULT.** The gather shipped ON in part 72; the operator's sky
# flicker was discriminated against it in part 74 (gather ON flickered twice including a
# deliberate positive control, gather OFF was clean), so the gather is now OFF by default
# and `CZ_VK_CONST_GATHER=1` turns it ON. Every arm below is rewritten accordingly: the
# "gather off" arm now sets NOTHING, and the "gather on" arms set CZ_VK_CONST_GATHER=1.
#
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part72-itemc"
ORDER="${ORDER:-verify,poison,gather,nogather}"
SECS="${SECS:-180}"
FPS="${FPS:-500}"
FLAGS="${FLAGS:-CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS}"
mkdir -p "$OUT"

busy=""
for p in $([ -n "${SELFTEST:-}" ] || pgrep -x cz_runtime 2>/dev/null) \
         $([ -n "${SELFTEST:-}" ] || pgrep -x cz_runtime_p72c 2>/dev/null); do
    busy="$busy  $p"$'\n'
done
if [ -n "$busy" ]; then
    echo "!! a cz_runtime is already running; refusing to start a second:"; printf '%s' "$busy"; exit 2
fi

BIN=cz_runtime_p72c
HEAD="$(cd "$ROOT" && git rev-parse --short HEAD)"
if [ -z "${SELFTEST:-}" ]; then
    cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
    echo "snapshot: $BIN <- cz_runtime ($HEAD)"
fi
STAMP="$(date +%m%d_%H%M)"

# ---- PREFLIGHT ----------------------------------------------------------------------
# THE LISTS' OWN GATE FIRST. The gather trusts a per-shader register list, and no run-time
# check can catch an omission — the missing register is by definition the one nobody reads.
# This is the only check that can, it costs a second, and spending operator time on a
# cache whose lists are wrong would measure a defect and call it a saving.
if [ -z "${SELFTEST:-}" ]; then
    if ! python3 "$ROOT/tools/alu_const_gate.py" --dir "$ROOT/assets/shader_spv_clip_a2m"; then
        echo "!! the play cache's ALU constant lists FAIL their gate. Fix that first."
        exit 3
    fi
    if ! diff -q <(cd "$ROOT/assets/shader_spv_clip_a2m" && ls *.spv) \
                 <(cd "$ROOT/assets/shader_spv" && ls *.spv) >/dev/null; then
        echo "!! the play cache and the stock cache hold DIFFERENT shader sets (gotcha 390)."
        exit 3
    fi
    echo "preflight: play cache name set matches stock"
    [ -f "$ROOT/assets/save/cz_settings.txt" ] && echo "preflight: cz_settings.txt ->" \
        "$(grep -av '^#' "$ROOT/assets/save/cz_settings.txt" | tr '\n' ' ')"
fi

arm_desc() {
    case "$1" in
      verify)   echo "CZ_VK_VERIFY_CONST_GATHER=1 + CZ_VK_ORDER_GATE=1 — both must read ZERO" ;;
      poison)   echo "+ both POISONS — both verifiers must now read NON-ZERO" ;;
      gather)   echo "the shipped default — the A/B's TREATMENT arm" ;;
      nogather) echo "gather OFF — the pre-part-72 full copy, the CONTROL arm and the part-74 default" ;;
      *)        echo "UNKNOWN ARM" ;;
    esac
}
arm_task() {
    case "$1" in
      verify|poison)
        echo ">>> Any outdoor spot with a crowd. Stand ~40 s. Quit."
        [ "$1" = poison ] && echo "    >>> THE PICTURE MAY LOOK WRONG IN THIS ARM. That is the control working." ;;
      *)
        echo ">>> Go to THE SAME heaviest spot in BOTH soak arms."
        echo "    >>> Stand still ~$((SECS / 60)) min. Do not walk. Quit." ;;
    esac
}

run_arm() {
    local arm="$1" n="$2" total="$3"
    local tag="p72c_${STAMP}_${n}_${arm}"
    local extra=()
    case "$arm" in
      # Every arm that EXERCISES the gather must turn it on explicitly since part 74 —
      # it is off by default. `nogather` is now the default and sets nothing.
      verify)   extra+=(CZ_VK_CONST_GATHER=1 CZ_VK_VERIFY_CONST_GATHER=1
                        CZ_VK_ORDER_GATE=1) ;;
      poison)   extra+=(CZ_VK_CONST_GATHER=1 CZ_VK_VERIFY_CONST_GATHER=1
                        CZ_VK_GATHER_POISON=1 CZ_VK_ORDER_GATE=1 CZ_VK_ORDER_POISON=0) ;;
      gather)   extra+=(CZ_VK_CONST_GATHER=1) ;;
      nogather) ;;
      *) echo "!! unknown arm '$arm'"; return 1 ;;
    esac
    cat <<BANNER

===================================================================
  ARM $n of $total:  $arm
  $(arm_desc "$arm")

  log: $OUT/$tag.log

  $(arm_task "$arm")
===================================================================

BANNER
    ( cd "$ROOT/runtime/build" && env \
        CZ_VKDRAW=1 "CZ_FPS_CAP=$FPS" CZ_FPS_LOG=10 \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        CZ_DEBUG_MENU=1 "CZ_DEBUG_FLAGS=$FLAGS" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
        "${extra[@]}" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    echo "  arm $n ($arm) finished."
}

# THE ENGAGEMENT GATES. Two-sided everywhere it matters: "the verifier ran" is not "the
# verifier found nothing", and for the poison arm a zero is a FAILURE and not a pass.
engaged() {
    local arm="$1" f="$2"
    case "$arm" in
      verify)
        # The gather must be RUNNING (not the full-copy fallback for everything), the
        # verifier must have CHECKED something, and both counts must be zero.
        grep -aq "const gather: [1-9]" "$f" &&
        ! grep -aq "constant gather OFF" "$f" &&
        grep -aq "verified: [1-9][0-9]* gathers checked" "$f" &&
        grep -aq "\*\*0 disagreed\*\*" "$f" &&
        grep -aq "draw-order gate: [1-9][0-9]* frames checked, \*\*0 FAILED\*\*" "$f" ;;
      poison)
        # BOTH controls must FIRE. A zero here means that verifier is blind and arm 1's
        # zero meant nothing — which is the only outcome of this session that would
        # invalidate the other three arms.
        grep -aq "CZ_VK_GATHER_POISON=1" "$f" &&
        grep -aq "verified: [1-9][0-9]* gathers checked" "$f" &&
        ! grep -aq "\*\*0 disagreed\*\*" "$f" &&
        grep -aq "CZ_VK_ORDER_POISON=0" "$f" &&
        ! grep -aq "draw-order gate: [0-9]* frames checked, \*\*0 FAILED\*\*" "$f" ;;
      gather)
        grep -aq "const gather: [1-9]" "$f" && ! grep -aq "constant gather OFF" "$f" ;;
      nogather)
        # Two-sided: the arm announced itself AND nothing was gathered.
        grep -aq "constant gather OFF (the default since part 74" "$f" &&
        grep -aq "const gather: 0.0% of window copies gathered" "$f" ;;
      *) return 1 ;;
    esac
}

# THE GATES' OWN GATE (gotcha 30). Cases live here so they cannot drift from what they test.
if [ -n "${SELFTEST:-}" ]; then
    d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
    G='[vk]   const gather: 97.8% of window copies gathered (1200 full — dynamic a0 or no list), 3.10 GB not copied over the run (78.1% of 3.97 GB), 44 memo top-ups'
    G0='[vk]   const gather: 0.0% of window copies gathered (54000 full — dynamic a0 or no list), 0.00 GB not copied over the run (0.0% of 0.88 GB), 0 memo top-ups'
    NOG='[vk] constant gather OFF (the default since part 74 is copied per stage per draw (the pre-part-72 behaviour)'
    VOK='[vk]     verified: 51234 gathers checked against the full copy, **0 disagreed**'
    VBAD='[vk]     verified: 51234 gathers checked against the full copy, **4211 disagreed**  (POISONED — a zero here means the verifier is BLIND)'
    GP='[vk] CZ_VK_GATHER_POISON=1 — one register is dropped from every gather.'
    OOK='[order]   draw-order gate: 900 frames checked, **0 FAILED**, 8100000 draws logged  (serial recording: zero is the only correct result)'
    OBAD='[order]   draw-order gate: 900 frames checked, **900 FAILED**, 8100000 draws logged  (POISONED — a zero here means the gate is BLIND)'
    OP='[order] CZ_VK_ORDER_POISON=0 — transposing one adjacent pair per frame.'
    mk() { local f="$d/$1"; shift; printf '%s\n' "$@" > "$f"; }
    mk g_verify   "$G" "$VOK" "$OOK"
    mk g_poison   "$G" "$GP" "$VBAD" "$OP" "$OBAD"
    mk g_gather   "$G"
    mk g_nogather "$NOG" "$G0"
    mk b_verify_nogather "$NOG" "$G0" "$VOK" "$OOK"
    mk b_verify_bad      "$G" "$VBAD" "$OOK"
    mk b_verify_orderbad "$G" "$VOK" "$OBAD"
    mk b_verify_nocheck  "$G" "$OOK"
    mk b_poison_blind    "$G" "$GP" "$VOK" "$OP" "$OBAD"
    mk b_poison_orderblind "$G" "$GP" "$VBAD" "$OP" "$OOK"
    mk b_nogather_still   "$NOG" "$G"
    pass=0; bad=0
    t() { engaged "$2" "$d/$3"; local got=$?
          if [ "$got" = "$4" ]; then echo "  ok    $1"; pass=$((pass+1))
          else echo "  FAIL  $1 (got $got want $4)"; bad=$((bad+1)); fi; }
    echo "SHOULD ENGAGE:"
    t "verify   clean"                  verify   g_verify   0
    t "poison   clean (both fired)"     poison   g_poison   0
    t "gather   clean"                  gather   g_gather   0
    t "nogather clean"                  nogather g_nogather 0
    echo "SHOULD REFUSE (deliberate breakages):"
    t "verify   ran with the gather OFF"      verify   b_verify_nogather 1
    t "verify   found disagreements"          verify   b_verify_bad      1
    t "verify   order gate FAILED frames"     verify   b_verify_orderbad 1
    t "verify   verifier never checked"       verify   b_verify_nocheck  1
    t "poison   gather verifier BLIND"        poison   b_poison_blind    1
    t "poison   order gate BLIND"             poison   b_poison_orderblind 1
    t "nogather still gathered"               nogather b_nogather_still  1
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
    f=$(ls "$OUT"/p72c_"$STAMP"_*_"$a".log 2>/dev/null | head -1)
    echo
    echo "--- $a  ($(basename "${f:-MISSING}"))"
    if [ -z "$f" ] || ! engaged "$a" "$f"; then
        echo "  ** NOT ENGAGED / GATE FAILED — this arm's numbers are NOT reportable."
        fail=1
        [ -n "$f" ] || continue
    else
        echo "  ENGAGED."
    fi
    grep -a "const gather:" "$f" | tail -1 | sed 's/^/    /'
    grep -a "verified: .* gathers checked" "$f" | tail -1 | sed 's/^/    /'
    grep -a "draw-order gate:" "$f" | tail -1 | sed 's/^/    /'
    grep -a "const memo:" "$f" | tail -1 | sed 's/^/    /'
    grep -a "CONST MEMO STALE" "$f" | tail -1 | sed 's/^/    /'
    case "$a" in
      gather|nogather)
        echo "  [fps] windows (the A/B; match on 'draws med'):"
        grep -a "^\[fps\]" "$f" | tail -4 | sed 's/^/    /' ;;
      *) echo "  (frame times not shown: the verifier does BOTH copies, so they mean nothing)" ;;
    esac
done
cat <<'READ'

  HOW TO READ IT
  --------------
  1. ARM 2 FIRST. If either poison did NOT fire, that verifier is blind and arm 1's zero
     meant nothing — stop and fix the verifier before believing anything else here.
  2. THEN ARM 1. `**0 disagreed**` over a real number of checks, and the order gate at
     `**0 FAILED**`, is what says item C is correct and the serial path preserves order.
  3. THEN THE A/B: `gather` against `nogather` at a MATCHED draw band, medians not means,
     and print each arm's within-band draw median beside its frame time (gotcha 417).
     The `const gather:` line gives the bytes saved independently of any frame time.
  4. Watch `CONST MEMO STALE`. It must stay absent. The gather changed what that verifier
     compares, so its silence here is also a check on that change.
READ
exit $fail
