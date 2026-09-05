#!/bin/bash
# Black gas-station rooftop (part 93/94) — isolate WHICH shading term of
# ps_4796cb371c33e1e2 collapses to the flat dark floor. Recompiles that ONE shader into
# assets/shader_spv_roofdiag with a chosen diagnostic output, then launches a normal play
# session (SAFE=god mode so the shot can be lined up in the crowd) pointed at that cache.
# Everything else in the cache is the stock clip_a2m, so only the rooftop material changes.
#
# The shader outputs, per MODE:
#   1  albedo.rgb            -> is there TEXTURE DETAIL? flat = broken UVs / texture decode;
#                              detail = the sample is fine and the bug is in the lighting.
#   2  albedo.alpha (grey)   -> the final diffuse is multiplied by this; 0 = kills the surface.
#   3  lighting term r6 rgb  -> the normal/light result before albedo; black = lighting collapse.
#   4  shadow visibility     -> PCF result 0..1; black = fully in shadow.
#   5  solid green           -> sanity: does this draw actually paint the black pixels?
#
# Drive to the gas-station rooftop, stand on the black floor, press F9. Quit. Read the
# capture: the floor's colour names the failing term. Repeat with a different MODE as needed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${MODE:-1}"
XENOS=~/GithubRepo/XenosRecomp
DXC="$XENOS/thirdparty/dxc-bin/bin/x64/dxc-linux"
DXCLIB="$XENOS/thirdparty/dxc-bin/lib/x64"
n=ps_4796cb371c33e1e2
tag=$(( 0x371c33e1e2 & 0xFFFF ))
DIAG="$ROOT/assets/shader_spv_roofdiag"

[ -d "$DIAG" ] || cp -r "$ROOT/assets/shader_spv_clip_a2m" "$DIAG"
if [ ! -f /tmp/roofdiag.hlsl ]; then
  python3 "$ROOT/tools/patch_rooftop_diag_hlsl.py" /tmp/rooftop_synth/.hlsl /tmp/roofdiag.hlsl || exit 1
fi

echo ">>> compiling $n with XE_ROOF_DIAG=$MODE"
LD_LIBRARY_PATH="$DXCLIB" "$DXC" -T ps_6_0 -HV 2021 \
    -all-resources-bound -spirv -fvk-use-dx-layout -Qstrip_debug \
    -D "XE_SHADER_TAG=$tag" -D "XE_ROOF_DIAG=$MODE" \
    -Fo "$DIAG/$n.spv" /tmp/roofdiag.hlsl || { echo "DXC FAILED"; exit 1; }

case "$MODE" in
 1) echo "MODE 1: floor shows ALBEDO.rgb  -> detail=texture OK (lighting bug) / flat=UV bug";;
 2) echo "MODE 2: floor shows ALBEDO.ALPHA (grey) -> black means alpha kills the diffuse";;
 3) echo "MODE 3: floor shows LIGHTING term -> black means the light/normal math collapsed";;
 4) echo "MODE 4: floor shows SHADOW visibility -> black means fully shadowed";;
 5) echo "MODE 5: floor shows SOLID GREEN -> if NOT green, this draw isn't painting it";;
esac
echo ">>> launching. Drive to the gas-station rooftop, stand on the black floor, press F9, then quit."

SAFE=1 PLAIN=1 TAG="roofdiag_m${MODE}" "$ROOT/tools/play_session.sh" \
    "CZ_SHADER_SPV=$DIAG" CZ_VK_A2M_ANY_SURFACE=1 CZ_VK_A2M_MODE=1
