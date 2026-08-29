#!/usr/bin/env bash
# Assemble the Linux release artifact (docs/release-plan.md A.4 and E.2).
#
# The layout is §2.2's, and the one decision worth stating is WHICH LIBRARIES GO IN.
#
#   bundled      SDL2, libavcodec, libavutil, libstdc++, libgcc_s
#   NOT bundled  the Vulkan loader, libc, libm, ld.so
#
# The Vulkan loader is deliberate and it is in the plan: it is the component that knows
# where THIS machine's drivers live, and a bundled one would either find nothing or find
# the wrong ICD. Every Linux game ships it this way.
#
# libc is deliberate for the opposite reason: glibc is not relocatable, and a bundled
# copy loaded against the host's ld.so is the packaging defect that produces
# `symbol lookup error` on someone else's distribution. Which means the artifact
# inherits the BUILD MACHINE's glibc floor, and that is a real limitation rather than
# something this script solves — see the note it prints at the end. The proper answers
# are an old build base or an AppImage runtime, and both are E.2 work.
#
# The RPATH does the finding, not a launcher script: runtime/CMakeLists.txt links the
# release binary with $ORIGIN/lib, so the bundle is what loads even when the player runs
# the executable directly. A launcher that sets LD_LIBRARY_PATH is a thing a player can
# bypass, and then the bundle silently is not what loads — the exact failure A.4's gate
# exists to catch.
#
# Usage:  tools/release_package_linux.sh [buildDir] [outDir]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/runtime/build-release}
OUT=${2:-$ROOT/dist}
NAME=CaseZeroRecomp
STAGE=$OUT/$NAME

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BUILD/cz_runtime" ] || fail "no executable at $BUILD/cz_runtime — configure a
  release tree first:
    cmake -S runtime -B runtime/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \\
        -DCZ_FFMPEG_PREFIX=$ROOT/thirdparty/ffmpeg-lgpl
    cmake --build runtime/build-release -j\$(nproc)"

# THE LICENCE CHECK, BEFORE ANYTHING IS COPIED (release-plan §4). Fedora's ffmpeg is
# --enable-gpl and this repo is PolyForm Noncommercial; shipping the two together is a
# conflict. The check is on what the binary ACTUALLY LINKS, not on what was configured,
# because the configure flag and the resolved library are two different facts and only
# the second one ships.
if ldd "$BUILD/cz_runtime" | grep -q 'libavcodec.*=> */\(usr/\)\?lib'; then
    fail "this binary links the SYSTEM libavcodec, which on this machine is
  --enable-gpl and must not be shipped with a PolyForm repo (release-plan §4).
    tools/build_ffmpeg_lgpl.sh
    cmake -S runtime -B $BUILD -DCZ_FFMPEG_PREFIX=$ROOT/thirdparty/ffmpeg-lgpl ..."
fi

echo "==> staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"/{lib,tools,assets/package}

cp "$BUILD/cz_runtime" "$STAGE/"

# The libraries, resolved from the binary's OWN ldd rather than from a hand-written
# list. A list drifts the first time a dependency is added and nobody notices until a
# player reports a missing library; ldd cannot drift.
echo "==> bundling libraries"
BUNDLE_RE='libSDL2|libavcodec|libavutil|libstdc\+\+|libgcc_s'
ldd "$BUILD/cz_runtime" | awk '/=>/ {print $3}' | grep -E "$BUNDLE_RE" | while read -r so; do
    [ -f "$so" ] || continue
    # Copy the real file AND recreate the SONAME symlink, because the DT_NEEDED entry
    # names the soname (libavcodec.so.62) and not the real file (…so.62.28.102).
    real=$(readlink -f "$so")
    cp -n "$real" "$STAGE/lib/" 2>/dev/null || true
    base=$(basename "$so")
    if [ "$base" != "$(basename "$real")" ]; then
        ln -sf "$(basename "$real")" "$STAGE/lib/$base"
    fi
    printf '    %-30s %8s KB\n' "$base" "$(( $(stat -Lc%s "$so") / 1024 ))"
done

# THE SHIM CHECK, and the reason it is here rather than left to the gate.
#
# The bundle's first version linked Fedora's libSDL2-2.0.so.0, which is `sdl2-compat` —
# a shim that dlopen()s libSDL3.so.0. `ldd` cannot see a dlopen, so the artifact passed
# every static check and then died in a clean container with "Failed loading SDL3
# library.". The general answer is the gate, which RUNS the binary; this is the specific
# one, because a class of defect that has bitten once should have a check that names it
# rather than a symptom somebody has to interpret.
if [ -f "$STAGE/lib/libSDL2-2.0.so.0" ] \
   && strings "$STAGE/lib/libSDL2-2.0.so.0" | grep -q libSDL3; then
    fail "the bundled libSDL2 is sdl2-compat (it dlopens libSDL3, which ldd cannot see).
  Build real SDL2 and point the release tree at it:
    tools/build_sdl2.sh
    cmake -S runtime -B $BUILD -DCZ_SDL2_PREFIX=$ROOT/thirdparty/sdl2 ..."
fi

# THE DXC LIBRARY, and it is NOT optional (release-plan E, part 85). Milestone D made
# the shipped build translate its own shaders — the disc pass at first run and the
# first-sight JIT both go through gpu/shader_translator.cpp, which dlopens
# libdxcompiler.so from <exe>/lib. A bundle without it boots, REFUSES every
# translation with one log line, and presents the black screen the first-run check
# exists to prevent — and ldd cannot report it missing, because a dlopen is invisible
# to ldd (the sdl2-compat lesson, one shelf over). So: fail here, loudly.
DXC_SRC=${CZ_DXC_LIB:-$HOME/GithubRepo/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxcompiler.so}
[ -f "$DXC_SRC" ] || fail "no libdxcompiler.so at $DXC_SRC — the shipped shader
  translator dlopens it and a bundle without it cannot build its cache.
  Point CZ_DXC_LIB at a libdxcompiler.so (XenosRecomp's thirdparty/dxc-bin has one)."
cp "$DXC_SRC" "$STAGE/lib/libdxcompiler.so"
cp "$ROOT/tools/licenses/LICENSE.DXC.txt" "$STAGE/lib/LICENSE.DXC"
printf '    %-30s %8s KB\n' "libdxcompiler.so" "$(( $(stat -c%s "$DXC_SRC") / 1024 ))"

# The drop-in step's dev-side tool, kept as the REFERENCE implementation: the runtime
# unpacks the package itself as of part 85 (host/stfs_extract.cpp), and this copy is
# what a player uses if they want to unpack by hand or the in-process path refuses
# their container (SVOD, for instance, which only the Python handles).
cp "$ROOT/tools/extract_stfs.py" "$STAGE/tools/"

cat > "$STAGE/assets/package/PUT_YOUR_GAME_HERE.txt" <<'TXT'
Put your own copy of the Dead Rising 2: Case Zero XBLA package in this directory.

It is the file your Xbox 360 downloaded. On the console's storage it lives at

    Content/0000000000000000/58410A8D/000D0000/<a long hash, no file extension>

and it is about 825 MB. Copying the whole 58410A8D folder in here works too — the
runtime looks recursively.

This build ships no game data and cannot supply it. Nothing else goes in this
directory; the unpacked files are written to ../game/ on first run.
TXT

cp "$ROOT/LICENSE" "$STAGE/"
cp "$ROOT/tools/release/README.md" "$STAGE/"

# RELEASE DEFAULTS (part 85). Every dev recipe says CZ_VKDRAW=1 out loud and the
# binary defaults it off, because the same binary with it unset is the control arm for
# every renderer claim. A player gets the opposite default from this file, which
# main.cpp applies only for variables the environment leaves unset — so the shipped
# binary stays byte-identical to the dev one and CZ_VKDRAW=0 still works.
cat > "$STAGE/cz_defaults.env" <<'ENV'
# Defaults for a shipped build. KEY=VALUE, one per line, # comments.
# Anything set in your environment overrides these.
CZ_VKDRAW=1
ENV

# THIRD_PARTY.md, GENERATED (release-plan E.3). Written from what the binary actually
# links so it cannot drift away from the artifact it describes — an attribution file
# maintained by hand is correct on the day it is written and wrong from the next commit.
echo "==> generating THIRD_PARTY.md"
{
    echo "# Third-party components in this build"
    echo
    echo "Generated by \`tools/release_package_linux.sh\` from the libraries this"
    echo "executable actually links. Do not edit by hand."
    echo
    echo '| component | licence | how it is here |'
    echo '|---|---|---|'
    echo '| XenonRecomp / XenosRecomp (hedge-dev) | MIT | the recompiled image and the translated shaders are their output |'
    echo '| SDL2 | zlib | bundled in `lib/` |'
    echo '| ffmpeg — libavcodec, libavutil | LGPL 2.1 or later | bundled in `lib/`; see below |'
    echo '| DirectX Shader Compiler (DXC) | University of Illinois/NCSA | `lib/libdxcompiler.so`, loaded at run time to translate shaders; license in `lib/LICENSE.DXC` |'
    echo '| o1heap | MIT | compiled in (the guest heaps) |'
    echo '| SIMDe | MIT | compiled in (the guest VMX unit) |'
    echo '| Vulkan loader | Apache 2.0 | NOT bundled — the host system supplies it |'
    echo
    echo '## ffmpeg (LGPL)'
    echo
    echo 'The bundled libavcodec/libavutil are built from unmodified upstream ffmpeg'
    echo 'sources with GPL and non-free components disabled, configured for the two XMA'
    echo 'decoders this port needs and nothing else. The exact recipe is'
    echo '`tools/build_ffmpeg_lgpl.sh` in the source repository.'
    echo
    echo 'They are DYNAMICALLY linked and shipped as separate files in `lib/`, so they'
    echo 'can be replaced with your own build.'
    echo
    if [ -f /var/tmp/cz-ffmpeg-build/ffmpeg-8.1.2.tar.xz ]; then
        echo 'Corresponding source:'
        echo
        echo '```'
        echo 'https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz'
        sha256sum /var/tmp/cz-ffmpeg-build/ffmpeg-8.1.2.tar.xz | awk '{print "sha256 " $1}'
        echo '```'
        echo
    fi
    echo '## Libraries actually linked by this binary'
    echo
    echo '```'
    ldd "$BUILD/cz_runtime" | sed 's/^\t//'
    echo '```'
} > "$STAGE/THIRD_PARTY.md"

if [ -f /var/tmp/cz-ffmpeg-build/ffmpeg-8.1.2/COPYING.LGPLv2.1 ]; then
    cp /var/tmp/cz-ffmpeg-build/ffmpeg-8.1.2/COPYING.LGPLv2.1 "$STAGE/lib/LICENSE.ffmpeg"
fi
if [ -f "$ROOT/thirdparty/sdl2/share/licenses/SDL2/LICENSE.txt" ]; then
    cp "$ROOT/thirdparty/sdl2/share/licenses/SDL2/LICENSE.txt" "$STAGE/lib/LICENSE.SDL2"
fi

echo "==> archive"
mkdir -p "$OUT"
TAR=$OUT/$NAME-linux-x86_64.tar.zst
rm -f "$TAR"
tar --zstd -cf "$TAR" -C "$OUT" "$NAME"
sha256sum "$TAR" > "$TAR.sha256"

printf '    %s  %s MB\n' "$(basename "$TAR")" "$(( $(stat -c%s "$TAR") / 1024 / 1024 ))"
cat "$TAR.sha256" | sed 's/^/    /'

cat <<MSG

==> KNOWN LIMITATION, stated rather than discovered by a player
    This artifact inherits the glibc floor of the machine it was built on
    ($(ldd --version | head -1)).
    It will refuse to start on any distribution older than that, with a
    "GLIBC_x.yz not found" message. Fixing it properly means building on an old
    base image or shipping an AppImage runtime, and that is release-plan E.2 work
    that has not been done.

==> NEXT: prove the bundle is what loads (A.4's gate)
    tools/release_gate_clean_container.sh $STAGE
MSG
