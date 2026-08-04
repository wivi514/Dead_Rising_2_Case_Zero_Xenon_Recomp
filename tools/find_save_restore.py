#!/usr/bin/env python3
"""Locate the CRT register save/restore helper ladders in a loaded XEX image.

WHY THIS EXISTS
---------------
XenonRecomp cannot find these itself. `config/<Game>.toml` must name eight guest
addresses — `save/restgprlr_14`, `save/restfpr_14`, `save/restvmx_14`, `save/restvmx_64` —
and `Recompiler::Analyse()` carves a symbol per register from each, using fixed size
formulas. Get one wrong and the recompiler silently emits nonsense for every function
that spills non-volatiles, which is most of them.

The template ports found these by hand-encoding a ladder and grepping the image. Two
lessons from that (recorded in Asura's Wrath's bootstrap notes) are baked in here:

  1. **The base register is not fixed across titles.** Fable 2's CRT builds the gpr
     ladders against r12 (`std r14,-0x98(r12)`), Asura's Wrath's against r1. A scan with
     the base register hardcoded finds the fpr/vmx ladders and reports "no gpr ladder",
     which reads like the image is unusual rather than like the scan is wrong. So the
     base register is *discovered* from the first instruction and only required to be
     consistent down the ladder.
  2. **Verify against XenonRecomp's own size formulas, not against a guess.** The tails
     differ per family (gpr save ends `stw r12,-8(r1); blr`; gpr restore ends
     `lwz r12,-8(r1); mtlr r12; blr`; fpr/vmx just `blr`), and those tails are what make
     the reported sizes match `Analyse()`. This script checks them and says so.

VMX128 note: the vector ladders are `li r11,<disp>; stvx128 vN,r11,r12` pairs. The store
and load encodings are VMX128 (primary opcode 4), which this script does not fully
decode — it matches the *shape* (an `li r11` with a displacement stepping by 16, followed
by a primary-opcode-4 instruction) and separates save from restore by the bit that
differs between the two forms, then confirms the ladder is the right length and ends in
a `blr` at exactly the offset `Analyse()` expects.

Usage:
    python3 tools/find_save_restore.py assets/game/default_image.bin
    python3 tools/find_save_restore.py assets/game/default_image.bin --base 0x82000000
"""
import argparse
import struct
import sys

BLR = 0x4E800020
MTLR_R12 = 0x7D8803A6

# Primary opcodes
OP_ADDI = 14
OP_LFD = 50
OP_LWZ = 32
OP_STFD = 54
OP_STW = 36
OP_LD = 58
OP_STD = 62
OP_VMX128 = 4


def words(buf):
    return struct.unpack(f">{len(buf) // 4}I", buf[:len(buf) // 4 * 4])


def signed16(v):
    return v - 0x10000 if v & 0x8000 else v


def ds_form(word, opcode):
    """Decode a DS-form load/store (ld/std). Returns (rS, rA, disp) or None."""
    if (word >> 26) != opcode or (word & 3) != 0:
        return None
    return (word >> 21) & 0x1F, (word >> 16) & 0x1F, signed16(word & 0xFFFC)


def d_form(word, opcode):
    """Decode a D-form load/store (lfd/stfd/lwz/stw). Returns (rS, rA, disp) or None."""
    if (word >> 26) != opcode:
        return None
    return (word >> 21) & 0x1F, (word >> 16) & 0x1F, signed16(word & 0xFFFF)


def find_gpr_fpr_ladder(w, opcode, decode, first_disp, tail):
    """Yield (word_index, base_reg) for every 18-long r14..r31 ladder.

    `first_disp` is the displacement expected for register 14; each step adds 8.
    `tail` is a list of predicates applied to the words after the ladder.
    """
    n = len(w)
    for i in range(n - 24):
        d = decode(w[i], opcode)
        if d is None or d[0] != 14 or d[2] != first_disp:
            continue
        base = d[1]
        ok = True
        for k in range(1, 18):
            dk = decode(w[i + k], opcode)
            if dk is None or dk[0] != 14 + k or dk[1] != base or dk[2] != first_disp + 8 * k:
                ok = False
                break
        if not ok:
            continue
        if all(pred(w[i + 18 + j]) for j, pred in enumerate(tail)):
            yield i, base


def vector_move(word):
    """True if `word` is one of the four vector load/store forms these ladders use.

    Registers v14..v31 are reachable by the *classic* VMX encodings (primary opcode 31,
    `stvx` XO=231 -> 0x1CE, `lvx` XO=103 -> 0x0CE); v64..v127 need the VMX128 forms
    (primary opcode 4, 0x1CB / 0x0CB). A scanner that only knows the VMX128 form finds
    the 64-register ladders and then latches onto their 18-pair *suffix*
    (__savevmx_110..127) as if it were __savevmx_14 -- which is a plausible-looking
    address 0x170 inside a function it just reported as 516 bytes long.
    """
    op = word >> 26
    if op == OP_VMX128:
        # VMX128 splits the 7-bit vector register number across the instruction; the
        # high bit (VDh) lands in bit 2 of the low half, so __savevmx_64..95 encode as
        # 0x...1CB and __savevmx_96..127 as 0x...1CF. Mask that bit out rather than
        # listing both, or the ladder scan stops dead halfway through at register 96.
        return (word & 0x7FB) in (0x1CB, 0x0CB)
    if op == 31:
        return (word & 0x7FF) in (0x1CE, 0x0CE)
    return False


def find_vmx_ladder(w, count, first_disp):
    """Yield (word_index, is_store, first_disp) for li/vector-op ladders of `count` pairs.

    Shape: `li r11,<disp>` (addi r11,r0,disp) followed by a vector load/store, repeated,
    displacement stepping by +16, then a `blr`.
    """
    n = len(w)
    for i in range(n - count * 2 - 1):
        first = d_form(w[i], OP_ADDI)
        if first is None or first[0] != 11 or first[1] != 0 or first[2] != first_disp:
            continue
        if not vector_move(w[i + 1]):
            continue
        # Reject a suffix of a longer ladder: __savevmx_64's 46th pair also carries
        # li r11,-0x120 and is followed by 17 more pairs and a blr, so it matches an
        # 18-pair scan exactly. A real ladder entry point is not preceded by the
        # previous rung of the same ladder.
        if i >= 2 and vector_move(w[i - 1]):
            prev = d_form(w[i - 2], OP_ADDI)
            if prev is not None and prev[0] == 11 and prev[1] == 0 and prev[2] == first_disp - 16:
                continue
        ok = True
        for k in range(1, count):
            dk = d_form(w[i + 2 * k], OP_ADDI)
            if dk is None or dk[0] != 11 or dk[1] != 0 or dk[2] != first_disp + 16 * k:
                ok = False
                break
            if not vector_move(w[i + 2 * k + 1]):
                ok = False
                break
        if not ok or w[i + 2 * count] != BLR:
            continue
        # Store and load differ in bit 8 of the low half, in both encodings.
        yield i, bool(w[i + 1] & 0x100), first_disp


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", help="flat image from tools/xex_image_dump (indexed by RVA)")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x82000000)
    args = ap.parse_args()

    buf = open(args.image, "rb").read()
    w = words(buf)
    base = args.base

    def addr(idx):
        return base + idx * 4

    found = {}

    # --- gpr -------------------------------------------------------------
    # save: 18x std r(14+k), -0x98+8k(rA); then stw r12,-8(r1); blr   (size 80)
    for i, rA in find_gpr_fpr_ladder(
            w, OP_STD, ds_form, -0x98,
            [lambda x: d_form(x, OP_STW) == (12, 1, -8), lambda x: x == BLR]):
        found.setdefault("savegprlr_14", (addr(i), f"base r{rA}, tail stw r12,-8(r1); blr, size 80"))
    # rest: 18x ld; then lwz r12,-8(r1); mtlr r12; blr                (size 84)
    for i, rA in find_gpr_fpr_ladder(
            w, OP_LD, ds_form, -0x98,
            [lambda x: d_form(x, OP_LWZ) == (12, 1, -8),
             lambda x: x == MTLR_R12, lambda x: x == BLR]):
        found.setdefault("restgprlr_14", (addr(i), f"base r{rA}, tail lwz r12,-8(r1); mtlr; blr, size 84"))

    # --- fpr -------------------------------------------------------------
    # 18x stfd/lfd f(14+k), -0x90+8k(rA); then blr                    (size 76)
    for i, rA in find_gpr_fpr_ladder(w, OP_STFD, d_form, -0x90, [lambda x: x == BLR]):
        found.setdefault("savefpr_14", (addr(i), f"base r{rA}, 18x stfd + blr, size 76"))
    for i, rA in find_gpr_fpr_ladder(w, OP_LFD, d_form, -0x90, [lambda x: x == BLR]):
        found.setdefault("restfpr_14", (addr(i), f"base r{rA}, 18x lfd + blr, size 76"))

    # --- vmx -------------------------------------------------------------
    for i, is_store, disp in find_vmx_ladder(w, 18, -0x120):
        key = "savevmx_14" if is_store else "restvmx_14"
        found.setdefault(key, (addr(i), f"18 li/vmx pairs from li r11,{disp:#x} + blr, size 148"))
    for i, is_store, disp in find_vmx_ladder(w, 64, -0x400):
        key = "savevmx_64" if is_store else "restvmx_64"
        found.setdefault(key, (addr(i), f"64 li/vmx pairs from li r11,{disp:#x} + blr, size 516"))

    order = ["savegprlr_14", "restgprlr_14", "savefpr_14", "restfpr_14",
             "savevmx_14", "savevmx_64", "restvmx_14", "restvmx_64"]

    missing = [k for k in order if k not in found]
    for k in order:
        if k in found:
            a, note = found[k]
            print(f"{k+'_address':<24} = 0x{a:08X}   # {note}")
        else:
            print(f"{k+'_address':<24} = ???        # NOT FOUND")

    if missing:
        print(f"\n!! {len(missing)} ladder(s) not found: {', '.join(missing)}", file=sys.stderr)
        print("   Check the base-register assumption and the first displacement before "
              "concluding the image lacks them.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
