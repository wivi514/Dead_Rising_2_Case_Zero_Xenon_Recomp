# Xenia capture analysis — numbered findings ledger

What the round-1 captures actually established, as opposed to what the requests hoped
they would. One numbered finding per claim, each with the measurement behind it.

**Retractions happen in place** (see finding 2 and finding 6): when something stated here
turns out to be an artifact, this file says so where the claim was made and explains the
artifact, rather than quietly dropping it. That convention exists because both template
ports lost multi-session stretches to inherited claims nobody re-checked.

Captures: `Xenia logs/` (index: `Xenia logs/Xenia_Run_Content.md`, written by the
operator). Requests: `docs/xenia-capture-requests.md`.

Round 1 delivered **complete** on 2026-08-04: A1–A5, B1/B1b/B2, C1/C2, D, E — all as the
full game.

---

## 1. The trial trap was real, and it was the first thing that happened

**The single highest-value line in the round-1 request.** Xenia's `license_mask` defaults
to **0**, and at 0 `XamContentGetLicenseMask` returns an unlicensed mask and Case Zero
boots its **trial** path — the main menu carries an "unlock full game" option. The first
A1 take was the trial. Setting `[Content] license_mask = 1` removes the option and boots
the full game.

The trial is not a harmless subset. Both runs load an identical set of `.big` banks with
identical counts except one: `chuckwalkietalkie.big`, **1,164× in the trial vs 2× in the
full game**. That reload storm is the entire difference between the logs — 1,476 file
opens vs 314, 45.8 MB vs 13.9 MB. A runtime written against the trial boot flow would
have been written against a demo loop.

**Every Case Zero capture must run with `license_mask = 1`.** This class of error cannot
occur on either template port (both are disc titles), so nothing in the inherited
methodology would have caught it.

*Evidence:* `A1_NOTES.txt`; `cz_run1_TRIAL_license0.log.gz` kept as the counter-example.
Verified independently here: `license_mask = 1` is present in the canonical A1's config
dump header.

## 2. `NtReadFile` is `kHighFrequency` — A5 is the read oracle, not A1 or A2

The round-1 request said A2 would give us the `.big` seek patterns because "at level 3
Xenia logs `NtReadFile` with offsets and sizes". **That premise was wrong.**
`NtReadFile` is tagged `kHighFrequency`, so it is suppressed at plain `log_level=3`
regardless of drive.

Measured on the canonical A1: **2** occurrences of `NtReadFile` in the whole log, and
both are import-table *declaration* lines (`F 820005B8 829C2BD4 0F0 ( 240) NtReadFile`),
not calls. Zero actual reads. A2 is the same: 23,965 `NtCreateFile` against 0
`NtReadFile`.

> **Retraction, in place:** `A1_NOTES.txt` states "File-I/O lines (NtReadFile
> offsets/sizes into the .big archives) are PRESERVED for the .big-format oracle." That
> is not true of A1 and was carried over from the request's wrong premise. A1 preserves
> file *opens*, not reads. A2's notes caught the same error independently and correctly.
> The operator's index is right; this one line in the A1 notes is not.

**A5 is the oracle.** With `log_high_frequency_kernel_calls=true`, `NtReadFile` becomes
visible: **408 calls**, verified. See finding 3 for what else that unlocks and
`docs/big-archive-format.md` for the format it yielded.

## 3. High-frequency logging did not hang this title, and `flush_log=false` is why

The request warned A5 might deadlock, on the strength of Asura's Wrath's D1b livelock. It
did not. The operator's diagnosis is that the AW livelock was high-frequency logging **+
`flush_log=true`** — a synchronous in-lock disk write starving the present thread — and
that buffered writes avoid it. Case Zero's A5 ran the full A1 drive to the title screen
at 231 MB with no hang.

Surfaces that exist only in A5, all reporting **0** at plain level 3:

| call | count | note |
|---|---|---|
| `NtReadFile` | 408 | the `.big` read oracle (finding 2) |
| `VdSwap` | 3,130 | per-frame flip; params `0x500`×`0x2D0` = 1280×720 |
| `XamInputGetState` | 12,364 | controller poll surface |
| sync primitives | ~178,000 | the whole synchronisation surface |

**This is strictly better than Asura's Wrath managed**: there, the flip cadence was in no
kernel log at any level and had to come from the GPU stream. Here `VdSwap` is logged, so
boot→title frame pacing is available directly.

*Verified independently:* counts reproduced from `cz_run5.log`. `VdSwap` came out at
3,130 against the notes' 3,131, and `XamInputGetState` 12,364 against 12,365 — the
off-by-one in each case is the import-table declaration line, not a discrepancy.

## 4. Line prefixes: a `d>` filter silently drops a third of the log

Xenia's log is not one line shape. In A5 the prefixes are `d>` (3.88 M), `G>` (305 K),
`A>` (74 K), `/*` (39 K, continuations), `i>` (3.5 K), `!>`, `F>`, and unprefixed
continuation lines.

**`VdSwap` is logged at `i>`, not `d>`.** A first pass here counted kernel calls with a
`d> .*<name>` filter and got `VdSwap = 0` — a clean, small, entirely wrong number that
looks exactly like "this title never swaps". This is Asura's Wrath's gotcha #23 in a new
costume: *check the log's line shapes before filtering on one.*

Any tool this project writes against these logs must either match all prefixes or state
which one it assumes and why.

## 5. The forwards coverage oracle: 110 real entry points out of 17,115 executed

C1 (boot→title) executed 12,278 functions; C2 (gameplay) 17,118, a strict superset —
**+4,840 gameplay-only**. Restricted to our image (`0x82000000`–`0x82B40000`; C1's range
starts at `0x80050030` because Xenia also traces the kernel and xam modules) the union is
**17,115**.

Diffing that against what XenonRecomp emitted gives the missing entry points. The raw
diff is 1,224 addresses. **Only 110 of them are real**, and the three filters that get
from one number to the other are each a way to be confidently wrong:

| filtered | count | why |
|---|---|---|
| already recompiled | 15,823 | — |
| jump-table case labels and switch-parent interiors | 980 | finding 5a |
| import thunks | 134 | finding 5b |
| save/restore ladder rungs | 68 | XenonRecomp synthesises these |
| **genuine missing entry points** | **110** | applied to `config/CaseZero.toml` |

Result: **57,728 → 57,837 functions, zero switch-boundary errors, zero `// ERROR:`
comments in the generated code.**

> **RETRACTED, 2026-08-04 (see 5d and finding 13).** The last clause was worthless. The
> recompiler emits `// ERROR {:X}` — no colon — so the pattern `// ERROR:` could never
> match anything and the check was never actually run. The real count at that commit was
> **31 dropped branches, 13 of which this coverage pass had just introduced.** A grep that
> cannot match is not a clean result. The function count is also superseded: after
> repairing both classes the image is **57,822** functions.

### 5a. A case label is not an entry point, and adding it is destructive

Xenia's analysis calls any executed branch target a "function", and a `switch` case body
is a branch target — so recovered jump tables' case labels arrive looking exactly like
undiscovered entry points. Here that was **870 of 1,090** candidates, 80% of the list.

Adding one **splits the switch's parent function**, so every case at or past the split
falls outside the parent's extent and lowers to a bare `return;` — no epilogue, caller
resumes with the callee's non-volatiles. That is precisely the defect
`fix_switch_function_bounds.py` exists to repair, reintroduced by the tool meant to
improve coverage. The two do not converge: the repair widens the parent, the coverage
entry re-splits it, and they settle at a stable **8** errors across 4 parent functions.

Excluding labels alone is still not enough. Xenia also recorded entry points **4 bytes
past** a loop-back case target — `0x82670084` for label `0x82670080`, where `0x82670080`
is the `addi` advancing a cursor and `0x82670084` the `lbz` after it. No label filter
catches those, and each truncated its parent just the same. The tell was in the trace
itself: Xenia had recorded **both** addresses as executed functions **with the same end
address**. Two "functions" 4 bytes apart sharing an extent are one function.

So the filter is the whole switch-parent span — `min(labels, bctr)` to `max(labels,
bctr)` — on the reasoning that a genuine indirect-call target does not live inside
another function's body. Residual risk: an address that is both a case label and a real
indirect-call target would be excluded and miss at runtime. That failure is *visible and
localisable* (an indirect-call miss naming an address); the alternative is silent wrong
execution. If a miss ever lands on one of these, special-case it rather than dropping the
filter.

### 5b. Import thunks look different in a loaded image than in the XEX

The inherited scanner looked for the on-disk thunk shape — two loader-patched descriptor
words then `mtctr r11; bctr`. It found **zero**, and zero is the dangerous answer: all
244 thunks fell through, 134 of them into the override list.

Cause: `Xex2LoadImage` **overwrites** every thunk with `nop; nop; nop; blr` as it
registers the import's symbol name, and `tools/xex_image_dump` dumps `Image::data`
*after* `ParseImage`. The on-disk pattern cannot exist in our analysis image. Asura's
Wrath never hit this because its analysis image came from `decrypt_xex.py`, which
decrypts the file and never runs the loader — the two ports have analysis images at
different stages of loading and the same scanner cannot be right for both without saying
which it expects.

Matching both forms finds 244 thunks in `0x829C2624..0x829C3564` — exactly the 244
imports in `ppc/ppc_recomp_shared.h`, and exactly at the tail of `.text`.

### 5c. A repair tool that reports success while failing

Exposed by the above, and independent of it: when a case label points *backwards*,
`fix_switch_function_bounds.py` widened the function's start and then computed its end
with `next_func_after(start)` — using the **widened** start. That returns the original
function start, so the emitted entry ended exactly where the real function began. It
covered neither the `bctr` nor its cases, merged to a no-op against the existing entry,
and the tool printed **"0 new this round"** while the errors persisted unchanged through
a fixpoint loop.

Fixed by computing the end from the original start, and by extending down to the lowest
label (`min(start, min_label)`) rather than to `func_containing(min_label)`, which
needlessly swallowed the preceding function. A tool that reports convergence is making a
claim; check it against the thing it claims to have fixed.

### 5d. A loop header is not an entry point either — and the label filter cannot see it

5a excluded case labels and switch-parent interiors. That is not the whole trap. A **loop
header** is also a branch target, is also recorded by Xenia as a "function", and is in no
switch table, so it passes every filter in 5a untouched.

Nine did. Each split a real function; the tail half's loop-back edge then pointed into the
head half — a *different* function — and XenonRecomp silently dropped it (finding 13).
All 9 of the dropped backward branches in the image traced to a coverage-added address,
and none of the 18 pre-existing ones did.

The uncomfortable part is that no heuristic available *before* applying them separates a
loop header from a genuine indirect-call target. Of the nine: two pairs shared an end
address (`82671E70`/`82673A6C` both ending `82673FAC`; `8281927C`/`828192A4` both ending
`828192D8`) — the signature 5a already names. Three were implausibly small: `821672A0`
spans 12 bytes, `825E243C` 8, `827EBABC` 16. But two, `82819B80` and `8281D940`, span
0x9C0 and 0x9D8 bytes and look like perfectly ordinary functions.

So the coverage tool cannot be made right on its own. What *is* decisive is a measurement
taken afterwards — did adding this address cause a branch to be dropped? That is finding
13's check, and it is now a required stage of the pipeline rather than an optional audit.

## 6. The shader shortcut: the disc banks are NOT usable microcode — but it doesn't matter

The bootstrap doc called this "the single biggest potential shortcut in the project",
on the reasoning that Case Zero ships loose shader banks
(`data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big`) where Fable 2 needed a
whole extraction pipeline.

> **Retraction, in place:** the hypothesis is **not confirmed**. Those banks are `.big`
> archives of `<hash>.vo` shader *objects*, and their payloads are not the microcode the
> guest submits. Measured against the 455 microcode blobs Xenia observed the guest
> actually submit, a `.vo` payload shares **4 of 159** aligned 8-byte n-grams — while two
> *unrelated* dumped shaders share 4 of 73. That is background noise from common
> instruction encodings, not a relationship. Payload entropy is 5.03, so it is not
> compression hiding a match; the bytes are simply different. What a `.vo` payload does
> contain is build metadata, including the original path
> `c:\bcg\deadrisingprologue\intermediate\xbox360\shaders\a07a5e80.updb` — `.updb` being
> the Xbox 360 shader debug database.

**The renderer is unblocked anyway, by a different route.** Section D of the request —
which I flagged as unverified, having no way to check Xenia's cvar name from this
machine — works: `dump_shaders` is a path cvar in the `[GPU]` group, and it produced
**455 distinct raw Xenos microcode blobs** (120 frontend/menu from A1, 335 gameplay from
A2) as `*.ucode.bin.{vert,frag}`, each with a disassembly alongside and Xenia's own D3D12
translation. Raw guest microcode is exactly XenosRecomp's input.

So the outcome is the good one, reached the other way: **no runtime `SHADER_DUMP` hook is
needed**, and the renderer plan does not depend on cracking `.vo`.

## 7. No Bink. The movie path is in-house.

Both template ports hook `BinkDoFrame` — RAD's decoder linked into the XEX — and the
runtime plan had a whole phase for it. **Case Zero contains no Bink and no `.bik` files.**
Cinematics stream through an in-house "Movie Player Object" that loads `.big` cinematic
archives (`ratinglogos.big`, `700_prologue_intro.big`, `cinematics.big`).

Consequences: the Bink phase is deleted from the plan; the video codec is unknown and
must be reverse-engineered from the `.big` payloads; and the movie era is *not* a cheap
early visible win the way it was on Asura's Wrath. Grep `.big`, never `.bik`.

## 8. The `.big` container format is cracked

Full writeup: **`docs/big-archive-format.md`**. Summary: little-endian (in a big-endian
title), 0x18-byte header, 28-byte index entries `{name_offset, hash, size, size2,
data_offset, flags, reserved}`, a fixed-width name table, then payload. Confirmed against
the archives themselves and independently against A5's read pattern, which shows the
runtime probing the header, reading the index, then seeking to individual entries — so
the VFS needs **random access within an archive, not sequential streaming**.

This should transfer verbatim to Case West.

## 9. The 2 GiB `.xtr` cliff was a real constraint here — and the operator removed it

Boot→title alone is 1.61 GiB of GPU stream, and a brief gameplay test hit 1.92 GiB (96%
of the limit that destroyed Asura's Wrath's B2). The request's warning was load-bearing
rather than precautionary.

The operator then **fixed it at the source**: `trace_writer.cc`'s compressed-write path
used 32-bit `long`/`std::ftell`/`std::fseek` to seek back and patch each command's length
header, so past 2 GiB the offset wrapped and patched the wrong place. Swapping all four
sites to Xenia's portable 64-bit `xe::filesystem::Tell`/`Seek` and rebuilding produced a
**7.95 GiB B2 trace** — ~4× past the old cliff, valid header, finalized, no corruption.
The `.xtr` *format* never had the limit (per-command `uint32` lengths, not absolute
offsets), so a 64-bit sequential decoder reads it unchanged.

All future GPU captures on that fork are cliff-free. **This is the single most valuable
thing in round 1 for the renderer phase**, because it removes the constraint that would
otherwise have forced every gameplay GPU capture to be a bounded, monitored slice.

## 10. Determinism: size is not the metric

B1 is 1.61 GiB, B1b — an identical repeat — is 1.12 GiB, a ratio of 0.70. That is **not**
evidence of non-determinism: it is idle/load length plus per-run host fields (ASLR
addresses and the like), which also make a raw `cmp` meaningless, since the streams
diverge on host fields almost immediately.

The determinism question has to be asked per-frame, with a decoder, over the fixed
boot+movie prefix. Asura's Wrath's equivalent pair came out content-deterministic with
±2-frame phase jitter. **Not yet measured here** — it needs the `.xtr` decoder, which
does not exist in this repo yet.

Until it is measured, do not gate anything on absolute frame index, and do not treat a
B1-vs-ours difference as a defect without first establishing the noise floor.

## 11. Numbers worth having in one place

| quantity | value | source |
|---|---|---|
| functions executed, boot→title | 12,278 | C1 |
| functions executed, gameplay | 17,118 | C2 |
| gameplay-only functions | 4,840 | C2−C1 |
| executed within our image | 17,115 | C1∪C2 filtered |
| recompiled functions (now) | 57,837 | after finding 5 |
| coverage of the image | ~30% | 17,115 / 57,837 |
| `.big` archives opened, gameplay | 433 | A2 |
| file opens, gameplay | 23,965 | A2 |
| guest threads, gameplay | 86 | A2 |
| distinct shaders, frontend | 120 (91 ps + 29 vs) | A1 / D |
| distinct shaders, gameplay | 335 raw microcode | A2 / D |
| XMA contexts in use | 0–17+ | A2 |
| swap resolution | 1280×720 | A5 `VdSwap` params |
| save file | `save:\DR2P000.DSF`, 303,104 B, one write | A3 |

## 12. Save shape

`XamContentCreateEx(…,"save",…,flags=0x1012)` → `XamContentCreateInternal("save")` →
symbolic link `save:` → `\Device\Content\1\` → `NtCreateFile(save:\DR2P000.DSF)` →
**a single `NtWriteFile` of 0x4A000 = 303,104 bytes** → `XamContentClose`. Load-back goes
through `XamContentCreateEnumerator` → re-mount → re-open.

The whole save is one write, which makes this phase much simpler than Asura's Wrath's.
The physical save file was delivered (`cz_A3_save_DR2P000.zip`) so the `.DSF` format can
be reverse-engineered offline without re-running anything.

## 13. Dropped direct branches — a defect class nothing here was measuring

Found while closing the unrecognized-instruction sites (finding 14), by noticing that the
"zero `// ERROR:` comments" claim in finding 5 was grepping for a colon the recompiler
never emits.

When a direct branch (`b`, `bl`, `beq`, …) leaves the current function, XenonRecomp looks
the target up in the symbol table. The lookup is **exact-start** — `SymbolTable::find`
runs `equal_range` on the address, so it matches only symbols beginning at exactly that
address, never a symbol that merely *contains* it. If the target is not the start of a
known function, there is nothing to call, and the recompiler emits:

    println("\t// ERROR {:X}", address);

and moves on. Nothing is printed to stdout, the run exits 0, and the generated C++
compiles fine — the control transfer simply never happens, and execution falls through
into whatever follows. This is the same shape of defect as an unimplemented mnemonic:
**wrong execution, no build failure.** `tools/find_dropped_branches.py` measures it.

Every dropped branch means a function boundary is wrong, and the branch *direction* says
which boundary and which repair — the two are opposites:

| direction | meaning | repair | count found |
|---|---|---|---|
| backward (target < function start) | a loop header was declared a function, splitting a real one; the tail's loop-back edge now leaves the function | remove the spurious start | 13, **all** coverage-added |
| forward (target > function start) | the branching function was truncated; the target is its own outlined cold block | widen the branching function until the target is inside it | 18, **none** coverage-added |

The forward set came from the XEX's `.pdata` table, not from linear sweep: the compiler
outlined cold blocks and `.pdata` describes each outlined region as its own entry, so a
`beq` into a cold block is a forward branch to a non-entry address. Widening works
because XenonRecomp only consults the symbol table when the target is outside the current
function, so growing the function turns the dropped branch into a plain local `goto`. It
does *not* remove the `.pdata` entries in between — the exact-start lookup means a
spanning entry cannot hide a nested one — so those addresses end up emitted both inside
the widened function and as their own `sub_`. That duplication costs code size, not
correctness, and is already the accepted outcome of `fix_switch_function_bounds.py`
widening a switch parent over its case labels.

Widening cascades: covering one target extends the function past further truncations, so
it needs a fixpoint loop. Here it converged after 3 rounds (18 → 3 → 0).

**Verification that nothing was lost.** Widening absorbed 6 functions
(`825DD928`, `825DF244`, `8281D4C4`, `828573A8`, `8287D298`, `82883A9C`) — the linear
sweep stops inventing separate functions inside a range an earlier function now claims.
All 6 are inside a widened parent, and **none appears in the C1/C2 coverage traces**, so
no address hardware ever entered as a function stopped existing. The 9 pruned addresses
*do* all appear in the traces, but that is not evidence they are functions — they came
*from* those traces, and their recorded extents are what convict them (5d).

Net: **57,837 → 57,822 functions** (−9 spurious splits, −6 absorbed), and the image now
has zero dropped branches.

**The order matters.** `coverage_to_function_overrides.py` proposes and
`find_dropped_branches.py` disposes; run the latter after the former, every time, and
treat a nonzero backward count as the coverage pass's error rather than a new problem.

## 14. The six unrecognized mnemonics — and a seventh that could not have compiled

An unrecognized instruction is not a TODO. `Recompile()` returns false, one line is
printed, and **nothing at all is emitted for that instruction** — the guest operation
silently becomes a no-op. 42 sites across six mnemonics: `lhbrx` (30), `stfsux` (5),
`vsubuws` (4), `vspltish`, `vpkuwum`, `vadduhs`. All implemented upstream in
`~/GithubRepo/XenonRecomp` (commit `981afe9`); the image now recompiles with zero.

`lhbrx`'s 30 sites do cluster, which is worth knowing before the VFS work. They fall in
**7 functions inside a single ~18 KB region**, `0x82764CF8`–`0x82769338`:

    sub_82768C78  8      sub_82764CF8  1
    sub_82769338  7      sub_827669D8  1
    sub_827691E8  6      sub_82766FC8  1
    sub_82769270  6

27 of the 30 are in the four adjacent functions `82768C78`–`82769338`. A byte-reversed
halfword load appearing that densely in one contiguous module is the signature of
little-endian structure parsing in a big-endian title — which is exactly what
`docs/big-archive-format.md` says the `.big` index is. Treat that region as the prime
candidate for the archive reader when phase 2 starts; it is a hypothesis from an
instruction histogram, not a confirmed identification.

The seventh was already "implemented" and could never have worked: **`VADDUWS` emitted
`simde_mm_adds_epu32`, which does not exist.** No SSE level has a 32-bit unsigned
saturating add and simde does not synthesise one, so any title using `vadduws` produced
C++ that fails to compile. Case Zero has exactly one such site, which would have blocked
its first compile (phase 0.2) with an error pointing at simde rather than at the
recompiler. Both 32-bit saturating cases now use algebraic identities instead of
nonexistent intrinsics:

    vadduws:  a + min_epu32(b, ~a)      overflow iff b > ~a, and then a + ~a = 0xFFFFFFFF
    vsubuws:  max_epu32(a, b) - b       a - b when a >= b, 0 otherwise

Verified by differential test against scalar references written from the PPC definitions,
modelling XenonRecomp's fully-reversed host vector layout: 200,153 cases (exhaustive over
saturation-boundary word pairs, plus 200k random and mixed-edge vectors), zero failures
under `-O2`, `-msse4.1`, `-mavx2` and `-O0`. Negative controls confirm the test can fail —
unswapping `vpkuwum`'s operands fails 199,957 cases, dropping `vsubuws`'s clamp fails
184,854. **A vector test that has never failed has not been shown capable of failing**;
the operand-order convention in particular is invisible to inspection.

---

## Open questions round 1 did *not* answer

- **`.xtr` decoding.** Nothing in this repo reads a GPU stream yet. Findings 9 and 10 both
  end at "needs the decoder".
- **The movie codec** (finding 7). Unknown, and now on the critical path for the boot era.
- **Gameplay-era `.big` seek order.** A5 covers the boot set; the container format is
  uniform so this is not needed to decode it, and an A2b capture is only warranted if
  streaming *order* turns out to matter.
- **`.vo` internals** (finding 6). Not on the critical path.
- **`sc`/`sd`/`ss` shader bank contents** (finding 8).
