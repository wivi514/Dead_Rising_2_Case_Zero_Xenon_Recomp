# Runtime bring-up plan — from recompiled image to pixels

Written 2026-08-04 before any ground truth existed; **revised the same day** once round 1
was delivered. The findings ledger (`docs/xenia-capture-analysis.md`) is now the
authority on measured numbers — where this plan and that ledger disagree, the ledger
wins.

Two phases changed shape outright when the captures landed: **phase 5 (renderer)** got
its input settled, and **phase 7 (Bink) was deleted** — this game contains no Bink at
all. Both are marked below.

The playbooks: `~/GithubRepo/Fable2XenonRecomp/docs/runtime.md` (the deepest write-up)
and `~/GithubRepo/Asuras_Wrath_Xenon_Recomp/docs/runtime-plan.md` (the same plan applied
a second time). `runtime/` is ported from Fable 2 module by module; UnleashedRecomp is
GPLv3 and is a **structural reference only** — guest structs come from XenonRecomp's
`XenonUtils/xbox.h` (MIT).

No phase starts on guesswork; each has a ground-truth gate that says "done".

---

## Phase 0 — make the image compilable and honest *(prerequisite)*

> **Status 2026-08-04: PHASE 0 IS COMPLETE.** The image sits at **57,822 functions, the
> recompiler log is completely silent** (zero unrecognized, zero undecodable, zero
> switch-boundary errors, zero dropped branches — findings 13 and 14), **and all 228 TUs
> compile and link with zero warnings**. Phase 1 is next.

**0.1 Close the 42 unrecognized-instruction sites.** *(done — XenonRecomp `981afe9`)*
Six mnemonics — `lhbrx` (30), `stfsux` (5), `vsubuws` (4), `vspltish`, `vpkuwum`,
`vadduhs`. All six were implemented rather than only the ones a capture proves are
executed: the marginal cost is minutes each, and "never executed" only ever means "never
executed on the paths captured so far".

A seventh came out of the same work. `vadduws` was already implemented, but emitted
`simde_mm_adds_epu32` — an intrinsic that does not exist at any SSE level and that simde
does not synthesise. Case Zero has one `vadduws` site, so this would have failed 0.2 with
an error pointing at simde rather than the recompiler. **An "implemented" recompiler case
is only proven by a title that uses it and a compile that consumes it.**

`lhbrx`'s 30 sites did cluster, as hoped, though the shape is worth knowing: 7 functions
inside a single ~18 KB region (`0x82764CF8`–`0x82769338`), with 27 of the 30 in four
adjacent functions at `82768C78`–`82769338`. Dense byte-reversed halfword loads in one
contiguous module is the signature of little-endian structure parsing in a big-endian
title, which is what `docs/big-archive-format.md` says the `.big` index is. **Prime
candidate for the archive reader in phase 2** — a hypothesis from an instruction
histogram, not a confirmed identification.

**0.1b Dropped direct branches — a class nothing was measuring.** *(done)* Found because
the "zero `// ERROR:` comments" gate grepped for a colon the recompiler never emits, so
it could never match. The real count was 31. See finding 13 for the mechanism and the
two opposite repairs; `tools/find_dropped_branches.py` is now a required stage of the
function-list pipeline, not an audit.

**0.2 First real compile.** *(done — `runtime/`, 2026-08-04)* The 228 TUs (156 MB) had
never been fed to a C++ compiler. They now build clean: **0 errors, 0 warnings, 89 s on
16 cores**, into a 155 MB `libppc_image.a` and a 109 MB `cz_smoke`.

Both earlier ports hit link-scale problems here. This one did not, and the reason is
just size: 57,822 functions against Asura's Wrath's 78,825 and Fable 2's 91k.

Three things had to be right, none of which is discoverable by reading the generated
code:

- **Clang is not optional.** `ppc_context.h` defines `PPC_FUNC_PROLOGUE()` as
  `__builtin_assume(...)`, which is Clang-only. GCC 16 fails in *every one* of the
  57,822 function bodies. `runtime/CMakeLists.txt` selects `clang++` before `project()`
  — after it, CMake has already locked the compiler in — and warns if overridden.
- **The `mftb` shadow must not be in scope when the system intrinsics headers are
  read.** `cpu/timebase.h` is force-included ahead of everything, and a function-like
  `#define __rdtsc()` rewrites `<immintrin.h>`'s own `__rdtsc(void)` *declaration*. The
  build then dies inside a system header with an error naming neither this project nor
  the guest. Fix: include `<x86intrin.h>` at the top of `timebase.h` so the guard is
  already set, then `#undef`/`#define`.
- **Import stubs need `PPC_FUNC`, not `PPC_FUNC_IMPL`.** `PPC_FUNC_IMPL` is
  `extern "C"`; the image's references are C++-mangled because
  `ppc_recomp_shared.h` declares imports with a plain `extern PPC_FUNC(x)`. Getting
  this wrong links 244 undefined references to names that are visibly present in the
  stub file. The guest functions escape the same trap only because each carries an
  `__attribute__((alias(...)))` weak alias re-exporting the unmangled definition under
  the mangled name — which is exactly the seam every hook is built on.

**What did NOT need doing:** the 236 save/restore ladder helpers look like they need
stubs — declared in the shared header, called throughout the image, defined nowhere
obvious. XenonRecomp synthesises them from the `*_address` config keys and emits them as
`__imp____savegprlr_14` with a weak alias to `__savegprlr_14`. They resolve on their own;
stubbing them yields 236 duplicate symbols. Only the 244 kernel imports are genuinely
undefined (`tools/gen_import_stubs.py`).

**Gate met.** `runtime/build/cz_smoke` walks the whole `PPCFuncMappings` table — 58,303
entries — and validates every one. The link uses `--whole-archive` deliberately: a normal
static-library link pulls in only referenced objects, so an unreferenced TU with an
undefined symbol would link cleanly and the gate would pass while proving nothing.
Verified after the fact from the binary: 57,822 guest function symbols and all 244
imports defined, **zero** undefined. And the gate is known to be capable of failing — it
did, on the `PPC_FUNC_IMPL` linkage bug above.

The table is 58,303 entries against 57,822 functions because it also maps the import
thunks and ladders: 57,822 + 244 + 236 + `_xstart` = 58,303. The harness says "entries"
rather than "functions" for that reason.

**Gate:** the recompiler log is completely clean — zero unrecognized, zero undecodable,
zero switch errors, **and zero dropped branches** (that last one is not in the log at
all; run `tools/find_dropped_branches.py`) — *and* a smoke `main` that walks the whole
`PPCFuncMappings` table links and runs, forcing the linker to resolve every generated
symbol. The log half of this gate is now met; the link half is 0.2.

## Phase 1 — kernel HLE and guest bootstrap

Memory map, o1heap guest arenas, thread creation, events/APCs, NT timers, TLS, the VFS
over `assets/game/`, and XAM stubs. Written against A1's call order.

Two rules that are not negotiable, both learned expensively:

- **Stubs fail honestly, never fake success.** Fable 2's faked XMA context cost weeks.
- **A stub that returns an error but leaves its out-parameter untouched is worse than no
  stub** — the guest frequently ignores the status and reads the buffer anyway.
- **Put the guest arenas where the console puts them.** Games select between the 4 KB-
  and 64 KB-page virtual regions with `MEM_LARGE_PAGES` and then reason about the
  addresses; matching the console's map also makes our addresses directly comparable to a
  capture's. And every size the kernel *reports* must be rounded the way the console
  rounds it, not just allocated correctly.

**Gate:** `tools/kernel_call_diff.py` (port from Asura's Wrath) against A1 — our call
sequence is a prefix-match of hardware's out to the title screen.

## Phase 2 — the VFS and the `.big` archives

**The container format is cracked** — `docs/big-archive-format.md`, confirmed both from
the archives themselves and independently from A5's read pattern. No reverse-engineering
phase is needed here any more.

What the format implies for the VFS: the runtime probes an archive's header, reads its
index, then seeks to individual entries, so we need **random access within an archive,
not sequential streaming**. Paths are constructed at runtime (`anm_%s.big`), so the VFS
handles arbitrary paths rather than a fixed manifest. Gameplay opens 433 distinct
archives across 23,965 file opens (A2).

**Where the reader probably lives:** `0x82764CF8`–`0x82769338`, which holds all 30
`lhbrx` sites in the image — 27 of them in the four adjacent functions at
`82768C78`–`82769338` (finding 14). Byte-reversed halfword loads that dense in one module
is what little-endian structure parsing looks like in a big-endian title, and the `.big`
index is little-endian. Start there rather than from the file-open call sites.

**Gate:** the file-open sequence in our log matches A1's, in order, out to the title
screen.

## Phase 3 — window, present seam, input

SDL window, the present seam every later stage goes through, keyboard + XInput gamepad.
Cheap, and it makes every later phase observable.

## Phase 4 — GPU command processor

> **Prerequisite MET (2026-08-04, phase 0.3).** The `.xtr` decoder exists —
> `tools/xtr.py` plus three CLIs, documented in `docs/xtr-decoder.md`. Finding 10 is
> closed. Two results change how this phase must be gated:
>
> - **Gate on per-era aggregates, NOT on absolute frame index.** Two *hardware* runs of
>   one drive agree to 0.42% on aggregates but only **80.0% frame-exact**, with phase
>   drift concentrated at lag +3. A frame-indexed gate would report ~20% divergence
>   against a correct renderer. The noise floor for any later comparison is **0.42%
>   worst aggregate, 0.19% on draws**.
> - **`INDIRECT_BUFFER` is recorded one dword short** — the size dword lives in the
>   following `IndirectBufferStart`, and the IB body follows inline. A replay tool that
>   trusts either the recorded length or the header length feeds the command processor a
>   malformed packet (finding 10b). Also: start/end nesting is *not* balanced at the tail
>   of these captures, so a parser requiring balance rejects all of them.
>
> Known packet inventory, from `xtr_pm4_census.py --verify`: **21 distinct type-3 opcodes
> in B1, every one of them named** — no unknown packets in the frontend stream.

Execute the real PM4 stream: register file, fences, waits, indirect buffers, CP
interrupts. Gate it offline first with a `pm4_replay` target that walks the B1 `.xtr`
captures and reports zero unknown opcodes and zero desyncs, before any of it runs live.

Three stream semantics that cost the earlier ports real time and are likely to recur:
the headerless first-IB preamble; ring size units and wrap behaviour; and **a command
stream carrying the answers to its own waits** — the D3D driver's GPU waits poll a
retired-fence counter and a consumed-to pointer that no runtime could honestly invent,
and both are written by `EVENT_WRITE_SHD` packets the guest itself put in the ring.

Also: **a packet's contract can include *when* it runs relative to its neighbours.**
Asura's Wrath's graphics ISR reads a callback pointer out of the scratch mirror that the
stream arms immediately before the `INTERRUPT` packet and poisons immediately after, so
deferring the completion signal to the end of the walk — free, and what Fable 2 does —
calls the poison.

**Gate:** `pm4_replay` over every `.xtr` capture: zero unknown opcodes, zero desyncs,
swap count matching the capture's frame count.

**The inventory is already known and it is small: 21 type-3 opcodes, identical in B1, B1b
and B2** (finding 10d). Gameplay introduces none the frontend does not already use, so the
command processor can be developed and gated against **B1 alone** — 1.61 GiB rather than
B2's 7.95 GiB — which is a large saving on iteration time. The opcode list is in finding
10d; `xtr_pm4_census.py --verify` is what reports an exception to it.

`XE_SWAP` count equals frame count exactly in all three captures (1,089 / 881 / 4,082),
which is the cheapest cross-check that a replay walk has not lost frames. `ME_INIT` = 1
and `COND_WRITE` = 256 in every capture regardless of length — both init-only.

## Phase 5 — renderer  *(input settled)*

**The shader question is answered, and not the way the bootstrap doc guessed.** The loose
disc banks are `.vo` shader-object containers whose payloads are *not* the microcode the
guest submits — they carry build metadata including `.updb` debug paths, and share only
background-noise n-gram overlap with the real thing (finding 6). That shortcut is gone.

It does not matter, because Xenia's `dump_shaders` delivered the real input:
**455 distinct raw Xenos microcode blobs** — 120 frontend/menu (A1) and 335 gameplay
(A2) — as `*.ucode.bin.{vert,frag}`, each with disassembly and Xenia's own D3D12
translation alongside. That is exactly what XenosRecomp consumes, so no runtime
`SHADER_DUMP` hook is needed and this phase starts with its inputs in hand.

**Gate:** per-pixel diff against the E-series screenshots at the same frame.

## Phase 6 — audio

`XAudioRegisterRenderDriverClient` / `XAudioSubmitRenderDriverFrame` pump at 48 kHz, XMA
decode via ffmpeg. Asura's Wrath measured that title using **all 32 hardware XMA
contexts** — a partial context implementation did not survive a cinematic. A2 shows Case
Zero using contexts **0–17+**, so plan for the full set rather than the observed maximum;
the observed maximum is a property of the drive, not of the title.

## Phase 7 — movies  *(was "Bink"; rewritten — there is no Bink here)*

**Case Zero contains no Bink and no `.bik` files** (finding 7). Both template ports hook
RAD's `BinkDoFrame`, and that entire approach is inapplicable. Cinematics stream through
an in-house "Movie Player Object" reading `.big` cinematic archives (`ratinglogos.big`,
`700_prologue_intro.big`, `cinematics.big`).

This is a *harder* phase than it was on either template port, not an easier one: the
codec is unknown and has to be reverse-engineered out of the `.big` payloads, and there
is no middleware API to hook — the seam will be engine-specific, which is exactly the
kind of hook Asura's Wrath's gotcha #22 warns is fragile. It also means the movie era is
not the cheap early visible win it was there.

The one thing that does transfer is the gating discipline. Asura's Wrath's Bink decode
looked right, advanced its frame counter, and was one display frame stale in *every*
frame — visible only as ~60 bad 8×8 blocks in the one moving corner of a mostly-static
picture, wearing the diff signature of a localised codec bug. Only a per-pixel diff
against a reference decode caught it. **Gate any video work per-pixel, never on "it looks
right" or on a frame counter the decoder itself maintains.**

## Phase 8 — saves and content  *(shape measured)*

`XamContentCreateEx(…,"save",…,flags=0x1012)` → symbolic link `save:` →
`\Device\Content\1\` → `NtCreateFile(save:\DR2P000.DSF)` → **one `NtWriteFile` of
0x4A000 = 303,104 bytes** → `XamContentClose`. Load-back goes via
`XamContentCreateEnumerator` → re-mount → re-open (finding 12).

The whole save is a single write, which makes this materially simpler than Asura's
Wrath's. The physical save file was delivered with the capture, so the `.DSF` format can
be reverse-engineered offline without re-running anything.

---

## Measurement discipline, from day one

- A/B with **same-binary arms** (`ab_gate.py`, port from Fable 2).
- **A probe expensive enough to stall the game manufactures the stability it reports** —
  every instrument needs its own control.
- **No silent caps.** If a comparison bounds coverage (top-N, sampling, no-retry), log
  what was dropped; silent truncation reads as "covered everything" when it didn't.
- **Retract in place.** When a stated finding turns out to be an artifact, say so where
  it was claimed and explain the artifact.
