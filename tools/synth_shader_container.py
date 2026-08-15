#!/usr/bin/env python3
"""Wrap bare Xenos ucode in a synthesized D3D shader container for XenosRecomp.

Fable2 ships shaders inside Lionhead 'ShaderBankFile' banks, not the 0x102A11xx
D3D containers XenosRecomp parses — and our FABLE2_SHADER_DUMP captures are the
bare microcode. This tool fabricates the minimal container around each dump:

  ShaderContainer header      flags 0x102A1100|isVS, table offsets, sizes
  ConstantTableContainer      one full-range float4 array ('vc'/'pc') so every
                              ALU constant reference resolves, plus one sampler
                              entry per tfetch fetch-constant used by the ucode
                              (the sidecar also records each of those slots'
                              texture DIMENSION — see tfetch_dims below, and
                              docs/open-items.md item 00 for what its absence cost)
  Shader / VertexShader /     interpolators + (for VS) vertex elements keyed by
  PixelShader structs         the VFETCH instruction slot address — exactly how
                              shader_recompiler.cpp keys its decl lookups
  (definition table omitted:  offset 0 is legal and skipped)

Usage assignment is positional: the first VFETCH is POSITION0, later ones are
TEXCOORD0..N; VS interpolator exports o0..oN become TEXCOORD0..N and the PS
declares the same list, so linkage is consistent within a synthesized pair.

Known fidelity gap (fine for compilation, revisit at runtime): PS constants at
register >= 224 clamp to 0 through XenosRecomp's tail-clamp macro.

Usage: synth_shader_container.py <ucode_dir> <out_dir>
"""
import glob
import json
import os
import struct
import sys

POSITION, TEXCOORD, COLOR = 0, 5, 10

# XenosRecomp's TextureDimension, in its order. Only for the log line — the sidecar
# carries the raw number, so the runtime and the translator agree on an integer rather
# than on a spelling.
DIM_NAME = {0: "1D", 1: "2D", 2: "3D", 3: "Cube"}

# XenosRecomp USAGE_LOCATIONS: the Vulkan input locations its HLSL assigns.
# TEXCOORD4..7 -> 12..15 and 8..15 -> 16..23 (world shaders take up to 12
# streams); keep in sync with shader_recompiler.cpp's table.
USAGE_LOCATION = {(POSITION, 0): 0, (TEXCOORD, 0): 4, (TEXCOORD, 1): 5,
                  (TEXCOORD, 2): 6, (TEXCOORD, 3): 7, (COLOR, 0): 8}
for _i in range(4, 8):
    USAGE_LOCATION[(TEXCOORD, _i)] = 8 + _i      # 12..15
for _i in range(8, 24):
    USAGE_LOCATION[(TEXCOORD, _i)] = 8 + _i      # 16..31


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def parse_ucode(data):
    """Return (vfetch_slots, tfetch_consts, tfetch_dims, ps_inputs, vs_exports)."""
    n = len(data) // 4
    dw = struct.unpack(f">{n}I", data[: n * 4])

    # Walk the whole CF region (don't stop at the first ExecEnd — conditional
    # flow puts more Exec blocks after it); the region ends where the lowest
    # exec target begins.
    execs = []
    cf_end = n
    i = 0
    while i + 2 < cf_end:
        for (w0, w1) in ((dw[i], dw[i + 1] & 0xFFFF),
                         (((dw[i + 1] >> 16) | (dw[i + 2] << 16)) & 0xFFFFFFFF,
                          dw[i + 2] >> 16)):
            op = bits(w1, 12, 4)
            addr, cnt, seq = bits(w0, 0, 12), bits(w0, 12, 3), bits(w0, 16, 12)
            if op in (1, 2, 3, 4, 5, 6, 13, 14) and cnt:  # Exec family
                if (addr, cnt, seq) not in execs:
                    execs.append((addr, cnt, seq))
                cf_end = min(cf_end, addr * 3)
        i += 3

    vfetch_slots = []   # instruction slot addresses, in program order
    tfetch_consts = []  # unique fetch-constant indices, in program order
    # WHICH DESCRIPTOR ARRAY EACH OF THOSE SLOTS IS SAMPLED FROM, and it is only the
    # SHADER that knows. The translated HLSL declares five register spaces --
    # Texture2D[] in space0, Texture3D[] in space1, TextureCube[] in space2,
    # Sampler[] in space3, Texture1D[] in space4 -- and reads the descriptor index for a
    # fetch out of the shared constants at +0/+64/+128/+288 according to the DIMENSION
    # the fetch instruction declares. A runtime that publishes only the Texture2D array
    # therefore leaves every cube fetch reading index 0, which is the 1x1 dummy: 91 of
    # this title's 395 shaders sample a cube map and every one of them multiplied its
    # specular by pure white for the whole of phase 5 (docs/open-items.md item 00).
    #
    # PER SLOT, NOT PER SHADER. A single pixel shader here samples slot 3 as a cube and
    # slots 0 and 2 as 2D in three consecutive instructions, so a per-module flag would
    # be wrong for two thirds of it.
    tfetch_dims = {}    # fetch-constant index -> 0=1D, 1=2D, 2=3D, 3=cube
    # PER-COMPONENT write tracking (part 45). The previous form of this analysis
    # kept one flat `written` set of REGISTER NUMBERS, so a register whose FIRST
    # touch was a partial write stopped counting as an interpolator input even
    # though its other components are read later. The menu GAS ball's pixel shader
    # is the canonical victim: instruction 10 writes r0.zw (a tfetch destination
    # swizzle keeps .xy), instruction 14 writes r3.zw, instruction 16 writes r2.w —
    # and the shader then samples its DIFFUSE at r0.xy, its spec mask at r2.xy and
    # its decal atlas at r3.xy. The flat analysis dropped interpolants 0/2/3, the
    # translated HLSL zero-initialised those registers, and every such surface
    # sampled ONE TEXEL (UV 0,0) forever — the white-surface class of open items
    # 00f/00g, prosecuted through seven refuted input hypotheses in parts 26-27
    # while every input really was correct: the defect was this tool's liveness.
    # A register is an input iff SOME component is read before THAT COMPONENT is
    # definitely written; predicated writes do not count as definite, because the
    # not-taken path leaves the interpolant visible (hardware seeds the register
    # from the param cache either way).
    written = {}        # reg -> set of components (0..3) definitely written
    # INDIRECT VERTEX FETCH (docs/world-era-fatal-and-selector.md §3s).
    # A vfetch's address is a REGISTER, not necessarily the vertex index.
    # XenosRecomp seeds r0 with the vertex id, so a fetch is "direct" (a real
    # per-vertex attribute) only while its source register still holds that id.
    # Fable2's skinned meshes fetch four bone INDICES from the per-vertex stream
    # and then address a 32-entry matrix palette with them -- three rows per
    # bone, four bones selected by srcSwizzle. Translating those as per-vertex
    # attributes reads the palette at gl_VertexIndex, which is the world's
    # garbage geometry. Record enough for the runtime to gather correctly.
    vid_regs = {0}      # registers currently known to hold the vertex id
    reg_producer = {}   # register -> index into vfetch_slots that last wrote it
    inputs = set()      # regs with a component read before written (PS interp inputs)
    exports = set()     # VS interpolator export indices (o0..o15)

    def note_read(reg, comps=(0, 1, 2, 3)):
        if reg in inputs:
            return
        have = written.get(reg, ())
        for c in comps:
            if c not in have:
                inputs.add(reg)
                return

    def note_write(reg, comps):
        written.setdefault(reg, set()).update(comps)

    for (addr, cnt, seq) in sorted(execs):
        for k in range(cnt):
            base = (addr + k) * 3
            w0, w1, w2 = dw[base], dw[base + 1], dw[base + 2]
            if (seq >> (k * 2)) & 1:  # fetch
                fop = bits(w0, 0, 5)
                # Source-address read, per component: a vfetch address is ONE
                # component (its 2-bit srcSwizzle names it); a tfetch reads as many
                # coordinate components as its declared dimension (2D reads .xy —
                # its 6-bit srcSwizzle is 2 bits per coordinate).
                if fop == 0:
                    note_read(bits(w0, 5, 6), (bits(w0, 30, 2),))
                elif fop == 1:
                    ncomp = {0: 1, 1: 2, 2: 3, 3: 3}[bits(w2, 14, 2)]
                    swz = bits(w0, 26, 6)
                    note_read(bits(w0, 5, 6),
                              tuple((swz >> (2 * c)) & 3 for c in range(ncomp)))
                else:
                    note_read(bits(w0, 5, 6))
                if fop == 0:
                    # VertexFetchInstruction: constIndex 20..24 + constIndexSelect
                    # 25..26 pick the vertex-buffer slot; w1 has format/signed/num,
                    # w2 has stride (dwords) + offset (dwords).
                    vf = {
                        "slot": addr + k,
                        "fetchSlot": bits(w0, 20, 5) * 3 + bits(w0, 25, 2),
                        "format": bits(w1, 16, 6),
                        "signed": bits(w1, 12, 1),
                        "integer": bits(w1, 13, 1),
                        "strideDwords": bits(w2, 0, 8),
                        "offsetDwords": bits(w2, 8, 23),
                    }
                    # mini-fetch: shares the previous full fetch's buffer + stride
                    if vf["strideDwords"] == 0 and vfetch_slots:
                        vf["strideDwords"] = vfetch_slots[-1]["strideDwords"]
                        vf["fetchSlot"] = vfetch_slots[-1]["fetchSlot"]
                    src = bits(w0, 5, 6)
                    vf["srcReg"] = src
                    vf["srcSwz"] = bits(w0, 30, 2)
                    # int, not bool: the runtime's meta reader is numeric-only
                    # and would silently read `true` as 0.
                    vf["indirect"] = int(src not in vid_regs)
                    # Which earlier vfetch produced the address. -1 means the
                    # address came from ALU work we do not model; the runtime
                    # must skip the gather rather than guess, and counts it.
                    vf["addrAttr"] = -1 if not vf["indirect"] else \
                        reg_producer.get(src, -1)
                    vfetch_slots.append(vf)
                    reg_producer[bits(w0, 12, 6)] = len(vfetch_slots) - 1
                elif fop == 1:
                    c = bits(w0, 20, 5)
                    if c not in tfetch_consts:
                        tfetch_consts.append(c)
                    # TextureFetchInstruction word 2 is
                    # `useRegGradients:1, sampleLocation:1, lodBias:7, pad:5,
                    #  dimension:2, offsetX:5, offsetY:5, offsetZ:5, predCondition:1`,
                    # so the dimension starts at bit 14. Taken from XenosRecomp's own
                    # shader_code.h rather than re-derived, because it is the struct the
                    # translator itself switches on when it picks tfetch2D vs tfetchCube
                    # — the two ends cannot disagree if they read the same field.
                    tfetch_dims[c] = bits(w2, 14, 2)
                # Any write kills the vertex id in that register -- including a
                # fetch writing its own source (seen: `dst=r2 src=r2`).
                vid_regs.discard(bits(w0, 12, 6))
                # The fetch destination swizzle is 3 bits per component; 7 = Keep,
                # which leaves that component of the register UNTOUCHED (that is
                # exactly how the ball shader's `tfetch2D r0.__xy` preserves the
                # interpolant in r0.xy). 0..3 write a fetched component, 4/5 write
                # the constants 0/1 — all of those DO write. A predicated fetch is
                # not a definite write.
                if not bits(w1, 31, 1):
                    dswz = bits(w1, 0, 12)
                    note_write(bits(w0, 12, 6),
                               [c for c in range(4) if ((dswz >> (3 * c)) & 7) != 7])
            else:  # ALU
                vdst = bits(w0, 0, 6)
                sdst = bits(w0, 8, 6)
                export = bits(w0, 15, 1)
                vmask = bits(w0, 16, 4)
                smask = bits(w0, 20, 4)
                predicated = bits(w1, 28, 1)
                vop = bits(w2, 24, 5)
                if not export:
                    vid_regs.discard(vdst)
                    reg_producer.pop(vdst, None)
                # Vector source reads, precise through the swizzle: operation lane i
                # reads source component ((swz >> 2i) + i) & 3, over the lanes the
                # opcode consumes — the dot family (and cube/max4/setp-push/kill)
                # reads fixed lane counts regardless of the write mask, everything
                # else reads exactly the masked lanes (XenosRecomp itself falls back
                # to .x on an empty mask). All transcribed from shader_recompiler.cpp's
                # operand printer so the two ends cannot disagree.
                if vop in (15, 18, 19, 28) or 20 <= vop <= 27:  # Dp4/Cube/Max4/Dst/Setp*/Kill*
                    lanes = (0, 1, 2, 3)
                elif vop == 16:                                 # Dp3
                    lanes = (0, 1, 2)
                elif vop == 17:                                 # Dp2Add (src3 reads .x only)
                    lanes = (0, 1)
                else:
                    lanes = tuple(i for i in range(4) if (vmask >> i) & 1) or (0,)
                swz1, swz2, swz3 = bits(w1, 16, 8), bits(w1, 8, 8), bits(w1, 0, 8)
                lane_comps = lambda swz, ls: tuple((((swz >> (2 * i)) + i) & 3)
                                                   for i in ls)
                if bits(w2, 31, 1):
                    note_read(bits(w2, 16, 8) & 0x3F, lane_comps(swz1, lanes))
                if bits(w2, 30, 1):
                    note_read(bits(w2, 8, 8) & 0x3F, lane_comps(swz2, lanes))
                if bits(w2, 29, 1):
                    # src3 is shared: the VECTOR op reads it only for the 3-source
                    # opcodes (Mad, the Cnd family, Dp2Add — lane .x there); the
                    # SCALAR co-issue reads operand A at component ((swz>>6)+3)&3
                    # and operand B at (swz&3), and is present exactly when the
                    # opcode is not RetainPrev (50) — the idle-slot marker
                    # XenosRecomp itself gates emission on. Adds is opcode 0, so
                    # "nonzero opcode" would drop a real adds feeding a *_prev.
                    comps = ()
                    if vop in (11, 12, 13, 14):                 # Mad, CndEq/Ge/Gt
                        comps += lane_comps(swz3, lanes)
                    elif vop == 17:
                        comps += (((swz3 >> 0) + 0) & 3,)
                    if bits(w0, 26, 6) != 50:                   # != RetainPrev
                        comps += (((swz3 >> 6) + 3) & 3, swz3 & 3)
                    if comps:
                        note_read(bits(w2, 0, 8) & 0x3F, comps)
                if export:
                    # both pipes write the export register named by vectorDest;
                    # scalarDest is not a second export index
                    exports.add(vdst)
                elif not predicated:
                    note_write(vdst, [i for i in range(4) if (vmask >> i) & 1])
                    if smask:
                        note_write(sdst, [i for i in range(4) if (smask >> i) & 1])

    return vfetch_slots, tfetch_consts, tfetch_dims, inputs, exports


class Blob:
    def __init__(self):
        self.b = bytearray()

    def u32(self, *vals):
        for v in vals:
            self.b += struct.pack(">I", v & 0xFFFFFFFF)

    def u16(self, *vals):
        for v in vals:
            self.b += struct.pack(">H", v & 0xFFFF)

    def raw(self, data):
        self.b += data

    def pad4(self):
        while len(self.b) % 4:
            self.b += b"\0"

    def __len__(self):
        return len(self.b)


def build_ctab(is_vs, tfetch_consts):
    """D3DX constant table; internal offsets relative to the ConstantTable start."""
    consts = [("vc" if is_vs else "pc", 2, 0, 256)]           # Float4 full range
    consts += [(f"s{c}", 3, c, 1) for c in sorted(tfetch_consts)]  # Samplers

    n = len(consts)
    table_size = 7 * 4
    info_off = table_size
    typeinfo_off = info_off + n * 0x14
    names_off = typeinfo_off + n * 0x10

    names = bytearray()
    name_offs = []
    for (name, _, _, _) in consts:
        name_offs.append(names_off + len(names))
        names += name.encode() + b"\0"
    while len(names) % 4:
        names += b"\0"

    ct = Blob()
    total = names_off + len(names)
    # ConstantTable {size, creator, version, constants, constantInfo, flags, target}
    ct.u32(table_size, 0, 0, n, info_off, 0, 0)
    for i, (name, regset, regidx, regcount) in enumerate(consts):
        # ConstantInfo {name, registerSet:16|registerIndex? -- fields are u16s}
        ct.u32(name_offs[i])
        ct.u16(regset, regidx, regcount, 0)
        ct.u32(typeinfo_off + i * 0x10, 0)  # typeInfo, defaultValue
    for (name, regset, regidx, regcount) in consts:
        if regset == 2:
            ct.u16(1, 3, 1, 4, regcount, 0)  # Vector, Float, 1x4, elements
        else:
            ct.u16(4, 21 if regset == 3 else 3, 1, 1, 1, 0)  # Object, Sampler2D
        ct.u32(0)
    ct.raw(bytes(names))
    assert len(ct) == total, (len(ct), total)

    out = Blob()
    out.u32(total + 4)  # ConstantTableContainer.size
    out.raw(bytes(ct.b))
    return bytes(out.b)


def build_shader_struct(is_vs, ucode_len, vfetch_slots, inputs, exports):
    sh = Blob()
    if is_vs:
        interps = sorted(e for e in exports if e < 16)
        # DEPENDENT fetches (indirect=1: the address register does not hold the
        # auto vertex index) are NOT declared as vertex elements since §3z.
        # XenosRecomp emits an undeclared vfetch as an in-shader raw load from
        # the stream (XeVfetchDep), which is the only correct translation for a
        # chained/lane-composed address. (usage, index) keeps the ORIGINAL
        # enumeration so the declared elements' input locations are unchanged.
        declared = [(i, vf) for i, vf in enumerate(vfetch_slots)
                    if not vf["indirect"]]
        # Shader {physicalOffset, size, field8, fieldC, field10, interpolatorInfo}
        sh.u32(0, ucode_len, 0, 0, 0, len(interps) << 5)
        # VertexShader {field18, vertexElementCount, field20, elements+interps}
        sh.u32(0, len(declared), 0)
        for i, vf in declared:
            usage, index = (POSITION, 0) if i == 0 else (TEXCOORD, i - 1)
            sh.u32(vf["slot"] | (usage << 12) | (index << 16))
        for i in interps:
            sh.u32(i | (TEXCOORD << 4) | (i << 8))
    else:
        interps = sorted(r for r in inputs if r < 16)
        sh.u32(0, ucode_len, 0, 31 << 8, 0, len(interps) << 5)
        # PixelShader {field18, outputs, interpolators}: color exports 0..3 map to
        # their bits, ucode export dst 61 = depth.
        outputs = 0
        for e in exports:
            if e < 4:
                outputs |= 1 << e
            elif e == 61:
                outputs |= 0x10
        sh.u32(0, outputs or 0x1)
        for r in interps:
            sh.u32(r | (TEXCOORD << 4) | (r << 8))
    return bytes(sh.b)


def synthesize(path, out_dir):
    name = os.path.basename(path).replace(".ucode", "")
    is_vs = name.startswith("vs_")
    ucode = open(path, "rb").read()
    vfetch_slots, tfetch_consts, tfetch_dims, inputs, exports = parse_ucode(ucode)

    ctab = build_ctab(is_vs, tfetch_consts)
    shader = build_shader_struct(is_vs, len(ucode), vfetch_slots, inputs, exports)

    header_size = 0x24
    ctab_off = header_size
    shader_off = ctab_off + len(ctab)
    virtual_size = shader_off + len(shader)

    out = Blob()
    # ShaderContainer {flags, virtualSize, physicalSize, fieldC, ctabOff,
    #                  defTableOff, shaderOff, field1C, field20}
    out.u32(0x102A1100 | (1 if is_vs else 0), virtual_size, len(ucode), 0,
            ctab_off, 0, shader_off, 0, 0)
    out.raw(ctab)
    out.raw(shader)
    assert len(out) == virtual_size
    out.raw(ucode)

    # Runtime sidecar: everything vk_renderer needs to bind this shader — vertex
    # attributes with their Vulkan input locations, and the tfetch constants.
    #
    # Built BEFORE the .xshd is written, because it is the step that can fail
    # (an unmapped usage/location raises KeyError). Writing the container first
    # left an orphan .xshd behind on failure, build_shader_spv.sh translated it
    # into a perfectly good .spv with no .meta.json beside it, and the runtime
    # then dropped that .spv SILENTLY at load — 25,364 draws per run skipped as
    # "unknown vs" for a shader sitting in the cache. Never emit the artifact
    # before the step that can fail.
    # `tfetchDims` is written POSITIONALLY against the sorted `tfetchConsts`, because
    # the runtime's sidecar reader is a flat integer-array reader and a parallel array
    # is the one shape it can already express. The two lists therefore have to be the
    # same length and in the same order, which is asserted rather than assumed.
    meta = {"kind": "vs" if is_vs else "ps", "tfetchConsts": sorted(tfetch_consts),
            "tfetchDims": [tfetch_dims[c] for c in sorted(tfetch_consts)]}
    if is_vs:
        attrs = []
        for i, vf in enumerate(vfetch_slots):
            usage, index = (POSITION, 0) if i == 0 else (TEXCOORD, i - 1)
            # A dependent fetch is not a vertex input since §3z: the shader
            # reads the stream itself (XeVfetchDep), so it has no location and
            # the runtime must instead publish the WHOLE stream for its fetch
            # slot in the per-draw table at SharedConstants+544. bufferRead is
            # the runtime's key for that; location -1 would KeyError the usage
            # table anyway for high texcoord indices.
            attrs.append({"location": -1 if vf["indirect"]
                          else USAGE_LOCATION[(usage, index)],
                          "bufferRead": vf["indirect"], **{
                k: vf[k] for k in ("fetchSlot", "format", "signed", "integer",
                                   "strideDwords", "offsetDwords",
                                   "srcReg", "srcSwz", "indirect", "addrAttr")}})
        meta["attributes"] = attrs
        meta["interpolators"] = sorted(e for e in exports if e < 16)
    else:
        meta["interpolators"] = sorted(r for r in inputs if r < 16)

    open(os.path.join(out_dir, name + ".xshd"), "wb").write(bytes(out.b))
    open(os.path.join(out_dir, name + ".meta.json"), "w").write(
        json.dumps(meta, indent=1) + "\n")

    kind = "VS" if is_vs else "PS"
    print(f"{name}: {kind} ucode={len(ucode)}B "
          f"vfetch={[(v['slot'], v['fetchSlot'], v['format']) for v in vfetch_slots]} "
          f"tfetch={[(c, DIM_NAME[tfetch_dims[c]]) for c in tfetch_consts]} "
          f"psin={sorted(inputs) if not is_vs else '-'} "
          f"vsout={sorted(exports) if is_vs else '-'} -> "
          f"{os.path.join(out_dir, name + '.xshd')}")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    os.makedirs(sys.argv[2], exist_ok=True)
    for f in sorted(glob.glob(os.path.join(sys.argv[1], "*.ucode"))):
        # A dump is only as good as the IM_LOAD that produced it: a mis-sized
        # load (we have seen a 224 KB "vertex shader") yields bytes that are not
        # microcode at all, and the control-flow walk runs off the end. Skip
        # those loudly instead of aborting the batch -- one bad dump must not
        # cost every other shader in the directory.
        try:
            synthesize(f, sys.argv[2])
        except Exception as e:
            print(f"{os.path.basename(f)}: SKIPPED, not parseable as ucode ({e})")


if __name__ == "__main__":
    main()
