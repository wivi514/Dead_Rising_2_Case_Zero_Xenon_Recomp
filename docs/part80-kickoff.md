# Part 80 kickoff — the board after part 79 shipped two items and refuted a third

> **THIS IS THE LIVE HAND-OFF**, superseding `part79-kickoff.md`.
>
> **Read `phase5-notes.md` §6dw, §6dx, §6dy, §6dz and §6ea, in that order.** They are part 79
> end to end: the upload ring, the pass extent census that refuted the post-chain item, the
> operator session that confirmed the ring, the stream-store growth that session found, and
> the confirmation run.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6ea** | **the third session — 0 growths, 0 unexplained spikes, AND NO OPERATOR VERDICT. Its §4 is the regime table to plan against.** |
> | **`phase5-notes.md` §6dz** | **the stream store's growth: found, measured, mis-fixed once, then fixed structurally** |
> | **`phase5-notes.md` §6dy** | **part 79's operator session — the ring confirmed, and the hitch class it exposed** |
> | `phase5-notes.md` §6dx | the PASS EXTENT CENSUS — the post-chain item, refuted |
> | `phase5-notes.md` §6dw | part 79 item 1 — the flush stops waiting |
> | `phase5-notes.md` §6dv §2 | part 78's session — where the crossover moved |
>
> Lessons: gotchas **465-472**. There is still no live PLAN; §1 is the board, in order.

---

## 0. WHAT PART 79 DID, IN ONE PARAGRAPH

**Item 1 shipped**: `FlushTextureUploads` submits into a three-slot ring and no longer waits —
**598 us -> 54 us per flush at the operator's load, −91%**, 0 slots stalled, and the fence did
not absorb it. It is worth **0.05 ms a frame**, which is what was predicted and said to them
before they played. **Item 2 was refuted by measurement** for the cost of one run: a per-pass
extent census showed the post chain is the title's own shaders at half, quarter and full
resolution, not pass overhead. **And then the operator session found the thing that actually
mattered** — two hitches they *felt*, 1.3 seconds after a load, which were the **cross-frame
stream store doubling itself: 71.7 ms of pump time in a single frame**. Raising its start
128 -> 512 removed those and left one 329.2 ms growth late in the run (which they also felt and
located); it now starts at `kPersistCeiling` so growth is **impossible**, and the third
session has 0 growths and **no unexplained single-frame spikes at all**.

## 0b. THE FOUR THINGS PART 79 WOULD TELL YOU BEFORE YOU START

**One. THE OPERATOR'S VERDICT ON THE FINAL BUILD WAS NEVER COLLECTED.** Sessions 1 and 2 were
confirmed by their own report — *"only felt hitches at the start right after loading"*, then
*"a single stutter near the end but didn't feel one after loading in"* — and both mapped onto
the data exactly. Session 3 has clean data and no eye (§6ea §2). **Ask in one sentence at the
start of the next session**: after loading, and late in a crowd — anything? That is the only
thing owed.

**Two. CLASSIFY A SLOW FRAME BY WHERE THE TIME WENT, NOT BY WHICH COUNTER IS NON-ZERO.** The
158.4 ms frame that led to the whole stream-store finding carried four texture uploads worth
**0.1 ms**; a presence test files it as a texture frame and the trail ends. The taxonomy that
worked is: **pump sleep** (26-64 draws, sleep is 97-99% of the frame — menus and load
transitions), **load** (texture + pipelines are most of the frame), and **unexplained CPU
recording** (draws, GPU, uploads and pipelines all identical to the neighbouring frames, fence
0.00, sleep 0.00). That third class is never a workload — it is an allocation, a growth, a
rehash or a free (gotcha 467).

**Three. PUT A CLOCK ON EVERY PATH WHOSE COMMENT SAYS IT RUNS RARELY, AND SPLIT IT BEFORE
FIXING IT.** `PersistMaintenance`'s comment said *"it runs at most a handful of times a run"*,
which is true and is why it went unmeasured for 22 parts while being the only thing the
operator could feel. And when it was measured, the split changed the fix twice: the two
`vkDeviceWaitIdle`-class waits are the **smallest** of its three terms (13.6 of 71.7), so the
obvious principled repair buys 19%; and the cost scaling with the NEW buffer size meant a
bigger start only skips the cheap growths (gotchas 468, 470).

**Four. NAME THE REFUTATIONS IN THE HARNESS.** `tools/part80_operator_session.sh` listed three
things that would refute part 79's attribution. **The first one fired**, and because it was
written down the operator's one-sentence report landed on a pre-registered branch instead of
starting a debugging session (gotcha 472). Keep doing this.

## 1. THE BOARD, IN ORDER

The GPU breakdown from the operator's own third session — 11,677 frames, residual 0.8%, and
the extent census reproduces across machines and loads to 0.001 ms/frame:

| region | ms/frame | share | regions/frame |
|---|---|---|---|
| **pass: >=256 draws** | 3.385 | 51.4% | 2.44 |
| **pass: 1 draw** | 0.922 | 14.0% | 28.41 |
| **resolve copy** | 0.699 | 10.6% | 46.73 |
| **pass: 2-255 draws** | 0.645 | 9.8% | 6.42 |
| **resolve clear** | 0.568 | 8.6% | 77.93 |
| present blit | 0.125 | 1.9% | 1.00 |
| resolve barriers | 0.110 | 1.7% | 93.45 |
| pass-begin barriers | 0.045 | 0.7% | 37.27 |
| snapshot views + cube | 0.033 | 0.5% | 8.81 |
| RESIDUAL | 0.051 | 0.8% | — |

**And the regime, which is what actually ranks the board** (§6ea §4): the fence is **0.00 at
every band from 5,000 draws up**, and the headroom between the wall and the GPU is **2.40 ms
at 7,000-9,000 draws and 3.06 at 9,000-12,000**. A CPU saving at their crowd converts to frame
time roughly 1:1 up to about 2.4-3.0 ms. **That is why the board leads with a CPU item.**

### ITEM 1 — PARALLEL COMMAND RECORDING. The largest thing left, and three parts have re-priced it upward.

`perf-state-parked.md` item A. Their crowd is CPU 13.16 ms against a GPU floor of 10.58 at
9,000-12,000 draws, fence 0.00. Part 76 measured 1.38 ms of headroom there, part 78's session
1.93-2.38, and part 79's **2.40-3.06**.

**Measure it at 8,000+ draws or not at all.** Part 79 is the second worked example of what
happens otherwise: a CPU item measured on the autonomous route at 6,200 draws read as a dead
null because the route is GPU-bound and the pump moved its blocking to the fence (§6dw §3,
gotchas 453 and 466). **State the fence wait and the GPU floor at the draw count you intend to
measure, in the same sentence as the expected saving.**

`perf-plan-part55.md` §0 is the ceiling argument and has not been retracted: the PM4 walk is
serial and draw ORDER is semantic, so the honest budget is 5-6 busy threads, not 16.

### ITEM 2 — THE LAST UNEXPLAINED HITCH CLASS. Cheap, closes a class, and needs one instrument.

Session 1 had two spikes that were **not** growths and were **not** felt — 50.0 ms at t=77.4 s
and 60.3 ms at t=93.2 s, both far from any load, both with the isolated-single-frame signature.
Session 3 showed none, but it ran 88.5 s against the 96.8 s in which those appeared, so
**absence there is weak evidence** (§6ea §3).

**The trace cannot answer it.** The phase columns are `ProfScope`s and read zero without
`CZ_VK_PROFILE`, which costs 2-4 ms a frame and inverts the regime (gotcha 454). This needs an
**always-on, coarse split of the pump's walk** — cheap enough to leave on in every session,
which is the property that matters, because the events are two per session and nobody can
predict when. Then one operator run names it.

Their threshold is a useful bound while designing: they felt 87 and 158 and 352 ms and did not
notice 50 or 60, in a crowd.

### ITEM 3 — THE UNTILE LOOP. **Re-priced UPWARD by the operator's session and no longer the small item.**

§6ds §10 priced this as small against a standing 40 ms kill, and that was measured on the
autonomous route where the staging half dominated. **At the operator's load the texture path
has flipped**: staging+submit is now 79.6 ms of a 387 ms path (17%, where §6dt measured 77%),
and the untile loops are **113.3 + 64.0 = 177.3 ms, 57.7% of the decode and the largest single
term in the whole path** (§6dy §5). The mip guards are another 98.8 ms (32.2%).

The work is fully specified and proven: `Tiled2DOffset` decomposes into a 32x32 table plus a
macro-tile base, **967,680 combinations checked, 0 mismatches**
(`tools/tile_offset_separable.py`), with units contiguous in 8-unit / 16-byte runs.

**But price it against the LOAD FRAME before building it**, which is the frame it can reach:
their biggest load frame is 111.2 ms carrying 62.0 ms of texture work, of which the untile is
~58%, so a 3x buys ~24 ms of a 111 ms frame. The standing kill has been 40 ms off the worst
frame throughout. It is bigger than it was and it may still not clear the bar — **do the
arithmetic before the work, which is the thing part 79 failed to do on the 512 MB fix**
(gotcha 470).

### ITEM 4 — THE RESOLVE CLEARS: 77.9 a frame, 549 Mpixel written for 31 rendered

0.568 ms, **94.3% of the writes removable in principle**, because `vkCmdClearColorImage` takes
a subresource range and not a rectangle so every clear takes the whole EDRAM stand-in.

**The ceiling is 0.568 ms and the obvious mechanism is a wash.** `CZ_VK_SCOPED_CLEAR` has been
an arm since part 32 and buys it by spending a render-scope cycle per clear at ~6.6 us of CPU
each — 78 a frame is 0.51 ms of pump time for 0.54 ms of GPU, **and at their load pump time is
worth MORE than GPU time** (fence 0.00, 2.4-3.0 ms of headroom), so that trade is now actively
bad rather than merely even. **Do not re-derive it.** The cheaper mechanism is
`vkCmdClearAttachments` inside a pass that is already open, which needs the clear moved from
the resolve into the pass; whether the title's ordering permits that is the question to answer
first, and it is a reading task, not a building one.

### ITEM 5 — THE RESOLVE COPIES: 46.7 a frame, 46.3 Mpixel, 6.6 full EDRAM surfaces

0.699 ms. Not a slow copy — a lot of copying, and it is the price of serving the title's own
resolve destinations as sampled images. **The design question nobody has asked is whether
every resolve needs its snapshot copied on the frame it is produced**, or whether a destination
no draw samples this frame could defer. That needs a census of "resolve destinations produced
vs sampled, per frame", which the renderer can answer. Same shape as part 79's extent census:
cheap, and it either names an item or kills one.

## 2. WHAT IS RULED OUT — do not start these

* **THE POST CHAIN** (`pass: 1 draw` + `pass: 2-255`, 1.57 ms, 24% of the device frame).
  Refuted by extent census (§6dx), and **reproduced at the operator's resolution** (§6dy §4):
  three extents at 60-182 us carry 76% of the `1 draw` class, they are the title's own shaders
  at half, quarter and full resolution, and per pixel the expensive one costs 3.4x the cheap
  one. The pure-overhead end — the six-step luminance pyramid down to 2x2 at 7 us a pass —
  totals 0.034 ms/frame. Nothing a translation layer does can reach it.
* **the texture upload SUBMIT path.** Part 77 batched it, part 79 removed its wait; the flush
  is 53-54 us and 0 slots stall. What is left in that path is the DECODE, which is item 3.
* **a fence or any other wait primitive for the per-upload submits** — part 73 measured three
  arms (gotcha 436), and part 79's ring removed the wait rather than replacing the primitive.
* **growing the stream store more gracefully.** It cannot grow: it starts at `kPersistCeiling`.
  If a session ever prints `stream store is at its 1024 MB ceiling and a frame still overran
  it`, THAT is a new item (the cache is being dropped and refilled) — but it has never fired.
* **pipeline compilation on the load frame** (8.8 ms of a 158 ms burst frame, §6du §5),
  **the readback** (§6dq), **the constant path** (§6dp), **the guest side** (§6dm),
  **reverting the RT era** (§6dj), and everything on part 73's exhausted list.
* **`CZ_VK_VALIDATION=1` as the gate** for anything in the texture upload path (§6ds §9) or
  for anything about a barrier (§6du §4 — use `CZ_VK_SYNC_VALIDATION=1`).
* **any CPU A/B below ~8,000 draws on the autonomous route.** Part 79 is the second
  demonstration and it cost a six-run campaign to re-learn.

## 3. THE MEASUREMENT METHOD — five readers, and one that needs building

**Classify the change first** (gotcha 452):

* **`tools/part76_band.py`** — menu as a number, crowd in matched 250-draw bands. For an item
  that affects every presented frame.
* **`tools/part75_ab_report.py`** — menu window as a machine-state fingerprint. For an item
  that affects the crowd and not the menu.
* **`tools/part77_tex_report.py`** — population is frames with a texture upload. For a HITCH
  item. **Known blind spot**: the flush's cost is charged to the frame that SUBMITS it, often
  the frame after the uploads, so ~70% of it lands on rows reading `texUploads == 0`. For an
  upload-path item, population on `texUpUs > 0` instead.
* **`CZ_VK_GPU_PASSES=1`** — primary evidence for anything that changes what the DEVICE does,
  with the wall clock secondary. Carries the pass extent census; reproduces to 0.001 ms/frame.
* **`tools/part79_picture_gate.sh`** — four arms, `CZ_VK_TEX_BATCH_BREAK=1` as the positive
  control. **Add a `STILL=1` pair** before believing anything it says about `meanLuma`: a
  turning-camera null pair agreeing to 0.05% measures a floor that is really ~1.5% (gotcha 465).
* **STILL MISSING, and it is item 2's blocker**: an always-on split of the pump's `walk`.

`tools/part79_flush_ab.sh` and `tools/part78_barrier_ab.sh` are the drivers to copy; both
reject a failed run **BY NAME**. `tools/part80_operator_session.sh` is the session harness and
**it names its own refutations** — keep that. Everything in `part76-kickoff.md` §5 still holds.

## 4. ~~WHAT IS OWED — ONE SENTENCE FROM THE OPERATOR~~ — **COLLECTED, 2026-08-27. NOTHING IS OWED.**

Sessions 1 and 2 were confirmed by their own report and both mapped onto the data exactly.
**Session 3 — the shipping build — had clean data and no verdict** (§6ea §2): 0 growths, 0
ceiling overruns, 0 unexplained spikes, every frame over 40 ms accounted for by boot, a load or
pump sleep, and worst-per-window 18.3-20.2 ms after the loads.

**Part 80 asked it in its first message — *after loading, and late in a crowd, anything?* — and
the answer was "Nothing — felt smooth."** The stream-store hitch class is now closed on both
channels: the counter says growth is impossible by construction, and the eye says nothing is
felt where two things were felt two builds ago. §6ea §2 carries the retraction in place.

The never-felt 50/60 ms class of session 1 is a DIFFERENT class and is **item 2 below**, still
open; §6ea §3's coverage gap stands.
