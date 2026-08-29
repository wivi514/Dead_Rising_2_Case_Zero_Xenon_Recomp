# Dead Rising 2: Case Zero Xenon Recomp — project guide

Static recompilation of the Xbox 360 XBLA title **Dead Rising 2: Case Zero**
(Capcom / Blue Castle Games, 2010) using **XenonRecomp** + **XenosRecomp** (hedge-dev's
faithful recompiler pair, the ones UnleashedRecomp uses).

This is the **third** game ported with this pipeline in this workspace. The two before it
are the playbook, and most infrastructure and every hard-won gotcha transfers — **read
them before re-deriving anything**:

- `~/GithubRepo/Fable2XenonRecomp` — the original and the deepest (91k functions → a live
  rendered world). Its `CLAUDE.md` is the project journal; its `docs/` hold the reusable
  methodology.
- `~/GithubRepo/Asuras_Wrath_Xenon_Recomp` — the second port, which proved the template
  transfers and consolidated the gotchas into a numbered list.

**Dead Rising 2: Case West is planned next** (`~/GithubRepo/Dead_Rising_2_Case_West_Xenon_Recomp`,
package present, not started). It is the same engine and the same studio, so essentially
everything learned here should carry over — worth keeping that in mind when deciding
whether a finding belongs in a Case Zero doc or in a general one.

## Template status: what is inherited vs. what is new here

Inherited wholesale:
- Repo shape, gitignore policy (game data + generated `ppc/` untracked), tools, docs style.
- Runtime architecture: kernel HLE with *honest-failure* stubs, o1heap guest arenas,
  guest-thread bootstrap, PM4 command processor, Vulkan renderer on XenosRecomp SPIR-V,
  XMA via ffmpeg, SDL window/input. UnleashedRecomp is GPLv3 → **structural reference
  only**; guest structs from XenonRecomp's `XenonUtils/xbox.h` (MIT).
- Ground-truth discipline: Xenia text logs, `.xtr` GPU stream traces,
  `--trace_function_data` coverage, and A/B methodology with same-binary arms.

**New here, because this is the first XBLA (not disc) title in the workspace** — this is
the part a future Case West port will reuse verbatim:
- `tools/extract_stfs.py` — STFS/SVOD container reader. There is no ISO and no
  `extract-xiso` step; the game is a single hash-interleaved block filesystem.
- `tools/xex_image_dump.cpp` + `build_xex_image_dump.sh` — offline image via XenonRecomp's
  own loader, because this XEX is LZX-compressed and `decrypt_xex.py` cannot read it.
- `tools/find_save_restore.py` — structural ladder scan, replacing the hand-encoded
  byte-pattern greps the earlier ports used.
- A XenonRecomp patch for the **devkit AES key** (`docs/xenonrecomp-upstream-bugs.md`).

## Transferable gotchas

**THE FULL NUMBERED LEDGER IS `docs/gotchas.md` — 500 entries, and every "gotcha N"
reference in this repo and in the docs resolves there.** It was split out of this file
on 2026-08-08, when this file reached 308 KB and was being loaded into every session
whole. Read it **before making a measurement claim, adding an instrument, believing a
zero, or trusting a number an earlier session wrote down**; those four situations
produced almost all of it.

The ten that bite most often, as one-liners. Each is a summary, not the entry:

3.  **A zero is a detection failure, not a fact.** XenonAnalyse found zero jump tables
    here; our scanner found 234. Applies to every number a detector prints.
5.  **Kernel stubs must fail honestly, never fake success** — and a stub that returns an
    error but leaves its OUT-PARAMETER untouched is worse than no stub. See also 59 and
    201: when a return value is a predicate or a computed value, "fail honestly" has no
    spelling and implementing it is the only correct option.
7.  **A probe expensive enough to stall the game manufactures the stability it reports.**
    Every instrument needs its own control — and see 151, an arm with no counter cannot
    be shown to have engaged, and 223, an instrument on a hot path can cancel the effect
    it is measuring exactly.
13. **A capture request, a plan, and your own status note all have a shelf life.**
    Re-read them against the current ledger before believing their conclusions.
25. **A grep that cannot match is not a clean result.** Check the emitter before
    believing a zero — and 109, a capped or thinned log line is not a count.
30. **A test that has never failed has not been shown capable of failing.** Break the
    implementation on purpose and confirm the test screams. Applies to diagnostics and
    to instruments as much as to tests (94, 158).
50/51/86. **A rate measured once is a fact about that afternoon, and the control is the
    old binary run NOW** — not its remembered numbers. 159: a bimodal arm makes every
    single-run claim a coin flip.
133/127. **One frame of an animated scene is ONE SAMPLE**, and that applies to LOOKING,
    not just to measuring.
172/268. **A retirement is only as good as the ORACLE it was measured on** — and YOUR OWN
    STUB IS AN ORACLE. Re-ask your earlier A/Bs whenever an upstream defect is fixed. A
    three-configuration same-binary arm retired "the prologue waits on audio" in part 16;
    it was true, and a real decoder proved it in phase A/V.
267. **A guest structure handed to a DMA device holds PHYSICAL addresses**, and in a flat
    recompiler map those are not the ones the CPU uses. Cost this port its whole audio
    subsystem for 28 parts. Print the DESTINATION on every file-IO trace.
190/103. **A gate that needs a human is a capture request in disguise** — extend the arm
    until it is not. 222 is the performance form: ask what the WORST case renders and
    whether your harness can reach it.
237/238. **A MEAN frame time measures this title's vblank pacing floor, not your change**
    — read medians and the share of frames pinned to a 16 ms multiple, which is the far
    more sensitive statistic (10% -> 97% where the mean moved 1.7%). And **a profiler
    column that falls to zero is not a saving until you find where the replacement work
    got charged**: `streams` read 0.0% while the guard that replaced it doubled `record`.

## Inherited from the Fable 2 port: shared-decode cross-checks, and a do-not-chase list

Everything below is **hardware-level decode**, not title-specific, so a defect found in
one port is a defect in the other unless this one was written differently. Checking is
minutes; diagnosing the symptom is days. **Both of the first two were checked in
session 21 and this repo is correct on both** — recorded so the next session does not
re-check them, and so Case West can check them in one grep.

| shared decode | correct form | this repo |
|---|---|---|
| fetch-constant SIZE field — the endian bits occupy the low 2 bits of `fdw1`, so `fdw1 & 0x7FFFFF` swallows them (reads ~4x too large, permits reads past the buffer, and *under*-reports past ~2^21 dwords) | `(fdw1 >> 2) & 0xFFFFFF` | ✓ `gpu/xenos.h:125`, and gotcha 110 is the same finding arrived at independently |
| `num_format_all` INTEGER semantics — a fetch declaring unsigned/integer on `k_8_8_8_8` bound as `R8G8B8A8_UNORM` delivers 0..1 where the guest asked 0..255 (typically packed bone indices, TEXCOORD-wrapped as 360 titles do) | deliver the integer as its own value into a FLOAT input | ✓ via `USCALED`/`SSCALED` in `XenosVertexFormat`, gotcha 122 |

On the second: Fable 2 fixes it by emitting `* (2^bits − 1)` at translation time and
warns *against* rebinding to a UINT format. `USCALED`/`SSCALED` is not that rebind — it
is the same semantic (an integer delivered as its own value into a float input) obtained
without touching the emitter or the vertex input signatures. Both are correct; ours
costs nothing in XenosRecomp, which matters because that recompiler is shared.

**Confirmed NON-issues — do not chase these.** Measured over ~860 shader blobs and 971
vertex fetches in the **Fable 2** bank, so the provenance is another title and the
census has not been repeated here (one pass over our 336 shaders would settle it, and
`tools/gdis.py` plus the meta sidecars are enough to do it):

- the guest requests **8-in-32 endian on 100%** of fetches;
- **`exp_adjust` is declared but zero everywhere**;
- the Xenos compiler emits a **`yxwz`-shaped destination swizzle on ~87% of 16-bit
  fetches** (identity on 32-bit) that compensates the 8-in-32 pair transposition, and
  XenosRecomp already honours it on both the declared and `XeVfetchDep` paths.

**That third one was a live lead here and PART 37 CONFIRMED IT — it was the whole
striped-material class (item 0s).** The shader's destination swizzle IS the complete
correction: our `CopySwapped` leaves 16-bit pairs in exactly the state the real fetch
pipe hands the shader, so `g_SwappedTexcoords` was compensating A SECOND TIME, and
lightmap UVs (16_16 TEXCOORD2 fetches) arrived transposed — baked prop shadows painted
as hard-edged black blotches on the tanker, Dick's far LOD and the pawnshop boards.
§6n's frame-wide null was honest and blind: the damage is localized to lightmapped
props. The mask now defaults to ZERO; `CZ_VK_TEXCOORD_SWAP=1` is the control arm.
`docs/phase5-notes.md` §6bo, gotchas 291-292. For Case West: publish NO texcoord swap
mask; trust the microcode's own swizzles.

## Layout

- `config/CaseZero.toml` — XenonRecomp main config: helper addresses, plus 139 function
  overrides from three sources that **merge, never replace** each other — switch-tail
  repairs (`tools/fix_switch_function_bounds.py`), coverage-recovered entry points
  (`tools/coverage_to_function_overrides.py`), and truncated-function widenings
  (`tools/find_dropped_branches.py --widen`). Regenerating any of them from a stale
  `ppc/` silently under-reports; always rebuild `ppc/` from the committed config first.
- `config/CaseZero_switch_tables.toml` — 232 jump tables (105 absolute, 85 offset8,
  42 offset16, 6,114 labels) from `tools/find_jumptables.py`. **XenonAnalyse finds zero
  here** — see gotcha 3.
- `assets/package/` — the XBLA STFS package as delivered (gitignored; copyrighted).
- `assets/game/` — what `tools/extract_stfs.py` unpacked out of it: `default.xex` +
  `data/` (gitignored).
- `assets/game/default_image.bin` (+ `.sections`) — the loaded image for offline
  analysis, from `tools/xex_image_dump`.
- `ppc/` — generated C++ (gitignored; 156 MB, 57,822 functions, regeneratable).
- `tools/` — analysis scripts. Several copied from the earlier ports; provenance in
  their headers. `gdis.py` is the guest disassembler and is usually the right first
  stop for any question about what the title's own code does.
  `import_call_sites.py` is the one to reach for when implementing a kernel import:
  the capture has no return values, so the guest code that consumes the result is the
  specification (finding 29).
- `docs/` — the project's memory. **Read in this order for a new session:**
  - **`xenia-capture-analysis.md`** — the numbered findings ledger, and the authority on
    any measured number: where another doc disagrees with it, it wins.
  - **`gotchas.md`** — the 500-entry transferable ledger. Every "gotcha N" resolves here.
  - **`windows-build-setup.md`** — **the verified Windows runbook.** `ssh czwin` is the
    build laptop; read this before touching it. Three compilers, one per target, and none
    interchangeable.
  - **`release-plan.md`** — **THE LIVE PROGRAMME as of part 82.** Five milestones (A
    shippable tree, B Windows, C macOS, D first-run shaders, E packaging); **A is complete
    and D.1 is done**, and its **§9 is the execution record with every gate's measurement
    and §9.2 what is owed**. Its §1.4 carries a retraction in place.
  - **`port-history.md`** (what each session established) and **`open-items.md`** (the
    backlog, in order) — both split out of this file on 2026-08-08.
  - **`part69-night-plan.md` — still the live plan, and its §3 is the live path, but
    ITEM 1 IS ANSWERED.** Part 69 established that the occluder set is no longer the
    defect (the primary ray resolves the real world and the shadows are still wrong);
    part 70 closed §3's item 1, the sun, which was a confound and not a defect
    (`phase5-notes.md` §6dc). §1 has been run and answered; §2 is explicitly NOT the
    work; §3's items 2 and 3 — the origin bias and the ray length — remain.
  - `rt-remix-plan.md` — the plan part 69 executed, five items taken from
    `rtx-remix-prior-art.md` (which records the licence: DXVK zlib/libpng, NVIDIA's
    `rtx_render/*` per-file MIT). **Items 0-3 shipped; item 4 is still open.**
  - **`d3d-translation-plan.md`** — the renderer-architecture pivot, its recon tables and
    licensing, plus the per-phase build-out records. **The first read before any renderer
    work.** `d3d-kickoff.md` and `d3d-phase-c{,2..26}-kickoff.md` are the per-part
    hand-offs, each superseding the last; the newest is the live one. A kickoff's most
    valuable section is its list of the parts of that phase that **already exist** and
    would otherwise be rewritten from the plan text — write that section for every phase.
  - `phase1-notes.md` / `phase3-notes.md` / **`phase5-notes.md`** — the per-phase records
    of what the runtime work found that neither the plan nor the kickoff predicted, with
    `phase{1,3,5}-kickoff.md` the matching hand-offs and `runtime-plan.md` the phase plan.
  - **`phase-av-notes.md`** (sound, the cinematic that was waiting for it, and part
    29's diagnosis of the loop that followed) with `phase-av-plan.md` the plan it
    executed, `phase-av-kickoff.md` the phase hand-off and `part29-kickoff.md` its
    successor. `d3d-phase-c28-kickoff.md` records how the white-surface chain
    was built, but **two of its eight steps are retired and its item 0 is answered** —
    read `phase5-notes.md` §6ba before following anything in it.
  - **THE LIVE HAND-OFF IS ALWAYS THE HIGHEST-NUMBERED `partNN-kickoff.md`**, and it
    supersedes every earlier kickoff on "where the port is". **IT IS
    `part86-kickoff.md`, AND THE SUBJECT IS THE RELEASE** — `docs/release-plan.md` is the
    programme and its **§9 is the execution record**: milestones A, B, D and E complete
    and gated (§9.8 is part 85 — both release artifacts exist, the whole §5 checklist ran,
    and CI exists), and what remains is the §9.8 owed list (Windows CI's first live run,
    the pre-warm key file, the glibc floor, macOS). ~~It is currently
    `part82-kickoff.md`~~, and **PERFORMANCE IS PARKED AS OF PART 81** — the operator's
    instruction closing it was *"we'll switch to something else then performance"*. THE
    SUBJECT IS OPEN; part 82's kickoff §0 is the one thing to read before anything else,
    because **two changes are shipping ON BY DEFAULT whose milliseconds were never
    measured** and their control arms are the first bisection for any picture complaint.
    ~~**THERE IS A LIVE PLAN AGAIN AS OF PART 80: `docs/perf-plan-part81.md`**~~ — it is
    the reference that RESUMES performance rather than a live plan, and it is not
    exhausted: its §1.3 campaign and its §3 were never run, and its §5 records in place
    what part 81 did with the rest. It was the first plan since
    part 73 exhausted its predecessor, because part 80 is the first part since then to leave an
    item concrete enough to plan (buildable, threadless, and incapable of changing a pixel).
    **Read its §1.0 before writing any code**: it is a census that decides whether half of item
    0 exists at all. Part 73 ran the last unrun item in `docs/perf-plan-autonomous.md` and
    **every item in that plan is now closed, refuted or shipped**; it is kept because it
    carries part 73's four retractions in place, as does `perf-plan-part72.md` (the item
    table). `part72-fix-plan.md` is what the operator sittings established, and
    `perf-state-parked.md` is the reference the item designs came from and is NOT
    superseded. **`docs/part82-kickoff.md` is the first thing to read after this file.**
    ~~`docs/part81-kickoff.md` is the first thing to read after this file, and
    its §1 is the board, in order, WITH EVERY ITEM'S MILLISECONDS ATTACHED~~ — its item 1
    (the guard's 86.2 MB) is CLOSED by part 81 and the rest of its board stands — because there
    is a per-region GPU split now (`CZ_VK_GPU_PASSES=1`, and as of part 79 it carries a PASS
    EXTENT CENSUS too) and an item without a number is a guess.
    **AND AS OF PART 80 THE BOARD IS MOSTLY EMPTY, WHICH IS ITSELF THE FINDING.**
    ~~(1) **parallel command recording**, the largest thing left and re-priced upward by
    part 78's regime change (2.38 ms of headroom at 9,000-12,000 draws, fence 0.00) —
    **measure it at 8,000+ draws or not at all**~~ — **REFUTED IN PART 80 FOR THE COST OF TWO
    RUNS.** `CZ_VK_NO_DRIVER_RECORD=1` measured the driver's own share of the record path at
    **251 ns a draw = 2.33 ms/frame**, so **1.56 ms with three workers** against a
    pre-registered 1.5 ms kill — before capture, re-establishment or scheduling, and with
    `ThreadBudget_Take` granting a `record` pool **zero** threads (§6eb §3). It had been
    re-quoted upward through three hand-offs as a SHARE while part 18's bind cache quietly
    ate it (gotcha 473). ~~(2) the resolve clears, 580 Mpixel written for 33 rendered but a
    0.601 ms ceiling and a scoped arm that is a wash; (3) the resolve copies, 7.0 full EDRAM
    surfaces a frame, 0.723 ms~~ — **BOTH DEAD ON REGIME, WITHOUT A RUN**: the fence is
    **0.00 at every band from 5,000 draws up** with 2.3-3.1 ms of headroom, so a GPU saving
    converts to nothing until the CPU falls by the whole headroom, and their combined ceiling
    is less than it (gotcha 476). (4) the untile, load-frame only and the arithmetic is still
    owed. **The corrected per-draw CPU decomposition (§6ec §1) says there is NO single large
    CPU item left**: record 524 ns/draw (driver 251, ours 273), other 323, textures ~167,
    constants ~161 — and three "remember the answer" items died in one session because this
    renderer's remaining cost is its CHANGE DETECTORS (gotchas 474, 475).
    **THE ROUTE PROBLEM IS SOLVED: `tools/part80_crowdroute.sh` replays the OPERATOR'S OWN
    route at 9,300-9,700 draws unattended**, reproduces their regime band for band, and has a
    measured ±2.9% floor. No CPU item should be measured on `autoroute.sh` again.
    ~~(1) **the post chain**, 36 passes a frame and 1.43 ms, never decomposed~~ — **that was
    part 79's item 2 and it is REFUTED**: the extent census says three extents at 60-182 us
    carry 76% of the `1 draw` class and they are the title's own shaders at half, quarter and
    full resolution; the pure-overhead end is 0.036 ms/frame, 3.6% of the class (§6dx).
    ~~(2) **drop the wait in `FlushTextureUploads`**, carried over untouched and the only
    remaining item definitely worth something at the OPERATOR's load~~ — **that was part 79's
    item 1 and it is SHIPPED**: the flush goes 999-1138 us to 106-114 us, −89.8%, **and the
    autonomous route measured the frame time as a NULL** because it is GPU-bound and the pump
    moved its blocking to the fence (§6dw). **The operator session is the one thing owed.**
    ~~(1) **the GPU**, which this project has never touched~~ — **that was part 78's item 1
    and it is DONE**: it has a breakdown now, and the largest thing in it that is not the
    game was **137 image barriers a frame at ALL_COMMANDS, 11.0% of the device's frame**.
    −11.9% at crowd load (§6du). ~~(2) **pipeline
    compilation on the load frame**~~ — **priced and DEAD**: 8.8 ms of a 158-165 ms burst
    frame, 5.6%, against a standing 40 ms kill (§6du §5). ~~(3) the
    untile loop, **honestly priced as SMALL** — §6ds §10; (4) parallel command recording, at
    9,000+ draws only.~~ — both carried into part 79's board. ~~(1) the texture path, fully specified in `part75-kickoff.md` §1
    (take the 469 ms DECODE half first)~~ — **that was part 77's item 1 and it is DONE**, and
    the specified fix turned out to be 17% of it: the cost was one `vkAllocateMemory` per
    texture. Burst frame **−121 ms, −42.4%**, decode −68%, submit −95% (§6ds). ~~(1) the F8/F9 readback~~ — **that was part 76's item 1 and
    it is DONE**: −2.13 ms, −16.4% of the crowd frame, gated (`phase5-notes.md` §6dq).
    `part76-kickoff.md` was the live hand-off and its §2 phase table is now stale in one
    row and understated in every other, because every number in it was taken with the
    readback running. (`part74-kickoff.md`
    said "outside the renderer"; part 74's own decomposition RETRACTED that — the residual
    is 0.0 ms on every hitch frame and the cost is inside `Pm4_Execute`.) `part71-kickoff.md`
    remains the RT-SHADOW hand-off for a feature that is PARKED, not deleted.** State the rule as well as the name, because this line said
    "`part32-kickoff.md` is the LIVE one" for nineteen parts after it stopped being true
    — a stale pointer in the file every session loads whole is the one documentation
    defect that misroutes a session before it has read anything else (gotcha 13).
  - **PERFORMANCE IS THE LIVE SUBJECT AGAIN AS OF PART 71.** ~~`docs/perf-plan-part71.md`
    IS THE PLAN~~ — **superseded twice: the plan is `docs/perf-plan-part72.md` and the live
    FIX LIST is `docs/part72-fix-plan.md`.** Part 71's is kept because it was executed and
    records its own two retractions in place. (This line named part 71's plan for a part
    after that stopped being true, which is the stale-pointer defect described at the
    bottom of this file — gotcha 13 — and it is why the rule is stated as well as the
    name.) The operator's instruction closing part 70: *"We'll stop for now with
    trying to get ray tracing running. Disable that we can select it in game. We'll now
    switch to fixing performance issue."* Its §0 is the rule the whole plan turns on: the
    frame at their soak has not been measured since part 58 and thirteen parts have
    shipped since, so **re-baseline before pricing any item**. `perf-state-parked.md`
    below is NOT superseded — it is the reference the plan is built on.
  - **~~PERFORMANCE IS PARKED AND~~ `docs/perf-state-parked.md` IS THE REFERENCE THAT
    RESUMES IT** — the operator's instruction closing part 55: *"Save all of what is needed
    for performance later on and all your finding. We'll switch to fixing the last few
    visual bugs for the next few sessions and we'll come back to performance later."* It
    carries where the frame is at THEIR load, the pump thread's current symbol table, the
    four remaining items in order with their risks, the three ways part 55 got a
    measurement wrong, every arm that exists, and the four things owed. Do not re-derive
    any of it.
  - ~~**`perf-plan-part55.md` IS THE LIVE PERFORMANCE PLAN**~~ — superseded by the above,
    and kept because its §0 and §0b were EXECUTED: §0 is the honest answer to the
    operator's "unless you tell me it's not possible" (possible, but the ceiling is 5-6
    busy threads, not 16, because the PM4 walk is serial and draw ORDER is semantic) and
    §0b is the thread budget that shipped. **Its prediction was half right in an
    instructive way**: it forecast roughly a third off the frame from three PARALLEL
    items, and part 55 delivered −18% at the operator's load with none of them, by
    deleting container lookups instead (gotcha 362).
  - ~~**`perf-plan-part52.md` IS STILL THE LIVE PERFORMANCE PLAN**~~ — superseded, and kept
    because its §9b/§9c/§9d record what parts 52-54 corrected in it (built from a `perf`
    SYMBOL budget rather than a phase table). Six of its items have shipped — four in
    part 52 and **items 1.1 and 1.3 in part 53, which are the first work this port has
    moved off the pump thread**. It records its own corrections in place: **§9b** the two
    statements part 52 refuted (item 1.0's probe key, item 2.1's sizing) and **§9c** what
    part 53 established, including the one thing the plan never budgeted — a (b) item's
    BILL. **`perf-plan-part50.md` is history.**
    Read `docs/part56-kickoff.md` first, then `phase5-notes.md` §6cl (part 55), §6ck (part 54), §6cj
    (part 53) and §6ci (part 52). **§6ci §5c is RETIRED as of part 54**: it said the
    headless route sits on the frame cap so an A/B there reads zero whatever the change
    was worth, and the cap default moving 60 -> 500 took the route off the rung — the pump
    is 93.7% of a core there now, where part 53 closed at 50.3%. §6cg (part 50) and §6ch (part 51) are the earlier corrections.
    `perf-cpu-plan.md` and `perf-plan-part{47,48}.md` are executed predecessors.
  - `instruments.md` (every env var and arm), `measurement.md` (how to judge a change),
    ~~`perf-cpu-plan.md` (the live performance plan)~~ and `perf-plan-overnight.md` (its
    executed predecessor).
  - Formats and tooling: `big-archive-format.md` (the cracked `.big` container, **plus
    two part-27 retractions: the name table is NOT fixed-width outside the shader
    banks, and 1,671 of 12,481 entries ARE compressed** — read it with
    `tools/big_list.py` and `tools/big_decompress.cpp`),
    `xtr-decoder.md` (the GPU stream format + the determinism method),
    `xenonrecomp-upstream-bugs.md` (local recompiler patches),
    `bootstrap-2026-08-04.md` (day 1), `xenia-capture-requests.md` (unfulfilled
    requests), `reusability.md` (what transfers to Case West), `phase5-3d-plan.md`
    (superseded, but its Step 0 instrument and Step 1 findings survive).
- `Xenia logs/` — captures land here (gitignored); keep an index in
  `Xenia logs/Xenia_Run_Content.md`, which **is** tracked.
- `~/DR2CZ-troubleshooting/` — **outside the repo on purpose**: operator screenshots
  (the only evidence channel for "does it look right", and the one no instrument here
  can replace) and headless `CZ_VK_FRAME_DUMP` frames. Its `INDEX.md` says what every
  shot showed, INCLUDING the ones that were lost — Spectacle deletes its temp directory
  when the window closes, so most of the first gameplay session's screenshots survive
  only as descriptions. Save straight into it.
- `runtime/` — the host runtime. Phases 1 and 3 complete; **phase 4's command
  processor is live too, ahead of the plan's ordering** — do not read the plan's phase
  numbers as the state of the code. There is a window, a present seam and real input;
  there is **no renderer** (phase 5), so the window is blank on purpose. Target is
  `cz_runtime`; the phase 0.2 link gate survives as `cz_runtime --smoke`.
  - `CMakeLists.txt` — **selects clang++ before `project()` (gotcha 31)**, and
    enables C for exactly one file (o1heap) so the .c source is not silently ignored.
  - `main.cpp` — image load → header publish → data-import resolution → guest entry,
    plus the `--smoke` gate.
  - `cpu/timebase.{h,cpp}` — the 49.875 MHz guest timebase, force-included over
    `ppc/` only (gotchas 1 and 32). `kernel/imports.cpp` shares `CZ_TIMEBASE_HZ` so
    `KeQueryPerformanceFrequency` and `mftb` cannot drift apart.
  - `cpu/guest_thread.{h,cpp}` — PCR/TLS/TEB block + guest stack. Both constants are
    from this XEX's header as A1 prints it: 64 TLS slots, 0x40000 stack.
  - `kernel/{memory,heap}.*` — the flat 4 GB map and the four arenas; the layout is
    checked against A1's own allocations, not inherited (`docs/phase1-notes.md` §3).
  - `kernel/{kobject,guestcall,klog}.*` — handles, the import marshalling seam, and
    the `[kcall]` trace the gate diffs.
  - `kernel/xex_imports.*` — publishes the XEX headers into guest memory and resolves
    the 244 IAT slots + 13 kernel variables.
  - `kernel/imports.cpp` — the kernel HLE, written in A1's call order.
  - `kernel/audio.{h,cpp}` — the XAudio render-driver client (a guest-thread pump
    calling the title's callback at 5.333 ms/frame, measured at 187.4-187.6/s against
    the 187.5 that 48 kHz needs), the XMA context array + its MMIO register file, and
    **the XMA DECODE WALK**. Finding 36; every structural claim is quoted from the
    guest function that states it, and the context layout is checked against the
    guest's own arithmetic rather than against Xenia's struct (`dw[8] - dw[7] = 6400
    = 25 blocks x 256`). **Read the physical-address note before touching a context
    pointer** — that one thing was the whole audio defect.
  - `audio/{xma_decoder,audio_out}.{h,cpp}` — phase A/V. ffmpeg's `AV_CODEC_ID_XMA2`
    (lifted from the Fable 2 port, so the licence is ours) and the SDL device.
    Deliberately dumb: the title's own mixer sums every voice into the one 5.1 frame
    it submits, so anything wrong with the SOUND is upstream of `audio_out.cpp` and
    `CZ_AUDIO_TRACE`'s peak is what tells the two apart.
  - `kernel/content.{h,cpp}` — the save-data layer: the content enumerators, the
    XAM enumerate message (app 0xFE, message 0x0002000E) and the mount that makes
    `save:` a host directory. Its header comment is the derivation of the whole
    protocol out of the title's own statically-linked `XamEnumerate` — read it before
    changing anything here, and lift it for Case West (gotchas 104-106).
  - `kernel/{vfs,file_imports}.*` — the file layer. In phase 1 rather than phase 2
    because A1's 22nd distinct kernel call is already an `NtCreateFile` (finding 16).
  - `kernel/import_stubs.cpp` — generated; honest-failure returns, not aborts.
  - `cpu/crash_report.cpp` — the guest state on any fault. Its host pc is the one
    field that is never stale (gotcha 57); `addr2line` it.
  - `cpu/guest_probe.cpp` — argument probes on named guest functions via the alias
    seam, behind `CZ_ARG_PROBE`. Kept as the worked example of tracing a bad value
    back to its producer; it is what closed finding 27.
  - `gpu/vk_renderer.{h,cpp}` + `gpu/xenos.h` — **phase 5: the renderer.** Its content
    guards fold on a four-worker pool as of part 53 — the first work this port moved off
    the pump thread — filed a frame ahead from the working set the pump last saw, with an
    inline fallback on every miss so correctness never depends on the prediction.
    `CZ_VK_NO_PARALLEL_GUARD=1` is the control arm. Inert
    unless `CZ_VKDRAW=1`. **As of part 54 it can also present through a real Vulkan
    swapchain** (`CZ_VK_SWAPCHAIN=1`), which is an ARM and not the default: it takes the
    frame −8.3% at 720p and −31.4% at 1440p, and it costs the host-rendered F4 overlay,
    because a window carrying `SDL_WINDOW_VULKAN` cannot also carry an `SDL_Renderer`.
    Nothing renders INTO a swapchain image — the finished frame is blitted in — so the
    renderer's dynamic rendering, pipeline formats and resolve chain are unaware it exists. `xenos.h` holds the register indices and format codes with
    each field layout written next to it, because every one of them is a magic number
    whose wrong value is silent. The header comment of `vk_renderer.cpp` transcribes
    the interface the translated shaders present (push constants, the five descriptor
    spaces, the shared-constants offsets) out of the generated HLSL — read that, not
    this, if the two ever disagree.
  - `host/host_paths.{h,cpp}` — **release A.1: where everything is.** One root, decided
    once and printed once, derived from the EXECUTABLE and never from the CWD. All three
    platform spellings are already written. Read its header before adding any path.
  - `host/first_run.{h,cpp}` — **release A.2: the honest refusal.** Package -> unpacked
    game -> shader cache; the first one missing says what, where, and which command
    produces it, then exits non-zero. `CZ_NO_FIRST_RUN_CHECK=1` is the off switch.
  - `host/window.{h,cpp}` — phase 3: the SDL window, the event loop, the present
    seam and the pad, deliberately in **one** module because in SDL they are one
    thread. Part 54 added the Vulkan swapchain seam — four functions the renderer thread
    calls, and the decision has to be made HERE, before the window exists, because
    `SDL_WINDOW_VULKAN` is a creation flag. Everything except `Host_Present` (called from the PM4 executor) and
    `Host_PadState` (called from whichever guest thread polls `XamInputGetState`)
    runs on the thread that created the window, which is the process's main thread —
    which is why `main.cpp` now runs the guest entry on a spawned thread
    (gotcha 99). Compiles to honest stubs without `CZ_HAVE_SDL`.
- Recompiler TOOL at `~/GithubRepo/XenonRecomp` (built at `build/`; carries local
  patches — see `docs/xenonrecomp-upstream-bugs.md`). Shader translator at
  `~/GithubRepo/XenosRecomp` (also patched; Case Zero inherits those fixes for free).

## Commands

Unpack the game (once). **As of part 85 the runtime does step 1 itself** — a boot whose
default xex is missing but whose `assets/package` holds the container extracts in-process
(`cz_runtime --extract-package <pkg> <dir>` runs it by hand; byte-identical to the Python,
which remains the reference and the SVOD fallback; `CZ_NO_STFS_EXTRACT=1` is the off
switch):
```
python3 tools/extract_stfs.py "assets/package/58410A8D/000D0000/<hash>" -o assets/game
./tools/build_xex_image_dump.sh
./tools/xex_image_dump assets/game/default.xex assets/game/default_image.bin
```

Regenerate the recompiled C++ (from repo root; `ppc/` must exist or XenonRecomp
segfaults in `fwrite`):
```
mkdir -p ppc && cd config && ~/GithubRepo/XenonRecomp/build/XenonRecomp/XenonRecomp \
    CaseZero.toml ~/GithubRepo/XenonRecomp/XenonUtils/ppc_context.h
```

Regenerate the switch tables — **use our scanner, not XenonAnalyse**:
```
python3 tools/find_jumptables.py assets/game/default_image.bin \
    -o config/CaseZero_switch_tables.toml
```

Repair function bounds after any switch-table change, then re-run the recompiler and
confirm the log has zero `jump outside function` lines:
```
python3 tools/fix_switch_function_bounds.py --apply
```

Check for silently dropped direct branches — **this is not optional after any change to
the function list**, and it is the only thing that catches the coverage oracle's
loop-header splits (gotcha 28). Regenerate `ppc/` between each step:
```
python3 tools/find_dropped_branches.py            # report both classes
python3 tools/find_dropped_branches.py --prune    # backward: remove spurious starts
python3 tools/find_dropped_branches.py --widen    # forward: widen truncated functions
```

Then check that every switch-shaped `bctr` was actually lowered — the gate for the
defect class that leaks a callee's non-volatiles into its caller (gotchas 53-55).
**Exit 1 = a real defect; run it last, and after any config change:**
```
python3 tools/find_unlowered_switches.py          # 0 defects expected
python3 tools/find_unlowered_switches.py --all    # also list the benign tail-call thunks
```

Build the SPIR-V shader cache. **`assets/shader_spv/` is gitignored, so a fresh clone
needs this before `CZ_VKDRAW=1` does anything — and AS OF PART 84 THE RUNTIME CAN DO IT
ITSELF**: a renderer boot with no cache builds the pixel half from the disc banks in 9 s
(`cz_runtime --build-shader-cache` runs it by hand) and first-sight translation supplies
the vertex half at run time, byte-identical to the pipeline below (which remains the dev
tool, and the authority for arm caches). `cz_runtime --translate-shaders <dumps> <out>`
+ `diff -r` against the cache is the standing identity gate between the two. Two sources, and they merge: the
captures' shaders (which reach gameplay, where our runtime cannot yet go) and our own
dump (which is the authority on the byte range, because the cache key is a hash of
it — gotcha 115). **Our dump run must go as DEEP as the runtime can go, not just to the
title screen** — the plain boot ends at the title and the prologue loads a shader
neither capture contains, which the renderer then declines to draw with (28,718 draws a
run, one line in the log and nothing else):
```
python3 tools/xenia_ucode_to_cache.py \
    "Xenia logs/A1_boot_title_fullgame/shaders" \
    "Xenia logs/A2_gameplay_stillcreek/shaders" /tmp/ucode      # 335 distinct
(cd runtime/build && CZ_NO_WINDOW=1 CZ_SHADER_DUMP=/tmp/ucode \
    CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A timeout 300 ./cz_runtime)  # +2
tools/build_shader_spv.sh /tmp/ucode assets/shader_spv          # 337, zero failures
```
The check that costs nothing, and the only thing that reports this at all — a shader the
cache lacks is one log line and a silent counter, not a failure:
```
grep -c "no translated shader" run.log         # must be 0
```
**AND THAT CHECK ONLY FIRES FOR A SHADER A RUN ACTUALLY BINDS.** Part 27 found
`ps_7d6044e7dcaea1f2` sitting in `~/DR2CZ-troubleshooting/ucode-dumps` and MISSING from
the built cache — microcode we have held for sessions and never translated. No run had
bound it, so the counter read 0 every time, and the module COUNT could not show it either
because the cache also carries `ps_926c15dd20571cf1`, whose microcode is lost: 410 dumps,
410 modules, different sets. **The gate is to rebuild from the dumps and compare the
NAMES, not the count** — it is free and it is two lines:
```
tools/build_shader_spv.sh ~/DR2CZ-troubleshooting/ucode-dumps /tmp/spv_check
diff <(ls /tmp/spv_check/*.spv | xargs -n1 basename) \
     <(ls assets/shader_spv/*.spv | xargs -n1 basename)   # only the lost-microcode entry may differ
```
And the gate on the sidecars themselves, which is **two-sided by construction** — the
per-slot texture dimension is derivable both from our ucode parse and from DXC's
`OpDecorate ... DescriptorSet` words, so a disagreement means one of the two decodes is
wrong. Run it after any cache rebuild; exit 1 is a real defect:
```
python3 tools/shader_dim_census.py             # 339 modules 2D, 100 cube, 0 disagreements
python3 tools/shader_dim_census.py assets/shader_spv_clip_a2m   # and every ARM cache
```
**And the RT occluder table is gated the same way, for the same reason.** A vertex
shader in the cache with no entry in `config/rt_world_xform.json` is a mesh the RT
collector silently DECLINES to place — the gotcha-390 shape one level up. Free, and it
is one line; exit 1 is a real defect:
```
python3 tools/rt_world_xform_census.py            # 104 of 104 covered, exit 0
```
It also names the sidecars carrying no `tfetchDims` at all — cache entries built before
part 25 whose microcode is gone. **That list is now EMPTY**: the last member,
`ps_926c15dd20571cf1`, had its microcode recovered in part 64's operator session and its
entry rebuilt in part 65.
**AND THE NAME-DIFF GATE APPLIES TO EVERY ARM CACHE, NOT JUST THE STOCK ONE**
(gotcha 390). Part 65 found `assets/shader_spv_clip_a2m` — the cache
`tools/play_session.sh` actually selects — holding 439 modules against the stock 449
since 2026-08-19, the ten ABSENT rather than stale, so every draw bound to one printed
`no translated shader` and was skipped in every operator session for three parts. All
six caches are now 449. **Keep ucode dumps in `~/DR2CZ-troubleshooting/ucode-dumps`,
not in `/tmp`**, which is a tmpfs: eleven entries were lost that way and two operator runs
(the military arrival, then Still Creek end to end) recovered TEN of the eleven. The last,
`ps_926c15dd20571cf1`, samples only sets 0 and 3 — an ordinary 2D shader, so nothing
depends on it. ~~A lost dump is a location nobody has replayed, not a permanent loss.~~
**And that last one is no longer lost**: part 64's operator session dumped it, and part
65 rebuilt its cache entry (which had been carrying no `tfetchDims` sidecar since
2026-08-15). Eleven of eleven recovered — a lost dump really is only a location nobody
has replayed.
**The cache is 449, and on 2026-08-21 the operator COMPLETED THE WHOLE GAME in one
sitting with `CZ_SHADER_DUMP` armed** — nine shaders surfaced across that run (three
mid-town, five late, one in the endgame), which is the closest this cache has ever
been to a completeness claim. The claim still has a shelf life (gotcha 13): eras no
run has entered — trial mode, other save states, error screens — can still hold
shaders nobody has counted. Growth trail: 335 from
the captures, 337 with our own dump, then 339, 353, 370, 371, 391, 394, 397, 402, 409, 411,
419, 424, 430, 435, 436, 440, 449 — 23 of those from two operator play sessions on 2026-08-08 alone, and
**11 more from part 39's operator evening, of which THREE were never reported missing by
any run and only the name-diff gate found them**, once the whole-frame black
stopped hiding the parts of the map nobody had visited. **Treat "the cache is complete"
as a claim with a shelf life** (gotcha 13): every era of this game that no run has
entered is a shader gap nobody has counted, and the counter is one log line.
A1 stops at the title screen,
A2 is gameplay, and the prologue and Still Creek each loaded a shader neither capture
nor our own dump contained. Any run that reaches new ground should carry
`CZ_SHADER_DUMP` so the blobs are captured for free — including an OPERATOR run, which
is the only way this port reaches most of the game.

**And if a run finds a missing shader without `CZ_SHADER_DUMP` set, the blobs are not
lost — recover them from the LIVE process.** `[imload] VS va=%08X hash=%016llx size=%u`
prints the guest address and the dword count, `runtime: guest memory at 0x...` prints
the host base, and the renderer's own hash is a self-check on the result:
```
gdb -p <pid> -batch -ex "dump binary memory vs_<hash>.ucode <base+va> <base+va+size*4>"
python3 -c "..."   # FNV-1a over the bytes must equal <hash>
tools/build_shader_spv.sh <dir> assets/shader_spv
```
Both of Still Creek's were recovered this way and both hashed EXACTLY, which is what
makes it a measurement rather than a hopeful memory read — a stale or reused buffer
would fail the hash rather than produce a plausible wrong shader. **Part 19 recovered
TWENTY this way from a single operator session, all twenty hashing exactly**, which
takes this from a trick that worked once to the standard recovery.

**Use `process_vm_readv`, not `gdb`, when the operator is still playing.** It reads
another process's memory without ptrace-stopping it, where a `gdb` attach freezes the
game for a second — which during a load test contaminates the very thing being tested.
Same permission model, no interruption; ~30 lines of `ctypes` and the FNV-1a check is
identical.

Look inside the game's `.big` archives — 146 of them, 12,481 entries:
```
python3 tools/big_list.py --all --find cc_          # search every archive by entry name
python3 tools/big_list.py <a.big> --extract <name> --out DIR
./tools/build_big_decompress.sh                     # once; links XenonRecomp's own LZX
tools/big_decompress <extracted> <out.bct>          # 1,671 entries are compressed
```
**`--extract` writes the STORED bytes**, which for a compressed entry is the compressed
stream and not the asset — it says so when that happens. `big_decompress` checks its own
output against an oracle (every loose `.bct` on disc opens `05 01 01 E2`) rather than
asking anyone to eyeball it. Both tools NAME what they could not parse instead of skipping
it: the first version of `big_list` silently dropped 95 of 146 archives, which would have
answered "is this asset here" with a confident no.

Build the runtime (needs `clang++`, **SDL2, Vulkan and ffmpeg**; ~90 s on 16 cores for a
cold image build). All three are required rather than optional-with-a-fallback, and for
one reason: **each of them, if it silently went missing, would present as a defect in the
game rather than as a build problem.** A lost window looks like input that stopped
working; a lost renderer looks like a black screen; a lost `libavcodec` looks like the
mute game this port shipped for 28 parts. Requiring them at CONFIGURE time is how those
stay build messages. It is safe to require all three because each is off at RUN time
unless asked for — `CZ_VKDRAW=1` for the renderer, `CZ_NO_AUDIO_OUT=1`/`CZ_NO_XMA_DECODE=1`
to switch audio back off — and `-DCZ_WINDOW=OFF` is how you say "headless on purpose" out
loud. On Fedora: `sudo dnf install SDL2-devel vulkan-loader-devel vulkan-headers
ffmpeg-devel`.
```
python3 tools/gen_import_stubs.py                 # after any change to the import set
cmake -S runtime -B runtime/build -G Ninja
cmake --build runtime/build -j$(nproc)
./runtime/build/cz_runtime --smoke                # the phase 0.2 link gate, still live
```

Reach live GAMEPLAY headlessly. Until part 16 this needed an operator, so every
gameplay claim was a report with no reproduction (gotcha 190). **START skips a
cinematic**, and the Zombrex tutorial's second page needs D-pad LEFT to open the watch
and B to leave it — without those two the run parks on the card forever:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,NONE \
  CZ_VK_FRAME_STATS=/tmp/gp.txt timeout 330 ./cz_runtime > /tmp/gp.log 2>&1)
tail -200 /tmp/gp.txt | awk '{print $5}' | sort -u | wc -l     # 200 = the camera moves
```
Arrives at ~185 s and reaches file **#184**, ~1,860 draws a frame. **Check the
camera-distinctness number before trusting anything measured off it**: every step is a
fixed 8 s interval against a boot whose depth in fixed wall time has always been a
distribution (gotcha 75), so the press counts will drift with load or frame rate. It
MANUFACTURES progress, so it is never a gate configuration (gotcha 78).

**REACH THE OUTDOOR WORLD AND A CROWD VIA THE TITLE'S OWN DEBUGJUMP SCREEN. This is the
recipe to use** — it is the operator's route, it is anchored to an EVENT rather than to a
wall clock, and it lands Chuck by the military camp in a full crowd at **7,431 draws** in
under half a minute of menu work:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE \
  CZ_VK_FRAME_STATS=/tmp/out.txt timeout 420 ./cz_runtime > /tmp/out.log 2>&1)
awk 'NR>1 && $2>m {m=$2} END {print m}' /tmp/out.txt      # >= 6,000 = it got there
grep -E "WAITJUMP|requested DebugJump" /tmp/out.log       # the four lines that prove it
```
`F2` opens the shipped DebugJump screen (needs `CZ_DEBUG_MENU=1`); `DOWN` once selects
`Case 0-2`, which spawns outside. **`WAITJUMP` is the load-bearing part**: the DebugJump
request is HELD until the frontend exists and lands whenever it lands (27 s here, 131 s on
another boot), so the barrier parks the sequence — repeating the preceding entry, which is
why `START` precedes it — until the screen is actually up, then starts the remaining
intervals from that moment. That is what makes this reproducible where a fixed-time recipe
is a fit to one afternoon (gotchas 75, 251).

**FULLY UNATTENDED, with the AI driving and the map handled** — this is the form to use
for texture coverage or any long roam:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_AUTOCHUCK=EXPLORER \
  CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE \
  CZ_SHADER_DUMP=~/DR2CZ-troubleshooting/ucode-dumps timeout 600 ./cz_runtime > /tmp/o.log 2>&1)
grep -E "WAITJUMP|EXPLORER engaged|changed the state away|pressing B" /tmp/o.log
```
Three things it handles that took a session to find, each measured rather than reasoned:
**the title's AI rewrites the state you set** (we wrote ITEM PICKER twice; a live read of
the running process found MISSION MASTER), so `CZ_AUTOCHUCK` re-asserts and counts the
overrides — 3 in an 8,657-frame run. **The AI opens the MAP by itself** about two minutes
into a roam and parks the run on it — not our input (a BACK-delivered counter reads 0, and
`CZ_FAKE_START_MS` bypasses the real pad entirely), so it is closed by hash
(`06903E1A`/`890DF3E5`, found with `CZ_SCREEN_TRACE=1`). **The close is B, not BACK** —
BACK is what opens the map. See `docs/instruments.md` for all four variables.

**Why this replaces the stick recipe below for anything comparative:** two arms of a
picture A/B are only comparable where their `drawFingerprint` and `cameraFingerprint`
agree, and with the stick recipe that filter yields 13-44 frames of ~300, every one under
1,800 draws — i.e. the outdoor era was never comparable at all (gotcha 247). Add
`AutoChuck` from the debug menu for coverage, or leave the character standing still, which
should hold the camera fingerprint matched across arms for long stretches.

The older stick recipe, kept because it walks a DIFFERENT route (out of the safehouse into
Still Creek, with a camera sweep) and because several recorded measurements were taken on
it — Chuck walks out of the safehouse into
Still Creek and the camera sweeps. It parks in the safehouse at
~1,900 draws, which is against the title's own two-vblank cap where a CPU saving
measures as exactly zero (`docs/perf-cpu-plan.md` item 0, which this closes). The extra
`B` is the safehouse door; a stick entry HOLDS for its whole interval where a button
taps for 150 ms, which is why walking needs `LSUP` and not `A`:
```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,LSUP,LSUP,B,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,LSUP,LSUP,LSUP,RSRIGHT,RSRIGHT,LSUP,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,LSUP,LSUP,RSLEFT,RSLEFT,RSLEFT,RSLEFT,LSUP,LSUP,RSRIGHT,RSRIGHT,RSRIGHT,RSRIGHT,NONE \
  CZ_VK_FRAME_STATS=/tmp/out.txt timeout 620 ./cz_runtime > /tmp/out.log 2>&1)
awk 'NR>1 && $2>m {m=$2} END {print m}' /tmp/out.txt      # >= 6,000 = it got there
```
Reaches the junkyard behind the safehouse by ~300 s at **6,400-8,100 draws a frame**,
past the operator's 6,592. Same warning as above and more so: it is 57 fixed 8 s steps
against a drifting boot, so **check the draw count before trusting anything measured off
it** rather than assuming the run went where the last one did.

Build and run on WINDOWS (`docs/windows-build-setup.md` is the full runbook; `ssh czwin`).
**git is the only way source moves between the machines** — commit here, push, pull there:
```
ssh czwin 'cd C:\cz\Dead_Rising_2_Case_Zero_Xenon_Recomp; git pull --ff-only'
C:\cz\vc.bat cmake --build C:\cz\Dead_Rising_2_Case_Zero_Xenon_Recomp\runtime\build
```
Everything there runs through `C:\cz\vc.bat` (the vcvars wrapper), and **which compiler is
per-target, not a preference**: XenonRecomp and the runtime need `clang-cl`, SDL2 needs
`cl`. A run launched over SSH lands in session 0 where its window is invisible — use the
`cz_play` scheduled task (`schtasks /run /tn cz_play`) to put it on the operator's desktop.

Build a RELEASE artifact (docs/release-plan.md; the gates are not optional — the first
bundle passed every static check and died on its first instruction, gotcha 485):
```
tools/build_ffmpeg_lgpl.sh             # once. LGPL, xma1+xma2 only. 120 deps -> 3
tools/build_sdl2.sh                    # once. REAL SDL2 — Fedora's is a shim that dlopens SDL3
cmake -S runtime -B runtime/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCZ_FFMPEG_PREFIX=$PWD/thirdparty/ffmpeg-lgpl -DCZ_SDL2_PREFIX=$PWD/thirdparty/sdl2
cmake --build runtime/build-release -j$(nproc)
tools/release_text_identity.sh         # .text identical = the build type is a null. NEEDS MATCHED
                                       # CONFIGURES (same prefixes + CZ_BUNDLE_RPATH, build type the
                                       # only delta) — against the everyday dev tree it fails for
                                       # config reasons and says so (part 85)
tools/release_package_linux.sh         # -> dist/CaseZeroRecomp + .tar.zst + generated THIRD_PARTY.md
                                       # + libdxcompiler.so + README.md + cz_defaults.env (part 85)
tools/release_gate_clean_container.sh  # podman, no dev packages. Must print GATE PASSED. As of part
                                       # 85 it REQUIRES the package + one ucode blob and runs the
                                       # whole first-run flow AND a DXC translation in-container
```
The Windows artifact (on czwin, through vc.bat; the gate RUNS the staged exe):
```
ssh czwin 'cmd /c "C:\cz\vc.bat powershell -ExecutionPolicy Bypass -File C:\cz\Dead_Rising_2_Case_Zero_Xenon_Recomp\tools\release_package_windows.ps1"'
```

Read the disc's own shader banks — **1,265 distinct PIXEL shaders, against the 345 we
accumulated in 25 parts** (release D.1). The gate is exact and free: every extracted blob
must FNV-1a to a name already in the built cache.
```
for b in "vs .vo" "ps .po"; do set -- $b
    python3 tools/big_list.py assets/game/data/shaders/deadrisingprologue-$1.big \
        --extract "$2" --out /tmp/discsh
done
python3 tools/vo_extract_microcode.py /tmp/discsh --gate --out /tmp/discuc   # 343 of 345
```
**VERTEX shaders are NOT recoverable this way and the plan's §1.4 is retracted on that
point** — the title patches their fetch instructions at load, so 0 of 104 appear verbatim.

Run the guest and gate it against hardware. **Both captures, always** — A1 is the
authority for the boot sequence, A5 for the synchronisation surface, and A5 is *not* a
superset of A1 (gotcha 45):
```
(cd runtime/build && ./cz_runtime > /tmp/run.log 2>&1)      # ^C or timeout; it parks
python3 tools/kernel_call_diff.py \
    --xenia "Xenia logs/A1_boot_title_fullgame/cz_run1.log" --ours /tmp/run.log
python3 tools/kernel_call_diff.py \
    --xenia "Xenia logs/A5_highfreq_boot/cz_run5.log" --ours /tmp/run.log \
    --include-high-frequency
```

Runtime instruments and measurement arms: **the full catalogue is
`docs/instruments.md`** — every environment variable this runtime reads, what it
measures, and for the ARMS what they are the control for. All are off by default and
free when off. Split out of this file on 2026-08-08.

The ones reached for most often:

```
CZ_NO_WINDOW=1     headless; the control arm for every phase 3 claim
CZ_VKDRAW=1        the renderer. OFF by default, so the same binary is the control arm
CZ_VK_PROFILE=N    the frame's CPU time by phase, every N seconds, plus a `pump` line
                   splitting the graphics pump's own ticks/sleep/walk/`pm4`, plus a
                   packet census (packets/frame, ns/packet, register dwords/frame).
                   START HERE for any performance question — and read the GPU's clock
                   before believing the `submit` column (gotcha 219). **Every phase is
                   EXCLUSIVE of the ones nested inside it as of part 20; it was not
                   before, and numbers from earlier sessions overstate `record` by the
                   whole of `streams` (gotcha 228)**
CZ_VK_FRAME_STATS=file   one line per presented frame; input to tools/frame_compare.py,
                   tools/frame_determinism.py (is a matched comparison possible at
                   all?) and tools/frame_era_medians.py (the outdoor A/B). **IT COSTS
                   ~3 ms A FRAME, 12-20%** — 921,600 pixels and a 2 MB bitmap zeroed per
                   PRESENTED frame — and it was on in every performance run this project
                   has recorded, so every absolute frame time here is inflated by it ON
                   TOP of the profiler's 2-4 ms (part 51, §6ch §6). A/Bs carrying it in
                   both arms are fine; say which instruments were on when quoting a time
CZ_PM4_TICK_US=N   how often the RING is walked, in microseconds. **Default 100 since
                   part 51**, where the 1 ms it replaced was never a measured period —
                   just the smallest the millisecond knob could say. The walk stops ~3.1
                   times a frame whatever the tick is, so this sets only how long each
                   stop lasts, and the Draw Thread spins on our read pointer throughout.
                   `CZ_PM4_TICK_MS=1` is the control arm
CZ_VK_FRAME_DUMP=dir     every 64th frame as a PPM — the picture, self-servable
CZ_VK_SNAP_DUMP=dir      EVERY resolve snapshot of one frame: which PASS went wrong
CZ_RING_TRACE=1    the ring, the brake's health, and the GPU/CPU hand-off chain counted
                   link by link. `truncated=0` is a standing gate
CZ_FILE_TRACE=1    every open/read, including the not-founds
CZ_WAIT_TRACE=1    name any infinite wait that outlasts 5 s, with guest callers
CZ_FAKE_PRESS_SEQ=...    synthetic input. MANUFACTURES PROGRESS — never a gate run
CZ_GUEST_LOG=1     the engine's OWN debug printf (640 callers). The sink — pair it
                   with CZ_GUEST_DIAG or it prints only the ungated errors
CZ_GUEST_DIAG=1    **the switch.** One byte, `0x829EC974`, read by 2,013 sites and
                   written by none, shipped as 1: a release KILL SWITCH, not the
                   "flag per category a shipped build left at zero" that gotcha 215
                   guessed and gotcha 266 corrects. Clearing it (plus `0x82AC3EAD` so
                   the un-silenced asserts print rather than trap) takes the outdoor
                   route from **0 `[guest]` lines of 11,168 to 1,239** — null and
                   positive control in one pair of runs. It is what makes the streaming
                   and zone-LOD decisions readable. A DIAGNOSTIC ARM ONLY: 2,013
                   formatting sites on the frame path, so never quote a frame time
                   from a run with it set (gotcha 7)
CZ_NO_XMA_DECODE=1 **the control arm for sound.** Contexts allocate, the pump runs,
                   nothing decodes — the runtime as it was for 28 parts. It is also the
                   arm that showed the prologue cinematic was waiting on audio
CZ_NO_AUDIO_OUT=1  the whole pipeline, no device. Separates "the guest produced audio"
                   from "we played it", and it is what a headless gate run wants
CZ_XMA_DECODE_LOG=1  per-context decode activity, and REFUSED split from short
CZ_SHADER_DUMP=dir put this on any run that might reach new ground, including an
                   operator run: a missing shader is one log line and a silent counter.
                   **NEVER point it under /tmp** — that is a tmpfs and it is why eleven
                   cache entries have no microcode left. Use
                   `~/DR2CZ-troubleshooting/ucode-dumps`
CZ_VK_NO_CUBE_SNAPSHOT=1  decline the cube map the TITLE RENDERS ITSELF to the white
                   dummy — the part-25 renderer in the part-26 binary, and the control arm
                   for the cube snapshot path (35.9% of cube fetches on the outdoor route)
CZ_VK_NO_CUBE=1    bind cube fetches the pre-part-25 way (into the 2D array, so the shader
                   samples the white dummy). The same-binary control arm for the cube-map
                   work, and the one to hand an operator for a side-by-side
CZ_VK_CUBE_POISON=1  the cube dummy becomes MAGENTA — the positive control that showed the
                   cube sample reaches the presented image (80 of 110 frames, up to 72% of
                   a frame's pixels). Read it with a per-pixel diff against the unpoisoned
                   run, NOT by looking for magenta: the sample TINTS, and the semantic
                   detector read 0.24% where the diff read 80 of 110 (gotcha 248)
CZ_VK_DIM_CENSUS=1 where the DIMENSION lives in a texture fetch constant, answered by
                   partitioning fetches on the shader's independent answer (gotcha 244)
CZ_VK_GPU_PASSES=1 **the frame's GPU time SPLIT BY REGION** (part 78) — passes bucketed by
                   draw count, resolve copies, resolve clears, the two barrier classes,
                   snapshot views, cube faces, the present blit and the present readback,
                   **with the RESIDUAL printed first** — plus, as of part 79, a **PASS EXTENT
                   CENSUS**: each pass class broken down by the largest scissor its draws
                   used, with microseconds PER PASS, which is what separates a full-screen
                   shader from pass overhead on a tiny target. It reproduces to 0.001
                   ms/frame across runs. The first GPU-side breakdown this
                   port has had, and its bill is nil. Read the residual before the classes
                   and the overflow line if it appears: a truncated frame reads LOW in every
                   class and looks like a saving
CZ_VK_SYNC_VALIDATION=1  **synchronization validation** — that a memory dependency actually
                   COVERS the accesses either side of it, which is a different question from
                   `CZ_VK_VALIDATION`'s "is this call legal" and the only gate for a barrier
                   change. STANDING GATE: 0 hazards. `CZ_VK_BARRIER_POISON=1` is its
                   positive control and must produce 30. Slow enough to change the route —
                   use `CZ_VK_RES=1280x720 PRESSMS=9000 SECS=45 TIMEOUT=420`
CZ_DEBUG_MENU=1    enables retained debug scaffolding; at the title menu press F2 to
                   open the shipped, operator-confirmed DebugJump testing screen; F4
                   opens the host-rendered Case Zero debug submenus (Left goes back)

```


`CZ_KCALL_WHO` is the companion to the phase gate: the gate says *that* our
first-occurrence order diverges, and the most informative divergences are imports we
call which hardware never calls at all. Only the call site explains those.

**The measurement and analysis recipes live in `docs/measurement.md`** — the renderer
A/B and its noise floor, the picture checks against capture E (`frame_signature.py`,
`frame_sharpness.py`), the guest disassembler `gdis.py`, the `.xtr` capture tools, the
bin-predication oracle and the two PM4 boundary oracles. **Run the PM4 oracles after any
command-processor change** (exit 1 = we would desync on a real stream), and never claim a
renderer improvement without a same-binary A/B aggregated over an era.

## The recompilation contract (identical to Fable 2 and Asura's Wrath)

- Every guest function → `PPC_FUNC_IMPL(__imp__sub_XXXXXXXX)` taking
  `(PPCContext& ctx, uint8_t* base)`.
- Guest 32-bit addresses index into `base`; `PPC_LOAD/STORE_*` swap endianness.
- Hooks: define a strong `PPC_FUNC(sub_X)` calling `__imp__sub_X(ctx, base)` pre/post.

## Game intel (established 2026-08-04)

- **Package**: XContent `LIVE`, content type `0x000D0000` (Arcade Title), STFS volume,
  256 files, 825 MB. Display name `DEAD RISING 2: CASE ZERO`. Title ID `58410A8D`.
- **XEX**: image base `0x82000000`, entry `0x825D9F30`, image size `0xB40000`,
  `.text 0x82150000 + 0x873564`. Encryption 1 (**devkit key**), compression 2 (**LZX**).
  244 imports.
- **Engine**: Blue Castle Games' in-house engine, shared with the full Dead Rising 2 —
  the image still carries DR2's zone names (`americana`, `atlantica`, `arena_stadium`,
  `boss_battle_*`) though Case Zero ships only the Still Creek content. This is why Case
  West should be cheap after this.
- **Middleware**: Havok physics (`hkp*`/`hkx*` RTTI), XMA audio, an in-house
  "CrowdEngine" for the zombie crowds. ~~Bink video.~~ **Retracted (finding 7)** — the
  image contains the strings `Bink_1`/`Bink_2`, which is what the day-1 inference was
  built on, but there is no Bink decoder in use and no `.bik` file in the package. The
  strings are a dead or renamed path. Middleware named in an image is evidence that a
  name exists, not that a codec runs.
- **Assets**: `.big` archive containers throughout, `.bct` textures, `.bcf` fonts. At
  least one path is constructed at runtime (`anm_%s.big`), so the VFS must handle
  arbitrary paths rather than a fixed manifest. Format not yet cracked; Fable 2's `.bnk`
  work is the closest model.
- **Shaders ship loose on disc**:
  `data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big`. **THEY DO HOLD RAW XENOS
  MICROCODE — the day-1 hypothesis was RIGHT and finding 6's retraction of it is itself
  RETRACTED, 2026-08-27.** `tools/vo_microcode_probe.py` over the 1,571 objects in the
  three prologue banks, against the 449-blob ucode oracle: **PS 335 of 335 verbatim,
  VS 81 of 103 verbatim + 16 more matching by tail (a patched head), 432 of 438 = 98.6%
  recoverable.** The microcode is a SUB-RANGE of each `.vo`/`.po` object, starting at one
  of 163 distinct offsets of which **86 are not 8-byte aligned** — and finding 6's test
  compared ALIGNED 8-byte n-grams against whole payloads, so it could not have matched
  more than half the population whatever was there (gotcha 25's shape; the retraction and
  the artifact are in `xenia-capture-analysis.md` §6). **The disc holds 1,571 shaders
  against the 449 we accumulated over 25 parts and eleven operator sessions**, so this is
  what lets a release build its own cache on first run and retires "the cache is complete"
  as a claim with a shelf life (`docs/release-plan.md` §4). **What is still unknown is
  where inside an object the microcode BEGINS as a rule** — the offset is a plain BE u32 in
  the header for only 34 of 416 — and that task has a free gate: 416 known
  (object, offset, length) triples and a hash every extracted blob must match.
  ~~Retracted (finding 6): they are
  `.big` archives of `<hash>.vo` shader *objects* carrying build metadata (including
  `.updb` debug paths), and their payloads share only background-noise n-gram overlap
  with the microcode the guest actually submits.~~ The renderer input **historically** came
  from
  Xenia's `dump_shaders`: 455 microcode blob files = **335 distinct shaders** (A1's
  120 are a strict subset of A2's 335), all translated in phase 5.
- **No Bink** (finding 7). Grep `.big`, never `.bik`. **And "movie" is the wrong word:
  part 27 read the archives and a cinematic is an IN-ENGINE SCRIPTED SCENE** — 29 `.txt`
  scripts in `data/cinematics/cinematics.big` naming a camera animation, actor animations,
  particles, a HUD event and one audio event, with the animation data in
  `data/anim/cinematic/*.big`. There is no codec to write. `docs/phase-av-plan.md`.

## Ground truth in hand (round 1, delivered 2026-08-04 — COMPLETE)

**START HERE: `docs/xenia-capture-analysis.md`** — the numbered findings ledger. It is the
authority on measured numbers; where any other doc disagrees with it, it wins.

Index of what each capture is: `Xenia logs/Xenia_Run_Content.md` (written by the
operator). All runs are the **full game** (`license_mask = 1`) on the instrumented
canary fork, STFS package launched directly.

- **A1** boot→title at L3 (13.9 MB) + the Section D shader dump · **A2** gameplay
  (606 MB) + gameplay shaders · **A3** save round-trip + the physical save file ·
  **A4** 5-min title idle · **A5** A1's drive with high-frequency logging (231 MB).
- **B1/B1b** GPU `.xtr` boot→title + determinism repeat · **B2** GPU gameplay
  (**7.95 GiB** — the operator fixed Xenia's 2 GiB `.xtr` cliff at source to get it).
  Each with a same-run L3 correlation log.
- **C1/C2** function coverage: 12,278 boot→title, 17,118 gameplay, +4,840 gameplay-only.
- **E** five screenshots as the visual target.

**THERE IS NO OUTSTANDING CAPTURE REQUEST.** The only candidate is an optional A2b
(gameplay-era `.big` seek order), and finding 8 explains why it is probably unnecessary.

Highlights that change how we work:
- **The trial trap fired.** `license_mask` defaults to 0 → the game boots its **trial**,
  whose boot differs measurably (`chuckwalkietalkie.big` 1,164× vs 2×). Finding 1.
- **`NtReadFile` is `kHighFrequency`** — invisible at plain L3. **A5 is the `.big` read
  oracle**, not A1 or A2. Finding 2.
- **The `.big` container format is cracked** — `docs/big-archive-format.md`. Should
  transfer verbatim to Case West.
- **There is no Bink in this game.** Movies stream through an in-house player reading
  `.big` cinematic archives. Finding 7; the Bink phase is deleted from the runtime plan.
- **The disc shader banks are NOT usable microcode** (retraction, finding 6) — but
  Xenia's `dump_shaders` gave us 455 microcode blob files — **335 distinct** shaders,
  A1's 120 being a strict subset of A2's — which is XenosRecomp's input, so the
  renderer was unblocked anyway.

## Current status

**The full per-session trail is `docs/port-history.md`; the open backlog is
`docs/open-items.md`.** Both were split out of this file on 2026-08-08. The
authoritative per-subject records are `docs/xenia-capture-analysis.md` (the numbered
findings ledger — it wins on any measured number), `docs/phase1-notes.md`,
`docs/phase3-notes.md`, `docs/phase5-notes.md` and `docs/d3d-translation-plan.md`.

**SUBJECT: THE RELEASE, 2026-08-27, operator instruction opening part 82:** *"Do the release
plan."* **`docs/release-plan.md` IS THE PROGRAMME AND `docs/part86-kickoff.md` IS THE LIVE
HAND-OFF.** **MILESTONES A, B, D AND E ARE COMPLETE — both release artifacts exist and are
gated** (`dist/CaseZeroRecomp-linux-x86_64.tar.zst` 26 MB, `CaseZeroRecomp-windows-x86_64.zip`
21 MB; `release-plan.md` §9.8 has every gate). A player's first run is real end to end:
in-process STFS extract, disc shader build, a progress window over both, and
`cz_defaults.env` turning the renderer on without touching any dev arm. What remains is
§9.8's owed list — the Windows CI leg's first live run, the pre-warm key file (needs one
operator playthrough), the glibc floor / AppImage, and macOS (milestone C, hardware).

**SUBJECT CHANGE, 2026-08-27, operator instruction closing part 81:** *"Update your memory
and all we'll switch to something else then performance."* **PERFORMANCE IS PARKED**, having
been the live subject since part 71. `docs/perf-plan-part81.md` is the reference that resumes
it and `docs/part82-kickoff.md` §2 is what is owed. Two changes from part 81 are live and ON
BY DEFAULT with their price unmeasured — see the part-81 status block below.

**SUBJECT CHANGE, 2026-08-23, operator instruction closing part 70:** *"We'll stop for
now with trying to get ray tracing running. Disable that we can select it in game. We'll
now switch to fixing performance issue."* **RT shadows are PARKED, NOT DELETED** — the
settings panel no longer offers the three RT rungs and a persisted `rt_shadows=N` no
longer engages the feature; `CZ_VK_RT_MENU=1` restores the rows and `CZ_VK_RT_SHADOWS=N`
still engages it directly. Both arms print the line that proves which one is running. The
feature's whole state is `open-items.md` 0v and `docs/part71-kickoff.md`. ~~**THE LIVE PLAN
IS `docs/perf-plan-part71.md`.**~~ ~~**THE LIVE PLAN IS `docs/perf-plan-part72.md`**~~
~~**THE LIVE PLAN IS `docs/perf-plan-autonomous.md`**~~ ~~**THERE IS NO LIVE PLAN AS OF
PART 73**~~ — that one is EXHAUSTED, every item in it closed, refuted or shipped, and part 80
built its successor from new ground rather than from its table: **THE LIVE PLAN IS
`docs/perf-plan-part81.md`**, and its §1.0 is a census that must run before any code. ~~`part75-kickoff.md`
is the hand-off~~ ~~**`part76-kickoff.md` is**~~ ~~**`part77-kickoff.md` is**~~ ~~**`part78-kickoff.md` is**~~ ~~**`part79-kickoff.md` is**~~ ~~**`part80-kickoff.md` is**~~ —
~~**`part81-kickoff.md` is**~~ — **`part82-kickoff.md` is**, and it says both where that
ground is and that PERFORMANCE IS PARKED. (This
line has now named the wrong plan TWICE — the two-live-pointers defect the block-rotation note at the bottom of this file
describes, and the reason that note asks for the rule and not just the name; gotcha 13.)

Where the port is, as of 2026-08-28 (**PART 85 CLOSED — THE RELEASE. MILESTONE E IS
COMPLETE: BOTH RELEASE ARTIFACTS EXIST AND EVERY §5 GATE RAN AGAINST THEM.**
**`docs/part86-kickoff.md` IS THE LIVE HAND-OFF**; records: `release-plan.md` **§9.8**):

* **THE §2.3 FIRST-RUN FLOW IS REAL, END TO END, THREE WAYS.** `host/stfs_extract.{h,cpp}`
  unpacks the player's package in-process — byte-identical to `tools/extract_stfs.py` over
  the real package (256 of 256 files), bounds- and traversal-checked because the package is
  player-supplied input, `default.xex` written LAST so an interrupted extract cannot
  counterfeit a complete tree. A fake root holding ONLY the package boots to live gameplay
  (extract → 1265-shader disc prebuild → 32 first-sight JITs, `no translated shader` = 0);
  the same flow runs inside the clean container; and a windowed run was screenshotted
  mid-flow showing the new **first-run progress window** (plain SDL, before
  Host_WindowInit, calling-thread-only callbacks). `--extract-package` is the gate verb,
  `CZ_NO_STFS_EXTRACT=1` the off switch.
* **TWO PACKAGING HOLES ONLY RUNNING THINGS COULD CATCH.** `libdxcompiler.so` was NOT in
  the bundle — every static check passed while a shipped build could not have translated a
  shader (a dlopen is invisible to ldd, the sdl2-compat shape again); the container gate
  now RUNS a translation inside the container with HOME unset. And a player
  double-clicking got the deliberately blank window (CZ_VKDRAW is opt-in everywhere) —
  `cz_defaults.env` beside the executable now applies release defaults ONLY where the
  environment is unset, printed per line, data-not-code (`.text` identity re-verified).
* **THE WINDOWS ARTIFACT EXISTS** (`tools/release_package_windows.ps1`): DLLs beside the
  exe (the search path Windows actually has), MSVC runtime from the toolchain's redist,
  gate = RUN the staged exe (`--smoke`, then all 1,265 disc shaders with the `[shxlate]`
  line naming the STAGED dll). **DXC's licence corrected in place: NCSA, not Apache**
  (§4); text vendored and shipped on both platforms.
* **E.1 CI EXISTS AND SAYS WHAT ITS TICK MEANS**: host sources + `--smoke` on a STUB image
  (`tools/gen_stub_ppc.py` — two scanner traps: no word boundary inside `__imp__sub_`,
  X-macro addresses as bare hex), upstream recompiler clones at pinned bases +
  `tools/ci/*.patch` (37 local commits; `regen_patches.sh`). Linux leg verified locally
  end to end BEFORE the YAML, green on its first real run; the Windows leg's first vcpkg
  run was still in progress at close. macOS absent on purpose until C.
* **§5 CHECKLIST, THIS ARTIFACT STATE**: container gate PASSED (now including the
  in-container first-run flow); `.text` identity on MATCHED configures (dev-vs-release
  compares different headers and a relocated image — the script says so); validation
  exactly the standing 6 `topology-08773` at 7,676 draws; bind-batch verify 0 of 87.4 M;
  both censuses clean; disc gate 343 of 345.
* **A stray `cz_runtime` from a dead session had filled /tmp (22 GB draw-trace log) and
  killed this session's shell mid-part** — bare `echo` exit 1 is the tmpfs-quota symptom;
  `du -sh /tmp/*`, `fuser` the big file, kill by pid.
* **OWED (release-plan §9.8):** the Windows CI leg's outcome; the pre-warm key file (one
  operator playthrough); the glibc floor / AppImage; an operator sitting on the shipped
  bundle; milestone C.

Where the port is, as of 2026-08-28 (**PART 84 CLOSED — THE RELEASE. MILESTONE D IS
COMPLETE: A SHIPPED BUILD TRANSLATES ITS OWN SHADERS.** D.2, D.4 and D.3 all landed in one
part, in that order, each gated before its commit. ~~**`docs/part85-kickoff.md` IS THE LIVE
HAND-OFF**~~ — it was, for one part; it is `part86-kickoff.md`. Records: `release-plan.md`
**§9.7**; lessons: gotchas **501-503**):

* **D.2 — IN-PROCESS TRANSLATION.** XenosRecomp's own recompiler (MIT, sibling checkout)
  compiled into `cz_runtime`, C++ ports of the synth/census Python, a JSON writer matching
  `json.dump(indent=1)` byte for byte, DXC through its dlopen'd C API. **The gate: 449 of
  449 dumps, all 898 files BYTE-IDENTICAL to the shell pipeline's output, 2.6 s vs 51 s.**
  The design was probed first: DXC's API and CLI produce identical SPIR-V. Positive
  control: an implementation poison (census drops one register) fires the gate; a blind
  input-bit poison was semantically DEAD and proved nothing (gotcha 501).
* **D.4 — TRANSLATE ON FIRST SIGHT.** A missing hash is translated on one worker,
  persisted, registered live — enqueue and drain both on the pump thread, so the tables
  are never touched off-thread. **Empty-vertex-half gate: crowd at 8,110 draws,
  `no translated shader` = 0, 45 shaders at 18-70 ms (median 23), every persisted module
  byte-identical to the canonical entry.** The in-flight skip is its own counter so the
  standing grep keeps meaning "ended up missing". Off-arm restores the old behaviour
  (29 missing, 0 translated); the null (full cache) is 0/0 — the standard path never sees
  the feature. `CZ_VK_NO_SHADER_JIT=1`.
* **D.3 — THE FIRST-RUN DISC PASS.** The `.big` index (LE) + D.1's container rule, every
  bound checked so a malformed player-supplied object is skipped BY NAME. **1,265 of 1,265
  distinct pixel shaders, 0 refused, 0 failed, 9.0 s** — resumable (killed at 433, resumed
  832, no-op third run), automatic on a renderer boot via marker files that keep it away
  from populated dev caches (898 files before and after a dev boot). **Crown gate: disc
  cache only → crowd, 0 missing — and the pixel first-sight list is EXACTLY the two hashes
  D.1 enumerated as absent from the disc.**
* **A SHADER CACHE IS ONE ARTIFACT, NOT ONE PER OS.** Windows (dxcompiler.dll, clang-cl)
  produces byte-identical output to Linux for all 348 dump-built and all 1,265 disc-built
  modules.
* **THREE WINDOWS PORTABILITY DEFECTS**, all in one TU's include preamble: lean windows.h
  excludes the COM types dxcapi needs; `win_compat.h`'s `#undef far` leaves `FAR` a stray
  identifier in every COM prototype; `E_FAIL` is #undef'd for guest code. Gotcha 503 — the
  fix for one collision class is itself a collision for later includes, and Case West will
  reuse the same file.
* **A FALSE CLAIM, CORRECTED IN-SESSION (gotcha 502):** two "Windows builds D.4"
  statements were made against a STALE tree — a silenced chained `git pull` had failed,
  the empty error grep read as success, and `--smoke` passed by exercising the previous
  binary. Verify the pulled HEAD, not the absence of error text.
* **OWED (release-plan §9.7):** the graphical progress screen (console lines until E gives
  the first run a window); the in-process STFS extract; the shipped pre-warm key file; and
  the variant arm caches do not gain first-sight entries (dev-only, known).
* **Gates:** everything part 83 inherited, plus the D.2 byte-identity diff, the disc-pass
  343-overlap identity, the empty-vertex crown gate, and the prebuild's
  `N translated, 0 failed` + done-marker discipline.

**Older per-part status blocks (parts 28-54, the superseded mid-part-44 closure and the
superseded MID-PART-46 block) moved to `docs/port-history.md`, NOW INCLUDING PARTS 60-83's** — part 85 moved part 83's out in the same commit that added its own block, part 84 moved part 82's out in the same commit that added its own block, part 83 moved part 81's out in the same commit that added its own block, part 82 moved part 80's out in the same commit that added its own block, part 78 moved part 76's out in the same commit that added its own block, part 76 moved part 74's out in the same commit that added its own block, part 74 moved part 72's out in the same commit that added its own block, part 73 moved part 71's out in the same commit that added its own block, part 72 moved part 70's out in the same commit that added its own block, part 71 moved part 69's out in the same commit that added its own block, part 70 moved part 68's out in the same commit that added its own block, part 69 moved part 67's out in the same commit that added its own block, part 68 moved part 66's out in the same commit that added its own block, part 67 moved part 65's out the same way, part 65 moved part 63's out the same way, part 64 moved parts 61/62's out the same way, part 63 moved part 60's out the same way, part 61 moved part 59's out the same way, part 59 moved part 57's out the same way, part 57 moved part 55's out the same way, part 55 moved part 53's
out in the same commit that added its own, which is what the rule below asks for. — CLAUDE.md keeps only the
live part and one part back, per the 2026-08-08 split's rule, and **part 53 moved part
51's out in the same commit that added its own**, which is what the rule below asks for.
**Part 51 had to move four at once**, because parts 47-50 each added a block without
retiring one and nobody noticed: this file is loaded into every session whole, and the cost is not its size, it is
that a reader cannot tell which block is current. Same defect as a stale LIVE pointer
(gotcha 13). If you are adding a block, move one out in the same commit — **and put the
retained block's kickoff pointer into the PAST TENSE while you are there.** Part 66 found
this file asserting "is the LIVE hand-off" twice, once in the live block and once in the
part-back block that had been true a day earlier. Two live pointers misroute a reader
exactly as reliably as one stale pointer does.

* **The recompilation is clean and has been since phase 0**: 57,808 functions, 228 TUs,
  zero unrecognized instructions, zero dropped branches, zero unlowered switch
  dispatches, silent recompiler.
* **The game boots, renders and PLAYS.** A headless run reaches the title screen, the
  menus, the prologue and live gameplay with no operator (the recipe is in Commands).
  The operator has played Still Creek: combo weapons, the Zombrex tutorial, cinematics.
* **Gates, both arms:** `--smoke` OK; A5 **exit 0, 3 permutation windows, 0 real**;
  `truncated=0`; `no translated shader` = 0; deepest file on a no-input boot
  **#83 `cinezombie.big`**;
  both PM4 capture oracles clean; the picture matches capture E2 at **+0.9597**
  correlation, identity orientation. **A1's strict-prefix gate is BIMODAL** — the benign
  position-71 three-name interleave of gotcha 221 shows on some runs and not others, and
  `CZ_PM4_TICK_MS=16 CZ_VBLANK_TICKCOUNT=1` reduces it rather than removing it (part 18
  said "restores" on the strength of one run). Quote A5.
* ~~**Performance: ordinary gameplay is 31 fps and CLOSED** — that is the title's own
  two-vblank pacing and it will not go higher.~~ **RETRACTED IN PART 49.** It was true
  of the shipped CONFIGURATION and false of the title. The 30 fps is the title's own
  D3D present interval, traced end to end from its config global to its swap
  scheduler, and **a 60 fps mode is a configuration the game already ships with**
  (`CZ_FPS_CAP=60`). The operator has played the whole map on it: 62.5 fps below 3,000
  draws, 43.5 at 3-5k, 35.7 at 5-7k, and only 3.6% of 16,788 frames below 30 fps. The
  rest of this bullet stands. **Crowds are the open item and are
  CPU-bound in our runtime**: 75% of a crowd frame is the renderer's draw path and the
  PM4 walk. `docs/perf-cpu-plan.md` is the plan; item 0 is closed (the headless recipe
  reaches the outdoor world at 6,400-8,700 draws a frame) and **part 20 re-measured the
  rest, because the profiler was counting nested phases twice and the plan's ranking of
  §1 was built on the result** (gotcha 228). Corrected, at ~6,800 draws: draw path
  19.9 ms (`record` 6.7, `other` 5.6, `streams` 3.7, `textures` 2.7, `constants` 1.3),
  PM4 walk 11.8 ms. Taking the instrumentation off the per-draw path is **−11.0% of a
  crowd frame**, three runs an arm with no overlap. **The PM4 walk is a register-write
  loop — 90,316 packets a frame carrying 815,020 register dwords at 15.3 ns each** — and
  it is the biggest untouched term.
  **The stream cache is CLOSED as of part 22** — the cross-frame store (§6av) takes
  `streams` from 11.1% of a crowd frame to **0.0%**, copied bytes from 61-66 MB/frame to
  0.23, for a net 3.3-4.0 ms after its content guard.
  **The noise floor here is 10-13% at one run a side** (gotcha 229): use
  `tools/frame_perf_bins.py`, three runs an arm, alternated, and run the null comparison
  first. A real A/B on this workload is an hour of wall time.
  **AND `frame_perf_bins.py` REPORTS MEANS, WHICH ON THIS TITLE MEASURE THE PACING FLOOR
  RATHER THAN YOUR CHANGE (gotcha 237).** It scored the store at +1.7% against a +1.3%
  null; read as medians the same data is **44 ms -> 32 ms at ~3,700 draws**, and the
  decisive statistic is neither — it is the share of frames within 1 ms of a 16 ms
  multiple, which goes **10% -> 97%**. A CPU saving converts to frame rate only where the
  frame is above one vblank floor and within reach of the next, so quote the pinned share.
  **DO NOT PIN THE GPU CLOCK — gotcha 219 is retracted in part.** The P8/210 MHz this
  project quoted for five sessions was an overnight session with the MONITOR ASLEEP.
  Awake, this workload governs itself to **P5, mean 524 MHz, 32% utilisation, 28.6 W**,
  which is where `vkcube` sits on the same machine. Sample and quote with
  `tools/gpu_clock_sample.py`. **The 32% is the real finding**: the GPU is idle 68% of
  every frame because `SubmitAndWait` blocks straight after submitting, so our CPU and
  GPU never overlap. Overlapping them is ~22 -> 36 fps at the SAME power and is now the
  largest item in the plan (gotcha 231, `docs/phase5-notes.md` §6ar).
* **The view-dependent whole-frame black is SOLVED** and was the renderer's per-frame
  bump arena overflowing at a fixed 128 MB against a 161 MB peak, which lost the whole
  post chain and presented black over a correctly rendered scene. 160 black frames of
  8,216 at 128 MB, zero at 512, and every one of the 160 the frame after an exhaustion.
  It grows now. `docs/phase5-notes.md` §6ap.
* **SAVE AND LOAD BOTH WORK — a closed round trip, and the first title state in this
  port that survives a process exit** (`docs/phase3-notes.md` finding 52). The file is
  303,104 bytes with bytes 4..31 identical to the real 360 save A3 shipped. Three
  defects, each of which alone was enough: `NtCreateFile` honoured no disposition,
  `NtWriteFile` was a stub, and the VFS cached negative lookups so a created file could
  never be re-opened. The LOAD needed a fourth — xam ordinal `0x271`,
  `XamContentCreateInternal`, which `kResolvable` refused because A1's list of resolves
  was captured with an empty save root.
* **CUBE MAPS ARE BOUND AS OF PART 25 — and that is about half the item by volume.**
  Found in part 23, built in part 25. 92 of the cache's 397 shaders sample set 2
  (`TextureCube[]`) and every one of them read descriptor index 0, the 1x1 white dummy, on
  every draw from phase 5 until now. The sidecar now carries each fetch slot's dimension,
  cube maps upload as six faces into a `VK_IMAGE_VIEW_TYPE_CUBE` view in set 2, and
  `CZ_VK_NO_CUBE=1` is the same-binary control arm. **The fetch constant's own dimension
  field was located by CENSUS (`CZ_VK_DIM_CENSUS=1`), not from memory, which had it one
  field off** — dword5 bits 9..10, cross-checked against dword2's stack depth reading 5 for
  every cube fetch (gotcha 244). `docs/open-items.md` item 00 and `phase5-notes.md` §6ay;
  three competing theories died in part 23's census and are recorded there.
  **THE SECOND HALF LANDED IN PART 26: the cube map the title RENDERS ITSELF is now
  assembled from its six resolve snapshots** — `06805000`, six faces at
  `base + i * 0x4000`, copied into a six-layer `VK_IMAGE_VIEW_TYPE_CUBE` image in set 2 and
  refreshed by each face's own resolve (8,850 face refreshes in 240 s, because the title
  re-renders it continuously — a one-shot fill would freeze the world's reflection).
  **The face layout was printed face by face and could have refuted the stride model; six
  of six filled.** `358,767 of 999,508 cube fetches (35.9%)` on the outdoor route now read
  it where all of them read the white dummy before. `CZ_VK_NO_CUBE_SNAPSHOT=1` is the arm.
  **What is still owed is the operator's verdict**, and it is now a three-way question
  (rendered cube / white / no cube at all). Know the headless answer first: part 25's
  four-config block put the magenta positive control at **12.7x its null**, so the
  instrument is NOT blind and binding real cube maps changes nothing measurable in the
  safehouse and prologue — both surviving explanations put the effect OUTDOORS.
* **A CLASS OF WHITE-SURFACE DEFECTS IS OPEN, IT IS OURS, AND SEVEN EXPLANATIONS ARE
  ALREADY REFUTED** (part 26, `docs/open-items.md` 00f/00g). White ground patches, white
  props, blown-out glass and windows — **Xenia renders all of them correctly.** Refuted by
  measurement: the tone map, a missing texture, constant UVs, the white dummy (all four
  heaps poisoned magenta), the clear colour, the EDRAM surface format, a flat-decoding
  texture. **Do not re-buy any of them.**
  Round-2 captures (`Xenia logs/R2_world/`, seven self-contained single-frame traces, read
  with `tools/xtr_draw_bindings.py`) then established: our shader coverage is complete
  (357/357); the ground draw matches hardware on shader, textures, texture CONTENTS and
  render state; and our cube declines fire on a shader-versus-constant disagreement
  hardware never shows.
  **PART 27 CLOSED THE LAST INPUT AND THE ANSWER IS THAT THE GROUND DEFECT IS IN THE
  SHADING.** `tools/xtr_draw_vertices.py` reads hardware's own vertex streams and shader
  constants for a named draw; the 25,234-vertex ground draw agrees on **all five vertex
  attributes to the printed digit** and on every constant the capture can reconstruct. The
  recorded "two texcoords decoding identically" anomaly **is what hardware does too**, and
  item 00f's "drawn twice with mask=F" lead was **TILING** — the two draws' scissors are
  `0,0 640x720` and `640,0 640x720` (gotcha 265). Read our translated
  `ps_ad65b98593f95926` against the capture's disassembly of it next.
  **On the cube declines, part 27 kept the conclusion and replaced the measurement**:
  "414 of 414" counted only slots already reading cube, so it could not have found a
  disagreement (gotcha 264) — `tools/xtr_cube_agreement.py` asks it per declared fetch slot
  and gets 0 of 13,203. The disagreement is ours, it is **9 enumerated (shader, slot,
  texture) cases**, and at those draws our slot 4 holds an **exact duplicate of slot 3**
  where the captures show hardware holding a real 128x128 DXT1 cube map for the same shader
  pair. **The magnitude was off by 10x and had a second, larger cause**: on the outdoor
  route the dummy is served to 3,210 of 1,903,592 cube fetches, **2,182 because the guest
  never set that slot at all** and 1,028 for the disagreement.
  **AND THE `.xtr` TOOLS WERE MISSING `LOAD_ALU_CONSTANT`** — 620 packets against 36
  `SET_CONSTANT`s in one capture, so every constant they reported for hardware was a zero
  that meant nothing (gotcha 262), and 81 of those 620 read memory the trace does not
  carry, which the tools now report as `UNRECOVERABLE` rather than as a stale value
  (gotcha 263).
* **OUTDOOR PICTURE CLAIMS ARE POSSIBLE AS ERA AGGREGATES AND IMPOSSIBLE AS MATCHED
  FRAMES — measured in part 26, and the route was never the problem.** Two arms are
  comparable only where `drawFingerprint` AND `cameraFingerprint` agree; run that filter on
  two runs of ONE configuration on the DebugJump route and it yields **422 of 13,056 frames,
  none above 141 draws, and 0 of the 12,174 outdoor frames** (`tools/frame_determinism.py`).
  The route is fine — 93% of its frames are outdoors and two runs' draw counts agree to
  1.4% — but a crowd of animated actors never renders the same draw list twice, so exact
  equality selects for stasis (gotcha 254). **The replacement is
  `tools/frame_era_medians.py`**: era medians over every frame above 1,800 draws, with the
  null measured from the same pair — **0.94% on mean luma, 0.76% on distinct colours**,
  while coverage saturates at 99.67% and can report nothing. That is what unblocks items
  00, 3, 4 and 6 (`docs/measurement.md`).
* **Ordinary gameplay is ~30 fps and the CPU/GPU now OVERLAP** (part 23): the fence wait
  fell from 31.5% of a crowd frame to 0.2% with `CZ_VK_FRAMES_IN_FLIGHT=2` (default; `=1`
  is the old renderer, same binary). The binned frame-time A/B is still owed.
* **THE HUD / AMMO DEFECT IS FIXED** (part 24, open item 00c). It was our own
  cross-frame stream store: its guard was exact only to 512 bytes and sampled 8x64
  above that, so a small edit inside a batched multi-KB UI vertex buffer hashed
  identical and the store served the previous frame's numbers. The exact bound is now
  16 KB (`CZ_VK_STREAM_GUARD_BYTES=N`), the HUD is correct in a gas-station crowd, and
  it costs **zero frame rate** — 32.2 ms at 6,778 draws, still the two-vblank floor.
  Gotchas 242 and 243 are the transferable halves.
* **The renderer's remaining picture defects** are the shadow cascade, NPC part meshes,
  mipmaps and the colour-grading LUT — all in `docs/open-items.md` with the measurement
  for each, and all worth re-testing now that a frame losing its post chain is no longer
  contaminating the evidence.

**The five-tool recompilation pipeline must run in this order**, re-running the
recompiler between each, because each one's evidence is only valid against a current
`ppc/`:

```
find_jumptables.py  ->  coverage_to_function_overrides.py  ->
    fix_switch_function_bounds.py --apply  ->  find_dropped_branches.py --prune / --widen
    ->  find_unlowered_switches.py
```

The last is a **gate, not a repair** — exit 1 means a real defect (gotcha 53).

## Reusability: what gets extracted, and when

**`docs/reusability.md`** — the tier list for what transfers to Case West and what does
not, and the two rules governing it: **extract only what is proven in BOTH ports, never
from Case Zero alone**, and **extract after the second implementation forces the seam, not
in anticipation of a third**. Do not build the Case West abstraction before Case West
starts.

## Conventions (same as the two template ports)

- No copyright/license headers in new files (user's own repo — ask before adding any).
- **Commit proactively** — whenever a change is useful on its own or important
  information was learned. End commit messages with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Document everything** in `docs/` for an outside reader — findings, dead ends,
  formats, retractions. Write it so someone porting a *different* Xbox 360 game can lift
  the technique: say what the idiom or format was, not just what we changed. That has
  concrete value here, because Case West is next and will read these documents.
- **Comment code for humans.** Every tool opens with a docstring answering *why it
  exists* — what went wrong without it — not just what it does. Inline comments explain
  the non-obvious bit-twiddling, the reason a scan runs forward instead of backward, and
  every deliberate exclusion. Generated config files carry a header saying which tool
  produced them and how to regenerate.
- **Retract in place.** When a stated finding turns out to be an artifact, say so where it
  was claimed and explain the artifact.
- Measurement discipline from day one: A/B with same-binary arms, gate comparisons,
  pre-register capture questions.

### Evidence rules (non-negotiable)

- **Measure before inferring.** A hypothesis about guest behaviour is tested against a
  census over the image, the shader bank or the capture — never argued from
  documentation or from model knowledge. **Report counts, not impressions.**
- **One change per experiment.** Fixes with distinct predictions land in separate
  commits and are verified separately.
- **State the prediction before running it.** Every fix commit records the falsifiable
  claim it makes about on-screen behaviour or dumped state, so a run can refute it.
- **A/B ADMISSIBILITY.** Two configurations are comparable at matched indices only if
  they are two states of ONE renderer producing the SAME draw set. If one arm renders
  less — or more — the comparison is inadmissible; say so and fall back to within-run
  evidence. This is the rule that would have caught `CZ_VK_FORCE_COLORMASK` in phase C
  part 9: the arm adds draws, so its picture cannot be compared with the baseline's at
  all, and it took a counter to settle what a picture never could.
- **Refutation by compensation beats refutation by absence.** When a mechanism is real
  but compensated somewhere else, record BOTH — that closes the branch properly, where
  "we looked and saw nothing" leaves it open. Phase 5 §6n is the worked example.
- **An untrusted path is not an oracle.** Only diff against a subsystem that has itself
  been validated, and re-ask that question whenever an upstream defect is fixed
  (gotcha 172). Case Zero has no in-project second implementation to diff against, so
  the substitutes are (a) Xenia traces, read for the FIRST divergent operation rather
  than the visibly broken object twenty frames later, and (b) the Fable 2 port for
  anything in the shared decode layer.

### Things not to do

- **Do not extract a library from this port alone**, and do not build the Case West
  abstraction before Case West starts. No interfaces or library splits "for later".
- **Do not bundle independent fixes into one commit.**
- **Do not treat documentation or prior model output as ground truth over a census** —
  including this file. Every number here was measured once and has a shelf life
  (gotcha 13).
- **Do not add speculative Xenos coverage.** An unsupported packet, format or import
  fails LOUDLY with its identifier; it never guesses (gotcha 5).
- **Do not copy external code before its licence is recorded.**
- **Do not delete the PM4 command processor.** See the note below — it is the control
  arm, not legacy.

### A conflict with the external "Project Constitution", recorded so it is not re-litigated

An outside constitution document describes this port as *"API-level HLE — a D3D
translation layer. There is no command processor in this project and none will be
added."* **That is not this repository.** `runtime/gpu/pm4.cpp` is a live PM4 command
processor: it is the boot engine (phase C's D3D arm still needs it for the ring and the
GPU/CPU hand-off), and more importantly it is the **same-binary control arm** for every
claim the D3D arm makes — the discipline the last nine sessions are built on. Phase C
part 9's four renderer fixes were all found on the PM4 arm because it is faster and
better instrumented. The constitution's *intent* — the D3D translation layer is where
the port is going — is right and is exactly `docs/d3d-translation-plan.md`; its
statement about the CP is wrong about the code and would, if acted on, delete the
ability to A/B.

Two more of its claims are superseded by measurement in this repo and should not be
re-derived: this title's tiles are **left/right 640-wide halves**, not horizontal bands
(window scissors `0..640` and `640..1280`, window offset `-640`), and its
"force a single tile, ignore predication" diagnostic HAS been run — it is
`CZ_PM4_NO_PREDICATION=1`, and it is destructive rather than diagnostic (phase5-notes
§6v).
