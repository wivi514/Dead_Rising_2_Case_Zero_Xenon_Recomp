#!/bin/bash
# Gas-station rooftop DECK diagnostic (part 96). Tests whether ps_d7182b (the 32x32
# surface part 94 injected gravel into) is COVERING the real deck floor (ps_f20be397,
# a tiled fine gravel that is present and correct in our build). Bakes a patched
# ps_d7182b into a diag cache (a clip_a2m copy, only this ONE shader changed) and
# launches a SAFE play session. Drive to the gas-station rooftop, stand on the deck,
# press F9. Read the shot:
#   MODE=1 (default) d7182b DISCARDED -> if the deck now shows FINE GRAVEL, d7182b was
#          covering f20be397 and the whole thing is a draw-coverage bug. If still smooth/
#          black, d7182b is NOT the cover and the floor draw itself is wrong.
#   MODE=2 d7182b MAGENTA -> shows exactly where d7182b covers the screen.
# The gravel INJECTION is turned OFF here (CZ_VK_GRAVEL_ADDR=0) so nothing masks the result.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${MODE:-1}"
XENOS=~/GithubRepo/XenosRecomp
DXC="$XENOS/thirdparty/dxc-bin/bin/x64/dxc-linux"
DXCLIB="$XENOS/thirdparty/dxc-bin/lib/x64"
n=ps_d7182b2fb8f8c474
tag=$(( 0xb8f8c474 & 0xFFFF ))
DIAG="$ROOT/assets/shader_spv_deckdiag"
SYN=/tmp/pit_synth

# regenerate the shader's HLSL if the /tmp copy was cleared (tmpfs)
if [ ! -f "$SYN/$n.hlsl" ]; then
  mkdir -p /tmp/deckuc "$SYN"
  cp ~/DR2CZ-troubleshooting/ucode-dumps/$n.ucode /tmp/deckuc/ 2>/dev/null
  python3 "$ROOT/tools/synth_shader_container.py" /tmp/deckuc "$SYN" >/dev/null 2>&1
  "$XENOS/build/XenosRecomp/XenosRecomp" "$SYN/$n.xshd" "$SYN/$n.hlsl" \
      "$XENOS/XenosRecomp/shader_common.h" >/dev/null 2>&1
fi

python3 "$ROOT/tools/patch_d7182b_diag_hlsl.py" "$SYN/$n.hlsl" /tmp/d71diag.hlsl || exit 1
[ -d "$DIAG" ] || cp -r "$ROOT/assets/shader_spv_clip_a2m" "$DIAG"

echo ">>> compiling $n with XE_D71_DIAG=$MODE (1=discard, 2=magenta)"
LD_LIBRARY_PATH="$DXCLIB" "$DXC" -T ps_6_0 -HV 2021 \
    -all-resources-bound -spirv -fvk-use-dx-layout -Qstrip_debug \
    -D "XE_SHADER_TAG=$tag" -D "XE_D71_DIAG=$MODE" \
    -Fo "$DIAG/$n.spv" /tmp/d71diag.hlsl || { echo "DXC FAILED"; exit 1; }

case "$MODE" in
 1) echo "MODE 1: d7182b DISCARDED. If the deck shows FINE GRAVEL now, it was the cover.";;
 2) echo "MODE 2: d7182b MAGENTA. Magenta area = where d7182b covers the screen.";;
esac
echo ">>> launching. Drive to the gas-station rooftop, stand on the deck, F9, then quit."

SAFE=1 PLAIN=1 TAG="deckdiag_m${MODE}" "$ROOT/tools/play_session.sh" \
    "CZ_SHADER_SPV=$DIAG" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1 CZ_VK_GRAVEL_ADDR=0
