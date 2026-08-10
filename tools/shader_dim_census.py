#!/usr/bin/env python3
"""Census the shader cache's texture DIMENSIONS, and cross-check the two derivations.

WHY THIS EXISTS
---------------
Phase C part 23 found that 91 of this port's 395 translated shaders sample a cube map
and every one of them was reading descriptor index 0 -- the 1x1 white dummy -- on every
draw, since phase 5. The renderer published only the `Texture2D` descriptor-index array
into the shared constants, and it could not do better, because the shader metadata was a
flat list of fetch-constant slots with no dimension in it (docs/open-items.md item 00).

Part 25 put the dimension in the sidecar. This tool is the gate on that, and it is a
gate rather than a report because the dimension is derivable TWICE, independently:

  * from OUR ucode parse -- `tfetchDims` in the .meta.json, taken from bits 14..15 of
    the texture-fetch instruction's third word;
  * from DXC's OUTPUT -- the .spv's `OpDecorate <id> DescriptorSet n` words, where the
    translated HLSL binds Texture2D[] to space0, Texture3D[] to space1, TextureCube[] to
    space2 and Texture1D[] to space4.

The second is the translator's own reading of the same instruction, arrived at through
XenosRecomp and DXC without passing through any code of ours. If the two disagree, our
bit position is wrong -- which is the one thing a single-sided census could never say,
and exactly the failure this project keeps meeting (gotcha 3: a zero is a detection
failure, not a fact).

A shader whose sidecar has NO `tfetchDims` at all is reported separately and is not a
mismatch: it is a cache entry built before part 25 whose microcode we no longer hold,
so the runtime falls back to 2D for it and counts that. `/tmp` is a tmpfs here, so the
dump directory a cache was built from does not survive a reboot -- keep dumps in
~/DR2CZ-troubleshooting/ucode-dumps and rebuild the cache from there.

Exit 1 on any disagreement between the two derivations.

Usage: shader_dim_census.py [shader_spv_dir]
"""
import glob
import json
import os
import struct
import sys

DIM_NAME = {0: "1D", 1: "2D", 2: "3D", 3: "Cube"}
# HLSL register space -> the dimension whose descriptor heap lives there. space3 is the
# sampler heap and is present in every shader that samples anything, so it says nothing
# about dimensions and is deliberately absent from this table.
SPACE_DIM = {0: 1, 1: 2, 2: 3, 4: 0}


def spirv_spaces(path):
    """The descriptor SETS a SPIR-V module decorates, from its OpDecorate words."""
    b = open(path, "rb").read()
    if len(b) < 20 or struct.unpack("<I", b[:4])[0] != 0x07230203:
        raise ValueError(f"{path}: not a little-endian SPIR-V module")
    w = struct.unpack(f"<{len(b) // 4}I", b[: len(b) // 4 * 4])
    out = set()
    i = 5  # past the five-word header
    while i < len(w):
        op, count = w[i] & 0xFFFF, w[i] >> 16
        if count == 0:  # a malformed module would otherwise spin here forever
            break
        if op == 71 and i + 3 < len(w) and w[i + 2] == 34:  # OpDecorate, DescriptorSet
            out.add(w[i + 3])
        i += count
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "assets", "shader_spv")
    slots = {}          # dimension -> declared fetch slots across the whole bank
    per_module = {}     # dimension -> modules declaring at least one
    no_dims = []
    mismatch = []
    for spv in sorted(glob.glob(os.path.join(root, "*.spv"))):
        name = os.path.basename(spv)[:-4]
        meta_path = os.path.join(root, name + ".meta.json")
        if not os.path.exists(meta_path):
            print(f"ORPHAN .spv with no sidecar: {name}")
            mismatch.append(name)
            continue
        meta = json.load(open(meta_path))
        consts = meta.get("tfetchConsts", [])
        dims = meta.get("tfetchDims")
        if dims is None:
            no_dims.append(name)
            continue
        if len(dims) != len(consts):
            print(f"{name}: tfetchDims has {len(dims)} entries for "
                  f"{len(consts)} tfetchConsts -- the arrays are POSITIONAL")
            mismatch.append(name)
            continue
        for d in dims:
            slots[d] = slots.get(d, 0) + 1
        # The two derivations, compared as SETS of dimensions rather than per slot:
        # the .spv records which heaps a module reads, not which slot reads which.
        ours = {d for d in dims}
        theirs = {SPACE_DIM[s] for s in spirv_spaces(spv) if s in SPACE_DIM}
        for d in ours:
            per_module[d] = per_module.get(d, 0) + 1
        if ours != theirs:
            print(f"{name}: our ucode parse says "
                  f"{sorted(DIM_NAME[d] for d in ours)}, the translated SPIR-V says "
                  f"{sorted(DIM_NAME[d] for d in theirs)}")
            mismatch.append(name)

    total = len(glob.glob(os.path.join(root, "*.spv")))
    print(f"{total} shaders in {os.path.normpath(root)}")
    for d in sorted(set(slots) | set(per_module)):
        print(f"  {DIM_NAME[d]:>4}: {per_module.get(d, 0):4d} modules, "
              f"{slots.get(d, 0):5d} declared fetch slots")
    if no_dims:
        print(f"  {len(no_dims)} sidecars carry NO tfetchDims (pre-part-25 entries "
              f"whose microcode we no longer hold; the runtime treats these as 2D "
              f"and counts them):")
        for n in no_dims:
            extra = "  <-- samples a CUBE map, so this one IS still unbound" \
                if 2 in spirv_spaces(os.path.join(root, n + ".spv")) else ""
            print(f"      {n}{extra}")
    if mismatch:
        print(f"MISMATCH: {len(mismatch)} shaders -- the two derivations disagree")
        return 1
    print("the ucode parse and the translated SPIR-V agree on every shader")
    return 0


if __name__ == "__main__":
    sys.exit(main())
