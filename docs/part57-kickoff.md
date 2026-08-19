# Part 57 kickoff — finish the four picture defects the operator found in part 56

> **THIS IS THE LIVE HAND-OFF.** Part 56 was a picture session driven end to end by
> operator captures. It **implemented the stencil test** (a real fix, partially confirmed),
> **refuted two hypotheses** about the other defects, and **left the decal flicker
> unexplained**. Everything below is measured; where something is a guess it says so.
>
> Performance is PARKED in `docs/perf-state-parked.md` and is not this part's subject.

---

## 0. THE FOUR DEFECTS, and exactly where each stands

| # | defect | state | next action |
|---|---|---|---|
| A | zombie slicing — two whole bodies, square blood | **stencil implemented, operator says "isn't as bad but far from perfect"** | find out what is still wrong — the question is ASKED and unanswered |
| B | decal flicker under camera motion | **UNEXPLAINED.** depth and mips both refuted | build the instrument the last session lacked (below) |
| C | GAS sign / canopy / bunting wrong at distance | open; mips refuted; operator's own steer is item 00i | read our translated SPIR-V for a far-only shader |
| D | polygon offset | implemented, correct emulation, **fixes nothing observed** | leave it; it is not a defect |

---

## 1. DEFECT A — the stencil, and the ONE question that is owed

**What shipped**: `stencilTestEnable` had appeared ZERO times in this renderer while
RB_DEPTHCONTROL bit 0 is `stencil_enable`; **350 of 1980 and 326 of 1823 draws** in the
operator's own slicing frames enable it. Both register layouts were confirmed BY COHERENCE
rather than from a document — decoded, the title's configurations come out as a matched
pair (`ALWAYS/REPLACE` writing reference 254, `EQUAL/KEEP` painting only where 254 was
written), which a wrong layout could not produce.

**Their verdict**: *"Isn't as bad but far from perfect"*, with captures in
`~/DR2CZ-troubleshooting/play/play_0819_1815/` showing a correctly shaped lower half
(`capture_013122`) and a correct decapitation cross-section (`capture_010173`).

**THE OWED QUESTION, asked and not yet answered: what is still wrong?** Three captures were
examined and the remaining defect could not be seen in them — both bodies in the close-up
render correctly. **Do not start guessing from the images; ask.** Useful shapes for the
question: is the cap the wrong shape, colour or place? do halves still sometimes come out
whole? does it depend on the weapon or the cut direction?

**If it turns out to be intermittent**, the first suspects in order, none of them yet
tested:
* the guest writes the mask in one pass and tests it in a later one, and our EDRAM handling
  clears or reuses the depth/stencil image in between;
* two-sided stencil: `ds.back = ds.front` is copied when RB_DEPTHCONTROL bit 7
  (`backface_enable`) is clear, which is an interpretation and not a measurement;
* the stencil is not preserved across a resolve.

---

## 2. DEFECT B — the decal flicker, and the instrument that has to be built FIRST

**Everything known, and it is all constraint rather than cause:**
* flickers ONLY while the camera moves; with a still camera it is stable — *"the decals
  either stay or is not there"*. That is the single most diagnostic thing the operator said.
* **NOT z-fighting.** `CZ_VK_POLY_OFFSET_SCALE=10000` visibly lifts decals in front of walls
  and zombies, and their verdict at that setting was *"when they are not floating they
  flicker"*. A ten-thousand-fold depth bias does not stop it.
* **NOT bad mip data.** `CZ_VK_NO_MIPS=1` engaged (mip-rejection lines 8 -> 0) and nothing
  changed.
* The decals are identified in the census beyond doubt: single-quad (`verts=6`),
  alpha-blended (`blend=07060706`), carrying `po=0/-4.8/-3e-05` or `-1.6/-1e-05`.

**THE INSTRUMENT GAP THAT STOPPED THE LAST SESSION, and the first thing to build here:
`F8`'s burst records pixels and per-frame FINGERPRINTS but no PER-DRAW CENSUS.** So when the
frames showed a decal blinking, there was no way to ask whether its draw was issued that
frame. Fix that — arm the draw census for the burst's frames (or every Nth) — and the
question "is the draw missing, or issued and then discarded" is answerable in one press.

Note the camera must move for this defect to appear, which is why `burst_read.py` will
refuse to conclude from the draw list; the per-draw census is what replaces that.

---

## 3. DEFECT C — the distance defects, and the operator's own steer

Their steer: *"For the gas sign, rooftop and all this type of thing that are at a distance
I am pretty sure it is similar to our last issue that was like that"* — i.e. **item 00i**,
which part 45 traced to OUR translation dropping pixel-shader interpolants so 217 shaders
sampled diffuse at ONE TEXEL. A single texel is a flat colour and a dark one is BLACK, which
is the sign's symptom: the letters do not blur, they are REPLACED by black with a
magenta/yellow/cyan fringe.

**What the captures established**: **17 pixel shaders appear only in the far/broken frame
and 9 only in the near/fine one**, so this title swaps pixel shaders BY DISTANCE. The far
view is different code, not the same shader with a smaller texture — which is consistent
with mips having been ruled out. Their metadata shows no obviously stripped interpolants
(2-8, median ~4, against a whole-cache median of 5), so if it is the 00i class it is a
different manifestation rather than a recurrence.

**The next measurement**: identify WHICH of the 17 draws the sign — `CZ_VK_DRAW_ID` on a
frame from the far viewpoint — then read our translated SPIR-V for that shader against the
capture's own disassembly of it. That step was named in part 27 for the white-surface class
and has still never been taken; it is now owed twice.

Far/near captures: `~/DR2CZ-troubleshooting/play/play_0819_1626/capture_f12174.census`
(far, broken) and `capture_f12526.census` (near, fine).

---

## 4. WHAT PART 56 CHANGED IN THE RENDERER

* **the stencil test**, from nothing. `CZ_VK_NO_STENCIL=1` is the control arm.
* **the polygon offset**, in the PipelineKey (not dynamic state). `CZ_VK_NO_POLY_OFFSET=1`
  is the control arm; `CZ_VK_POLY_OFFSET_SCALE=N` tunes it without a rebuild.
* **`F8` — the burst recorder**: every presented frame for a second into `CZ_BURST_DUMP`,
  with `tools/burst_read.py` to read it. `SAFE=1 tools/play_session.sh` arms god mode,
  no-death and ZOMBIES IGNORE ALL HUMANS so a capture session is not a fight.
* **the draw census gained `po=`, `su=`, `dc=` and `sr=`** — the polygon offset,
  PA_SU_SC_MODE_CNTL, RB_DEPTHCONTROL and RB_STENCILREFMASK per draw. Those four fields are
  what made both register layouts confirmable; keep them.

---

## 5. THE METHOD LESSONS FROM PART 56, because three of them cost real time

1. **VULKAN VALIDATION CAUGHT THREE DEFECTS THAT NOTHING ELSE COULD** — not a gate, not the
   picture, not a counter. Run `CZ_VK_VALIDATION=1` on the outdoor route after ANY pipeline
   or state change. The standing baseline is **6 `topology-08773`** (it was 20+6 before
   part 56).
2. **AN ARRAY AND A SEPARATELY-WRITTEN COUNT WILL DRIFT.** `dsi.dynamicStateCount` was
   hardcoded to 3 while the array had grown to 4, so a feature was inert and NOTHING
   reported it — the picture was unaffected and every gate passed.
3. **READ THE VALIDATION MESSAGE, DO NOT GUESS THE VUID.** Two rounds were spent fixing the
   opposite of what `VUID-vkCmdDraw-None-08608` says; its text states it plainly.
4. **A HYPOTHESIS THAT FITS EVERY SYMPTOM SHOULD BE TESTED FIRST AND ADVOCATED SECOND.** The
   mip story explained all four defects elegantly and was wrong.
5. **THE OPERATOR'S OFFHAND SENTENCES KILLED BOTH WRONG HYPOTHESES** — *"if you do not move
   the camera"* and *"when they are not floating they flicker"*. Ask what a defect does over
   TIME and under WHICH CONDITIONS before building anything.
6. **A POSITIVE CONTROL BEFORE A FIX IS BELIEVED.** The polygon offset shipped without one;
   the 10,000x arm that finally provided it also refuted the fix.
