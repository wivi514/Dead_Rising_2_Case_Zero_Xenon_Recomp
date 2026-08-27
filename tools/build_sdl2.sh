#!/usr/bin/env bash
# Build a real SDL2 for shipping (docs/release-plan.md A.4).
#
# WHY, and it was found by running the artifact rather than by reading anything.
#
# The first Linux bundle passed its `ldd` check completely — every library resolved
# inside /app — and then died on its first instruction in a clean container with
#
#     Failed loading SDL3 library.
#
# Fedora's `libSDL2-2.0.so.0` is not SDL2. It is `sdl2-compat`, a shim that implements
# the SDL2 API by dlopen()ing `libSDL3.so.0` at run time. A dlopen is invisible to ldd,
# so the packaging tool the plan specifies for exactly this job (A.4: "ldd on the
# packaged binary in a clean container") CANNOT SEE IT. On the build machine it worked,
# because SDL3 was installed there. That is the classic packaging defect in its purest
# form, and it survives every check made where the thing was built (gotcha: a dependency
# the gate's instrument cannot perceive reads as zero — the same shape as gotcha 25).
#
# Bundling libSDL3 as well would not fix it either: the shim dlopens by SONAME, and the
# shim has no RUNPATH of its own, so the loader would look in the system directories
# and not in ours. Patching one in needs patchelf, which is another build dependency for
# a problem that should not exist.
#
# So: build the real thing. SDL2 proper has no SDL3 dependency, dlopens X11/Wayland at
# runtime the way every shipped Linux game does, and is zlib-licensed. This is also what
# the artifact should have contained in the first place — shipping another project's
# compatibility shim was never a deliberate decision, it was whatever `find_package` on
# this distribution happened to hand us.
#
# Usage:  tools/build_sdl2.sh [prefix]        (default: thirdparty/sdl2)
set -euo pipefail

VERSION=2.32.10
PREFIX=${1:-$(cd "$(dirname "$0")/.." && pwd)/thirdparty/sdl2}
WORK=${CZ_SDL2_WORK:-/var/tmp/cz-sdl2-build}

mkdir -p "$WORK"
cd "$WORK"

TARBALL=SDL2-$VERSION.tar.gz
if [ ! -f "$TARBALL" ]; then
    echo "==> fetching $TARBALL"
    curl -fL --retry 3 -o "$TARBALL.part" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$VERSION/$TARBALL"
    mv "$TARBALL.part" "$TARBALL"
fi
SRC=$WORK/SDL2-$VERSION
[ -d "$SRC" ] || { echo "==> unpacking"; tar xf "$TARBALL"; }

echo "==> configuring"
# Shared only. The *_SHARED options are SDL2's "dlopen this backend at runtime rather
# than linking it", which is what keeps the artifact's hard dependency list at libc and
# libm while still supporting X11, Wayland, PulseAudio, PipeWire and ALSA on whichever
# of them the player actually has. They default ON; naming them here is so that a future
# reader can see the decision was made rather than inherited.
rm -rf "$WORK/build" && mkdir -p "$WORK/build"
cmake -S "$SRC" -B "$WORK/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    -DSDL_X11_SHARED=ON -DSDL_WAYLAND_SHARED=ON \
    -DSDL_PULSEAUDIO_SHARED=ON -DSDL_PIPEWIRE_SHARED=ON -DSDL_ALSA_SHARED=ON \
    >"$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log"; exit 1; }

echo "==> building"
cmake --build "$WORK/build" -j"$(nproc)" >"$WORK/make.log" 2>&1 \
    || { tail -30 "$WORK/make.log"; exit 1; }
rm -rf "$PREFIX"
cmake --install "$WORK/build" >>"$WORK/make.log" 2>&1

# THE CHECK THAT WOULD HAVE CAUGHT THE ORIGINAL DEFECT, stated as the property rather
# than as the symptom: this library must not reach for SDL3 by any route -- neither a
# DT_NEEDED (which ldd sees) nor a dlopen (which it does not). `strings` is the crude
# instrument that covers both, and crude is the point: the sophisticated one missed it.
# CMake installs to lib64 on this distribution and lib on others; find it rather than
# assume. The first version assumed `lib`, readlink returned an empty string, and under
# `set -e` the script died with NO MESSAGE AT ALL — which looked exactly like a build
# failure and was a path typo (gotcha 483's family: the failure did not say what failed).
LIBDIR=""
for d in lib64 lib; do
    [ -e "$PREFIX/$d/libSDL2-2.0.so.0" ] && { LIBDIR=$PREFIX/$d; break; }
done
[ -n "$LIBDIR" ] || { echo "FAIL: no libSDL2-2.0.so.0 under $PREFIX/{lib,lib64}" >&2; exit 1; }
SO=$(readlink -f "$LIBDIR/libSDL2-2.0.so.0")

if strings "$SO" | grep -q 'libSDL3'; then
    echo "FAIL: the built libSDL2 still mentions libSDL3 -- this is the shim, not SDL2." >&2
    exit 1
fi

# And the video backends must actually be in there, or the bundle is an SDL that cannot
# open a window, and that failure arrives at the player's first launch. Read from the
# INSTALLED SDL_config.h, which is the header the runtime will compile against, rather
# than from the build tree.
CFG=$PREFIX/include/SDL2/SDL_config.h
grep -q '#define SDL_VIDEO_DRIVER_X11 1' "$CFG" \
    || { echo "FAIL: built without the X11 video driver." >&2; exit 1; }
if grep -q '#define SDL_VIDEO_DRIVER_WAYLAND 1' "$CFG"; then
    echo "    video drivers: X11 and Wayland, both dlopened at run time"
else
    echo "    NOTE: built WITHOUT the Wayland driver (X11 only)"
fi

echo "==> result"
printf '    %-30s %8s KB\n' "$(basename "$SO")" "$(( $(stat -Lc%s "$SO") / 1024 ))"
deps=$(ldd "$SO" | grep -c '=>' || true)
echo "    links $deps shared objects (sdl2-compat + SDL3 was the alternative)"
ldd "$SO" | sed 's/^\t/      /'
echo "    no libSDL3 reference, by DT_NEEDED or by dlopen"
echo "    prefix libdir: $LIBDIR"
echo "    source: $WORK/$TARBALL"
sha256sum "$WORK/$TARBALL" | sed 's/^/    /'
echo "OK: $PREFIX"
