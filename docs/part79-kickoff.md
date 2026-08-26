# Part 79 kickoff — the GPU has a breakdown now; the board is what it named

> **THIS IS THE LIVE HAND-OFF**, superseding `part78-kickoff.md`.
>
> **Read `phase5-notes.md` §6du first.** It is part 78 end to end: the per-region GPU
> instrument, the eleven-row breakdown it produced, the barrier fix that came out of it, the
> synchronization-validation gate that is new here, and item 3 priced dead in two greps.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6du** | **part 78 — the first GPU-side breakdown, and the 137 barriers a frame it found** |
> | `phase5-notes.md` §6dt | part 77's operator session — the batch's benefit is LOAD-SHAPED |
> | `phase5-notes.md` §6ds | part 77 — the texture path |
> | `phase5-notes.md` §6dr | part 76's operator session — **the TWO REGIMES, and where to measure a CPU item** |
> | `part77-kickoff.md` §0b | the regime table — still current |
>
> Lessons: gotchas **459-463**. There is still no live PLAN; §1 is the board, in order.

---

## 0. WHAT PART 78 DID, IN ONE PARAGRAPH

`part78-kickoff.md` §1 item 1 was **the GPU** — "the largest thing nobody has ever looked
at" — and it was a MEASUREMENT item, not a fix item, because this renderer wrote two
timestamps a frame and they bracketed the whole command buffer. `CZ_VK_GPU_PASSES=1` now
splits the frame's device time by region, with its residual printed first. It said that
**a third of the device's frame is the EDRAM emulation's overhead rather than the title's
rendering**, and that the largest single item in that third was something nobody had ever
priced: **137.6 image layout transitions a frame, all of them
`ALL_COMMANDS -> ALL_COMMANDS` with `MEMORY_READ | MEMORY_WRITE`, costing 0.930 ms of an
8.49 ms GPU frame.** Deriving each barrier's masks from its layouts is **−11.9% at crowd
load on the autonomous route** (94.9 -> 107.6 fps) against a +1.0% null floor.

## 0b. THE THREE THINGS PART 78 WOULD TELL YOU BEFORE YOU START

**One. Do not build a split that cannot report that it is wrong.** The obvious design for a
per-region GPU breakdown is a chain of timestamps at every boundary: it partitions the frame
exactly and every nanosecond is accounted for. That last property is the defect — a chain
has no residual, so it will confidently charge unwrapped work to whichever class comes next.
The instrument shipped as explicit (begin, end) pairs with the gap printed FIRST, and the
residual moving 16.7% -> 3.5% is how the barriers became the finding instead of staying
invisible inside the copy class (gotcha 459).

**Two. `CZ_VK_SYNC_VALIDATION=1` EXISTS NOW AND IT IS A DIFFERENT INSTRUMENT FROM
`CZ_VK_VALIDATION=1`.** The ordinary layer checks that a call is legal; this one checks that
a memory dependency covers the accesses on either side of it. It found a pre-existing
undefined-ordering defect in the resolve clears that no picture instrument in this project
could ever have seen. **It is slow enough to change the route** — the first three attempts
peaked at 2,538 draws and the route gate correctly refused them; use
`CZ_VK_RES=1280x720 PRESSMS=9000 SECS=45 TIMEOUT=420`, which reaches 5,268 draws.
`CZ_VK_BARRIER_POISON=1` is its positive control and it must produce 30 hazards.

**Three. The regime table in `part77-kickoff.md` §0b is unchanged and still governs.** The
autonomous route is GPU-bound below ~6,000-7,000 draws and the operator's crowd is CPU-bound
above it. Part 78's saving is entirely on the GPU, so **it may reach the operator's crowd
much less than it reaches this route** — that is the open question and it is §4 below.

## 1. THE BOARD, IN ORDER

The breakdown, so every item below has its number attached. Autonomous route, 3440x1440
internal, 16,907 frames, residual 3.5%, **after** the barrier fix takes 0.930 -> 0.099:

| region | ms/frame | regions/frame | ns each |
|---|---|---|---|
| **pass: >=256 draws** | 4.224 | 3.11 | 1,358,271 |
| **pass: 1 draw** | 0.838 | 29.87 | 28,051 |
| **resolve copy** | 0.712 | 49.04 | 14,527 |
| **resolve clear** | 0.691 | 81.65 | 8,468 |
| **pass: 2-255 draws** | 0.592 | 6.50 | 91,021 |
| snapshot views | 0.114 | 7.60 | 14,987 |
| present blit | 0.063 | 1.00 | 63,170 |
| cube face refresh | 0.029 | 2.07 | 13,990 |
| barriers (after part 78) | 0.099 | 137.6 | ~700-800 |
| RESIDUAL | 0.294 | — | — |

### ITEM 1 — THE POST CHAIN: 36 passes a frame, 1.43 ms, never decomposed

`pass: 1 draw` is **29.87 passes a frame at 28 us each**, and that is *after* the
pass-opening barriers are counted separately. `pass: 2-255` adds 6.50 more at 91 us.
Together **1.43 ms/frame, 17% of the device's frame, in passes that are not the crowd** —
which is very likely what part 76 was seeing as a floor that does not scale with draw count
(9.26 ms at 2,484 draws against 12.20 at 9,208).

**Nobody knows what they ARE.** They are one full-screen-ish draw each, they are almost
certainly the title's post chain (the bloom pyramid part 25's validation run named — 96x45,
64x22, 32x11, 32x5, 32x2, 32x1 — plus DoF, tone map and the LUT), and this project has never
listed them. **Price them by EXTENT before designing anything**: the instrument already
carries a per-region hook, so adding the scissor's pixel count to the pass classes is a
one-line extension and it will say immediately whether 28 us is a full-screen shader or pass
overhead on a tiny target. If it is the latter, the item is the same shape as the barriers.

### ITEM 2 — DROP THE WAIT IN `FlushTextureUploads`. **Carried over from `part78-kickoff.md` §1 item 2, UNTOUCHED, and it is a CPU item.**

`FlushTextureUploads` submits **and waits**. At the operator's load that is **1,092.5 ms of a
150-second session**, 568 us per flush across 1,841 flushes, and unlike batching it does not
care how the uploads are distributed (§6dt §3). The wait exists only so the staging arena and
the command buffer can be reused immediately; fence them against the frame instead.

**It is the only remaining item that is definitely worth something at the OPERATOR's load**,
because their crowd is CPU-bound above ~8,000 draws and part 78's whole saving is on the GPU.

Part 77 deliberately did not do it — recycling two resources against a fence is a second
mechanism to get wrong — and part 78 did not either. It is now the oldest open item.

**Gate it on the picture, not on validation** (gotcha 458); `CZ_VK_TEX_BATCH_BREAK=1` is the
positive control that proves whatever you choose can fail.

### ITEM 3 — THE RESOLVE CLEARS: 575 Mpixel written a frame for 33 rendered

81.65 clears a frame, 0.691 ms, and **every one takes the whole EDRAM stand-in** because
`vkCmdClearColorImage` accepts a subresource range and not a rectangle. The passes they
follow rendered 32.85 Mpixel of the 575.29 written — **scoping would remove 94.3% of the
writes.**

**But the class is only 0.691 ms**, so the ceiling is that, and the depth-scoped form already
in the tree (`CZ_VK_SCOPED_CLEAR`, an arm since part 32) buys it by spending a render-scope
cycle per clear, which §4b prices at ~6.6 us of CPU each — 81 a frame is 0.54 ms of pump time
to save 0.65 ms of GPU. **That trade is a wash and the arm is why nobody should re-derive
it.** The cheaper mechanism is `vkCmdClearAttachments` inside the pass that is already open,
which needs the clear moved from the resolve into the pass; whether that is possible is a
question about the title's ordering and nobody has asked it.

### ITEM 4 — THE RESOLVE COPIES: 48.90 Mpixel a frame, 6.9 full EDRAM surfaces

0.712 ms over 49.04 copies. Not a slow copy — a lot of copying, and it is the price of
serving the title's own resolve destinations as sampled images. **The design question nobody
has asked is whether every resolve needs its snapshot copied on the frame it is produced**,
or whether a destination no draw samples this frame could defer. That needs a census of
"resolve destinations produced vs sampled, per frame", which the renderer can answer.

### ITEM 5 — PARALLEL COMMAND RECORDING, at 9,000+ draws only.

`perf-state-parked.md` item A, unchanged. **Re-price it first**: part 78 took ~1.2 ms off the
frame and part 76's CPU/GPU balance at the operator's load was CPU 14.50 against GPU 11.87,
so the headroom has moved. State the fence wait and the GPU floor at the draw count you
intend to measure, in the same sentence as the expected saving.

### ITEM 6 — THE UNTILE LOOP, still SMALL. `part78-kickoff.md` §1 item 4 and §6ds §10 unchanged.

## 2. WHAT IS RULED OUT — do not start these

* **pipeline compilation on the load frame.** Part 78 priced it: **8.8 ms of a 158-165 ms
  burst frame, 5.6%**, run total 29.6 ms. The standing kill is 40 ms off the worst frame.
  Dead. (§6du §5.)
* **the texture path as a HITCH** (§6dt), **further BATCHING of the uploads** (§6dt §3),
  **the readback** (§6dq), **the constant path** (§6dp), **the guest side** (§6dm),
  **reverting the RT era** (§6dj), and everything on part 73's exhausted list.
* **a fence or any other wait primitive for the per-upload submits** — part 73 measured
  three arms (gotcha 436). Item 2 above is a different change: it removes the wait, it does
  not replace the primitive.
* **`CZ_VK_VALIDATION=1` as the gate for anything in the texture upload path** (§6ds §9) or
  for anything about a barrier (§6du §4 — use `CZ_VK_SYNC_VALIDATION=1`).
* **any CPU A/B below ~8,000 draws on the autonomous route** (§6dr, gotcha 453).

## 3. THE MEASUREMENT METHOD — four readers now

**Classify the change first** (gotcha 452):

* **`tools/part75_ab_report.py`** — menu window as a machine-state fingerprint. For an item
  that affects the crowd and not the menu.
* **`tools/part76_band.py`** — menu as a NUMBER, crowd in matched 250-draw bands. For an item
  that affects every presented frame. **Part 78 used this.**
* **`tools/part77_tex_report.py`** — population is frames with a texture upload, headline is
  the burst frame. For a HITCH item.
* **`CZ_VK_GPU_PASSES=1` itself** — for anything that changes what the DEVICE does, this is
  the primary evidence and the wall clock is the secondary. Part 78's barrier classes moved
  0.938 -> 0.099 ms/frame with a spread of 0.001 ms across three runs an arm, which is a far
  tighter measurement than any frame-time band.

`tools/part78_barrier_ab.sh` is the driver to copy. **It rejects a failed run BY NAME** —
renaming its log to `.rejected` — because the first pass of part 78's own A/B printed "DID
NOT REACH THE OUTDOOR WORLD" and then globbed that log into the comparison anyway, where it
contributed 26 extra menu windows to one arm and none to the other. A message on the terminal
is not a drop.

Everything in `part76-kickoff.md` §5 still holds, plus part 77's two: a trace with no `.rc`
beside it is a run that has not finished, and do not quote the run's overall maximum frame.

## 4. WHAT IS OWED

**The operator's verdict on part 78, and it is a real question rather than a formality.**
`tools/part78_operator_session.sh` is armed with `CZ_VK_GPU_PASSES` (free) and
`CZ_VK_FRAME_TRACE` (one line a frame) and deliberately NOT with `CZ_VK_PROFILE`, which
inverts the regime (gotcha 454).

The question: **part 78's saving is entirely on the GPU, and their crowd was CPU-bound above
~8,000 draws in part 76.** So the honest prediction is that they see the full −12% below
about 8,000 draws and much less above it — and if they see the full saving at 9,000+, the
regime table is wrong and that is a bigger finding than the fix. Their session's own
per-region split is what settles it, and it costs nothing to collect.
