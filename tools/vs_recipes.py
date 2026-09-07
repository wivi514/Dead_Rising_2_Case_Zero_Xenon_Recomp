#!/usr/bin/env python3
"""Build `vs_recipes.bin` — how to regenerate every runtime VERTEX shader from the disc.

WHY THIS EXISTS (docs/part102-no-popin-plan.md §2). The disc's vertex-shader bank holds
142 TEMPLATES, and the title patches each one's vertex-fetch instructions at bind time out
of the vertex declaration (release-plan §1.4's retraction: 0 of 104 runtime vertex shaders
appear verbatim on disc). The bind is lazy — it runs inside the draw flush, `sub_8284F300`,
which is the ONLY caller of the bind check `sub_8284F1C0` and the binder `sub_8284EF28` —
so on a player's first session every vertex shader is translated at the draw that first
binds it, and every draw wanting a pipeline on it is SKIPPED until that finishes. That skip
is the session-one pop-in (234,849 draws on the outdoor route, phase5-notes §6er).

The census this tool makes permanent: 102 of the 104 runtime vertex shaders are a disc
template of the SAME LENGTH with 2-32 dwords rewritten (978 dwords in total), always the
vfetch triples whose slot/offset/stride/format fields are zero on disc:

    dw13  disc 00000A88  runtime 00393A88
    dw14  disc 00000000  runtime 00000003
    dw15  disc 05F85000  runtime 03F85000

So a runtime vertex shader = (template hash, list of (dword index, value)). Applying that
to the player's own disc reproduces the microcode byte-for-byte, and the first-run pass
(gpu/shader_prebuild.cpp) can translate the vertex half BEFORE the first frame — which is
what lets the shipped 1,365-key pre-warm seed create every pipeline at boot on session one.

The two runtime vertex shaders WITHOUT a template (`vs_539ea9e08aa83f0c`, 108 B, and
`vs_a4ae7c2b7c1818c4`, 60 B) are engine-synthesised and are the 2nd and 4th shaders the
title ever binds — at boot, before any visible frame — so first-sight covers them and
they are listed here by name rather than treated as a failure.

THE GATE IS TWO-SIDED AND EXACT.
  1. Every recipe written is re-applied to its template and must FNV-1a to the runtime
     hash it claims (the same hash the renderer keys the cache with). Exit 1 otherwise.
  2. Every vertex-shader hash the pre-warm seed (`prewarm.keys`) names must be a recipe or
     one of the two synthesised shaders. Any other is a SEED KEY NO FIRST RUN CAN SATISFY
     and is printed by name; --strict-seed makes that exit 1 too. (The 2026-09-07 seed
     carries three such orphans whose microcode no dump holds — a shelf-life defect of the
     seed, not of the recipes, so it is reported rather than fatal by default.)

FILE FORMAT (all little-endian, like prewarm.keys):
    u32 magic 'ZCVR' (0x5256435A)   u32 version 1   u32 recipeCount
    per recipe:  u64 templateHash  u64 runtimeHash  u32 dwordCount  u32 patchCount
                 patchCount x { u32 dwordIndex, u32 valueBE }
  `valueBE` is the dword exactly as it sits in the microcode byte stream (big-endian on
  the wire), stored as the raw 4 bytes so the runtime memcpy's it and never byte-swaps.

Usage:
    tools/vs_recipes.py                      # defaults: repo banks, ucode-dumps, seed
    tools/vs_recipes.py --out tools/release/vs_recipes.bin
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from big_list import read_toc, BadArchive            # noqa: E402

# vo_extract_microcode.py parses argv at import, so its two helpers are restated here
# (D.1's container rule, unchanged): the microcode is the TAIL of the object, starting
# at blobOffset + the literal-constant block size that +0x18 points to.
MAGIC_VS = 0x102A1101


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def be32(b, off):
    return struct.unpack_from('>I', b, off)[0]


def microcode_range(obj):
    """(start, length, 'vs') or (None, None, reason) — every bound checked by name."""
    if len(obj) < 0x20:
        return None, None, 'shorter than a header'
    if be32(obj, 0) != MAGIC_VS:
        return None, None, f'magic {be32(obj, 0):08X} is not a vertex-shader object'
    blob, blob_len, desc = be32(obj, 4), be32(obj, 8), be32(obj, 0x18)
    if blob >= len(obj) or blob + blob_len != len(obj) or desc + 4 > len(obj):
        return None, None, 'container bounds refuse to parse'
    start = blob + be32(obj, desc)
    if start >= len(obj) or start % 4:
        return None, None, f'microcode start {start} refuses to parse'
    return start, len(obj) - start, 'vs'

MAGIC = 0x5256435A  # 'ZCVR'
VERSION = 1
# Engine-synthesised at boot; on no disc bank; bound before any visible frame.
SYNTHESISED = {0x539ea9e08aa83f0c, 0xa4ae7c2b7c1818c4}
# A patch larger than this is not "a template with its fetches filled in".
MAX_PATCH_DWORDS = 64


def load_templates(bank_path):
    """-> {hash: bytes} of every distinct vertex-shader microcode in the vs bank."""
    entries, _ = read_toc(bank_path)
    out = {}
    with open(bank_path, 'rb') as f:
        for e in entries:
            if not e['name'].endswith('.vo'):
                continue
            f.seek(e['offset'])
            obj = f.read(e['size'])
            start, length, kind = microcode_range(obj)
            if start is None:
                print(f"  refused {e['name']}: {kind}")
                continue
            if kind != 'vs':
                continue
            uc = obj[start:start + length]
            out.setdefault(fnv1a(uc), uc)
    return out


def load_dumps(dump_dir):
    """-> {hash: bytes} of every runtime vertex shader the dumps hold, hash-checked."""
    out = {}
    for path in sorted(glob.glob(os.path.join(dump_dir, 'vs_*.ucode'))):
        b = open(path, 'rb').read()
        h = fnv1a(b)
        claimed = int(os.path.basename(path)[3:19], 16)
        if h != claimed:
            print(f"  dump {os.path.basename(path)} does not hash to its own name — skipped")
            continue
        out[h] = b
    return out


def seed_vs_hashes(keys_path):
    b = open(keys_path, 'rb').read()
    magic, ver, n = struct.unpack_from('<III', b, 0)
    if magic != 0x5750435A or ver != 1:
        raise SystemExit(f"{keys_path}: not a v1 prewarm key file")
    return {struct.unpack_from('<Q', b, 12 + i * 56)[0] for i in range(n)}


def best_template(runtime, templates_by_len):
    """The same-length template with the fewest differing dwords, or None."""
    n = len(runtime) // 4
    best = None
    for th, t in templates_by_len.get(len(runtime), []):
        diff = [i for i in range(n) if runtime[i * 4:i * 4 + 4] != t[i * 4:i * 4 + 4]]
        if best is None or len(diff) < len(best[1]):
            best = (th, diff, t)
    return best


def apply(template, patches):
    b = bytearray(template)
    for idx, raw in patches:
        b[idx * 4:idx * 4 + 4] = raw
    return bytes(b)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bank', default=os.path.join(
        root, 'assets/game/data/shaders/deadrisingprologue-vs.big'))
    ap.add_argument('--dumps', default=os.path.expanduser('~/DR2CZ-troubleshooting/ucode-dumps'))
    ap.add_argument('--seed', default=os.path.join(root, 'tools/release/prewarm.keys'))
    ap.add_argument('--out', default=os.path.join(root, 'tools/release/vs_recipes.bin'))
    ap.add_argument('--strict-seed', action='store_true',
                    help='exit 1 if the seed names a vertex shader no recipe can produce')
    a = ap.parse_args()

    try:
        templates = load_templates(a.bank)
    except BadArchive as e:
        raise SystemExit(f"{a.bank}: {e}")
    dumps = load_dumps(a.dumps)
    print(f"{len(templates)} distinct disc templates, {len(dumps)} runtime vertex shaders dumped")

    by_len = {}
    for h, t in templates.items():
        by_len.setdefault(len(t), []).append((h, t))

    recipes = []
    no_template = []
    too_big = []
    hist = {}
    for rh, rb in sorted(dumps.items()):
        b = best_template(rb, by_len)
        if b is None:
            no_template.append(rh)
            continue
        th, diff, t = b
        if len(diff) > MAX_PATCH_DWORDS:
            too_big.append((rh, len(diff)))
            continue
        patches = [(i, rb[i * 4:i * 4 + 4]) for i in diff]
        recipes.append((th, rh, len(rb) // 4, patches))
        hist[len(diff)] = hist.get(len(diff), 0) + 1

    # Gate 1: every recipe reproduces its runtime shader exactly.
    bad = 0
    for th, rh, n, patches in recipes:
        if fnv1a(apply(templates[th], patches)) != rh:
            print(f"  GATE FAIL: recipe for vs_{rh:016x} does not reproduce it")
            bad += 1

    total_patch = sum(len(p) for _, _, _, p in recipes)
    print(f"recipes: {len(recipes)}  (patch dwords total {total_patch}; "
          f"per shader min {min(hist) if hist else 0} max {max(hist) if hist else 0})")
    for rh in no_template:
        tag = 'engine-synthesised, first-sight at boot' if rh in SYNTHESISED else 'UNEXPECTED'
        print(f"  no same-length template: vs_{rh:016x} ({len(dumps[rh])} B) — {tag}")
    for rh, n in too_big:
        print(f"  patch too large ({n} dwords): vs_{rh:016x} — not a template+fetch patch")

    # Gate 2: what the seed asks for that no recipe provides.
    orphans = []
    if os.path.exists(a.seed):
        have = {rh for _, rh, _, _ in recipes} | SYNTHESISED
        seed = seed_vs_hashes(a.seed)
        orphans = sorted(seed - have)
        print(f"seed {os.path.basename(a.seed)}: {len(seed)} distinct vertex shaders, "
              f"{len(seed & have)} producible at first run, {len(orphans)} orphan(s)")
        for h in orphans:
            print(f"  seed names vs_{h:016x}: no recipe and no dump — a key no first run can satisfy")

    with open(a.out, 'wb') as f:
        f.write(struct.pack('<III', MAGIC, VERSION, len(recipes)))
        for th, rh, n, patches in recipes:
            f.write(struct.pack('<QQII', th, rh, n, len(patches)))
            for i, raw in patches:
                f.write(struct.pack('<I', i))
                f.write(raw)
    print(f"wrote {a.out}: {len(recipes)} recipes, {os.path.getsize(a.out)} bytes")

    unexpected = [h for h in no_template if h not in SYNTHESISED]
    if bad or too_big or unexpected or (a.strict_seed and orphans):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
