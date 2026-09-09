# Performance plan, part 107 — 60 fps minimum on a Ryzen 3 3100-class CPU

**The operator's instruction, 2026-09-09, after the GPU half of the target was met:**
*"Prepare the plan so we can run at minimum 60 fps on a cpu equivalent of ryzen 3100."*

`docs/perf-plan-part106.md` is the GPU half and stands: with the store mirror the crowd's
device frame is ~4 ms on the dev box and a GTX 1050 Ti-class clock held 58-62 fps in the
operator's hands (§4.2). This plan is the CPU half. "Minimum 60" is a WORST-CASE claim:
it is judged at the crowd (`tools/part80_crowdroute.sh`, 8,000-9,500 draws), on the p99
and the pinned-to-16.7 ms share as much as on the median (gotcha 237), and on a machine
that stands in for the target, never on this box at full strength.

## §0. The arithmetic

### 0.1 What the target CPU is

A Ryzen 3 3100: **4 cores / 8 threads, Zen 2, 3.6 GHz base / 3.9 boost, 16 MB L3 split
across two CCXs**. Against this box's Ryzen 7 5700 (8 cores / 16 threads, Zen 3, up to
4.65 GHz): about 0.7x per thread (Zen 3's ~19% IPC and the clock), and half the cores.
Nothing here is measured on a 3100; §1 is the stand-in and §1's table its calibration.

### 0.2 What the crowd costs on this box, at 1080p, mirror ON (part 106, seven baselines)

| load | wall | GPU | fence | pump regime |
|---|---|---|---|---|
| light street (2,000-3,000 draws) | 3.9 ms | 3.25 | 0.0 | CPU-bound at 3.9 |
| crowd (8,000-9,000) | **10.3-10.6 ms** | 4.0 | 0.0 | **CPU-bound by ~6.5 ms** |
| crowd, serial recorder (`CZ_VK_NO_PAR_RECORD=1`) | 12.8-12.9 | 4.0 | 0.0 | the 3-worker parallel record is worth 2.2 ms |

Scaled by 0.7x per thread alone, a 3100's crowd is **~15 ms with three workers and ~18
ms on the serial recorder** — and a 4-core box does not get three workers: the thread
budget's part-100 floor gives >=4 physical cores **two** (`runtime/cpu/thread_budget.cpp`:
reserve 2, committed 3, floor 3@6c / 2@4c, cap 6). With two workers the record path
lands between those. **The crowd on a 3100 is therefore 15-18 ms before anything is
done — over the budget by 0-10%.** The light street is fine (~5.5 ms). So the whole plan
is about the crowd's last 10-20%, and every item converts 1:1 there because the frame
is pump-bound with the fence at zero.

### 0.3 Where the pump's crowd frame goes (the ledger's decomposition, part 89-90; re-profile first)

The pump thread is the critical path; nothing else is. Its ~10.5 ms at the crowd is, from
`phase5-notes.md` §6ec §1 / §6ej / §6ek and the §6ei serial residue:

| term | ns/draw or ms | ms at 9,000 draws | movable? |
|---|---|---|---|
| **PM4 walk** (register-write change detector, serial by construction) | ~3.1 ms | **3.1** | research: never decomposed |
| record — ours (state, vertex, index binds, capture) | ~196 ns/draw after parts 81/89 | 1.8 | 80% already replayed on workers |
| record — the driver's `vkCmd*` | 235 ns/draw | 2.1 | on the workers already |
| `UploadStream` resolve half (flat-cache lookup, guard) | 162 ns/draw | 1.5 | a perfect hash over the working set was never tried |
| textures (`UploadTexture`, ~13,900 calls/frame, 0.0014% do work) | ~167 ns/draw | 1.5 | pure lookup — a per-draw memo of the slot's last answer |
| constants (gather, memo, patch) | ~161 ns/draw | 1.5 | memo serves 93%; the gather copies ~9 VS / 27 PS regs |
| fixed per frame (resolves, clears, present, frame stats) | — | ~0.5 | — |

Every earlier share overstates `record` by ~30% (§6ej §4) and every one of these has a
shelf life: **the first deliverable is a fresh `CZ_VK_PROFILE` decomposition under the
stand-in**, mechanism only, never wall (gotcha 454: the profiler inverts the regime).

### 0.4 What a 4-core box has that this one does not: contention

On 8 physical cores the process runs ~3.3 of them busy: the pump, the guest's two busy
threads (one of which — the title's Draw Thread — SPINS on our ring read pointer at ~94%
of a core, part 51), the three workers, and six threads "outside the budget" (pipeline
warm 4, translate 1, golden 1, audio 1, XMA 1) that are idle except when they are not.
On 4 cores + SMT that is the pump, two guest threads, two workers and the outsiders on
eight hardware threads sharing four cores' execution units. The spin is the item this
box cannot see: a core that is busy-waiting is free here and is a quarter of the machine
there. **Item 2 is a 4-core item that a measurement on 8 cores can never find.**

## §1. The stand-in, and its calibration

`taskset -c 0-3,8-11` pins the process to four physical cores and their SMT siblings
(this box's siblings are (0,8), (1,9), …); the thread budget intersects its count with the
affinity mask, so the budget sees a 4-core machine and gives it two workers. That models
the core count. The per-thread speed needs the clock: **`sudo cpupower frequency-set -u
3200MHz`** caps this Zen 3 at ~0.7x of its 4.65 GHz, which is a 3100's per-thread
throughput to within the IPC guess. Both halves are needed; the first can run without
root and is what part 107 measured first.

### 1.1 Four cores — the first campaign (2026-09-09 01:22-01:35, crowd route, 1080p, mirror ON, one run each)

`~/DR2CZ-troubleshooting/part106-1080/cpu4.{sh,out}` and the `c4_*.trace` / `c8_control.trace`
beside them; `tools/part80_trace_band.py`, 1,000-draw bands, medians of texture-free
frames. **The clock cap landed DURING this campaign** (after `c4_default`, before `c4_w2`),
so the four runs are two calibrations at once — read the clock column, not the row order:

| run | cores | clock | workers | 2,000-3,000 | 8,000-9,000 | 9,000-10,000 | p99 / worst at ≥8,000 draws |
|---|---|---|---|---|---|---|---|
| `c8_control` | 8 | **3.2 GHz cap** | 3 | 4.97 ms | **13.22** | 13.95 | 16.4 / 20.4 ms |
| `c4_default` | 4 | stock (4.65) | 2 (the old floor) | 4.46 | **12.56** | — | 16.4 / 18.4 |
| `c4_w2` | 4 | 3.2 cap | 2 | 5.79 | **16.41** | 17.59 | 22.6 / 25.2 |
| `c4_w3` | 4 | 3.2 cap | 3 (the new floor) | 5.87 | **16.77** | 17.96 | 21.5 / 23.6 |

What it says, each a single run and to be read with §1.2's null:

* **Four cores at stock beat eight cores at the cap** (12.56 vs 13.22 at the crowd): the
  crowd is clock-bound before it is core-bound. The cap alone costs the 8-core box
  +25-28% against part 106's 10.3-10.6 ms baselines.
* **The cap on four cores is +31% (12.56 → 16.4-16.8)** for a 0.69x clock — ~90% of
  clock-proportional, i.e. the pump's critical path is almost pure CPU, not memory or
  the GPU.
* **Four cores against eight, both capped, is +24-27% (13.22 → 16.4-16.8 ms)** — that is
  §0.4's contention item, measured for the first time: 3.2-3.5 ms of the crowd frame
  on the stand-in is threads sharing cores. The GPU column and the fence are unchanged
  (fence 0.00 everywhere), so it is all pump-side.
* **The stand-in's crowd is 16.4-18.0 ms = 55-61 fps** — §0.2's 15-18 ms estimate was
  right, and the target is missed by 0-8% in the 8,000-9,000 band and 5-8% at
  9,000-10,000, with a p99 of 21.5-22.6 ms.
* The third worker reads +1.7% frame-weighted (`w2` vs `w3`, monotone across nine
  bands) — one run each, so §1.2's pairs decide it.

### 1.2 Four cores, 3.2 GHz cap — the calibrated stand-in (2026-09-09 01:35-01:52, `cpu4cap.{sh,out}`)

Two runs an arm, alternated, at the cap (`scaling_max_freq` 3200000 read back at the
start); the 8-core control at the cap after them. Same reader as §1.1.

| arm | 2,000-3,000 | 7,000-8,000 | 8,000-9,000 | 9,000-10,000 | p99 / worst at ≥8,000 (window means) |
|---|---|---|---|---|---|
| `c8cap_control` (8 cores, 3 workers) | 4.92 | 11.82 | **12.60** | — | 16.1 / 19.3 |
| `c4cap_w2` pooled (2 workers) | 5.87 | 14.44 | **16.62** | 17.30 | 22.1-22.5 / 24.5-25.5 |
| `c4cap_w3` pooled (3 workers, the new floor) | 5.97 | 14.68 | **16.72** | 17.36 | 21.1-21.7 / 23.4-23.7 |

**The null under the mask, measured before the arm was read** (gotcha 229): `w2a` vs `w2b`
−0.1% frame-weighted with single bands at −3.9%…+4.9%; `w3a` vs `w3b` +1.1% with bands
at −1.8%…+4.6%. **The floor under the stand-in is ±1% frame-weighted and ±4% in any one
1,000-draw band**, wider than the ±2.9% the route has on 8 cores. A per-band delta
inside ±4% is not a result here.

What it says:

* **THE STAND-IN'S CROWD IS 16.6-17.4 ms = 57-60 fps.** The 8,000-9,000 band misses
  16.7 ms by 0-0.5%, the 9,000-10,000 band by 4%; the p99 is 21-22.5 ms. §0.2's 15-18
  ms estimate stands as a measurement now, at its upper end.
* **Four cores against eight at the same clock is +32% (12.60 → 16.6-16.7)** — 4.0-4.1 ms
  of the crowd frame is contention (§0.4), with the GPU column and fence unchanged.
  That is the largest single item on the stand-in and it is not in §0.3's table,
  because §0.3's table was taken where it does not exist.
* **Item 1 is CLOSED: the third worker is a null on 4c/8t.** `w3` vs `w2` reads +0.8%
  frame-weighted (+0.10 ms at 8,000-9,000, +0.06 at 9,000-10,000), inside the null; its
  p99 reads 0.4-1.4 ms BETTER. No loss, no gain the median can see; the floor stays at
  three (the operator's instruction) and the record is this row.

## §2. The items, in order

Order is by expected milliseconds ON A 4-CORE BOX, which is not this box's order.

0. **Fresh decomposition under the stand-in** (§0.3's table re-measured; `CZ_VK_PROFILE=10`
   one run an arm under `taskset`, plus a `perf record` of the pump for the walk's own
   symbols — `tools/part55_srcline.py` is the reader). Nothing below is priced until this
   exists.
1. **The thread budget on 4 cores.** The floor gives two workers; the arm is
   `CZ_WORKERS=1|2|3|4` under the mask, and the question is whether SMT siblings are
   worth counting on a small part — the budget counts PHYSICAL cores by design (the
   operator's rule: leave cores for the OS), but on a 4c/8t box a third worker on a
   sibling may be a net gain where on this box it was +4.5% SLOWER (perf-state-parked
   §5.2, measured with the guard pool at 4). §1.1 answers half of this for free.
2. **The guest's spinning Draw Thread** (part 51: the title's own thread spins reading
   ~~the ring read pointer~~ **the FENCE-COMPLETION word — RETRACTED IN PLACE, 2026-09-09:**
   the first build of this item woke the park on the read-pointer publication and every
   park timed out; `CZ_FENCE_PARK_TRACE` showed the polled word at BC739A00 climbing by
   ones (FAC3, FAC9, FACF…) while the ring cursor sat at 0x460E. The word is
   `kDeviceWritebackPtr`'s target, the fence pm4.cpp already calls `g_fenceWord`; the
   read-pointer writeback is +0x3C in the same block, and only the executor's
   `StoreGpuRaw` advances the fence. The wake now lives there. The predicate below is
   unchanged; "R" is the completed fence and "target" the one the driver waits for —
   the Draw Thread throttling itself against the GPU, exactly finding 38's reading —
   while the pump walks; on 8 cores it is a free core, on 4 it is
   a quarter of the machine). The loop is already read (`phase5-notes.md` §6ch §1,
   phase1 finding 38): `sub_82845160` re-checks `R` (~~the ring read pointer~~ the fence
   word, a guest-memory word only our executor writes) against its target and calls
   `sub_8283C6C8` between checks — a body of `mr rX,rX` SMT-yield hints in a `bdnz` loop
   plus a timebase read, which yielded a hardware thread on the 360 and is a pure
   busy-loop here. There is no kernel import in it, so the hook is the recompiler's: a
   strong `PPC_FUNC(sub_8283C6C8)` that keeps the body's contract (return after a bounded
   time, touch nothing) but parks the thread on a futex over the read-pointer word, which
   ~~the pump wakes after every publication (mid-walk since part 86, and at the walk's
   end)~~ the executor wakes after any store landing on it (`cpu/fence_wait.cpp`;
   `CZ_FENCE_PARK=0` the control). Latency is the risk — the frame chain is pump -> R -> Draw Thread -> next submit
   — so the park must be wake-on-write, not a timed sleep, and the timebase-timeout path
   stays as written.
   Prediction: zero effect on this box (the control that must be boring), a whole core
   returned on the 4-core stand-in. Gate: the A5 kernel-order diff and the ring
   `truncated=0` gate, because a wait that wakes late is a desync.
3. **The PM4 walk** (~3.1 ms, the largest serial term, never decomposed). After item 0's
   `perf` split: (a) batch the register-window writes into wider memcpys where the packet
   stream writes consecutive registers (a census first: how many SET_CONSTANT packets are
   contiguous runs, and how long); (b) the boundary-speculating second walk thread is
   research and stays research until (a) is measured. Memoisation is refuted (gotchas
   474/4) — do not re-buy.
4. **`UploadStream`'s resolve half** (162 ns/draw ≈ 1.5 ms): the flat cache's lookup is
   the measured majority; a perfect hash over the frame's stable working set (the key
   set repeats 94% frame to frame) was never tried. Prediction ≤0.8 ms on this box, 1:1
   on the target.
5. **`UploadTexture` as pure lookup** (~167 ns/draw ≈ 1.5 ms; 13,900 calls a frame and
   0.0014% do work): a per-fetch-slot memo of (fetch constant → descriptor index) keyed on
   the constant's six dwords, invalidated by the upload path itself. Same shape as the
   ALU constant memo that shipped in part 55; the verifier arm is the design.
6. **The constants** (~161 ns/draw): the gather serves 93% from the memo; what is left is
   the copy of the ~9 VS / 27 PS registers a draw reads and the projection patch. Small
   here; smaller on the target than items 3-5.
7. **The felt hitches, separately from the median:** texture-upload bursts (open item 0w,
   up to 77 ms in one frame), the stream store's growth (fixed at a 1 GB start), the
   pipeline warm (async since part 98). A locked 60 is a p99 claim; item 0's trace must
   carry `texUploads` and the band table's texture-upload row is the gate.

## §3. Protocol and gates

- Every A/B: three runs an arm, alternated, under the SAME stand-in mask and clock;
  `tools/part80_trace_band.py` on the crowd band; `CZ_FPS_LOG=10`'s p99 and worst
  window beside the median; the pinned-to-16.7 ms share (`tools/frame_perf_bins.py
  --pin-ms 16.7`) as the decisive statistic once a run is near the floor.
- The null first: two runs of one configuration under the mask, so the mask's own noise
  is known before an arm is read against it (gotcha 229; the route's floor on 8 cores
  is ±2.9% and is NOT the floor under a mask).
- An item is shipped when it moves the crowd band monotonically under the stand-in AND
  reads a null or a gain on the unmasked box (the operator's own machine must not lose).
- Kill rule, pre-registered: an item under 0.4 ms at the crowd on the stand-in is not
  measurable on this route and is parked, not built.
- The picture gates are unchanged: `CZ_VK_SYNC_VALIDATION=1` 0 hazards for anything
  that touches ordering (item 2 does), the order gate for anything that touches the
  recorder, and the operator's eye at the crowd before a release.

## §4. What "60 minimum on a 3100" will and will not mean

It will mean: the crowd route's 8,000-9,500-draw band under the calibrated stand-in reads
a median under 16.7 ms with a p99 under ~20 and a pinned share that says the title's own
vblank ladder, not the pump, is the limiter. It will not mean a measurement on a Ryzen 3
3100, until someone with one runs `--diag` and the route; every number in this plan is
this box under a mask and a cap.
