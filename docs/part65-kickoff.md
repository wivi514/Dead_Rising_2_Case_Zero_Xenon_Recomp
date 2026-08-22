# Part 65 kickoff — RT shadows: the occluder set, then the operator's verdict

> **THIS IS THE LIVE HAND-OFF**, superseding `part64-kickoff.md`. Part 64
> (2026-08-21 night → 2026-08-22) built RT stage 2 end to end and diagnosed the
> one defect standing between it and an operator session. The record with every
> number is `phase5-notes.md` §6cv; the backlog entry is `open-items.md` 0v; the
> arms are in `instruments.md`; the transferable lessons are gotchas 381-383.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part
> 59). Part 64's runs were headless MEASUREMENT — luma statistics, atlas dumps,
> occlusion queries — which is the part-61/62 practice for instruments. **The
> LOOK verdict has not been asked for.** Part 64 closed with the arm at 72.00
> outdoor median luma against OG's 80.61, having started at 63.7 next to an
> all-shadow floor of 61.2 — so it is no longer shadowing the world against
> itself, but whether the remaining 8.6 is correct added shadowing or residual
> defect is exactly the question §1 item 0 settles cheaply first. Do that, then
> ask.

## 0. Read these first, and do not re-measure them

`phase5-notes.md` §6cv is the whole record. The facts it closes:

* **The injection point is the shadow ATLAS SNAPSHOT, and it needs no shader
  patch.** `CZ_VK_SHADOW_FILL=0.0` → outdoor median luma 80.61 → 61.43;
  `=1.0` → 81.46. Two polarities, opposite directions.
* **The convention is STANDARD** (near = occluder). `CZ_VK_RT_INVERT` exists for
  the other answer and is not needed here.
* **The trace pipeline's writes reach the title's shadow term.**
  `CZ_VK_RT_POISON=1` reads 61.18 against the direct fill's 61.43 — the same
  measured extreme through the whole BLAS/TLAS/ray-query path.
* **The plumbing engages and holds**: ~1,400 BLASes, ~33 MB, zero pool flushes,
  ~370-530 TLAS instances/frame, zero key collisions, zero unreadable positions,
  zero refused endians, and a traced atlas that is recognizably a shadow map.
* **Two real but SECONDARY causes of the over-shadowing, both measured and both
  smaller than they looked**: junk-coordinate streams at ±6.3M units (gated;
  worth 63.72 → 63.71, i.e. nothing measurable — hygiene, not a fix), and
  structurally, **the title's own cascade is 52.8% EMPTY because it draws
  CASTERS, not the world** while we trace everything the camera sees (worth
  63.71 → 66.14 via `CZ_VK_RT_CASTERS=cascade`). Both are recorded because a
  mechanism that is real and immaterial is worth as much to the next session as
  one that is real and decisive — it stops the item being re-bought.

* **The largest term was found and fixed at part 64's close: the light matrix
  was bound by RECENCY and that binding is false.** A distinctness count read
  **0 slices with one c0-3 and 28,704 with several** — the cascade pass is not a
  single-matrix pass. Binding by DATAFLOW (capture only from cascade draws of
  streams the SCENE pass has vouched for as world-space) moved the arm
  **63.71 → 72.00** outdoor median luma, coverage **86.3% → 61.3%**, accepting
  1,293,758 draws and rejecting 849,840 object-transform ones.
  `CZ_VK_RT_ANY_MATRIX=1` is the control arm. In the same run the matrix inverse
  self-check cleared its hypothesis (2.38e-07), so that candidate is closed too.

## 1. The work, in order

0. **READ ONE NUMBER FIRST — it decides which fix you build.** The `[rt]` line's
   `slice matrix (WORLD-VOUCHED ONLY)` count now runs after the dataflow filter
   and prints the mean distinct count. **ONE** distinct matrix per slice means
   the selection is exact and the residual (72.00 against OG's 80.61) is
   elsewhere — the surviving suspects are the depth convention and the slice
   rectangle. **FOUR** means the title renders all four cascades before resolving
   any of them: every matrix is legitimate, one per cascade, and what is left is
   the slice↔matrix PAIRING, whose fix is different in kind (associate each
   captured matrix with the resolve that follows the draws it belongs to, in
   order, rather than by recency). Part 64 queued a run for this and it may
   already be in `phase5-notes.md` §6cv 7d.

1. **The three statistics that judge this arm, in this order** (all are in §6cv
   with their part-64 values, so the job is to re-read them after any change):
   * `CZ_VK_RT_COVERAGE=1`'s won-fraction. **61.3%** after the binding fix, from
     86.3% before it. The caster arm was measured and helps only a little
     (86.4% / 66.14 luma) — the occluder set is real but was never the largest
     term, which is why §6cv §6's second reading turned out to be the right one.
   * the atlas diff — `CZ_VK_SNAP_DUMP` on an RT run and an OG run at the same
     stationary camp, then convert the stretched greys back through the printed
     24-bit range. **Report BOTH tails** (gotcha 382): today nearer 49.6% /
     farther 1.3%.
   * outdoor median `meanLuma` against OG's **80.61**; the arm is at **72.00**
     today. Expect RT to sit somewhat BELOW OG — real added occluders darken a
     scene — but nowhere near the all-shadow floor of **61.2-61.4**. Whether
     72.00 is already "correct with added shadows" or still over-shadowed is a
     LOOK question, and it is the first thing worth an operator's eye once the
     number above says the selection is exact.
2. **Re-price the bias.** `CZ_VK_RT_BIAS=0.05` was worth +10 luma on the BROKEN
   build (63.72 → 73.89) purely by hiding a wrong transform; that reading is
   retired. On the fixed build the default 0.0015 has never been swept. Sweep it
   against stills, not luma alone — a bias large enough to move luma is large
   enough to detach shadows from their casters, and only a picture shows that.
   `CZ_VK_RT_CASTERS=cascade` should also be re-measured on the fixed build: its
   +2.4 luma was measured under the false binding and may be worth more or
   nothing now.
3. **Then, and only then, the operator session.** Ask for OG vs RT LOW
   side by side; fold in **the shadow Low-vs-High LOOK verdict owed since part
   60**, which the same session can judge. Wire `CZ_VK_PROFILE` into the launch
   (their frame is ~2x the headless one) and read the `rt` phase.
4. **Price the tier ladder before promising MED/HIGH.** The panel refuses them at
   the setter today, which is the honest state; MED/HIGH need a measured ms cost
   on the operator's machine first (plan §6's rule).

## 2. The decision this part should make consciously

Route (a) — what part 64 built — traces INTO the 4096x1024 atlas, so **its
ceiling is the atlas's resolution**: exact depths and missing occluders, never
soft or per-pixel shadows. The plan's route (b) — patch the ~dozen
shadow-sampling PS to read a screen-space traced factor — is where a quality
tier actually lives, and **every piece part 64 built is reusable by it
unchanged** (BLAS, TLAS, the sun-matrix capture, the arms, the profiler phase).

Decide (a)-vs-(b) on the fixed build's numbers and the operator's verdict, not on
effort already spent. A defensible outcome of part 65 is "(a) is correct and
cheap, ship it as LOW, and (b) becomes MED/HIGH" — and so is "(a)'s ceiling is
too low to be worth a row; (b) is the tier".

## 3. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**, filed
  in `perf-state-parked.md` (part 62). If the operator asks for it, THAT is the
  part's work instead.
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* Suspected input leak at panel open (one Resolution-row write per open).
* Performance PARKED (`perf-state-parked.md`).
* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1`.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Live-resolution switch parked; point-list PointSize VUID class named, cheap,
  unowned — note it is now the ONLY validation class the RT arm leaves behind
  (part 64 fixed the two its first validation run caught).
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 4. Practical notes from part 64's session

* **`/tmp` is a tmpfs and `CZ_VK_FRAME_DUMP` at 3440x1440 is ~15 MB a frame.**
  A three-arm campaign with dumps every 96 frames filled it and killed the shell
  mid-part. Dump to `~/DR2CZ-troubleshooting/`, or read `meanLuma` out of
  `CZ_VK_FRAME_STATS` — which is already per-frame, already outdoor-filterable
  by draw count, and costs nothing extra.
* **The atlas is `1439B000`**, dumped at the internal resolution (11008x2048 at
  3440x1440). `CZ_VK_SNAP_DUMP` writes it with the 24-bit range in the log line;
  that range is what makes the greys convertible back to depth.
* The RT arms are all headless-safe and all off by default. `CZ_VK_RT=0` creates
  the pre-part-64 device exactly, which is the arm to quote against.
