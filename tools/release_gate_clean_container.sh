#!/usr/bin/env bash
# A.4's GATE: prove the BUNDLE is what loads, on a machine that has none of the
# development packages (docs/release-plan.md A.4).
#
# WHY IT NEEDS A CONTAINER. On the build machine every one of these libraries is also
# in /lib64, so `ldd` will happily report a resolved, working binary whichever copy it
# picked. That is the classic packaging defect: it passes every test done where it was
# built, and fails on the first machine that is not that one. The only honest test is a
# filesystem where the system copies do not exist.
#
# This uses podman with fedora-minimal, which is enough to answer the question — no
# SDL2, no ffmpeg, no Vulkan headers, no clang. It is NOT a claim about every
# distribution; it is the claim that the bundle satisfies itself.
#
# What is checked, in order:
#   1. Every non-system library resolves INSIDE the bundle (the point of the exercise).
#   2. Nothing resolves to a path outside it except the permitted list: the Vulkan
#      loader, and libc/libm/ld.so, which are deliberately the host's.
#   3. `cz_runtime --smoke` runs — the phase 0.2 link gate, in the packaged binary.
#   4. The first-run refusal fires with the right message, since a fresh container has
#      no game and that is exactly the state a player is in.
#
# Usage:  tools/release_gate_clean_container.sh [stageDir] [image]
set -uo pipefail

STAGE=${1:-$(cd "$(dirname "$0")/.." && pwd)/dist/CaseZeroRecomp}
IMAGE=${2:-registry.fedoraproject.org/fedora-minimal:latest}

[ -x "$STAGE/cz_runtime" ] || { echo "FAIL: no $STAGE/cz_runtime" >&2; exit 1; }
command -v podman >/dev/null || { echo "FAIL: podman not installed" >&2; exit 1; }

# One real microcode blob, so the container run can prove the DXC dlopen works — the
# library ldd cannot vouch for (it is how the first bundle died, and a shipped build
# that cannot dlopen it cannot build its shader cache). Deliberately REQUIRED, not
# skipped-if-absent: a gate with a weak mode gets run in its weak mode forever.
UCODE_SRC=${CZ_GATE_UCODE:-$HOME/DR2CZ-troubleshooting/ucode-dumps}
UCODE_ONE=$(ls "$UCODE_SRC"/ps_*.ucode 2>/dev/null | head -1)
[ -n "$UCODE_ONE" ] || { echo "FAIL: no ps_*.ucode under $UCODE_SRC (set CZ_GATE_UCODE)" >&2; exit 1; }
UCODE_DIR=$(mktemp -d)
mkdir -p "$UCODE_DIR/in"
cp "$UCODE_ONE" "$UCODE_DIR/in/"

# The real package, so the container can run the WHOLE first-run flow (release-plan
# §5 item 3): in-process extract -> disc shader build -> a boot that reads the
# extracted tree. Required for the same reason the ucode is. The one thing a
# GPU-less container cannot cover is the renderer itself; the gate says so in its
# output rather than leaving the gap implied.
PKGFILE=${CZ_GATE_PACKAGE:-$(find "$(cd "$(dirname "$0")/.." && pwd)/assets/package" \
    -type f -size +100M 2>/dev/null | head -1)}
[ -n "$PKGFILE" ] && [ -f "$PKGFILE" ] \
    || { echo "FAIL: no STFS package under assets/package (set CZ_GATE_PACKAGE)" >&2; exit 1; }

# `-i` is load-bearing and its absence is why the first run of this gate printed
# "GATE PASSED" having executed nothing: without it podman does not attach stdin, the
# heredoc goes nowhere, `sh -s` reads EOF and exits 0. A gate whose body never ran and
# whose exit code says OK is the worst possible shape — it is gotcha 483 with the
# clean-shutdown-and-still-report-a-number symptom, reproduced in a tool written the
# same afternoon that gotcha was read. The marker check below is the belt to this
# brace: the gate now requires evidence that its body executed, not just an exit code.
echo "==> $IMAGE, bundle mounted read-only at /app"
LOG=$(mktemp)
trap 'rm -f "$LOG"; rm -rf "$UCODE_DIR"' EXIT
podman run --rm -i -v "$STAGE:/app:ro,Z" -v "$UCODE_DIR:/ucode:Z" \
    -v "$PKGFILE:/pkg/package:ro,Z" "$IMAGE" /bin/sh -s > "$LOG" 2>&1 <<'IN'
set -u

# The Vulkan LOADER is installed here on purpose, and it is the one library this gate
# adds. It is not bundled (a bundled loader finds the wrong ICD or none — the plan says
# so, and every Linux game ships it this way), so a player HAS one, supplied by their
# GPU driver stack. Without it in the image, `libvulkan.so.1 => not found` appears and
# the gate has to carry an exception for it — and an exception is exactly where a real
# missing library would hide. Installing it instead means the rule can be absolute:
# NOTHING may be "not found".
echo "--- installing the Vulkan loader (the one library a player's driver supplies):"
microdnf -y --nodocs install vulkan-loader >/dev/null 2>&1 \
    || { echo "    could not install vulkan-loader — gate cannot run"; exit 2; }
echo "    ok"

echo "--- the container has none of the dev packages:"
clean=1
for l in libSDL2-2.0.so.0 libavcodec.so.62 libavutil.so.60; do
    if [ -e "/lib64/$l" ]; then
        echo "    !! /lib64/$l EXISTS -- this image is not clean, the gate proves nothing"
        clean=0
    else
        echo "    /lib64/$l absent (good)"
    fi
done
[ "$clean" = 1 ] || exit 3

# libstdc++ is deliberately NOT on that list, and it is the more interesting case. It
# is present in this image (vulkan-loader pulls it in) and it is present on almost any
# real machine, so requiring its absence would just make the gate unrunnable. What
# matters is which copy WINS, and the ldd check below answers that: if the executable's
# $ORIGIN/lib RUNPATH did not take precedence over /lib64, libstdc++ shows up as
# resolved outside the bundle and the gate fails. A system copy sitting there is
# therefore a stronger test of the RPATH than an empty filesystem would be.
if [ -e /lib64/libstdc++.so.6 ]; then
    echo "    /lib64/libstdc++.so.6 present -- good, it makes the RPATH check meaningful"
fi

echo "--- ldd /app/cz_runtime:"
ldd /app/cz_runtime > /tmp/ldd.txt 2>&1
sed 's/^\t/    /' /tmp/ldd.txt

echo "--- verdict:"
rc=0

# 1. NOTHING may be missing. No exceptions -- see the loader note above.
if grep -q "not found" /tmp/ldd.txt; then
    echo "    MISSING LIBRARIES:"
    grep "not found" /tmp/ldd.txt | sed 's/^\t/      /'
    rc=1
fi

# 2. Everything that resolves must be either inside the bundle or on the permitted
#    list. `not found` lines are excluded first, or awk's $3 picks up the word "not"
#    and reports it as a mysterious library outside the bundle.
outside=$(grep -v "not found" /tmp/ldd.txt | awk '/=>/ {print $3}' \
    | grep -v '^/app/' | grep -v '^$' \
    | grep -vE '/(libc|libm|libvulkan|libdl|libpthread|librt)\.so')
if [ -n "$outside" ]; then
    echo "    RESOLVED OUTSIDE THE BUNDLE and not on the permitted list:"
    echo "$outside" | sed 's/^/      /'
    rc=1
fi

[ $rc -eq 0 ] && echo "    OK: every bundled dependency resolved inside /app; only libc,"
[ $rc -eq 0 ] && echo "        libm, the Vulkan loader and ld.so are the host system's."

echo "--- cz_runtime --smoke (the phase 0.2 link gate, in the PACKAGED binary):"
/app/cz_runtime --smoke 2>&1 | tail -3 | sed 's/^/    /'

# THE DLOPEN GATE (part 85). The shader translator dlopens lib/libdxcompiler.so —
# invisible to every ldd check above, and a bundle missing it boots into the black
# screen the first-run check exists to prevent. So RUN a translation, on a real
# microcode blob, in this container where no other dxcompiler exists. HOME is unset
# so the dev-checkout fallback path cannot rescue a broken bundle.
echo "--- the DXC dlopen: translate one real shader inside the container:"
mkdir -p /tmp/spv
if HOME= /app/cz_runtime --translate-shaders /ucode/in /tmp/spv 2>&1 | sed 's/^/    /' \
   && ls /tmp/spv/*.spv >/dev/null 2>&1; then
    echo "    dxc-translate OK: $(ls /tmp/spv/*.spv | head -1 | xargs -n1 basename)"
else
    echo "    dxc-translate FAILED"
fi

echo "--- the bundle carries the release files:"
for f in README.md THIRD_PARTY.md LICENSE cz_defaults.env lib/libdxcompiler.so lib/LICENSE.DXC; do
    if [ -e "/app/$f" ]; then echo "    $f present"; else echo "    $f MISSING"; fi
done

# THE FIRST-RUN FLOW, END TO END (release-plan §5 item 3), on a machine that has
# nothing but the bundle and the package. Run as the three explicit verbs rather
# than the automatic boot path because this container has NO GPU: the automatic
# path is renderer-gated, and it is exercised on the host (part 85's fake-root run,
# with the auto-extract, the prebuild AND the renderer). What this section proves
# is that the same three steps work against the bundle's own glibc-floor claim.
echo "--- the first-run flow, end to end (extract -> shader build -> boot):"
mkdir -p /w/assets/game
echo "    [1/3] in-process extract of the mounted package:"
/app/cz_runtime --extract-package /pkg/package /w/assets/game 2>&1 | tail -1 | sed 's/^/    /'
echo "    [2/3] disc shader build (DXC, all cores):"
/app/cz_runtime --build-shader-cache /w/assets/game/data/shaders/deadrisingprologue-ps.big \
    /w/assets/shader_spv 2>&1 | grep -E "translated|refused|FAIL" | tail -2 | sed 's/^/    /'
echo "    [3/3] boot from the extracted tree (no GPU in this container — the renderer"
echo "          cannot run here and is covered by the host-side first-run test):"
mkdir -p /w/assets/save
CZ_ROOT=/w CZ_VKDRAW=0 CZ_NO_WINDOW=1 CZ_NO_AUDIO_OUT=1 CZ_FILE_TRACE=1 \
    timeout 45 /app/cz_runtime > /tmp/boot.log 2>&1
bigs=$(grep -c "\.big" /tmp/boot.log)
echo "    the guest referenced $bigs .big archives from the extracted tree in 45 s"
if [ "$bigs" -gt 0 ]; then
    echo "    first-run-flow OK"
else
    echo "    first-run-flow FAILED — first lines of the boot:"
    sed -n '1,20p' /tmp/boot.log | sed 's/^/      /'
fi

echo "--- the first-run refusal, from a container with no game:"
# CZ_ROOT because /app is read-only and the bundle's own assets/package is empty either
# way; this is the state a player is in before they drop the package in.
mkdir -p /tmp/empty/assets/package
CZ_ROOT=/tmp/empty /app/cz_runtime 2>&1 | head -10 | sed 's/^/    /'

exit $rc
IN
rc=$?
cat "$LOG"
echo

# THE GATE MUST HAVE RUN. Three markers, one per section, each of which can only be
# printed by code inside the container. An exit status is not evidence that a body
# executed (see the -i note above).
missing_marker=0
for marker in "ldd /app/cz_runtime:" "verdict:" "cz_runtime --smoke" "first-run refusal"; do
    grep -qF -- "$marker" "$LOG" || { echo "GATE DID NOT RUN: no \"$marker\" in the output" >&2
                                   missing_marker=1; }
done
# And the smoke gate's own sentence, which is the one line that says the packaged
# binary works at all.
grep -q "OK: every generated symbol resolved" "$LOG" || {
    echo "GATE FAILED: the packaged binary did not pass --smoke" >&2; missing_marker=1; }
# The first-run flow's own sentence — the extract, the shader build and a boot that
# read the result, all inside the container.
grep -q "first-run-flow OK" "$LOG" || {
    echo "GATE FAILED: the first-run flow did not complete in the container" >&2
    missing_marker=1; }
# The dlopen gate's own sentence — a .spv actually produced inside the container.
grep -q "dxc-translate OK" "$LOG" || {
    echo "GATE FAILED: the bundled libdxcompiler.so did not translate a shader" >&2
    missing_marker=1; }
# And nothing the bundle must carry may be missing.
grep -q " MISSING" "$LOG" && {
    echo "GATE FAILED: release files missing from the bundle (see above)" >&2
    missing_marker=1; }

if [ $rc -eq 0 ] && [ $missing_marker -eq 0 ]; then
    echo "GATE PASSED"
    exit 0
fi
echo "GATE FAILED (container rc=$rc)"
exit 1
