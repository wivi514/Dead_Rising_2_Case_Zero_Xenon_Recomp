# Part 72's fix list — what the two operator sessions caught, and what to do about it

**Written 2026-08-23, after both operator sittings of part 72.** The record of the
sessions themselves is `phase5-notes.md` §6df; the live performance plan is
`perf-plan-part72.md`, which this document feeds. **Nothing here needs an operator until
§4**, which is the one sitting that is owed and which is deliberately small.

> The short version. **One measurement retracted a part-71 headline** (the pipeline cache
> is not the stutter fix; the driver's own cache is, and ours is measurably worse than
> passing nothing). **One measurement refuted itself** — the vertical-waste census's own
> horizontal control fired, and the census is not yet measuring what it claims. **Two new
> defects were found for free** in the process. Everything in §1-§3 is desk work.

---

## 0. WHAT WAS ESTABLISHED, and how much confidence each item carries

| # | finding | confidence | who can act |
|---|---|---|---|
| A | Part 71's "a persisted `VkPipelineCache` is −97.5%" is **wrong**; the driver's own cross-process cache is the mechanism | **high** — same-arm across two orderings | desk (docs), then §4 |
| B | Our `VkPipelineCache` costs **7.5x** more per pipeline than passing `VK_NULL_HANDLE` on a warm driver | medium — one ordering | **§4 run 1** |
| C | The vertical-waste census **is not measuring placement correctly**: ~98% of world draws read "entirely off-screen horizontally" | **high** — its own control, and implausible on its face | desk (§2) |
| D | The census's headline was a **cumulative mean dominated by a transient** — 62/frame cumulative vs 1.0/frame in steady state | **high** — the dump history is monotone `C/n` | desk (§2.1) |
| E | Pipeline compilation is **not the whole stutter**: 165-315 ms gameplay frames remain with compilation at 51 ms/run | medium | desk (§3), then a run |
| F | The pump's slope re-confirmed at **2.35 us/draw** (30.6 ms @ 9,750 vs 25.0 ms @ 7,370, census on both sides) | high | — |
| G | **Item 1 (the wide-culling over-widen) is UNPRICED.** It is not 4.8 ms, probably not 2.5 ms, and C means this session did not measure it | — | §2 then §4 |

**The one thing not to conclude yet.** Every vertical reading in session B was small, and it
is tempting to declare item 1 dead. **Do not** — finding C means the instrument that
produced those readings mis-places its geometry, and the mis-placement direction
(laterally, not vertically) is exactly the direction that would HIDE vertical waste. A
small number from a broken instrument is not a small effect.

---

## 1. FINDING A/B — THE PIPELINE CACHE. Retract, then decide with one run

### What the re-ordered run showed

The pipeline COUNT is near-constant across all seven runs ever taken (484-513), which
controls for the route: the same population was created each time and only the cost per
creation moved.

| session | pos | arm | pipelines | total | **ms/pipeline** |
|---|---|---|---|---|---|
| part 71 | 1 | `nocache` | 489 | 17,827 ms | **36.457** |
| part 71 | 2 | `cold` | 484 | 1,161 ms | 2.398 |
| part 71 | 3 | `warm` | 490 | 451 ms | 0.920 |
| part 72 | 1 | `cold` | 490 | 821 ms | 1.676 |
| part 72 | 2 | `warm` | 513 | 403 ms | 0.786 |
| part 72 | 3 | `nocache` | 484 | **51 ms** | **0.105** |

**Same arm, different position** is the discriminator, and it is unambiguous:

* `cold` 2.398 -> 1.676 (**1.4x**, noise)
* `warm` 0.920 -> 0.786 (**1.2x**, noise)
* `nocache` 36.457 -> 0.105 (**346x**)

If POSITION were the variable, `cold` and `warm` would move with it. They do not. The one
thing that changed for `nocache` is that something **outside our process** had gone warm
between part 71's sitting and part 72's. The same 97-pipeline event at frame ~1250 cost
**3,754 ms** in part 71 and **9.9 ms** in part 72, both with `VK_NULL_HANDLE` — so it is
a driver-side cache that persists across processes. (RTX 3070. The location was not
identified: nothing in `~/.cache/nvidia` was touched, so no claim is made about where it
lives — only that it is not ours and not in-process.)

### What to retract, and where

**Part 71's headline — "a persisted `VkPipelineCache` takes the run's total to 450.8 ms
(−97.5%)" — attributed the driver's cache warming to our file.** Retract in place in:
`phase5-notes.md` §6dd, **gotcha 420**, `CLAUDE.md`'s part-71 status block,
`perf-plan-part72.md` §2, `part72-kickoff.md` §0, and the memory note
`part71-pipeline-cache-was-the-stutter.md`.

**What survives the retraction, and it is not nothing:**

* **The 17.8 seconds was real, and it is what a genuinely cold shader set costs.** Part
  71's diagnosis of the *stutter* — that the 3,891 ms frame was pipeline compilation on
  the pump thread — is CONFIRMED by its own top-frame table and is untouched.
* **The unconditional per-frame compilation census is what made all of this visible** and
  stays. It was gated behind `CZ_VK_PROFILE` for phase 5 through part 70, which is why the
  theory was inferred three times and never measured.
* **A cold driver cache can return at any time** — a driver update, a new machine, a cache
  eviction, a changed shader set. Users hit it on first run. That makes `perf-plan-part72`
  **§2b (create pipelines off the pump thread during the load screen) the real answer**,
  and it is now the only answer, because our own cache demonstrably does not prevent it.

### The decision that needs one run (finding B)

`warm` (pos 2, 0.786) against `nocache` (pos 3, 0.105) is still position-confounded within
part 72's sitting. The stability of `cold`/`warm` across positions argues it is not, but
that is an argument and this project decides with arms.

**Pre-registered:** if `ORDER=nocache,warm` on a warm driver reproduces the 7.5x, our
`VkPipelineCache` is a pessimization on this driver and its default flips to OFF, with
`CZ_VK_PIPELINE_CACHE=1` as the opt-in — **not deleted**, because a driver without an
implicit disk cache would need it, and this port has exactly one machine's worth of
evidence. If the gap closes, keep it as it is and stop thinking about it.

**Do not change the default before that run.** One machine, one driver, one ordering.

---

## 2. FINDING C/D — THE CENSUS IS WRONG, AND ITS OWN CONTROL SAID SO

### 2.0 What happened

The census counts world draws whose object-space bounding box projects entirely outside
the clip volume. It reports a vertical figure (the item) and a horizontal figure, and the
horizontal figure was added **as a control with a stated expectation**: the horizontal
widening is part 62's fix and is kept, so the game culls to exactly the screen's horizontal
extent and the number should be small.

It read **98.1%**. In steady state, of 7,298 tested draws a frame, 7,156 were classified
"entirely off-screen horizontally" — leaving ~142 on-screen draws to paint a scene that
submits 9,750. That is not credible, and it is the control doing its job.

The vertical channel reads **1.0 draws/frame in BOTH census arms** — the `62 -> 1` semantic
response was entirely a transient (§2.1). So the vertical channel discriminates nothing
either. **The predicate is fine** (the scale sweep moves it monotonically both ways: 5,653
at `SCALE=0.02`, 0 at `SCALE=50`, and `tools/vcull_predicate_test.cpp` passes 13/13 and
fails on a flipped sign). **The geometry going into it is wrong**, displaced laterally but
not vertically.

### 2.1 DEFECT 1 — a cumulative mean is not a rate. FIX FIRST, it is five lines

The census printed a running mean over the whole run. Its own dump history:

```
frames   vert/f   V total   dV/df
  1800        0         0     0.0
  2400      184    441600   736.0      <- the entire signal is this burst
  3000      188    564000   204.0
  4200      135    567000     3.0
  6000       95    570000     5.0
  9198       62    570276     1.0
```

`V total` is **flat from frame 3,000 onward**: everything was accrued between frames 1,800
and 3,000 (the approach to the spot), and the soak that the measurement exists to
characterise contributed ~1 draw a frame. The decaying `62` is `C/n`, not a rate.

**Fix:** print the WINDOWED delta since the previous dump as the headline and keep the
cumulative as a secondary. This is gotcha 237 in a new costume ("a MEAN frame time
measures the pacing floor, not your change") and it should be checked against every other
counter this renderer prints as a per-frame average.

### 2.2 DEFECT 2 — THE BOXES WERE NEVER PLACED, and this repo had already said so

**Written before the cause was found, and kept because the three candidates below are the
ones a reader would reach for too. The actual cause was none of them, it was worse, and it
was findable without a single new measurement.**

`ShaderMeta`'s own comment in `vk_renderer.cpp` carries **part 67's retraction of the exact
assumption this census was built on**:

> *"`rtshadow::Collect` gates a draw on `SceneXformForm(c0..c3) == 2`, and §6cs read that
> as 'so the position stream is world-space'. **It is not.** c0..c3 is the CAMERA's
> view-projection, and that is the same matrix whether the shader feeds it a world position
> or an object position it transformed one line earlier — which is what this title's world
> shaders do, from a row-major 4x3 at vc(8..10). Measured over the twenty `.xtr` world
> traces, 46,820 accepted draws: 100% of them carry a NON-IDENTITY world translation, and
> the fraction whose bounding box intersects the frustum it was drawn into goes from
> **0.1% untransformed to 97.8% placed.**"*

The census measured **1.9% on screen**. It reproduced part 67's 0.1% figure, from part 67's
mistake, one part after part 67 fixed it — and the fix (`config/rt_world_xform.json`,
`ShaderMeta::xfCount/xfBase/xfPalette`, covering 104 of 104 vertex shaders) was already
sitting in the file the census was written into.

**How it got past me.** §6de §3 — written that same morning — cites §6cs by name for the
world-space claim, quoting a conclusion that had been retracted three parts earlier. The
retraction lives in a code comment and in §6cy, not in §6cs, so re-reading §6cs would not
have caught it. **This is gotcha 13 with the retraction on the wrong side**: a superseded
claim that stays quotable because the correction was filed somewhere else.

**Fixed** (`PlaceBox`, shipped): object->world applied per CORNER before projection,
composed across stages; declined-and-counted when the shader has no table entry; palette
draws declined (part 69: 0 of 2,786 reference a single matrix, so entry 0 would pile a
batch onto whichever prop is bone 0). Deliberately **not** `rtshadow::ObjectXform`, which
returns identity when `PlaceInstances()` is off — borrowing it would have made the census
silently depend on an RT arm being armed, which is the gotcha-414 shape.

### 2.2b The three candidates that were wrong, kept because two of them were real anyway

Part 63's first-sight bounds scan **confirmed** the composite population is world-space
(z ±550, y −47..360, only 27% centered within 100 units of the origin). So world-space is
right for the population *it* gated. The census gates less tightly, and three mechanisms
fit the observed signature — wrong lateral position, right height:

**(a) STALE BOUNDS.** Bounds are scanned once at first sight and never re-scanned. A
buffer rewritten each frame at the same guest address (crowds — this engine has a
"CrowdEngine", and the soak is a crowd) keeps frame N's box forever. A zombie that has
walked away is at the wrong X/Z and the right Y, because they all walk on the ground.
*Fix: hash the stream per frame (the RT census already does) and re-scan on change.*

**(b) INSTANCED / LOCAL-SPACE GEOMETRY.** A base mesh at the local origin drawn many times
with per-instance transforms elsewhere in the constants projects to the world origin — far
off-axis laterally, plausible height. The census excludes only `pos.indirect` on attribute
0; part 63's verified population also excluded `depVS` (ANY attribute carrying a dependent
fetch). *Fix: adopt `depVS`, and report how many draws it removes.*

**(c) WHOLE-BUFFER BOUNDS.** One zone-sized vertex buffer drawn with many index
sub-ranges gives every draw the whole zone's box. This biases toward "on screen", so it is
**not** the cause of over-firing — but it destroys resolution and would hide real waste.
*Fix: none cheap; report the box extent distribution so its size is known.*

Of those three, **(a) and (b) were fixed anyway** — bounds are now re-scanned when the
stream's content hash changes, and dependent-fetch draws are excluded to match the
population part 63 verified. Neither was the cause, both were real, and both produce the
same lateral-only error, so leaving them in would have made the next run ambiguous.

### 2.3 THE REAL FIX — the census must refuse to report when its own invariant fails

The deeper defect is not any of (a)-(c). It is that **the census had no way to know it was
wrong**, and only a control added on a hunch caught it. Two changes make it
self-validating:

1. **A SANITY INVARIANT WITH A REFUSAL.** A correctly-placed world draw set must be
   *mostly on screen* — that is what the game's culling is for. Compute the on-screen
   share and **refuse to print a headline below 50%**, printing the diagnosis instead.
   This is the same shape as the harness's engagement gates: an instrument that cannot
   show it is working does not get to report a number (gotchas 30, 151, 408).
2. **THE DIAGNOSIS, PRINTED WITH THE REFUSAL.** Split the counts by `depVS` and by
   "this stream's content has changed since its bounds were scanned", and name a capped
   sample of offending draws (box centre, extent, off-axis angle, stream key, shader hash).
   That sample is what distinguishes (a) from (b) in one run instead of three.

**Note what this does to the pre-registered decision.** `perf-plan-part72` §1 says kill
item 1 below 700 recovered draws. That threshold stands, but **it cannot be applied to a
number from an instrument that fails its own invariant.** Item 1 stays unpriced until §4
run 2 comes back with the invariant satisfied.

### 2.4 Order of work

**ALL SIX ARE DONE** (commit `4b701e3`), plus the placement that turned out to be the
actual defect:

```
[x] object->world placement, per corner    (§2.2)  — THE cause
[x] windowed rates                         (§2.1)
[x] depVS exclusion + its counter          (§2.2b)
[x] re-scan bounds on content change       (§2.2a)
[x] on-screen invariant + refusal          (§2.3)
[x] offender sample, object AND placed centre (§2.3)
[x] predicate gate extended to the placement — 18 cases, and confirmed capable of
    failing on a flipped comparison (0->6), a dropped placement (0->1) and a
    TRANSPOSED matrix read (0->1). The transpose needed a SHEAR case: every
    symmetric transform passes either way, and a 90-degree yaw is its own inverse
    under transposition.
```

---

## 3. FINDING E — THE STUTTER IS NOT ALL PIPELINE COMPILATION

With compilation at **51 ms for the whole run**, the `nocache` arm still shows a **173 ms
frame at 7,400 draws** and its worst pipeline frame is 9.9 ms. All three session-A arms
carry gameplay frames of 165-315 ms that the top-frame table does not account for.

This is a **new open item**, and it is the honest remainder of part 71's stutter work: that
part attributed the felt stutter to compilation, and for the 3,891 ms frame it was right,
but a 165-315 ms hitch survives the fix. Candidates, none tested: zone streaming / file
I/O, texture upload bursts, the arena growing, descriptor-pool exhaustion. `CZ_FPS_LOG`'s
`worst` already names the window; the cheap next step is to correlate it with the counters
the renderer already prints per frame rather than to add an instrument.

**Filed to `open-items.md`, not to this session.**

---

## 4. THE ONE SITTING THAT IS OWED, and it is two short runs

Deliberately small, because the operator has already given two sittings.

```
ORDER=nocache,warm tools/part71_pipeline_session.sh    # ~4 min — decides finding B
tools/part72_vcull_session.sh                          # the FIXED census, ORDER=census,nofov
```

* **Run 1** answers whether our pipeline cache is a pessimization. Pre-registered above.
* **Run 2** re-asks item 1 with the invariant in place. **Two arms are enough now** — the
  mechanical controls have already done their job and do not need repeating (the predicate
  is unchanged and has an offline gate). If the invariant still fails, the offender sample
  says which of (a)/(b)/(c) it is and the fix is one more desk pass, not another sitting.

**What NOT to ask for:** another four-arm session. The mechanical controls fired correctly
and re-running them is spending operator time to confirm something already confirmed
(gotcha 13's inverse — a result with a shelf life is not the same as a result to repeat).

---

## 4b. WHAT WAS DONE OFFLINE AFTER THIS PLAN WAS WRITTEN

The plan above says §1-§3 are desk work. They were, and two more things turned out to be
desk work as well — both of which had been assumed to need the operator.

### 4b.1 THE CENSUS FIX IS VERIFIED, WITHOUT A RUN (`3c7d528`)

The placement fix shipped as a prediction, and under "all runtime verification goes through
the operator" it would have sat unverified until the next sitting. It did not have to: the
`.xtr` captures carry hardware's own draws with their own constant windows, so the census's
arithmetic runs against them with no runtime at all — and the verdict comes from a source
this project did not write. `tools/vcull_xtr_oracle.py`, over 12,560 classifiable world
draws in 20 traces:

| | on screen | off-V | off-H | near-plane |
|---|---|---|---|---|
| **placed** (the fix) | **84.6%** | 0.1% | 2.5% | 12.8% |
| **unplaced** (the defect, `--no-placement`) | 12.2% | 2.4% | **59.1%** | 28.7% |

84.6% + 12.8% near = **97.4%**, against part 67's independent 97.8% "intersects its own
frustum" over 46,820 draws — the gap is definitional (this counts a near-plane straddle as
not-on-screen, deliberately, so the output stays conservative). And the deliberate breakage
reproduces the operator session's signature: a dominant off-H share, exactly what session B
measured at 98.1%.

**And it found a coverage problem the runtime was hiding.** 34,184 palette draws declined
against 12,560 classified: **this census speaks for roughly a quarter of the world.**
Declining them is still right (part 69 read 0 of 2,786 palette draws referencing a single
matrix, so a box over a batch placed at entry 0 cannot answer a visibility question) — but
the runtime folded palette into a generic "unplaceable" total where a reader would take the
headline for the whole population. Palette now has its own line and the classified share is
labelled **"THE HEADLINE SPEAKS FOR THIS SHARE ONLY"**.

**What it cannot do:** these captures are hardware at 16:9 with the game's own fov, so they
contain no wide-mode substitution and **cannot price item 1**. They validate the machinery,
not the item.

### 4b.2 THE ORDER GATE IS BUILT AND PROVEN (`28990f9`)

§5's precondition, owed since part 55, is closed — and it was always desk work. See §5 of
the plan for the mechanism. The part worth repeating here is the failure mode: **a
commutative hash would have made the gate a placebo**, passing every misordered frame while
looking like it worked, and "FNV is non-commutative" is exactly the kind of claim this
project has a rule about measuring rather than asserting.
`tools/order_gate_test.cpp` measures it: all 3,999 adjacent transpositions in a 4,000-draw
frame of near-duplicates detected, plus drops, duplicates, rotations and reversals — and a
control confirming a commutative mix IS blind, without which the test would pass for any
hash at all.

**So the largest item in the plan is now unblocked**, and it is the next thing to write.

## 5. THE TRANSFERABLE LESSONS, for the gotcha ledger and for Case West

1. **A control added "because this should be small" is worth more than the headline it
   accompanies.** The horizontal channel existed only to be boring, and it is the only
   reason a wrong number was not shipped as item 1's price.
2. **A cumulative mean printed periodically looks like a time series and is not one.** Print
   the delta.
3. **When arms warm something OUTSIDE the process, the arm order is a variable and so is
   the SESSION order.** Gotcha 422 said run them backwards; doing so inverted the
   conclusion. The generalisation: a same-arm comparison ACROSS orderings is the
   discriminator, and it works because the invariant (pipeline count) held.
4. **A measurement whose error direction hides the effect cannot produce a null.** The
   census mis-places laterally, which is exactly the direction that hides vertical waste —
   so its small numbers are not evidence of a small effect.
5. **"This needs an operator" is a claim worth re-testing before you accept it.** Two of
   this part's verifications were assumed to need a sitting and turned out to be desk work:
   the captures already contained hardware's own draws, and the order gate's one fatal
   failure mode is a pure property of its hash. Both are now proven without an operator run.
6. **A gate's fatal failure mode is usually a property you assumed rather than measured.**
   For the order gate it was commutativity; for the census it was which space the vertices
   were in. Neither would have announced itself.
