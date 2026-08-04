#!/usr/bin/env bash
# Build tools/xex_image_dump (see its header comment for why it exists).
#
# It links against the already-built XenonUtils static library rather than vendoring a
# copy, deliberately: the whole point of the tool is that our analysis image comes out
# of the SAME loader the recompiler uses. If XenonRecomp is rebuilt, rebuild this too.
set -euo pipefail

XR="${XENONRECOMP_ROOT:-$HOME/GithubRepo/XenonRecomp}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -f "$XR/build/XenonUtils/libXenonUtils.a" ]; then
    echo "XenonUtils not built at $XR/build -- build XenonRecomp first" >&2
    exit 1
fi

g++ -O2 -std=c++20 -o "$HERE/xex_image_dump" "$HERE/xex_image_dump.cpp" \
    -I"$XR/XenonUtils" \
    -I"$XR/thirdparty/simde" \
    -I"$XR/thirdparty/fmt/include" \
    "$XR/build/XenonUtils/libXenonUtils.a" \
    "$XR/build/thirdparty/disasm/libdisasm.a" \
    "$XR/build/thirdparty/fmt/libfmt.a"

echo "built $HERE/xex_image_dump"
