#!/usr/bin/env python3
"""Rename Xenia's shader dumps into the runtime's own cache naming.

WHY THIS EXISTS
---------------
The SPIR-V cache is keyed on FNV-1a of the microcode as the GUEST holds it, computed
by our own IM_LOAD handler (`runtime/gpu/pm4.cpp`). That key is only obtainable from a
run that actually loads the shader — so the gameplay shaders, which our runtime cannot
reach until the title screen is driven past, would have no cache entries.

Xenia's `dump_shaders` has them: capture A2 carries 335 gameplay blobs. They are the
same bytes, and this was MEASURED rather than assumed — of the 121 blobs our own boot
dumped, 120 are byte-identical to A1's, and the only difference is dword order:

    Xenia writes host-endian (little) dwords; the guest holds big-endian.

So a byte-swap plus our own hash turns a Xenia dump into a cache entry with exactly the
name the runtime will ask for. Nothing is guessed: if the swap were wrong, the resulting
hash would simply never be looked up, and the renderer's miss report would say so.

The one blob of the 121 with no A1 counterpart is not a discrepancy to chase — our run
and the capture are different drives through the same era and neither is a subset of
the other (the same lesson as gotcha 45 about A1 and A5).

Usage: xenia_ucode_to_cache.py <xenia_shader_dir>... <out_dir>
"""
import glob
import os
import struct
import sys


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def swap_dwords(d):
    if len(d) % 4:
        return None
    return b"".join(d[i:i + 4][::-1] for i in range(0, len(d), 4))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[-1]
    os.makedirs(out, exist_ok=True)

    written, skipped = 0, 0
    for src in sys.argv[1:-1]:
        for f in sorted(glob.glob(os.path.join(src, "*.ucode.bin.vert")) +
                        glob.glob(os.path.join(src, "*.ucode.bin.frag"))):
            kind = "vs" if f.endswith(".vert") else "ps"
            data = open(f, "rb").read()
            be = swap_dwords(data)
            # The microcode is 3-dword groups; anything else is not a shader we can
            # translate, and writing it would put a file in the cache that the
            # container synthesizer then fails on for a reason far from here.
            if be is None or (len(be) // 4) % 3:
                print(f"skipped (not 3-dword aligned): {os.path.basename(f)}")
                skipped += 1
                continue
            name = f"{kind}_{fnv1a(be):016x}.ucode"
            open(os.path.join(out, name), "wb").write(be)
            written += 1
    print(f"wrote {written} microcode blobs into {out} ({skipped} skipped)")


if __name__ == "__main__":
    main()
