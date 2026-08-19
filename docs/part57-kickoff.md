# Part 57 kickoff — finish the picture defects the operator found in part 56

> **THIS IS THE LIVE HAND-OFF.** Part 56 was a picture session driven end to end by operator
> captures. It **implemented the stencil test**, **proved the mechanism behind the remaining
> half of the slicing defect**, and **refuted two of its own hypotheses** about the others.
>
> **THE ORDER BELOW IS THE OPERATOR'S, NOT MINE.** Closing part 56 they said of the zombie
> slicing: *"place it at the end because it's the least important visual issue"* — so it is
> §3, even though it is the one with a proven cause and a ready plan. Do not reorder on the
> grounds that it is the most tractable; tractability is not priority.
>
> Performance is PARKED in `docs/perf-state-parked.md` and is not this part's subject.

---

## 0. THE DEFECTS, in the operator's priority order

| # | defect | state | first action |
|---|---|---|---|
| 1 | GAS sign / canopy / bunting wrong at distance | open; mips REFUTED; their own steer is item 00i | identify which of the 17 far-only shaders draws the sign |
| 2 | decal flicker under camera motion | **UNEXPLAINED** — depth and mips both refuted | build the per-draw census inside F8's burst |
| 3 | zombie slicing — bodies whole and doubled | **cause PROVEN: user clip planes; we implement none** | one shader, one plane, on a sliced zombie |
| — | zombie slicing — the "square blood" cap | **FIXED** by the stencil test | nothing |
| — | polygon offset | correct emulation; fixes nothing reported | leave it |

---

## 1. THE DISTANCE DEFECTS — the operator's own steer, and a new fact

Their steer: *"For the gas sign, rooftop and all this type of thing that are at a distance I
am pretty sure it is similar to our last issue that was like that"* — i.e. **item 00i**,
which part 45 traced to OUR translation dropping pixel-shader interpolants, leaving 217
shaders sampling diffuse at ONE TEXEL. A single texel is a flat colour and a dark one is
BLACK, which is the symptom exactly: the sign's letters do not blur at distance, they are
REPLACED by black with a magenta/yellow/cyan fringe. The canopy fascia goes red -> black in
DISCRETE STEPS, and the bunting's pennants vanish leaving their tops as a regularly spaced
dashed line (so the geometry is drawn and nearly every texel is discarded).

**THE NEW FACT, from the captures**: **17 pixel shaders appear only in the far/broken frame
and 9 only in the near/fine one.** This title swaps pixel shaders BY DISTANCE — the far view
is different CODE, not the same shader with a smaller texture. That is consistent with mips
having been ruled out, and it makes "the defect lives in a distance-only shader" cheap to
test. Their metadata shows no obviously stripped interpolants (2-8, median ~4 against a
whole-cache median of 5), so if this is the 00i class it is a different manifestation rather
than a recurrence.

**REFUTED, do not re-buy**: the mip chain. `CZ_VK_NO_MIPS=1` **engaged** (mip-rejection
lines 8 -> 0) and the operator's verdict was *"gas is still black and everything still is
like before"*. The plumbing was checked afterwards and is correct (images and views both
carry the full level count; 7,088 chains uploaded).

**First action**: `CZ_VK_DRAW_ID` on a frame from the far viewpoint to identify WHICH of the
17 draws the sign, then read our translated SPIR-V for that shader against the capture's own
disassembly. That step was named in part 27 for the white-surface class and has never been
taken; it is now owed twice.

Captures: `~/DR2CZ-troubleshooting/play/play_0819_1626/capture_f12174.census` (far, broken)
and `capture_f12526.census` (near, fine).

---

## 2. THE DECAL FLICKER — unexplained, and the instrument to build FIRST

**All that is known is constraint, not cause:**
* flickers ONLY while the camera moves; with a still camera it is stable — *"the decals
  either stay or is not there"*. The single most diagnostic thing the operator said.
* **NOT z-fighting.** `CZ_VK_POLY_OFFSET_SCALE=10000` visibly lifts decals in front of walls
  and zombies, and their verdict at that setting was *"when they are not floating they
  flicker"*.
* **NOT bad mip data**, per §1's refutation.
* the decals are identified beyond doubt in the census: single-quad (`verts=6`),
  alpha-blended (`blend=07060706`), carrying `po=0/-4.8/-3e-05` or `-1.6/-1e-05`.

**BUILD THIS FIRST: `F8`'s burst records pixels and per-frame fingerprints but NO PER-DRAW
CENSUS.** So a decal seen blinking could not be asked whether its draw was issued that frame,
which is exactly what stopped part 56's analysis. Arm the draw census for the burst's frames
(or every Nth) and the question "missing, or issued and then discarded" is answerable in one
press. Note the camera MUST move for this defect to appear, so `burst_read.py` will refuse to
conclude from the draw list — the per-draw census is what replaces that.

---

## 3. ZOMBIE SLICING — LAST, at the operator's instruction, though the cause is proven

*"Place it at the end because it's the least important visual issue."*

**The defect split in two** once they clarified: *"they are still full zombies and they just
get a double instead of slicing in two different part"*. The stencil test fixed the
cross-section CAP; the BODIES are clipped by something else. Treating one symptom as proof
of a single cause was part 56's error.

**The second mechanism is USER CLIP PLANES, and the address hunt is worth reading as a
method lesson** — it took three captures because the address was guessed twice before it was
measured:

* `PA_CL_CLIP_CNTL` at **0x2204** — CONFIRMED, `cl=00080001` on **312 of ~2,480 draws**.
* planes at 0x2240 — **guessed, refuted**: every draw read `0/0/0/0` while 312 enabled one.
* a scan of 0x2115..0x2140 — **guessed, refuted**: `ucp=none` across three captures.
* **the whole register file dumped at the first clip-enabled draw** — one capture, and it
  holds exactly one plane-shaped quad:

      2388 -2.10425e-05   2389 0.362499   238A -29.4662   238B 29.078

  a normal of about (0, 0.012, −1.0) at distance 0.987 normalised, just past the
  polygon-offset block at 0x2380..0x2383 confirmed the same way. **`kPaClUcp0X = 0x2388`.**

**Dumping and looking for something SHAPED like a plane found it in one capture; proposing
addresses found nothing twice.** Do that first next time.

**What the confirming capture shows, with its caveat.** Eight distinct planes on
clip-enabled draws, in **two families with opposite z/w signs** — the shape a cut produces.
**But the pairing is NOT exact**: in the best pair z and w negate almost perfectly while x
and y do not —

    (-0.087, -0.343, -24.264,  23.791)  x48
    ( 0.628,  0.043,  24.284, -23.858)  x16

so "the same plane with opposite signs" is **not** established and the implementation must
not assume it. Eight planes is a good fit for four sliced bodies, but that is inference.

**WHY THIS IS THE BIGGEST JOB IN THE LIST.** Vulkan has no fixed-function user clip planes;
it clips against `ClipDistance` written by the VERTEX SHADER. So this cannot be done in
renderer state — the translated SPIR-V must compute and export a distance per enabled plane,
which means patching the shader cache or teaching XenosRecomp to emit it. `ClipDistance`
appears NOWHERE in this renderer or in Fable 2's, so it is a shared gap.

**Suggested order**: one shader emitting one clip distance, proven on a sliced zombie, before
generalising to six planes and 439 shaders.

---

## 4. WHAT PART 56 CHANGED IN THE RENDERER

* **the stencil test**, from nothing — `stencilTestEnable` had appeared zero times while
  RB_DEPTHCONTROL bit 0 is `stencil_enable`, and **350 of 1980 draws** enable it.
  `CZ_VK_NO_STENCIL=1` is the control arm.
* **the polygon offset**, in the PipelineKey (not dynamic state). `CZ_VK_NO_POLY_OFFSET=1`;
  `CZ_VK_POLY_OFFSET_SCALE=N` tunes it without a rebuild.
* **`F8` — the burst recorder**: every presented frame for a second into `CZ_BURST_DUMP`,
  read with `tools/burst_read.py`. `SAFE=1 tools/play_session.sh` holds god mode, no-death
  and ZOMBIES IGNORE ALL HUMANS so a capture session is not a fight.
* **the draw census gained `po=`, `su=`, `dc=`, `sr=`, `cl=`, `ucp=`** — the fields that made
  three register layouts confirmable. Keep them.
* **a clip-draw register dump**: the whole register file at the first draw enabling a clip
  plane, written when `CZ_CAPTURE_KEY` is armed.

---

## 5. THE METHOD LESSONS FROM PART 56 — three of them cost real time

1. **DUMP AND LOOK FOR THE SHAPE; DO NOT PROPOSE ADDRESSES.** Two guesses at the clip-plane
   register found nothing and cost a capture each. One register-file dump found it.
2. **VULKAN VALIDATION CAUGHT THREE DEFECTS NOTHING ELSE COULD** — not a gate, not the
   picture, not a counter. Run `CZ_VK_VALIDATION=1` on the outdoor route after ANY pipeline
   or state change. Baseline is **6 `topology-08773`** (was 20+6 before part 56).
3. **AN ARRAY AND A SEPARATELY-WRITTEN COUNT WILL DRIFT.** `dsi.dynamicStateCount` was
   hardcoded to 3 while the array had grown to 4, so a feature was inert, every gate passed,
   and driver leniency produced a positive control that appeared to pass (gotcha 366).
4. **READ THE VALIDATION MESSAGE; DO NOT GUESS THE VUID** (gotcha 367). Two rounds were
   spent fixing the opposite of what 08608 says in its own text.
5. **A HYPOTHESIS THAT FITS EVERY SYMPTOM SHOULD BE TESTED FIRST AND ADVOCATED SECOND.** The
   mip story explained all four defects elegantly and was wrong.
6. **THE OPERATOR'S OFFHAND SENTENCES KILLED BOTH WRONG HYPOTHESES AND SPLIT THE THIRD
   DEFECT.** *"If you do not move the camera"*, *"when they are not floating they flicker"*,
   *"they just get a double"*. Ask what a defect does over TIME and under WHICH CONDITIONS
   before building anything.
7. **A POSITIVE CONTROL BEFORE A FIX IS BELIEVED.** The polygon offset shipped without one.

---

## 6. GATES AT CLOSE — ALL CLEAN, run against the final binary

`--smoke`; the switch gate (0 defects); the dimension census (0 disagreements); both PM4
oracles on B1; `no translated shader` = 0; **E3 best of five +0.8472 with 4 of 5 agreeing on
layout**; and **Vulkan validation 6 `topology-08773` and nothing else**, against a standing
baseline of 20+6 — the stencil work removed 20 `Input-08733` as a side effect.

Nothing is owed.
