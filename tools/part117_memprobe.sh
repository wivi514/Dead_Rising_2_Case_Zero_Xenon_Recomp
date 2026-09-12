#!/bin/bash
# Part 117 step 0: WHAT KIND OF BOUND IS THE PUMP? Counters, not shares.
#
# WHY THIS EXISTS. Part 111 named the pump "bound by BYTES, machine-wide" (gotcha 551)
# from one controlled pair — pre-zeroing the shared block on another core recovered
# nothing — and predicted B2 (streams + textures on workers) dead from it. That pair has a
# second reading the plan never wrote down: the memset was a PREFETCH for the constants
# the pump writes into the same lines a moment later, so moving it to another core moved
# the misses back onto the pump as coherence misses. Under that reading the pump is
# LATENCY-bound on its own touches, not bandwidth-bound machine-wide, and the two
# predictions for B2 differ. The PMU can tell them apart without building anything:
#
#   IPC of the pump thread            <1 = stalled on memory; >2 = compute
#   demand fills from DRAM / L3 / L2  where the misses are served from
#   L1 DTLB reloads by page size      the 4 GB guest map runs on 4K pages
#   IBS op samples with data source   WHICH symbols miss, and to WHERE (guest memory,
#                                     our tables, the Vulkan arena)
#
# Same route and same crowd gate as tools/part116_probe.sh (which this copies rather
# than extends: the sampling section is the whole difference). `perf_event_paranoid`
# is 2 here, so every event is user-only (`:u`) and there are no kernel samples.
#
# Usage:
#   tools/part117_memprobe.sh <tag> [ENV=VAL ...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part117}"
TAG="${1:?usage: part117_memprobe.sh <tag> [ENV=VAL ...]}"; shift || true
STAT_SECS="${STAT_SECS:-10}"
IBS_SECS="${IBS_SECS:-10}"
IBS="${IBS:-1}"
DRAW_GATE="${DRAW_GATE:-8000}"
RES="${RES:-1920x1080}"
BIN_SRC="${BIN_SRC:-$ROOT/runtime/build/cz_runtime}"
SOAK="${SOAK:-120}"
mkdir -p "$OUT"

echo "=== $TAG  res $RES  soak ${SOAK}s  stat ${STAT_SECS}s + ibs ${IBS_SECS}s once draws >= $DRAW_GATE"
cp -f "$BIN_SRC" "$OUT/$TAG.bin"
sha256sum "$OUT/$TAG.bin" | cut -c1-16 > "$OUT/$TAG.bin.sha"

( OUT="$OUT" RES="$RES" SOAK="$SOAK" BIN_SRC="$BIN_SRC" "$ROOT/tools/part80_crowdroute.sh" "$TAG" \
      CZ_FPS_LOG=10 "$@" > "$OUT/$TAG.route.out" 2>&1 ) &
RUNNER=$!

PID=""
for _ in $(seq 1 90); do
    for p in $(pgrep cz_runtime 2>/dev/null); do
        c=$(cat "/proc/$p/comm" 2>/dev/null) || continue
        case "$c" in cz_runtime_crow*) PID=$p ;; esac
    done
    [ -n "$PID" ] && break
    sleep 1
done
[ -z "$PID" ] && { echo "!! no cz_runtime_crowd appeared"; wait $RUNNER; exit 3; }
LOG=""
for _ in $(seq 1 30); do LOG=$(ls -t "$OUT"/crowd_*_"$TAG".log 2>/dev/null | head -1); [ -n "$LOG" ] && break; sleep 1; done
echo "    pid=$PID  log=$LOG"

reached=0; prev=0; d=""
for _ in $(seq 1 200); do
    kill -0 "$PID" 2>/dev/null || break
    d=$(grep -a "^\[fps\]" "$LOG" 2>/dev/null | grep -aoE "draws med [0-9]+" | tail -1 | awk '{print $3+0}')
    if [ "${d:-0}" -ge "$DRAW_GATE" ] && [ "$prev" -ge "$DRAW_GATE" ] && [ "${d:-0}" != "$prev" ]; then
        reached=1; break
    fi
    [ "${d:-0}" != "$prev" ] && prev="${d:-0}"
    sleep 2
done
[ "$reached" != 1 ] && echo "    !! never reached $DRAW_GATE draws (last=${d:-none}); sampling anyway"
echo "    at the crowd $(date +%H:%M:%S), draws=${d:-?}"

# The pump's tid: several threads carry the comm `cz-pump` (children inherit it at
# clone), so take the one with the most CPU time, which is the pump itself.
PUMP=""; best=0
for t in /proc/$PID/task/*; do
    c=$(cat "$t/comm" 2>/dev/null) || continue
    [ "$c" = "cz-pump" ] || continue
    u=$(awk '{print $14+$15}' "$t/stat" 2>/dev/null || echo 0)
    if [ "${u:-0}" -gt "$best" ]; then best=$u; PUMP=$(basename "$t"); fi
done
echo "    pump tid=$PUMP"
for t in /proc/$PID/task/*; do printf '%s %s\n' "$(basename "$t")" "$(cat "$t/comm" 2>/dev/null)"; done \
    > "$OUT/$TAG.threads" 2>/dev/null

# 1. Counters on the pump thread. Two groups so the PMU's six counters are not
#    multiplexed inside a group whose ratios matter.
EV1="cycles:u,instructions:u,ls_dmnd_fills_from_sys.mem_io_local:u,ls_dmnd_fills_from_sys.ext_cache_local:u,ls_dmnd_fills_from_sys.lcl_l2:u,ls_dmnd_fills_from_sys.int_cache:u"
EV2="cycles:u,ls_l1_d_tlb_miss.all:u,ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss:u,ls_l1_d_tlb_miss.tlb_reload_2m_l2_miss:u,ls_l1_d_tlb_miss.tlb_reload_4k_l2_hit:u,ls_dispatch.ld_dispatch:u"
EV3="cycles:u,l1-dcache-loads:u,l1-dcache-load-misses:u,ls_hw_pf_dc_fills.mem_io_local:u,ls_any_fills_from_sys.mem_io_local:u,ls_mab_alloc.load_store_allocations:u"
perf stat -e "$EV1" -t "$PUMP" -- sleep "$STAT_SECS" > "$OUT/$TAG.stat1" 2>&1
perf stat -e "$EV2" -t "$PUMP" -- sleep "$STAT_SECS" > "$OUT/$TAG.stat2" 2>&1
perf stat -e "$EV3" -t "$PUMP" -- sleep "$STAT_SECS" > "$OUT/$TAG.stat3" 2>&1
# Also the Draw Thread and the Main Thread on the first group, for the same reading of
# the guest (their frame share is the other floor).
for name in "Main Thread" "Draw Thread"; do
    T=""; best=0
    for t in /proc/$PID/task/*; do
        c=$(cat "$t/comm" 2>/dev/null) || continue
        [ "$c" = "$name" ] || continue
        u=$(awk '{print $14+$15}' "$t/stat" 2>/dev/null || echo 0)
        if [ "${u:-0}" -gt "$best" ]; then best=$u; T=$(basename "$t"); fi
    done
    [ -n "$T" ] && perf stat -e "$EV1" -t "$T" -- sleep 5 > "$OUT/$TAG.stat1.$(echo "$name" | tr ' ' _)" 2>&1
done
cat "$OUT/$TAG.stat1" "$OUT/$TAG.stat2" "$OUT/$TAG.stat3" | grep -vE "^$|^#"

# 2. Flat cycles on the pump, with a symfs, for the symbol table this binary has now.
perf record -F 999 -t "$PUMP" -o "$OUT/$TAG.perf.data" -- sleep 10 > "$OUT/$TAG.perf.log" 2>&1
SYMFS="$OUT/$TAG.symfs"
mkdir -p "$SYMFS$(dirname "$ROOT/runtime/build/cz_runtime_crowd")" "$SYMFS/usr"
ln -sfn "$OUT/$TAG.bin" "$SYMFS$ROOT/runtime/build/cz_runtime_crowd"
ln -sfn /usr/lib64 "$SYMFS/usr/lib64"

# 3. IBS op sampling on the pump: every sample carries the data source and the address
#    of a load/store, which is what says WHERE the misses go. `swfilt` keeps user-only
#    under paranoid 2; if the PMU refuses, the log says so and the rest stands.
if [ "$IBS" = 1 ]; then
    perf record -e 'ibs_op/swfilt=1/u' -c 200000 -d -t "$PUMP" -o "$OUT/$TAG.ibs.perf.data" \
        -- sleep "$IBS_SECS" > "$OUT/$TAG.ibs.log" 2>&1 || echo "    ibs: refused (see $TAG.ibs.log)"
    ln -sfn "$OUT/$TAG.symfs" "$OUT/$TAG.ibs.symfs"
fi

wait $RUNNER
grep -a "^\[fps\]" "$LOG" | tail -4
echo "artifacts in $OUT ($TAG.*)"
