#!/usr/bin/env python3
"""Is the Xenos microcode the guest submits actually inside the disc's `.vo`/`.po` shader objects?

WHY THIS EXISTS. `docs/xenia-capture-analysis.md` finding 6 said no — that the disc shader
banks are "NOT usable microcode" — and that answer stood for the whole project. It was
reached with an **aligned 8-byte n-gram overlap** test between a `.vo` payload and the
microcode blobs Xenia captured, which read 4 of 159 against a 4-of-73 background.

That test cannot find what is actually there, for two reasons this tool fixes:

  1. **The microcode is a SUB-RANGE of the object, not the payload.** These objects run about
     twice the size of the microcode inside them — the rest is build metadata (including the
     original `c:\\bcg\\...\\a07a5e80.updb` path) and constant tables. Comparing whole payloads
     dilutes a real match into noise.
  2. **It starts at an arbitrary offset.** Measured here, 162 distinct start offsets, and many
     of them (316, 340, 364, 380, 388 …) are not 8-byte aligned. An ALIGNED n-gram comparison
     is structurally blind to an unaligned embedding — it can only match if the payload
     happens to begin on the same phase.

So this asks the containment question instead: does a known microcode blob appear VERBATIM
inside some disc object? A 48-byte exact match is four Xenos ALU words and is far past
coincidence.

It also asks the follow-up that matters, because the guest is known to patch shaders at load:
for a blob whose HEAD does not match, does its TAIL? A tail match with a different head is a
shader the title rewrote the first instruction(s) of — real, on disc, and recoverable.

    tools/vo_microcode_probe.py <objects-dir> [--ucode ~/DR2CZ-troubleshooting/ucode-dumps]

Extract the objects first (they are three different extensions in three banks):

    for b in "vs .vo" "ps .po" "sc .scv"; do set -- $b
        python3 tools/big_list.py assets/game/data/shaders/deadrisingprologue-$1.big \\
            --extract "$2" --out /tmp/discsh
    done
"""
import argparse, glob, os, sys

ap = argparse.ArgumentParser()
ap.add_argument("objects", help="directory of extracted .vo/.po/.scv objects")
ap.add_argument("--ucode", default=os.path.expanduser("~/DR2CZ-troubleshooting/ucode-dumps"),
                help="directory of known-good *.ucode blobs — the ORACLE")
ap.add_argument("--probe", type=int, default=48,
                help="exact-match probe length in bytes (default 48 = four ALU words)")
a = ap.parse_args()

objs = {os.path.basename(p): open(p, "rb").read()
        for p in sorted(glob.glob(os.path.join(a.objects, "*")))
        if os.path.isfile(p)}
if not objs:
    sys.exit(f"** no objects in {a.objects}")
# One haystack with a separator that cannot occur mid-instruction, so a match can never
# straddle two objects and be counted as real.
hay = b"\x00\xff\x00\xff".join(objs.values())

ucode = sorted(glob.glob(os.path.join(a.ucode, "*.ucode")))
if not ucode:
    sys.exit(f"** no *.ucode oracle blobs in {a.ucode}")

print(f"{len(objs)} disc shader objects, {sum(len(v) for v in objs.values())} bytes")
print(f"{len(ucode)} known microcode blobs as the oracle\n")

stat = {"vs": [0, 0, 0, 0], "ps": [0, 0, 0, 0]}   # total, verbatim, tail-only, partial
absent, offsets = [], []
for p in ucode:
    b = open(p, "rb").read()
    if len(b) < a.probe + 16:
        continue
    kind = "vs" if os.path.basename(p).startswith("vs_") else "ps"
    stat[kind][0] += 1
    if b[:a.probe] in hay:
        stat[kind][1] += 1
        for n, o in objs.items():
            i = o.find(b[:a.probe])
            if i >= 0:
                offsets.append(i)
                break
    elif b[-a.probe:] in hay:
        stat[kind][2] += 1                  # head patched at load, body on disc
    elif any(b[i:i + 32] in hay for i in range(0, len(b) - 32, 4)):
        stat[kind][3] += 1
    else:
        absent.append((os.path.basename(p), len(b)))

tot = sum(s[0] for s in stat.values())
rec = sum(s[1] + s[2] for s in stat.values())
for k in ("vs", "ps"):
    t, v, tl, pa = stat[k]
    if not t:
        continue
    print(f"{k.upper()}: {t} blobs -> verbatim {v} ({100*v/t:.1f}%), "
          f"tail-only/patched {tl}, partial {pa}, absent {t-v-tl-pa}")
print(f"\nRECOVERABLE (verbatim or tail-matched): {rec} of {tot} ({100*rec/tot:.1f}%)")
if offsets:
    print(f"start offsets inside the object: {len(set(offsets))} distinct, "
          f"{sum(1 for o in set(offsets) if o % 8)} of them NOT 8-byte aligned "
          f"— which is why finding 6's aligned n-gram test could not see this")
for n, l in absent:
    print(f"  absent from the disc entirely: {n} ({l} B)")
# Exit 1 if the disc turns out not to carry the shaders, so this can be a gate later.
sys.exit(0 if rec else 1)
