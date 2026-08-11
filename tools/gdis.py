#!/usr/bin/env python3
"""Disassemble a guest address range out of the loaded XEX image.

WHY THIS EXISTS
---------------
Every non-trivial question in this port eventually becomes "what does the title's own
code do here?" — what populates a struct field, which branch a predicate takes, how
many arguments a call site really passes. The generated `ppc/*.cpp` answers some of
that, but it is 156 MB across 228 files, it is indexed by nothing, and reading a
recompiled function is reading a translation when the question is about the original.

The host toolchain cannot help. `objdump` here has no PowerPC target, `llvm-objdump`
has no `-b binary`, and `llvm-mc --disassemble` silently loses instruction alignment
the first time it meets an encoding it does not know — which on a VMX128-heavy image
is constantly, and the damage is invisible because the output after that point still
looks like plausible instructions. Capstone gets the alignment right because we feed
it one fixed-width instruction at a time.

This was written from scratch twice in two sessions before being kept. It is here so
there is not a third time.

Examples:
    # a function, from the address the crash reporter printed
    python3 tools/gdis.py 8284B568 --count 120

    # a window around a faulting instruction
    python3 tools/gdis.py 8284B6C0 --to 8284B710

    # every instruction in the image that references a constant, with context
    python3 tools/gdis.py --find-uses 0x7FEA1800
"""
import argparse
import struct
import sys

try:
    import capstone
except ImportError:
    sys.exit("need capstone: pip install capstone")

DEFAULT_IMAGE = "assets/game/default_image.bin"
DEFAULT_BASE = 0x82000000


def load(path, base):
    with open(path, "rb") as f:
        data = f.read()
    return data, base


def md():
    # Big-endian 32-bit PowerPC. detail=False: we only ever print, and the detail
    # decoder roughly triples the cost over an 8.8 MB text section.
    m = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    m.skipdata = True          # never lose alignment on an unknown encoding
    return m


def disasm(data, base, start, count):
    """Yield (addr, raw, text) for `count` instructions from guest `start`.

    Deliberately one instruction per capstone call. PowerPC is fixed-width, so a
    bad decode must cost exactly 4 bytes and nothing more — handing capstone a long
    buffer lets `skipdata` resynchronise on its own terms and the addresses after an
    unknown encoding stop being trustworthy.
    """
    m = md()
    for i in range(count):
        addr = start + i * 4
        off = addr - base
        if off < 0 or off + 4 > len(data):
            return
        raw = struct.unpack_from(">I", data, off)[0]
        ins = list(m.disasm(data[off:off + 4], addr))
        if ins:
            yield addr, raw, f"{ins[0].mnemonic}\t{ins[0].op_str}"
        else:
            yield addr, raw, ".long\t0x%08X" % raw


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("start", nargs="?", help="guest address, hex (0x optional)")
    ap.add_argument("--to", help="disassemble up to this guest address (exclusive)")
    ap.add_argument("--count", type=int, default=64, help="instructions (default 64)")
    ap.add_argument("--image", default=DEFAULT_IMAGE)
    ap.add_argument("--base", default=hex(DEFAULT_BASE))
    ap.add_argument("--find-uses", metavar="VALUE",
                    help="scan for lis/addi and lis/ori pairs building this constant")
    ap.add_argument("--context", type=int, default=4,
                    help="instructions of context around each --find-uses hit")
    args = ap.parse_args()

    base = int(args.base, 16)
    data, base = load(args.image, base)

    if args.find_uses:
        target = int(args.find_uses, 16 if args.find_uses.lower().startswith("0x") else 16)
        find_uses(data, base, target, args.context)
        return 0

    if not args.start:
        ap.error("give a start address, or --find-uses")
    start = int(args.start, 16)
    count = args.count
    if args.to:
        count = (int(args.to, 16) - start) // 4

    for addr, raw, text in disasm(data, base, start, count):
        print(f"{addr:08X}  {raw:08X}  {text}")
    return 0


# D-form memory access opcodes: EA = (rA) + sign_extend(d). These are the OTHER way a
# 32-bit address reaches the pipeline, and the one the compiler prefers when the address
# is a global it is about to dereference rather than pass along.
#
# Deliberately excluded: the DS-form 64-bit pair (opcodes 58/62, ld/std), whose
# displacement is the top 14 bits of the low half rather than all 16. This image is
# 32-bit PPC user code and does not use them; including them would need a different
# mask and would silently mis-decode if it ever matched.
D_FORM_MEM = {
    32: "lwz",  33: "lwzu", 34: "lbz",  35: "lbzu",
    36: "stw",  37: "stwu", 38: "stb",  39: "stbu",
    40: "lhz",  41: "lhzu", 42: "lha",  43: "lhau",
    44: "sth",  45: "sthu",
    48: "lfs",  49: "lfsu", 50: "lfd",  51: "lfdu",
    52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
}


def find_uses(data, base, target, context):
    """Every site in the image that materialises `target` — as a VALUE or as an ADDRESS.

    A 32-bit constant never appears as one instruction on PowerPC, so grepping the
    image for its bytes finds data references and misses every code reference. This
    reconstructs the pair instead: `lis rX, hi` followed within a short window by the
    instruction carrying the low half. The window is deliberately short — the two
    halves are almost always adjacent, and widening it turns a precise answer into a
    list of coincidences.

    TWO SHAPES, AND FOR A LONG TIME THIS ONLY KNEW ONE. The original version matched
    `addi`/`ori` alone, i.e. only the case where the address is computed into a
    register and then used. But a global the compiler is about to LOAD OR STORE never
    gets its address materialised at all — the low half is folded into the memory
    operand:

        lis  r9,  0x82A4
        lwz  r31, 0x6294(r9)        <- the reference to 0x82A46294, invisible to addi/ori

    Part 29 asked this scanner who touched `0x82A46294`, the global holding the object
    whose cinematic Update() is called every frame, and got **"0 sites"** — for a
    global that is read on the frame path. A zero from a scanner that cannot match the
    dominant spelling is a detection failure, not a fact (gotchas 3 and 25), and here
    it would have retired the only live lead in the item.

    Note the two spellings report differently and that is on purpose: an `addi` hit
    means "code took this ADDRESS" (or built this constant), a memory-operand hit means
    "code READ or WROTE this location", and for a data address the second is the
    question almost always being asked. The mnemonic is printed in the header so a
    `stw`/`stfs` site — the writer, which is usually what you are hunting — can be
    picked out of a long read list by eye.
    """
    hi, lo = (target >> 16) & 0xFFFF, target & 0xFFFF
    # addi and a D-form displacement both sign-extend their immediate, so the high half
    # the compiler emits is one greater when the low half has bit 15 set. Both spellings
    # have to be searched or every address ending above 0x8000 goes silently unfound.
    hi_addi = (hi + 1) & 0xFFFF if lo & 0x8000 else hi
    hits = 0
    for off in range(0, len(data) - 4, 4):
        word = struct.unpack_from(">I", data, off)[0]
        if (word >> 26) != 15:                      # addis / lis
            continue
        rt = (word >> 21) & 0x1F
        imm = word & 0xFFFF
        if imm not in (hi, hi_addi):
            continue
        for k in range(1, 9):
            if off + k * 4 + 4 > len(data):
                break
            nxt = struct.unpack_from(">I", data, off + k * 4)[0]
            op, ra = nxt >> 26, (nxt >> 16) & 0x1F
            src = (nxt >> 21) & 0x1F
            kind = None
            if op == 14 and ra == rt and (nxt & 0xFFFF) == lo and imm == hi_addi:
                kind = "addi"                       # addi rD, rT, lo
            elif op == 24 and src == rt and (nxt & 0xFFFF) == lo and imm == hi:
                kind = "ori"                        # ori  rD, rT, lo
            elif op in D_FORM_MEM and ra == rt and (nxt & 0xFFFF) == lo \
                    and imm == hi_addi:
                kind = D_FORM_MEM[op]               # lwz/stw/... lo(rT)
            else:
                continue
            addr = base + off
            hits += 1
            print(f"--- {addr:08X}  [{kind}]")
            for a, raw, text in disasm(data, base, addr - context * 4,
                                       context * 2 + k + 1):
                mark = ">>" if a in (addr, addr + k * 4) else "  "
                print(f" {mark} {a:08X}  {raw:08X}  {text}")
            # An `addi`/`ori` consumes the pair, but ONE `lis` commonly feeds several
            # accesses to neighbouring globals (and to the same global, read then
            # written). Keep scanning the window for those instead of reporting the
            # first and stopping — the store is frequently not the first hit.
            if kind in ("addi", "ori"):
                break
    print(f"\n{hits} site(s) reference {target:08X}")


if __name__ == "__main__":
    sys.exit(main())
