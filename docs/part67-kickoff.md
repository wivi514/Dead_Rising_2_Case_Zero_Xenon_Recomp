# Part 67 kickoff — RT shadows, route (b): the gate is built, the picture is unseen

> **THIS IS THE LIVE HAND-OFF**, superseding `part66-kickoff.md`. Part 66 answered
> that kickoff's central question and **refuted its central suspect**, without an
> instrumented run: the factor pass was reading a depth buffer at its clear value,
> because **this title has no scene Z prepass**. The receiver now comes from a
> primary ray, which cannot be too early.
>
> **Part 67's whole job is the operator session, and it already exists:**
> `tools/part66_operator_session.sh`. Arm 1 is a GATE and the rest are meaningless
> without it.
>
> The record is `phase5-notes.md` **§6cx**; the backlog entry is `open-items.md` 0v;
> the arms are in `instruments.md`; the lessons are gotchas **391-393**. §6cw (route
> (b) built) and §6cv (route (a), closed) are history — read §6cx first.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction), and
> the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

---

## 0. WHAT PART 66 ESTABLISHED, so it is not re-derived

### The defect, and it is not what part 66's kickoff said it was

The part-66 hand-off's §0 named **the pass's texture bindings** as the live suspect,
on two readings: the depth sampled as exactly 1.0, and the colour buffer sampled as
exactly black. Both are now accounted for and neither implicates a binding.

* **The depth reading was honest and the buffer really was empty.**
  `tools/rt_depth_order_census.py` walks all twenty `.xtr` world traces in stream
  order. The 233,155 "depth-only draws" the trigger was built on are the **shadow
  cascade** (EDRAM depth base 0, pitch 1040, `RB_MODECONTROL` 5, colour mask 0, ~969
  a frame). The scene pass (base 736, pitch 640) has **no prepass**: in every trace
  its FIRST draw already samples the cascade atlas, with **0 depth-writing draws
  before it and ~5,200 (2.0M verts) after it**. The depth buffer is at its clear
  value whenever the factor pass fires, and no trigger could have changed that.
* **The colour reading is RETRACTED.** `g_colour` was never bound — a three-element
  `VkWriteDescriptorSet` array passed with a count of two — and the validation layer
  reported it on the first draw of every RT run since the feature was written.
  Modes 12 and 13 were reading an unwritten descriptor. (Fixed. And note there was
  an innocent reading available too: the pass fires at the first scene draw, when the
  colour buffer genuinely is still at its black clear.)

### The fix, shipped

The receiver is the closest hit of a **PRIMARY RAY** from the camera through the pixel
into the same TLAS the shadow ray uses. The TLAS is built from the PREVIOUS frame's
draws (`rtshadow::g_prevKeys`), so it is populated whatever the title's draw order is.
On route (b) the collector was already using the camera's world set rather than the
cascade casters, which is exactly what a primary ray needs — nothing had to change
there. The per-resolve invalidation is gone with the depth dependency: **3.01 passes
a frame become 1**.

**Its known cost, and it is a real one:** the TLAS holds only opaque depth-writing
draws, so a pixel covered by a skinned actor (zombies, Chuck — structurally 3.0% of
scene draws) or by alpha-tested foliage receives the factor of the opaque surface
BEHIND it. Those meshes already cast no RT shadow, so the tier is consistent with
itself, and per-frame BLAS rebuilds for skinned meshes buys back both halves at once —
that is the MED/HIGH feature §2 of the last kickoff priced. **Whether it looks bad is
arm 3's question and only the operator can answer it.**

### Also fixed, and free

* The descriptor set was rewritten with this frame's TLAS while the RECORDING command
  buffer still held it ("updated without UPDATE_AFTER_BIND"), which the validation
  layer followed with the whole `commandBuffer-recording` cascade. Skipped now when
  the handle has not changed.
* **The collector census prints on route (b).** `[rt] collector: ... skips: ...`
  printed only from `TraceSlice` — route (a)'s path — so the live route could not see
  how much of the world entered the TLAS. With a primary ray that number is
  load-bearing: absent world reads as SKY, i.e. LIT, which in the frame is
  indistinguishable from "the rays are broken". **If arm 1 fails, this line is the
  first thing to read.**

---

## 1. THE SESSION — `tools/part66_operator_session.sh`

Seven arms, chained (quitting one starts the next). ~30-40 s of play each: get
outdoors, `F4` → SHADOW → the tier named, look around, `F9`, quit.

| arm | what | PASS |
|---|---|---|
| 0 | the A5 kernel gate, headless | exit 0, nothing to do |
| **1** | **GATE — `CZ_VK_RT_FACTOR_DEBUG=17`, does the primary ray find the world?** | **the WORLD IS BLACK, the SKY is lit.** Anything else: STOP |
| 2 | `=18`, how far is each hit | a smooth distance gradient — separates "hit something" from "hit the right thing" |
| 3 | **the picture, RT LOW** | shape questions below |
| 4 | RT HIGH — four rays over the sun's disc | as arm 3, plus: is the penumbra visible, is the five-level banding objectionable? |
| 5 | `CZ_VK_RT_FACTOR_SOURCE=depth` — the control | **no shadows**, which confirms the census from the picture |
| 6 | RT OFF, the shipped default | the side-by-side, plus the raster LOW-vs-HIGH LOOK verdict owed since part 60 |

**The shape questions for arms 3 and 4** — a number is blind to all four:

* do LIT surfaces stay lit? Route (a) died by greying every one of them.
* do shadows sit UNDER their casters, or float away from them?
* is there acne — speckle or stripes on surfaces that should be clean?
  `CZ_VK_RT_FACTOR_BIAS` / `CZ_VK_RT_FACTOR_CAMBIAS` are the knobs, both defaulting
  from the cascade's own depth extent.
* how bad does the missing-actor hole look — shadows crawling on zombies and foliage
  from the ground behind them?

**Read the arms with `tools/part65_luma_read.py $OUT --control fs_off.txt`** (the
script runs it at the end). Calibration: all-lit **99.86**, all-shadow **90.16**. It
REFUSES any arm below 85% of the control's frame count, because part 65 built and
retracted three conclusions on partial reads of these files (gotcha 384).

---

## 2. IF THE GATE FAILS — the order to check, cheapest first

1. **`[rt] collector:` in `gate17.log`.** `prevKeys` and `tlasInst` are how much world
   the rays can hit; the `skips:` columns say where the rest went. A few hundred
   instances against ~5,200 scene draws a frame may be right (most draws are UI,
   particles and transparents) or may be the whole problem — this line is the only
   thing that can say.
2. **`CZ_VK_RT_FACTOR_DEBUG=15`** — `GetDimensions` on the depth binding. Still the
   probe part 66's kickoff asked for and still worth having: it tests the one
   assumption every sampling mode shares. PASS = the frame goes dark.
3. **`CZ_VK_RT_CASTERS=cascade`** — the other occluder set, as an arm.
4. The scene matrix. `[rtb] ... Scene composite: N frames carried ONE, 0 SEVERAL` is
   the binding check and it passed at 2,343/0 in part 65; if it starts reading SEVERAL
   the camera binding has moved.

**Do NOT re-buy** the five causes part 65 killed by their own counters (the sun
direction, the two Z-prepass gates, the world reconstruction, the barrier masks) or
route (a) (`§6cv §7j`).

---

## 3. WHAT ROUTE (B) CAN STILL BUY, if the verdict is good — priced, not promised

* **MED/HIGH: the missing casters.** Skinned actors and alpha-tested foliage. On
  route (b) this now fixes TWO things at once — they would both cast shadows and stop
  receiving the ground's factor. Per-frame BLAS rebuilds for skinned meshes is the
  price and it has never been measured on this title.
* **The five-level ceiling.** The `pcf4` family quantises the factor to five levels
  and the 24 `tap1` uses stay binary. Going past that means patching the compare
  itself, per-shader, at a population of 126 — do not start without a reason from the
  picture.
* **Cost.** `CZ_VK_PROFILE`'s `rt` phase carries the pass, and it should now be a
  third of what part 65 would have measured. Nothing has been measured on the
  operator's machine; performance is PARKED (`perf-state-parked.md`) and an RT tier is
  opt-in, so this is a number for the panel, not a regression to chase.

---

## 4. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**, filed in
  `perf-state-parked.md` (part 62). If the operator asks for it, THAT is the work.
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict — arm 6 asks for it.
* Suspected input leak at panel open (one Resolution-row write per open).
* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1`.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Live-resolution switch parked; the point-list PointSize VUID class is the only
  validation class the RT work leaves behind.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

---

## 5. Measurement discipline this part must not repeat

* **A counter added in the same commit as the thing it measures is usually never
  read** (gotcha 391). Part 65 added the exact instrument that would have named this
  defect — `[rtb] the factor pass fires at draw 831 of ~2480` — and every ladder run
  on disc predates it, so the number reached no document. Re-run the arm that
  motivated a counter **in the same sitting**, and put the number in the hand-off even
  when it agrees with what you already believed.
* **A count over a run is not an order within a frame** (gotcha 392). When a design
  rests on sequence, measure sequence — and a GPU trace preserves the whole frame's
  stream, so that is usually an afternoon offline rather than a session.
* **Read the validation output you collected.** Part 65 ran `CZ_VK_VALIDATION=1`
  three times and the logs named an unbound descriptor and a command-buffer
  invalidation on their first `[rtb]` line. Note that validation slows a run below
  `part65_luma_read.py`'s completeness bar — validate and measure in separate runs.
* `/tmp` is a tmpfs; a 3440x1440 frame dump is ~15 MB. Dump to
  `~/DR2CZ-troubleshooting/`.

---

## 6. Gates at part 66's close

RT off is the shipped default and nothing outside the RT pass changed, so the
carry-overs from parts 62-65 stand.

* `--smoke` OK.
* The generated `rt_factor_spv.h` PS module carries **two `OpRayQueryInitializeKHR`**
  (primary + shadow) and **two `OpImageQuerySize`** (modes 15/16) — the new code
  compiled rather than folding away.
* `tools/rt_depth_order_census.py` runs over 20 traces and prints its own verdict.
* **A5 is arm 0 of the session** rather than something run here, per the standing
  instruction that the game is the operator's.
