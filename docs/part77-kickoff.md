# Part 77 kickoff — the readback is gone, and the texture path is the only felt stutter left

> **THIS IS THE LIVE HAND-OFF**, superseding `part76-kickoff.md`.
>
> **Read `phase5-notes.md` §6dq first.** It is part 76 end to end: the readback split, the
> A/B, the off-by-one that making a static predicate dynamic introduced, the gate that
> could not fail on its first attempt, and the per-draw `getenv` census.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6dq** | **part 76 — −16.4% of the crowd frame, and it was our own launcher** |
> | `phase5-notes.md` §6dp | part 75 — the write-combined read, and §10 the operator session that set the board |
> | `phase5-notes.md` §6dn | the per-frame CPU/GPU profiler, and the texture DECODE clock |
> | `part75-kickoff.md` §1 | **the texture item, fully specified and priced — still current** |
> | `docs/perf-state-parked.md` | the reference the older item designs came from — not superseded |
>
> Lessons: gotchas **451-454**. There is still no live PLAN; **§1 is the board, in order.**
>
> **AND READ §0b BEFORE PRICING ANY CPU ITEM.** The port now has ONE resolution and **TWO
> REGIMES**, separated by about 8,000 draws: the autonomous route is GPU-bound, the
> operator's crowd is CPU-bound. Quote which one a number came from.

---

## 0. WHAT PART 76 DID, IN ONE PARAGRAPH

`part76-kickoff.md` §1 put the F8/F9 readback first: 3.49 ms of the operator's 23.31 ms
crowd frame, and not the game. `CZ_CAPTURE_KEY` and `CZ_BURST_DUMP` are edge-triggered
instruments that were in a list of "is a picture instrument armed", and
`tools/play_session.sh` sets both unconditionally so the keys work — so every play session
since part 54 paid a whole-frame `vkCmdCopyImageToBuffer` plus a 19.8 MB `memcpy` a frame
into a buffer the swapchain never displays. The press now arms the readback for the frames
it needs. **−2.13 ms, −16.4% of the crowd frame at 3440x1440 — 77.0 -> 92.2 fps at ~6,000
draws** — monotone across every draw band, against a null floor of 0.01 ms. A second
finding of the same class, a `getenv` on the per-draw path in every run this project has
ever made, is in §6dq §6.

## 0c. WHAT PART 76 DELIVERED, AT THE OPERATOR'S OWN LOAD

**The honest frame rate, no instruments but the trace, standing in the Main Street crowd at
8,300-8,500 draws: 12.44 / 12.58 / 12.77 ms median over three consecutive 10 s windows —
78 to 80 fps.** Predicted from the profiled session before the run: "around 12.6 ms, about
79 fps". Within 2%.

### the profiled comparison, which is the like-for-like one

Same harness, same resolution, same instrument load as part 75's verification session, so
the two are directly comparable — **>= 7,000 draws, profiler on: 23.31 ms -> 16.65 ms,
−28.6%, 42.9 -> 60.1 fps** (n=1,377). With the profiler's measured 4.00 ms taken off, the
game they actually play sits near **12.6 ms at that load, about 79 fps.** 12,772 frames
traced, every gate clean (0 `no translated shader`, 0 slot mix-ups, 0 `CONST MEMO STALE`,
0 stale present slots).

## 0b. THE FLOOR MOVED, AND IT WILL MAKE YOUR NEXT CPU ITEM READ ZERO

`CZ_VK_FRAME_TRACE` on the autonomous route, 3440x1440, medians over 5,000-7,000 draws,
the two arms of part 76's item 1:

| arm | wall | GPU | fence | CPU record | GPU/wall |
|---|---|---|---|---|---|
| **shipping** | 10.59 ms | **10.55** | **2.99** | 7.10 | **100%** |
| pre-part-76 (`CZ_VK_PRESENT_ALWAYS=1`) | 12.29 | 12.21 | 0.53 | 11.25 | 99% |

The readback was a CPU cost **and** a GPU cost — CPU record fell 4.15 ms and the GPU fell
1.66 ms, because a whole-frame `vkCmdCopyImageToBuffer` is work the device does — and the
wall fell only 1.70 because the fence wait rose 2.46. **`fence` was 0.37 ms mean run-wide in
part 74, whose conclusion was "the GPU is never the limiter here". That is retracted for
this route.**

Part 76 proved the consequence the expensive way: a correct, free removal of ~0.77 ms of
predicted per-draw CPU work measured **−0.08 ms against a −0.06 ms null**. The operation was
priced right and the call count was confirmed right; the frame simply was not waiting on
that thread any more (gotcha 453).

**AND THE OPERATOR'S OWN PLAY ANSWERED THE SCOPE QUESTION — §6dr, and the profiler-free run
settled it with NO SUBTRACTION.** 12,677 texture-free frames, 3440x1440, nothing armed but
`CZ_VK_FRAME_TRACE`:

| draws | n | wall | GPU | fence | CPU rec | regime |
|---|---|---|---|---|---|---|
| 0-3,000 | 9,570 | 9.02 | 9.07 | **4.15** | 4.16 | GPU is the limiter — CPU idles 46% of the frame |
| 3,000-5,000 | 371 | 8.68 | 8.69 | 2.42 | 6.00 | GPU is the limiter |
| 5,000-7,000 | 713 | 9.84 | 9.61 | 0.64 | 8.55 | **balanced — THE CROSSOVER** |
| 7,000-9,000 | 1,670 | 11.93 | 11.35 | **0.00** | 11.30 | **CPU is the limiter** |
| 9,000-12,000 | 353 | 13.58 | 12.20 | **0.00** | 13.06 | **CPU is the limiter** |

**The crossover is 6,000-7,000 draws, not 8,000** — the profiled session's flat 4.00 ms
subtraction put it a band too high, because the instrument's bill scales with the draw count
(+1.55 ms at 2,500 draws, +5.45 at 9,200). The GPU column agrees between the two runs to
within 0.5 ms at every band, which is the cross-check that licenses the rest.

**The GPU is flat — 8.5 to 11.9 ms across a 4x range of draw counts** — which is what a
pixel-bound cost looks like at a fixed resolution. The CPU runs 5.7 to 18.5 over the same
range. **Both readings were right and neither generalised: my route is GPU-bound because it
is a light-draw run at a heavy resolution; their play is CPU-bound because they stand in
crowds.**

**So, operationally:** run `CZ_VK_FRAME_TRACE` and read `fence` and `gpuUs` BEFORE pricing a
CPU item, and say which regime AND WHICH DRAW COUNT the number was taken at. An item worth
3 ms of CPU is worth 0 ms of frame time on the autonomous route and close to 3 ms in a
crowd. **Measure CPU items at 9,000+ draws or not at all.**

**AND USE THE CHEAPEST INSTRUMENT THAT ANSWERS THE QUESTION.** `CZ_VK_PROFILE` costs
**+1.55 ms of CPU in a menu and +5.45 ms in a crowd** and only ~+1 ms of wall — it eats the
CPU slack, so it takes the fence from 2.99 to 0.00 and reports the same route as CPU-bound
that it reports as GPU-bound without it. A regime question is a question about SPARE CAPACITY
and a CPU instrument destroys the quantity being asked about; worse, **its bill has the same
shape as the thing being measured**, so a flat subtraction moves the answer. `CZ_VK_FRAME_TRACE`
alone is a line of I/O per frame and does not move the numbers. Gotcha 454.

## 1. THE BOARD, IN ORDER

### ITEM 1 — THE TEXTURE PATH. **Confirmed first by the operator's own 20 marks.**

Their part-76 session marked twenty stutters with F7. **All twenty had `fence 0.00` and a
healthy 11.1-12.7 ms GPU; 18 of 20 carried a real texture upload, with `texPh` at 9.3-16.8 ms
of a 30-37 ms frame** (§6dr §4). And the marks were not hitches this time — the whole marked
stretch is the crowd frame at ~9,500 draws with a texture-driven excursion on one frame in
five, so **at their draw count this path is no longer only the HITCH item; it is roughly half
the excess on a bad frame.** The era splits 16.67 ms median with no upload against 22.61 with
one, p99 29.09 against 35.15.

**Unchanged and still fully specified in `part75-kickoff.md` §1** — do not re-derive it,
and do not re-price it from a remembered number; the two halves were measured in part 74
(§6dn) and the operator's marks in part 75 (§6dp §10) said all six were this.

| half | cost/run | per texture | fix |
|---|---|---|---|
| **decode** — untile every mip, endian swap, image creation | **469.0 ms (66%)** | 209 us | **parallelise or cache. Take this first.** Pure CPU on the pump |
| staging + submit — `memcpy` + `RunImmediate`, 94% of it `vkQueueWaitIdle` | 244.0 ms (34%) | 109 us | batch; constraints in `part75-kickoff.md` §1 |

**The untile loop is the target and it is embarrassingly parallel.** `UploadTexture`
(`vk_renderer.cpp`, the block after `const uint64_t texDecodeT0 = CycNow();`) walks
`for y in unitH { for x in unitW { CopySwapped(dst[y][x], src[Tiled2DOffset(...)]) } }` —
disjoint destination rows, a read-only source, and one `skipped` counter to reduce. A
row-range fork-join is the shape.

**Take the thread budget from `runtime/cpu/thread_budget.h`, not from
`hardware_concurrency()`** — the operator's rule is that cores are left free for them, the
budget counts PHYSICAL cores, and part 53's guard pool already asks for 4 and is clamped to
3 on their machine. A second pool that sizes itself independently is how a six-core box ends
up running twelve workers. Read `ThreadBudget_Take`'s header before adding a pool.

**Pre-registered kill: below 40 ms off the worst frame of the route, do not ship that
half.** Amortised the whole path is 0.020 ms/frame — it is a HITCH item, so a small win does
not justify touching the upload path.

**The gate is a picture gate, and it must be**, because this changes when and how pixels
arrive: `tools/part76_readback_gate.sh` is the wrong shape here. Use `CZ_VK_FRAME_DUMP` plus
the E3 correlation, and a texture census that a poisoned arm can be shown to move.

### ITEM 2 — THE GPU. **Promoted then partly demoted again — by the operator's own session.**

`part76-kickoff.md` had this last because `fence` was 0.00. Part 76's autonomous route made
it 2.99 and this file promoted it. **Their session then put it back in perspective: at their
draw count the fence is 0.00 and the CPU is 2.6 ms clear of the GPU.** The device is not what
bounds the frame they play.

It is still worth doing, and for a reason the numbers make plain rather than for its own
sake: **the GPU is FLAT at 8.5-11.9 ms across a 4x range of draw counts**, so it is a fixed
floor under everything, and it is 60-70% of the frame in every band below the crossover —
menus, interiors, the safehouse, and the whole 0-3,000 band where the CPU idles 2.87 ms. It
is also the ceiling every CPU item is measured against.

**2a. Find out where the GPU's ~11 ms goes — and §6dr §8 says where to look first.**

| draws (band median) | 1,157 | **2,484** | 4,222 | 6,236 | 8,142 | **9,208** |
|---|---|---|---|---|---|---|
| **GPU ms** | 6.97 | **9.26** | 8.69 | 9.61 | 11.35 | **12.20** |

**At 2,484 draws the GPU already costs 9.26 ms; at 3.7x the draws it costs 12.20.** About
three quarters of the device's cost in a full crowd is present in a light scene, and the
0-3,000 band's **4.15 ms fence wait** is the same fact from the CPU side. A cost that barely
moves with the draw count at a fixed resolution is **full-screen work — the post chain, the
resolves, the passes that run whatever is on screen** — not geometry, and it agrees with
§6dn's pixel scaling (7,986 / 10,167 / 19,856 us at 3.69 / 4.95 / 14.75 Mpx).

This project has never had a GPU-side breakdown; the frame trace gives one number for the
whole command buffer. **Timestamp queries per PASS would settle it, and this renderer already
writes two per frame** — the mechanism exists and needs extending, not inventing.

Do NOT quote a linear fit over those bands as a fixed/variable split. One was run: it returns
`4.46 ms + 0.891 per 1,000 draws` and it has not earned that shape — the series is not
monotone (9.26 at 2,484 falls to 8.69 at 4,222) because the bands are different CONTENT, not
merely different draw counts (`measurement.md` §4).

### ITEM 3 — PARALLEL COMMAND RECORDING. **Un-demoted for the crowd, and only for the crowd.**

`perf-state-parked.md` item A. `part76-kickoff.md` demoted it because the GPU was 54% of the
frame, and part 76's autonomous route seemed to bury it (fence 2.99 — no CPU headroom at
all). **The operator's session revives it in exactly one place**: at 9,000-12,000 draws the
CPU is 14.5 ms against an 11.9 ms GPU, so there are roughly **2.6 ms of frame time** available
to a CPU item before it hits the floor, and more at higher draw counts since the GPU is flat.

That is a real but bounded prize, and item 1 sits in front of it: the texture path is worth
9-17 ms on one marked frame in five at that same load. **Whoever picks this up must state the
fence wait and the GPU floor at the draw count they intend to measure, in the same sentence
as the expected saving — and must measure at 9,000+ draws**, because part 76 spent a six-run
A/B proving that a correct CPU fix measures exactly zero below the crossover.

## 1b. NOTHING IS OWED FROM PART 76

The profiler-free trace it asked for was run the same night (§6dr §6-9). **Part 76 is closed
with no measurement outstanding.**

## 2. RE-BASELINE BEFORE PRICING ANYTHING

The crowd frame's shape has changed twice in two parts and **every phase number in
`part76-kickoff.md` §2 was taken with the readback running**. Its `readback 3.49*` row is
gone; every other row is now a larger share of a smaller frame. The trace carries all
twenty-one phase columns — band it, and quote the RESOLUTION with every number.

## 3. THE MEASUREMENT METHOD — TWO TOOLS NOW, AND THEY ARE NOT INTERCHANGEABLE

`docs/part76-kickoff.md` §5 is still the checklist and every entry in it still holds (pin
`CZ_VK_RES`; read the route gate on a finished log; never send an A/B loop to `/dev/null`;
report the per-arm gate failure rate; `pgrep -x cz_runtime_auto`, never `pgrep -f`). What
changed is which reader to use:

* **`tools/part75_ab_report.py`** partitions runs by the MENU window as a machine-state
  fingerprint. Right for an item that affects the crowd and not the menu.
* **`tools/part76_band.py`** prints the menu window as a number and bands the crowd. Right
  for an item that affects EVERY presented frame — where the fingerprint is measuring the
  change, and `part75_ab_report.py` will refuse a perfectly good comparison rather than
  report it. It did exactly that to part 76's six runs. **Classify the change first**
  (gotcha 452).

The within-run profiled attribution is immune to both; prefer it, and use the A/B to
confirm rather than to discover.

## 4. WHAT IS RULED OUT — do not start these

* **the readback.** Done, measured, gated (§6dq).
* **the GPU as a source of felt STUTTER, and as the limiter of the frame they PLAY** — it
  is the limiter of the autonomous route, which is a different thing (§0b). Part 74's 10
  marks, part 75's 6 and part 76's **20** all had `fence 0.00`, now across three sessions
  and 36 marks. Nothing they feel is the device.
* **the guest side / outside the renderer.** Residual is 0.0 ms on every hitch frame (§6dm).
* **the constant GATHER and the constant MEMO**, and the whole constant path: 2.22 ms of a
  23.31 ms frame after part 75.
* **reverting the RT era for performance** — 0.5-0.7 ms (§6dj).
* **the crowd's steady-state frame as a source of STUTTER.** It has no tail, and after part
  76 it is a fifth faster than it was on this route.
* **further per-draw CPU micro-optimisation ON THIS ROUTE, or any CPU A/B below ~8,000
  draws.** Part 76 already ran that experiment and it measured zero against the null (§0b).
  Measure at 9,000+ draws or not at all.
* **quoting a regime verdict from a run with `CZ_VK_PROFILE` armed.** It costs +4.00 ms of
  CPU and +1.01 ms of wall, so it eats the slack and inverts the answer (gotcha 454). Use
  `CZ_VK_FRAME_TRACE` alone for that question.
* everything on part 73's list (`docs/perf-plan-autonomous.md`, exhausted).

## 5. THE TWO THINGS TO CARRY FORWARD

**One.** Part 75's cost hid under a phase NAME that described the other thing in the same
scope. Part 76's hid in a LAUNCHER, and its follow-on hid in the first operand of an `&&`.
None of the three was in the renderer's design — they were in what the shipping
configuration actually does. **Before pricing a new item, spend an hour reading what the
configuration you measure under is switching on.** Part 76 did that twice and both passes
found something: the launcher audit found the 3.5 ms, and a 157-site census of
`Env()`/`EnvOn()` found three per-draw `getenv`s that had been there since phase 5.

**Two, and it is the more expensive lesson.** The second of those was a correct fix, freely
made, whose every ingredient was measured — and it moved the frame by nothing, because the
FIRST fix had already taken the frame below the GPU floor an hour earlier. **Your own last
change is the most likely thing to have invalidated your next item's price.** Re-read the
floor after shipping, not before (gotcha 453). This is the same shape as gotcha 172 —
a retirement is only as good as the oracle it was measured on, and a performance floor is
an oracle.
