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

## Inherited patches (from the Fable 2 / Asura's Wrath ports)

The shared XenonRecomp checkout already carries these; they are listed so that a Case
Zero-only rebuild is not mistaken for stock upstream:

- `bd*` branches to out-of-function targets become tail calls.
- Switch tables keyed by the dispatch **value** rather than the label position, with an
  honest `PPC_SWITCH_ABORT` default instead of `__builtin_unreachable()` (Asura's Wrath
  finding 38 — the one that let a bad dispatch jump into an unrelated function's body).
- Stale `ppc_recomp.N.cpp` files past the last emitted index are deleted.
- `PPC_FUNC_PROLOGUE` guarded with `#ifndef`.
- `vpkd3d128` float16_2 pack (type 3).
