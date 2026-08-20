#!/bin/bash
# NIGHT RUN 1 — the unattended overnight A/B campaign of 2026-08-19. Plan and
# pre-registered claims: docs/perf-nightrun1.md. This is the HEADLESS sibling of
# tools/part55_chained_ab.sh: same one-variable-per-arm discipline, same CZ_FPS_LOG-only
# instrumentation, same held debug flags — but the soak is the DebugJump route standing
# still instead of an operator, because the operator is asleep and said to run it.
#
# Why it looks the way it does:
#   * ONE snapshotted binary for every arm (nothing rebuilt mid-campaign can be blamed).
#   * 3 runs per arm, alternated A,B,A,B,A,B — one run a side is a coin flip here
#     (gotcha 159); alternation spreads machine drift across both arms.
#   * N0 (null: identical config twice) runs FIRST and is the floor every other
#     experiment is read against. A campaign without a null arm is uninterpretable.
#   * GPU clock sampled through EVERY run: the monitor will be asleep, which is the
#     exact condition that once manufactured a P8/210 MHz reading (gotcha in
#     tools/gpu_clock_sample.py's header). The clock is QUOTED, never pinned.
#   * Engagement evidence per run (WAITJUMP fired, draws reached, budget line, memo %,
#     cache misses) — an arm with no counter cannot be shown to have engaged (gotcha 151).
#
# Results: ~/DR2CZ-troubleshooting/nightrun1-2026-08-19/  (disk — /tmp is a tmpfs).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/nightrun1-2026-08-19"
RUNSECS="${RUNSECS:-720}"          # per-run wall clock; load+menu eat 60-170 s of it
mkdir -p "$OUT"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/campaign.log"; }

# Refuse to start over a live game (the operator may have left one).
for p in $(pgrep -x cz_runtime 2>/dev/null; pgrep -x cz_night1 2>/dev/null); do
    log "!! cz_runtime pid $p is running; refusing to start. Nothing was launched."
    exit 2
done

BIN=cz_night1
cp -f "$ROOT/runtime/build/cz_runtime" "$ROOT/runtime/build/$BIN"
log "binary snapshot: $BIN <- cz_runtime @ $(cd "$ROOT" && git rev-parse --short HEAD)"

run_one() {
    # run_one <tag> [ENV=VAL ...]
    local tag="$1"; shift
    log "run $tag starting (${RUNSECS}s)"
    python3 "$ROOT/tools/gpu_clock_sample.py" --interval 5 --skip 90 \
        --duration "$RUNSECS" --csv "$OUT/$tag.gpu.csv" \
        > "$OUT/$tag.gpu.txt" 2>&1 &
    local gpid=$!
    ( cd "$ROOT/runtime/build" && env \
        CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FAKE_START_MS=8000 \
        CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE \
        CZ_FPS_LOG=10 CZ_FPS_CAP=500 CZ_VK_RES=1280x720 \
        "CZ_DEBUG_FLAGS=CHUCK GOD MODE,DISABLE DEATH SEQUENCE,ZOMBIES IGNORE ALL HUMANS" \
        CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
        "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
        "CZ_SHADER_SPV=$ROOT/assets/shader_spv_clip_a2m" \
        "$@" timeout "$RUNSECS" "./$BIN" > "$OUT/$tag.log" 2>&1 )
    wait "$gpid" 2>/dev/null
    # Engagement evidence, per run, or the arm cannot be shown to have run at all.
    {
        echo "=== $tag"
        grep -aE "WAITJUMP|requested DebugJump" "$OUT/$tag.log" | head -4
        grep -a "^\[threads\]" "$OUT/$tag.log" | tail -1
        grep -a "const memo" "$OUT/$tag.log" | tail -1
        echo "fps windows: $(grep -ac '^\[fps\]' "$OUT/$tag.log")"
        echo "peak draws med: $(grep -a '^\[fps\]' "$OUT/$tag.log" \
            | sed 's/.*draws med \([0-9]*\).*/\1/' | sort -n | tail -1)"
        echo "missing shaders: $(grep -ac 'no translated shader' "$OUT/$tag.log")"
        grep -a "GPU over" "$OUT/$tag.gpu.txt" | tail -1
    } >> "$OUT/checks.txt"
    log "run $tag done"
}

# ---- the four experiments; every arm is the shipped default plus AT MOST one variable.
# 3 pairs each, alternated. Control first within each pair.
for i in 1 2 3; do
    run_one "N0_${i}_nullA"
    run_one "N0_${i}_nullB"
done
for i in 1 2 3; do
    run_one "N1_${i}_stock" "CZ_SHADER_SPV=$ROOT/assets/shader_spv_a2m"
    run_one "N1_${i}_clip"
done
for i in 1 2 3; do
    run_one "N2_${i}_budget3"
    run_one "N2_${i}_guard4" CZ_VK_GUARD_WORKERS=4
done
for i in 1 2 3; do
    run_one "N3_${i}_memo"
    run_one "N3_${i}_nomemo" CZ_VK_NO_CONST_MEMO=1
done

# ---- pooled, draw-band-matched summary. Pooling is safe: the reader greps [fps] lines.
{
    echo "NIGHT RUN 1 — $(date)  (read docs/perf-nightrun1.md; N0 floor FIRST)"
    for exp in "N0 nullA nullB" "N1 stock clip" "N2 budget3 guard4" "N3 memo nomemo"; do
        set -- $exp
        cat "$OUT/${1}"_*_"${2}.log" > "$OUT/pool_${1}_${2}.log" 2>/dev/null
        cat "$OUT/${1}"_*_"${3}.log" > "$OUT/pool_${1}_${3}.log" 2>/dev/null
        echo; echo "=== $1: base=$2 arm=$3"
        python3 "$ROOT/tools/part54_fps_bins.py" \
            "$OUT/pool_${1}_${2}.log" --arm "$OUT/pool_${1}_${3}.log" 2>&1
    done
    echo; echo "=== per-run engagement (checks.txt)"; cat "$OUT/checks.txt"
} > "$OUT/SUMMARY.txt" 2>&1

log "CAMPAIGN DONE — summary at $OUT/SUMMARY.txt"
