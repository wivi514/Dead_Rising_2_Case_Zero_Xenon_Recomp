# Bootstrap findings — 2026-08-04 (session 1)

What was established the day this repo was initialized, with enough detail to re-derive
or challenge any of it. Template projects: `~/GithubRepo/Fable2XenonRecomp` (the
playbook) and `~/GithubRepo/Asuras_Wrath_Xenon_Recomp` (the second port, whose
`docs/bootstrap-2026-08-02.md` this file is modelled on).

Written so that someone porting a *different* XBLA title can lift the technique: almost
everything below is about the ways an arcade title differs from a disc title, and none
of those differences were covered by the two template ports.

---

## 1. The container: STFS, not an ISO

The game arrived as one 864,768,000-byte file with no extension, at

    Assets/Dead Rising 2 - Case Zero (World)/58410A8D/000D0000/3A98C69EE94FD53A3D592BBAC2236F2247A2957158

That path shape is the console's content layout: `<TitleID>/<ContentType>/<hash of the
content ID>`. The blob is an **XContent package**, magic `LIVE`, content type
`0x000D0000` (Arcade Title), volume type STFS, display name `DEAD RISING 2: CASE ZERO`,
title ID `58410A8D`, media ID `00000000`.

STFS is a 0x1000-block filesystem with **hash tables interleaved into the data**: one
hash-table block per 170 data blocks, one level-1 table per 170 of those, three levels
deep. Each block's hash entry also carries the *next* block number, so a file is a
linked list, not an extent. Consequences:

- You cannot `dd` a file out of it, and a magic-byte search for the XEX finds only its
  first 4 KB before the chain jumps somewhere else.
- The `contiguous` bit in a directory entry is a hint; the chain is the authority.

There is no packaged Linux tool for this (wxPirs is Windows/GUI, Velocity is
unmaintained), so `tools/extract_stfs.py` reimplements the walk from the format
description in Xenia-Canary's `xcontent_container_device.cc`, by way of UnleashedRecomp's
`install/xcontent_file_system.cpp` (read as a *format reference* only — UnleashedRecomp is
GPLv3; no code was copied). It also implements the SVOD flavour, which larger arcade and
Games-on-Demand titles use, so a package of that shape fails loudly rather than silently.

**Result: 256 files, 850 MB extracted in 2 s.** `default.xex` (4,538,368 bytes) plus a
`data/` tree.

## 2. The XEX: devkit key + LZX, and the silent failure that hides both

XEX2 header: `headerSize = 0x3000`, `securityOffset = 0x108`, 14 optional headers.

| field | value |
|---|---|
| image base (`0x00010201`) | `0x82000000` |
| entry point (`0x00010100`) | `0x825D9F30` |
| image size (security info) | `0xB40000` (11,796,480) |
| title id | `58410A8D` |
| media id | `00000000` |
| encryption | 1 (normal) |
| compression | **2 (normal = LZX)**, window `0x8000`, first block `0xF800` |

Two traps, in the order they bite:

**2a. `encryption = 1` does not say *which* key.** Xbox 360 titles are encrypted with
either the retail key or the all-zero "devkit" key, and the header carries no
discriminator. XBLA titles commonly use the devkit key; this one does. Verified directly:
decrypting the title key with the retail key and hashing the first `0xF800` bytes gives
`b4b87f1d…`; with the all-zero key it gives `5e752cd6…`, which is exactly the block hash
in the header.

Stock XenonRecomp hardcodes `Xex2RetailKey`, so `Image::ParseImage` fails the first
block's SHA-1 and **returns an empty `Image` with no diagnostic at all** — base 0, size
0, zero sections. Fixed locally; see `docs/xenonrecomp-upstream-bugs.md`.

**2b. `compression = 2` breaks the template's Python.** `tools/decrypt_xex.py` (copied
from Asura's Wrath, itself from Fable 2) only understands *basic* compression, where the
FileFormatInfo holds a table of `(data_size, zero_size)` pairs. Parsed that way, this
title's normal-compression info reads as `block data=0x5E752CD6 zero=0x0CA60E7D` — i.e.
it produces a confident, entirely fictional block table rather than an error. Both
template ports would have accepted it silently.

Rather than port an LZX decoder to Python, `tools/xex_image_dump.cpp` links XenonUtils
and dumps the image the recompiler itself loads. If the analysis image and the
recompiler ever disagree about a byte, they are now the same code path — which is the
property we actually want from an analysis oracle.

`decrypt_xex.py` is kept in `tools/` because parts of the toolchain still import its
address-mapping helpers, but **it cannot read this game's XEX** and nothing should call
its `decrypt()` on it.

### Section map (from `tools/xex_image_dump`)

```
.rdata    0x82000400  0x001145BC
.pdata    0x82114A00  0x000338B0
.text     0x82150000  0x00873564   <- code
.data     0x829D0000  0x00118AF8
.tls      0x82AE8C00  0x00000015
.XBMOVIE  0x82AE8E00  0x00000008
.XEXID    0x82AE9000  0x00000004
.idata    0x82AF0000  0x0000047A
.XBLD     0x82B00000  0x00000100
.reloc    0x82B00200  0x000B9ED4
```

## 3. Register save/restore helpers

Found by `tools/find_save_restore.py`, a structural scan of the loaded image. All eight
on the first pass:

| helper | address | shape |
|---|---|---|
| savegprlr_14 | 0x8280FED0 | 18× `std r(14+k),-0x98+8k(**r1**)`, `stw r12,-8(r1)`, `blr` — size 80 |
| restgprlr_14 | 0x8280FF20 | 18× `ld`, `lwz r12,-8(r1)`, `mtlr r12`, `blr` — size 84 |
| savefpr_14 | 0x82810000 | 18× `stfd f(14+k),-0x90+8k(**r12**)`, `blr` — size 76 |
| restfpr_14 | 0x8281004C | 18× `lfd`, `blr` — size 76 |
| savevmx_14 | 0x82815190 | 18 `li r11`/`stvx` pairs from `-0x120`, `blr` — size 148 |
| savevmx_64 | 0x82815224 | 64 `li r11`/`stvx128` pairs from `-0x400`, `blr` — size 516 |
| restvmx_14 | 0x82815428 | 18 pairs, size 148 |
| restvmx_64 | 0x828154BC | 64 pairs, size 516 |

All sizes match XenonRecomp's formulas in `Recompiler::Analyse()`. **The strong check is
that the four vector ladders are contiguous** — 0x82815190 + 148 = 0x82815224,
+516 = 0x82815428, +148 = 0x828154BC — so the set is self-consistent, not four
independent guesses.

Two scanner bugs worth recording, because both produced *plausible wrong answers* rather
than failures:

1. **Mixed base registers within one title.** The gpr ladders here are r1-based and the
   fpr ladders are r12-based. Asura's Wrath's notes warn that the base register varies
   between titles; it also varies *within* one. A scan with the base register fixed
   reports half the ladders missing, which reads as "this image is unusual".
2. **A long ladder contains a short one.** `__savevmx_64`'s 46th rung is `li r11,-0x120`,
   followed by 17 more pairs and a `blr` — an exact match for an 18-pair `__savevmx_14`
   scan, at an address 0x170 *inside* a function the same scan just reported as 516 bytes
   long. The first run of the tool emitted that address. The fix is to reject a match
   preceded by the previous rung of the same ladder; the contiguity check above is what
   made the error visible in the first place.
3. **v14–v31 use the classic VMX encodings, v64–v127 the VMX128 ones.** `stvx` is primary
   opcode 31 (XO 231 → `0x…1CE`); `stvx128` is primary opcode 4 (`0x…1CB`). And VMX128
   spreads the 7-bit register number across the instruction, so the high bit lands in bit
   2 of the low half: registers 64–95 encode `0x…1CB` and 96–127 `0x…1CF`. A matcher that
   doesn't mask that bit stops halfway through the 64-rung ladder.

## 4. Jump tables: XenonAnalyse finds zero, our scanner finds 232

`XenonAnalyse assets/game/default.xex config/CaseZero_switch_tables.toml` produced an
empty file — no absolute, computed, or offseted tables in an 8.8 MB code section. That is
not credible, and it is the *same* result Asura's Wrath got, for the same reason:
XenonAnalyse matches exact ordered opcode sequences, and this compiler schedules them
differently.

`tools/find_jumptables.py` (copied from Asura's Wrath, adapted to read a flat image)
anchors on the invariant tail instead and recovers:

```
absolute jump tables      : 105
offset16 jump tables      :  42
offset8  jump tables      :  85
total tables              : 232
total case labels         : 6114
unhandled                 :   0
```

The cost of missing these is not a compile error — see Asura's Wrath finding 38 and the
tool's own docstring. **Never accept a zero here.**

## 5. First recompilation pass

`XenonRecomp CaseZero.toml`: **57,728 functions, 227 TUs, 154 MB, 3.3 s.**

```
PPC_IMAGE_BASE 0x82000000   PPC_IMAGE_SIZE 0xB40000
PPC_CODE_BASE  0x82150000   PPC_CODE_SIZE  0x873564
```

### Switch-tail repairs — 28 functions, closed

The first pass reported 517 `Switch case at X is trying to jump outside function` errors
across 28 distinct functions: XenonRecomp's boundary analysis ends each at its jump-table
`bctr`, so the case bodies fall outside and are lowered to a bare `return;` — no
epilogue, so the caller resumes with the callee's non-volatiles.

`tools/fix_switch_function_bounds.py --apply` computed size overrides to a fixpoint and
the second pass reports **zero**. The entries are in `config/CaseZero.toml`.

### Unrecognized instructions — 42 sites, 6 mnemonics (open work item)

```
30 lhbrx     5 stfsux     4 vsubuws     1 vspltish     1 vpkuwum     1 vadduhs
```

For scale: Asura's Wrath's first pass had **3,192** sites across 32 mnemonics. This is a
small, tractable list and should be closed before any runtime work — an unimplemented
instruction is a silent wrong-execution trap, not a build failure.

- `lhbrx` (30) — halfword load, byte-reversed. Trivial, and its frequency suggests a
  byte-swapping helper somewhere hot (endianness conversion in the `.big` reader is the
  obvious guess — worth confirming, because if so it is on every asset load path).
- `stfsux` (5) — update-form indexed float store: the access plus an EA writeback.
- `vsubuws` / `vadduhs` / `vspltish` / `vpkuwum` (7 total) — VMX saturating arithmetic,
  splat-immediate, and a pack. Mechanical against the existing VMX emitters.

## 6. Game intel

- **Engine**: Blue Castle Games' in-house engine, shared with the full Dead Rising 2 —
  the image still carries DR2's zone names (`americana`, `atlantica`, `arena_stadium`,
  `boss_battle_*`) although Case Zero ships only the Still Creek content. Expect the Case
  West port to reuse nearly all of this.
- **Middleware**: Havok (physics; the image contains `hkp*`/`hkx*` RTTI strings and an
  assert naming a developer to send `havok_assert_dump.txt` to),
  XMA audio (`XMACreateContext`), an in-house "CrowdEngine" for zombie crowds.

  > **Retracted 2026-08-04 (capture finding 7): this list said "Bink video".** It was
  > inferred from the strings `Bink_1` and `Bink_2` in the image. The round-1 captures
  > show **no Bink decoder in use and no `.bik` file in the package** — cinematics stream
  > through an in-house "Movie Player Object" reading `.big` archives. The strings are a
  > dead or renamed path. The lesson generalises: a middleware name in an image proves
  > the *name* exists, not that the *codec runs*. Both template ports linked Bink, which
  > made the inference feel safe.
- **Archives**: `.big` containers throughout (`data/**/*.big`), plus `.bct` textures and
  `.bcf` fonts. `anm_%s.big` shows runtime name construction, so the VFS must handle
  arbitrary paths, not a fixed manifest.
- **244 imports** in `ppc/ppc_recomp_shared.h`. The graphics surface is the usual `Vd*`
  set including `VdSwap`, `VdSetGraphicsInterruptCallback` and
  `VdInitializeRingBuffer`; audio is `XAudioRegisterRenderDriverClient` +
  `XAudioSubmitRenderDriverFrame` + `XMACreateContext`, the same shape both template
  ports handled.
- **Shaders ship loose on disc**: `data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big`.
  Fable 2 needed a whole `.sbk` extraction pipeline to reach its shader microcode; here
  there are named vertex- and pixel-shader banks sitting in the filesystem. If those hold
  raw Xenos microcode they feed XenosRecomp almost directly — **this is the single
  biggest potential shortcut in the project and should be verified early**, before the
  renderer plan is committed to.

## 7. Immediate next steps

1. **Verify the shader banks** (§6). Cheap, and it changes the renderer plan.
2. **Capture Xenia ground truth** per `docs/xenia-capture-requests.md`. Nothing here has
   been checked against hardware yet; every port so far has been carried by these.
3. **Implement the 6 missing mnemonics** in `~/GithubRepo/XenonRecomp`, regenerate,
   confirm zero.
4. **Compile `ppc/`** — 227 TUs that have never been fed to a C++ compiler. Both template
   ports hit link-scale problems here and both solved them; see `docs/runtime-plan.md`.
