#!/bin/bash
# THE GATE FOR PART 76 ITEM 1 — the edge-triggered present readback.
#
# WHY THIS EXISTS. Part 76 item 1 stops the present readback from running on every frame
# just because `CZ_CAPTURE_KEY` or `CZ_BURST_DUMP` is NAMED in the environment; the key
# press now arms it for the frames it needs. That is a saving of ~3.5 ms of the operator's
# 23.31 ms crowd frame, and it is invisible: **nothing on screen changes either way**, so
# a fix that quietly broke F9 and F8 would look exactly like a fix that worked. The only
# thing that can tell them apart is whether the ARTIFACTS still appear.
#
# It also closes the trap the readback predicate names: gating a fast path on "is an
# instrument armed" ships a default path no gate exercises, because every picture gate in
# this project sets one of those variables. So arm 1 here runs with **no continuous picture
# instrument at all** — exactly the configuration `tools/play_session.sh` uses — and checks
# the COUNTERS and the F9/F8 artifacts rather than a frame dump.
#
# THE ARMS, and what each one would catch:
#
#   A  default        F9 + F8 pressed, no continuous instrument. The shipping path.
#                     Must produce a capture PPM and a burst, and its counters must show
#                     the readback SKIPPED on the great majority of frames.
#   B  present-always CZ_VK_PRESENT_ALWAYS=1 — the same-binary control, i.e. the
#                     pre-part-76 behaviour. Same artifacts, readback on EVERY frame.
#                     If A and B differ in what they produce, A is broken.
#
# A note on the burst: the first frame or two of every burst in the swapchain arm have no
# readback pixels yet, because F8 is consumed at the BOTTOM of a swap whose readback
# decision was made at the top. That is expected and the runtime counts and reports it;
# this gate fails if it exceeds 3, which is what "the F8 arm never reached the predicate"
# would look like.
#
# Usage:  tools/part76_readback_gate.sh [RES=3440x1440]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part76-gate}"
RES="${RES:-3440x1440}"
rm -rf "$OUT"; mkdir -p "$OUT"

fail=0
note() { echo "     $*"; }
bad()  { echo "  ** $*"; fail=1; }

run_arm() {
    local tag="$1"; shift
    local dir="$OUT/$tag"
    mkdir -p "$dir"
    echo "=== arm $tag"
    # F9 and F8 are pressed AFTER arrival, from inside the route's own press sequence —
    # both are synthesizable (kernel/imports.cpp's named-button table) precisely so that
    # checking them is not an operator errand (gotcha 190).
    # F9, then a pause, then F8 — appended to the route's own sequence via POSTSEQ so the
    # journey is unchanged and only the presses are added. The NONEs after each press give
    # the armed frames time to be read back and written.
    OUT="$dir" SECS=40 PRESSMS=3000 TIMEOUT=200 POSTSEQ="F9,NONE,NONE,F8,NONE,NONE,NONE" \
        "$ROOT/tools/autoroute.sh" "$tag" \
        "CZ_VK_RES=$RES" \
        "CZ_CAPTURE_KEY=$dir" "CZ_BURST_DUMP=$dir" \
        CZ_BURST_DUMP_MS=700 CZ_BURST_DUMP_MAX=40 \
        "$@" \
        > "$dir/gate.txt" 2>&1
    local rc=$?
    cat "$dir/gate.txt"
    local log; log="$(ls -t "$dir"/auto_*.log 2>/dev/null | head -1)"
    [ -n "$log" ] || { bad "$tag: no log"; return; }
    if [ $rc -ne 0 ]; then bad "$tag: the route gate failed (rc=$rc) — NOT REPORTABLE"; return; fi

    # 1. the F9 capture picture. Its EXISTENCE is the claim; a zero-byte or all-black one
    #    would mean the readback was armed for the wrong frame, so the bytes are checked.
    local ppm; ppm="$(ls "$dir"/capture_*.ppm 2>/dev/null | head -1)"
    if [ -z "$ppm" ]; then bad "$tag: F9 wrote NO capture picture"
    else
        local lit; lit="$(python3 - "$ppm" <<'PY'
import sys
f=open(sys.argv[1],'rb'); assert f.readline().strip()==b'P6'
w,h=map(int,f.readline().split()); f.readline()
d=f.read(); n=sum(1 for i in range(0,len(d)-3,3*64) if d[i] or d[i+1] or d[i+2])
print(f"{w}x{h} {100.0*n/max(1,len(d)//(3*64)):.1f}")
PY
)"
        note "F9 picture: $(basename "$ppm")  ${lit}% lit"
        case "$lit" in *" 0.0") bad "$tag: the F9 picture is entirely black";; esac
    fi
    # 2. the F9 per-draw census, the other half of one press.
    local cens; cens="$(ls "$dir"/capture_f*.census 2>/dev/null | head -1)"
    if [ -n "$cens" ] && [ -s "$cens" ]; then
        note "F9 census: $(basename "$cens") $(grep -ac . "$cens") lines"
    else bad "$tag: F9 wrote no per-draw census"; fi
    # 3. the F8 burst.
    local nb; nb="$(ls "$dir"/burst*_*.ppm 2>/dev/null | wc -l)"
    note "F8 burst: $nb frames"
    [ "$nb" -ge 5 ] || bad "$tag: the F8 burst wrote only $nb frames (expected >= 5)"
    # THE STALE-SLOT CHECK — and it is a check on an INVARIANT, not a content canary.
    #
    # The first version of this compared every burst PPM against the F9 capture: a stale
    # slot would serve pixels from the F9 window, nine seconds and a camera sweep earlier.
    # It was built, run against a DELIBERATELY BROKEN BUILD, and PASSED — because the F9
    # press arms `framesInFlight + 2` frames and the capture PPM is only one of them, so
    # the stale read landed on a sibling nobody had a copy of. A canary aimed at one
    # artifact cannot cover a window, and the positive control is what said so (gotcha 30).
    #
    # The runtime now stamps each frame slot with the frame whose pixels are in it and
    # compares that against the frame the slot describes, on every present. A disagreement
    # is a defect by construction — no route, no camera, no second artifact.
    local stale; stale="$(grep -ac "present slot describes frame" "$log")"
    note "stale present slots: $stale"
    [ "$stale" -eq 0 ] || bad "$tag: $stale present(s) served pixels from the wrong frame"
    local lost; lost="$(grep -ao "+[0-9]* frame(s) at the start with no readback" "$log" | grep -o '[0-9]*' | head -1)"
    lost="${lost:-0}"
    note "F8 burst frames lost at the start: $lost"
    [ "$lost" -le 3 ] || bad "$tag: $lost burst frames had no pixels — the F8 readback arm is not engaging"
    # 4. THE COUNTERS — the only thing that says which path actually ran.
    grep -a "^\[vk\]   readback: " "$log" | sed 's/^/     /'
    local skipped ran_edge ran_cont
    skipped="$(grep -a "readback: skipped" "$log" | awk '{print $NF}' | tail -1)"; skipped="${skipped:-0}"
    ran_edge="$(grep -a "readback: ran because F8/F9" "$log" | awk '{print $NF}' | tail -1)"; ran_edge="${ran_edge:-0}"
    ran_cont="$(grep -a "readback: ran for a continuous" "$log" | awk '{print $NF}' | tail -1)"; ran_cont="${ran_cont:-0}"
    echo "$tag skipped=$skipped edge=$ran_edge cont=$ran_cont" >> "$OUT/summary.txt"
    case "$tag" in
      default)
        [ "$ran_cont" -eq 0 ] || bad "default: $ran_cont frames took the CONTINUOUS path — the split is not in force"
        [ "$ran_edge" -gt 0 ] || bad "default: the F8/F9 arm NEVER engaged (0 frames) — the presses did not reach the predicate"
        [ "$skipped" -gt "$ran_edge" ] || bad "default: only $skipped of frames skipped the readback against $ran_edge armed — the saving is not being taken"
        note "default: $skipped frames skipped, $ran_edge armed by a key press" ;;
      always)
        [ "$skipped" -eq 0 ] || bad "always: $skipped frames skipped the readback — CZ_VK_PRESENT_ALWAYS=1 did not force it"
        [ "$ran_cont" -gt 0 ] || bad "always: the control arm never took the continuous path"
        note "always: $ran_cont frames on the every-frame path, 0 skipped" ;;
    esac
}

# ARMS lets the positive control run just the arm that can see the defect (gotcha 30 —
# a gate that has never failed has not been shown capable of failing; the stale-pixel
# canary was proved by reverting `pres.hasPixels` to `doReadback` and re-running this).
ARMS="${ARMS:-default always}"
for a in $ARMS; do
    case "$a" in
        default) run_arm default ;;
        always)  run_arm always CZ_VK_PRESENT_ALWAYS=1 ;;
        *) echo "!! unknown arm $a"; exit 2 ;;
    esac
done

echo
echo "=== summary"
cat "$OUT/summary.txt" 2>/dev/null
# The two arms must produce the SAME artifacts. That is the whole correctness claim: the
# saving is invisible, so "did it still work" can only be answered by comparing against the
# configuration that always worked.
a="$(ls "$OUT"/default/capture_f*.census 2>/dev/null | head -1)"
b="$(ls "$OUT"/always/capture_f*.census 2>/dev/null | head -1)"
if [ -s "${a:-/nonexistent}" ] && [ -s "${b:-/nonexistent}" ]; then
    echo "  F9 census lines: default $(grep -ac . "$a"), always $(grep -ac . "$b")"
fi
if [ "$fail" -eq 0 ]; then echo "  GATE OK"; else echo "  ** GATE FAILED"; fi
exit "$fail"
