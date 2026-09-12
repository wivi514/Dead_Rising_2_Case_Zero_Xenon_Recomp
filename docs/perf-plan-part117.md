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

(filled in as the items run)
