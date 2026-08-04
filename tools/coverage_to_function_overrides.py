#!/usr/bin/env python3
"""Turn a Xenia function-coverage trace into XenonRecomp `[main].functions`
overrides — i.e. use hardware execution as a *function-discovery oracle*.

THE IDEA
--------
XenonRecomp finds functions statically: it follows direct calls, then linearly
sweeps whatever is left. Anything reached only through a computed branch — a
switch whose jump table the analyser failed to recognise, a vtable slot, a
function-pointer table built at runtime — is invisible to it. The generated code
still contains those instructions (they fall inside whatever function swallowed
them), but there is no `PPC_FUNC_IMPL(__imp__sub_<addr>)` entry, so
`PPC_CALL_INDIRECT_FUNC(addr)` at runtime finds nothing and the call dies.

Xenia's `--trace_function_data` writes one record per guest function it actually
executed, with the function's start AND end address. That is exactly the missing
information. Diffing "executed on hardware" against "emitted by XenonRecomp"
gives a precise, zero-guesswork list of functions to force into the config:

    [[main.functions]]  ->  functions = [{ address = 0x82242BB8, size = 0x4C }, ...]

WHAT IT FILTERS OUT
-------------------
Two categories of address legitimately appear in the trace with no `sub_` entry,
and adding them would be wrong:

  * XEX import thunks — 4-word stubs that the loader patches. XenonRecomp routes
    these through the kernel HLE seam instead of recompiling them. **They look
    different in a loaded image than in the XEX on disk**; see
    `find_import_thunks`, which is where this tool most easily goes silently
    wrong.
  * The register save/restore ladders (`__savegprlr_14` … `__restvmx_127`).
    XenonRecomp synthesises these itself from the `*_address` config keys; every
    4-byte entry point in a ladder shows up in the trace as its own "function".
  * **Jump-table case labels.** This is the big one and it is not obvious; see
    below.

WHY CASE LABELS MUST BE EXCLUDED  (Case Zero: 870 of 1,090 candidates)
----------------------------------------------------------------------
Xenia's function analysis calls any branch target it sees executed a "function".
A `switch` case body is a branch target. So a recovered jump table's case labels
arrive in the coverage trace looking exactly like undiscovered entry points —
and on Case Zero they were 80% of the raw candidate list.

Adding one is not merely useless, it is actively destructive. XenonRecomp lowers
a recognised jump table to a `switch` *inside the parent function*, with the case
bodies as labels in that same function. Declaring a case label to be its own
function **splits the parent there**, so every case body at or past the split
falls outside the parent's extent and XenonRecomp lowers it to a bare `return;` —
no epilogue, caller resumes with the callee's non-volatiles. The exact defect
`fix_switch_function_bounds.py` exists to repair, reintroduced by the tool that
was supposed to improve coverage.

It also does not converge: `fix_switch_function_bounds.py` widens the parent to
cover its labels, the coverage entry re-splits it, and the two tools fight to a
stable non-zero error count (8, on Case Zero, from 4 parent functions).

So every address that appears as a `label` in the switch-table TOML is dropped.
The residual risk is an address that is BOTH a case label and a genuine
indirect-call target — it would be excluded and its indirect call would miss at
runtime. That is a visible, localisable failure (an indirect-call miss naming a
specific address), whereas the alternative is silent wrong execution in the
parent. If a runtime miss ever lands on one of these, that is the signal to
special-case it, not to drop the filter.

CASE LABELS ARE NOT THE ONLY MID-BODY TRAP -- ALWAYS RUN THE DROPPED-BRANCH CHECK
---------------------------------------------------------------------------------
The case-label filter below only knows about addresses that appear in the switch
TOML. A **loop header** is also a branch target, is also recorded by Xenia as a
"function", and is in no switch table -- so it sails straight through. On Case
Zero nine such addresses were added here and split nine real functions, which
turned their loop-back edges into silently dropped branches.

No heuristic available at this point reliably separates a loop header from a
genuine indirect-call target: of those nine, two pairs shared an end address
(the signature this tool's notes already describe), three were absurdly small,
and two looked entirely ordinary. What does separate them is a *measurement*
taken after the fact -- did adding this address cause a branch to be dropped?

So this tool proposes and `tools/find_dropped_branches.py` disposes. After
applying overrides from here, ALWAYS regenerate and run:

    python3 tools/find_dropped_branches.py --prune     # then regenerate again

Treat a nonzero backward/split count there as this tool's error, not as a
separate problem.

USAGE
    coverage_to_function_overrides.py --trace <trace.0> [--trace <trace.0> ...] \
        --ppc-dir ppc/ --image assets/game/default_image.bin \
        --config config/CaseZero.toml

Provenance: copied from ~/GithubRepo/Asuras_Wrath_Xenon_Recomp/tools and adapted --
reads a flat loaded image instead of decrypting the XEX (Case Zero's is devkit-key
encrypted and LZX-compressed, which decrypt_xex.py cannot read), bounds the executed
set to our image, and scans the whole image for import thunks instead of a
title-specific window.
"""
import argparse
import bisect
import re
import struct
import subprocess
import sys
from pathlib import Path

REC = 48                # coverage record size, bytes
IMAGE_BASE = 0x82000000
IMAGE_END = 0x82B40000  # base + PPC_IMAGE_SIZE (0xB40000)
MTCTR_R11 = 0x7D6903A6
BLR = 0x4E800020
NOP = 0x60000000
BCTR = 0x4E800420


def read_coverage(path):
    """Return {start: end} for every function in a Xenia coverage trace.

    The file is a preallocated buffer (32 MiB here) zero-padded past the real
    data, so we trim the zero tail first. Records are little-endian; a valid one
    starts with its own length (0x30) and a plausible guest code address. When a
    record fails that check we step 4 bytes and resync rather than bailing —
    Xenia occasionally leaves small gaps between flushes.
    """
    data = Path(path).read_bytes()
    end = len(data)
    while end > 0 and data[end - 1] == 0:
        end -= 1

    funcs, off = {}, 0
    while off + REC <= end:
        f = struct.unpack_from("<12I", data, off)
        # Reject anything outside OUR image. Case Zero's C1 trace spans
        # 0x80050030..0x829C3554: the low addresses are the kernel/xam modules
        # Xenia also traces, and they are not ours to recompile.
        if f[0] != 0x30 or not (IMAGE_BASE <= f[1] < IMAGE_END):
            off += 4
            continue
        funcs[f[1]] = f[2]
        off += REC
    return funcs


def read_recompiled(ppc_dir):
    """Every guest address XenonRecomp emitted a function body for."""
    out = subprocess.run(
        ["grep", "-rhno", r"PPC_FUNC_IMPL(__imp__sub_[0-9A-F]*)", str(ppc_dir)],
        capture_output=True, text=True, check=True).stdout
    return {int(m, 16) for m in re.findall(r"sub_([0-9A-F]{8})", out)}


def read_helper_ladders(config_path):
    """Derive the save/restore ladder address ranges from the recompiler config.

    XenonRecomp's own size formulas (recompiler.cpp Analyse()):
      savegpr/restgpr  entry i (14..31): 4 bytes apart, ladder ends with a tail
      savefpr/restfpr  same shape
      savevmx/restvmx  8 bytes per entry (lis/stvx pairs), two ranges each
    We only need conservative *ranges*, so take each declared base and cover the
    largest ladder that can start there.
    """
    text = Path(config_path).read_text()

    def addr(key):
        m = re.search(rf"^{key}\s*=\s*(0x[0-9A-Fa-f]+)", text, re.M)
        return int(m.group(1), 16) if m else None

    ranges = []
    for key, span in (("savegprlr_14_address", 18 * 4 + 12),
                      ("restgprlr_14_address", 18 * 4 + 16),
                      ("savefpr_14_address", 18 * 4 + 12),
                      ("restfpr_14_address", 18 * 4 + 16),
                      ("savevmx_14_address", 18 * 8 + 12),
                      ("restvmx_14_address", 18 * 8 + 16),
                      ("savevmx_64_address", 64 * 8 + 12),
                      ("restvmx_64_address", 64 * 8 + 16)):
        a = addr(key)
        if a:
            ranges.append((a, a + span))
    return ranges


def read_switch_labels(path):
    """Every case label of every recovered jump table.

    These must not become function boundaries — see the module docstring. Read
    from the same TOML XenonRecomp consumes, so the filter can never disagree
    with what the recompiler actually lowered.
    """
    labels = set()
    text = Path(path).read_text()
    for m in re.finditer(r"labels\s*=\s*\[([^\]]*)\]", text):
        labels.update(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(1)))
    return labels


def read_switch_spans(path):
    """The address span each recovered jump table's parent function must cover.

    A switch's `bctr` and all of its case bodies live inside ONE function. Any
    function start placed strictly inside that span splits the parent and
    re-creates the truncation defect (see the module docstring).

    The case labels themselves are the obvious offenders, but not the only ones:
    Xenia's analysis also reports entry points a few instructions *after* a label.
    On Case Zero three coverage records started exactly 4 bytes past a loop-back
    case target (0x82670084 for label 0x82670080) — not labels, so a label-only
    filter passes them through, and each one truncated its parent again. Xenia had
    in fact recorded BOTH addresses as executed "functions" with the same end,
    which is the tell: two entry points 4 bytes apart sharing an extent are one
    function, not two.

    Excluding the whole span is safe because a genuine indirect-call target does
    not live inside another function's body.
    """
    spans = []
    text = Path(path).read_text()
    for block in re.split(r"\[\[switch\]\]", text)[1:]:
        base = re.search(r"base\s*=\s*0x([0-9A-Fa-f]+)", block)
        labels = re.search(r"labels\s*=\s*\[([^\]]*)\]", block)
        if not base or not labels:
            continue
        addrs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", labels.group(1))]
        if not addrs:
            continue
        addrs.append(int(base.group(1), 16))
        spans.append((min(addrs), max(addrs)))
    spans.sort()
    return spans


def in_switch_span(spans, a):
    """True if `a` would split a switch parent. `lo` itself is allowed: it is the
    earliest thing the parent must contain, so it is a legal function start."""
    i = bisect.bisect_right(spans, (a, float("inf"))) - 1
    # Spans can nest/overlap, so scan back over any that could still contain `a`.
    while i >= 0:
        lo, hi = spans[i]
        if lo < a <= hi:
            return True
        if hi < a and i > 0 and spans[i - 1][1] < a:
            break
        i -= 1
    return False


def find_import_thunks(image_word, lo=IMAGE_BASE, hi=IMAGE_END):
    """Locate the XEX import thunks, in EITHER of their two forms.

    An import thunk is 4 words. Which 4 words depends on whether you are looking
    at the raw file or at a loaded image, and getting that wrong is silent:

      * **In the XEX on disk** -- two loader-patched descriptor words (high byte
        0x01 then 0x02) followed by `mtctr r11; bctr`. This is what Asura's Wrath
        scanned for, because its analysis image came from `decrypt_xex.py`, which
        decrypts the file and never runs the loader.

      * **In a loaded image** -- `nop; nop; nop; blr`. `Xex2LoadImage` *overwrites*
        every thunk with that stub as it registers the import's symbol name
        (xex.cpp: `uint32_t thunk[4] = {0x60,0x60,0x60,0x2000804E}` memcpy'd over
        `descriptors[im].firstThunk`). Our image comes from `tools/xex_image_dump`,
        which dumps `Image::data` *after* `ParseImage`, so the on-disk pattern
        **cannot exist in it** and a scanner looking for it finds exactly zero.

    Zero is the dangerous answer, not an error: every thunk then falls through to
    the override list as a "function executed on hardware that we never
    recompiled". On Case Zero that was 134 of 1,224 entries -- addresses whose
    bodies are `nop; nop; nop; blr` and whose real implementation is the kernel
    HLE seam. Feeding them to XenonRecomp as functions would emit 134 stubs that
    return immediately, shadowing the imports they stand for.

    Both forms are matched so this works on either kind of image, and the caller
    prints the count and range so a zero stays visible instead of silently
    becoming overrides.
    """
    thunks, a = set(), lo
    while a < hi:
        w2, w3 = image_word(a + 8), image_word(a + 12)
        # Loaded-image form: nop; nop; nop; blr
        if (w3 == BLR and w2 == NOP
                and image_word(a) == NOP and image_word(a + 4) == NOP):
            thunks.add(a)
            a += 16
            continue
        # On-disk form: <descriptor>; <descriptor>; mtctr r11; bctr
        if w2 == MTCTR_R11 and w3 == BCTR:
            w0 = image_word(a) or 0
            if (w0 >> 24) == 1:
                thunks.add(a)
                a += 16
                continue
        a += 4
    return thunks


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--trace", action="append", required=True,
                    help="Xenia coverage trace (repeatable; results are unioned)")
    ap.add_argument("--ppc-dir", default="ppc", help="directory of generated C++")
    ap.add_argument("--image", default="assets/game/default_image.bin",
                    help="flat loaded image from tools/xex_image_dump")
    ap.add_argument("--config", default="config/CaseZero.toml")
    ap.add_argument("--switch-tables", default="config/CaseZero_switch_tables.toml",
                    help="jump-table TOML; its case labels are excluded (see docstring)")
    ap.add_argument("-o", "--out", help="write the TOML fragment here")
    ap.add_argument("--apply", action="store_true",
                    help="merge the overrides into --config in place (preserves\n"
                         "existing entries, e.g. fix_switch_function_bounds.py's)")
    args = ap.parse_args()

    executed = {}
    for t in args.trace:
        executed.update(read_coverage(t))
    recompiled = read_recompiled(args.ppc_dir)

    # Flat loaded image (see tools/xex_image_dump.cpp): guest VA - base == offset.
    # decrypt_xex.py cannot read this title's XEX at all (devkit key + LZX).
    data = Path(args.image).read_bytes()

    def word(a):
        o = a - IMAGE_BASE
        return None if not (0 <= o + 4 <= len(data)) else struct.unpack_from(">I", data, o)[0]

    switch_labels = read_switch_labels(args.switch_tables)
    switch_spans = read_switch_spans(args.switch_tables)
    print(f"switch case labels        : {len(switch_labels)} (excluded)", file=sys.stderr)
    print(f"switch parent spans       : {len(switch_spans)} (interiors excluded)",
          file=sys.stderr)

    thunks = find_import_thunks(word)
    if thunks:
        print(f"import thunks found       : {len(thunks)} "
              f"in 0x{min(thunks):X}..0x{max(thunks) + 16:X}", file=sys.stderr)
    else:
        print("import thunks found       : 0  <- SUSPICIOUS, check the pattern",
              file=sys.stderr)
    ladders = read_helper_ladders(args.config)

    def in_ladder(a):
        return any(lo <= a < hi for lo, hi in ladders)

    missing, skipped_thunk, skipped_ladder, skipped_bad = [], 0, 0, 0
    skipped_label = 0
    for start, end in sorted(executed.items()):
        if start in recompiled:
            continue
        if start in thunks:
            skipped_thunk += 1
            continue
        if start in switch_labels or in_switch_span(switch_spans, start):
            skipped_label += 1
            continue
        if in_ladder(start):
            skipped_ladder += 1
            continue
        size = end - start
        # A function must be a sane, word-aligned, in-image extent. Anything
        # else means Xenia's bounds and ours disagree about what this even is.
        if size <= 0 or size % 4 or size > 0x20000 or word(start) is None:
            skipped_bad += 1
            continue
        missing.append((start, size))

    print(f"executed functions        : {len(executed)}", file=sys.stderr)
    print(f"already recompiled        : {len(executed) - len(missing) - skipped_thunk - skipped_ladder - skipped_bad - skipped_label}",
          file=sys.stderr)
    print(f"skipped, switch labels    : {skipped_label}", file=sys.stderr)
    print(f"skipped, import thunks    : {skipped_thunk}", file=sys.stderr)
    print(f"skipped, helper ladders   : {skipped_ladder}", file=sys.stderr)
    print(f"skipped, implausible      : {skipped_bad}", file=sys.stderr)
    print(f"MISSING -> overrides      : {len(missing)}", file=sys.stderr)

    lines = [
        "# Function overrides recovered from Xenia --trace_function_data coverage.",
        "# Generated by tools/coverage_to_function_overrides.py — see",
        "# docs/xenia-capture-analysis.md for why these are needed.",
        "functions = [",
    ]
    lines += [f"    {{ address = 0x{a:X}, size = 0x{s:X} }},  # executed on hardware, not found by static analysis"
              for a, s in missing]
    lines.append("]")
    text = "\n".join(lines) + "\n"

    if args.apply:
        # MERGE into the config's existing `functions` list, never replace it. That
        # list is shared with tools/fix_switch_function_bounds.py, whose switch-tail
        # repairs must survive a coverage refresh (and vice versa — that tool merges
        # the same way, so the two compose in either order). Where both propose an
        # entry for one address, keep the LARGER extent: a switch repair deliberately
        # widens a function past what any single analysis saw.
        toml_text = Path(args.config).read_text()
        merged = {}
        block = re.search(r"^functions = \[(.*?)^\]\n", toml_text, re.M | re.S)
        if block:
            for m in re.finditer(r"address = 0x([0-9A-Fa-f]+), size = 0x([0-9A-Fa-f]+)",
                                 block.group(1)):
                merged[int(m.group(1), 16)] = int(m.group(2), 16)
        before = len(merged)
        for a, s in missing:
            merged[a] = max(s, merged.get(a, 0))
        out_lines = ["functions = ["]
        out_lines += [f"    {{ address = 0x{a:08X}, size = 0x{s:X} }},"
                      for a, s in sorted(merged.items())]
        out_lines.append("]")
        new_block = "\n".join(out_lines)
        if block:
            toml_text = toml_text[:block.start()] + new_block + "\n" + toml_text[block.end():]
        else:
            toml_text = toml_text.replace("[main]\n", "[main]\n" + new_block + "\n", 1)
        Path(args.config).write_text(toml_text)
        print(f"merged into {args.config}: {before} -> {len(merged)} entries "
              f"(+{len(merged) - before} new)", file=sys.stderr)
    elif args.out:
        Path(args.out).write_text(text)
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
