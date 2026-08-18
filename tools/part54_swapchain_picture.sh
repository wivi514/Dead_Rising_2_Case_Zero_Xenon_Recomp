#!/bin/bash
# THE PICTURE GATE FOR CZ_VK_SWAPCHAIN — and it needed a new kind of oracle.
#
# WHY THE EXISTING PICTURE GATES CANNOT SEE THIS ARM. Every one of them reads the
# present READBACK: CZ_CAPTURE_KEY, CZ_VK_FRAME_DUMP, CZ_VK_FRAME_STATS and the E3
# correlation all walk the host copy of the frame. The swapchain arm's whole point is
# that the frame never becomes a host copy — it is blitted straight into a swapchain
# image — so those gates would pass with the readback path still doing all the work and
# say nothing whatever about the pixels on screen. An instrument that reports on a path
# the change does not touch is not a gate (gotcha 151).
#
# So the oracle here is the COMPOSITOR: a screen grab of what is actually on the display,
# which is the only thing in this pipeline neither our renderer nor our instruments
# produced (memory: an oracle must be something you did not write). It is compared
# against capture E3 — Xenia's own screenshot of this exact screen — with the same
# `frame_signature.py` the E3 gate uses, so a wrong format, a channel swap, a flip or a
# blank window all fail loudly and by name rather than as a lower number.
#
# BOTH ARMS RUN, and the readback arm is the control: it is the picture this port has
# been shipping, grabbed through the same compositor at the same screen, so the two
# numbers are directly comparable and neither depends on remembering what E3 "usually"
# scores (gotcha 51).
#
# THE SCREEN IS THE TITLE BACKDROP, chosen because it needs no navigation and because
# E3 is a screenshot of it. It is ANIMATED, so the two arms sample different moments and
# the correlation will not be identical between them — several grabs are taken and the
# BEST is reported, which is the same statistic and the same reasoning as the E3 gate's
# best-of-five (gotcha 133: one frame of an animated scene is one sample).
#
# Usage:  tools/part54_swapchain_picture.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$HOME/DR2CZ-troubleshooting/part54/picture}"
REF="$ROOT/Xenia logs/E_screenshots/E3_title_background_stillcreek.png"
GRABS="${GRABS:-5}"
SETTLE="${SETTLE:-45}"
mkdir -p "$OUT"
# `spectacle -b -n -f`, not `grim`: this is a KWin/Wayland session and KWin does not
# implement the wlr-screencopy protocol grim needs — it answers "compositor doesn't
# support the screen capture protocol" and writes nothing. Spectacle goes through KWin's
# own interface. On a wlroots compositor grim would be the one that works, so the check
# below names the tool it wants rather than assuming either.
GRAB="${GRAB:-spectacle}"
command -v "$GRAB" >/dev/null || { echo "!! $GRAB is not installed; this gate needs a screen grabber (spectacle on KDE, grim on wlroots)"; exit 2; }
[ -f "$REF" ] || { echo "!! reference missing: $REF"; exit 2; }

run_arm() {
    local tag="$1"; shift
    local extra=("$@")
    for p in $(pgrep -x cz_runtime 2>/dev/null); do kill "$p" 2>/dev/null; done
    sleep 1
    echo "=== $tag ${extra[*]:-（default）}"
    ( cd "$ROOT/runtime/build" && env CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
        CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=NONE,NONE,NONE,NONE,NONE,NONE,NONE \
        "${extra[@]}" timeout $((SETTLE + 8 * GRABS + 20)) ./cz_runtime \
        > "$OUT/$tag.log" 2>&1 ) &
    local runner=$!
    sleep "$SETTLE"
    for i in $(seq 1 "$GRABS"); do
        # The whole output, not a window rectangle: naming a window needs a toplevel
        # handle that not every compositor exposes, and frame_signature.py auto-crops to
        # the non-black bounding box anyway, which is what makes a full-screen grab
        # usable as a frame here. -b background, -n no notification, -f full screen.
        case "$GRAB" in
            spectacle) timeout 25 spectacle -b -n -f -o "$OUT/${tag}_$i.png" \
                           >/dev/null 2>&1 ;;
            *)         "$GRAB" "$OUT/${tag}_$i.png" >/dev/null 2>&1 ;;
        esac
        [ -s "$OUT/${tag}_$i.png" ] || echo "    grab $i FAILED"
        sleep 3
    done
    for p in $(pgrep -x cz_runtime 2>/dev/null); do kill "$p" 2>/dev/null; done
    wait $runner 2>/dev/null
}

score_arm() {
    local tag="$1"
    local best=0 bestshot="" agree=0 n=0
    for shot in "$OUT/${tag}"_*.png; do
        [ -f "$shot" ] || continue
        n=$((n + 1))
        python3 "$ROOT/tools/frame_signature.py" --ref "$REF" "$shot" \
            > "$shot.sig" 2>&1
        local c
        c=$(grep -oE 'identity +corr \+[0-9.]+' "$shot.sig" | grep -oE '[0-9.]+$' | head -1)
        [ -z "$c" ] && c=0
        grep -q "LAYOUT AGREES" "$shot.sig" && agree=$((agree + 1))
        awk -v a="$c" -v b="$best" 'BEGIN{exit !(a>b)}' && { best="$c"; bestshot="$shot"; }
    done
    echo "  $tag: best identity correlation +$best over $n grabs, $agree agreeing on layout"
    echo "         best grab: $bestshot"
}

# The swapchain is the DEFAULT since part 54, so the ARMS ARE INVERTED from how this
# script was first written: the plain run is the swapchain and the readback needs the
# control flag. Kept in this order so the output still reads readback-then-swapchain.
run_arm readback CZ_VK_NO_SWAPCHAIN=1
run_arm swapchain

echo
echo "=== E3 correlation through the COMPOSITOR (the oracle neither arm produced) ==="
score_arm readback
score_arm swapchain
echo
echo "  A swapchain number materially below the readback number, or one that stops"
echo "  saying LAYOUT AGREES, is a real defect in the blit — a channel swap, a flip, a"
echo "  crop or a blank window. Equal-within-the-animation is the pass."
