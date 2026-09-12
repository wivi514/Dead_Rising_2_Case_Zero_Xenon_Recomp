#!/bin/bash
# Part 118 step 0: WHAT IS THE GUEST'S MAIN THREAD BOUND BY, AND WHAT DO WE COST IT?
#
# WHY THIS EXISTS. Part 116 measured the Main Thread at 7.9-8.1 ms CPU with the renderer
# and 6.4-6.7 ms under CZ_VK_NO_DODRAW=1 — so OUR renderer's presence costs the title's
# own thread ~1.5 ms of CPU a frame, the largest single term on the board, and nobody has
# said WHY. Part 117's PMU pass on the same thread read IPC 1.33 with ~29,000 demand fills
# from DRAM a frame, and the hottest instructions in the hottest function are all the
# consumer of a guest-memory load: the thread is pointer-chasing the title's heap and the
# misses are serialised. The candidate mechanisms for the +1.5 differ in what they do to
# the counters, and the PMU separates them without building anything:
#
#   L3 eviction by our threads' traffic   -> mem_io_local fills per frame UP with the renderer
#   SMT / core contention                  -> cycles per instruction UP, fills the same
#   DRAM bandwidth contention              -> fills the same, latency (cycles) up — needs IBS
#
# Two arms on the same binary: normal, and CZ_VK_NO_DODRAW=1 (the guest floor). Same route
# and crowd gate as tools/part117_memprobe.sh; the thread sampled is the difference.
#
# Usage:
#   tools/part118_guestprobe.sh <tag> [ENV=VAL ...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part118}"
TAG="${1:?usage: part118_guestprobe.sh <tag> [ENV=VAL ...]}"; shift || true
STAT_SECS="${STAT_SECS:-10}"
DRAW_GATE="${DRAW_GATE:-8000}"
RES="${RES:-1920x1080}"
BIN_SRC="${BIN_SRC:-$ROOT/runtime/build/cz_runtime}"
SOAK="${SOAK:-120}"
IBS="${IBS:-0}"
mkdir -p "$OUT"

echo "=== $TAG  res $RES  soak ${SOAK}s  stat ${STAT_SECS}s x3 once draws >= $DRAW_GATE"
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

# The guest's Main Thread: several host threads carry that comm (children inherit it at
# clone), so take the one with the most CPU time.
tid_of() {
    local name="$1" T="" best=0
    for t in /proc/$PID/task/*; do
        c=$(cat "$t/comm" 2>/dev/null) || continue
        [ "$c" = "$name" ] || continue
        u=$(awk '{print $14+$15}' "$t/stat" 2>/dev/null || echo 0)
        if [ "${u:-0}" -gt "$best" ]; then best=$u; T=$(basename "$t"); fi
    done
    echo "$T"
}
MAIN=$(tid_of "Main Thread"); DRAW=$(tid_of "Draw Thread")
echo "    main tid=$MAIN  draw tid=$DRAW"
for t in /proc/$PID/task/*; do printf '%s %s\n' "$(basename "$t")" "$(cat "$t/comm" 2>/dev/null)"; done \
    > "$OUT/$TAG.threads" 2>/dev/null

EV1="cycles:u,instructions:u,ls_dmnd_fills_from_sys.mem_io_local:u,ls_dmnd_fills_from_sys.ext_cache_local:u,ls_dmnd_fills_from_sys.lcl_l2:u,ls_dmnd_fills_from_sys.int_cache:u"
EV2="cycles:u,ls_l1_d_tlb_miss.all:u,ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss:u,ls_l1_d_tlb_miss.tlb_reload_2m_l2_miss:u,ls_l1_d_tlb_miss.tlb_reload_4k_l2_hit:u,ls_dispatch.ld_dispatch:u"
EV3="cycles:u,l1-dcache-loads:u,l1-dcache-load-misses:u,ls_hw_pf_dc_fills.mem_io_local:u,ls_any_fills_from_sys.mem_io_local:u,ls_mab_alloc.load_store_allocations:u"
# Which CPU the thread sits on, sampled through the stat window: the SMT question.
( for _ in $(seq 1 $((STAT_SECS * 3 * 4))); do
      # `processor` is field 39, but the comm can carry a space ("Main Thread") and shift
      # every field after it: cut at the last ')' and count from there.
      m=$(sed 's/.*) //' /proc/$PID/task/$MAIN/stat 2>/dev/null | awk '{print $37}')
      dr=$(sed 's/.*) //' /proc/$PID/task/$DRAW/stat 2>/dev/null | awk '{print $37}')
      others=""
      for t in /proc/$PID/task/*; do
          c=$(cat "$t/comm" 2>/dev/null) || continue
          case "$c" in cz-pump|cz-draw|cz-guard*) 
              u=$(awk '{print $14+$15}' "$t/stat" 2>/dev/null || echo 0)
              [ "${u:-0}" -gt 100 ] && others="$others $c@$(sed 's/.*) //' "$t/stat" 2>/dev/null | awk '{print $37}')";;
          esac
      done
      echo "main@$m draw@$dr$others"
      sleep 0.25
  done ) > "$OUT/$TAG.cpus" 2>/dev/null &
CPUS=$!
perf stat -e "$EV1" -t "$MAIN" -- sleep "$STAT_SECS" > "$OUT/$TAG.main.stat1" 2>&1
perf stat -e "$EV2" -t "$MAIN" -- sleep "$STAT_SECS" > "$OUT/$TAG.main.stat2" 2>&1
perf stat -e "$EV3" -t "$MAIN" -- sleep "$STAT_SECS" > "$OUT/$TAG.main.stat3" 2>&1
[ -n "$DRAW" ] && perf stat -e "$EV1" -t "$DRAW" -- sleep 5 > "$OUT/$TAG.draw.stat1" 2>&1
wait $CPUS 2>/dev/null
cat "$OUT/$TAG.main.stat1" "$OUT/$TAG.main.stat2" "$OUT/$TAG.main.stat3" | grep -vE "^$|^#"

if [ "$IBS" = 1 ]; then
    perf record -e 'ibs_op/swfilt=1/u' -c 100000 -d -t "$MAIN" -o "$OUT/$TAG.ibs.perf.data" \
        -- sleep 10 > "$OUT/$TAG.ibs.log" 2>&1 || echo "    ibs: refused (see $TAG.ibs.log)"
    SYMFS="$OUT/$TAG.symfs"
    mkdir -p "$SYMFS$(dirname "$ROOT/runtime/build/cz_runtime_crowd")" "$SYMFS/usr"
    ln -sfn "$OUT/$TAG.bin" "$SYMFS$ROOT/runtime/build/cz_runtime_crowd"
    ln -sfn /usr/lib64 "$SYMFS/usr/lib64"
fi

wait $RUNNER
grep -a "^\[fps\]" "$LOG" | tail -3
grep -a "^\[guestwait\]" "$LOG" | tail -1
echo "artifacts in $OUT ($TAG.*)"
