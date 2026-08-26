# Part 78 kickoff — item 1 is done; the board is the GPU, the pipelines and the untile

> **THIS IS THE LIVE HAND-OFF**, superseding `part77-kickoff.md`.
>
> **Read `phase5-notes.md` §6ds first.** It is part 77 end to end: the decomposition that
> moved the item, the memory pool, the mip guards, the upload batch, and the gate that
> turned out to be blind.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6ds** | **part 77 — the texture path, and why its specified fix was 17% of it** |
> | `phase5-notes.md` §6dr | part 76's operator session — the TWO REGIMES, and where to measure a CPU item |
> | `phase5-notes.md` §6dq | part 76 — the readback split |
> | `phase5-notes.md` §6dp | part 75 — the write-combined read |
> | `part77-kickoff.md` §0b | **the regime table — still current and still the first thing to read before pricing a CPU item** |
>
> Lessons: gotchas **455-458**. There is still no live PLAN; §1 is the board, in order.

---

## 0. WHAT PART 77 DID, IN ONE PARAGRAPH

`part77-kickoff.md` §1 named the texture path first, on 36 operator stutter marks across
three sessions, and specified the fix: *"decode — untile every mip ... parallelise or cache.
Take this first."* **Splitting the decode clock before touching it showed the untile loop is
17.2% of the scope it was specified against, and one `vkAllocateMemory` per texture — a
kernel-side allocation, 145 us, 2,424 a run — is 70%.** Texture images are now
bump-suballocated out of 32 MB blocks; the mip chain's two validation passes stopped
re-walking the level above; and the 2,432 per-upload submit-and-waits became one submit per
burst. **The burst frame went 285.6 ms -> ~110 ms** and the whole `>150 ms` population of
the route collapsed to the two frames this change cannot reach.

## 0b. THE THREE THINGS PART 77 WOULD TELL YOU BEFORE YOU START

**One. Split the scope before you optimise the operation it is named for, even when the item
is fully specified.** The texture item had 36 operator stutter marks, a two-half cost
breakdown, a named fix per half, a pre-registered kill and three hand-offs repeating it. All
of that confirmed *"the texture path is the hitch"*, which is true. None of it touched *"the
untile loop is where the decode's time is"*, which was never measured and was false
(gotcha 456). The split cost forty minutes.

**Two. `CZ_VK_VALIDATION=1` IS BLIND TO THE TEXTURE HEAP'S IMAGE LAYOUTS.** A deliberately
broken build left ~1,400 textures never copied to the GPU at all — thousands of draws
sampling `UNDEFINED` images — and the layer reported only the six pre-existing pipeline
VUIDs, while the route gate passed and every draw counter read healthy. The heap is a
bindless update-after-bind array and the layer cannot associate those descriptors with a
draw. **Any change to when a texture image is written or transitioned must be gated on a
picture statistic or an explicit invariant counter, not on validation** (gotcha 458).

**Three. The regime table in `part77-kickoff.md` §0b is unchanged and still governs.** The
autonomous route is GPU-bound below ~6,000-7,000 draws and the operator's crowd is CPU-bound
above it. Run `CZ_VK_FRAME_TRACE` alone — never `CZ_VK_PROFILE` — to ask which, and measure
a CPU item at 9,000+ draws or not at all.

## 1. THE BOARD, IN ORDER

### ITEM 1 — THE GPU. **Now the largest thing nobody has ever looked at.**

Unchanged from `part77-kickoff.md` §1 item 2a and it moves up because item 1 closed:

| draws (band median) | 1,157 | **2,484** | 4,222 | 6,236 | 8,142 | **9,208** |
|---|---|---|---|---|---|---|
| **GPU ms** | 6.97 | **9.26** | 8.69 | 9.61 | 11.35 | **12.20** |

**Three quarters of the device's cost in a full crowd is already there in a light scene**, so
it is full-screen work — the post chain, the resolves, the passes that run whatever is on
screen — and it is a fixed floor under everything, 60-70% of the frame in every band below
the crossover. **This project has never had a GPU-side breakdown**; the frame trace gives one
number for the whole command buffer, and this renderer already writes two timestamps per
frame, so the mechanism exists and needs extending to per-PASS rather than inventing.

Do NOT quote a linear fit over those bands as a fixed/variable split — the series is
non-monotone because the bands are different CONTENT (`measurement.md` §4).

### ITEM 2 — PIPELINE COMPILATION ON THE LOAD FRAME.

Newly visible, because the texture cost that was covering it is gone. The route's burst frame
compiles **97 pipelines**, and after part 77 that frame's remaining time is roughly the
untile plus those. Part 71 shipped the pipeline CACHE (17.8 s of first-run compilation) and
this is what is left with a warm cache. It has never been priced per-frame.

**Price it before designing anything**: the trace's `pipes` column is per frame and
unconditional, so one banded read of an existing run says whether this is 10 ms or 40.

### ITEM 3 — THE UNTILE LOOP, and it is a SMALL item now — read §6ds §10 before starting.

It is finally the largest column of the decode (79 of 152 ms/run), and the work that would
make it fast is **already done and proven**: `Tiled2DOffset` decomposes exactly into a 32x32
table plus a per-macro-tile base, checked over 967,680 combinations with zero mismatches
(`tools/tile_offset_separable.py`), and consecutive units are contiguous in 16-byte runs so
the address can be computed once per run instead of once per unit.

**But 79 ms/run is ~25 ms on a burst frame and the standing kill for this item has been 40 ms
off the worst frame.** A 3x would be correctly killed by it. Do not start this without
re-pricing it; part 77's whole lesson is that an item's size is a measurement.

### ITEM 4 — PARALLEL COMMAND RECORDING, at 9,000+ draws only.

`perf-state-parked.md` item A, unchanged from `part77-kickoff.md` §1 item 3. At 9,000-12,000
draws the CPU is 14.5 ms against an 11.9 ms GPU, so there are ~2.6 ms of frame time available
before the floor. **State the fence wait and the GPU floor at the draw count you intend to
measure, in the same sentence as the expected saving.**

## 2. WHAT IS RULED OUT — do not start these

* **the texture path.** Decode −68%, submit −95%, burst frame −60%. What remains of it is
  item 3 and it is priced.
* **the readback** (§6dq), **the constant path** (§6dp), **the guest side** (§6dm),
  **reverting the RT era** (§6dj), and everything on part 73's exhausted list.
* **a fence or any other wait primitive for the uploads** — part 73 measured three arms
  (gotcha 436) and part 77 removed the waits by not doing them, which was the only route.
* **`CZ_VK_VALIDATION=1` as the gate for anything in the texture upload path** (§6ds §9).
* **any CPU A/B below ~8,000 draws on the autonomous route** (§6dr, gotcha 453).

## 3. THE MEASUREMENT METHOD — the readers, and which one for what

Three now, and they are not interchangeable. **Classify the change first** (gotcha 452):

* **`tools/part75_ab_report.py`** — partitions runs by the MENU window as a machine-state
  fingerprint. Right for an item that affects the crowd and not the menu.
* **`tools/part76_band.py`** — prints the menu as a number and bands the crowd. Right for an
  item that affects EVERY presented frame.
* **`tools/part77_tex_report.py`** — population is FRAMES WITH A TEXTURE UPLOAD and its
  headline is the BURST FRAME, the one frame per run that uploaded the most (the DebugJump
  load, 774-780 uploads, the same event in every run — a matched comparison, not a
  distribution). Right for a HITCH item. Its control channel is the no-upload median, which
  such a change cannot reach.

Everything in `part76-kickoff.md` §5 still holds — pin `CZ_VK_RES`; read the route gate on a
finished log; never send an A/B loop to `/dev/null`; `pgrep -x cz_runtime_auto`, never
`pgrep -f`. Two more from part 77:

* **A trace with no `.rc` beside it is a run that has not finished**, and a partial trace
  reads as a complete run of a SHORTER ROUTE. The first pass of part 77's A/B compared a
  control arm at 6,178 frames against a finished arm's 16,685 and reported the run totals 43%
  apart. Quote per-upload (or per-draw) columns alongside run totals for the same reason.
* **Do not quote the run's overall maximum frame.** This route produces multi-second frames
  with zero uploads, zero pipelines and a healthy 10 ms GPU — an OS or compositor stall. One
  control run held an 8,275 ms and a 3,311 ms frame; crediting a change with removing them
  would have made part 77's headline `−97%` instead of `−42%`.

## 4. THE INSTRUMENTS PART 77 LEFT

```
(unconditional, free, printed at exit)
  decode split          the seven scopes inside g_texDecodeNs, WITH ITS RESIDUAL. Read the
                        residual first — a large one means the split is wrong, not that work
                        vanished. It runs at 0.5-0.6%
  base untile ns/unit   a total cannot tell a slow loop from a lot of units (4.6 ns over
                        11.5 M units says the loop is fine)
  CreateImage x N       its five driver calls, separately
  image memory:         P pooled into B blocks, D dedicated — the pool's engagement, and the
                        line that says how close a session got to maxMemoryAllocationCount
  texture upload batch: jobs, flushes, jobs/flush, biggest, and how many were forced by a
                        full staging arena
CZ_VK_NO_TEX_MEMPOOL=1     control arm: one dedicated vkAllocateMemory per texture
CZ_VK_NO_TEX_BATCH=1       control arm: one submit-and-wait per upload
CZ_VK_TEX_BATCH_BREAK=1    **NOT a control arm — the POSITIVE CONTROL.** Skips the ordering
                           flush; the picture goes fully black. Use it to prove any gate you
                           choose for the upload path can actually fail
CZ_VK_VERIFY_MIP_GUARD=1   recompute the pre-part-77 mip guards alongside; 0 disagreements is
                           the only passing value. ..._POISON=1 must read 100%
```

## 5. THE ONE THING TO CARRY FORWARD

**An item's size is a measurement, and confirmation of a parent does not transfer to a
child.** Part 77's item had 36 operator stutter marks behind it and every one of them
confirmed *"the texture path is the hitch"* — which is true, and which says nothing about
where inside the texture path the time is. The child claim, repeated verbatim through three
hand-offs, had never been measured and was wrong by a factor of four.

The repair was forty minutes: split the scope, print the residual, and look at what the split
says instead of at what the plan says. It is the third part in four where that was the whole
finding (75, 77, and 74 twice). **Before writing a line of a specified fix, ask what
measurement says the time is at the line you are about to change, and when it was taken.**
