# Part 90 kickoff — after the parallel record: the CPU board has no ≥1 ms lead left

**THIS IS THE LIVE HAND-OFF**, superseding `part89-kickoff.md` (kept as the record of
part 89's step list — the plan it pointed at, `perf-plan-part89.md`, was executed WHOLE
in one part and its §6 records that in place).

## 0. What part 89 shipped (`phase5-notes.md` §6ei-§6ej are the records and win on numbers)

**Parallel command recording is ON BY DEFAULT** (`CZ_VK_NO_PAR_RECORD=1` the
same-binary control; `CZ_VK_RECORD_CHUNK=N`, default 512, tunes it):

| what | number |
|---|---|
| step 0 pricing (§6ei) | movable 2.4-2.6 ms; formula saving 1.0-1.24 conservative, ~2.2 schedule-bound; kill (<1.0) did not fire |
| **3v3, dominant crowd band 9,000-9,500** | **13.00 → 11.20 ms, −13.9%, −1.80 ms** (n 12,886/25,715); 9,500-10,000 −12.8%; 8,000-9,000 −9/−10% |
| honest cost | GPU-bound sub-5,000 bands +0.09-0.22 ms (instance splits), fence 1.5-3.1 covers it — invisible at the 60 fps cap |
| order gate | 0 fails / 22.1M draws; poison fails 6,905/6,905 frames naming the transposed draw |
| sync validation | 0 hazards (the 6 `topology-08773` messages PRE-EXIST — in part 77's logs — separate old item) |
| picture | era medians INSIDE the null, both fix runs (4-run gate); title frame intact |
| engagement | 140,479 chunks/run, 80% of draws on workers, pump helped 0, submit wait 0.6 ms/RUN; guard prehash HELD 98.3%, pool 15.5→18% busy |

The design that shipped is simpler than the plan's: **no secondaries, no
suspend/resume** — LOAD/LOAD attachments make a dynamic-rendering instance split the
identity, so chunks are self-contained instances in per-worker PRIMARY buffers,
submitted as one ordered `vkQueueSubmit`. Passes under the chunk size never split.
§6ej §1 has the two safety properties (pump helps at the wait; lost-wakeup fix).

**Two step-0 instruments remain for reuse**: `CZ_VK_RESOLVE_SPLIT_CENSUS=1` (sampled
1-in-16 UploadStream split inside record) and the guard pool's always-on occupancy
line under `CZ_VK_PROFILE`. Both are positive-controlled (§6ei).

## 0b. THE PRICING FRAME FOR THIS PART — the regime is about to flip, and that is the lead

Read the fix arm's own 3v3 numbers before choosing anything: at the dominant crowd
band, **wall 11.20 ms vs GPU median 10.62 ms, fence 0.00**. Two consequences, and
they are the whole shape of part 90:

* **Only ~0.6 ms of CPU saving can still CONVERT at the crowd.** Beyond that the
  frame is GPU-bound and the fence absorbs every further CPU win (the same regime
  arithmetic that killed part 80's GPU items, now pointed the other way). There is
  no known CPU lead ≥1 ms anyway; the realistic CPU take is ≤0.6 ms.
* **The GPU items part 80 killed ON REGIME — never on measurement — come back to
  life the moment that gap closes**: the resolve clears (0.601 ms ceiling) and the
  resolve copies (0.723 ms), ~1.3 ms combined, plus whatever a fresh look at the
  10.6 ms GPU frame finds — it has had exactly one optimization pass (part 78's
  barriers, −11.9%). `CZ_VK_GPU_PASSES=1` is the instrument and its bill is nil.

Realistic expectation for the whole part: **0.5-1.5 ms (4-12% at the crowd),
assembled from two or three sub-milli-second pieces** — each near the route's ±2.9%
floor (~0.33 ms), so they ship on mechanism numbers, part 88's way, not on wall time,
part 89's way. And the operator-facing framing: at 11.2 ms the crowd already holds a
locked 60 fps with 5.5 ms of margin — further wins buy headroom and resolution, not
felt frame rate. Surface that before spending sessions here; parking again is an
honest outcome.

## 1. THE BOARD (in order)

0. **Every pre-part-89 CPU decomposition now overstates `record` by ~30%** (625 → ~430
   ns/draw on the pump). Any new performance question starts with a fresh
   `CZ_VK_PROFILE` crowd decomposition, not §6ec/§6eg/§6ei's tables — one run, and it
   also says (via `submit`/fence and the GPU column) whether §0b's flip has already
   happened.
1. **There is NO known CPU lead ≥1 ms left.** The serial residue is the PM4 walk
   (~3.1 ms instrumented share) and the resolve half (~162 ns/draw) — the
   change-detector class (gotchas 474, 4). If the operator wants more CPU, the next
   bar is ~0.5 ms and the first step is the fresh decomposition above; §0b says at
   most ~0.6 ms of it can convert. The GPU side (§0b) is the larger open surface.
2. **The CW 2a/2b serial record-restructure item is SUBSUMED** — part 89's §0a note
   said it would be if the maximal design shipped; it did. Dead without a port.
3. The Windows bundle save-squatter hunt (`part86-kickoff.md` §0b repro; needs czwin).
   NOTE: the Windows build has not been rebuilt since parallel record shipped —
   **build and run the Windows leg before any operator session there**; the code is
   platform-neutral Vulkan but untested on czwin this part.
4. A natural level-up check (`part86-kickoff.md` §0b(b)); glibc floor / AppImage;
   macOS (milestone C, hardware). The combo bench vs phantom card grants
   (`part86-kickoff.md` §0c residual).

## 2. Gates inherited (unchanged)

`--smoke` after every build; PM4 boundary oracles after any pm4.cpp change (part 89
touched none); `truncated=0`; any new default ships WITH its off-arm and measured
milliseconds in the same commit; censuses stay diagnostic arms. **New standing note:
any picture complaint bisects first on `CZ_VK_NO_PAR_RECORD=1`** — it is the newest
default and the one that rearranges command buffers.

## 3. For Case West (standing send-back)

The parallel recorder transfers whole once CW has a renderer: the LOAD/LOAD
instance-split identity is a property of the same EDRAM model, the capture struct is
engine-agnostic, and the shared-pool occupancy question should be MEASURED there too
before assuming either way (part 89's 0c pattern: 15% busy meant sharing was free).
Build the order gate BEFORE the recorder, poison-proven — it was the difference
between "we believe it" and "22.1M draws say so".
