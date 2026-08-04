#!/usr/bin/env python3
"""Find PPC jump tables (switch statements) in an Xbox 360 XEX by idiom, not by
a fixed instruction sequence.

WHY THIS EXISTS
---------------
XenonAnalyse detects jump tables by matching *exact ordered* opcode sequences
(XenonAnalyse/main.cpp, `absoluteSwitch` et al). Its absolute-table pattern is

    lis, addi, rlwinm, lwzx, mtctr, bctr

Asura's Wrath was built by a compiler that schedules the same six instructions
in a different order:

    lis, rlwinm, addi, lwzx, mtctr, bctr        <-- rlwinm/addi transposed

so XenonAnalyse matched *zero* tables in a 12 MB UE3 binary. This scanner
anchors on the part that cannot vary — the `lwzx/lhzx/lbzx; mtctr; bctr` tail —
and then walks backwards over a small window to recover the table base, the
index register, and the case count, whatever order the setup landed in.

WHAT IT EMITS
-------------
A TOML fragment in XenonAnalyse's own output format, so it can be concatenated
into (or replace) config/<Game>_switch_tables.toml:

    [[switch]]
    base = 0x82242A48        # address of the `bctr`
    r = 26                   # register holding the (already bounds-checked) index
    default = 0x82244384     # where the bounds check jumps when out of range
    labels = [0x82242A4C, ...]
    values = [0x0, 0x5C0, ...]   # OPTIONAL — see "KEYING" below

KEYING: THE CASE CONSTANT IS NOT ALWAYS THE CASE NUMBER
-------------------------------------------------------
XenonRecomp lowers a table to `switch (r<N>.u32)`, and by default numbers the cases
0,1,2,... That is right only when the register it switches on still holds the raw
case number at the `bctr`. It frequently does not:

  * the offset idiom (2 below) dispatches on the byte OFFSET just loaded out of the
    table — often in the same register the load was indexed by (`lhzx r0,r12,r0`), so
    the index is simply gone;
  * an in-place scale (`rlwinm r0,r0,2`) leaves n*elem, not n.

In both cases only case 0 can ever match. Worse, the recompiler's `default:` arm was
`__builtin_unreachable()`, which licenses the compiler to drop the bounds check — so a
live dispatch read PAST THE END of the host jump table into a neighbouring function's
table and jumped into an unrelated function's body, in the current function's frame.
It faults later somewhere unrelated, with no call, no corruption and nothing to hook.
That is docs/xenia-capture-analysis.md finding 38, and it cost three sessions.

So this scanner emits `values` — the actual dispatch value per label — whenever it is
not the position, and XenonRecomp emits `case <value>:` for it (plus an honest
`PPC_SWITCH_ABORT` default). Absent `values` means "the register holds the index",
which is exactly the old behaviour, so tables that were already correct are untouched.

TWO IDIOMS, NOT ONE
-------------------
1. SWITCH_ABSOLUTE — a table of full 32-bit code addresses:

       lis/addi (table base); rlwinm (index*4); lwzx; mtctr; bctr

   The `lwzx` feeds CTR directly, so `<load>; mtctr; bctr` are adjacent.

2. SWITCH_OFFSET — a table of *byte* offsets from a code base, which is what
   this compiler emits whenever every case body fits within 1020 bytes of the
   first one:

       lis/addi (offset table);  lbzx rRaw, rTable, rIndex
       rlwinm rOff, rRaw, 2, 0, 29                  ; rOff = rRaw * 4
       lis/addi (code base);     add rT, rCode, rOff
       mtctr rT; bctr

   There is no address table anywhere in the image — the targets are computed.
   Nothing feeds CTR from a load, so the adjacency the first idiom relies on
   does not hold, and a scanner written for idiom 1 finds *nothing* here. On
   Asura's Wrath this idiom accounts for 27 further dispatch sites.

   The cost of missing one is not a compile error. XenonRecomp lowers an
   unrecognised `bctr` to `PPC_CALL_INDIRECT_FUNC(ctr); return;` — a *call* to
   the case body followed by a return that skips the function's epilogue. The
   non-volatile registers the prologue saved are never restored, so the caller
   resumes with the callee's values in r14..r31 and dies later, somewhere else,
   with a pointer that is really a loop counter. That is how this idiom was
   found: a store through r30, which held 1 because `li r30,1` in a callee had
   leaked past a bare `return;` (see docs/xenia-capture-analysis.md finding 15).

USAGE
    find_jumptables.py <default.xex> [-o out.toml] [--stats]
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from decrypt_xex import decrypt, make_converters  # noqa: E402

# ---------------------------------------------------------------------------
# Minimal PPC instruction predicates. We only need to recognise the handful of
# opcodes that participate in a jump-table sequence, so a full disassembler
# would be overkill (and capstone's PPC mode does not know the Xenon VMX128
# extensions anyway).
# ---------------------------------------------------------------------------
BCTR = 0x4E800420   # bctr  — branch to CTR, no link
BCTRL = 0x4E800421  # bctrl — branch to CTR with link (a call, not a switch)


def opcd(i):
    """Primary opcode field (bits 0..5)."""
    return i >> 26


def xop(i):
    """Extended opcode field (bits 21..30) for opcode-31 forms."""
    return (i >> 1) & 0x3FF


def is_lis(i):
    """lis rD,imm  ==  addis rD,r0,imm  (opcode 15 with rA == 0)."""
    return opcd(i) == 15 and ((i >> 16) & 0x1F) == 0


def is_addi(i):
    return opcd(i) == 14


def is_ori(i):
    return opcd(i) == 24


def is_rlwinm(i):
    return opcd(i) == 21


def rlwinm_dest(i):
    """rlwinm is `rlwinm rA,rS,SH,MB,ME` — destination is rA (bits 11..15)."""
    return (i >> 16) & 0x1F


def rlwinm_src(i):
    """...and the source is rS (bits 6..10), the opposite of most D-forms."""
    return (i >> 21) & 0x1F


def is_lwzx(i):
    return opcd(i) == 31 and xop(i) == 23


def is_lhzx(i):
    return opcd(i) == 31 and xop(i) == 279


def is_lbzx(i):
    return opcd(i) == 31 and xop(i) == 87


def is_add(i):
    """add rD,rA,rB (opcode 31, extended 266). The offset-table idiom's dispatch
    register is produced by this, not by a load."""
    return opcd(i) == 31 and xop(i) == 266


def is_mtctr(i):
    # mtspr with SPR == 9 (CTR). The SPR field is split/swapped in the encoding.
    if opcd(i) != 31 or xop(i) != 467:
        return False
    spr = ((i >> 16) & 0x1F) | (((i >> 11) & 0x1F) << 5)
    return spr == 9


def is_cmplwi(i):
    """cmpli with L == 0 (32-bit compare against an unsigned immediate)."""
    return opcd(i) == 10 and ((i >> 21) & 1) == 0


def is_bc(i):
    return opcd(i) == 16


def rt(i):
    """Destination / first register operand (bits 6..10)."""
    return (i >> 21) & 0x1F


def ra(i):
    """Second register operand (bits 11..15)."""
    return (i >> 16) & 0x1F


def rb(i):
    """Third register operand (bits 16..20)."""
    return (i >> 11) & 0x1F


def simm(i):
    """Sign-extended 16-bit immediate."""
    v = i & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def bc_target(addr, i):
    """Branch target of a conditional branch (opcode 16), absolute-bit aware."""
    bd = i & 0xFFFC
    if bd & 0x8000:
        bd -= 0x10000
    return bd if (i & 2) else addr + bd


# `bgt`/`ble` (and their *lr forms) are the two bounds-check branches MSVC emits
# in front of a jump table: BO/BI encode "branch if CR[bi] {set,clear}".
def bc_kind(i):
    """Return ('gt'|'le'|None, cr_field) for a conditional branch."""
    bo = (i >> 21) & 0x1F
    bi = (i >> 16) & 0x1F
    cr = bi >> 2
    bit = bi & 3          # 0=lt 1=gt 2=eq 3=so
    if bit != 1:
        return None, cr
    if (bo & 0x1E) == 12:     # branch if TRUE  -> bgt
        return "gt", cr
    if (bo & 0x1E) == 4:      # branch if FALSE -> ble
        return "le", cr
    return None, cr


class Image:
    """Random-access view of the loaded XEX image, addressed by guest VA.

    Two input forms, because two things can produce an analysable image:

      * a `.xex` -> decrypted here by `decrypt_xex.py`, which handles retail-key +
        *basic* compression (the Fable 2 / Asura's Wrath case);
      * a flat image from `tools/xex_image_dump` -> already loaded, indexed by RVA.

    Case Zero needs the second: its XEX is devkit-key encrypted and LZX-compressed
    (`compression = 2`), which decrypt_xex.py cannot read at all — it parses the
    normal-compression FileFormatInfo as a basic block table and yields nonsense.
    Rather than teach the Python an LZX decoder, we dump the image with the
    recompiler's own loader and scan that. See tools/xex_image_dump.cpp.
    """

    def __init__(self, path, base=None):
        if path.lower().endswith(".xex"):
            self.data, ranges = decrypt(path)
            self.g2d, _ = make_converters(ranges)
        else:
            self.data = open(path, "rb").read()
            image_base = base if base is not None else 0x82000000
            # A flat loaded image is contiguous: guest VA - image base == file offset.
            self.g2d = lambda a: (a - image_base) if 0 <= a - image_base < len(self.data) else None

    def word(self, addr):
        off = self.g2d(addr)
        if off is None or off + 4 > len(self.data):
            return None
        return struct.unpack_from(">I", self.data, off)[0]

    def byte(self, addr):
        off = self.g2d(addr)
        if off is None or off >= len(self.data):
            return None
        return self.data[off]

    def half(self, addr):
        off = self.g2d(addr)
        if off is None or off + 2 > len(self.data):
            return None
        return struct.unpack_from(">H", self.data, off)[0]


def scan(image, code_lo, code_hi, window=24):
    """Yield one dict per detected absolute jump table.

    `window` is how many instructions back from the `bctr` we are willing to
    look for the setup. Compilers interleave unrelated work into these
    sequences, so a fixed-length pattern match is exactly the wrong tool; a
    bounded backwards search tolerates the scheduling.
    """
    addr = code_lo
    while addr < code_hi:
        insn = image.word(addr)
        if insn != BCTR:
            addr += 4
            continue

        tbl = analyse_bctr(image, addr, window, code_lo, code_hi) or \
            analyse_bctr_offset_table(image, addr, window, code_lo, code_hi)
        if tbl:
            yield tbl
        addr += 4


# How far a case body may be from its dispatch before we stop believing the two
# belong to the same function. Used only by the count INFERENCE below, to tell a
# switch (bodies a few hundred bytes away) from a table of unrelated function
# pointers. The largest real distance measured on Case Zero is 0x678.
NEAR_DISPATCH = 0x10000


def infer_count_absolute(image, tbl_base, bctr_addr, code_lo, code_hi):
    """Recover a case count from the table's own contents. Returns (count, labels).

    WHY THIS IS NEEDED — the count normally comes from the `cmplwi` bounds check,
    but the compiler is free to hoist that check anywhere, including out of the
    window we look at and out of the loop entirely. When that happens the whole
    table is REJECTED, silently, and the cost is severe and remote: XenonRecomp
    lowers the unrecognised `bctr` to `PPC_CALL_INDIRECT_FUNC(ctr); return;`, so
    the case body runs as a *call* and the function then returns without its
    epilogue, leaking the callee's r14..r31 to the caller. On Case Zero that was
    one site (0x82955A94) and it presented as a null-pointer crash three frames
    away — see docs/xenia-capture-analysis.md section 15, and
    tools/find_unlowered_switches.py, which is the check that catches it.

    THE SELF-VALIDATING RULE — greedy reading alone cannot find the end, because
    a PPC instruction word very often looks like a code address (anything
    starting 0x82.. decodes as `lwz`). What bounds it exactly is a structural
    fact: **a case body cannot lie inside its own jump table.** So read greedily
    with cheap plausibility tests, then truncate the table at the first case body
    that would otherwise fall inside it. On the Case Zero site the greedy pass
    stops at 25 entries and the containment rule independently also says 25 — two
    different arguments agreeing, which is what makes this safe to act on.

    Only tables embedded IN THE CODE SECTION get this treatment. A table in
    .data/.rdata has no following code to bound it and no containment rule to
    apply, so inferring there would be a guess; those sites are reported by
    tools/find_unlowered_switches.py for a human instead.
    """
    if not (code_lo <= tbl_base < code_hi):
        return None, None

    labels = []
    for n in range(4096):
        v = image.word(tbl_base + 4 * n)
        if v is None or v & 3 or not (code_lo <= v < code_hi):
            break
        if abs(v - bctr_addr) > NEAR_DISPATCH:
            break        # too far to be a case body of this switch
        labels.append(v)

    # A case body at address x that lies after the table start puts a hard ceiling
    # on the table's length: the table cannot reach x.
    for x in labels:
        if x > tbl_base:
            limit = (x - tbl_base) // 4
            if limit < len(labels):
                labels = labels[:limit]

    if len(labels) < 2:
        return None, None
    return len(labels), labels


def analyse_bctr(image, bctr_addr, window, code_lo=0x82150000, code_hi=0x829C3564):
    """Try to recover a jump table whose dispatch is the `bctr` at bctr_addr."""
    # The two instructions directly before a table dispatch are fixed:
    #   <load from table>; mtctr rX; bctr
    if not is_mtctr(image.word(bctr_addr - 4) or 0):
        return None
    ctr_src = rt(image.word(bctr_addr - 4))

    load = image.word(bctr_addr - 8)
    if load is None:
        return None
    if is_lwzx(load):
        kind, elem = "absolute", 4
    elif is_lhzx(load):
        kind, elem = "shortoffset", 2
    elif is_lbzx(load):
        kind, elem = "byteoffset", 1
    else:
        return None
    if rt(load) != ctr_src:
        return None            # the loaded value is not what we branch through

    base_reg, index_reg = ra(load), rb(load)

    # Recover, from the `window` instructions preceding the load: the table base
    # (lis + addi/ori into base_reg), the index shift (rlwinm producing
    # index_reg), the bounds compare, and the default label.
    #
    # We scan FORWARD over that window rather than backward from the bctr,
    # because the pieces have data dependencies (`lis` must be seen before the
    # `addi` that completes the address) and the compiler is free to interleave
    # them in any order that respects those dependencies. Scanning forward means
    # the later, closer-to-the-dispatch instruction wins when a register is
    # written twice — which is the one that actually feeds the branch.
    tbl_base = None
    lis_val = {}
    idx_reg = None
    scaled_in_place = False
    count = None
    default = None
    cr_of_check = None

    lo = bctr_addr - 8 - 4 * window
    for a in range(lo, bctr_addr - 8, 4):
        i = image.word(a)
        if i is None:
            continue

        if is_lis(i):
            lis_val[rt(i)] = (i & 0xFFFF) << 16
        elif is_addi(i) and rt(i) == base_reg and ra(i) in lis_val:
            tbl_base = (lis_val[ra(i)] + simm(i)) & 0xFFFFFFFF
        elif is_ori(i) and rt(i) == base_reg and ra(i) in lis_val:
            tbl_base = (lis_val[ra(i)] | (i & 0xFFFF)) & 0xFFFFFFFF
        elif is_rlwinm(i) and rlwinm_dest(i) == index_reg:
            # slwi rIndex, rRaw, log2(elem): the raw case number lives in rS.
            idx_reg = rlwinm_src(i)
            # ...unless the shift is IN PLACE (`rlwinm r0,r0,2`), in which case there
            # is no register left holding the raw case number — the one we switch on
            # holds n*elem. The recompiler's case constants have to say so; see the
            # `values` note below.
            scaled_in_place = rlwinm_src(i) == index_reg
        elif is_bc(i):
            kind_, cr = bc_kind(i)
            if kind_ == "gt":
                default, cr_of_check = bc_target(a, i), cr
        elif is_cmplwi(i):
            # cmplwi crF,rRaw,N  ->  N+1 cases (the check is `index > N -> default`)
            count = (i & 0xFFFF) + 1
            cmp_cr = rt(i) >> 2
            if cr_of_check is not None and cmp_cr != cr_of_check:
                count = None
            elif idx_reg is None:
                idx_reg = ra(i)

    if tbl_base is None:
        return None

    # No bounds check in the window — the compiler hoisted it. Recover the count
    # from the table's own contents rather than dropping the table on the floor,
    # which is what this scanner used to do (and the dropped table cost a crash
    # three frames away; see infer_count_absolute).
    inferred = False
    if count is None and kind == "absolute":
        count, _labels = infer_count_absolute(image, tbl_base, bctr_addr, code_lo, code_hi)
        inferred = count is not None

    if count is None or not (1 <= count <= 4096):
        return None

    labels = []
    for n in range(count):
        v = image.word(tbl_base + n * elem) if elem == 4 else None
        if kind != "absolute":
            return {"unhandled": kind, "base": bctr_addr}
        if v is None or not (0x82000000 <= v < 0x84000000):
            return None        # not a plausible code-address table: reject
        labels.append(v)

    # The case constants the recompiler must emit are the values of the register it
    # switches on, which is the raw case number ONLY when the scale landed in a
    # different register. With an in-place `rlwinm r0,r0,2` the same register holds
    # n*elem at the dispatch, and `case n:` would then be right for n = 0 alone.
    # Emitting nothing here means "the register holds the index", which is what the
    # recompiler assumes by default, so this stays a no-op for every table that was
    # already correct.
    out = {
        "base": bctr_addr,
        "r": idx_reg if idx_reg is not None else index_reg,
        "default": default or 0,
        "labels": labels,
        "table_at": tbl_base,
        "kind": kind,
        "inferred": inferred,
    }
    if scaled_in_place:
        out["values"] = [n * elem for n in range(count)]
    return out


NOP = 0x60000000  # ori r0,r0,0 — the compiler pads these into the sequence freely


def rlwinm_fields(i):
    """(rA, rS, SH, MB, ME) of an rlwinm, in IBM bit order."""
    return ((i >> 16) & 0x1F, (i >> 21) & 0x1F, (i >> 11) & 0x1F, (i >> 6) & 0x1F,
            (i >> 1) & 0x1F)


def rlwinm_shift_left(i):
    """SH if this rlwinm is a plain `slwi rA,rS,SH` (mask 0..31-SH), else None.

    This distinction matters: the same mnemonic also spells `clrlwi` (a
    zero-extend, e.g. `rlwinm rA,rS,0,24,31` for a byte) and treating that as a
    shift silently multiplies the case offsets by one."""
    _rA, _rS, sh, mb, me = rlwinm_fields(i)
    return sh if mb == 0 and me == 31 - sh else None


def analyse_bctr_offset_table(image, bctr_addr, window, code_lo=0x82150000,
                              code_hi=0x829C3564):
    """Recover the SWITCH_OFFSET idiom (see the module docstring).

    Shape:  add rT, rCodeBase, rOffset ; mtctr rT ; bctr

    with three degrees of freedom the compiler uses interchangeably:
      * the table holds bytes (lbzx) or halfwords (lhzx);
      * the scaling by 4 is applied to the *loaded value* (`rlwinm rOff,rRaw,2`)
        or is absent because the table already stores byte offsets;
      * the *index* may be pre-scaled for the load (`rlwinm rI,rIdx,1` in front
        of an lhzx), which is a scale of the index, not of the offset.
    Case i lands at rCodeBase + table[i] << shift.
    """
    if not is_mtctr(image.word(bctr_addr - 4) or 0):
        return None
    ctr_src = rt(image.word(bctr_addr - 4))

    add = image.word(bctr_addr - 8)
    if add is None or not is_add(add) or rt(add) != ctr_src:
        return None
    operands = (ra(add), rb(add))

    # Register provenance over the preceding window, forward scan (same rationale
    # as analyse_bctr: the pieces are ordered only by data dependency).
    #
    # Each register maps to a small tagged value describing what it holds, and
    # every definition builds its new value from the CURRENT values of its
    # sources before assigning. That ordering is the whole trick: the sequences
    # here overwrite registers in place (`rlwinm r0,r0,2` right after
    # `lbzx r0,r12,r31`, and `lis r12,...` reusing the table pointer's register
    # for the code base), so a scheme that resolves chains lazily at the end
    # reads either a stale or a clobbered value. Tags:
    #   ('reg', n)              unknown — just register n
    #   ('const', v)            a fully formed lis+addi/ori constant
    #   ('load', elem, tbl, ix) a table read: element size, base, index value
    #   ('scale', inner, sh)    inner << sh
    values = {}
    lis_val = {}   # reg -> pending high half of a constant
    count = None
    inferred = False
    default = None
    cr_of_check = None

    def val(reg):
        return values.get(reg, ("reg", reg))

    def index_of(reg):
        """The register the recompiler should switch on.

        A halfword table is indexed by `idx*2`, so the register feeding the load
        is a scaled copy — unwrap to the variable itself. Anything else (the
        window may have modelled the index as a constant because an unrelated
        `lis` earlier reused its register) falls back to the register we started
        from, which is always a defensible answer: it IS the register holding the
        index at the dispatch."""
        v = val(reg)
        for _ in range(8):
            if v[0] != "scale":
                break
            v = v[1]
        return v[1] if v[0] == "reg" else reg

    lo = bctr_addr - 8 - 4 * window
    for a in range(lo, bctr_addr - 8, 4):
        i = image.word(a)
        if i is None or i == NOP:
            continue
        if is_lis(i):
            lis_val[rt(i)] = (i & 0xFFFF) << 16
            values.pop(rt(i), None)
        elif is_addi(i) and ra(i) in lis_val:
            values[rt(i)] = ("const", (lis_val[ra(i)] + simm(i)) & 0xFFFFFFFF)
        elif is_ori(i) and ra(i) in lis_val:
            values[rt(i)] = ("const", (lis_val[ra(i)] | (i & 0xFFFF)) & 0xFFFFFFFF)
        elif is_rlwinm(i):
            source = val(rlwinm_src(i))
            shift = rlwinm_shift_left(i)
            # A non-shift rlwinm here is a zero-extend (clrlwi) — a move, not a
            # scale. Treating it as a scale of zero keeps the chain walkable.
            values[rlwinm_dest(i)] = ("scale", source, shift) if shift else source
        elif is_lbzx(i) or is_lhzx(i) or is_lwzx(i):
            elem = 1 if is_lbzx(i) else 2 if is_lhzx(i) else 4
            table = val(ra(i))
            values[rt(i)] = ("load", elem, table[1] if table[0] == "const" else None,
                             index_of(rb(i)))
        elif is_bc(i):
            kind_, cr = bc_kind(i)
            if kind_ == "gt":
                default, cr_of_check = bc_target(a, i), cr
        elif is_cmplwi(i):
            if cr_of_check is None or (rt(i) >> 2) == cr_of_check:
                count = (i & 0xFFFF) + 1

    if count is not None and not (1 <= count <= 4096):
        return None

    # One `add` operand is the code base, the other the (possibly scaled) offset.
    for code_reg, off_reg in (operands, operands[::-1]):
        code = val(code_reg)
        if code[0] != "const":
            continue
        offset, shift = val(off_reg), 0
        if offset[0] == "scale":
            offset, shift = offset[1], offset[2]
        if offset[0] != "load":
            continue
        _tag, elem, tbl_base, index_reg = offset
        if tbl_base is None or index_reg is None:
            continue

        code_base = code[1]
        reader = {1: image.byte, 2: image.half, 4: image.word}[elem]

        # No bounds check in the window (the compiler hoisted it) — walk the table
        # until an entry stops being a plausible case body. The same defect and the
        # same reasoning as infer_count_absolute, but the termination test is
        # different and, here, stronger: a case body is `code_base + offset`, so a
        # wrong entry usually produces an address that is not 4-byte aligned, which
        # no PPC instruction ever is. On Case Zero's one site (0x82990350) that test
        # alone ends the table, and a mis-read entry would have to be aligned AND
        # land inside the same function to slip through.
        n_inferred = count
        if n_inferred is None:
            n_inferred = 0
            while n_inferred < 4096:
                v = reader(tbl_base + n_inferred * elem)
                if v is None:
                    break
                target = (code_base + (v << shift)) & 0xFFFFFFFF
                if target & 3 or not (code_lo <= target < code_hi):
                    break
                if abs(target - code_base) > NEAR_DISPATCH:
                    break
                n_inferred += 1
            if n_inferred < 2:
                continue
            inferred = True
        count = n_inferred

        # THE KEYING (finding 38). This idiom does not dispatch on the case number: it
        # dispatches on the BYTE OFFSET the guest just loaded out of the table, in the
        # register that feeds the `add`. That register is `off_reg` and its value at the
        # dispatch is `v << shift` — frequently the *same* register the load was indexed
        # by (`lhzx r0,r12,r0`), so the index is gone by the time the branch happens.
        #
        # Emitting `case <n>:` here instead cost this project three sessions: only case 0
        # can ever match, and the recompiler's `default:` arm let the compiler drop the
        # bounds check, so a live dispatch read past the end of the host jump table and
        # jumped into an unrelated function's body. Both halves are fixed — the constants
        # here, and the honest default in recompiler.cpp.
        pairs = []       # (dispatch value, target), first occurrence wins
        seen = set()
        for n in range(count):
            v = reader(tbl_base + n * elem)
            if v is None:
                return None
            target = (code_base + (v << shift)) & 0xFFFFFFFF
            # Two cheap sanity checks that between them reject every
            # mis-identified register seen so far: PPC instructions are 4-byte
            # aligned, and a case body is in the image.
            if target & 3 or not (0x82000000 <= target < 0x84000000):
                return None
            # Repeats are normal — a guest table routes many case numbers to one body,
            # so the same offset appears many times. Equal values give equal targets by
            # construction, so keeping the first is lossless, and it MUST be done: two
            # `case` labels with the same constant is a compile error.
            value = (v << shift) & 0xFFFFFFFF
            if value in seen:
                continue
            seen.add(value)
            pairs.append((value, target))

        return {
            "base": bctr_addr,
            "r": off_reg,
            "default": default or 0,
            "labels": [t for _v, t in pairs],
            "values": [v for v, _t in pairs],
            "table_at": tbl_base,
            "kind": "offset%d" % (elem * 8),
            "inferred": inferred,
        }
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xex", help="default.xex, or a flat image from tools/xex_image_dump")
    ap.add_argument("-o", "--out", help="write TOML here (default: stdout)")
    ap.add_argument("--image-base", default="0x82000000",
                    help="guest VA the flat image starts at (ignored for a .xex)")
    ap.add_argument("--lo", default="0x82150000", help="code section start (guest VA)")
    ap.add_argument("--hi", default="0x829C3564", help="code section end (guest VA)")
    ap.add_argument("--stats", action="store_true", help="summary only, no TOML")
    args = ap.parse_args()

    img = Image(args.xex, int(args.image_base, 16))
    lo, hi = int(args.lo, 16), int(args.hi, 16)

    tables, unhandled, labels_total = [], 0, 0
    by_kind = {}
    for t in scan(img, lo, hi):
        if "unhandled" in t:
            unhandled += 1
            continue
        tables.append(t)
        labels_total += len(t["labels"])
        by_kind[t["kind"]] = by_kind.get(t["kind"], 0) + 1

    print(f"# scanned {lo:08X}..{hi:08X}", file=sys.stderr)
    for kind in sorted(by_kind):
        print(f"# {kind + ' jump tables':<26}: {by_kind[kind]}", file=sys.stderr)
    print(f"# {'total tables':<26}: {len(tables)}", file=sys.stderr)
    print(f"# {'total case labels':<26}: {labels_total}", file=sys.stderr)
    print(f"# {'unhandled':<26}: {unhandled}", file=sys.stderr)
    if args.stats:
        return

    out = ["# Generated by tools/find_jumptables.py — DO NOT EDIT BY HAND.",
           "# Regenerate with:",
           f"#   python3 tools/find_jumptables.py {args.xex} \\",
           f"#       -o {args.out or '<this file>'}",
           "#",
           "# 'kind' records which idiom produced the entry: absolute = a table of",
           "# code addresses (lwzx), offset8/offset16 = a table of byte/halfword",
           "# offsets from a code base (lbzx/lhzx + add). See the tool's docstring.",
           ""]
    for t in tables:
        out.append(f"# {t['kind']} table at 0x{t['table_at']:X}")
        out.append("[[switch]]")
        out.append(f"base = 0x{t['base']:X}")
        out.append(f"r = {t['r']}")
        out.append(f"default = 0x{t['default']:X}")
        # `values` is the dispatch value for each label, emitted only when it is NOT
        # the label's position — i.e. whenever the register the recompiler switches on
        # holds something other than the raw case number (finding 38). Its absence
        # means "the register holds the index", which is the recompiler's default.
        if t.get("values"):
            out.append("values = [")
            out += [f"    0x{v:X}," for v in t["values"]]
            out.append("]")
        out.append("labels = [")
        out += [f"    0x{l:X}," for l in t["labels"]]
        out.append("]")
        out.append("")
    text = "\n".join(out)
    if args.out:
        Path(args.out).write_text(text)
        print(f"# wrote {args.out}", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
