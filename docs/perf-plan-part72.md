# The performance plan, part 72 — with a denominator, and an item order built from it

**This supersedes `docs/perf-plan-part71.md` as the live performance plan.** That document
is kept because it was EXECUTED and because it records its own two retractions in place
(§1.4's `CZ_VK_RT=0` and §6's `CZ_VK_WIDE=0`, gotchas 414-415). Its §5 measurement rules
are still correct and are not restated here.

> Read first, in this order: `docs/part72-kickoff.md` (where the port is),
> `phase5-notes.md` **§6dd** (part 71's record, §7-11 are the measurements),
> `docs/perf-state-parked.md` (the pump's symbol table, items A-E, every arm that exists).
> **All runtime verification goes through the operator** (standing instruction).

---

## 0. THE DENOMINATOR, which part 71 established and this plan is priced against

| | |
|---|---|
| the frame at the operator's soak | **28.09 ms / 35.6 fps at ~9,800 draws** |
| internal resolution | **3440x1440** (their `cz_settings.txt`, fov slider +10, 21:9) |
| CPU or GPU? | **CPU. A quarter of the pixels costs −6.8%** (28.09 -> 26.19, n=19/25) |
| the pump's slope | **~2.5 us per submitted draw** at that load |
| post-load stutter | **solved** — 17,827 -> 451 ms of pipeline compilation |

**Two things follow and neither is optional.**

1. **Do not re-price anything against `perf-state-parked.md` §2's text.** Those numbers
   were taken at ~10.5 ms / ~7,000 draws / 1280x720. The operating point moved as well as
   the code (gotcha 419) — 5.4x the pixels and 1.4x the draws.
2. **The CPU items all survive**, which is not a foregone conclusion and is why part 71
   spent an arm asking. At 3440x1440 the GPU is idle enough that 4x the pixels is 1.9 ms
   of a 28 ms frame.

**A draw is worth ~2.5 us.** That single number prices every culling item in this plan
without another session, and it is the reason §1 is first.

---

## 1. ITEM 1 — THE WIDE-CULLING OVER-WIDEN. ~~≈4.8 ms of 28~~ **an upper bound of 4.8 ms; the value is ≈2.5-2.8 ms**, and it is the operator's own complaint

> **CORRECTED IN PART 72, AND ROUTE (a) IS DEAD.** Two changes, both below and both in
> place: the ≈4.8 ms figure is an UPPER BOUND rather than the item's value, because the
> arm it was measured with removes the horizontal widening as well as the vertical
> over-widen (§1a); and the engine has no aspect scalar to widen, settled by a census over
> the whole image rather than by a run (§1b). `phase5-notes.md` **§6de**.

### What it is

Part 62 fixed a real defect: at 21:9 the flanks showed regions the game's own 16:9 frustum
had culled away. The fix substitutes the game's camera fov with
`v' = 2*atan(k*tan(v/2))`, `k = 9W/16H = 1.3438` at 3440x1440, and the renderer's composite
wide patch narrows the projection back vertically. **The picture is correct. The culling is
not**: the game frustum carries ONE scalar, so widening it widens the frustum in BOTH axes
— `k^2 = 1.806` of the volume where only `k` was needed horizontally. Everything above and
below the screen is submitted and then clipped away.

### What it costs, measured

Part 71 session 1, arm `nogamefov` against arm `base`, same spot, same sitting:

```
substitution ON   9,817 draws   28.05 ms
substitution OFF  7,890 draws   23.94 ms
                 +1,930 draws  (+24.4%)
```

At ~2.5 us/draw that is ≈4.8 ms of a 28 ms frame.

**The two arms render different draw sets, so 28.05 − 23.94 is NOT the item's value** —
that difference also contains whatever the extra geometry costs downstream. The draw-count
delta times the measured slope is the honest estimate for *the arm*.

### 1a. RETRACTION (part 72): 1,930 draws is an UPPER BOUND, not the item's value

**`CZ_NO_GAME_FOV=1` removes more than the defect.** It turns off the *whole*
substitution, and that includes the **horizontal** widening — which is the part-62 fix,
the thing that stops the flanks popping in, and which is being kept. With
`t = tan(v/2)` and `k = 9W/16H = 1.34375`:

| | horizontal half-tan | vertical half-tan | draws |
|---|---|---|---|
| `CZ_NO_GAME_FOV=1` (the arm) | `(16/9)·t` | `t` | 7,890 |
| shipped today | `(16/9)·k·t` | `k·t` | 9,817 |
| **what a horizontal-only fix reaches** | `(16/9)·k·t` | `t` | **strictly between** |

The wanted frustum has the arm's vertical and today's horizontal, so it is a strict subset
of today's and a strict superset of the arm's: **the recoverable draws are strictly fewer
than 1,930.** That is containment, not a model. Two models put it near half — linear in
tan-space area gives 1,105 recovered, a power law fitted to the two measured points gives
1,017 — i.e. **≈2.5-2.8 ms, not 4.8**, and probably less, because in an outdoor town the
horizontal flanks hold buildings and crowds while the vertical extension holds sky and
near ground.

**It still clears its own 700-draw / 1.75 ms kill threshold**, so the item lives. What
changes is that it no longer dominates the plan on an unmeasured number, and its ordering
against items 3 and 4 is a live question.

**So measure it instead of modelling it: `CZ_VK_VCULL_CENSUS=1`** counts the world draws
that land entirely off-screen in Y — the ceiling on what any vertical-cull fix recovers,
with no horizontal confound. Two controls (`CZ_VK_VCULL_SCALE` mechanical,
`CZ_NO_GAME_FOV=1` semantic) and an offline gate for its predicate
(`tools/vcull_predicate_test.cpp`). `phase5-notes.md` §6de §3.

### 1b. ROUTE (a) IS DEAD (part 72) — the engine has no aspect scalar

~~**(a) FIND THE GAME'S ASPECT SCALAR AND WIDEN THAT INSTEAD.**~~ **KILLED, by the kill
this section pre-registered, and it cost one afternoon of desk work rather than a
session.** Three independent lines, all in `phase5-notes.md` §6de §1:

* `tools/find_named_properties.py` (new) scans `.text` for all six universal property
  binders — **2,056 sites, 1,966 names recovered (95.6%)** — and the whole image contains
  **exactly one `Aspect`**: `+0x24` on **`cZombieSpawnRegion`**, beside `X/Y/Width/Height`.
  A 2D spawn box. Sixty-odd camera configs register `FOV` and nothing aspect-shaped.
* The image's single `1.777778` constant belongs to the **UI** layout system
  (`1.7778 / aspect`, gated on widescreen booleans); there is no 16:9 constant on the
  scene-projection path at all, so the scene aspect is computed rather than stored.
* The renderer's own `Is169Perspective` already said it: `|xscale/yscale| = 9/16` exactly,
  *"because this title's scene cameras are all 16:9 whatever their fov"*. An aspect
  identical across every camera in the game is not a per-camera tunable.

There are also **no `frustum`/`cull` strings in the image**, so route (b) has no debug
surface to grep for. What part 72 did recover for a future (b) attempt: the active scene
camera is a **stride-0x98** record at
`[[[r3+0xd60] + view*0x38C] + 0x348] + [r3+0x10c8]*0x98`, and **`cam+0x68` is the fov in
degrees** (read at `0x82791710`, × 0.5 × deg2rad, into `tan` at `0x8280F878`).

**The original text of route (a) is kept below because its reasoning is what a Case West
port should re-run — the answer is title-specific, the method is not.**

### The route, in the order to try it

**(a) FIND THE GAME'S ASPECT SCALAR AND WIDEN THAT INSTEAD.** This is the correct fix and
it makes the item free rather than a trade: if the engine's camera carries an aspect ratio
as well as a fov, widening the aspect gives exactly the right frustum in both axes and the
vertical over-widen disappears entirely.

* The instrument already exists: `CZ_FOV_PROP_TRACE=1` prints every FOV-named property
  registration through the universal binder `sub_82375518` with its live field address —
  it is what enumerated all 132 named camera configs in part 61. **Widen its filter from
  `FOV`/`Fov` to also match `Aspect`/`aspect`/`Ratio`, then read the list.**
* `tools/gdis.py` can look statically at `sub_8246BF48` (the camera param getter, lr
  `0x8246E31C`) and at the structure it reads at `+0x14` — an aspect field, if there is
  one, is likely adjacent.
* **Pre-registered kill: if no aspect-shaped property exists, say so and go to (b).** Do
  not spend a second session searching.

**(b) CULL-SIDE ASYMMETRY.** Leave the fov substitution alone but give the game's own
frustum test the correct asymmetric planes. Higher risk — it means finding and hooking the
cull, and this engine's culling has never been touched.

**(c) A SMALLER `k`, accepting some pop-in.** The cheap fallback, and the one that is a
PICTURE decision rather than a performance one: `k` is currently the exact value that makes
the flanks correct, so anything less brings back part 62's defect at the edges. **This is
the operator's call, not a number's.** If it comes to this, hand them two arms and a
sentence, not a percentage.

### Arms and gates

* `CZ_NO_GAME_FOV=1` — the whole substitution off. Already prints
  `[fov] CZ_NO_GAME_FOV=1 ...`, and the two-sided gate is the ABSENCE of
  `[fovgame] ... ACTIVE`.
* **A new arm is owed for whatever (a)/(b)/(c) becomes**, with a counter that says how many
  draws the frustum admitted, so the item's value is readable WITHOUT a frame time — the
  draw count is the thing being changed and it has no noise floor worth speaking of.
* **Pre-registered kill threshold: below 700 submitted draws recovered (≈1.75 ms) this is
  not worth a picture risk.**

---

## 2. ITEM 2 — FINISH THE PIPELINE-CACHE RESULT. 90 seconds of operator time, then a decision

### 2a. The attribution run, owed

Compilation went **17,827 -> 1,161 -> 451 ms** across arms run in that order, and the big
step happened with our cache file still EMPTY. So it is either the cache OBJECT (the driver
reusing stages within one run) or the driver's own implicit on-disk cache warmed by arm 1.
Arm order and cache state are confounded by construction (gotcha 422). One run separates
them:

```
ORDER=cold,warm,nocache tools/part71_pipeline_session.sh
```

* `nocache` still ~17 s -> **the object is doing it**, our persisted file is a further −61%,
  and a fresh machine pays ~1.2 s once. Nothing more to build.
* `nocache` collapses -> **a driver-side cache is doing it**, which means a driver update,
  a new machine or a new shader can resurrect the 17 s at any time, and §2b becomes worth
  building rather than optional.

### 2b. Pipeline creation OFF the pump thread — conditional on 2a

Even fully warm, part 71 measured **451 ms of compilation and a 108.6 ms single frame** in
a warm run. 108 ms is a visible hitch. The shape: create on a worker from
`ThreadBudget_Take` with an inline fallback on every miss, so correctness never depends on
the prediction — the same shape as part 53's content guards.

**THE OBVIOUS DESIGN DOES NOT WORK AND THE LOGS ALREADY SAY SO.** A draw needs its
pipeline NOW — `GetPipeline` is called from `DoDraw` and returns the pipeline that draw
binds — so every pipeline is created at the moment of FIRST USE, by construction. Deferring
to a worker means stalling (no gain) or skipping the draw (a wrong picture), and the
guard-pool style "predict from what the pump last saw" cannot help either: part 71's
per-frame table shows new combinations arriving across thirteen CONSECUTIVE frames
(arm 1, frames 4121-4133), i.e. they are genuinely new, not repeats a predictor could have
seen. **Do not build the predictive form.**

**THE FORM THAT DOES WORK: persist the state SET, not just the compiled blobs, and warm it
at load on a worker.** The pipeline cache stores compiled results keyed by state; it does
not tell us which states to ask for. A companion file — the `PipelineKey`s this machine has
seen — replayed on a background thread during the load screen would create the whole set
before the first draw needs it, hitting the warm cache each time. That is ~490 creations at
the warm rate, off the pump, in a window where the player is already waiting.

* Correctness is free: `GetPipeline` already returns from `R->pipelines` on a hit, so the
  warm thread only ever pre-populates a map the draw path was going to populate anyway. The
  inline path stays as the fallback for anything the file did not predict.
* The one real hazard is CONCURRENT insertion into `R->pipelines` from two threads, which
  is the part to gate — the same class as part 53's slot mix-up, and it needs that shape of
  verifier.
* **Pre-registered kill: if the warm worst-frame is under 50 ms after 2a, drop this.** A
  50 ms hitch a few times a session is below what the operator could rank (gotcha 421) —
  and part 71's warm arms measured 108.6 and 93.5 ms, so this is close to the line and the
  2a run may put it under.

### 2c. Watch the cache file

12.4 -> 12.9 -> 13.3 MB over three runs. It should asymptote as the pipeline set closes.
**If it is still growing ~450 KB a run after ten runs, it needs a cap** — this is derived
data in the user's cache directory and unbounded growth there is our bug.

---

## 3. ITEM 3 — THE LAST NAMED SUSPECT FOR THE PART-58 REGRESSION, and it still has no arm

Part 58 saw +1.3-1.6 ms appear between part 55's close and then. Part 71 eliminated the
clip-plane cache (+0.09 ms at the operator's load, against Night Run 1's headless +3.0%),
and the hook fold accounted for +0.74 ms of it. **§1.2 — part 56's per-draw dynamic-state
calls — is the only named candidate left and it has never had an arm.**

* What part 56 added: the stencil ref/mask block (three `vkCmdSet*` when stencil is on and
  changed) and, more subtly, `R->bound.haveStencil = false` on **every pipeline bind**,
  because binding a pipeline that specifies state statically makes the corresponding
  dynamic state undefined. So every non-stencil draw in between forces the next stencil
  draw to re-issue all three.
* `CZ_VK_NO_STENCIL=1` exists but is not the arm: it disables the stencil TEST in the
  pipeline too, so it changes the picture and is inadmissible.
* **The arm to build**: keep the stencil state exactly as it is and only remove the
  invalidation, by declaring the stencil states dynamic on EVERY pipeline rather than only
  on stencil-enabled ones. Then the skip-cache survives a bind and the three calls collapse
  to their real change rate.

**WHAT PART 71's PREP ALREADY ESTABLISHED, AND THE ONE THING IT COULD NOT.** Two of the
three numbers this item needs are in the archived logs:

```
[vk]   stencil test: 6941475 draws enabled it            (of 93,608,167 -> 7.4%)
[vk]   binds skipped per draw: pipeline 73.6% ...        (so 26.4% of draws rebind)
```

7.4% is already a ceiling: even if EVERY stencil-enabled draw re-issued all three
`vkCmdSet*` calls, that is 6.94 M x 3 calls over a five-minute run — order 1 s total, ~0.3%
of the frame. **So this item cannot be the whole +1.3-1.6 ms, and it may be nothing at
all.**

The third number decides which. `R->skips.stencil` **has been collected since part 56 and
was never printed** — the print at `VkRenderer_DumpStats` lists pipeline, viewport, scissor,
blend and descriptor-sets, and simply omits it. That is this project's recurring defect (a
counter you already pay for that no log carries), and part 71's prep added the line, with
stencil-ENABLED draws as the denominator rather than all draws — folding in the 92.6% of
draws that neither set nor skip would report a healthy 95% for a cache that never serves
anything. **The next operator run of any kind prints it for free.**

**Pre-registered: if the skip rate is above 90%, kill this item and declare part 58's
remainder unattributed.** The hook fold has already accounted for +0.74 ms of it and the
operating point has moved underneath the rest (gotcha 419) — an unattributed 0.5 ms at a
load that no longer exists is not worth a session.

---

## 4. ITEM 4 — THE CONSTANT GATHER (item C), sized and unbuilt

Night Run 1's `tools/alu_const_census.py` killed the range-copy design (median PS span
255/256 — the c255 tonemap cluster) and left the GATHER alive at ~10x: **median 9 VS / 27
PS registers actually read of 256**, with `a0`-relative indexing forcing a full copy on 22
VS and 0 PS. The target is ~28 MB/frame of copy at the OLD soak load; at the new one it is
larger.

Shape: a per-shader register list in the `.meta.json` sidecar, a gather copy, and a verify
arm shaped like the constant memo's (`CZ_VK_VERIFY_CONST_MEMO` + its poison arm, which read
0 of 117,521 and 100.0000% respectively — that pair is the template).

**It is also the only thing that can un-refute item E** (geometry in VRAM measured ~14%
SLOWER because we re-upload constants every draw, gotcha 363). Re-ask E after C lands and
not before; if C works, E is a second win for one item's risk.

---

## 5. ITEM 5 — PARALLEL COMMAND RECORDING (item A), and the gate that has been owed for eighteen parts

Still the largest single item (~40% of the pump: `DoDraw` plus the driver) and still the
riskiest. `perf-state-parked.md` §2 has the design. **Nothing about it changes except that
it must be re-priced against 28 ms rather than 10.5.**

**Its ORDER GATE must exist before any of it is written.** A per-frame ordered hash of
(draw index, pipeline, vertex range) recorded by both the serial and the parallel path and
compared. `perf-state-parked.md` §5 item 3 has called this owed since part 55 and it is
still the right precondition: **there is no other gate that catches getting draw order
wrong, and draw order on this title is semantic.** Build the gate, ship it on the SERIAL
path with a poison arm proving it can fire, and only then start the item.

Thread budget: `ThreadBudget_Take("record", N, nullptr)`, never `hardware_concurrency()`
(gotchas 358, 359). The operator's box is 8 physical / 16 logical and the whole budget is
3, already held by the guard pool.

---

## 6. THE MEASUREMENT RULES PART 71 ADDED, and they are not in `measurement.md` yet

`perf-plan-part71.md` §5's five still hold. These four are new:

1. **A draw band must be narrow relative to (effect / slope), not merely "narrow"**
   (gotcha 417). At ~2.5 us/draw a 300-draw mismatch inside a bin is 0.75 ms — the entire
   size of the effects being measured. 500-draw bands flipped BOTH of session 1's
   conclusions. **Print each arm's within-band draw median beside its frame time.**
2. **The configuration goes stale as fast as the code** (gotcha 419). Echo
   `cz_settings.txt` and the internal resolution with every number; both harnesses do.
3. **The operator's eye saturates** (gotcha 421). Above the perception floor their verdict
   outranks any statistic; below it, decide with the counter and tell them the number
   rather than asking them to rank it.
4. **When arms warm something shared, the arm ORDER is a variable** (gotcha 422). Prove it
   is not by running them backwards — one run.

And one that is not new but was decisive twice in part 71: **write the harness gate BEFORE
the session, from a line the FEATURE prints.** It caught `CZ_VK_RES` printing nothing on
its success path, which would have made the CPU/GPU arm unreportable on every run.

---

## 7. THE FIRST SESSION OF PART 72, concretely

**Nothing here needs an operator until §1's route is chosen.** In order:

1. ~~**Offline, free**: read `R->skips.stencil` out of part 71's archived logs.~~ **DONE,
   and it found the counter was never PRINTED** — see §3. The line exists now; the number
   arrives with the next operator run of any kind, and §3's kill threshold is
   pre-registered at a 90% skip rate.
2. ~~**Offline, free**: answer §2b's precondition from the per-frame pipeline table.~~
   **DONE, and it killed the predictive design** — pipelines are created at first USE by
   construction, and part 71's table shows new state combinations arriving across thirteen
   consecutive frames, so there is nothing for a predictor to have seen. §2b is rewritten
   around persisting the state SET and warming it at load.
3. ~~**Offline**: `tools/gdis.py` on `sub_8246BF48` and the camera param structure, looking
   for an aspect field (§1 route (a)).~~ **DONE, and it KILLED route (a)** — a census over
   the whole image beats a look at one function: there is exactly one `Aspect` property in
   the game and it belongs to `cZombieSpawnRegion`. §1b. It also forced §1a's retraction of
   this item's price.
4. **Then one operator sitting**, chained, `CZ_FPS_LOG` only, their heaviest spot:
   * `ORDER=cold,warm,nocache tools/part71_pipeline_session.sh` — 90 s, closes §2a;
   * ~~plus a `CZ_FOV_PROP_TRACE=1` run with the widened filter if §1 route (a) is live —
     it needs one boot and the frontend, not a soak.~~ **CANCELLED** — route (a) is dead
     offline (§1b), and a live property trace could only ever have reported what one route
     constructs where the census answered over the whole image.
   * **plus the vertical-waste census, which is what now prices item 1** (§1a): one soak at
     their heaviest spot with `CZ_VK_VCULL_CENSUS=1`, the same soak with
     `CZ_VK_VCULL_CENSUS=1 CZ_NO_GAME_FOV=1` (semantic control), and two short runs at
     `CZ_VK_VCULL_SCALE=0.02` and `=50` (mechanical control, both directions). **Frame
     times from these runs are worthless by construction** — the counts are the result.

**Copy `tools/part71_perf_session.sh` for any new arm rather than writing a harness from
scratch**: every arm proves it engaged from a line the feature prints, the harness refuses
to report one that does not and exits non-zero, the preflight NAME-diffs any shader caches
it switches between, and all its gates were tested against deliberate breakages before an
operator ever saw it.
