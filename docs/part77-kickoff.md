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
> Lessons: gotchas **451-452**. There is still no live PLAN; **§1 is the board, in order.**

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

### ITEM 2 — PARALLEL COMMAND RECORDING. Still demoted, and the arithmetic moved AGAIN.

`perf-state-parked.md` item A. Part 76 took ~2.1 ms of CPU off the frame, so the headroom
below the GPU floor is smaller than the 10.7 ms `part76-kickoff.md` quoted, not larger.
**Whoever picks this up must state the GPU floor in the same sentence as the expected
saving** — and must re-measure that floor, because the operator session it came from
(12.57 ms GPU of a 23.31 ms frame) was taken with the readback still running, so the GPU's
SHARE of the frame is now higher than 54% even though its milliseconds have not moved.

### ITEM 3 — THE GPU, which this project has still never touched.

`fence` is still 0.00, so it is not the limiter — but it is the ceiling every CPU item is
measured against, and part 76 raised its share of the frame without changing its cost.
`CZ_VK_RES` is the one lever already built.

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
* **the GPU as a source of felt stutter.** All 10 of the operator's part-74 marks and all 6
  of part 75's had `fence 0.00` and a healthy GPU.
* **the guest side / outside the renderer.** Residual is 0.0 ms on every hitch frame (§6dm).
* **the constant GATHER and the constant MEMO**, and the whole constant path: 2.22 ms of a
  23.31 ms frame after part 75.
* **reverting the RT era for performance** — 0.5-0.7 ms (§6dj).
* **the crowd's steady-state frame as a source of STUTTER.** It has no tail, and after part
  76 it is a third faster than it was.
* everything on part 73's list (`docs/perf-plan-autonomous.md`, exhausted).

## 5. THE ONE THING TO CARRY FORWARD

Part 75's cost hid under a phase NAME that described the other thing in the same scope.
Part 76's hid in a LAUNCHER, and its follow-on hid in the first operand of an `&&`. All
three were free to find and none of them was in the renderer's design — they were in what
the shipping configuration actually does. **Before pricing a new item, spend an hour
reading what the configuration you measure under is switching on.** Part 76 did that twice
and both passes paid: the launcher audit found the 3.5 ms, and a 157-site census of
`Env()`/`EnvOn()` found three per-draw `getenv`s that had been there since phase 5.
