#!/usr/bin/env bash
# Build tools/big_decompress (see its header comment for why it exists).
#
# Links the already-built XenonUtils static library rather than vendoring an LZX decoder,
# for the same reason build_xex_image_dump.sh does: the decompressor that unpacks this
# title's assets should be the SAME one that unpacks its executable. A second
# implementation is a second thing that can be subtly wrong, and there is no oracle for
# "these bytes are right" except the data. If XenonRecomp is rebuilt, rebuild this too.
set -euo pipefail

XR="${XENONRECOMP_ROOT:-$HOME/GithubRepo/XenonRecomp}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -f "$XR/build/XenonUtils/libXenonUtils.a" ]; then
    echo "XenonUtils not built at $XR/build -- build XenonRecomp first" >&2
    exit 1
fi

g++ -O2 -std=c++20 -o "$HERE/big_decompress" "$HERE/big_decompress.cpp" \
    -I"$XR/XenonUtils" \
    -I"$XR/thirdparty/simde" \
    -I"$XR/thirdparty/fmt/include" \
    "$XR/build/XenonUtils/libXenonUtils.a" \
    "$XR/build/thirdparty/disasm/libdisasm.a" \
    "$XR/build/thirdparty/fmt/libfmt.a"

echo "built $HERE/big_decompress"
