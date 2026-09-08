# Part 103 plan — AMD / Windows performance (czamd: RX 6600, Ryzen 5 5500, Win10)

**Written 2026-09-08 at the end of part 102, to be executed fresh.** Part 102 closed the
session-one pop-in (vertex recipes), made the boot warm async, and found and fixed the
czamd main-road stutter (the golden store's synchronous file write). What is left on the
AMD box is not a stutter any more — it is the frame itself, and the numbers below say
exactly which half of the machine it lives in. Read `phase5-notes.md` §6es (all of it,
including the two addenda) before touching anything; every number here comes from it.

## §0 Where czamd is, measured (crowd route replay, counter + per-frame trace, warm)

| median per frame | czamd, 2,000-3,200 draws | czamd, 8,000-10,000 draws | 3070, 8,000-10,000 draws |
|---|---|---|---|
| wall | 12.4 ms | **19.7 ms** | 11.3 ms |
| gpu (timestamps in the frame's own command buffer) | 12.2 ms | **19.5 ms** | 8.9 ms |
| record (our CPU) | 4.9 ms | 12.9 ms | 11.0 ms |
| fence (CPU blocked on the GPU) | 4.8 ms | 4.5 ms | 0.0 ms |
| sleep (pump idle) | 2.6 ms | 1.8 ms | 0.2 ms |

**czamd is GPU-BOUND at every load: wall equals gpu to within 0.2 ms, and the CPU waits
4.5-4.8 ms a frame on the fence.** The 3070 is the opposite regime at the crowd
(CPU-bound, fence 0.0). So on AMD every CPU item on the old board is worth exactly nothing
until the GPU frame falls by the whole fence (gotcha 476's rule, now pointing the other way
from part 80), and the GPU items parked in part 80 as "dead on regime" are ALIVE on this
box. The 2.2x GPU gap (19.5 vs 8.9 ms) is about the hardware gap between an RX 6600 and a
3070, so the target is not parity — it is the GPU work that is OURS rather than the
title's, priced on this GPU.

Also established: steady-state frame-time distribution is flat (p99 24 ms, 0.0% >2x
median at the crowd, headless and windowed), 50 fps at the crowd against the 3070's 78.

## §1 The board, in order

**0. The per-region GPU split ON CZAMD — before any code.** `CZ_VK_GPU_PASSES=1` on the
crowd route (`crowd_run.ps1` is the headless launcher on czamd; add the env). It prints
the residual first, then passes by draw count, resolve copies, resolve clears, the two
barrier classes, snapshot views, cube faces, present blit, and the pass extent census.
Every number in part 78-80's GPU work was taken on the 3070; the shares on RDNA2 under
AMD's driver may differ by class (barriers and clears in particular). Write the table into
this file before choosing items 1-4. Kill rule for the whole plan: an item whose class
reads under 0.5 ms on czamd is not worth its risk.

**1. MSAA.** The default is 2x (`CZ_VK_MSAA=0` is the single-sample arm; release chose 2x
in `release-github-plan.md` §1). On a GPU-bound box this is the largest free lever, and it
is a picture decision the operator makes by eye, not a measurement. Run the A/B headless
first (crowd route, both arms, fps windows + trace), then hand the operator the arm. If
the saving is large the launcher's settings panel should expose it — it already carries
shadows and resolution.

**2. Resolve copies and clears.** Part 80 priced them at 0.72 + 0.60 ms on the 3070 and
parked both because the 3070's fence was 0.00. czamd's fence is 4.5 ms. Part 90 shipped
the deferred scoped clears (0.66 -> 0.009 ms of the clear class on the 3070); the copy
census (§6el) named 36.4% of resolve copies as DEAD (never read) and parked them. Item:
skip the dead copies, measured by the GPU split on czamd, with `CZ_VK_GPU_PASSES` as the
gate and the picture era-medians (`tools/frame_era_medians.py`) as the correctness gate.

**3. Barriers.** Part 78 took 137 ALL_COMMANDS image barriers a frame to a narrower set
(-11.9% of the device frame on the 3070). AMD's driver treats a full barrier as a
heavier flush than NVIDIA's. Count what the split says on czamd; if the barrier class is
>1 ms, the remaining full barriers (the snapshot and cube paths) are the item.
`CZ_VK_SYNC_VALIDATION=1` must still read 0 hazards — on the 3070, since validation is
driver-independent; the AMD run is the picture check.

**4. The first-run pipeline warm on small machines (the last session-one cost).** On a
cold driver cache czamd creates pipelines at 155 ms each; four workers take ~60 s and
oversubscribe six cores (four compilers + pump + guest + three guard workers) for that
minute. Options, cheapest first: (a) lower the workers' priority
(`SetThreadPriority(BELOW_NORMAL)` / `SCHED_IDLE`) so the game wins the cores — no
correctness surface; (b) size the pool from the busy-thread budget on <8-core machines;
(c) order the seed by first use so the logo/title/first-street pipelines build first —
needs a first-seen frame in the key file (a v2). Measure with `VkCache` emptied
(`Get-ChildItem $env:LOCALAPPDATA\AMD\VkCache -File | Remove-Item`; the folder itself is
held open by Radeon Software) and the per-frame trace over the first 90 s; the statistic
is the >2x-median share in windows 0-6 and `pipeline: speculative build promoted by a
draw`.

**5. Boot-time file work on Windows.** The golden store preloads every file at boot
(3,811-5,266 files on czamd, 29,416 on the dev box) — thousands of opens on NTFS before the
first frame. Time it (a `[vk] golden texture store: preloaded N in M ms` line is the
instrument, missing today), then either a single packed file or a lazy load keyed by
signature. Same audit for anything else that opens files per asset on the frame thread:
`SavePipelineKeysIfDue` (77 KB rewrite every 32 pipelines — measure, probably fine), and
grep the renderer for `ofstream`/`fopen` reachable from `DoDraw`.

**6. Thread budget on six cores, stated once.** As of part 102 czamd runs: pump + guest's
two busy threads + 3 guard workers + 4 pipeline workers (idle except while creating) +
1 first-sight translation worker + 1 golden writer + the audio pump. Print it in
`ThreadBudget_Report` so a log names every pool, and decide whether the pipeline and
golden threads should count against the budget. They should not while blocked; they do
during a burst.

## §2 The black square (czamd only) — same box, separate subject

Not performance, but it is the other open czamd item and the arms are staged there:
`cz_arm_fif1.bat` (one frame in flight) first, then `cz_arm_noclear.bat`, then
`cz_arm_norecord.bat`. The operator reports it VANISHES on any capture (Win+PrtScn, F9), so
a readback's forced GPU wait removes it: a cross-frame race is the lead. No dump has ever
held it (165 automatic frames + 16 F9 snapshots scanned in part 102).

## §3 What not to do

- Do not measure a CPU item on czamd and call it a win: the regime is GPU-bound and the
  fence absorbs it (§0). Read `fence` and `gpu` in the trace before believing `record`.
- Do not quote a czamd frame time from a run with `CZ_VK_FRAME_DUMP`, F9 or the profiler
  armed: the dump alone put 1/64 of frames over 2x the median (§6es).
- Do not park stores by env var without listing the directories the process actually
  opens (gotcha 516): the golden store ignored `XDG_CACHE_HOME` for eighteen parts.
- Do not touch the shader half: on czamd the prebuild + recipes + seed make session one
  read first-sight 4 (boot only), skipped draws in the hundreds. It is done.

## §4 How to reach czamd (unchanged)

`ssh czamd` = `lisab@192.168.0.60`; PowerShell via `-EncodedCommand`; strip CLIXML by
regex. Build on czwin (`wivi5@192.168.0.153`, `C:\cz\vc.bat`), deploy with
`~/DR2CZ-troubleshooting/part102-fresh/deploy_czamd.sh` (its last `dir` line errors
harmlessly). Headless route: `C:\Users\lisab\Desktop\CaseZeroRecomp\crowd_run.ps1`
(lead-in 130 s for czamd's boot). Visible: `schtasks /run /tn cz_play` runs
`cz_play.bat` on the desktop (currently launcher + `CZ_FPS_LOG=10` + the per-frame trace
to `frame_trace.txt`). A czamd boot to title is 90-130 s — never call it hung under 150 s.
