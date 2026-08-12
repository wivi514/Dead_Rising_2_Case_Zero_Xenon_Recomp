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

## 6. `sync`, `lwsync` and `eieio` emit nothing — not even a compiler barrier

**Severity:** silent wrong execution, multi-threaded only, intermittent. **Status:**
patched locally (commit `e7ac625`).

All three memory barriers were `// no op`:

```cpp
case PPC_INST_LWSYNC:
    // no op
    break;
```

"No op" is the right answer for the *hardware* half on x86-64 and the wrong answer
overall, because the recompiled image's memory ordering is not decided by the host CPU
alone. Everything the guest does to memory becomes a plain C++ load or store through
`base`, and a construct that generates no code constrains the host compiler not at all.
At `-O2` clang is free to move those stores across a barrier the guest put there
precisely to stop that.

What breaks is every release-publish idiom in the title. The canonical one:

```
    ...fill a command buffer...
    lwsync                     ; make those stores visible BEFORE the next one
    stw   r11, 0x58(r30)       ; publish the new tail index
```

With the barrier emitting nothing, the publish may be reordered ahead of the fill, and
the consumer thread walks a buffer that is not written yet. Nothing faults at the
reordering; the damage lands later, in whatever the consumer does with the data, on a
thread that has nothing to do with the producer.

**The distinction between the two barriers is the whole fix**, and it is easy to get
backwards:

| instruction | orders | x86-64 TSO already gives it? | correct lowering |
|---|---|---|---|
| `lwsync` | load-load, load-store, store-store | **yes**, all three | `std::atomic_signal_fence` — compiler barrier, **no instruction emitted** |
| `sync` (hwsync) | the above **plus store-load** | **no** — store-load is the one reordering x86 does | `std::atomic_thread_fence` — a real `mfence`/locked op |
| `eieio` | stores to device memory | n/a (our MMIO is host memory) | `std::atomic_signal_fence` — the honest floor |

Lowering `lwsync` to a full `atomic_thread_fence` would be correct but would put an
`mfence` on hot guest paths for an ordering the hardware already provides; lowering
`sync` to a signal fence only would be silently wrong on the one ordering that matters.

Case Zero's image has 51 `lwsync`, 11 `sync` and 14 `eieio` — few enough that the cost
is nil and concentrated enough (the graphics command-stream producer/consumer, the
`lwarx`/`stwcx.` spin locks) that they are exactly the sites where it matters.

Two related lowerings are *not* changed, and are worth knowing about:

- `lwarx` is a plain non-atomic load, and that is fine here only because `stwcx.` is a
  `__sync_bool_compare_and_swap` against the value it loaded — a correct CAS emulation
  of the reservation pair for the uncontended and contended cases alike, and a full
  barrier in its own right. So lock *acquire* was always ordered; lock *release*, a
  plain `stw`, was not.
- `isync` does not appear in this image at all.

`ppc_context.h` gains `#include <atomic>`.

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

## XenosRecomp local patch: `XE_NAN_PAINT`, a NaN detector in the pixel shader epilogue

**PART 33 CAVEAT, and it is gotcha 281: this detector sits DOWNSTREAM of the tone
epilogue's `max`/`saturate`, which launder a NaN into a finite `sqrt(K1*K2)` before
`oC0` is written.** Its zero-magenta result was read in part 27 as "the value never was
a NaN"; it could not have said that, and the value WAS a NaN — the white plateau was
NaN-fed vertex normals, found the moment the detector moved to the max's OPERANDS
(`XE_FLOOR_IS_NAN`, below). This instrument still answers "is `oC0` itself NaN", which
is a different and narrower question than "did a NaN reach this shader".

**Added part 27, and it is an INSTRUMENT rather than a fix** — the emitter always writes
the block and it compiles only when the define is passed, so the default cache is
unchanged. Verified: rebuilding the default cache with the patched emitter reproduces
**409 of 410 modules byte for byte** (the one difference is a shader the shipped cache
never had — see below).

```hlsl
#ifdef XE_NAN_PAINT
#ifndef XE_NAN_PAINT_FORCE
#define XE_NAN_PAINT_FORCE 0
#endif
    if (any(isnan(oC0)) || XE_NAN_PAINT_FORCE)
        oC0 = float4(1.0, 0.0, 1.0, 1.0);
#endif
```

Build an arm with `CZ_DXC_DEFINES` (added to `tools/build_shader_spv.sh`) into its own
directory and select it at run time with `CZ_SHADER_SPV`, which makes a shader change a
same-binary A/B:

```
cp -r assets/shader_spv assets/shader_spv_nanpaint
CZ_DXC_DEFINES="-D XE_NAN_PAINT=1" tools/build_shader_spv.sh <ucode_dir> assets/shader_spv_nanpaint
CZ_SHADER_SPV=$PWD/assets/shader_spv_nanpaint ./cz_runtime
```

**`XE_NAN_PAINT_FORCE=1` is the positive control and it is not optional.** "No magenta"
is only a result once the detector has been shown able to produce magenta — it is
otherwise equally consistent with the define not compiling in, `isnan` being folded away
by DXC, or the painted shaders never being bound in the frame measured. Measured:
**99.85% of the scene buffer and 100.00% of the presented frame magenta** on the forced
arm. There is also a static check that costs nothing — `OpIsNan` is opcode 156 with a
word count of 4, so `9C 00 04 00` appears in every painted module: 317 of 317 pixel
shaders in the arm, against 190 of 316 in the default cache (XenosRecomp already emits
`isnan` elsewhere).

**A cache gap found on the way.** Rebuilding from `~/DR2CZ-troubleshooting/ucode-dumps`
produces `ps_7d6044e7dcaea1f2`, which **the shipped `assets/shader_spv` does not contain**
— we hold its microcode and never built it. The shipped cache instead carries
`ps_926c15dd20571cf1`, whose microcode was lost to `/tmp`. Both caches are 410 modules,
which is why the count never showed it.

## XenosRecomp local patch: `XE_VALUE_PAINT`, "did a shader write this value?"

Same shape as `XE_NAN_PAINT` and same build path — emitted always, compiled only when
defined, `CZ_DXC_DEFINES` into its own cache, `CZ_SHADER_SPV` to select it. Paints GREEN
so both can run together.

```
CZ_DXC_DEFINES="-D XE_VALUE_PAINT=0.7071068" tools/build_shader_spv.sh <ucode> <dir>
```

**Its positive control needs no extra define**: a large `XE_VALUE_PAINT_EPS` admits every
pixel. Measured at `EPS=10.0`: **99.73% of the scene buffer and 100.00% of the presented
frame green.**

**What it settled in part 27.** Case Zero's white patches are exactly rgb(180,180,180) in
the scene buffer at every location, and 255*sqrt(0.5) = 180.3. Since the same value
appears on surfaces with different materials, shaders and constants, the prior question
was whether any material shader emits it at all, or whether it arrives from the resolve or
the EDRAM path. Six captures an arm on the DebugJump route:

| arm | px at exactly (180,180,180) | green |
|---|---|---|
| default cache | **3,242** | 0 |
| `XE_VALUE_PAINT=0.7071068` | **0** | **3,982** |

**The conversion is total** — not one plateau pixel survives — so the value is
`oC0.rgb ~ 0.7071` written by the material pixel shaders themselves. The resolve and the
EDRAM path are eliminated. (The two totals differ because the frames differ between runs;
this is a presence/absence test, not a matched count, and the arms cannot be frame-matched
outdoors — gotcha 254.)

### `XE_SHADER_TAG` — the paint carries the shader's identity

`tools/build_shader_spv.sh` now passes `-D XE_SHADER_TAG=<low 16 bits of the hash>` to
every module, taken from the filename it already has. It costs nothing when nothing reads
it, and the default cache is still reproduced **410 of 410 byte for byte** with it always
passed. `XE_VALUE_PAINT` writes `(hi, 255, lo)` instead of flat green, so a painted frame
names the shaders instead of merely counting them.

`tools/shader_tag_decode.py` reads the scene-buffer PPMs back. Green 255 occurs naturally,
so it selects on the marker, groups the payload, and **prints the pairs that resolve to no
shader as the noise floor rather than discarding them** — a decoder that silently drops
what it cannot explain is how a wrong reading survives. 16 bits collide about 1.3 times
over 411 shaders, so a tag with more than one candidate is reported AMBIGUOUS with all
names rather than resolved by picking the first.

Headless self-test, three frames of the DebugJump route: **seven shaders named**, largest
`ps_4d2ac8618715bdbe` at 815 px, with the noise floor a single pair — pure white
`R=255 B=255`, 400 px — correctly separated.

## XenosRecomp local patches (part 33): the NaN-tracing family, and the fmt16 fix

Full narrative: `docs/phase5-notes.md` §6bg. All three instruments are emitted always
and compile only under their defines; the default cache rebuilt byte-for-byte (416 of
416) after each emitter change, which is the standing gate.

**`XE_FLOOR_IS_NAN`** (with `XE_FLOOR_PAINT` + `XE_VALUE_PAINT`) — repoints the
`xe_max` hook's predicate from `(b > a)` to `isnan(a) || isnan(b)`: the max's OPERANDS
are the last place a NaN is visible before `max(NaN,K1) = K1` and `saturate(NaN) = 0`
launder it. Magenta = value-band pixel whose max saw NaN; green = one whose max did
not. The predicate swap doubles as its own control: the same population painted magenta
under `isnan` and green under `(b > a)` proves the flag follows the predicate.
`XE_FLOOR_IS_NAN_FORCE=1` is the positive control.

```
CZ_DXC_DEFINES='-D XE_VALUE_PAINT=0.70711 -D XE_FLOOR_PAINT=1 -D XE_FLOOR_IS_NAN=1' \
    tools/build_shader_spv.sh ~/DR2CZ-troubleshooting/ucode-dumps assets/shader_spv_nanpaint
```

**`XE_NAN_IN_PAINT`** — samples the interpolator-fed GPRs once at pixel-shader entry
(`xe_nanin`, its own flag so the `xe_max` hook stays inert) and paints magenta
unconditionally: the footprint of ARRIVING NaNs, as distinct from manufactured ones.
Part 33: 20,563 px on the outdoor route — 17x the visible plateau.

**`XE_NAN_VS_KILL_IN`** — in every vertex shader, if any declared float input arrives
NaN, `oPos = NaN` culls the triangle. Separates NaN in the vertex DATA from NaN made by
VS arithmetic; it cannot see dependent (`XeVfetchDep`) fetches. Part 33: with it on,
the input-paint's magenta went 20,563 -> 0 and the plateau to 0.

**The FIX: `XeUnpack_10_11_11` + the read-site format branch (4621beb), runtime
fmt16 -> `R32_SFLOAT` (7889e99).** A fmt16 `k_10_11_11` element declared under a
float-typed usage (this title wraps every packed normal as TEXCOORD; Fable 2 uses
NORMAL, which the uint4 + `tfetchR11G11B10` path already covered) reached a `float4`
input from a `R32_UINT` attribute — `VUID-VkGraphicsPipelineCreateInfo-Input-08733`,
undefined values, in practice the packed dword's bits as a float: NaN wherever bits
30..23 are all ones. The fix delivers the bits through a type-matched `R32_SFLOAT`
fetch and unpacks in-shader by the fetch instruction's own static format field.
**Upstream-relevant**: any title wrapping packed normals in a float-typed usage hits
this; the upstream emitter only handles the uint4 usages.
