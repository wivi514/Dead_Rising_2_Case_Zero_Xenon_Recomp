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
> Lessons: gotchas **451-453**. There is still no live PLAN; **§1 is the board, in order.**
>
> **AND READ §0b BEFORE PRICING ANY CPU ITEM.** Part 76 crossed a floor: on the autonomous
> route at 3440x1440 the GPU is now **100% of the wall** and the CPU idles ~3 ms a frame.

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

**SCOPE — this does not automatically transfer to the operator.** This route is 6,000-7,000
draws and they play at ~9,750. GPU cost scales with PIXELS, CPU with DRAWS, and the same
run's 7,000-9,000 band already reads **CPU 10.02 against GPU 12.12** where 5,000-6,000 reads
7.00 against 10.24. Their frame is probably near-balanced rather than GPU-bound.

**So, operationally:** run `CZ_VK_FRAME_TRACE` and read `fence` and `gpuUs` BEFORE pricing a
CPU item, and say which regime the number was taken in. An item worth 3 ms of CPU on this
route is worth 0 ms of frame time on it and possibly 3 ms of frame time on theirs.

## 1. THE BOARD, IN ORDER

### ITEM 1 — THE TEXTURE PATH. The only thing the operator still FEELS.

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

### ITEM 2 — THE GPU. **PROMOTED, by §0b, and this project has still never touched it.**

`part76-kickoff.md` had this last, on the reasoning that `fence` was 0.00 so the GPU was not
the limiter. **On the autonomous route it now is** — 100% of the wall, 2.99 ms of fence wait
— and it is the first time in this port's history that the device has been the thing to
work on. It is not certain to be the operator's limiter (§0b's scope note), so the first
piece of work is not an optimisation:

**2a. Find out where the GPU's 10.5 ms goes.** This project has never had a GPU-side
breakdown: the frame trace gives one number for the whole command buffer. The lever already
built is `CZ_VK_RES` (part 51: resolution scaling is nearly free in crowds and expensive in
light zones) and the fact that GPU cost tracks PIXELS is already measured (7,986 / 10,167 /
19,856 us at 3.69 / 4.95 / 14.75 Mpx, §6dn) — which points at fill rate and the post chain
rather than at geometry. Timestamp queries per PASS would say it outright and this renderer
already writes two per frame.

**2b. Then ask what the operator's regime is**, with the same trace at their draw count.

### ITEM 3 — PARALLEL COMMAND RECORDING. **Demoted again, and now by two measurements.**

`perf-state-parked.md` item A led the board for four parts. `part76-kickoff.md` demoted it
because the GPU was 54% of the frame; part 76 makes that worse, not better — on the
autonomous route there is **no CPU headroom left at all** (fence 2.99 ms), and part 76 spent
a six-run A/B demonstrating exactly what a CPU item measures in that regime. It is not dead:
at the operator's ~9,750 draws the CPU is close to the GPU again, so it may still be worth
milliseconds THERE. **Whoever picks it up must state the fence wait and the GPU floor at the
draw count they intend to measure, in the same sentence as the expected saving** — and must
measure it at that draw count, not on this route.

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
* **the GPU as a source of felt STUTTER** — but NOT as the throughput limiter, which it now
  is on this route (§0b). All 10 of the operator's part-74 marks and all 6 of part 75's had
  `fence 0.00` and a healthy GPU, so nothing they FEEL is the device. That is a statement
  about the hitches, not about the frame rate, and part 76 separated the two.
* **the guest side / outside the renderer.** Residual is 0.0 ms on every hitch frame (§6dm).
* **the constant GATHER and the constant MEMO**, and the whole constant path: 2.22 ms of a
  23.31 ms frame after part 75.
* **reverting the RT era for performance** — 0.5-0.7 ms (§6dj).
* **the crowd's steady-state frame as a source of STUTTER.** It has no tail, and after part
  76 it is a fifth faster than it was on this route.
* **further per-draw CPU micro-optimisation ON THIS ROUTE.** Part 76 already ran that
  experiment and it measured zero against the null (§0b). Measure at the operator's draw
  count or not at all.
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
