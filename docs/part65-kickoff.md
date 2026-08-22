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
> LOOK verdict has not been asked for and must not be asked for until the
> occluder set is fixed**, because today the arm shadows the world against
> itself and an operator session would only re-report that.

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
* **The cause of the over-shadowing, in two parts, both measured**:
  junk-coordinate streams at ±6.3M units (gated in part 64,
  `bounds=` counter, 66,095 rejections in one run), and — structurally — **the
  title's own cascade is 52.8% EMPTY because it draws CASTERS, not the world**,
  while we trace everything the camera sees. Filling that emptiness converts lit
  regions to shadowed ones; 15.8 points of the atlas changed meaning that way.

## 1. The work, in order

1. **Read the two runs part 64 queued at close** — `CZ_VK_RT_CASTERS=cascade`
   (the fix candidate) and the bounds-gated default. Their numbers belong in
   §6cv. The three statistics that matter, in this order:
   * `CZ_VK_RT_COVERAGE=1`'s won-fraction. Today it is **86.3%**. If the caster
     arm does not take it well under 20%, the occluder set is not the whole
     story and §6cv §6's second reading applies (our ray disagrees with our own
     rasterizer about geometry both agree on).
   * the atlas diff — `CZ_VK_SNAP_DUMP` on an RT run and an OG run at the same
     stationary camp, then convert the stretched greys back through the printed
     24-bit range. **Report BOTH tails** (gotcha 382): today nearer 49.6% /
     farther 1.3%.
   * outdoor median `meanLuma` against OG's **80.61**. Expect RT to sit somewhat
     BELOW it — real added occluders darken a scene — but nowhere near the
     all-shadow floor of **61.2-61.4**.
2. **If the caster arm works, make it the default** and re-price the bias: the
   0.05 that half-recovered the broken build is a peter-panning bias and should
   come back down once the map is right. Sweep it against stills, not luma alone.
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

Decide (a)-vs-(b) on the caster arm's numbers and the operator's verdict, not on
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
