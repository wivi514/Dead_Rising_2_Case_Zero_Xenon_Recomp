#!/usr/bin/env bash
# Build the ONE ffmpeg this port actually uses, under LGPL, for shipping.
#
# WHY THIS EXISTS (docs/release-plan.md §4 and A.4). Two separate problems with the
# system ffmpeg, either of which alone would block a release:
#
#   1. LICENSING. Fedora's ffmpeg is configured --enable-gpl. This repo is PolyForm
#      Noncommercial; bundling a GPL library with it is a conflict, and §4 says so.
#      An LGPL build with no --enable-gpl and no --enable-nonfree is not a formality —
#      it is the difference between a redistributable artifact and one that is not.
#
#   2. SIZE, and it is not a rounding error. `ldd` on the release binary against the
#      system ffmpeg lists 120 shared objects: x264, x265, SvtAv1Enc, vvenc, librsvg,
#      cairo, pango, harfbuzz, OpenCL, VA-API, VDPAU — a whole desktop graphics stack,
#      dragged in because Fedora's ffmpeg enables every codec that exists. This port
#      calls FOURTEEN ffmpeg functions and needs exactly one decoder.
#
# The whole ffmpeg surface the runtime uses, censused over runtime/audio and
# runtime/kernel rather than assumed:
#
#   avcodec_alloc_context3   avcodec_find_decoder   avcodec_free_context
#   avcodec_open2            avcodec_send_packet    avcodec_receive_frame
#   av_frame_alloc           av_frame_free          av_packet_alloc
#   av_packet_free           av_mallocz             av_channel_layout_default
#   av_get_sample_fmt_name   av_sample_fmt_is_planar
#
# libavcodec and libavutil. No avformat (we never demux — the guest hands us raw XMA
# packets out of its own contexts), no swresample (the title's own mixer does the
# mixing; runtime/audio/audio_out.cpp is deliberately dumb), no avfilter, no swscale,
# no avdevice, no programs.
#
# XMA1 is enabled alongside XMA2 even though finding 36 established the title submits
# XMA2. It costs nothing, and a decoder that is absent fails as "unsupported codec" at
# the moment a player reaches whatever asset uses it — the kind of failure that arrives
# late and reads as a game bug (gotcha 5: fail loudly with the identifier, never guess).
#
# VERSION IS PINNED TO THE ONE THIS MACHINE DEVELOPS AGAINST, so the headers the
# release compiles against and the headers every recorded measurement was taken with
# are the same file. A different major version would be a second variable in the one
# artifact nobody re-tests.
#
# Usage:  tools/build_ffmpeg_lgpl.sh [prefix]     (default: thirdparty/ffmpeg-lgpl)
set -euo pipefail

VERSION=8.1.2
PREFIX=${1:-$(cd "$(dirname "$0")/.." && pwd)/thirdparty/ffmpeg-lgpl}
WORK=${CZ_FFMPEG_WORK:-/var/tmp/cz-ffmpeg-build}

mkdir -p "$WORK"
cd "$WORK"

TARBALL=ffmpeg-$VERSION.tar.xz
if [ ! -f "$TARBALL" ]; then
    echo "==> fetching $TARBALL"
    curl -fL --retry 3 -o "$TARBALL.part" "https://ffmpeg.org/releases/$TARBALL"
    mv "$TARBALL.part" "$TARBALL"
fi

SRC=$WORK/ffmpeg-$VERSION
if [ ! -d "$SRC" ]; then
    echo "==> unpacking"
    tar xf "$TARBALL"
fi

BUILD=$WORK/build
rm -rf "$BUILD" && mkdir -p "$BUILD"
cd "$BUILD"

# nasm/yasm, or an honest downgrade. ffmpeg's x86 SIMD is hand-written assembly and
# needs an external assembler; without one configure refuses outright. Falling back to
# --disable-x86asm produces a CORRECT decoder built from C, and the cost lands on audio
# decode only — but it IS a cost, and a bundle that silently shipped a slower decoder
# because the build machine lacked a package is exactly the kind of packaging defect
# that presents later as a game defect. So it is a decision the script makes out loud
# and records in the build, not a fallback it takes quietly.
ASMFLAG=""
if command -v nasm >/dev/null || command -v yasm >/dev/null; then
    echo "==> assembler: $(command -v nasm || command -v yasm)"
else
    ASMFLAG="--disable-x86asm"
    cat >&2 <<'WARN'
==> WARNING: no nasm or yasm on this machine.
    Building ffmpeg WITHOUT its hand-written x86 assembly. The decoder is correct but
    slower, and the cost falls on XMA decode. Install one and re-run before cutting a
    release artifact:
        sudo dnf install nasm        (Fedora)
        sudo apt install nasm        (Debian/Ubuntu)
WARN
fi

echo "==> configuring (LGPL, xma1+xma2 only)"
# --disable-everything turns off every codec/muxer/protocol; the enables below are the
# complete list of what comes back. --disable-autodetect is the load-bearing one: without
# it configure links whatever happens to be installed on the BUILD machine, which is
# exactly how a bundle acquires dependencies nobody chose (and how a GPL-only external
# library could sneak back in).
"$SRC/configure" \
    --prefix="$PREFIX" \
    --enable-shared --disable-static \
    --disable-everything \
    --disable-autodetect \
    --disable-programs --disable-doc \
    --disable-avdevice --disable-avformat --disable-avfilter \
    --disable-swscale --disable-swresample \
    --disable-network --disable-iconv \
    --enable-decoder=xma1,xma2 \
    --disable-gpl --disable-nonfree --disable-version3 $ASMFLAG \
    >"$BUILD/configure.log" 2>&1 || { tail -40 "$BUILD/configure.log"; exit 1; }

# THE LICENCE CHECK, made against configure's OWN output rather than against the flags
# we passed. Passing --disable-gpl and believing it is the same class of mistake as
# believing an arm engaged because its variable was in the description (gotcha 401):
# ffconfig records what was actually built.
if grep -q '^#define CONFIG_GPL 1' config.h; then
    echo "FAIL: this build is GPL. It must not be shipped with a PolyForm repo." >&2
    exit 1
fi
if grep -q '^#define CONFIG_NONFREE 1' config.h; then
    echo "FAIL: this build is non-free and is not redistributable at all." >&2
    exit 1
fi
echo "    licence: CONFIG_GPL=0 CONFIG_NONFREE=0  (LGPL 2.1+)"
if [ -n "$ASMFLAG" ]; then
    echo "    x86 assembly: DISABLED (no nasm/yasm) — see the warning above"
else
    echo "    x86 assembly: enabled"
fi

echo "==> building"
make -j"$(nproc)" >"$BUILD/make.log" 2>&1 || { tail -40 "$BUILD/make.log"; exit 1; }
rm -rf "$PREFIX"
make install >>"$BUILD/make.log" 2>&1

# THE SIZE CHECK, stated as a threshold rather than printed for someone to eyeball.
# The system build's closure is 120 objects. If ours is anywhere near that, the
# --disable-autodetect did not take and the whole point of this script is lost.
echo "==> result"
for so in "$PREFIX"/lib/libavcodec.so.*.* "$PREFIX"/lib/libavutil.so.*.*; do
    [ -f "$so" ] || continue
    printf '    %-28s %8s KB\n' "$(basename "$so")" "$(( $(stat -Lc%s "$so") / 1024 ))"
done

# LD_LIBRARY_PATH matters here, and its absence is why the first version of this check
# reported 22 dependencies for a library that has three. Our libavcodec's DT_NEEDED
# names `libavutil.so.60`; a bare ldd resolves that against /lib64 and then reports the
# SYSTEM libavutil's closure -- every codec Fedora enabled -- as if it were ours. The
# check was measuring the library it was built to replace (gotcha 172: an untrusted path
# is not an oracle, and here the oracle was the wrong file entirely).
deps=$(LD_LIBRARY_PATH="$PREFIX/lib" ldd "$PREFIX"/lib/libavcodec.so | grep -c '=>' || true)
echo "    libavcodec links $deps shared objects (the system ffmpeg's closure is 120)"
LD_LIBRARY_PATH="$PREFIX/lib" ldd "$PREFIX"/lib/libavcodec.so | sed 's/^\t/      /'
if [ "$deps" -gt 12 ]; then
    echo "FAIL: $deps dependencies -- --disable-autodetect did not take." >&2
    exit 1
fi

# The source tarball is kept: the LGPL requires that the exact source corresponding to
# the shipped binary be available, and "we downloaded it from ffmpeg.org" is only an
# answer while that file still exists. E.3 copies its checksum into THIRD_PARTY.md.
echo "    source: $WORK/$TARBALL"
sha256sum "$WORK/$TARBALL" | sed 's/^/    /'
echo "OK: $PREFIX"
