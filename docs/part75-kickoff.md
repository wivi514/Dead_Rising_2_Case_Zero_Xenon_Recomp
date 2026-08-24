# Part 75 kickoff — the hitch is inside the renderer, and step 3 is the only item left

> **THIS IS THE LIVE HAND-OFF**, superseding `part74-kickoff.md`.
>
> **The live subject is PERFORMANCE**, and unusually there is exactly one specified item
> left rather than a plan. Read in this order:
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6dj/§6dk/§6dl/§6dm** | **part 74's record — read §6dm and §6dl first** |
> | `docs/part74-kickoff.md` §1b | the ordered plan; steps 0-2 are DONE, step 3 is the work, step 4 is ruled out |
> | `docs/perf-plan-autonomous.md` | EXHAUSTED — every item closed, refuted or shipped |
> | `docs/perf-state-parked.md` | the reference the item designs came from — still not superseded |
>
> Lessons: gotchas **439-444**.

---

## 0. WHAT PART 74 SETTLED

| question | answer |
|---|---|
| **"how does the game fare against before the RT stuff?"** | the RT era costs **0.5-0.7 ms**. The 4.85 ms that looked like a regression is the 21:9/FOV fix — the part-60 build ignores `fov=10` and culls at 16:9 (gotcha 439) |
| **the sky flicker** | **SOLVED — two defects**, both fixed, gather ON by default again (§6dl) |
| **where the remaining hitch is** | **INSIDE the renderer.** Residual 0.0 ms on every hitch frame; part 73's "outside the renderer" is retracted (§6dm) |
| **the A5 kernel gate**, owed since part 67 | **CLEAN** — exit 0, 4 permutation windows, 0 real |

## 1. THE ITEM: THE TEXTURE PATH — AND THE DECODE IS THE BIGGER HALF

> **REVISED at part 74's close (§6dn).** A per-frame CPU/GPU profiler was built on the
> operator's request and it both attributed the stutter and re-shaped this item.

**The stutter is 100% CPU-side.** On every hitch frame the GPU is doing 3.8-9.9 ms while the
CPU burns 181-269; run-wide the GPU averages **7.93 ms/frame** against a **0.37 ms** fence
wait. The GPU is never the limiter on this route.

**And it is the texture path — 82-90% of every hitch frame.** In two halves:

| half | cost/run | per texture | fix |
|---|---|---|---|
| **decode** — untile every mip, endian swap, image creation | **469.0 ms (66%)** | 209 us | **parallelise or cache.** Pure CPU on the pump; this port already has a worker pool (part 53) |
| **staging + submit** — `memcpy` + `RunImmediate` (94% of it `vkQueueWaitIdle`) | 244.0 ms (34%) | 109 us | **batch** — see the constraints below |

**The decode was invisible until part 74's close** (the clock started at the staging memcpy,
gotcha 445), which is why this file previously named only the batching. **Take the decode
first: it is nearly twice the size and its fix is independent.**

```
frame 2771:  285.5 ms = CPUrec 268.9 + fence 2.6 + sleep 14.0   GPU 3.8   775 tex (up 65.6 + dec 177.3)
frame 6787:  209.1 ms = CPUrec 208.7 + fence 0.0 + sleep  0.5   GPU 9.9   692 tex (up 61.8 + dec 119.2)
```

**One frame is still unexplained and is worth a look:** 6788 is **96.5 ms of CPU recording
with ZERO uploads, zero decode, zero pipelines** and a 10.9 ms GPU, immediately after a
692-upload burst. The instrument to chase it now exists (`CZ_VK_FRAME_TRACE`).

### 1b. THE BATCHING HALF (the original step 3 text)

**Why it is the item:** the frame decomposition puts the whole hitch inside `Pm4_Execute`,
and inside that, texture uploads drive **288.8 ms of immediate submits a run, 94.0% of it
`vkQueueSubmit`+`vkQueueWaitIdle`**. Part 74 proved **no cheaper wait primitive exists**
(three arms, gotcha 436) — the only fix is not doing 2,243 round-trips.

**The design constraint, already found so it is not rediscovered:** `RunImmediate` exists
precisely to be OUTSIDE the frame's command buffer, because a pipeline barrier is illegal
inside a dynamic-rendering scope. Batching into the frame's command buffer therefore means
**breaking the render pass at each upload** — and part 74 priced that cycle at **~6.6 us**,
so 2,243 uploads is **~15 ms against 271.6 ms, roughly 18x cheaper**.

**The second constraint:** `R->staging` is ONE buffer written at offset zero by every
upload, which is why the current code must drain before reusing it. The per-frame arena
(`ArenaAlloc`, 256 MB, host-visible) is the obvious replacement staging source.

**Pre-registered kill for EACH half separately: below 40 ms off the worst frame of the
route, do not ship that half.** It is
a HITCH item — 0.020 ms/frame amortised — so a small win does not justify touching the
upload path. Picture gate plus the operator's look before shipping: it changes when pixels
arrive.

~~One cheap measurement first: ... GPU backpressure ...~~ **DONE at part 74's close and the
hypothesis was WRONG.** The fence wait on those frames is **0.0-3.6 ms** — there is no GPU
backpressure. The unattributed time was the texture DECODE, which no clock was covering.

## 2. WHAT IS RULED OUT — do not start these

* **step 4, the guest side.** The residual is 0.0 on every hitch frame. `CZ_FILE_TRACE` /
  `CZ_GUEST_DIAG` / `perf` are the right tools for a problem this measurement says does not
  exist here.
* everything on part 74's do-not-re-buy list: the pipeline-cache A/B, route (a) for the wide
  culling, whole-pass parallel recording, a range copy for the constants, geometry in VRAM,
  the 41 near-empty passes (0.27 ms ceiling), a cheaper wait primitive for uploads.
* **reverting the RT era for performance** — it is 0.5-0.7 ms (§6dj).

## 3. WHAT IS OWED BY THE OPERATOR — one thing, and it is short

**"Is there still a felt stutter, and where — menu, load, or gameplay?"** Part 72's binning
of their session said gameplay was smooth and every hitch sat below ~2,000 draws; part 74's
route puts its worst frames at 2,446-6,635 draws. One of those readings is about a route
rather than about the game, and only their eye separates them. **If they report gameplay as
smooth, step 3 is worth much less than its threshold suggests and the subject should
change.**

The thread-budget decision for item A is moot — item A is demoted twice over.

## 4. THE TOOLING PART 74 LEFT

```
BIN_SRC=<path>          run any binary on the route. THE positive control for an
                        intermittent defect — the pre-fix binary flickers 6 of 6 where a
                        same-binary arm was equivocal. Worktree: ~/GithubRepo/dr2cz-part60
STILL=1                 hold the camera. The turn block is for STUTTER and masks a FLICKER
PRESSMS=<ms>            menu walk speed (default 3000; was 8000 and is now 21 s not 56 s)
CZ_VK_CONST_RACE=1      the constant-slot race detector, with CZ_VK_CONST_RACE_POISON=1
CZ_PUMP_POISON_MS=N     the residual column's positive control
CZ_VK_SKY_ASYM=<file>   per-frame sky asymmetry — a THREE-RUN aggregate, not a single-run
                        instrument (gotcha 444)
CZ_KEEP_SYNTH=<dir>     keep the HLSL so alu_const_gate.py --hlsl-dir can actually run
tools/play_session.sh KEY=VALUE ...   trailing env passes through
```

## 5. GATES AT PART 74's CLOSE — all clean

```
--smoke OK                              A5  exit 0, 4 permutation windows, 0 real
alu_const_gate --hlsl-dir  clean/449    shader_dim_census  clean
rt_world_xform  104 of 104              play cache NAME diff  empty
play session: 0 'no translated shader', 0 slot mix-ups, 0 CONST MEMO STALE
```

**A5 is no longer owed** — it was carried from part 67 to part 74 and is now clean.

## 6. THE ONE THING TO CARRY FORWARD

Part 73's lesson was that a priced item is not a measured item. Part 74's is narrower and
sharper: **three of its findings were reversals of conclusions drawn from an ABSENCE.**
"The cost is outside the renderer" came from three columns reading zero. "The lists are
clean" came from a gate printing a caveat beside exit 0. "The fix works" came from one quiet
run of an intermittent defect. Each time the repair was the same — **build the thing that
CANNOT return a false absence**: a decomposition where every millisecond lands somewhere, a
gate whose inputs survive by default, a positive control that must scream. When a conclusion
rests on not having seen something, that is the moment to spend the effort.
