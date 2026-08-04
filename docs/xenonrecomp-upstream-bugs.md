# XenonRecomp bugs and local patches (Case Zero)

Local changes carried in `~/GithubRepo/XenonRecomp`, in the format the Fable 2 port used
so they can be filed upstream. That checkout is shared by all three ports in this
workspace, so anything here also lands on Fable 2 and Asura's Wrath — every patch below
is written to be a no-op for titles that already worked.

---

## 1. `Xex2LoadImage` assumes the retail AES key, and fails silently when it is wrong

**Severity:** blocks the port entirely. **Status:** fixed locally 2026-08-04.

### Symptom

`Image::ParseImage` returns a default-constructed `Image` — `base = 0`, `size = 0`,
`sections.empty()`. `XenonRecomp` then segfaults or emits nothing, with no message.
There is no diagnostic anywhere that says the image failed to load, so the first
plausible reading is "my TOML is wrong" and the second is "the XEX is corrupt".

### Cause

Xbox 360 XEXs encrypted with `XEX_ENCRYPTION_NORMAL` use **one of two** AES keys: the
retail key, or the all-zero *devkit* key. **Nothing in the header distinguishes them** —
`encryptionType` reads 1 in both cases. XBLA / arcade titles commonly ship with the
devkit key; Dead Rising 2: Case Zero (title `58410A8D`) does.

`XenonUtils/xex.cpp` hardcoded `Xex2RetailKey`. With the wrong key the decrypted stream
is noise, so:

- with `XEX_COMPRESSION_NORMAL`, the first block's SHA-1 check fails → `return {}`;
- with `XEX_COMPRESSION_NONE` or `_BASIC` there is no checksum at all, so a wrong key
  would have produced a garbage image and no error whatsoever.

Xenia handles this by trying each key and validating the result.

### Fix

`XenonUtils/xex.h` gains `Xex2DevkitKey` (all zeroes). `Xex2LoadImage`'s
decrypt-and-decompress body is refactored into a lambda parameterised by key, which
returns null on failure; the caller tries retail first, then devkit. Retail-key titles
take exactly the same path they did before, so this is behaviour-preserving for Fable 2
and Asura's Wrath.

The lambda also gained a validation step the original lacked: the decompressed image must
start with `MZ` (it is a PE). That is the only check the NONE/BASIC paths have, and it is
what makes the key fallback safe for them rather than just for LZX images.

### Still missing (not fixed)

**There is no diagnostic on failure.** `Xex2LoadImage` returning `{}` is indistinguishable
from success at every call site; `Recompiler::LoadImage` returns `true` regardless. A
one-line "failed to load XEX (bad key? unsupported compression?)" would have saved the
first hour of this project. Worth filing separately from the key fix.

---

## 2. `Recompiler::SaveCurrentOutData` segfaults when the output directory does not exist

**Severity:** cosmetic, but wastes time on a first run. **Status:** not fixed; noted.

`out_directory_path = "../ppc"` with no `ppc/` directory present gives:

```
Program received signal SIGSEGV
#0  fwrite () from /lib64/libc.so.6
#1  Recompiler::SaveCurrentOutData(...)
#2  Recompiler::Recompile(...)
```

`fopen` fails, the null `FILE*` is not checked, and `fwrite` dereferences it. On a first
run — exactly when the directory is most likely to be missing — this reads as a crash in
the recompiler rather than as a missing directory. `mkdir -p ppc` is the workaround.

---

## 3. `VADDUWS` emits `simde_mm_adds_epu32`, which does not exist

**Severity:** breaks the build of any title that uses `vadduws`. **Status:** fixed
locally, commit `981afe9`.

The `PPC_INST_VADDUWS` case emitted `simde_mm_adds_epu32(...)`. There is no such
intrinsic: no SSE level provides a 32-bit unsigned saturating add, and simde does not
synthesise one (it has `adds_epu8` and `adds_epu16` only). The recompiler runs perfectly
happily and produces C++ that fails to compile with an error naming simde, so the
evidence points at the wrong component.

This case had presumably never been exercised — which is the transferable point: **a
recompiler case is only proven by a title that uses it *and* a compile that consumes its
output.** Anything not yet compiled is unverified regardless of how long it has been in
the tree. Worth auditing the rest of the vector cases against simde's actual surface
before the first compile of a new port, not after.

Fixed with an algebraic identity that needs only SSE4.1 `min_epu32` (already relied on by
`VPKUWUS`):

    vadduws:  a + min_epu32(b, ~a)      overflow iff b > ~a, and then a + ~a = 0xFFFFFFFF

The same gap exists on the subtract side, so `VSUBUWS` (added for Case Zero) uses:

    vsubuws:  max_epu32(a, b) - b       a - b when a >= b, 0 otherwise

## 4. An unrecognized instruction emits nothing, silently

**Severity:** silent wrong execution. **Status:** upstream behaviour; not changed.

When `Recompile()` returns false the recompiler prints `Unrecognized instruction at ...`
to stdout, sets `allRecompiled = false`, **and emits no code for that instruction at
all**. The generated C++ still compiles; the guest operation simply does not happen.

The exit code does not reflect it and the message is easily lost in the progress spam, so
treat the count of `Unrecognized`/`Unable to decode` lines as a build gate. Same for
dropped branches (bug 5), which are not reported to stdout at all.

## 5. A branch to a non-function address is dropped with only a comment

**Severity:** silent wrong execution, invisible. **Status:** upstream behaviour; worked
around in the config by `tools/find_dropped_branches.py`.

`printFunctionCall` resolves a branch target through `image.symbols.find(address)`, which
is an **exact-start** lookup — `SymbolTable::find` runs `equal_range` on the address, so a
symbol that merely *contains* the target never matches. When it misses, the emitter does:

    println("\t// ERROR {:X}", address);

Nothing is printed to stdout, the run exits 0, and the C++ compiles. Case Zero had 31 of
these. See analysis finding 13 for the two signatures (backward = split function, forward
= truncated function) and their opposite repairs. Both are repairable from the config, so
no recompiler change was needed — but the *absence of any diagnostic* is the bug, and any
port that does not go looking will never learn it has them.

---

## Inherited patches (from the Fable 2 / Asura's Wrath ports)

The shared XenonRecomp checkout already carries these; they are listed so that a Case
Zero-only rebuild is not mistaken for stock upstream:

- `bd*` branches to out-of-function targets become tail calls.
- Switch tables keyed by the dispatch **value** rather than the label position, with an
  honest `PPC_SWITCH_ABORT` default instead of `__builtin_unreachable()` (Asura's Wrath
  finding 38 — the one that let a bad dispatch jump into an unrelated function's body).
- Stale `ppc_recomp.N.cpp` files past the last emitted index are deleted.
- `PPC_FUNC_PROLOGUE` guarded with `#ifndef`.

## Added for Case Zero (commit `981afe9`)

Six mnemonics this title needs that stock XenonRecomp does not implement, plus the
`VADDUWS` repair above. `lhbrx` mirrors `LWBRX` one size down; `stfsux` follows `LFSUX`'s
operand shape (frS, rA, rB) rather than `STFSU`'s (frS, displacement, rA); `vpkuwum`
takes vB as the first `packus` argument for the same whole-vector-reversal reason as the
existing `VPKSHUS`/`VPKUWUS` cases.

All verified by differential test against scalar references written from the PPC
definitions — 200,153 cases, zero failures across `-O2`/`-msse4.1`/`-mavx2`/`-O0`, with
negative controls confirming the test discriminates. Vector lowering hides two conventions
that are invisible to inspection (the byte reversal, and saturation edges) and both fail
as silent wrong *values*, so an untested vector case is an unverified one.
- `vpkd3d128` float16_2 pack (type 3).
