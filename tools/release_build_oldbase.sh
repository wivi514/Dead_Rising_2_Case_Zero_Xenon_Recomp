#!/usr/bin/env bash
# Build the Linux release on the OLD BASE, so the artifact's glibc floor is the base's and
# not this machine's (docs/release-plan.md E.2 / §9.2 item 3, part 104).
#
# Everything the artifact links is built INSIDE the container: SDL2 and the LGPL ffmpeg
# (their host-built copies under thirdparty/ carry this machine's 2.43 and would raise the
# floor straight back), then the runtime, then the packaging — the packaging too, because
# tools/release_package_linux.sh bundles what `ldd` resolves, and an `ldd` run on the host
# would resolve libstdc++ to the host's GLIBCXX_3.4.35 / glibc-2.43 copy and ship that.
#
# The repository is mounted at ITS OWN ABSOLUTE PATH inside the container. Not a
# convenience: the build embeds RPATHs to thirdparty/oldbase/{sdl2,ffmpeg-lgpl}, and the
# packaging script's `ldd` must resolve them to the same files on either side; a different
# mount point would resolve them to nothing (or, on the host, to the wrong library).
#
# THE IDENTITY GATE MOVES INSIDE TOO. tools/release_text_identity.sh proves the build TYPE
# is a null by comparing .text between a RelWithDebInfo and a Release build of the same
# toolchain. Across toolchains it cannot hold — jammy's clang 15 and the dev box's clang 22
# generate different code for the same -O2, as clang-cl on Windows already does — so the
# matched RelWithDebInfo build is made here, with the same compiler, and the gate is run on
# that pair. The compiler change itself is a real code-generation change, stated as such in
# the release record; it is the same class of change the Windows leg has always carried.
#
# Usage:  tools/release_build_oldbase.sh              build image if needed, build, package
#         tools/release_build_oldbase.sh --shell      an interactive shell in the image
#         CZ_OLDBASE_SKIP_DEPS=1 ...                  reuse thirdparty/oldbase/{sdl2,ffmpeg-lgpl}
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${CZ_OLDBASE_IMAGE:-cz-oldbase:jammy}
OB=$ROOT/thirdparty/oldbase
DXC_SRC=${CZ_DXC_LIB:-$HOME/GithubRepo/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxcompiler.so}
# XenosRecomp's translator is compiled INTO the runtime (release D.2), so the checkout is
# mounted read-only at its own path and named to CMake explicitly — the CMakeLists
# default derives it from $HOME, which is the throwaway one inside the container.
XENOS_ROOT=${XENOS_ROOT:-$HOME/GithubRepo/XenosRecomp}
[ -f "$XENOS_ROOT/XenosRecomp/shader_recompiler.cpp" ] || fail "no XenosRecomp checkout at $XENOS_ROOT (set XENOS_ROOT)"
# XenonRecomp: the runtime links its XenonUtils (the XEX loader), fmt and xxHash as static
# libraries out of a BUILD TREE. The host's build tree was compiled by the host's clang
# against the host's glibc headers, so those objects are rebuilt inside the container
# into thirdparty/oldbase/xenon-build — three targets, not the recompiler.
XENON_ROOT=${XENON_ROOT:-$HOME/GithubRepo/XenonRecomp}
[ -f "$XENON_ROOT/XenonUtils/ppc_context.h" ] || fail "no XenonRecomp checkout at $XENON_ROOT (set XENON_ROOT)"

fail() { echo "FAIL: $*" >&2; exit 1; }
command -v podman >/dev/null || fail "podman not installed"
[ -f "$DXC_SRC" ] || fail "no libdxcompiler.so at $DXC_SRC (set CZ_DXC_LIB)"
[ -d "$ROOT/ppc" ] || fail "no ppc/ — regenerate the recompiled tree first (CLAUDE.md, Commands)"

if ! podman image exists "$IMAGE"; then
    echo "==> building the image $IMAGE (tools/release/oldbase/Containerfile)"
    podman build -t "$IMAGE" -f "$ROOT/tools/release/oldbase/Containerfile" "$ROOT/tools/release/oldbase"
fi

mkdir -p "$OB/work/sdl2" "$OB/work/ffmpeg"
# The source tarballs the host scripts already fetched, so the container needs no network
# for them (it still has network; this is so a rebuild is byte-for-byte the same source).
[ -f /var/tmp/cz-sdl2-build/SDL2-2.32.10.tar.gz ] && cp -n /var/tmp/cz-sdl2-build/SDL2-2.32.10.tar.gz "$OB/work/sdl2/" || true
[ -f /var/tmp/cz-ffmpeg-build/ffmpeg-8.1.2.tar.xz ] && cp -n /var/tmp/cz-ffmpeg-build/ffmpeg-8.1.2.tar.xz "$OB/work/ffmpeg/" || true

# A throwaway HOME: the runtime's caches and the packaging script's $HOME-relative
# defaults must not touch the real one from inside the container. TAR_OPTIONS: the
# container's root is this user, but a tarball's recorded owners map through the subuid
# range, so a `tar xf` run as container root produced a source tree the HOST user could
# not read (`podman unshare rm -rf` is the way to delete one if it happens again).
HOMEDIR=$OB/home
mkdir -p "$HOMEDIR"

RUN=(podman run --rm -i
     -v "$ROOT:$ROOT:Z"
     -v "$DXC_SRC:/opt/dxc/libdxcompiler.so:ro,Z"
     -v "$XENOS_ROOT:$XENOS_ROOT:ro,Z"
     -v "$XENON_ROOT:$XENON_ROOT:ro,Z"
     -e XENOS_ROOT="$XENOS_ROOT" -e XENON_ROOT="$XENON_ROOT"
     -e HOME="$HOMEDIR" -e CZ_DXC_LIB=/opt/dxc/libdxcompiler.so
     -e CZ_SDL2_WORK="$OB/work/sdl2" -e CZ_FFMPEG_WORK="$OB/work/ffmpeg"
     -e CZ_SDL2_PREFIX="$OB/sdl2" -e CZ_FFMPEG_PREFIX="$OB/ffmpeg-lgpl"
     -e CZ_OLDBASE_SKIP_DEPS="${CZ_OLDBASE_SKIP_DEPS:-}"
     -e TAR_OPTIONS=--no-same-owner
     -e CZ_OLDBASE_INSIDE=1
     -w "$ROOT" "$IMAGE")

if [ "${1:-}" = "--shell" ]; then
    exec "${RUN[@]/-i/-it}" bash
fi

"${RUN[@]}" bash -s <<'IN'
set -euo pipefail
echo "==> inside $(. /etc/os-release; echo "$PRETTY_NAME"), $(ldd --version | head -1), $(clang++ --version | head -1)"
OB=$PWD/thirdparty/oldbase

if [ -z "${CZ_OLDBASE_SKIP_DEPS:-}" ] || [ ! -f "$OB/sdl2/lib/libSDL2-2.0.so.0" ] || [ ! -f "$OB/ffmpeg-lgpl/lib/libavcodec.so" ]; then
    echo "==> SDL2 (real SDL2, X11 + Wayland dlopened) on the old base"
    tools/build_sdl2.sh "$OB/sdl2" | tail -4
    echo "==> ffmpeg (LGPL, xma1+xma2, WITH nasm) on the old base"
    tools/build_ffmpeg_lgpl.sh "$OB/ffmpeg-lgpl" | grep -E "licence|assembly|libavcodec |OK:"
fi

XB=$OB/xenon-build
if [ ! -f "$XB/XenonUtils/libXenonUtils.a" ] || [ ! -f "$XB/thirdparty/fmt/libfmt.a" ] \
   || [ ! -f "$XB/thirdparty/xxHash/cmake_unofficial/libxxhash.a" ]; then
    echo "==> XenonRecomp's XenonUtils + fmt + xxHash on the old base (static libs the runtime links)"
    cmake -S "$XENON_ROOT" -B "$XB" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$XB.configure.log" 2>&1 \
        || { tail -30 "$XB.configure.log"; exit 1; }
    cmake --build "$XB" --target XenonUtils fmt xxhash -j"$(nproc)" 2>&1 | tail -1
fi

cfg() {
    cmake -S runtime -B "$1" -G Ninja -DCMAKE_BUILD_TYPE="$2" \
        -DCZ_FFMPEG_PREFIX="$OB/ffmpeg-lgpl" -DCZ_SDL2_PREFIX="$OB/sdl2" -DCZ_BUNDLE_RPATH=ON \
        -DXENOS_ROOT="$XENOS_ROOT" -DXENON_ROOT="$XENON_ROOT" -DXENON_BUILD="$XB" \
        > "$1.configure.log" 2>&1 || { tail -30 "$1.configure.log"; exit 1; }
}
echo "==> configuring + building runtime/build-release-oldbase (Release)"
cfg runtime/build-release-oldbase Release
cmake --build runtime/build-release-oldbase -j"$(nproc)" 2>&1 | tail -2
echo "==> the matched RelWithDebInfo build for the identity gate (same toolchain)"
cfg runtime/build-relmatch-oldbase RelWithDebInfo
cmake --build runtime/build-relmatch-oldbase -j"$(nproc)" 2>&1 | tail -2
echo "==> tools/release_text_identity.sh (build type is a null on THIS toolchain)"
tools/release_text_identity.sh runtime/build-relmatch-oldbase runtime/build-release-oldbase

echo "==> packaging (ldd resolved INSIDE the old base, so the bundled libstdc++ is the base's)"
tools/release_package_linux.sh runtime/build-release-oldbase dist
IN

echo
echo "==> NEXT"
echo "    tools/release_package_appimage.sh                     # wrap dist/CaseZeroRecomp"
echo "    # AT THE FLOOR (ubuntu:22.04 = glibc 2.35, the base): both must print GATE PASSED"
echo "    tools/release_gate_clean_container.sh dist/CaseZeroRecomp docker.io/library/ubuntu:22.04"
echo "    tools/release_gate_clean_container.sh dist/CaseZeroRecomp-linux-x86_64.AppImage docker.io/library/ubuntu:22.04"
echo "    # BELOW the floor (Rocky 9 = glibc 2.34): expected to REFUSE with 'GLIBC_2.35 not found'"
echo "    # (libavutil needs 2.35) — a demonstration of the documented floor, not a passing gate"
echo "    tools/release_gate_clean_container.sh dist/CaseZeroRecomp quay.io/rockylinux/rockylinux:9-minimal"
