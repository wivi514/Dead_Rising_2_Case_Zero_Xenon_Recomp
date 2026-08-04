#!/usr/bin/env python3
"""Generate stubs for the kernel imports the runtime has not implemented yet.

WHY THIS EXISTS
---------------
The recompiled image calls the kernel through `__imp__<Name>` symbols — 244 of
them here. XenonRecomp *declares* every one in `ppc/ppc_recomp_shared.h` and
*defines* none: routing them to a host implementation is the whole point of the
kernel HLE seam. So until each name exists somewhere, nothing links, and phase
0.2's gate is precisely "the whole image links".

Note this is the ONLY category of undefined symbol in the generated code. The 236
register save/restore ladder helpers look like they belong here — they are
declared in the shared header and called all over the image — but XenonRecomp
synthesises their bodies from the `*_address` config keys and emits them into
`ppc/` as `__imp____savegprlr_14` and friends, with a weak alias to the plain
name. They resolve on their own. Do not stub them; you will get 236 duplicate
symbols.

WHAT THE STUBS DO, AND WHY THAT CHANGES AT PHASE 1
--------------------------------------------------
Right now they print their name and `abort()`. That is correct *for phase 0.2 and
only for phase 0.2*, where the question being asked is "does the image link and
can we enter it", and any call into an unimplemented kernel is a result we cannot
interpret anyway.

From phase 1 onward this becomes wrong, and the Asura's Wrath port has already
paid for learning why: the phase 1 gate is that our kernel-call *sequence*
matches Xenia's A1 capture, and aborting on the first unimplemented name makes
the ordering half of that gate unobservable. At that point these become
honest-failure stubs returning STATUS_NOT_IMPLEMENTED — visible to the guest,
steerable, and still not a lie.

What must never happen, in any phase, is a stub that fakes success. Fable 2 lost
weeks to a faked XMA context call that presented as an audio-decode bug for a
month. And a stub that returns an error but leaves its **out-parameter**
untouched is worse than no stub at all, because the guest frequently ignores the
status and reads the buffer anyway.

THE NAME LIST COMES FROM THE IMAGE, NOT FROM A PREVIOUS PORT
------------------------------------------------------------
It is read out of `ppc/ppc_recomp_shared.h` — the recompiler's own extern
declarations — so it cannot drift from what this image actually references.
Copying an import list from Fable 2 or Asura's Wrath would be wrong in both
directions at once: names this title never imports, and names it does.

Regenerate after any recompilation that changes the import set, and after adding
a real implementation:

    python3 tools/gen_import_stubs.py
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SHARED_H = REPO / "ppc" / "ppc_recomp_shared.h"
RUNTIME = REPO / "runtime"
OUT = RUNTIME / "kernel" / "import_stubs.cpp"

# Vendored third-party code never defines a kernel import, and build trees are
# full of copies that would produce phantom "already implemented" hits.
SKIP_DIRS = {"build", "thirdparty"}

# The spellings a runtime source may use to define an import. None of these exist
# yet — phase 1 introduces them — but the scan has to be in place before the first
# real implementation lands, or that implementation silently collides with its own
# generated stub and the error arrives as a duplicate symbol after a full rebuild.
DEFINITION_PATTERNS = [
    re.compile(r"GUEST_FUNCTION_HOOK\(\s*__imp__(\w+)"),
    re.compile(r"GUEST_FUNCTION_STUB\(\s*__imp__(\w+)"),
    re.compile(r"^\s*PPC_FUNC\(\s*__imp__(\w+)\s*\)", re.MULTILINE),
    re.compile(r"^\s*STUB_RET\(\s*(\w+)\s*,", re.MULTILINE),
]


def already_implemented():
    """Names some runtime source already defines — stubbing those duplicates them."""
    names = set()
    if not RUNTIME.is_dir():
        return names
    # Walk the whole runtime tree, not a fixed directory list: every phase adds a
    # module (gpu/, audio/, video/, ...), and one this script does not know about
    # means duplicate symbols found only after a full rebuild of 227 TUs.
    for path in sorted(RUNTIME.rglob("*.cpp")):
        if SKIP_DIRS.intersection(path.relative_to(RUNTIME).parts):
            continue
        if path.resolve() == OUT.resolve():
            continue
        text = path.read_text()
        for pattern in DEFINITION_PATTERNS:
            names.update(pattern.findall(text))
    return names


def main():
    referenced = re.findall(r"PPC_EXTERN_FUNC\(__imp__(\w+)\);", SHARED_H.read_text())
    if not referenced:
        raise SystemExit(f"no __imp__ externs found in {SHARED_H} — wrong path, or "
                         "ppc/ has not been generated?")

    # No ladder filter is needed, and it is worth saying why rather than carrying a
    # no-op one. The 236 save/restore helpers ARE declared in this header, but as
    # plain `PPC_EXTERN_FUNC(__savegprlr_14);` — no `__imp__` prefix — so the regex
    # above never sees them. XenonRecomp emits their bodies as
    # `PPC_FUNC_IMPL(__imp____savegprlr_14)` with a weak alias to the plain name,
    # which is what the call sites bind to. Stubbing them would duplicate 236
    # symbols. If a future XenonRecomp changes either spelling, this count moves off
    # the image's real import total and that is the signal to re-check.
    imports = referenced

    ladder_decls = len(re.findall(r"PPC_EXTERN_FUNC\(__(?:save|rest)\w+\);",
                                  SHARED_H.read_text()))

    implemented = already_implemented()
    todo = [n for n in imports if n not in implemented]   # recompiler order: small diffs

    unknown = sorted(implemented - set(imports))
    if unknown:
        # A hook for a name this image never imports is dead code, and the usual
        # cause is a file carried over from another title's runtime.
        print(f"warning: runtime defines {len(unknown)} import(s) this image does not "
              f"reference: {', '.join(unknown)}", file=sys.stderr)

    body = [
        "// GENERATED by tools/gen_import_stubs.py — do not edit by hand.",
        "//",
        f"// {len(todo)} of this image's {len(imports)} kernel imports have no",
        f"// implementation yet ({len(implemented & set(imports))} are real).",
        "//",
        "// PHASE 0.2 BEHAVIOUR: print the name and abort. The only question this",
        "// phase asks is whether the image links and can be entered, so a call into",
        "// an unimplemented kernel is a result we could not interpret anyway.",
        "//",
        "// This becomes WRONG at phase 1, whose gate is that our kernel-call",
        "// *sequence* matches the A1 capture — aborting on the first unimplemented",
        "// name makes the ordering half of that gate unobservable. Convert to",
        "// STATUS_NOT_IMPLEMENTED returns then. Never to a success-shaped 0.",
        "",
        "#include <ppc_config.h>",
        "#include <ppc_context.h>",
        "#include <cstdio>",
        "#include <cstdlib>",
        "",
        "namespace {",
        "// Out-of-line so each stub is a two-instruction body rather than 244 copies",
        "// of the printf setup.",
        "[[noreturn]] void unimplemented_import(const char* name)",
        "{",
        '    std::fprintf(stderr, "\\n[kernel] unimplemented import: %s\\n", name);',
        '    std::fprintf(stderr, "[kernel] phase 0.2 has no kernel; this is expected '
        'if the\\n"',
        '                         "         image is being entered rather than just '
        'linked.\\n");',
        "    std::fflush(stderr);",
        "    std::abort();",
        "}",
        "}  // namespace",
        "",
    ]
    # PPC_FUNC, not PPC_FUNC_IMPL. The distinction is a linkage one and it is the
    # kind of thing that only shows up at link time, after a full build:
    #
    #   PPC_FUNC(x)      void x(PPCContext&, uint8_t*)              C++ linkage
    #   PPC_FUNC_IMPL(x) extern "C" void x(PPCContext&, uint8_t*)   C linkage
    #
    # ppc_recomp_shared.h declares imports with PPC_EXTERN_FUNC, which is a plain
    # `extern PPC_FUNC(x)` — so every reference from the image is *mangled*. Defining
    # a stub with PPC_FUNC_IMPL emits an unmangled symbol that nothing refers to, and
    # the link fails with 244 undefined references to names that visibly exist in
    # this file.
    #
    # The generated guest functions get away with PPC_FUNC_IMPL because each is
    # paired with `__attribute__((alias(...))) PPC_WEAK_FUNC(sub_X)`, which
    # re-exports the unmangled definition under the mangled name. That alias is what
    # makes the hook seam work; imports have no such alias.
    for name in todo:
        body.append(f'PPC_FUNC(__imp__{name}) {{ unimplemented_import("{name}"); }}')
    body.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(body))

    print(f"kernel imports (__imp__): {len(imports)}")
    print(f"  ladders seen alongside: {ladder_decls} (no __imp__ prefix; XenonRecomp "
          f"defines these, they are NOT stubbed)")
    print(f"  already implemented   : {len(implemented & set(imports))}")
    print(f"  stubbed here          : {len(todo)}")
    print(f"wrote {OUT.relative_to(REPO)}")


if __name__ == "__main__":
    main()
