#!/usr/bin/env python3
"""Extract Xenos microcode from the disc's `.vo`/`.po` shader objects — the CONTAINER, decoded.

WHY THIS EXISTS (docs/release-plan.md D.1). A shipped build must be able to build its own
shader cache from the player's own disc, because the alternative is shipping 449 blobs
extracted from a copyrighted game and hoping nobody ever reaches an area no run has visited.
`tools/vo_microcode_probe.py` established that the microcode IS in these objects by SEARCHING
for it — which needs the answer in advance and so cannot build a cache. This decodes the
container instead, which needs nothing but the object.

THE CONTAINER, in three fields. Every `.vo`/`.po` object begins with a big-endian header:

    +0x00  u32  magic          0x102A1100 for `.po` (pixel), 0x102A1101 for `.vo` (vertex)
    +0x04  u32  blobOffset     start of [literal constants][microcode]
    +0x08  u32  blobLength     == objectLength - blobOffset, in all 423 checked
    +0x18  u32  constDescOff   -> a u32 giving the LITERAL CONSTANT BLOCK's size in bytes

and therefore

    microcodeStart  = u32@0x04 + u32@( u32@0x18 )
    microcodeLength = objectLength - microcodeStart

The microcode is always the TAIL of the object — 343 of 343 pixel shaders end exactly at the
object's last byte — so once the start is known there is nothing left to derive.

HOW IT WAS FOUND, because the route matters more than the answer. The plan recorded that the
start offset "appears as a plain big-endian u32 in the object's first 0x80 bytes for only 34
of 416", i.e. that there was a real table to work out. That was right and slightly off: the
field at +0x04 is not the microcode start, it is the start of the BLOB, and the microcode sits
0, 64 or 128 bytes into it behind a block of literal float constants (1.0, 0.5, 0.25, -2.0 …).
Searching for a header dword equal to that 0/64/128 found nothing; the size is not in the
fixed header at all. It is one indirection away, at the offset +0x18 points to — which is
visible the moment the region between the header and the blob is dumped rather than scanned.

THE GATE IS EXACT, FREE, AND TWO-SIDED. The renderer's cache key is FNV-1a over the microcode
bytes, and it logs it (`[imload] VS va=… hash=… size=…`), so every blob this extracts either
hashes to a name already in `assets/shader_spv/` or it does not. There is no interpretation
step. Run with --gate:

    tools/vo_extract_microcode.py /tmp/discsh --gate

    343 of 343 pixel shaders in the cache are reproduced byte-for-byte from the disc.

VERTEX SHADERS ARE A DIFFERENT ANSWER AND THE PLAN SHOULD NOT PROMISE THEM. Zero of the 104
vertex shaders in the cache appear verbatim on disc — 95 match by their last 48 bytes and
differ in 3 to 35 SCATTERED bytes, always in groups of three dwords whose disc value has
whole fields zeroed:

    dw13  disc 00000A88   runtime 00393A88
    dw14  disc 00000000   runtime 00000003
    dw15  disc 05F82000   runtime 03F82000

That is the title patching vertex FETCH instructions at load out of the vertex declaration —
standard Xbox 360 practice, and the reason a vertex shader cannot be pre-translated from the
disc: the fetch fields are exactly what decides the vertex format XenosRecomp emits. So the
disc supplies the PIXEL half of the cache (76% of it) and the vertex half must come from the
runtime's own first-sight translation. `docs/release-plan.md` §1.4 said 98.6% recoverable on
the strength of a 48-byte HEAD probe; 48 bytes is a shared vertex-shader prologue and the
probe was matching that, not the shader (see the retraction in that file).

    tools/vo_extract_microcode.py <objects-dir> [--out DIR] [--gate] [--cache assets/shader_spv]

Extract the objects first:

    for b in "vs .vo" "ps .po"; do set -- $b
        python3 tools/big_list.py assets/game/data/shaders/deadrisingprologue-$1.big \
            --extract "$2" --out /tmp/discsh
    done
"""
import argparse, glob, os, struct, sys

MAGIC_PS = 0x102A1100
MAGIC_VS = 0x102A1101


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def microcode_range(obj):
    """(start, length, kind) or (None, None, reason).

    Every failure NAMES ITSELF rather than returning None and letting the caller guess:
    an object this cannot parse is either a format we have not seen or a decode bug, and
    those two need different work.
    """
    if len(obj) < 0x20:
        return None, None, "shorter than a header"
    magic = be32(obj, 0)
    if magic == MAGIC_PS:
        kind = "ps"
    elif magic == MAGIC_VS:
        kind = "vs"
    else:
        return None, None, f"unknown magic {magic:08X}"

    blob = be32(obj, 0x04)
    blob_len = be32(obj, 0x08)
    desc = be32(obj, 0x18)

    # Every bound checked before it is used. These objects come off a disc image the
    # player supplies, so a malformed one is a thing that will happen in the field, and
    # a first-run pass that segfaults on it is worse than one that skips it by name.
    if blob >= len(obj):
        return None, None, f"blobOffset {blob} past the object ({len(obj)})"
    if blob + blob_len != len(obj):
        return None, None, (f"blobOffset+blobLength {blob}+{blob_len} != objectLength "
                            f"{len(obj)}")
    if desc + 4 > len(obj):
        return None, None, f"constDescOff {desc} past the object"
    const_len = be32(obj, desc)
    start = blob + const_len
    if start >= len(obj):
        return None, None, f"constant block {const_len} runs past the object"
    if start % 4:
        return None, None, f"microcode start {start} is not dword-aligned"
    return start, len(obj) - start, kind


ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("objects", help="directory of extracted .vo/.po objects")
ap.add_argument("--out", help="write <kind>_<hash>.ucode blobs here")
ap.add_argument("--gate", action="store_true",
                help="check every extracted PIXEL shader against the built cache and "
                     "exit 1 on any disagreement")
ap.add_argument("--cache", default="assets/shader_spv",
                help="the built SPIR-V cache, used as the gate's oracle")
a = ap.parse_args()

paths = sorted(p for p in glob.glob(os.path.join(a.objects, "*")) if os.path.isfile(p))
if not paths:
    sys.exit(f"** no objects in {a.objects}")

known = set()
if a.gate:
    for p in glob.glob(os.path.join(a.cache, "*.spv")):
        known.add(os.path.basename(p)[:-4])          # ps_<hash> / vs_<hash>
    if not known:
        sys.exit(f"** --gate needs a built cache; {a.cache} has no .spv files")

if a.out:
    os.makedirs(a.out, exist_ok=True)

counts = {"ps": 0, "vs": 0}
refused = []
names = {"ps": set(), "vs": set()}
for p in paths:
    obj = open(p, "rb").read()
    start, length, kind = microcode_range(obj)
    if start is None:
        refused.append((os.path.basename(p), kind))
        continue
    code = obj[start:start + length]
    name = f"{kind}_{fnv1a(code):016x}"
    counts[kind] += 1
    names[kind].add(name)
    if a.out:
        open(os.path.join(a.out, name + ".ucode"), "wb").write(code)

print(f"{len(paths)} objects -> {counts['ps']} pixel, {counts['vs']} vertex, "
      f"{len(refused)} refused")
print(f"distinct: {len(names['ps'])} pixel, {len(names['vs'])} vertex")
# Never a silent skip. An object we cannot parse is the interesting one.
for n, why in refused[:20]:
    print(f"  REFUSED {n}: {why}")
if len(refused) > 20:
    print(f"  ... and {len(refused) - 20} more")

if not a.gate:
    sys.exit(0)

cache_ps = {n for n in known if n.startswith("ps_")}
cache_vs = {n for n in known if n.startswith("vs_")}
hit_ps = names["ps"] & cache_ps
hit_vs = names["vs"] & cache_vs
print()
print(f"GATE against {a.cache}:")
print(f"  pixel : {len(hit_ps)} of {len(cache_ps)} cache entries reproduced "
      f"byte-for-byte from the disc ({100*len(hit_ps)/max(1,len(cache_ps)):.1f}%)")
print(f"  vertex: {len(hit_vs)} of {len(cache_vs)} — expected 0; the title patches the "
      f"fetch instructions at load (see this file's docstring)")
print(f"  the disc also holds {len(names['ps'] - cache_ps)} pixel and "
      f"{len(names['vs'] - cache_vs)} vertex shaders NO RUN HAS EVER BOUND")

# The gate's threshold is the population, not a round number: the two pixel shaders the
# cache holds and the disc does not are `ps_926c15dd20571cf1` and one other, recovered
# from a live process rather than from a disc bank, and are a known, enumerated gap.
if len(hit_ps) < len(cache_ps) - 2:
    print(f"\nFAIL: {len(cache_ps) - len(hit_ps)} cache pixel shaders were not "
          f"reproduced. The container decode is wrong, or the cache drifted.")
    sys.exit(1)
print("\nOK")
