#!/usr/bin/env python3
"""Find `bctr` dispatches that XenonRecomp did NOT lower to a `switch` — the
defect class that leaks a callee's non-volatile registers into its caller.

WHY THIS EXISTS
---------------
When XenonRecomp meets a `bctr` it cannot resolve to a jump table, it emits

    PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    return;

That is a *call* to the case body followed by a return that skips the function's
epilogue. Nothing warns. The C++ compiles. The case body even computes the right
answer, because the recompiler hands it the same PPCContext. The damage is that
`__restgprlr_14` never runs, so the caller resumes with the callee's r14..r31.

On Case Zero this was ONE site — the `bctr` at 0x82955A94 inside sub_82955780 —
and it presented as a segfault on guest address 0 in a completely different
function, three frames away: sub_829565B8 did `lwz r8,4(r31)` with r31 still
holding sub_82955780's value, read a word that happened to be zero, and loaded a
matrix through it. Nothing in the crash pointed at the switch.
The defect class is docs/xenia-capture-analysis.md section 15; how it was
run to ground is docs/phase1-notes.md finding 27.

WHY tools/find_jumptables.py's OWN OUTPUT IS NOT THE CHECK
-----------------------------------------------------------
A scanner reports what it found; it cannot report what it silently rejected, and
this one rejected the site above for a mundane reason (no `cmplwi` bounds check
within its search window — the compiler had hoisted it out of the loop). "232
tables found" reads exactly the same whether the true number is 232 or 234.
Gotcha 3, again: a zero — or any number — from a detector is a detection result,
not a fact about the title.

So this tool asks the opposite question directly against the image: which `bctr`
sites *look like* a table dispatch and are absent from the emitted TOML?

NOT EVERY UNLOWERED bctr IS A BUG
----------------------------------
The same shape is also how a compiler writes a computed *tail call*: a small,
frameless thunk that loads a function pointer out of a table and jumps to it.
There, `call; return;` is behaviourally identical to the jump — no frame to pop,
no non-volatiles saved, and the callee returns straight through. Case Zero has
two of these (sub_8296CB28 and sub_8296CBA0, both dispatching into tables of real
function starts in .data).

The discriminator is whether the generated function containing the dispatch uses
a register save ladder. If it saved r14..r31, skipping the epilogue destroys them
and the site is a DEFECT; if it saved nothing, there is nothing to destroy. That
is read out of the generated C++ rather than guessed, so it stays true as the
function list changes.

"CONTAINING", NOT "NEAREST PRECEDING START"
--------------------------------------------
Generated functions OVERLAP. The coverage oracle injects branch targets it saw
executed as function starts (gotcha 21), so a case body or a loop header inside a
real function gets its own `sub_`, and the recompiler emits both — the whole
function AND the fragment. Picking the nearest preceding start therefore answers
a different question than the one asked: at Case Zero's real defect the nearest
start was `sub_82955A78`, a 0x1C-byte coverage fragment with no save ladder, and
this tool's first version duly reported the site as benign. The function that
actually loses its registers is `sub_82955780`, which also emits that same `bctr`
2 KB into its body.

So the containment test reconstructs each emitted instruction's guest address —
walking the body, anchoring on the `loc_XXXXXXXX:` labels the recompiler emits
and advancing 4 bytes per instruction comment — and reports EVERY function whose
emitted stream includes the dispatch. If any of them saves registers, it is a
defect.

USAGE
    find_unlowered_switches.py                       # report; exit 1 on any defect
    find_unlowered_switches.py --all                 # include the benign thunks

Run it after ANY change to the function list or the switch tables, in the same
spirit as tools/find_dropped_branches.py — both measure a silent failure of the
recompiler that only shows up as a crash somewhere unrelated.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import find_jumptables as J  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
IMAGE = ROOT / "assets/game/default_image.bin"
TABLES = ROOT / "config/CaseZero_switch_tables.toml"
PPC = ROOT / "ppc"
IMAGE_BASE = 0x82000000
TEXT_LO, TEXT_HI = 0x82150000, 0x82150000 + 0x873564

# The generated form of an unlowered `bctr`, as the two source lines that follow
# the `// bctr` comment. Compared after stripping indentation rather than as one
# literal block: the emitter's exact leading whitespace is not something this check
# should depend on, and depending on it made the first version match nothing at all
# while still printing a confident, wrong "0 defects".
UNLOWERED = ("PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);", "return;")
SAVE_LADDER = re.compile(r"__save(?:gprlr|fpr|vmx)_\d+")


def read_image():
    data = IMAGE.read_bytes()

    def word(addr):
        off = addr - IMAGE_BASE
        return int.from_bytes(data[off:off + 4], "big") if 0 <= off <= len(data) - 4 else None

    return word


def lowered_bctr_addresses():
    """Every `bctr` the switch-table TOML claims to have lowered.

    `base` in that file is the address of the branch itself, which is exactly the
    key this tool needs — no address arithmetic, so no chance of an off-by-one
    that would silently mark a real site as covered.
    """
    return {int(m.group(1), 16)
            for m in re.finditer(r"^base = 0x([0-9A-Fa-f]+)", TABLES.read_text(), re.M)}


LOC_LABEL = re.compile(r"^\tloc_([0-9A-F]{8}):")
INSN_COMMENT = re.compile(r"^\t// (?!ERROR)")


def unlowered_sites_in(body, start):
    """Guest addresses inside one generated body that emit the unlowered form.

    The recompiler prints one `\\t// <mnemonic>` comment per guest instruction and a
    `\\tloc_XXXXXXXX:` label at every branch target, so walking the body and adding 4
    per comment — resynchronising on each label — reconstructs the address of every
    instruction. The labels matter: without them any single miscount would shift the
    rest of the function, and with them a miscount can only ever affect one basic
    block. `// ERROR` comments are excluded because they are the recompiler's
    dropped-branch marker, not an instruction (see tools/find_dropped_branches.py).
    """
    found = set()
    addr = start
    lines = body.split("\n")
    for idx, line in enumerate(lines):
        label = LOC_LABEL.match(line)
        if label:
            addr = int(label.group(1), 16)
            continue
        if not INSN_COMMENT.match(line):
            continue
        if line.strip() == "// bctr" and \
                tuple(x.strip() for x in lines[idx + 1:idx + 3]) == UNLOWERED:
            found.add(addr)
        addr += 4
    return found


def generated_bodies():
    """{guest address: generated body} for every function in ppc/.

    Read once into memory (about 156 MB of C++) rather than grepped per site: there
    are only ever a handful of sites, but a grep per site over 227 files costs more
    than the single pass.
    """
    bodies = {}
    for path in sorted(PPC.glob("ppc_recomp.*.cpp")):
        src = path.read_text()
        hits = [(m.start(), int(m.group(1), 16))
                for m in re.finditer(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)", src)]
        for i, (pos, addr) in enumerate(hits):
            end = hits[i + 1][0] if i + 1 < len(hits) else len(src)
            bodies[addr] = src[pos:end]
    return bodies


def switch_shaped(word, addr):
    """True if the `bctr` at addr is fed by a table read rather than a plain
    pointer. `<load or add>; mtctr rX; bctr` with the destination of the load
    being the register that reaches CTR — the part of the idiom that cannot vary
    however the compiler schedules the setup."""
    prev, prev2 = word(addr - 4), word(addr - 8)
    if prev is None or prev2 is None or not J.is_mtctr(prev):
        return False
    if not (J.is_lwzx(prev2) or J.is_lhzx(prev2) or J.is_lbzx(prev2) or J.is_add(prev2)):
        return False
    return J.rt(prev2) == J.rt(prev)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true",
                    help="also list the benign frameless-thunk dispatches")
    args = ap.parse_args()

    word = read_image()
    lowered = lowered_bctr_addresses()
    bodies = generated_bodies()

    total = 0
    candidates = []
    for addr in range(TEXT_LO, TEXT_HI, 4):
        if word(addr) != J.BCTR:
            continue
        total += 1
        if switch_shaped(word, addr) and addr not in lowered:
            candidates.append(addr)

    # One pass over every generated body, collecting which functions emit an
    # unlowered dispatch at each candidate address. Functions overlap, so this is a
    # set per site, not a single owner.
    owners = {a: [] for a in candidates}
    wanted = set(candidates)
    reconstructed = misplaced = 0
    for start, body in bodies.items():
        sites = unlowered_sites_in(body, start)
        for site in sites:
            reconstructed += 1
            # SELF-CHECK. Every address this reconstruction produces must hold a
            # `bctr` in the image. If the walk ever drifts, this is where it shows
            # up — otherwise a drifting walk quietly finds nothing and the tool
            # reports "0 defects" with total confidence (gotcha 25).
            if word(site) != J.BCTR:
                misplaced += 1
            if site in wanted:
                owners[site].append((start, sorted(set(SAVE_LADDER.findall(body)))))

    defects, benign = [], []
    for addr in candidates:
        holders = sorted(owners[addr])
        # A function that saved non-volatiles and reaches a bare `return;` after an
        # indirect call cannot restore them. A function that saved nothing has
        # nothing to lose, and the call-then-return is a faithful tail jump.
        (defects if any(saved for _s, saved in holders) else benign).append((addr, holders))

    print(f"unlowered dispatches found in ppc/  : {reconstructed}")
    if misplaced:
        print(f"  *** {misplaced} of them are NOT at a `bctr` in the image — the address")
        print("      reconstruction has drifted and every result below is unreliable.")
    print(f"bctr sites in .text                 : {total}")
    print(f"lowered to a switch (TOML)          : {len(lowered)}")
    print(f"switch-shaped but NOT lowered       : {len(candidates)}")
    print(f"  of those, DEFECTS (save ladder)   : {len(defects)}")
    print(f"  of those, benign frameless thunks : {len(benign)}")

    for addr, holders in defects:
        print(f"\nDEFECT  bctr {addr:08X}")
        if not holders:
            print("        no generated function emits this dispatch — ppc/ is stale")
            print("        relative to config/. Regenerate it and re-run.")
        for start, saved in holders:
            where = f"sub_{start:08X} (+0x{addr - start:X})"
            if saved:
                print(f"        {where} saves {', '.join(saved)} and never restores them")
                print("          on this path — its caller resumes with the callee's r14..r31.")
            else:
                print(f"        {where} also emits it, frameless (harmless by itself)")
        print("        Fix: recover this table in tools/find_jumptables.py, then rerun")
        print("        the four-tool pipeline in CLAUDE.md before trusting anything.")

    if args.all:
        for addr, holders in benign:
            names = ", ".join(f"sub_{s:08X}" for s, _ in holders) or "(none)"
            print(f"\nbenign  bctr {addr:08X}  in {names}")
            print("        no save ladder in any containing function — a frameless")
            print("        dispatch thunk, where call-then-return equals the tail jump.")

    return 1 if defects else 0


if __name__ == "__main__":
    sys.exit(main())
