#!/usr/bin/env python3
"""Find direct branches that XenonRecomp silently dropped, and prune the config
entries responsible for the repairable class.

WHY THIS EXISTS
---------------
XenonRecomp lowers a direct branch (`b` / `bl` / `beq` ...) whose target is
outside the current function by looking the target up in the image's symbol
table. If the target is not the *start* of a known function, it cannot emit a
call, so it emits a bare comment and moves on:

        println("\\t// ERROR {:X}", address);      // recompiler.cpp

That is a silently dropped branch. No diagnostic is printed to the recompiler's
stdout, the run still exits 0, and the generated C++ compiles cleanly — the
control transfer simply does not happen at runtime, and execution falls through
into whatever follows. It is the same class of defect as an unimplemented
mnemonic (wrong execution, no build failure), but nothing in this repo was
measuring it, and the bootstrap notes claimed "zero `// ERROR:` comments" while
grepping for a colon the recompiler never emits. A pattern that cannot match is
not a clean result; it is an unrun check.

WHAT IT MEASURES, AND THE TWO SIGNATURES
----------------------------------------
Every dropped branch means a function boundary is wrong. Which boundary depends
entirely on the branch *direction*, and the two cases have opposite repairs:

  BACKWARD (target < enclosing function's start)
      A loop header inside a real function was declared to be its own function,
      splitting it. The tail half is now a separate `sub_`, and its loop-back
      edge points into the head half, which is a different function -> dropped.
      Repair: remove the spurious start. The two halves merge back.

      On Case Zero all 9 of these came from `coverage_to_function_overrides.py`.
      Xenia's coverage trace calls every executed branch target a "function", and
      a loop header is a branch target -- the identical trap that tool already
      documents for jump-table case labels (see its docstring), reappearing in a
      form the case-label filter does not cover, because a loop header is not in
      any switch table.

  FORWARD (target > enclosing function's start)
      The branching function was truncated: the target is real code belonging to
      the same logical function, but the function was ended early and a new one
      started at or before the target. On Case Zero all of these come from the
      XEX's `.pdata` table, not from linear sweep -- the compiler outlined cold
      blocks and `.pdata` describes each outlined region as its own entry, so a
      `beq` into a cold block is a forward branch to a non-entry address.

      Repair: widen the *branching* function until the target falls inside it.
      XenonRecomp only routes a branch through the symbol table when the target
      is outside the current function:

          if (target < fn.base || target >= fn.base + fn.size)   // -> ERROR path

      so widening converts the dropped branch into an ordinary local `goto`. It
      does NOT and cannot suppress the `.pdata` entries in between -- the symbol
      table's find() is an exact-start lookup, so a spanning entry does not hide
      a nested one -- which means those addresses end up emitted both inside the
      widened function and as their own `sub_`. That duplication is already the
      accepted outcome of `fix_switch_function_bounds.py` widening a switch
      parent over its case labels; it costs code size, not correctness.

The direction test is what makes the pruning safe: this tool only ever removes a
config entry when the evidence is a backward edge into a function that starts
earlier, which is the unambiguous split signature. It never touches an entry
implicated only by a forward branch.

USAGE
    find_dropped_branches.py                       # report against ppc/
    find_dropped_branches.py --prune               # remove split entries from
                                                   # config/CaseZero.toml
    find_dropped_branches.py --widen               # widen truncated functions

Run one repair at a time and regenerate `ppc/` between them: both repairs change
function boundaries, so the second one's evidence is only valid after the first
one's rebuild.

`ppc/` must be current for the config being examined -- regenerate first, or the
report describes a build that no longer exists. Same stale-artifact trap as the
coverage oracle.
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PPC_DIR = REPO / "ppc"
CONFIG = REPO / "config" / "CaseZero.toml"

FUNC_RE = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
ERR_RE = re.compile(r"//\s*ERROR\s+([0-9A-F]{8})\b")


def scan(ppc_dir):
    """Return [(enclosing_function, dropped_target, file, line)] for every
    `// ERROR` the recompiler emitted, in source order."""
    hits = []
    for path in sorted(ppc_dir.glob("*.cpp")):
        current = None
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            m = FUNC_RE.search(line)
            if m:
                current = int(m.group(1), 16)
                continue
            m = ERR_RE.search(line)
            if m:
                # A dropped branch outside any function body would mean the
                # emitter changed shape; surface it rather than assume.
                if current is None:
                    print(f"  !! {path.name}:{lineno} ERROR outside a function",
                          file=sys.stderr)
                    continue
                hits.append((current, int(m.group(1), 16), path.name, lineno))
    return hits


def config_addresses(text):
    return {int(a, 16) for a in re.findall(r"address\s*=\s*(0x[0-9A-Fa-f]+)", text)}


def function_starts(ppc_dir):
    """Every function start in the current build, sorted. Read from the emitted
    code rather than the config, because most functions come from `.pdata` and
    linear sweep and never appear in the config at all."""
    starts = set()
    for path in ppc_dir.glob("*.cpp"):
        starts.update(int(a, 16) for a in FUNC_RE.findall(path.read_text()))
    return sorted(starts)


def widen(text, sizes):
    """Merge `{address: size}` into the config's `functions` list, keeping the
    larger size where an entry already exists. Same merge rule as
    fix_switch_function_bounds.py and coverage_to_function_overrides.py, so the
    three tools compose in any order instead of overwriting each other."""
    remaining = dict(sizes)
    out = []
    for line in text.splitlines(keepends=True):
        m = re.search(r"address\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)",
                      line)
        if m:
            addr, cur = int(m.group(1), 16), int(m.group(2), 16)
            if addr in remaining:
                new = max(cur, remaining.pop(addr))
                line = line.replace(m.group(2), f"0x{new:X}")
        out.append(line)
    text = "".join(out)
    if remaining:
        # Append the entries that had no existing line, just before the closing
        # bracket of the functions list.
        added = "".join(f"    {{ address = 0x{a:X}, size = 0x{s:X} }},\n"
                        for a, s in sorted(remaining.items()))
        idx = text.rindex("]")
        text = text[:idx] + added + text[idx:]
    return text


def prune(text, drop):
    """Remove the `{ address = ..., size = ... }` entries whose address is in
    `drop`. Entries are one per line in the generated config, which is what
    coverage_to_function_overrides.py and fix_switch_function_bounds.py both
    emit; anything else is left alone and reported."""
    out, removed = [], set()
    for line in text.splitlines(keepends=True):
        m = re.search(r"address\s*=\s*(0x[0-9A-Fa-f]+)", line)
        if m and int(m.group(1), 16) in drop and line.lstrip().startswith("{"):
            removed.add(int(m.group(1), 16))
            continue
        out.append(line)
    return "".join(out), removed


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ppc-dir", type=Path, default=PPC_DIR)
    ap.add_argument("--config", type=Path, default=CONFIG)
    ap.add_argument("--prune", action="store_true",
                    help="remove split-function entries from the config")
    ap.add_argument("--widen", action="store_true",
                    help="widen truncated functions to cover their targets")
    args = ap.parse_args()
    if args.prune and args.widen:
        ap.error("run --prune and --widen separately, regenerating ppc/ between "
                 "them; see the docstring")

    hits = scan(args.ppc_dir)
    if not hits:
        print("No dropped branches. (If this is the first run, confirm the "
              "check can fail -- see the docstring.)")
        return 0

    backward = [(f, t) for f, t, _, _ in hits if t < f]
    forward = [(f, t) for f, t, _, _ in hits if t > f]
    # A branch to the function's own start is a self tail-call, not a boundary
    # bug; count it separately rather than folding it into either class.
    selfcall = [(f, t) for f, t, _, _ in hits if t == f]

    print(f"{len(hits)} dropped direct branches "
          f"({len({t for _, t, _, _ in hits})} distinct targets)\n")

    cfg_text = args.config.read_text()
    cfg_addrs = config_addresses(cfg_text)

    split_fns = sorted({f for f, _ in backward})
    print(f"BACKWARD / split function      : {len(backward)} branches, "
          f"{len(split_fns)} functions")
    for f in split_fns:
        tgts = sorted({t for g, t in backward if g == f})
        where = "config entry" if f in cfg_addrs else "from Analyse(), NOT in config"
        print(f"    sub_{f:08X}  -> {' '.join(f'{t:08X}' for t in tgts)}   [{where}]")

    trunc_fns = sorted({f for f, _ in forward})
    print(f"\nFORWARD / truncated function   : {len(forward)} branches, "
          f"{len(trunc_fns)} functions   (reported only, not auto-repaired)")
    for f in trunc_fns:
        tgts = sorted({t for g, t in forward if g == f})
        where = "config entry" if f in cfg_addrs else "from Analyse(), NOT in config"
        print(f"    sub_{f:08X}  -> {' '.join(f'{t:08X}' for t in tgts)}   [{where}]")

    if selfcall:
        print(f"\nSELF (target == function start): {len(selfcall)}")

    if args.widen:
        starts = function_starts(args.ppc_dir)
        import bisect
        sizes, notes = {}, []
        for f in trunc_fns:
            far = max(t for g, t in forward if g == f)
            # End the widened function where the function containing the
            # furthest target ends -- i.e. at the next start strictly after it.
            # Stopping at the target itself would leave the rest of the outlined
            # block outside, so a fallthrough past it would run off the end.
            i = bisect.bisect_right(starts, far)
            if i >= len(starts):
                notes.append(f"sub_{f:08X}: no function starts after {far:08X}")
                continue
            end = starts[i]
            sizes[f] = end - f
            notes.append(f"    sub_{f:08X}  size -> 0x{end - f:X}  "
                         f"(covers {far:08X}, ends at {end:08X})")
        print("\nWidening proposals:")
        for n in notes:
            print(n)
        if sizes:
            args.config.write_text(widen(args.config.read_text(), sizes))
            print(f"\nWidened {len(sizes)} functions in {args.config.name}.")
            print("Regenerate ppc/ and re-run this tool to confirm.")
        return 0

    if not args.prune:
        return 0

    # Only prune what the config actually owns. A split function that came from
    # Analyse() cannot be removed by editing the config, and pretending
    # otherwise would report a repair that did not happen.
    droppable = {f for f in split_fns if f in cfg_addrs}
    skipped = [f for f in split_fns if f not in cfg_addrs]
    if skipped:
        print(f"\nNOT prunable (not config entries): "
              f"{' '.join(f'sub_{f:08X}' for f in skipped)}")
    if not droppable:
        print("\nNothing to prune.")
        return 0

    new_text, removed = prune(cfg_text, droppable)
    missed = droppable - removed
    if missed:
        print(f"\n!! matched but not removed (unexpected line shape): "
              f"{' '.join(f'sub_{f:08X}' for f in missed)}", file=sys.stderr)
    args.config.write_text(new_text)
    print(f"\nPruned {len(removed)} split-function entries from {args.config.name}.")
    print("Regenerate ppc/ and re-run this tool to confirm the backward class "
          "is empty.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
