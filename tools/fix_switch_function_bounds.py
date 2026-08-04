#!/usr/bin/env python3
"""Repair function boundaries for switches whose case bodies fall outside the
analyzer-detected function.

Symptom (see docs/runtime.md): XenonRecomp emits

    switch (ctx.r11.u64) {
    case 0:
        // ERROR: 0x82A2F4A0
        return;

when a jump-table label lies outside the recompiled function's byte range. The function
then silently does nothing for those cases — Fable2's allocator, among ~2,300 other case
labels, hits this and corrupts itself.

Root cause: the XEX's .pdata (and XenonAnalyse's splitting) treats the inline jump-table
DATA as a function boundary, so the case bodies land in separate tiny "functions" and the
switch's home function ends at the bctr.

Fix: emit manual `functions = [{ address, size }]` entries for XenonRecomp's main TOML.
Manual entries are registered before .pdata parsing, so they take precedence. The correct
extent is computed to a fixpoint: extend the function to cover every label of every
switch table whose bctr lies inside the current range (labels can pull in further
switches).

Usage:
    python3 tools/fix_switch_function_bounds.py            # prints the TOML block
    python3 tools/fix_switch_function_bounds.py --apply    # patches config/CaseZero.toml

Provenance: copied from ~/GithubRepo/Asuras_Wrath_Xenon_Recomp/tools (which took it from
Fable2XenonRecomp); only the repo-specific paths and the .text bounds differ.
"""
import bisect
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PPC = os.path.join(REPO, "ppc")
MAIN_TOML = os.path.join(REPO, "config", "CaseZero.toml")
SWITCH_TOML = os.path.join(REPO, "config", "CaseZero_switch_tables.toml")

# Case Zero's .text spans 0x82150000..0x829C3564 (from the section map dumped by
# tools/xex_image_dump). Labels outside this are bogus jump-table detections in
# data-as-code stretches — widening a function to cover them would swallow half the
# image, so they are reported and skipped rather than acted on.
CODE_BEGIN = 0x82150000
CODE_END = 0x829C3564


def valid_code_addr(a):
    return CODE_BEGIN <= a < CODE_END


def parse_function_addresses():
    addrs = []
    with open(os.path.join(PPC, "ppc_func_mapping.cpp")) as f:
        for m in re.finditer(r"\{ 0x([0-9A-Fa-f]+), sub_", f.read()):
            addrs.append(int(m.group(1), 16))
    addrs.sort()
    return addrs


def parse_switches():
    """[(base, [labels...]), ...] from the switch-table TOML."""
    switches = []
    with open(SWITCH_TOML) as f:
        text = f.read()
    for m in re.finditer(
        r"\[\[switch\]\]\s*base\s*=\s*0x([0-9A-Fa-f]+)\s*r\s*=\s*\d+\s*"
        r"(?:default\s*=\s*0x([0-9A-Fa-f]+)\s*)?labels\s*=\s*\[([^\]]*)\]",
        text,
    ):
        base = int(m.group(1), 16)
        labels = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(3))]
        if m.group(2):
            labels.append(int(m.group(2), 16))
        switches.append((base, labels))
    return switches


def find_error_functions():
    """{func_addr: max_error_label} scanned from the generated sources."""
    result = {}
    func_re = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)")
    err_re = re.compile(r"// ERROR: 0x([0-9A-F]+)")
    for path in glob.glob(os.path.join(PPC, "ppc_recomp.*.cpp")):
        current = None
        with open(path) as f:
            for line in f:
                fm = func_re.search(line)
                if fm:
                    current = int(fm.group(1), 16)
                    continue
                em = err_re.search(line)
                if em and current is not None:
                    target = int(em.group(1), 16)
                    if valid_code_addr(target):
                        lo, hi = result.get(current, (None, 0))
                        lo = target if lo is None else min(lo, target)
                        result[current] = (lo, max(hi, target))
                    else:
                        result.setdefault(current, (None, 0))
    return result


def main():
    funcs = parse_function_addresses()
    switches = parse_switches()
    errors = find_error_functions()

    def next_func_after(addr):
        i = bisect.bisect_right(funcs, addr)
        return funcs[i] if i < len(funcs) else addr + 4

    # Merge with entries already in the TOML (the script runs iteratively: fixing one
    # round of boundaries can surface the next round in the regenerated sources).
    existing = {}
    with open(MAIN_TOML) as f:
        toml = f.read()
    block_match = re.search(r"^functions = \[(.*?)^\]\n", toml, re.M | re.S)
    if block_match:
        for m in re.finditer(r"address = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+)",
                             block_match.group(1)):
            existing[int(m.group(1), 16)] = int(m.group(2), 16)

    def func_containing(addr):
        i = bisect.bisect_right(funcs, addr)
        return funcs[i - 1] if i > 0 else addr

    entries = []
    skipped = []
    for start, (min_label, max_label) in sorted(errors.items()):
        if max_label == 0:
            skipped.append(start) # only bogus labels: data-as-code, leave untouched
            continue
        # Labels may point backward: the analyzer split one original function into
        # fragments. Extend the start down to the function containing the lowest label.
        if min_label is not None and min_label < start:
            start = func_containing(min_label)
        end = next_func_after(max(start, max_label))
        # Fixpoint: cover the labels of every switch whose bctr is inside the range.
        while True:
            new_end = end
            for base, labels in switches:
                if start <= base < new_end:
                    for label in labels:
                        if valid_code_addr(label) and label >= new_end:
                            new_end = max(new_end, next_func_after(label))
            if new_end == end:
                break
            end = new_end
        entries.append((start, end - start))
    for addr in skipped:
        print("# skipped sub_%08X (only garbage labels; data-as-code)" % addr, file=sys.stderr)

    newCount = sum(1 for a, s in entries if a not in existing)
    for addr, size in entries:
        existing[addr] = max(size, existing.get(addr, 0))
    entries = sorted(existing.items())
    print("# %d new this round" % newCount, file=sys.stderr)

    lines = ["functions = ["]
    for addr, size in entries:
        lines.append("    { address = 0x%08X, size = 0x%X }," % (addr, size))
    lines.append("]")
    block = "\n".join(lines)

    print("# %d functions with out-of-bounds switch cases" % len(entries))
    if "--apply" in sys.argv:
        if re.search(r"^functions\s*=", toml, re.M):
            # Replace the existing generated block (from 'functions = [' to its ']').
            toml = re.sub(r"^functions = \[[^\]]*\]\n", block + "\n", toml, flags=re.M | re.S)
        else:
            # `functions` belongs to the top-level [main] table: insert after its header.
            toml = toml.replace("[main]\n", "[main]\n" + block + "\n", 1)
        with open(MAIN_TOML, "w") as f:
            f.write(toml)
        print("patched", MAIN_TOML)
    else:
        print(block)


if __name__ == "__main__":
    main()
