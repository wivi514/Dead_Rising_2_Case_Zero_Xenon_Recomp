# Xenia ground-truth captures needed for Case Zero — request list (round 1)

> **STATUS 2026-08-04: ROUND 1 DELIVERED COMPLETE.** A1–A5, B1/B1b/B2, C1/C2, D and E,
> all as the full game. What each file is: `Xenia logs/Xenia_Run_Content.md`. What they
> taught us: **`docs/xenia-capture-analysis.md`** (the numbered findings ledger — read
> that, not this).
>
> **Two things in this document were wrong, and are corrected in place below:**
> 1. **§A2's premise.** `NtReadFile` is `kHighFrequency`, so it is suppressed at plain
>    `log_level=3` and A2 contains **zero** reads. A5 is the `.big` seek oracle. See the
>    strikethrough in A2 and finding 2.
> 2. **§D's caveat was right to exist but the pessimism was misplaced** — `dump_shaders`
>    works and produced raw Xenos microcode. The *conclusion* drawn from it was the wrong
>    one though: the disc shader banks turned out not to match that microcode at all.
>    Finding 6.
>
> One thing this document got right and it mattered more than everything else: the
> **★ trial-mode check**. The first A1 take booted the trial. Finding 1.
>
> **ROUND 2 IS OPEN AS OF 2026-08-10 — see `xenia-capture-requests-round2.md`.** It asks for
the WORLD rather than the boot: seven paired screenshot+trace captures at the surfaces the
operator reported wrong, because part 26 established those are OUR defects (Xenia renders
them correctly) and refuted every input-side explanation for them.

**Round 1 is closed and nothing in it is outstanding.** The only candidate is an optional A2b
> (gameplay-era `.big` seek order), and finding 8 explains why it is probably unnecessary.

Written 2026-08-04, before any runtime work. Revised the same day, after reading what the
Fable 2 and Asura's Wrath ports needed in their *later* rounds — several items below
exist purely so this title does not repeat a round trip those ports paid for. Each such
item says which one.

**Nothing has been captured yet.**

Priorities are ordered: **A is enough to start the runtime; B–C unlock the renderer and
the differential debugging that carried both earlier ports through their hardest bugs.**

Everything lands in `Xenia logs/` (gitignored), with one entry per run appended to
`Xenia logs/Xenia_Run_Content.md`: what you did, in order, plus anything unusual
(crashes, missing graphics in Xenia itself, skips).

---

## Read this before capturing anything

### Do A1 first, alone, and hand it over before doing the rest

A1 is ~20 minutes. Everything else is hours. Both earlier ports wrote round-1 request
lists containing assumptions that turned out to be wrong — Asura's Wrath's guessed that
Canary strips `--trace_function_data` (it does not) and that from-boot coverage would
deadlock the title (it did not). **Capturing one log, confirming it is readable and says
what this document expects, and only then batching the rest, costs one message and can
save a whole evening.**

### Two rules about log levels, learned the hard way

1. **Only `--log_level=3` names kernel calls.** At level 2 Xenia prints handle churn and
   file paths but *no HLE call names at all*. A level-2 log is not a smaller level-3 log;
   it is a different, much less useful thing.
2. **`log_level=3` is still not the whole kernel surface.** Exports tagged
   `kHighFrequency` are logged only with `log_high_frequency_kernel_calls = true`, which
   defaults **off** — on Asura's Wrath that hid 40 of 288 imports, including most of the
   synchronisation surface and `VdSwap`. Diff against a capture without knowing this and
   you will chase divergences that do not exist. That cvar also *deadlocked* that title
   (it puts a disk write inside the lock paths), which is why it is a separate item (A5)
   and not the default.

### Never skip the movies, and say in the notes that you didn't

Asura's Wrath's B1 drive note said "skipped all intro movies". Three sessions of GPU
diffing later it turned out the port had been comparing its loading-movie frames against
hardware's title-screen frames, and a whole round-3 capture was specified to fix it — at
which point the packets showed B1 had covered the movie era all along and the *prose note
was simply wrong*. So: **let every logo/intro/loading movie play in full, and record in
the index that you did.** If you had to skip something, say exactly what.

### The `.xtr` 2 GiB cliff

Asura's Wrath's B2 GPU trace overshot 2 GiB by ~15 KB and had to be **discarded entirely**
(an `ftell` limit in the writer). Stop a GPU capture promptly at the end of its drive
rather than idling, and confirm the header is valid and the file finalized before sharing.

### Where captures come from

**All Xenia work happens on the user's Windows PC.** Xenia is unstable on the Linux box
this repo lives on, so nothing in this document can be run here — no capture can be
self-served, no cvar can be checked by grepping a local source tree, and no "let me just
try it" shortcut exists. Every item below is a request to a human at another machine, and
should be written to be executed without a follow-up question.

That machine has the **instrumented Canary fork** built for the Asura's Wrath port
(confirmed 2026-08-04), which forces the `.xtr` GPU stream writer on in Release. Section B
is therefore viable immediately.

### General

- Keep the **config dump header** Xenia prints at the top of its log — never trim it.
- Note the **Xenia build** (the `Build: ...` line) in the index.
- Vanilla settings unless stated (`gpu="any"`, `apu="any"`, no resolution scaling). If you
  changed anything in `xenia.config.toml`, copy the toml next to the log.
- Big logs are fine (Fable 2's Run2 was 1.27 GB). Compress with zstd/7z if convenient.
- **Launch the STFS package directly** (the file under `58410A8D/000D0000/`), not an
  extracted `default.xex` — the content-mount calls at boot differ, and the package is
  what the console sees. Note in the index which you used.

### ★ XBLA-specific: make sure it is running as the FULL game, not the trial

Case Zero was a paid arcade title with a trial mode, and the image imports
`XamContentGetLicenseMask`. If Xenia reports an unlicensed/trial mask, the game may take a
**different code path entirely** — different content, a timer, a nag screen — and we would
be writing the runtime against the trial's boot flow without knowing it.

This does not apply to either template port (both are disc games) and it is the single
most likely way these captures could be quietly wrong. **In A1, check early in the log for
`XamContentGetLicenseMask` and record what it returned in the index.** If the game shows
any trial/unlock prompt, say so.

---

## A. Kernel-call text logs (Canary is fine — this is the priority)

Ground truth for boot order, kernel HLE return values, file I/O, thread creation and
content mounting. Both earlier runtimes were written against these from day one.

```
xenia.exe --log_file=C:\xenia_logs\cz_runN.log --log_level=3 "path\to\58410A8D\000D0000\3A98C6..."
```

### A1 — SHORT boot at maximum verbosity  ★ do this one first, alone
Boot → every logo/intro movie played **in full** → title/menu → sit ~60 s → quit cleanly.

This becomes `docs/xenia-boot-flow.md`, the sequence the runtime must reproduce. It is
also where the license-mask check above happens.

### A2 — Into gameplay
Boot → new game → play the opening ~5 minutes of Still Creek (get outside, fight a few
zombies, pick up a weapon) → quit. Adds the gameplay kernel surface: streaming loads,
physics/audio thread creation, XMA context allocation.

> ~~**This is also our only oracle for the `.big` archive format right now.** At level 3
> Xenia logs `NtReadFile` with offsets and sizes, so the *seek pattern* into each `.big`
> tells us where its header, index and payload live.~~
>
> **RETRACTED — this premise is false.** `NtReadFile` is tagged `kHighFrequency` and is
> suppressed at plain `log_level=3` no matter what the drive is. A2 has 23,965
> `NtCreateFile` and **zero** `NtReadFile`; the canonical A1 likewise has zero (its two
> hits are import-table declaration lines). **A5 is the read oracle** — with
> `log_high_frequency_kernel_calls=true` there are 408 reads, and they cracked the format
> (`docs/big-archive-format.md`). Finding 2.
>
> A2 is still worth what it gave: 24k streaming opens, 433 distinct `.big` archives, the
> XMA context lifecycle, 86 guest threads, and the gameplay shader set.

### A3 — Save / content
Boot → new game → reach the first save point → save → back to the menu → load that save →
quit. Case Zero saves through `XamContentCreateEx`; Asura's Wrath needed
`savedrive:` → `\Device\Content\N\` and a plain file write would not have worked. This is
what tells us the exact shape.

### A4 — Idle at the title screen, long
Boot → title screen → leave it alone ~5 minutes → quit. A quiet log makes the per-frame
steady state legible against A1's noisy boot.

### A5 — High-frequency kernel calls (separate run; may hang)
A1's drive with `--log_high_frequency_kernel_calls=true`. Expect it to be huge and expect
it possibly to deadlock — if it does, say so in the index and keep however far it got.
Even a boot-only prefix is valuable: it is the only view of the synchronisation surface,
`VdSwap`, and `XamInputGetState`.

---

## B. GPU command-stream traces (`.xtr`)

```
xenia.exe --trace_gpu_stream=true --trace_gpu_prefix=C:\xenia_logs\cz_B1\ ... "path\to\package"
```

**Stock Canary strips the `.xtr` writer in Release.** Use the instrumented Canary fork
built for the Asura's Wrath port, which forces it on — confirmed still available
2026-08-04. All custom instruments in the fork **off**; vanilla settings otherwise.

The trace must be running **from process start**, not attached later. Frame 0 matters more
than any other frame.

### B1 — Boot → title screen
**Same drive as A1**, movies played in full. Gives the draw profile per frame, the shader
set, the EDRAM layout and the swap cadence for everything up to the menu.

### B1b — B1 again, unchanged  ★ new; not in either earlier port's round 1
Identical drive, identical settings, second run. This is the determinism control.

Asura's Wrath only obtained a repeat capture in round 3 and it immediately paid for
itself: it established that a GPU stream is **deterministic in content and jittery in
phase** (±2 frames at era boundaries, 0.38% on totals). Without that, there is no way to
know whether a difference between our stream and hardware's is a real defect or run-to-run
noise — and the port spent time treating noise as signal. It costs one extra run now.

### B2 — Gameplay
**Same drive as A2.** Expect an order of magnitude more draws per frame. Watch the 2 GiB
cliff — stop promptly.

### Same-run correlation log — please treat as required, not optional
If the fork will emit both at once, take a `--log_level=3` log **from the same run** as
each `.xtr`. That lets a frame index be tied to a file open. Asura's Wrath's E1/E1b did
this and it was worth more than either capture alone; its B-series did not, and that is
part of why the movie-era confusion above went unnoticed for three sessions. If it costs
a second run instead, the `.xtr` is the one that matters.

### If you are rebuilding the fork anyway, build Debug too  ★ new
Three GPU cvars — `log_guest_driven_gpu_register_written_values`, `disassemble_pm4`,
`log_ringbuffer_kickoff_initiator_bts` — are gated at **compile time** on `XE_DEBUG`, not
by the runtime `debug` cvar. In a Release build they are silently compiled out: Asura's
Wrath ran an arm with `disassemble_pm4=true debug=true` and got **0 PM4 lines**, which
reads as "the cvar did nothing" rather than "this build cannot do that".

We do not need those captures yet — they belong to the renderer phase, and the first of
them is only useful once we know what to ask it. But if a fork build is happening now,
producing a Debug binary alongside the Release one costs a compile and removes a future
round trip. **Do not capture with it yet; just have it.**

For scale on what it eventually gives: on Asura's Wrath that register log was 4.9 M writes
over 2,473 registers, which reads as "port the whole Xenos register file" — until it is
reduced by *distinct value count*, at which point 2,255 registers never change and **48**
are the actual render state. A 50:1 compression, and the difference between a month and a
week.

---

## C. Function-coverage traces (`--trace_function_data`)

Stock Canary does **not** strip this.

```
xenia.exe --trace_function_data=true --log_file=... "path\to\package"
```

### C1 — Boot → title.  C2 — gameplay.
Same drives as A1 and A2.

This is a **two-sided** oracle and most ports use only one side:

- **Forwards** — *hardware ran an address we have no function for* — recovers missing
  entry points for `config/CaseZero.toml`'s `functions` list (vtable slots and
  runtime-built function-pointer tables that no `bl` points at). Asura's Wrath recovered
  215 this way. **This is the one item in section C that pays off immediately**, before
  any runtime exists.
- **Backwards** — *we ran a function hardware never ran* — localises a control-flow
  divergence to a single function, with no debugger and no reproduction. Needs a running
  runtime, so it is for later.

Treat the capture's function boundaries as **ranges, never identities**: the emulator's
function analysis will not agree with the recompiler's, and 4-byte single-instruction
functions are not comparable at all (they were 52 of 52 first-pass false "divergences" on
Asura's Wrath). Classify by function size before believing any divergence.

---

## D. Shader dumps  ★ new; answers the biggest open question in the project

Case Zero ships its shaders as **loose banks on disc**
(`data/shaders/deadrisingprologue-{vs,ps,vd,pd,sc,sd,ss}.big`) rather than embedded in
packages the way Fable 2 did. If those banks hold raw Xenos microcode, they feed
XenosRecomp almost directly and the renderer phase looks nothing like Fable 2's — which
needed a whole `.sbk` extraction pipeline and, in the interim, a hand-written software
rasterizer.

A dump of the shaders Xenia sees the guest submit gives us the ground-truth microcode to
compare those banks against, which settles it.

The cvar to use is **`dump_shaders`**, in Xenia's `GPU` cvar group — a *path*, not a
boolean: it names a directory that Xenia writes each shader into as it is compiled.

```
xenia.exe --dump_shaders=C:\xenia_logs\cz_shaders\ ... "path\to\package"
```

**Verify before relying on it.** This cannot be checked from here (see "Where captures
come from"), neither earlier port used it — neither title had loose shader banks, so the
question never arose — and the fork is several commits from upstream. Confirm it appears
in `xenia.exe --help` or `xenia.config.toml`'s `[GPU]` section, and confirm the directory
actually fills up. If the cvar is absent or writes nothing, say so and we will get the
same information from a runtime `SHADER_DUMP` hook later instead; it is not worth
debugging Xenia over.

**This is a five-minute check bolted onto A1's drive, not a separate capture.** Run it
alongside A1 and report what the directory contains — file count, extensions, and the
first few bytes of one file is enough to tell whether we are looking at raw Xenos
microcode or Xenia's translated output.

---

## E. Screenshots

A handful of PNGs at known points — first logo, title screen, first gameplay frame, a
zombie crowd. The visual target for the renderer. Note the frame index if you can.

---

## Not requested, and why

- **A dedicated audio capture.** A2 at level 3 already shows XMA context allocation and
  the `XAudio*` pump, which is what phase 6 needs to start. A targeted capture should wait
  until there is a specific question for it to answer — Asura's Wrath produced two rounds
  of requests it later had to retract because they were written before the question was
  sharp. Its gotcha #18: *a capture request is a hypothesis with a shelf life.*
- **The Debug-build GPU register captures.** Same reason; see the note in section B. Build
  the binary now if convenient, capture later.
- **`trace_function_coverage`** (distinct from `trace_function_data`). It writes a
  *per-instruction* branch oracle in a different, self-describing record format — Asura's
  Wrath's 48-byte reader read it as zero functions and 762,193 resyncs, i.e. as an inert
  flag, when it was actually the most detailed oracle available. Worth remembering it
  exists; not worth capturing until there is a divergence to chase with it.

## R5 (requested part 39, 2026-08-12) — ONE trace standing at a SHARD TREE

**Why this is the whole ask.** Item 0t (foliage renders leaf cards as solid angular
shards, the cutout never happens) had ALPHA-TO-MASK as its suspect, and part 39
**refuted it from data already on disk**: across all eight `R4_world` traces — 40,703
draws — hardware enables neither RB_COLORCONTROL bit 3 (alpha test) nor bit 4
(alpha-to-mask). Nor is it a shader `kill`: exactly 1 of R4's 208 dumped pixel shaders
has one.

**What R4 cannot answer, and why.** The shard-tree material — our
`ps_c9ca4f73ba93d023`, DXT5 albedo + DXN normal — is **absent from R4's 261-shader
bank**. R4 is the Big Buck hardware store; the operator's tree capture
(`part38-operator/arm1_default/capture_f28446`) is somewhere else. So the register
state that would answer "how does hardware cut these leaves out" has simply never been
captured at the right place.

**The request:** one single-frame F4 GPU trace **standing where capture_f28446 was
taken**, in front of the trees, with the usual `dump_shaders` on and the frame-locked
PNG. Same form as R3/R4 — no new capability needed. One trace is enough; this is a
register read, not a survey.

**What it decides, either way.** With the trace in hand, `tools/xtr_draw_bindings.py`
prints RB_COLORCONTROL, RB_ALPHA_REF, RB_BLENDCONTROL0 and RB_DEPTHCONTROL per draw,
and the shader dump says whether hardware's foliage microcode kills. That is the same
one-column read that closed the alpha-to-mask branch, applied to the place that
actually has the defect.

### R5 IS WITHDRAWN — R4 ALREADY ANSWERS IT (part 39, same night)

The operator's street sweep produced a mid-distance shard tree with a per-draw census,
and the material is **`ps_790283523afcaf20`**, not `ps_c9ca4f73ba93d023`. That shader IS
in R4's bank, with **1,408 hardware draws across the eight traces**. The request above
was keyed to the shader from the part-38 tree frame and was therefore asking for ground
truth we already hold.

**The lesson, and it is the reusable half:** before requesting a capture for a material,
identify the material from a CENSUS at the defect rather than from the last frame that
happened to show the symptom. A shader hash carried over from an earlier capture is a
guess about which draw you are looking at.

What R4 then says, at those 1,408 draws: **no alpha test, no alpha-to-mask**, and a
blend split of 118 blended / 58 opaque per frame that **our frame reproduces exactly**
(176 draws, 118/58). The leaf albedo is `md5 6f621715…`, **byte-identical** between our
live process and hardware's trace. So state and inputs agree and the divergence is
downstream of both. See `docs/open-items.md` 0t.

## R5 (part 43): ONE matched-state capture at the fresh DebugJump spawn — settles item 00i's remaining fork

**The ask, in one line: DebugJump to Case 0-2, do not move, press F4 once at the
spawn, deliver the trace + its frame-locked PNG.**

No memory patching, no new instrument, one capture. Full game as always
(`license_mask = 1`), same fork and settings as R4.

**What it decides.** Part 43 named every input of the per-zone
`COMMON_TEXTURE.tex` vs `COMMON_TEXTURE_LOD.tex` choice and verified ours are
live and correct at the spawn (`docs/phase5-notes.md` §6bw): the engine's own
math picks the thumbnail set for zones 1/2/3/7 from there, deterministically,
by margins of 31–107 m — and the choice is made ONCE per zone load, with no
promotion trigger we could find or provoke. The eight R4 traces cannot
adjudicate this because they are a standing sweep AT Big Buck taken after a
walk — a WARM session, in which zone texture sets may have been (re)loaded
with the player near them. Fresh jump vs warm walk are unmatched arms.

* If the fresh-spawn hardware frame shows the same patternless far buildings
  (and its trace binds ≤16×16 s0 atlases on ≥200-vert draws of
  `ps_34524bb64374d20e`), our runtime is CORRECT, item 00i's flat-at-range is
  the engine's own fresh-state behaviour, and the item collapses to "compare
  like states before comparing pictures".
* If the fresh-spawn hardware frame is fully textured (0 tiny-on-big, as in
  R4), a real input divergence exists at hardware's zone load, and the suspect
  list is ordered: the per-volume skip bit (u32 at elem+0x90 bit 0 — set
  forces FULL; all zero on our side), the volume spheres/thresholds
  themselves, then a zone-reload mechanism we have not found.

**Optional second capture, same session, if cheap:** after the F4 at the
spawn, walk to Big Buck (the R4 route), F4 again anywhere mid-walk. If the
spawn frame was flat and the mid-walk frame is full, hardware DOES promote on
approach and the reload trigger is the thing to find next; if both are flat
until some event, that event is the mechanism.


### R5 addendum (same day): THE TRACES ALREADY HELD MOST OF IT — R5 narrows to one QUESTION, capture optional

The operator pointed out R4's eight far-to-close shots should already answer
this, and they did (`docs/phase5-notes.md` §6bw addendum): hardware's camera
positions are recoverable from the traces (`vc12..14` .w of the 25,234-vert
ground draw, (y,x,z) order), and at ALL ten recovered positions (8x R4 +
tanker + green_building from R3) the fresh decision math says LOD for zones
1/2/3/7 by +21..+136 m — yet every frame renders full. Hardware's zone state
is therefore NOT a fresh batch decision at the capture spot; its zones were
loaded or re-loaded with the player near them.

**What remains is ONE SENTENCE from the operator, not a capture: how did the
R4 session enter the level — DebugJump (F2 -> Case 0-2) then walk, or normal
play / a save?**
* DebugJump-then-walk -> hardware re-ran zone loads on approach, our runtime
  never does: the reload trigger is the defect and the hunt has a target.
* Normal play -> both runtimes are faithful; the flat-at-range is the
  batch-load state itself, and the fix conversation changes shape (match
  session states, or reproduce the on-approach loading normal play does).

The R5 capture (fresh-jump stand-still F4) is still welcome as belt-and-
braces — it would show whether hardware's own DebugJump batch decision
matches ours — but it is no longer the blocking measurement.

## R6 (part 57): ONE single-frame trace at the FAR gas-sign viewpoint

**The question it settles**: whether the GAS sign's black-letters defect is in the
CONTENT of the far-LOD textures (our streaming/level machine filling guest memory
with something other than hardware's) or in our GPU pipeline (untile/format/sampler).

Part 57 exhausted the self-serviceable half:
* the sign was ATTRIBUTED with the draw-ID pass — the far disc+letters are
  `ps=57d441f53fc93ad7` (batched far-LOD material) and `ps=86ac6569ea0d700d`
  (the letters, s0 = a 32x16 tiled DXT1);
* our translation of `57d441f53fc93ad7` was read instruction-by-instruction against
  the capture's own disassembly — FAITHFUL, the shader code is exonerated;
* a live dump of the letters' 32x16 (109EF000, taken seconds after an operator F9,
  standing still) decodes to 68.8% black with garbage, and its companion 64x64
  (11FA0000) decodes to shredded stripes AT EVERY CANDIDATE EXTENT — while the same
  dump's 256x256 lightmap decodes perfectly with the same tool, so the decode method
  is validated and the CONTENT ITSELF is the anomaly;
* the content matches nothing in `pro_z02_gas_text_flicker{,_LOD}.tex` in any
  endianness (though per-record compression inside .tex could hide a match).

**The ask**: stand at the far viewpoint where the sign's letters are broken (the
part-56 capture_012174 spot — down the street, sign small), single-frame .xtr trace,
same protocol as the R2 seven. The trace carries the letters draw's fetch constants
AND the texture bytes hardware sampled; `tools/xtr_draw_bindings.py` reads both.
* If hardware's bytes at the letters' s0 differ from our dump -> the defect is in OUR
  loading (streaming level machine / file layer), and hardware's bytes show what the
  content should be.
* If they MATCH ours -> the tiny-texture content is legitimate and the defect is in
  our sampling of it (and the census's fetch fields for that draw become the diff).
