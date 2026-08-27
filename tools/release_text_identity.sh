#!/usr/bin/env bash
# A.3's GATE, and the reason it costs a second instead of an hour.
#
# docs/release-plan.md A.3 says "a build-type change is a performance change until
# measured", and proposes measuring it with the crowd route and part80_trace_band.py.
# That is the right instinct and it is the expensive answer: this workload's noise floor
# is 10-13% at one run a side (gotcha 229), so an honest A/B is three runs an arm and
# an hour of wall time — to test a difference that, the way runtime/CMakeLists.txt
# defines Release, CANNOT EXIST.
#
#   RelWithDebInfo   -O2 -g -DNDEBUG
#   Release          -O2 -g -DNDEBUG, then objcopy splits the debug info off
#
# -g does not affect code generation and objcopy --strip-debug does not touch .text.
# So the two binaries' executable code should be byte-identical, and THAT is the
# statement to check — it is decisive where a frame-time comparison is statistical, and
# it names the file if it ever stops being true.
#
# If this ever fails, something real changed: a flag drifted, LTO got switched on, or
# somebody set -O3. The message says so rather than leaving the reader to infer it. A
# failure here is a genuine reason to fall back to the route A/B; a pass makes the route
# run a confirmation rather than the evidence.
#
# Usage:  tools/release_text_identity.sh [devBuildDir] [releaseBuildDir]
set -uo pipefail

DEV=${1:-runtime/build}
REL=${2:-runtime/build-release}

fail() { echo "FAIL: $*" >&2; exit 1; }

for d in "$DEV" "$REL"; do
    [ -x "$d/cz_runtime" ] || fail "no executable at $d/cz_runtime"
done

OBJCOPY=$(command -v llvm-objcopy || command -v objcopy) || fail "no objcopy"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# .text alone, not the whole file: the release binary legitimately differs in its
# section table, its .gnu_debuglink and its build id. Comparing whole files would fail
# for reasons that are the POINT of the split, and a gate that fails for expected
# reasons gets ignored.
for arm in dev rel; do
    case $arm in dev) src=$DEV/cz_runtime ;; rel) src=$REL/cz_runtime ;; esac
    "$OBJCOPY" -O binary --only-section=.text "$src" "$tmp/$arm.text" \
        || fail "objcopy could not extract .text from $src"
    sz=$(stat -c%s "$tmp/$arm.text")
    [ "$sz" -gt 1000000 ] || fail "$arm .text is only $sz bytes — that is not this image"
done

# THE CONFIGURATION DIFF, printed before the verdict rather than suggested after it.
# The first version of this script offered "an -O level drifted, LTO, a stale tree" as
# the things to go and check by hand — and the first real FAIL it produced was none of
# those: it was CZ_FFMPEG_PREFIX set in one tree and not the other, because a release
# links the LGPL ffmpeg and a dev tree links Fedora's, and their headers inline
# differently. A gate that names the wrong cause sends the reader somewhere else
# (gotcha 483's shape: the failure still reported a number). So it reads both caches
# and says which of the variables that CAN move .text disagree.
cachediff=""
# CZ_BUNDLE_RPATH is in this list and it surprised the first run of the matched
# comparison. It is a LINK option, so it should not be able to touch .text — but the
# RUNPATH string lives in .dynstr, which sits before .text, so adding one lengthens an
# earlier section and RELOCATES the whole image. Every address-bearing byte in .text
# then differs while every instruction is the same instruction. That is a layout
# difference and not an optimisation difference, and the way to keep this gate decisive
# is to hold it matched rather than to carve out an exception for it.
for var in CMAKE_BUILD_TYPE CMAKE_CXX_COMPILER CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE \
           CMAKE_CXX_FLAGS_RELWITHDEBINFO CZ_FFMPEG_PREFIX CZ_SDL2_PREFIX CZ_DEBUG_TUS \
           CZ_BUNDLE_RPATH CMAKE_INTERPROCEDURAL_OPTIMIZATION; do
    va=$(grep -m1 "^$var:" "$DEV/CMakeCache.txt" 2>/dev/null | cut -d= -f2-)
    vb=$(grep -m1 "^$var:" "$REL/CMakeCache.txt" 2>/dev/null | cut -d= -f2-)
    [ "$va" = "$vb" ] || cachediff="$cachediff  $var:\n      dev     '$va'\n      release '$vb'\n"
done

a=$(sha256sum "$tmp/dev.text" | cut -d' ' -f1)
b=$(sha256sum "$tmp/rel.text" | cut -d' ' -f1)
sz=$(stat -c%s "$tmp/dev.text")

printf 'dev      %s  %s\n' "$a" "$DEV/cz_runtime"
printf 'release  %s  %s\n' "$b" "$REL/cz_runtime"
printf '.text    %s bytes\n' "$sz"

if [ "$a" = "$b" ]; then
    echo "OK: the release binary's executable code is byte-identical to the dev build's."
    echo "    The release build type therefore cannot have changed the frame."
    exit 0
fi

echo >&2
echo "FAIL: .text differs between the two builds." >&2
if [ -n "$cachediff" ]; then
    echo >&2
    echo "  These configuration variables differ, and each of them can move .text:" >&2
    printf "$cachediff" >&2
    echo "  Match them and re-run. If the ONLY difference is CMAKE_BUILD_TYPE, then the" >&2
    echo "  build type really did change the code and the next paragraph applies." >&2
else
    echo "  The configuration variables this script knows about all MATCH, so the cause" >&2
    echo "  is something it does not read — check the full caches by hand." >&2
fi
cat >&2 <<'MSG'

  A .text difference is a REAL code-generation difference, not a packaging one, and it
  means the change must be measured like any other item on this project before it
  ships: three runs an arm on tools/part80_crowdroute.sh, read with
  tools/part80_trace_band.py. See docs/measurement.md and gotcha 229.
MSG
exit 1
