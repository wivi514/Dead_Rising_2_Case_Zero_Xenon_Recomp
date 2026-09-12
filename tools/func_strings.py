#!/usr/bin/env python3
"""Which STRINGS does a guest function reference? — a subsystem oracle for a flat profile.

WHY THIS EXISTS. Part 116's guest profile is 2,000 `sub_XXXXXXXX` names at under 5% each.
The title's own code names its subsystems everywhere — assert paths, debug printf
formats, class names — but the reference is a `lis rX,hi / addi rX,rX,lo` pair inside
the function and nothing prints it. This scans a function's body (start to the next
function start in the recompiler's map) for those pairs, and prints every one that
lands on a printable string in the image. A function that mentions
`c:\\dr2\\code\\crowd\\...` or `hkpWorld` has told you what it is; a function that
mentions nothing is classified by its callers instead (tools/part116_callers.py).

Also `--callees`: the `bl` targets in the body, for a one-hop look without the profile.

Usage:
    tools/func_strings.py 8248EB00 [8242E920 ...] [--callees]
"""
import argparse, bisect, re, struct, sys

IMAGE = "assets/game/default_image.bin"
BASE = 0x82000000

def load_starts():
    starts = []
    for line in open("ppc/ppc_func_mapping.cpp"):
        m = re.match(r"\s*\{\s*0x([0-9A-Fa-f]+),", line)
        if m:
            starts.append(int(m.group(1), 16))
    starts.sort()
    return starts

def cstr(img, off, lim=120):
    s = bytearray()
    while off < len(img) and len(s) < lim:
        c = img[off]
        if c == 0:
            break
        if not (32 <= c < 127):
            return None
        s.append(c); off += 1
    return s.decode() if len(s) >= 5 else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("funcs", nargs="+")
    ap.add_argument("--callees", action="store_true")
    a = ap.parse_args()
    img = open(IMAGE, "rb").read()
    starts = load_starts()
    for f in a.funcs:
        start = int(f.replace("sub_", ""), 16)
        i = bisect.bisect_right(starts, start)
        end = starts[i] if i < len(starts) else start + 0x4000
        end = min(end, start + 0x20000)
        hi = {}          # reg -> (value<<16, pc)
        strings, callees = [], []
        for pc in range(start, end, 4):
            w, = struct.unpack(">I", img[pc - BASE:pc - BASE + 4])
            op = w >> 26
            rd = (w >> 21) & 31; ra = (w >> 16) & 31; imm = w & 0xFFFF
            if op == 15 and ra == 0:                       # lis rd, imm
                hi[rd] = (imm << 16, pc)
            elif op == 14 and ra in hi:                    # addi rd, ra, simm
                v = (hi[ra][0] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
                if BASE <= v < BASE + len(img):
                    s = cstr(img, v - BASE)
                    if s:
                        strings.append((pc, v, s))
            elif op == 24 and ra in hi:                    # ori rd, ra, uimm
                v = hi[ra][0] | imm
                if BASE <= v < BASE + len(img):
                    s = cstr(img, v - BASE)
                    if s:
                        strings.append((pc, v, s))
            elif op == 18 and (w & 1):                     # bl
                li = w & 0x03FFFFFC
                if li & 0x02000000: li -= 0x04000000
                tgt = (pc + li) if not (w & 2) else li
                callees.append(tgt & 0xFFFFFFFF)
            if op == 15 and ra == 0:
                pass
        print(f"== sub_{start:08X}  ({(end-start)//4} instructions to next start {end:08X})")
        seen = set()
        for pc, v, s in strings:
            if s in seen: continue
            seen.add(s)
            print(f"   {pc:08X} -> {v:08X}  {s!r}")
        if a.callees:
            from collections import Counter
            for t, c in Counter(callees).most_common(30):
                print(f"   bl sub_{t:08X} x{c}")

if __name__ == "__main__":
    main()
