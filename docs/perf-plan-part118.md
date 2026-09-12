# Part 118 — the guest's Main Thread: what it is bound by, and what can move it

**The operator's order (2026-09-12, 12:30, deadline 16:00 EST):** *"Find a way to reduce
the main thread ms it takes by splitting some part you can on other core. Optimising
stuff or whatever you can if possible."* Part 117 had left the frame bound by the
title's own Main Thread (8.1-8.4 ms CPU at the operator's crowd) with the pump on two
cores and the wait-any wake on. This document is the execution record; every number has
its run named. §5 is the honest answer.

## §0. What was known (part 116 §4.1, part 117 §0)

The Main Thread is 100% recompiled code and FLAT (top self symbol 4.8%): half
simulation (cZombieManager 15%, cAIManager 12%, Havok 11%, actors 5%) and half render
submission (cLevel::Render 36%, cTransModel::Render 15%). Codegen on the recompiled TUs
is dead (PGO −0.25, LTO/-O3 null). A spin before its parks is dead (+0.06 wall, +0.64
CPU). Part 116 §4.4 had one number nobody explained: **under `CZ_VK_NO_DODRAW=1` the
Main Thread's CPU is 6.4-6.7 ms against 7.9-8.1 with the renderer — our renderer's
presence costs the title's own thread ~1.5 ms a frame** (gotcha 562 named the pump's
bytes; it was a guess).

## §1. Step 0 — the PMU on the Main Thread, with and without our renderer (12:36-12:46)

`tools/part118_guestprobe.sh`: the crowd route, 1920x1080, `perf stat` on the Main
Thread's tid for 3 x 10 s at ≥8,000 draws. `gp_normal1` (107.2 fps, 8,190 draws) vs
`gp_nodd1` (`CZ_VK_NO_DODRAW=1`, 127.0 fps, 8,900 draws — a heavier band, so read the
per-instruction columns):

| counter | normal /frame | NO_DODRAW /frame | normal per 1M instr | NO_DODRAW per 1M instr |
|---|---|---|---|---|
| cycles | 33.8 M | 30.6 M | **775 k** | **665 k** |
| instructions | 43.6 M | 46.0 M | — | — |
| demand fills from DRAM | 31,070 | 27,416 | 713 | 596 |
| demand fills from L3 | 26,006 | 30,509 | 597 | 664 |
| demand fills from L2 | 72,662 | 69,494 | 1,668 | 1,512 |
| HW-prefetch fills from DRAM | 26,649 | 23,381 | 612 | 509 |
| all fills from DRAM | 59,464 | 52,438 | 1,365 | 1,141 |
| 4K page walks (L2 TLB miss) | 6,080 | 5,617 | 140 | 122 |
| loads dispatched | 9.95 M | 10.55 M | 228 k | 230 k |

IPC **1.29 with the renderer, 1.50 without**: the same instruction stream costs 17% more
cycles. The L2-miss count per instruction is the same (1,310 vs 1,260 per M) but under
the renderer a larger share of them goes to DRAM (54% vs 47%) — ~3,700 more DRAM fills a
frame, ~0.3 ms if serialised — which is L3 eviction by our threads, real but a third of
the gap at most. And the thread is what part 116 guessed: `perf annotate` of its hottest
function (`sub_827C6E28`, cTransModel::Render) puts 9.9%, 7.2%, 6.5% and 9.9% of its
samples on the four instructions that CONSUME a guest-memory load — pointer-chasing
the title's heap, serialised misses, 31k demand DRAM fills a frame.

**Placement (`x_hk4.cpus`, 120 samples at 4 Hz through the crowd):** the Main Thread
sat on cpus 14, 8, 10, 9, 12, 0 ... — it migrates on nearly every wake (it waits ~11
times a frame) — and **in 30 of 120 samples its SMT sibling was running one of our
saturated threads (`cz-pump`, `cz-draw`, a guard), in 5 more the Draw Thread**: 29% of
the time sharing a physical core. That is the candidate for the other two-thirds of the
CPI gap, and campaign 2 tests it.

## §2. The wait census by caller (`CZ_WAIT_CALLERS=1`, `x_stock`, `x_hk4`)

The `[guestwait]` line said "Main: single 0.5 ms in 5.7 calls, multi 0.75 in 5.0".
Keyed by guest tid and the import's lr plus two frames of the back chain (the title's
waits go through a `WaitForMultipleObjects` wrapper, `sub_82822548`):

| thread | kind | caller | ms/frame | calls/frame | what |
|---|---|---|---|---|---|
| Main | single | `sub_827CC6A8` <- `sub_827CC770` | 0.5 (stock) / 0.95 (Havok 4) | 1.0 | `WaitForSingleObject(obj+0x9b0, INFINITE, alertable)` in a loop on STATUS_USER_APC — the render front end waiting for the **Draw Thread** |
| Main | multi | `sub_82788808` <- `sub_8237D6BC` | 0.36 | 1.0 | a wait-any then `sync` — a frame-boundary join |
| Draw | fence | `sub_82845160` | 2.5-3.2 | 4-6 | our fence (`cz-draw`) |
| Draw | multi | `sub_827D3944` <- `sub_827D3B64` | 0.3-0.9 | 2.0 | the draw cache from the Main Thread |

The rest of the Main Thread's calls are below 0.5 ms/s each. **So the frame at this
crowd is a three-stage pipeline whose stages are within 5% of each other**: Main
(7.9-8.4 CPU + ~1.3 waits), the Draw Thread (6.3 CPU + 3 on our fence), and `cz-draw`
(9.1-9.4 CPU, 95% of a core, ~0.5 idle) all read the 9.6-10.0 ms wall. A saving on
the Main Thread alone reappears as time in `sub_827CC6A8` waiting for the Draw Thread,
which is waiting for `cz-draw` — campaign 1 shows exactly that shape.

## §3. Campaign 1 — the Havok worker count and the guard's non-temporal sweep (13:00-13:28)

`tools/part118_campaign.sh`, one binary (`c1.bin`, sha 0acfa2ccd746030a), three runs an
arm alternated, `tools/part116_guestcpu.py` matched 250-draw bands, 1920x1080, 70 s soak.

**The Havok worker count.** The title's physics init (`sub_827E6440`..) hard-codes
`li r29, 2` -> `hkCpuJobThreadPoolCinfo::m_numThreads` and `li r30, 3` ->
`hkJobQueueCinfo::m_jobQueueHwSetup.m_numCpuThreads` (main + 2): right for a 360 whose
six hardware threads already carry the Draw Thread and six JobThreads, and on the 8-core
host the two `HavokWorkerThread`s idle 95% while the Main Thread runs 8% of its frame
inside `hkJobQueue::processAllJobs` (real jobs — `sub_82915A18` and friends, not a spin;
§1 of this file's fold of the part-116 DWARF chains). `runtime/cpu/havok_threads.cpp`
hooks the two constructors and rewrites the stock values only (`CZ_HAVOK_WORKERS=N`,
0..6, the pool's own slot cap; the hardware-thread-id array is emptied so the
constructor's unbounded index read takes its default path).

| stock -> 4 workers | band | nA | nB | wall | pump | main CPU | draw CPU |
|---|---|---|---|---|---|---|---|
| | 8250 | 15 | 8 | +0.89 | +0.30 | −0.21 | +0.23 |
| | 8500 | 8 | 3 | +0.46 | +0.31 | −0.34 | +0.18 |
| | 8750 | 6 | 9 | −0.01 | +0.00 | −0.15 | +0.04 |
| median, 5 bands | | | | **+0.46** | +0.30 | **−0.21** | +0.18 |

The Main Thread's CPU falls 0.15-0.34 ms — the split of Havok's jobs moved as designed —
**and its single-object waits rise from 0.38 to 0.90 ms a frame, 5.7 -> 9.9 calls**: with
four workers the last jobs of every step are on a worker, so the Main Thread parks and
pays our kernel's wake latency four more times a frame instead of finishing them itself.
The wall is +0.46 (not monotone; +0.89 in the best-populated band). **KILLED.** The
default stays at stock (2); the arm is kept because a machine whose wake latency is
lower, or a Havok load that is heavier, may answer differently — and because it is the
one place the title's own code can be told to use more cores.

**The guard's non-temporal prefetch (`CZ_VK_GUARD_NTA=1`, on top of Havok 4).** The
content guard streams ~37 MB a frame of the title's vertex data through the shared L3
once a frame and never again; `prefetchnta` 512 bytes ahead of the fold keeps those
lines out of L2/L3 on AMD. Prediction: the Main Thread's DRAM fills fall towards the
NO_DODRAW arm's and its CPU with them. **Result: main CPU +0.03 (not monotone), wall
−0.22 (not monotone), draw −0.05 — a null.** Either the L3 eviction §1 measured comes
from somewhere else (the Draw Thread's own copies into the ring, `cz-draw`'s reads) or
PREFETCHNTA does not keep Zen 3's L3 clean the way the AMD guide reads. Not pursued
further; the arm stays for a PMU pass on it (`part118_guestprobe.sh gp_nta
CZ_VK_GUARD_NTA=1` would settle which).

## §4. Campaign 2 — placement (`CZ_GUEST_PIN`, 13:35-14:08)

`runtime/cpu/thread_budget.cpp`: the process is confined, from its main thread before
anything spawns, to the CPUs outside two (mode 1) or four (mode 2) reserved physical
cores — taken from the TOP of the affinity mask, both SMT siblings each — and the title's
Main Thread and Draw Thread (mode 2: our `cz-pump` and `cz-draw` too) are each moved onto
one of them when named, sibling left empty. A sweep of `/proc/self/task` after each pin
and once per `[fps]` window moves every later spawn that inherited a pinned creator's
mask back onto the rest. **The first build had no sweep and the Main Thread's children —
the Havok workers, the job pool — ran on its core: 25 fps** (gotcha 571; the run is in
`rejected/`). `tools/part118_campaign2.sh`, one binary (`c2.bin`), Havok at stock, three
runs an arm alternated, 8,000+ draws, `part116_guestcpu.py` matched bands:

| arm vs no pin | wall | `cz-draw` CPU ("pump") | Main CPU | Draw CPU | bands monotone |
|---|---|---|---|---|---|
| mode 1 (Main + Draw pinned) | +0.07 | **+0.38** | −0.17 | −0.37 | all four columns |
| **mode 2 (+ cz-pump, cz-draw)** | −0.01 | **−0.52** | **−0.35** | **−0.58** | pump/main/draw, 3 bands |
| mode 2 vs mode 1 | −0.07 | −0.85 | +0.01 | −0.04 | pump |

Mode 1 takes 0.2-0.4 ms off each guest thread and gives 0.4 back on `cz-draw`, which is
now confined to six cores with everything else: the guest got faster and waited on us.
Mode 2 gives every stage of the pipeline its own core: **every CPU column falls, monotone
in every matched band — the renderer thread −0.52, the Main Thread −0.35, the Draw Thread
−0.58 — and p99 12.0 -> 11.0 ms.** Per-run, the first mode-2 run read 9.00 ms median at
8,700 draws against 9.98-10.02 for the stock arm at 8,750 (campaign 1, same hour).

**And the WALL column cannot read a change this size on this route, for a reason that
was in the log all along:** `[kernel] fps cap: 500 fps requested — vblank period 1 ms,
present interval 2, so the cap is 2 ms and the ladder steps 1 ms`. The title presents on
a vblank, our vblank is a 1 ms tick, so the presented frame time is QUANTISED to whole
milliseconds — the medians of every arm of every campaign today read 9.0x or 10.0x and
nothing between, and a run flipped 9.11 -> 9.99 mid-soak at the same draw count
(`c2B_pin1_3`) when the machine's clock drifted (90 minutes of back-to-back crowd runs;
every CPU column rose ~6% at the same moment). A 0.5 ms change shows in the wall only
where it crosses a boundary. **The fine instrument for a change of this size is the CPU
per frame of each stage, and the frame follows its longest stage** (gotcha 572 — and
gotcha 237's shape at a finer quantum).

**Default: mode 2 ON from eight physical cores with SMT** (`CZ_GUEST_PIN=0` the
control; `=1`/`=2` force either). Off below eight cores — mode 2 reserves four, and what
the guard pool, the Havok workers and the audio pump do on the two that remain of a
six-core part is unmeasured — and off without SMT, where the empty sibling does not
exist. Linux only; Windows is a no-op and owed.

## §4b. Campaign 3 — A/B/A on the shipped binary (14:08-14:18)

The final build (`c3.bin`: the default decides mode 2 from the topology), control
`CZ_GUEST_PIN=0`, back to back in the machine's late (slow-clock) state, last four
windows of each soak:

| run | draws | wall mean | wall median | p99 | `cz-draw` | Main | Draw |
|---|---|---|---|---|---|---|---|
| A1 no pin | 9,150-9,180 | 10.24-10.31 | 10.02-10.04 | 12.0-13.0 | 9.76-9.81 | 8.52-8.67 | 6.80-7.08 |
| **B pin (default)** | 8,390-8,430 | **9.00-9.04** | **9.00** | **10.0-10.1** | **8.26-8.34** | **7.78-7.85** | **5.77-5.81** |
| A2 no pin | 8,600-8,640 | 9.60-9.76 | 9.92-9.96 | 11.0-12.0 | 9.08-9.26 | 8.18-8.44 | 6.35-6.45 |

Against A2 (the nearer band, +2.5% draws): wall mean −0.6, `cz-draw` −0.84, Main −0.5,
Draw −0.6, p99 −1.9 — the same shape as campaign 2's matched bands, and the wall MEAN
(which averages across the quantum) reads it where the median cannot. The band gap
overstates it by perhaps 0.2; the campaign-2 matched-band numbers (§4) are the ones to
quote.

## §4c. The operator's 3440x1440, one run each (14:50-15:00) — a check, not a claim

| run | draws | wall mean / median | p99 | `cz-draw` | Main | Draw | Main single waits |
|---|---|---|---|---|---|---|---|
| no pin | 9,040-9,070 | 9.94-9.99 / 10.00 | 12.0 | 9.41-9.50 | 8.42-8.48 | 6.80-6.85 | 0.79 / 5.7 |
| **pin (default)** | 9,510-9,555 | 10.02 / 10.00 | **11.0** | 9.29-9.33 | **7.96-8.03** | **6.30-6.32** | 1.30 / 5.8 |
| pin + Havok 3 | 9,230-9,380 | 9.80-10.03 / 9.97-10.00 | 11.0-11.9 | 9.18-9.37 | 8.10-8.20 | 6.28-6.32 | 1.24 / 7.8 |

The pinned run landed 5% heavier and still reads Main −0.45, Draw −0.5, p99 −1.0; the
wall is at the 10 ms quantum on both (at this resolution the GPU is the other bound —
part 117 §4.5). Havok 3 shows the same tail-wait shape as 4 (two more single waits a
frame, Main CPU no lower than the default at fewer draws): stock stays.

## §4d. Windows — czwin (i7-12700H, 6 P-cores with SMT + 8 E-cores, RTX 3070 laptop), evening

The Windows spelling shipped (`thread_budget.cpp`: per-thread `SetThreadAffinityMask`,
the process mask left whole, a Toolhelp32 sweep, `NameSelf` pinning `cz-pump`/`cz-draw`
from the threads themselves; both platforms now reserve only cores WITH an SMT sibling,
which on a hybrid part keeps the frame's threads off the E-cores). The boot log on czwin:
mode 2, Main Thread -> cpu 10, Draw -> 8, `cz-pump` -> 6, `cz-draw` -> 4, the rest on
mask `ff00f` (two P-cores + the eight E-cores). `tools/windows/crowd_ab.ps1` is the
route; one 3.5-minute run each way, warm pipeline cache, headless 1920x1080, the last
five windows at the crowd:

| arm | draws | wall mean | wall median | p99 |
|---|---|---|---|---|
| no pin (`CZ_GUEST_PIN=0`) | 8,010-8,200 | 22.5-24.8 | **21.9-23.9** | 35-46 |
| **pinned (the default)** | 7,690-7,930 | 14.2-15.7 | **13.1-15.0** | 23-27 |

**−38% on the wall, 42 -> 70 fps, at 4% fewer draws** — one run a side, so the size is
approximate, but it is ten times this route's noise. On the hybrid part Windows had been
scheduling the guest's threads onto E-cores and SMT siblings; the pin is worth far more
there than on the desktop Ryzen. The `guest main`/`draw` columns on Windows read ~0 and
are NOT yet trustworthy (GetThreadTimes on the registered handle; to be checked) — the
wall carried this comparison. czamd (Ryzen 5 5500, 6c/12t, the six-core shape) has the
build and the runner deployed and is unmeasured: the operator asked for short runs and a
czamd boot alone is 90-130 s.

## §4e. Windows — czamd (Ryzen 5 5500 6c/12t, RX 6600), and THE WINDOWS TIMER

The first czamd run of the pin read a FLAT **46.8 ms a frame at 150 draws and at 8,000**
(mode 1 and no pin alike, `a_pin1_short`/`a_nopin_short`, headless 1280x720) where part
106 had read 9.9 ms at 2,490 draws on the same box. 46.8 = 3 x 15.625: **the runtime had
never asked Windows for the 1 ms timer**, so every 1 ms sleep in the frame path (the
pump's nap, the vblank tick, the fence park's bounded wait) ran at the system default
granularity of 15.6 ms whenever nothing else on the machine held it at 1 ms — part 106's
czamd numbers were taken with the operator's browser open; tonight the box had nobody on
it. On Windows 11 the resolution is per process unless the window is in the foreground,
so a player's game behind another window is in the same state. `timeBeginPeriod(1)` at
start-up (`main.cpp`, `CZ_NO_TIMER_PERIOD=1` the control), one run each, same route:

| czamd, 1280x720 | menu (2,470 draws) | crowd (7,900-8,100 draws) | crowd p99 |
|---|---|---|---|
| before (no timer) | 46.8-47.2 ms | **46.2-46.9** | 49-64 |
| **timer, no pin** | 8.0 | **13.0** (mean 13.5) | 18.0 |
| timer, `CZ_GUEST_PIN=1` (the six-core shape) | 7.0 | **13.0** (mean 13.3) | **16.0** |

**21 -> 77 fps at the crowd from one call.** It is the largest Windows finding this port
has had, it is release-worthy on its own (the public Windows stutter reports have a new
candidate), and it was invisible for 118 parts because every Windows measurement was
taken with the operator at the machine and a browser holding the timer.

The six-core shape (mode 1, two cores reserved, four for everything else): no loss
anywhere, the menu −1 ms, the crowd p99 −2 ms, the median at the 1 ms quantum either
way (the box is GPU-bound there — part 103). One run; the default stays OFF under eight
cores, and `CZ_GUEST_PIN=1` is safe to try on such a machine.

## §5. The honest answer

The Main Thread's CPU at the operator's crowd is **8.3 -> 7.9 ms** (mode 2), and that
is the whole of what this part could take off it in three and a half hours, because:

1. **The thread is pointer-chasing the title's heap** — 31k demand DRAM fills a frame on
   a flat profile — and nothing but the title's own data layout changes that. Codegen is
   dead (part 116), a spin is dead (part 117), the guard's L3 footprint is a null here.
2. **Its work cannot be moved by us**: it is 100% recompiled title code with one
   exception — Havok's pool, which the title sizes for a 360. Four workers DO move the
   work (Main CPU −0.21) and the Main Thread then parks for them (+0.5 ms in waits); the
   title's `hkJobQueue` design has the calling thread finish the tail of every step, and
   with two workers it rarely has to wait for anyone. More cores, more waits.
3. **What we COULD take was ours to give**: the 17% of extra cycles our renderer's
   presence costs the thread (§1) was a third L3 eviction and two-thirds the scheduler
   putting the Main Thread on an SMT sibling of one of our saturated threads 29% of the
   time, migrating it on every wake. Giving each of the four pipeline stages a physical
   core of its own returns ~0.35 ms to the Main Thread, ~0.6 to the Draw Thread and ~0.5
   to `cz-draw`.
4. **The frame is a three-stage pipeline within 5% of itself** (§2) — Main Thread, Draw
   Thread + our fence, `cz-draw` — so no single stage's saving moves the wall alone; mode
   2 is the first change since part 117 that moves all three at once.

**The operator played it (15:10-15:40, their settings): *"feels smoother, above 100 fps
almost all the time"* — 101-111 fps at 8,900-9,100 draws, Main 7.9-8.4 / Draw 5.3-5.7 ms
CPU, p99 11-13.**

What is owed: the Windows spelling of the pin (SetThreadAffinityMask + a process mask;
`thread_budget.cpp` has the seam), the operator's session on the default (does the
confinement change how it feels — the p99 says better), the six-core and no-SMT shapes,
and the frequency drift: `amd-pstate-epp` at `balance_performance` let the clock fall
~6% ninety minutes into today's runs, which is a machine fact every future campaign
should print (`/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` once a window).
