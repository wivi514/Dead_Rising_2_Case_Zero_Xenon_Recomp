#!/usr/bin/env python3
"""Show every place the recompiled image calls a kernel import, with the guest code
that consumes the result.

WHY THIS EXISTS
---------------
Implementing a kernel import needs its *return value*, and the capture does not
contain one. Xenia's level-3 log prints an import's arguments on entry — pointer
arguments as `ADDR(value)`, where `value` is what was there BEFORE the call — and for
the XAM and NetDll surface it prints no `= result` line at all. So A1 gives arity,
argument shapes and call order, and says nothing about what to return.

The image does. The recompiled C++ emits `__imp__<Name>(ctx, base);` inline at every
call site with the original PPC as comments, so the instructions immediately after the
call ARE the specification: what the title compares the result against, which branch
each answer takes, and which out-parameter it reads back. That is how
docs/phase1-notes.md finding 28 established that XamGetSystemVersion must return a
value below 0x0008A100 — seven `cmplw` sites said so, unambiguously.

This tool is that lookup, so the next twenty imports do not each need a hand-rolled
grep. It is deliberately dumb: it prints code and does not interpret it. Deciding what
a title wants back is a reading job, and a tool that guessed would be worse than none.

USAGE
    import_call_sites.py XamUserGetName            # all call sites, 24 lines of use
    import_call_sites.py XamUserGetName -n 40      # more context
    import_call_sites.py --list-stubs              # which imports are still stubs

NOTE ON `values`: the lines printed are the recompiler's own comments, i.e. the guest
instructions, not our C++. Read the comments; the C++ under them is just the lowering.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PPC = ROOT / "ppc"
STUBS = ROOT / "runtime/kernel/import_stubs.cpp"


def call_sites(name, after):
    """Yield (file, index-in-file, snippet) for every `__imp__<name>(ctx, base);`.

    Matched with word boundaries: without them `XamUserGetXUID` also matches inside
    nothing today, but `NetDll_select` would match `NetDll_selectX` the moment a title
    imported one, and a silently over-broad match here would send someone reading the
    wrong function's code.
    """
    call = re.compile(r"^\t__imp__" + re.escape(name) + r"\(ctx, base\);$")
    func = re.compile(r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]{8})\)")
    for path in sorted(PPC.glob("ppc_recomp.*.cpp")):
        lines = path.read_text().split("\n")
        owner = "?"
        for i, line in enumerate(lines):
            m = func.search(line)
            if m:
                owner = m.group(1)
            if call.match(line):
                yield path.name, owner, lines[max(0, i - 2):i + after]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", nargs="?", help="import name, without the __imp__ prefix")
    ap.add_argument("-n", "--after", type=int, default=24,
                    help="lines of consuming code to print (default 24)")
    ap.add_argument("--list-stubs", action="store_true",
                    help="list imports still served by a generated honest-failure stub")
    args = ap.parse_args()

    if args.list_stubs:
        names = re.findall(r"PPC_FUNC\(__imp__([A-Za-z0-9_]+)\)", STUBS.read_text())
        for n in sorted(names):
            print(n)
        print(f"\n{len(names)} generated stubs", file=sys.stderr)
        return 0

    if not args.name:
        ap.error("give an import name, or --list-stubs")

    found = 0
    for fname, owner, snippet in call_sites(args.name, args.after):
        found += 1
        print(f"===== {args.name} call site {found}  ({fname}, in {owner})")
        for line in snippet:
            print("   " + line)
        print()

    if not found:
        # Not an error: plenty of imports in the IAT are never called on any path.
        # Saying so explicitly beats printing nothing, which reads like a broken tool.
        print(f"no call site for __imp__{args.name} in the generated image.\n"
              f"Either the name is wrong or the title imports it without calling it "
              f"(the IAT is the authority on imports, not on calls).", file=sys.stderr)
    else:
        print(f"{found} call site(s)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
