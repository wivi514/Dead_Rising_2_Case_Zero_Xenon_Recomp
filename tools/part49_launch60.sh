#!/bin/bash
# Part 49: launch ONE 60 fps run for the operator, with the guard that part 48 learned
# and part 49 then bypassed by launching directly with nohup.
#
# TWO INSTANCES AT ONCE IS NOT A COSMETIC MISTAKE HERE: the whole point of an operator
# run is a real frame rate on their machine, and two runs sharing the CPU measure
# contention. It happened because a launcher without the guard was used "just this
# once" while iterating -- so the guard lives in the launcher, and the launcher is the
# only way this is started.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/DR2CZ-troubleshooting/part49-operator"
TAG="${1:-cap60}"
shift || true
mkdir -p "$OUT/$TAG"

others=$(pgrep -a . 2>/dev/null | awk '$2 ~ /^cz_runtime/ {print $1" "$2}')
if [ -n "$others" ]; then
    echo "!! REFUSING: a cz_runtime is already running --"
    echo "$others" | sed 's/^/     /'
    echo "   kill its PROCESS GROUP: kill -9 -\$(ps -o pgid= -p <pid>)"
    exit 2
fi

echo "=== $TAG   $*"
( cd "$ROOT/runtime/build" && env \
    CZ_VKDRAW=1 CZ_DEBUG_MENU=1 \
    "CZ_CAPTURE_KEY=$OUT/$TAG" \
    "CZ_SHADER_DUMP=$HOME/DR2CZ-troubleshooting/ucode-dumps" \
    CZ_VK_PROFILE=20 "CZ_VK_FRAME_STATS=$OUT/$TAG.stats" \
    "CZ_SHADER_SPV=$ROOT/assets/shader_spv_a2m" \
    CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 \
    "$@" \
    ./cz_runtime > "$OUT/$TAG.log" 2>&1 )
echo "done -> $OUT/$TAG.log"
