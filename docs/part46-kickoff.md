# Part 46 kickoff — the flat class is FIXED AT ITS ROOT and the operator confirms it in play; what is left is the tree canopy and an overshoot with no symptom

Written at the end of part 45 (2026-08-15). **This is the LIVE hand-off**,
superseding `part45-kickoff.md`. Read `docs/phase5-notes.md` §6by **and its
two addenda** first — part 45 solved the white/flat-surface class at its
root, the operator's own A/B confirmed it in play (addendum 2), and every
shading-side measurement taken before it has gotcha-172 exposure.

## What part 45 established (do not re-derive)

* **The white/flat-prop class was OUR OWN `tools/synth_shader_container.py`:
  register-granular liveness dropped a PS interpolant whenever the register's
  FIRST touch was a partial write** (`tfetch2D r0.__xy` keeps .xy — dest
  swizzle 7 = Keep), so the translated HLSL zero-initialised the register and
  the surface sampled ONE TEXEL forever. 265 of 333 PS containers changed;
  **217 gained interpolants**; zero VS changes. Fixed per-component (commit
  fdda6f3), encodings transcribed from XenosRecomp's own operand printer;
  gotcha 316. The menu-lab elimination that convicted it (all inputs
  byte-matched against B1 — textures md5-equal, UVs equal, constants equal,
  dummies poison-refuted) is §6by's record and the method template: when
  every input matches and the output differs, read the generated HLSL against
  the microcode disassembly.
* **Gates on the new default cache** (`assets/shader_spv`, 435 modules): dim
  census clean; `no translated shader` = 0; menu ball RED (E3's content);
  **E3 correlation +0.687 → +0.710 (fail → pass)**; spawn capture gains the
  QUARANTINE bus lettering + van panel detail (part-26 white-prop members).
  The pre-fix cache is preserved WHOLE at `assets/shader_spv_pre45` —
  `CZ_SHADER_SPV` selects it; every part-45 picture claim is a same-binary
  A/B against it.
* **The lost-microcode entry** `ps_926c15dd20571cf1` is carried over from the
  old cache un-rebuilt (its ucode is gone; the standing name-diff exception).
  If any run ever binds it, its interpolator list has the OLD defect.
* **pc255=0 is not automatically a defect**: the ball shader's literal bank
  (c253..c255 via LOAD_ALU_CONSTANT) is coherent and c255.x=0 is the CORRECT
  bias for its normal-z reconstruction. The DoF gather's pc255 question
  (part-42 kickoff item) should be re-derived from ITS microcode's use, not
  presumed broken because hardware's copy is unrecoverable.
* **The overshoot signature SURVIVES the fix**: mip tint on the CLEAN bank at
  the DebugJump spawn reproduces part 44's reading (trucks solid L1 at ~2 m,
  vans L2 at ~10 m; `~/DR2CZ-troubleshooting/part45/tintcap_fixed/`). The
  scene pass's derivative environment was audited and is structurally
  matched (viewport path, true 1280×720 raster = hardware's pixel grid; MSAA
  window scaling touches only window-coordinate draws), so the remaining
  suspects are sampler-level terms and the reading itself.

## The plan

0. **DONE AT PART 45'S CLOSE — the operator session ran and the verdict is
   IN.** Two launches, one binary, `CZ_SHADER_SPV` apart. Fixed: *"the game
   looks way better now, the building doesn't seem to have issues… almost
   like OG game."* Pre-45: *"way worse — gas station looks bad and building
   look FLAT DEPENDING ON DISTANCE."* Item 00i's flat-at-range class is
   substantially closed by the liveness fix (§6by addendum 2). Their ONLY
   remaining complaint is the tree canopy (item 3 below). Session logs:
   `~/DR2CZ-troubleshooting/part45-operator-session{,-pre45}.log`; captures
   in `part45-operator/`. What is still owed from an operator, cheaply, next
   time they play: an F9 at the gas station on BOTH arms (run 2 took none),
   and the part-26 white-prop tour, which this session did not walk.
1. **The overshoot, on the clean bank** (open-items 00i outdoor half):
   * **DO THIS ONE FIRST: re-run `CZ_VK_NO_MIPS=1` at a matched view on the
     FIXED cache.** Part 44's "no-mips restores building detail" is the
     overshoot's ONLY picture-level support and it was measured through the
     broken shaders. If it no longer restores anything, the overshoot is an
     instrument reading with no symptom and the item drops down the list —
     the operator now says the buildings look right with the mip chain
     untouched. (Part 45 queued this run; see Standing state for its result.)
   * The contact points ran at part 45's close, same view, clean bank:
     **tint+`CZ_VK_ANISO=0` is ~1 octave DEEPER than the default** (ground
     and vans shift red→green at the same distances) — aniso ENGAGES in the
     default and buys its octave back — and **tint+`CZ_VK_NO_FETCH_SAMPLERS=1`
     is deeper still** (Chuck's skin reads L1 at arm's length). Neither arm
     SHALLOWS, so the sampler terms are helping, not causing: the residual
     +1..2-octave global shift persists with everything engaged, cause
     unnamed. Both captures + A/B stacks in
     `~/DR2CZ-troubleshooting/part45/` (`tint_aniso0`, `tint_nofetch`,
     `tint_ab_*.png`).
     Calibration note recorded: these fetches declare LINEAR mip filtering,
     so a SOLID code color needs LOD ≥ ~1 at the surface; the vans' solid
     red at 5–10 m is a genuine ≥ +1-octave shift, not trilinear bleed.
   * The hard number: hardware's implied LOD for a NAMED wall draw from
     R4_04's vertex streams (UV span × texture size ÷ screen extent) vs our
     tint reading at the matched pose. `r4_04_bindings.csv` is already cut;
     `xtr_draw_vertices.py` gives the streams; the wall draw should be named
     by projecting candidates with the recorded constants, not by texture
     inference (gotcha 302).
   * Also worth one thought first: verify the TINT READING's own calibration
     (trilinear blends L0/L1 into visible tint well below LOD 1; a "solid
     L1 at 2 m" claim needs the tint's alpha as a function of LOD written
     down once) — the overshoot magnitude may be smaller than it reads.
2. The 0u residues (DoF fmt6 packed byte view; pc255 re-derivation per
   above), parked since part 42.
3. **THE TREE SHARDS — the operator's one complaint on the fixed cache, and
   it now has a name.** Close-up canopies render hard-edged BLACK shards
   among otherwise-correct leaves
   (`~/DR2CZ-troubleshooting/part45-operator/capture_006615/006826` + census).
   Established in the part-45 session: it PRE-DATES the liveness fix
   (part-44's capture_008693 tree shows the same class; the menu tree is
   identical on both caches), and the leaf materials are
   **ps_69a5c3be9359b87c / ps_8602b5fd69289893 at cc=AA00001C — alpha test
   GREATER + ALPHA_TO_MASK, 56 draws in the capture**. Both shaders GAINED
   interpolants in part 45 (ps_8602 gained its UV regs 0/1), which is why
   the leaf texture itself improved; the shards are the remaining term.
   Leading mechanism: DXT1 punch-through transparent texels decode BLACK,
   and the A2M half of the cutout is the part-41 parked item 4 — read how
   `vk_renderer.cpp` treats RB_COLORCONTROL bit 4 before theorising. Also
   still parked: clamp modes / cyan fringes (part-41 item 5).
4. The AO-only-up-close observation: re-check like-for-like on the NEW cache
   (its old evidence predates the fix).

## Standing state

* Cache 435 at `assets/shader_spv` (fixed liveness), `assets/shader_spv_pre45`
  is the control arm. Dim gate clean. Commits: fdda6f3 (the fix), f09a0d1
  (docs), plus this kickoff.
* Part-45 artifacts: `~/DR2CZ-troubleshooting/part45/` (README indexes the
  ball adjudication byte dumps, the verdict images, the fixed-cache menu and
  spawn captures, the clean-bank tint baseline, r4_04_bindings.csv).
* The headless spawn F9 recipe that works around the CASE FILE card:
  `CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,NONE,A,NONE,A,NONE,NONE,F9,NONE`
  (the card lands late; two spaced As catch it, F9 at interval 14).
* The sledgehammer freeze (part-43) remains the operator-session rider.
