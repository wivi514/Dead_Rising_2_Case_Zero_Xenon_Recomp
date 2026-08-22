# Part 65 kickoff — RT shadows, ROUTE (B): a screen-space traced factor

> **THIS IS THE LIVE HAND-OFF**, superseding `part64-kickoff.md`. Part 64
> (2026-08-21 night → 2026-08-22) built RT stage 2 end to end on **route (a)**
> (write depths into the title's own cascade atlas), proved the injection point,
> put it in front of the operator twice, and **concluded by measurement that
> route (a) cannot produce correct shadows**. Part 65's subject, set by the
> operator, is **route (b)**.
>
> The full record is `phase5-notes.md` §6cv — read **§7j first, it is the
> verdict**, then §7f (the operator's session) and §7e (a retraction that
> matters). The backlog entry is `open-items.md` 0v; the arms are in
> `instruments.md`; the transferable lessons are gotchas 381-386.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part
> 59). Part 64's two operator sessions are why this part exists at all: their
> eleven-word description named a mechanism three headless statistics had missed.

---

## 0. Why route (a) was abandoned — do not re-derive any of this

Route (a) writes depths into the 4096x1024 shadow atlas the title samples, so
the title's own comparison applies them. It works as a MECHANISM and that is
proven, not assumed:

* **The injection point is real.** `CZ_VK_SHADOW_FILL=0.0` takes the outdoor
  median luma 80.61 → 61.43 and `=1.0` reads 81.46 — two polarities, opposite
  directions, ~11k outdoor frames an arm. No shader patch is needed to make the
  title's shadow term read what we write, and the convention is STANDARD
  (near = occluder).
* **The whole ray path reaches that term.** `CZ_VK_RT_POISON=1` (all-shadow
  written by the trace pipeline) reads 61.18 against the direct fill's 61.43 —
  the same measured extreme, through BLAS + TLAS + ray query + composite.
* **The plumbing engages and holds**: ~1,400 BLASes, ~33 MB, zero pool flushes,
  ~200-530 TLAS instances/frame, zero key collisions, zero unreadable positions,
  zero refused endians.

**And it cannot be made correct.** The operator's report — *"pretty much like
shadow squares following where the player is and normal shadow still on"* —
named the mechanism: **self-shadowing**. Writing the MAP means every receiver
inside the map is compared against itself and comes out occluded, and route (a)
has no receiver-side offset to apply. Five independent knobs were then tried and
**the frame does not care about any of them**:

| what was changed | median meanLuma (n) |
|---|---|
| OG | **80.61** (11,243) |
| union, camera's world set | 66.34 (6,484) |
| union, the title's own casters | 66.14 (10,991) |
| union, dataflow-bound light matrix | 66.40 (6,550) |
| replace, casters, bias 0.0015 | 65.98 (11,211) / 64.35 (9,381) |
| replace, casters, bias 0.01 (6.7x) | 64.79 (9,063) |
| all-shadow floor (poison / fill) | 61.18 (10,865) |

A 6.7-fold bias change moved 0.44 luma against a 16-luma gap. Stills say the
same thing in one glance: the white vans and the quarantine bus are bright under
OG and uniformly grey under RT — **every LIT surface shadows itself.**

**Everything route (a) built is reusable by route (b) unchanged**, which is why
none of it is deleted: the BLAS/TLAS construction, the sun-matrix capture and
its dataflow binding, the pooled AS allocator, every arm, and the `rt` profiler
phase.

**One hypothesis was never tested**, recorded so it is not lost rather than
because it is recommended: that a slice's traced content lands in the correct
QUARTER of the atlas but is paired with a different cascade's matrix. That would
look exactly like this and the distinctness counter cannot catch it — it
verifies that ONE matrix governs a slice, not that the matrix BELONGS to that
slice. The test is a per-cascade-index content comparison between our traced
quarter and the raster quarter it replaced. Cheap, and the only reason to
revisit route (a).

---

## 1. Route (b), the spec

**Compute a shadow factor per RECEIVING PIXEL, in screen space, and have the
shadow-sampling pixel shaders read that instead of comparing against the
atlas.** A surface cannot shadow itself when the ray starts at that surface and
is offset along its own normal — the defect above becomes impossible by
construction rather than tuned away. It is also the only route that can ever do
soft or per-pixel shadows: the atlas route is capped at the atlas's resolution
however many rays it fires.

### Step 1 — CENSUS FIRST: which shaders sample the cascade atlas, and where

**Build nothing before this returns a list.** The atlas is resolve destination
`1439B000` (4096x1024 guest; 11008x2048 host at the operator's internal
resolution). Wanted, per pixel-shader hash: does it fetch that address, at which
`tfetchConsts` slot, and on how many draws a frame.

The machinery exists. The sidecars carry `tfetchConsts` and `tfetchDims`
(`tools/shader_dim_census.py` reads them), and the renderer already resolves
each fetch's address per draw in its texture-binding walk — so a counter keyed
on (psHash, slot) when the fetch address matches the atlas is a small diagnostic
arm in the same shape as `CZ_VK_DIM_CENSUS`. **Report counts, not impressions**:
the plan's "~a dozen shaders" is a guess that has never been measured, and this
project has been wrong about a population size before (gotcha 3).

Free cross-check: Xenia's `dump_shaders` writes `.ucode.*` disassembly beside
each blob, so the shadow compare can be READ in the microcode of the named
shaders instead of inferred.

### Step 2 — the factor pass

A compute or fullscreen pass, once the frame's scene depth exists:

* **Reconstruct world position** from the scene depth. The scene camera's
  view-projection is available and already recognized: world draws carry the
  composite P*V at c0-3 (§6cs, `SceneXformForm` form 2), and part 64 shipped an
  `Invert4x4` whose self-check reads `|M·M⁻¹ − I| = 2.38e-07` and logs on
  success as well as failure.
* **Sun direction** from the cascade's light matrix — captured and DATAFLOW-BOUND
  in part 64 (`g_lightM`). Do NOT use last-write-wins: that binding was refuted
  outright (0 slices carried one c0-3, 28,704 carried several; half the cascade
  pass's ortho-shaped matrices are per-object composites). The fix uses the scene
  pass as the oracle — a stream the scene pass draws world-space vouches for the
  matrix a cascade draw of the same bytes carries.
* **Ray-query toward the sun** from position + a normal-offset origin, write a
  factor texture. Tiers become expressible here, which is the whole point: LOW =
  half-res, 1 ray, temporal only; MED = full-res, 1 ray, bilateral filter;
  HIGH = cone-sampled sun radius, 2-4 rays, i.e. genuinely soft.

### Step 3 — the injection: a shader VARIANT CACHE

This port already has the mechanism AND the precedent, so do not invent one.
`CZ_DXC_DEFINES` builds a second SPIR-V cache and `CZ_SHADER_SPV` selects it;
the part-33 NaN family (`XE_FLOOR_IS_NAN`, `XE_NAN_IN_PAINT`,
`XE_NAN_VS_KILL_IN`) and `assets/shader_spv_pre45` are all this shape. Build
lines and readings are in `docs/xenonrecomp-upstream-bugs.md`.

* Build a variant cache in which the named shaders' atlas compare is replaced by
  a load from our factor image at `SV_Position.xy`.
* The runtime binds the factor image and selects the variant cache only while an
  RT tier is active, so the stock cache and the default path are untouched.
* **The null must be byte-identical**: with RT off the run uses the stock cache
  and must be instruction-path identical to today.

### Gates and arms

* `CZ_VK_RT=0` stays the master arm (device created exactly as pre-part-64).
* A poisoned factor (all-black) must darken the frame — the `CZ_VK_CUBE_POISON`
  pattern, and part 64's poison landed on the fill's number to 0.25 luma, so
  this control is known capable of firing.
* **An engagement counter on the factor pass, re-checked after every change that
  COMBINES two others** (gotcha 386: part 64 shipped a build measuring 80.61
  against a control's 80.61 — a flawless-looking fix that was the feature
  silently switched off, caught only by an engagement line reading zero).
* Validation with the RT extensions on; the PM4 oracles untouched; `--smoke`;
  A5 exit 0; `no translated shader` = 0; `shader_dim_census.py` clean.

---

## 2. Measurement discipline this part must not repeat

Part 64 lost an hour to two mistakes, both now gotchas:

* **384 — read only EXITED processes.** `CZ_VK_FRAME_STATS` is appended to while
  the run continues and the `[rt]` counters are cumulative. On these routes a
  partial read measures a DIFFERENT PLACE in the level, not a noisier version of
  the same one: the same arm read 63.71 at 4,663 outdoor frames and 66.34 at
  6,484; another 72.00 at 1,212 and 66.40 complete; coverage ~86% early and
  52.8% at exit. Three "findings" were built on partials and all three
  dissolved. Gate every read on a `done` flag the runner writes (not `pgrep`,
  which races the next queued run), quote the frame COUNT beside every median,
  and run the control to the same depth.
* **385/386 — a proven mechanism is not a proven arm**, and a number that
  matches the control EXACTLY is inertness until a counter says otherwise.

And the positive lesson, which is why this part exists: **on a question about
SHAPE, or about WHICH SURFACES, the operator's eye is not a slower instrument —
it is the only one.** Three statistics agreed the frame was 14 luma dark and not
one of them could say the white vans had gone grey.

---

## 3. The settings row, as the operator specified it

Shipped in part 64 and unchanged by route (b): **ONE row**, `SHADOW`, values
`LOW / MEDIUM / HIGH / RT LOW / RT MEDIUM / RT HIGH`. Values 0-2 are the raster
tiers with RT off; 3-5 select an RT tier which **replaces** the raster shadow.
The raster tier is remembered while an RT value is selected, so stepping back
down returns the quality the player had. On a device without ray query the
ladder stops at HIGH and the footer says why.

Their words: *"for shadow we'll have the normal settings and then RT low, rt
medium and rt high in the same settings since normal shadow would be removed to
be replaced by the RT shadow if a rt settings is selected."*

**The trade replacement creates is what gives MED/HIGH something real to be**:
anything not in the TLAS casts no shadow — skinned actors (zombies, Chuck) and
alpha-tested foliage. On route (b) the tiers can buy that back (MED adds the
deformed streams, HIGH adds skinned actors with per-frame BLAS rebuilds) as well
as buying softness. Do not offer a rung before it is priced in ms on the
operator's machine (plan §6).

---

## 4. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**, filed
  in `perf-state-parked.md` (part 62). If the operator asks for it, THAT is the
  part's work instead.
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict — fold it into the
  first operator session that judges an RT tier.
* Suspected input leak at panel open (one Resolution-row write per open).
* Performance PARKED (`perf-state-parked.md`).
* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1`.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Live-resolution switch parked; the point-list PointSize VUID class is now the
  ONLY validation class the RT work leaves behind (part 64 fixed the two its
  first validation run named).
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 5. Practical notes from part 64's session

* **`/tmp` is a tmpfs and a 3440x1440 frame dump is ~15 MB.** A three-arm
  campaign with dumps filled it and killed the shell mid-part. Dump to
  `~/DR2CZ-troubleshooting/`, or read `meanLuma` out of `CZ_VK_FRAME_STATS`,
  which is per-frame and outdoor-filterable by draw count at no extra cost.
* The shadow atlas is resolve destination **`1439B000`**; `CZ_VK_SNAP_DUMP`
  writes it with its 24-bit range on the log line, and that range is what makes
  the contrast-stretched greys convertible back to depth (gotcha 383).
* Part 64's stills and atlas dumps are preserved in
  `~/DR2CZ-troubleshooting/part64-bias/` and `part64-fill-experiment/`.
* The operator launch script is `~/DR2CZ-troubleshooting/launch-part64-rt.sh` —
  note it deliberately does NOT set `CZ_VK_RT_SHADOWS`, because the env arm wins
  over the panel row and would freeze the live toggle the session depends on.
