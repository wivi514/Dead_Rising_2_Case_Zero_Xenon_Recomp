#!/bin/bash
# Microcode dumps -> synthesized D3D containers -> XenosRecomp HLSL -> DXC SPIR-V.
# One .spv (+ .meta.json sidecar) per shader, named by the SAME hash the runtime
# computes at IM_LOAD time, so a cache entry and a bound shader agree by construction.
#
# WHY THE INPUT IS OUR OWN DUMP AND NOT XENIA'S
# ---------------------------------------------
# Xenia's `dump_shaders` writes the same shaders (we checked: 120 of our 121 boot-era
# blobs are byte-identical to A1's, modulo dword order), but it writes them with
# Xenia's idea of where the microcode ends. The runtime looks a shader up by hashing
# the bytes its own IM_LOAD handler read, so any disagreement about the length is not a
# wrong picture — it is a total, silent cache miss. Dump from the runtime
# (`CZ_SHADER_DUMP=<dir>`), build the cache from that, and the question cannot arise.
#
# Xenia's dumps stay useful as a cross-check and as free disassembly: the `.ucode.*`
# text files beside them are what a shader actually does, which is worth reading before
# theorising about a wrong-looking draw.
#
# Usage: tools/build_shader_spv.sh <ucode_dir> [out_dir]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UCODE="${1:?usage: build_shader_spv.sh <ucode_dir> [out_dir]}"
OUT="${2:-$ROOT/assets/shader_spv}"
XENOS=~/GithubRepo/XenosRecomp
DXC="$XENOS/thirdparty/dxc-bin/bin/x64/dxc-linux"
DXCLIB="$XENOS/thirdparty/dxc-bin/lib/x64"

SYNTH=$(mktemp -d)
trap 'rm -rf "$SYNTH"' EXIT

python3 "$ROOT/tools/synth_shader_container.py" "$UCODE" "$SYNTH" > "$SYNTH/synth.log" || {
    echo "synth failed; see $SYNTH/synth.log"; exit 1; }
mkdir -p "$OUT"

ok=0
failed=""
for x in "$SYNTH"/*.xshd; do
  n=$(basename "$x" .xshd)
  # A shader that fails to translate must not cost the whole batch. XenosRecomp still
  # has codegen gaps, and a dump directory is normally mostly-good; the failure list at
  # the end is the actionable output. Both artifacts are removed on failure so the
  # runtime can never see a .spv without its sidecar (or the reverse) and silently drop
  # a shader it appears to have.
  if ! "$XENOS/build/XenosRecomp/XenosRecomp" "$x" "$SYNTH/$n.hlsl" \
        "$XENOS/XenosRecomp/shader_common.h" > "$SYNTH/$n.xenos.log" 2>&1; then
    echo "FAILED (XenosRecomp): $n"; failed="$failed $n"; continue
  fi
  case "$n" in vs_*) target="vs_6_0" ;; *) target="ps_6_0" ;; esac
  # No -fvk-invert-y. The Xenos vertex shader emits clip coordinates in D3D
  # convention and the renderer folds the window->NDC mapping itself (see
  # vk_renderer.cpp's viewport note), so adding the invert here would flip the whole
  # frame a second time. Kept as one comment in one place rather than a flag nobody
  # can re-derive: Fable 2 spent a session on exactly this double flip.
  # CZ_DXC_DEFINES="-D NAME=1 ..." — extra preprocessor defines, so an ARM of the shader
  # cache can be built from the same emitter and the same microcode into a second
  # directory, and selected at run time with CZ_SHADER_SPV. That is what makes a shader
  # change a same-binary A/B rather than a rebuild you cannot go back from.
  # Each module learns its OWN identity: the low 16 bits of the hash its filename already
  # carries. Costs nothing when unused (a define no emitted code reads), and it is what
  # lets an instrument paint a colour a decoder can turn back into a shader NAME instead
  # of a human inferring which material a green patch belonged to.
  tag=$(( 0x${n#*_} & 0xFFFF ))
  if LD_LIBRARY_PATH="$DXCLIB" "$DXC" -T "$target" -HV 2021 \
      -all-resources-bound -spirv -fvk-use-dx-layout -Qstrip_debug \
      -D "XE_SHADER_TAG=$tag" ${CZ_DXC_DEFINES:-} \
      -Fo "$OUT/$n.spv" "$SYNTH/$n.hlsl" > "$SYNTH/$n.dxc.log" 2>&1; then
    cp "$SYNTH/$n.meta.json" "$OUT/"
    ok=$((ok + 1))
  else
    echo "FAILED (DXC): $n  ($SYNTH/$n.dxc.log)"
    head -3 "$SYNTH/$n.dxc.log" | sed 's/^/    /'
    failed="$failed $n"
    rm -f "$OUT/$n.spv" "$OUT/$n.meta.json"
  fi
done

echo "translated $ok shaders into $OUT"
[ -n "$failed" ] && { echo "failed:$failed"; exit 0; }
exit 0
