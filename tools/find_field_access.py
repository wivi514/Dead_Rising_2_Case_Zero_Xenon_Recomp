#!/usr/bin/env python3
"""Census every D-form load/store in the image that touches a given structure
OFFSET, and name the function each one lives in.

WHY THIS EXISTS.  Co-op part 11 had a list of 31 structure offsets that a HOST
item pickup writes and a GUEST pickup does not (docs/coop-plan.md section 11),
and no way to turn an offset into code except `grep` over a disassembly dump of
the whole image.  That is slow enough that the previous session hand-disassembled
six addresses and inferred the rest, which is exactly the habit this project's
evidence rules forbid.  The offsets are the measurement; this makes reading them
cheap.

It scans the .text range for the D-form encodings (`lwz`/`stw`/`lbz`/`stb`/
`lhz`/`sth`/float pairs) plus the DS-form 64-bit pair, because on this compiler a
fixed member access is almost always D-form off a base register.  INDEXED stores
(`stwx`) are invisible to it BY CONSTRUCTION -- they carry no displacement -- and
it says so in its own output rather than letting a zero read as "nothing writes
this field".  Co-op part 11's `+0x1648` is exactly that case: no D-form site, so
the write goes through a computed index, which is itself the finding.

The enclosing function comes from the recompiler's own function list
(`ppc/ppc_func_mapping.cpp`), so it agrees with the names the runtime hooks use.
"""
import argparse
import bisect
import re
import sys

IMAGE_BASE = 0x82000000
TEXT_BEGIN = 0x82150000
TEXT_END = 0x829C3554

# primary opcode -> (mnemonic, is_store, is_ds_form)
D_FORM = {
    32: ("lwz", False), 33: ("lwzu", False), 34: ("lbz", False), 35: ("lbzu", False),
    36: ("stw", True), 37: ("stwu", True), 38: ("stb", True), 39: ("stbu", True),
    40: ("lhz", False), 41: ("lhzu", False), 42: ("lha", False), 43: ("lhau", False),
    44: ("sth", True), 45: ("sthu", True),
    48: ("lfs", False), 49: ("lfsu", False), 50: ("lfd", False), 51: ("lfdu", False),
    52: ("stfs", True), 53: ("stfsu", True), 54: ("stfd", True), 55: ("stfdu", True),
}
DS_FORM = {58: ("ld/lwa", False), 62: ("std/stdu", True)}


def load_functions(path):
    addrs = set()
    for m in re.finditer(r"0x(82[0-9A-Fa-f]{6})", open(path).read()):
        addrs.add(int(m.group(1), 16))
    return sorted(addrs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("offsets", nargs="+", help="structure offsets, e.g. 0xFA0 0x1448")
    ap.add_argument("--image", default="assets/game/default_image.bin")
    ap.add_argument("--funcs", default="ppc/ppc_func_mapping.cpp")
    ap.add_argument("--stores-only", action="store_true")
    args = ap.parse_args()

    wanted = {int(o, 16) for o in args.offsets}
    img = open(args.image, "rb").read()
    funcs = load_functions(args.funcs)
    if not funcs:
        sys.exit(f"no function addresses parsed out of {args.funcs}")

    hits = {}
    for va in range(TEXT_BEGIN, min(TEXT_END, IMAGE_BASE + len(img)), 4):
        w = int.from_bytes(img[va - IMAGE_BASE:va - IMAGE_BASE + 4], "big")
        op = w >> 26
        ent = D_FORM.get(op)
        if ent is not None:
            d = w & 0xFFFF
        else:
            ent = DS_FORM.get(op)
            if ent is None:
                continue
            d = w & 0xFFFC
        if d not in wanted:
            continue
        mnem, is_store = ent
        if args.stores_only and not is_store:
            continue
        rs = (w >> 21) & 31
        ra = (w >> 16) & 31
        i = bisect.bisect_right(funcs, va) - 1
        hits.setdefault(d, []).append((va, mnem, rs, ra, funcs[i]))

    for d in sorted(wanted):
        rows = hits.get(d, [])
        print(f"=== +0x{d:04X}: {len(rows)} D-form site(s)"
              + ("  (none -- written through a computed index, if at all)" if not rows else ""))
        for va, mnem, rs, ra, fn in rows:
            print(f"    {va:08X}  {mnem:9s} r{rs},0x{d:x}(r{ra})   in sub_{fn:08X}+0x{va - fn:x}")
    print("\nNOTE: indexed forms (stwx/lwzx) carry no displacement and CANNOT appear here.")


if __name__ == "__main__":
    main()
