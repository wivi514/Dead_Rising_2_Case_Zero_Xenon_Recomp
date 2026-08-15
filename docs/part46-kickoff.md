# Part 46 kickoff — trees first, then the performance regression (which now owns the UI guard fix too); the flat class is FIXED and operator-confirmed

Written at the end of part 45 (2026-08-15). **This is the LIVE hand-off**,
superseding `part45-kickoff.md`. Read `docs/phase5-notes.md` §6by **and its two
addenda**, then §6bz **and its addendum**, before anything else.

## The operator's own priority order for part 46

Stated at the close of part 45, and it governs this kickoff:

1. **The trees** (item 1 below) — their one outstanding picture complaint.
2. **The performance regression** — *"the performance degraded with all the fix
   you did in the last few days"*. Explicitly to be taken up right after the
   trees, in the same conversation.

Everything else in this document is background for those two, or is parked.

## What part 45 established (do not re-derive)

* **The white/flat-surface class was OUR OWN `tools/synth_shader_container.py`:
  register-granular liveness dropped a PS interpolant whenever the register's
  FIRST touch was a partial write** (`tfetch2D r0.__xy` keeps .xy — dest
  swizzle 7 = Keep), so the translated HLSL zero-initialised the register and
  the surface sampled ONE TEXEL forever. 265 of 333 PS containers changed;
  **217 gained interpolants**; zero VS changes. Fixed per-component (commit
  fdda6f3), encodings transcribed from XenosRecomp's own operand printer;
  gotcha 316. The menu-lab elimination that convicted it (all inputs
  byte-matched against B1 — textures md5-equal, UVs equal, constants equal,
  dummies poison-refuted) is §6by's record and the method template: when every
  input matches and the output differs, read the generated HLSL against the
  microcode disassembly.
* **THE OPERATOR CONFIRMED IT IN PLAY, both directions** (§6by addendum 2). One
  binary, `CZ_SHADER_SPV` apart, their own route. Fixed: *"the game looks way
  better now, the building doesn't seem to have issues… almost like OG game."*
  Pre-45: *"way worse — gas station looks bad and building look FLAT DEPENDING
  ON DISTANCE."* **That second sentence is item 00i's founding complaint**, so
  what drove parts 42-44 was substantially this defect.
* **Gates on the new default cache** (`assets/shader_spv`, 435 modules): dim
  census clean; `no translated shader` = 0; menu ball RED; **E3 correlation
  +0.687 → +0.710 (fail → pass)**. The pre-fix cache is preserved WHOLE at
  `assets/shader_spv_pre45` and selected with `CZ_SHADER_SPV` — every part-45
  picture claim is a same-binary A/B against it, and it is the arm to reach for
  when asking whether a NEW defect predates the fix.
* **The mip overshoot signature survives on the clean bank but has NO VISIBLE
  SYMPTOM** (§6by addenda 1 and 2). Sampler arms exonerated (`CZ_VK_ANISO=0`
  and `CZ_VK_NO_FETCH_SAMPLERS=1` both go DEEPER; neither shallows). Before
  spending anything more on it, re-run part 44's `CZ_VK_NO_MIPS=1` arm on the
  FIXED cache — it is the overshoot's only picture-level support and it was
  measured through the broken shaders.
* **pc255=0 is not automatically a defect**: the ball shader's literal bank is
  coherent and c255.x=0 is the CORRECT bias for its normal-z reconstruction.
* **The lost-microcode entry** `ps_926c15dd20571cf1` is carried over from the
  old cache un-rebuilt; if a run ever binds it, its interpolator list has the
  OLD defect.

## The plan

1. **THE TREE SHARDS — the operator's one remaining picture complaint.**
   Close-up canopies render hard-edged BLACK shards among otherwise-correct
   leaves (`~/DR2CZ-troubleshooting/part45-operator/capture_006615`, `_006826`,
   plus their censuses). **It PRE-DATES the liveness fix** — part 44's
   capture_008693 tree shows the same class and the menu tree is identical on
   both caches — so it is not a regression from part 45.
   Named: the leaf materials are **`ps_69a5c3be9359b87c` /
   `ps_8602b5fd69289893` at `cc=AA00001C` — alpha test GREATER +
   ALPHA_TO_MASK, 56 draws in the capture**. Both GAINED interpolants in part
   45 (ps_8602 gained UV regs 0/1), which is why the leaf texture itself
   improved; the shards are the remaining term.
   Leading mechanism: DXT1 punch-through transparent texels decode BLACK, and
   the A2M half of the cutout is part 41's parked item 4 — read how
   `vk_renderer.cpp` treats `RB_COLORCONTROL` bit 4 (`kRbColorControl`, and
   mind the register-index warning: 0x2202, not 0x2205) before theorising.
   Name the property first (memory: *name the property a fix should move*) —
   e.g. the count of pure-black pixels inside the canopy's draw-ID footprint,
   which `CZ_VK_DRAW_ID` + `drawid_read.py` can bound exactly.
2. **THE PERFORMANCE REGRESSION — operator-reported, unmeasured.** *"The
   performance degraded with all the fix you did in the last few days."* Take
   it up immediately after the trees.
   Candidate causes, in the order they were introduced: part 41's per-fetch
   samplers; part 44/45's mip-chain and tail uploads; and **part 45's liveness
   fix itself, which added interpolants to 217 pixel shaders** — more varyings
   is more interpolation and more register pressure, so this is a real suspect
   and `assets/shader_spv_pre45` makes it a ONE-VARIABLE A/B.
   **Measure it the way this project has learned to** (do not shortcut):
   `CZ_VK_PROFILE` for the phase split, THREE runs an arm alternated, and read
   MEDIANS plus the share of frames pinned to a 16 ms multiple — the mean
   measures this title's vblank pacing floor and not your change (gotchas
   237/238, `docs/measurement.md`). Noise floor is 10-13% at one run a side.
   Quote `tools/gpu_clock_sample.py` rather than assuming the clock.
3. **THE UI TEXT LAYER (open item 00k, §6bz + addenda) — MECHANISM CONFIRMED,
   the FIX is what is owed, and it is entangled with item 2.** A matched
   operator A/B (STATUS / KEY ITEMS tab, same save, one env var apart) settles
   it: the default guard renders the ATTRIBUTES tab's labels, and
   `CZ_VK_STREAM_GUARD_EXACT=1` renders the tab's real contents. It is the
   cross-frame store's GUARD — item 00c above the 16 KB bound that fixed it.
   **But "always exact" is not the fix**: that arm read 63.76 MB/frame in the
   guard against 9.28, which is exactly the kind of cost item 2 is about, so
   design and measure them TOGETHER. Try raising the bound first
   (`CZ_VK_STREAM_GUARD_BYTES=N`, no rebuild), then making exactness a
   property of the stream KIND. **Part 45's TEAR reading is RETRACTED and
   `CZ_VK_FRAMES_IN_FLIGHT=1` is NOT the arm.**
   Symptoms: colour changes mid-word, glyphs missing, and the PREVIOUS screen's
   text persisting; static text in the same frames is perfect. The text layer
   is ONE dynamic vertex buffer sub-allocated per run by `VGT_INDX_OFFSET`
   (§6ab), so one bad copy garbles every run at once.
   The frame-to-frame variation (three captures of one screen, 3.5-4.3% apart)
   is the guard too: 8 sampled blocks of 64 bytes catch an edit only when one
   lands on it, so different fragments update on different frames.
   Eliminated: draws are not dropped (both bounds counters ZERO across 54.7M
   draws) and the ALU constant window is read per draw.
   **THE HEADLESS REPRO THE OPERATOR HANDED US, and it is the cheap check for
   this whole class**: press START at the title screen and a card appears for a
   second or two **completely EMPTY** — trim drawn, not one glyph inside.
   Captured at `part45-operator/ui_fixed/capture_001343.ppm`. Reachable with
   `CZ_FAKE_PRESS_SEQ=START,…` and needs no play session, which every other
   instance of this defect did. **Check it whenever this class is suspected.**
4. Parked, unchanged: the mip overshoot (above, and re-run NO_MIPS first); the
   0u residues (DoF fmt6 packed byte view; pc255 re-derivation per above);
   part 41's clamp modes / cyan fringes; the AO-only-up-close observation
   (re-check like-for-like on the NEW cache — its old evidence predates the
   fix); and the part-43 sledgehammer FREEZE, which never recurred in part 45's
   long operator sessions with `CZ_WAIT_TRACE=1` armed.

## Open questions to put to the operator FIRST

* ~~Did the `CZ_VK_STREAM_GUARD_EXACT=1` arm change the UI text?~~
  **ANSWERED, and by a picture rather than a sentence**: the operator's arm
  session left one capture and it is decisive (§6bz addendum 2). Nothing to
  ask.
* Cheap and still owed from any operator session: an F9 at the gas station on
  both cache arms (part 45's run 2 took none), and the part-26 white-prop tour
  (newspaper boxes, cash register, gas-station sign, bathroom window), which no
  session has walked since the fix.

## Standing state

* Cache 435 at `assets/shader_spv` (fixed liveness); `assets/shader_spv_pre45`
  is the control arm. Dim gate clean. Part-45 commits: fdda6f3 (the fix),
  f09a0d1, 8b2bf18, 60939bd, 1a0463d, 1330135, plus this kickoff.
* Artifacts: `~/DR2CZ-troubleshooting/part45/` (ball adjudication byte dumps,
  verdict images, fixed-cache menu/spawn captures, clean-bank tint baseline,
  the two sampler contact points, r4_04_bindings.csv — README indexes it) and
  `~/DR2CZ-troubleshooting/part45-operator/` (`ui_fixed/` = 32 UI captures on
  the fixed cache, `arm_pre45/` = run 2, `ui_guardexact/` = EMPTY).
* Session logs: `part45-operator-session.log` (fixed),
  `part45-operator-session-pre45.log` (run 2),
  `part45-operator-ui-session.log` (the 32-capture UI session, with
  `CZ_SCREEN_TRACE`), `part45-operator-ui-guardexact.log` (the arm).
* Headless recipes verified this part:
  * spawn F9 past the CASE FILE card —
    `CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,NONE,A,NONE,A,NONE,NONE,F9,NONE`
  * pause menu from gameplay — the same with `START,NONE,F9,NONE` appended, and
    it renders CORRECTLY on a fresh session (that is the point: the UI defect
    needs an accumulated session, except for the empty title card above).
