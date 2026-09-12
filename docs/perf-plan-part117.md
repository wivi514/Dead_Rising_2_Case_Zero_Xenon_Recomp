# Performance plan, part 117 — the pump on two cores, and what the PMU says the pump is

**The operator's order (morning of 2026-09-12, unattended until ~16:00):** *"Go try to
improve cpu performance ... try to see if you can separate the pm4 pump on multiple core.
See if some function we made are not properly optimised, etc."*

Read with `docs/part116-kickoff.md` (the live hand-off) and `docs/perf-plan-part111.md`
§10 (item B's record and the "bytes, machine-wide" verdict this part re-examines).

## §0. Step 0 — what kind of bound is the pump? Counters, not shares (RUN)

Part 111 named the pump "bound by BYTES, machine-wide" (gotcha 551) from one controlled
pair and predicted B2 dead from it. The pair has a second reading the plan never wrote
down: the pre-zero was a PREFETCH for the constants the pump writes into the same lines
a moment later, so moving it to another core handed the misses back to the pump as
coherence misses. Under that reading B2 is not refuted. The PMU tells the two apart
without building anything — `tools/part117_memprobe.sh`, one crowd run, the pump thread
at 8,400 draws / 1920x1080 / 97 fps, user-only counters (`perf_event_paranoid` = 2):

| counter (pump thread, 10 s) | value | per frame (97 fps) | reading |
|---|---|---|---|
| cycles / instructions | 41.7 G / 67.3 G | 43 M / 69 M | **IPC 1.61** — not a starved core |
| instructions per draw (8,400 draws) | — | **~8,200** | the walk + the per-draw renderer |
| demand fills from DRAM (`ls_dmnd_fills_from_sys.mem_io_local`) | 55.0 M | 57 k | 3.6 MB/frame on demand |
| all fills from DRAM (demand + HW prefetch) | 112 M | 116 k | **717 MB/s** — 2% of the DDR4 pipe |
| demand fills from L2 | 571 M | 590 k | the working set is L2-resident |
| L1 DTLB reloads (`ls_l1_d_tlb_miss.all`) | 481 M | 496 k | |
| ... of which 4K PAGE WALKS (`tlb_reload_4k_l2_miss`) | 33.2 M | **34 k** | the 4 GB guest map on 4K pages; ~0.3-0.5 ms/frame |
| IBS op samples by data source | L1 40.1% · L2 1.8% · **RAM 0.47%** · N/A (non-load) 57% | | |

**So the pump is NOT bandwidth-bound, machine-wide or otherwise: it pulls 0.7 GB/s from
DRAM and retires 1.6 instructions a cycle.** It is an instruction-throughput-bound serial
thread with a modest miss tail (57 k demand DRAM fills and 34 k page walks a frame). Two
things follow, and they are the two halves of this part:

1. **Two cores CAN help** — not by moving the same bytes (B1's null stands and is now
   explained: the pump re-touched every line the worker zeroed) but by splitting the
   serial instruction stream into two serial streams that overlap. Gotcha 551's verdict
   is retracted in part: the mechanism named there (bandwidth) is refuted by the counters;
   the observation (relocating the zero recovered nothing) stands.
2. **Fewer instructions per draw is time at ~1:1**, which is what "functions we made that
   are not properly optimised" means here; each such trim is small and they are measured
   as a bundle (§3).

## §1. Item 1 — the walk on one core, the renderer on another (`CZ_PUMP_SPLIT=1`)

**The design.** The pump thread (W, `cz-pump`) keeps everything it does today EXCEPT the
renderer: it walks the ring and the indirect buffers, keeps the register file, evaluates
waits, publishes the read pointer, delivers the vblank ISR. Everything with an effect the
renderer or the guest can observe is written into an ordered single-producer /
single-consumer queue and executed by a second thread (D, `cz-draw`) in stream order:

| what the walk reaches | today | under the split |
|---|---|---|
| a register write (single or run) | `g_regs[]` | `g_regs[]` (W's own) **plus the run appended to a log** that D replays into its replica before each op |
| DRAW_INDX / DRAW_INDX_2 | `VkRenderer_Draw` inline | a draw op: the `Pm4Draw`, both shader bindings, the ALU/fetch version stamps and the VS-palette take, all captured at the packet |
| EVENT_WRITE* / MEM_WRITE / REG_TO_MEM / COND_WRITE / the scratch mirror — every store into guest memory | `GuestStore32` inline | a store op, executed by D **in order with the draws** — a fence written before D has copied the streams before it would hand the guest permission to overwrite them (the UP-draw ring) |
| INTERRUPT (0x54) | the ISR on the pump thread, in position | an interrupt op: D reaches it, asks W to deliver, and WAITS until W has — so the ISR still runs on the pump thread's guest context, still reads a scratch mirror D has stored, and the poison store after it is still after it |
| XE_SWAP | `VkRenderer_OnSwap` + `Host_Present` inline | a swap op |
| IM_LOAD first sight | `VkRenderer_OnShaderBind` | a shader op carrying a copy of the code |
| WAIT_REG_MEM | polled on the pump | polled on W as today; a wait on a word a DEFERRED store writes holds until D gets there (counted; not a deadlock — D never waits on W except at an interrupt, and W services interrupts while it waits for queue space) |

The register replica is what makes the renderer's code untouched: `DoDraw` still takes a
`const uint32_t* regs`; under the split it is D's replica, replayed from the log up to the
draw's own position. The log carries 815 k dwords a frame (part 20's number) in ~42 k runs
(part 109's census: mean 19.3 dwords/run) — ~3.3 MB/frame copied on W and again on D.

**Pre-registered predictions** (crowd route, 1920x1080, 8,250-8,750 draw bands, three
runs a side alternated, medians; `tools/part116_guestcpu.py` reads them):

* D's CPU per frame (printed as `pump cpu`, the thread that calls DoDraw) lands at
  **8.0-8.8 ms**: today's 10.0-10.5 minus the walk (`WriteRegisterRun` 1.06 +
  `ExecutePacket` 0.84 + `ExecuteLinear` 0.36 = 2.26 ms, the fresh profile) plus the
  replay (~0.3-0.6 ms).
* W's CPU per frame (new column `walk cpu`) lands at **2.5-3.5 ms**.
* The wall median falls from ~10.2 to **8.5-9.0 ms** (−1.2 to −1.7), the new floor being
  D against the guest's 8.1 (the poll default) — so the wake (`CZ_WAITANY_WAKE=1`,
  parked) becomes worth measuring again the moment this ships.
* **Kill: wall better than −0.5 ms or the arm ships OFF.** Any gate failure ships it OFF.

**Gates:** `--smoke`; A5 exit 0; `truncated=0`; `no translated shader` 0; both PM4
oracles; unlowered switches; E3 pinned correlation; `CZ_VK_SYNC_VALIDATION=1` 0 hazards
with the poison producing 30 (the barrier gate — D records exactly what the pump
recorded, so this must not move); `frame_era_medians.py` with a null pair; an explorer
soak with `CZ_WAIT_TRACE=1`; the hand-off counters (`Pm4_WaitUnmetCount`, hold streak)
against the control arm.

## §2. Item 2 — the guest map on huge pages (34 k page walks a frame)

`/sys/kernel/mm/transparent_hugepage/enabled` is `madvise` here and the runtime never
advises, so the 4 GB guest map runs on 4K pages; `shmem_enabled` is `never`, which rules
out the memfd-backed physical views without root. What CAN be done unprivileged is
`madvise(MADV_HUGEPAGE)` on the private anonymous range (the title's virtual heap, code,
stacks). Measured by the same counter that priced it; below 0.2 ms it is a note for the
operator (one `echo advise > shmem_enabled`) rather than an item.

## §3. Item 3 — the instruction diet

Per-draw work that runs whether or not anything needs it, found in the fresh profile and
by reading `DoDraw`: `ProfScope::~ProfScope` at 0.5% with the profiler OFF; `InternalRes`
0.58% and `AspectPatchMode` 0.40% and `rtfactor::Active` 0.33% and `FovHalfRadThisFrame`
0.14% recomputed per draw; `std::map<string,...>::operator[]` 0.28% — a `Count()` still on
a hot path. Each is ~0.03-0.06 ms; measured as ONE bundle against a 0.3 ms bar.

## §4. Execution record

### 4.0 Step 0 (07:15-07:25) — the table in §0. The pump is instruction-bound.

One `tools/part117_memprobe.sh mem_base` run at the crowd. The numbers are in §0; the
sentence they license is the one this part is built on: **two serial instruction streams
on two cores overlap; the same bytes on two cores did not, and never could have.**

### 4.1 Item 1, the build (07:25-08:25) — six crowd runs, one crash, one restructure

The first build (07:32) booted, played the DebugJump route and held the crowd with the
split on: wall 10.06 ms against 10.1-10.2, D 9.5 ms/frame, W 3.25 — the walk had moved
and the frame had not, because D was idle 1.5 ms a frame and paying ~1.4 ms to replay
the stream. What each of the next builds found, in order:

| build | change | what the numbers said |
|---|---|---|
| split1 | op ring + run log, interrupts via W | `irqwait` 100 us per INTERRUPT (W asleep in its 100 us nap), 3.3 a frame |
| split2p | prefetch the log two lines ahead; ~200 us spin before D parks; stores in the log | replay loop 9.4% -> ~3% of D; wall 10.01 |
| split3 | idle + wait census | **D idle 1.5 ms/frame; 52 unmet wait evaluations a frame, 46 on OUR OWN pending store** — the walk drained the pipeline at every hand-off block |
| split4 | run-ahead: a wait on a word a pending store satisfies proceeds | waits 0.1/frame, run-ahead 10/frame; wall unmoved; **the wake arm crashed with `ctr=0BADF00D`** |
| split5 | **one ordered stream** (the crash: D's idle path replayed the log PAST an unexecuted INTERRUPT op, landing the scratch-mirror poison before the ISR read it); the pending value REPLACES the memory read at a wait (satisfied: run ahead; unsatisfied: hold, as hardware's CP would) | wall 10.03; D 9.3 (1.6 idle); **the guest's Main Thread is now the longest term: 8.1 ms CPU + 2.2 ms in the 1 ms wait-any poll** |
| split5w | + `CZ_WAITANY_WAKE=1` (the parked part-116 item — its trigger, "the pump under ~8 ms", is met: D's work is ~8.3) | **9.91-9.96 median, p99 12.1** (was 14); Main Thread waits 2.2 -> 1.35; D idle 0.9 |
| split6w | ProfScope's off-check inlined; contiguous runs merged on W (83 of 50,000 — the guest's runs are not adjacent; harmless) | 9.03-9.06 median at 8,450 draws |
| split5wt | `taskset -c 0-7` (one hyperthread per core) | WORSE: 10.9-11.9, p99 22 — the scheduler does better than the pin |

**The interrupt hand-off is the pipeline's clock.** Every INTERRUPT packet costs a drain:
D reaches it and must wait for W to deliver the ISR (design point 2), and W's next
WAIT_REG_MEM polls the ack the ISR writes, so W cannot run past it either. 3.3 a frame,
~10 us each with the interruptible nap. The swap's rendezvous (the walker in the vblank
ISR clears `mirror+4`) is the one hold that lasts: up to a vblank period.

### 4.2 Item 1, the campaign (08:26-08:52) — `tools/part116_ab.sh`, 3 runs a side, alternated, one binary

`c1base` (the one-thread pump, every release's default) vs `c1splitw`
(`CZ_PUMP_SPLIT=1 CZ_WAITANY_WAKE=1`), 1920x1080, `tools/part116_guestcpu.py`:

| band | n A / n B | wall A -> B | pump/D A -> B | Main A -> B | Draw A -> B |
|---|---|---|---|---|---|
| 8000 | 11 / 13 | 10.27 -> 9.08 (**−1.19**) | 10.06 -> 8.88 | 7.73 -> 8.09 | 5.67 -> 6.20 |
| 8250 | 24 / 19 | 10.46 -> 9.07 (**−1.39**) | 10.28 -> 8.85 | 7.83 -> 8.18 | 5.88 -> 6.28 |
| 8500 | 12 / 12 | 10.57 -> 9.92 (**−0.65**) | 10.36 -> 9.08 | 7.85 -> 8.41 | 5.84 -> 6.48 |
| **median** | | **−1.19 ms, monotone** | −1.28 | +0.36 | +0.53 |

**The pre-registered kill (−0.5) is cleared by a factor of two: ~96 -> ~110 fps at the
operator's crowd.** The prediction was right about W (3.3-3.7 ms) and half right about D
(8.6-9.1 ms/frame with 0.9 of it idle, against 8.0-8.8 predicted): the replay is ~0.7 ms
where 0.3-0.6 was budgeted and the guest/D ping-pong leaves D idle ~0.9 ms a frame. The
guest's own threads cost +0.36/+0.53 ms more CPU under the split — cache and core
contention from a fifth busy core (gotcha 562's mechanism again) — and that is now the
frame's longest term: the Main Thread at 8.1-8.4 ms CPU plus ~1.2 ms of waits.

### 4.3 Campaign 2 (09:25-10:17) — the split's share, the wake's, the bundle's, the huge pages'

Four env arms on one binary (`tools/part117_campaign2.sh`), three runs each, alternated:
A = split with the wake OFF, B = split + wake (the new default), C = B + the part-109
bundle (`CZ_VK_SCOPED_SHARED_ZERO=1 CZ_VK_TEXMEMO=1`), D = B with `CZ_NO_HUGEPAGES=1`.

| pair | wall (median over matched bands) | D's CPU | verdict |
|---|---|---|---|
| A -> B (the wake, under the split) | **−0.94 ms, monotone in 4 bands** (10.00-10.10 -> 9.06-9.13 at 8,000-8,500) | −0.38 | **the wake is most of the pair's −1.19**; the split alone is ~−0.3 (A against campaign 1's baseline) and is what UNLOCKS the wake — on the one-thread pump the same wake read +0.3 (part 116) |
| B -> C (the bundle) | −0.13, monotone | **−0.56, monotone** | the bundle takes 0.56 ms off the renderer thread and 0.13 off the frame — the thread is not the bound. Below the 0.3 bar; stays the operator's call, OFF |
| B -> D (huge pages OFF) | +0.40, NOT monotone (+0.03 / +0.76 / +0.86 / −0.00; one band n=1) | +0.06 | not established either way; the advice stays ON (harmless: 43 MB of the private range promoted, the views need root's shmem policy) |

**So the decomposition of the shipped −1.19 is: ~−0.3 from the two cores, ~−0.9 from the
wait-any wake that the two cores made worth having.** That is gotcha 569 in numbers: the
parked item's trigger was "which term is longest", and the split changed the term. It is
also the honest ceiling: with the wake on, D at 8.5-9.1 and the Main Thread at 8.1-8.4,
the next term is the guest's.

