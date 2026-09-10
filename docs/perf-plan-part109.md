# Perf plan, part 109 — 120 fps on the OPERATOR's CPU

**The instruction (2026-09-10, the operator going to sleep):** *"I want to try to get the
cpu ms to be able to do 120 fps on my cpu that's the goal if possible ... do an overnight
plan."*

Read `docs/perf-plan-part107.md` first: its §0.3 decomposition and §2 item list are this
plan's parent, and items 4-6 there were never run. This plan re-targets them from "60 on
a Ryzen 3 3100" to "**the CPU frame under 8.33 ms on a Ryzen 7 5700**", which is a
different question with a different order.

## §0. The arithmetic, and what "120 fps on my CPU" means

120 fps = **8.33 ms**. The goal is explicitly the CPU half: the frame must not be
CPU-bound at 120 fps. What the GPU then does at 3440x1440 is a separate question the
operator did not ask.

**The box:** Ryzen 7 5700, 8 cores / 16 threads, Zen 3, **cap restored to 4654 MHz**
(checked 2026-09-10, `scaling_max_freq` = 4654000 — this closes part 108 kickoff §1b item
0), governor `powersave`, RTX 3070 at 2100 MHz.

**Baseline, 2026-09-10, three runs, `tools/part80_crowdroute.sh`, mirror ON, MSAA 2x,
profiler OFF, at the operator's own 3440x1440:**

| run | crowd windows | median | p99 | draws |
|---|---|---|---|---|
| 1 | 22 | 11.39 ms | 14.15 | 9,657 |
| 2 | 22 | 10.72 ms | 13.92 | 9,036 |
| 3 | 22 | 11.42 ms | 14.50 | 9,329 |

**11.2 ms median at the crowd = 89 fps.** That is wall, at their native resolution, so it
is CPU and GPU together. The 1080p pair below is what separates them, because part 106
put the GPU at 4.0 ms there against a 10.3-10.6 ms wall — CPU-bound by ~6.5 ms.

**The gap to close: about 2.5-3 ms of CPU at ~9,300 draws**, if the 1080p wall confirms
the CPU frame is ~11 ms. Light street (2,000-3,000 draws) was 3.9 ms in part 106 and is
already past 250 fps; **this plan is entirely about the crowd.**

**What is NOT in scope.** The GPU at 3440x1440 (4.0 ms was a 1080p number; 3440x1440 is
2.4x the pixels). If the CPU reaches 8.33 and the GPU does not, the operator gets 120 fps
at a lower resolution or with MSAA off and the CPU claim still stands. Say which is which
in the record; do not quote a wall number as a CPU result (gotcha 219's shape).

## §1. Protocol, and the rules this plan does not get to bend

* **Three runs an arm, alternated, `tools/part80_crowdroute.sh <tag> [ENV=VAL]`** — the
  tag is the FIRST argument and env follows it (an env var passed first is silently eaten
  as the tag and the run is unprofiled; that happened once tonight and the three runs it
  produced were re-read as an honest profiler-OFF baseline rather than thrown away).
* **The band is >= 8,000 draws**, read with `tools/read_crowd.py`. A run-wide median
  measures how long the menus lasted.
* **The profiler is for MECHANISM, never for wall** (gotcha 454: it inverts the regime and
  costs 2-4 ms). Every wall number in this plan comes from a profiler-OFF run.
* **A kill rule per item, pre-registered below.** Under it, the item is closed and NOT
  shipped, however good the idea was.
* **Gates before any A/B is believed:** `--smoke`; the A5 kernel-order diff exit 0;
  `truncated=0`; and for anything touching the renderer, `CZ_VK_VALIDATION=1` clean and
  `CZ_VK_SYNC_VALIDATION=1` at 0 hazards with the poison control at 30.
* **A memo needs a verifier arm.** Every cache added here ships with an env var that
  computes both answers and counts disagreements, and the A/B is only believed after a
  run with the verifier reads 0. This is the shape that made part 55's constant memo safe
  and part 24's stream guard unsafe.

## §2. The items, in order of expected milliseconds ON THIS BOX

Order differs from part 107's, which was ordered for a 4-core machine.

### Item 0 — the fresh decomposition (running first, no code)

`CZ_VK_PROFILE=10` at 1080p and at 3440x1440, three runs each, plus a `perf record` of the
pump for symbol-level truth (`tools/part55_srcline.py` reads it). Part 107's §2b table was
taken on the 4-core stand-in with the glyph scan still running; **every share in it has a
shelf life.** Nothing below is priced until this exists.

### Item 1 — `UploadTexture` as a pure lookup (~1.5 ms predicted)

13,900 calls a frame and **0.0014% of them do work**. Each call today computes an FNV-1a
over six fetch-constant dwords plus the shader dimension, runs `DecodeTextureFetch`, and
then does a hash-map lookup. The constants for a given slot almost never change between
draws.

**Design:** a per-`constIdx` memo holding the six dwords, the shader dim, and the answer.
A hit is a 24-byte `memcmp` plus one compare, and skips the hash, the decode and the map.
**The invalidation is the whole design:** the answer depends on the resolve-snapshot set,
which changes DURING a frame, so the memo carries a generation counter bumped by every
snapshot registration and by frame start. A memo that skipped that would freeze a
colour-grading LUT for the run — the exact defect the snapshot-before-cache ordering
comment in `UploadTexture` was written about.

**Verifier arm `CZ_VK_TEXMEMO_VERIFY=1`:** compute both answers, count disagreements,
print at exit. Ship only after a crowd run reads 0.
**Kill rule: below 0.4 ms it does not ship.**

### Item 2 — `UploadStream`'s resolve half (~1.5 ms predicted)

162 ns/draw, and the flat cache's lookup is the measured majority. The key set repeats 94%
frame to frame and a perfect hash over the stable working set was never tried. Same
verifier discipline.
**Kill rule: below 0.4 ms it does not ship.**

### Item 3 — the constants (~1.5 ms predicted)

The gather serves 93% from the part-55 memo; what remains is the copy of ~9 VS / 27 PS
registers per draw and the projection patch. The copy is the target, not the memo.
**Kill rule: below 0.4 ms it does not ship.**

### Item 4 — the PM4 walk's per-packet dispatch (~3.1 ms, the largest single term)

Never decomposed. Part 108 established that item 3(a) of the old plan (bulk register runs)
**already exists** and that the cost is the **per-packet dispatch**, not the register
writes. This is research and it is last on purpose: it is the biggest number and the
least-understood one, and an overnight session should not start there.
**Do not re-buy memoisation** (gotchas 474/4, refuted twice).

### Item 5 — the p99, separately from the median

`CZ_FPS_LOG` p99 is 13.9-14.5 ms at the crowd against an 11.2 ms median. **A locked 120 is
a p99 claim, not a median claim.** Classify the slow frames by where the time went
(`CZ_VK_FRAME_TRACE`'s columns; texture-upload frames counted separately). If the median
reaches 8.33 and the p99 sits at 14, the operator does not have 120 fps.

## §3. What this plan will and will not have proved

It can prove: the CPU frame at ~9,300 draws on THIS box, before and after, three runs an
arm, at a stated resolution with stated instruments.

It cannot prove: anything about the operator's felt experience (their route is not this
route), anything about the GPU at 3440x1440, or that 120 fps is reachable at all — if the
items land and the frame stops at 9.5 ms, **that is the answer**, and the honest close is
to say so and name what remains rather than to keep buying items.

## §4. Execution record

(filled in as items run)
