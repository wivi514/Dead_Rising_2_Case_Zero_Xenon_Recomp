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
