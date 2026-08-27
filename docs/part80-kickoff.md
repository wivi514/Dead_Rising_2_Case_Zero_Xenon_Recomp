# Part 80 kickoff — item 1 shipped and honestly small, item 2 REFUTED by measurement

> **THIS IS THE LIVE HAND-OFF**, superseding `part79-kickoff.md`.
>
> **Read `phase5-notes.md` §6dw and §6dx first.** §6dw is part 79 item 1 end to end — the
> upload ring, the A/B that removed the mechanism and moved the frame time by nothing, and
> the picture gate that raised a false alarm before it cleared. §6dx is item 2, answered and
> closed for the cost of one run: the post chain is real shading, not pass overhead.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6dx** | **part 79 item 2 — the PASS EXTENT CENSUS, and why the post chain is not addressable** |
> | **`phase5-notes.md` §6dw** | **part 79 item 1 — the flush stops waiting; the mechanism is gone and the route could not price it** |
> | `phase5-notes.md` §6dv | part 78's operator session — **the crossover moved to ~3,000 draws** |
> | `phase5-notes.md` §6du | part 78 — the first GPU-side breakdown, and the 137 barriers a frame |
> | `phase5-notes.md` §6dt | part 77's operator session — the batch's benefit is LOAD-SHAPED, and where item 1 came from |
>
> Lessons: gotchas **465-466**. There is still no live PLAN; §1 is the board, in order.

---

## 0. WHAT PART 79 DID, IN ONE PARAGRAPH

Item 1 — **`FlushTextureUploads` no longer waits.** Three upload slots, each with a command
buffer, a fence and a 32 MB segment of the staging arena; a flush submits into the current
slot and waits only on the slot it is about to reuse. **The flush went 999-1138 us to
106-114 us, −89.8%, and the staging half of the texture path went 78.7-83.8 ms/run to
17.1-18.1** — but **the run's frame time did not move**, because the autonomous route is
GPU-bound at 6,200 draws and the pump moved its blocking to the frame fence (median fence
0.699 -> 1.136 ms on the affected population). The item's value is at the operator's load,
where the flush count is 27x higher per second and the fence is 0.00; **nobody has measured
it there yet.** Item 2 — the post chain — was **closed by measurement**: a per-pass extent
census says three extents at 60-182 us carry 76% of the `1 draw` class and the pure-overhead
end (the six-step luminance pyramid down to 2x2, 7 us a pass) is 3.6% of it.

## 0b. THE THREE THINGS PART 79 WOULD TELL YOU BEFORE YOU START

**One. THE ONE THING OWED IS AN OPERATOR SESSION, AND `tools/part79_operator_session.sh` IS
READY TO RUN.** Item 1 is shipped, gated and committed, and the only number that can price it
does not exist. The script states its own pre-registered prediction — the 1,092.5 ms of
`vkQueueWaitIdle` §6dt measured in their 150 s session should now be ~0 — and it names what
would refute the reading (a flush still at ~1,000 us, a non-zero slot-stall count, or the
fence rising to absorb it). **It also says out loud that they should not expect to feel it**:
~0.59 ms on about one frame in six. Do not oversell it to them.

**Two. WHEN THE HAND-OFF SAYS YOUR ROUTE CANNOT PRICE THE ITEM, BELIEVE IT BEFORE THE RUNS,
NOT AFTER.** `part79-kickoff.md` §2 ruled out CPU A/Bs on the autonomous route below ~8,000
draws in as many words, and part 79 ran one at 6,200 draws anyway and got the null it was
promised. Nothing was lost — the mechanism measurement is the real evidence and the frame-time
null is itself a finding (gotcha 466) — but the honest plan was always "build it, measure the
MECHANISM, hand the frame time to the operator", and stating that up front would have been
better than discovering it in the table.

**Three. `STILL=1` IS THE PICTURE GATE ON THIS ROUTE, NOT THE TURNING CAMERA.** Part 79's
gate first read the shipping arm at **33.7x the null on `meanLuma`** for a change that cannot
alter a pixel. It was composition: the arms' era median draw counts were 4,573-4,851 against
3,866-3,915 on a run whose luma ramps 28.5 -> 68.5 -> 78.7. With the camera held the same
comparison reads 0.03% and 0.12% against a 0.50% null — INSIDE on every statistic — while the
positive control still reads 36,799x. **A null pair agreeing to 0.05% is one sample of a floor
that is really ~1.5%** (gotcha 465). Use `tools/part79_picture_gate.sh`, and add a `STILL=1`
pair to it before believing an alarm.

## 1. THE BOARD, IN ORDER

The GPU breakdown, re-measured in part 79 on the shipping renderer. Autonomous route,
3440x1440 internal, 17,820 frames, **residual 0.6%**, and it reproduces to 0.001 ms/frame
across two runs:

| region | ms/frame | regions/frame | ns each |
|---|---|---|---|
| **pass: >=256 draws** | 3.988 | 3.14 | 1,269,179 |
| **pass: 1 draw** | 0.987 | 30.18 | 32,707 |
| **resolve copy** | 0.723 | 49.47 | 14,618 |
| **resolve clear** | 0.601 | 82.38 | 7,300 |
| **pass: 2-255 draws** | 0.596 | 6.53 | 91,312 |
| present blit | 0.061 | 1.00 | 61,128 |
| resolve barriers | 0.066 | 98.93 | 669 |
| pass-begin barriers | 0.032 | 39.85 | 794 |
| snapshot views | 0.028 | 7.67 | 3,644 |
| cube face refresh | 0.008 | 2.14 | 3,538 |
| RESIDUAL | 0.045 | — | — |

### ITEM 0 — THE OTHER TWO SINGLE-FRAME SPIKES. **Small, unfelt, and the only unexplained thing left.**

Part 79's operator session had four isolated spikes with the same draw count, GPU time,
uploads and pipeline count as their neighbours and fence/sleep 0.00 (§6dy §3). **Two were the
stream store growing and are fixed** (§6dz). The other two — **50.0 ms at t=77.4 s and 60.3 ms
at t=93.2 s**, both far from any load — are a different cause, and **neither was felt**: their
threshold sits between 60 and 87 ms in a crowd.

**The trace cannot answer it.** The phase columns are `ProfScope`s and read zero without
`CZ_VK_PROFILE`, which costs 2-4 ms a frame and inverts the regime (gotcha 454). This needs an
**always-on split of the pump's walk** — cheap enough to leave on, coarse enough to cost
nothing. Then one operator session names it.

Take this first only because it is cheap and it closes a class; it is worth less than item 1.

### ITEM 1 — PARALLEL COMMAND RECORDING. **The largest thing left, and part 78 made it worth more.**

`perf-state-parked.md` item A, and `part79-kickoff.md` §1 item 5. It moves to the top because
everything above it on the last two boards is now shipped or refuted, and because §6dv §2
re-priced it: the operator's crowd is **CPU 12.71 ms against a GPU floor of 10.80 at
9,000-12,000 draws, i.e. 2.38 ms of headroom where part 76 measured 1.38**, and their fence
is **0.00 at every band from 3,000 draws up**.

**State the fence wait and the GPU floor at the draw count you intend to measure, in the same
sentence as the expected saving** — and measure it at 8,000+ draws or not at all (§6dr,
gotcha 453, and part 79 is the worked example of ignoring that).

`perf-plan-part55.md` §0 is the ceiling argument and it has not been retracted: the PM4 walk
is serial and draw ORDER is semantic, so the honest budget is 5-6 busy threads, not 16.

### ITEM 2 — THE RESOLVE CLEARS: 82.4 a frame, 580.5 Mpixel written for 33.3 rendered

0.601 ms, **94.3% of the writes removable in principle**, because `vkCmdClearColorImage`
takes a subresource range and not a rectangle so every clear takes the whole EDRAM stand-in.

**The ceiling is 0.601 ms and the obvious mechanism is a wash.** `CZ_VK_SCOPED_CLEAR` has
been an arm since part 32 and buys it by spending a render-scope cycle per clear at ~6.6 us
of CPU each — 82 a frame is 0.54 ms of pump time for 0.57 ms of GPU. **That trade is why
nobody should re-derive it.** The cheaper mechanism is `vkCmdClearAttachments` inside a pass
that is already open, which needs the clear moved from the resolve into the pass; whether the
title's ordering permits that is a question nobody has asked, and it is the first thing to
find out before any code.

### ITEM 3 — THE RESOLVE COPIES: 49.5 a frame, 49.4 Mpixel, 7.0 full EDRAM surfaces

0.723 ms. Not a slow copy — a lot of copying, and it is the price of serving the title's own
resolve destinations as sampled images. **The design question nobody has asked is whether
every resolve needs its snapshot copied on the frame it is produced**, or whether a
destination no draw samples this frame could defer. That needs a census of "resolve
destinations produced vs sampled, per frame", which the renderer can answer and which is the
same shape as part 79's extent census — cheap, and it either names an item or kills one.

### ITEM 4 — THE UNTILE LOOP, still SMALL and still fully specified. §6ds §10 unchanged.

79 ms/run, ~25 ms on a burst frame, against a standing 40 ms kill.
`tools/tile_offset_separable.py` proves the decomposition over 967,680 combinations. It is
shovel-ready and it is not worth the shovel yet.

## 2. WHAT IS RULED OUT — do not start these

* **THE POST CHAIN** (`pass: 1 draw` + `pass: 2-255`, 1.58 ms, 20% of the device frame).
  Part 79 censused it by extent: three extents at 60-182 us carry 76% of the `1 draw` class,
  they are the title's own shaders at half, quarter and full resolution, and per pixel the
  expensive one costs 3.4x the cheap one — **it is shading, not pass overhead.** The
  pure-overhead end totals 0.036 ms/frame. Nothing a translation layer does can reach it.
  §6dx.
* **more batching of the texture uploads** (§6dt §3), **the texture path as a HITCH** (§6dt),
  **pipeline compilation on the load frame** (8.8 ms of a 158 ms burst frame, §6du §5),
  **the readback** (§6dq), **the constant path** (§6dp), **the guest side** (§6dm),
  **reverting the RT era** (§6dj), and everything on part 73's exhausted list.
* **a fence or any other wait primitive for the per-upload submits** — part 73 measured three
  arms (gotcha 436). Part 79's ring is a different change: it removes the wait rather than
  replacing the primitive, and it is done.
* **`CZ_VK_VALIDATION=1` as the gate** for anything in the texture upload path (§6ds §9) or
  for anything about a barrier (§6du §4 — use `CZ_VK_SYNC_VALIDATION=1`).
* **any CPU A/B below ~8,000 draws on the autonomous route.** Part 79 is the second demonstration.

## 3. THE MEASUREMENT METHOD — five readers now

**Classify the change first** (gotcha 452):

* **`tools/part75_ab_report.py`** — menu window as a machine-state fingerprint. For an item
  that affects the crowd and not the menu.
* **`tools/part76_band.py`** — menu as a NUMBER, crowd in matched 250-draw bands. For an item
  that affects every presented frame. Part 78 used this.
* **`tools/part77_tex_report.py`** — population is frames with a texture upload. For a HITCH
  item. **Part 79 found its blind spot**: the flush's cost is charged to the frame that
  SUBMITS it, which is often the frame after the uploads, so ~70% of it lands on rows reading
  `texUploads == 0`. For an upload-path item, population on `texUpUs > 0` instead.
* **`CZ_VK_GPU_PASSES=1`** — for anything that changes what the DEVICE does, this is the
  primary evidence and the wall clock is the secondary. **It now carries the pass EXTENT
  CENSUS** (§6dx), which reproduces to 0.001 ms/frame — far tighter than any frame-time band.
* **`tools/part79_picture_gate.sh`** — four arms, `CZ_VK_TEX_BATCH_BREAK=1` as the positive
  control. **Add a `STILL=1` pair** before believing anything it says about `meanLuma`.

`tools/part79_flush_ab.sh` and `tools/part78_barrier_ab.sh` are the drivers to copy. Both
reject a failed run **BY NAME**. Everything in `part76-kickoff.md` §5 still holds.

## 4. WHAT IS OWED — NOTHING

Part 79's operator session ran (§6dy, §6dz). The pre-registered prediction held on every
clause — the flush went 598 us to 54 us per flush at their load, 0 slots stalled, and the
fence did not absorb it — and the frame rate was unchanged, which is what they were told to
expect before they played. Their picture verdict was *"look identical"*.

**And their one complaint turned out to be the most valuable thing in the session.** *"Only
felt hitches at the start right after loading"* localised to two frames 1.3 and 1.4 seconds
after a load burst, which were the cross-frame stream store growing; that is measured
(71.7 ms in one frame, split waits 13.6 / allocate 42.9 / free 15.2) and fixed by starting the
store at 512 MB (§6dz). **Re-ask them whether the after-load hitch is gone** — that is the one
outstanding question, and it is a yes/no.

~~**One thing, and it is the operator's session.**~~ `tools/part79_operator_session.sh`. Item 1's
frame-time value has never been measured in the regime where it exists, and the run also
carries the extent census at their resolution, where the post chain is a larger share of the
GPU frame than it is on my route (14.4% against 9.9%, §6dv §4).

Their PICTURE verdict is worth collecting explicitly even though the change cannot alter a
pixel: this project has twice shipped a defect only their eye could see with every automatic
check green (§6bo's lightmap transposition, part 60's overlay).
