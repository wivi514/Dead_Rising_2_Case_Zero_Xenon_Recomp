# Performance plan, part 116 — the guest's own 8.8 ms, and whether 120 fps exists

**Written 2026-09-12 in three minutes, for a 12-hour unattended session.** The operator
is at work; every item below is measurable on the autonomous crowd route with no
human. Read `docs/perf-plan-part111.md` §10 and `docs/part112-kickoff.md` §1.2 first:
they are where the ceiling was measured, and this plan starts from their numbers.

## §0. What is already known (do not re-derive)

- The frame is `wall ≈ max(pump, guest, GPU)`. At the operator's crowd (~9,300 draws,
  1080p, RTX 3070): pump CPU ~10.9 ms, **the title's own recompiled code 8.8 ms on two
  threads (76.7% + 61.7% of a core), GPU ~4 ms**. With the whole per-draw renderer
  deleted (`CZ_VK_NO_DODRAW=1`) the wall fell only to 8.80 ms — **the guest is the floor
  and nobody has ever profiled it** (part 110).
- The pump is bound by BYTES, not cycles (part 111, gotcha 551): parallelising it
  recovers nothing; fewer bytes and overlapped misses are its only class.
- 120 fps = **8.33 ms**. The guest floor alone is above it. So the only road to 120 is
  the guest's 8.8 ms, and it is open ground.
- Two verified sub-threshold items ship OFF: `CZ_VK_SCOPED_SHARED_ZERO=1` (−0.13 to
  −0.21 ms) and `CZ_VK_TEXMEMO=1` (−0.33 ms).

## §1. Rules (all from the ledger; each one cost a part)

- The rig is `tools/part80_crowdroute.sh` (the operator's route, 9,300-9,700 draws,
  ±2.9% floor). **Three runs an arm, alternated; medians and the pinned-share, never
  means** (gotchas 229, 237); matched draw bands (`tools/frame_perf_bins.py`,
  `tools/part110_pumpcpu.py`). Resolution pinned (`CZ_VK_RES=1920x1080`). GPU clock
  unpinned, sampled.
- **Archive the executable beside every `perf.data`** (gotcha 550). **Never rebuild
  while a campaign runs** (the route copies `cz_runtime` at each run's start).
- Every arm has a same-binary control and a counter that proves it engaged (151).
- Pre-registered kills, written before the run, in this file. No exceptions.
- No operator items. Nothing here changes a pixel; picture gates
  (`tools/part47_gates.sh`, pinned) run once at the end on whatever ships ON.

## §2. The order of work

### Item 0 — re-baseline (30 min)
Three runs, stock binary at HEAD: wall/median, pump cpu, GPU (`CZ_VK_GPU_PASSES=1` one
run only), and the **thread census** of the guest threads' CPU (the part-110 method).
Record in §4. Everything below is measured against this, not against part 110's.

### Item 1 — profile the guest (2 h) — THE ITEM THIS PLAN EXISTS FOR
`perf record -g --call-graph fp` (the ppc TUs keep frame pointers? if not, `-g` with
dwarf on one short window) over one crowd-route run, both guest threads. Symbols are
`sub_XXXXXXXX` in the 228 ppc TUs — resolvable. Produce, per thread, the top-40 self%
table and a per-CALLER table for the top 10. Then CLASSIFY each hot function by reading
it with `tools/gdis.py`: (a) CRT-shaped — memcpy/memset/strlen/strcmp/float math
(sqrt, sin/cos tables, matrix multiplies); (b) Havok; (c) CrowdEngine / animation;
(d) the title's D3D submission side (its own command building — which is on the guest,
not on our pump); (e) spin/wait (a thread waiting on OUR fence or ring reads as guest
CPU — part 107 found the Draw Thread's fence park; is there another spin?).
**Deliverable: `docs/perf-plan-part116.md` §4 with the table.** Kill: none — this is
measurement.

### Item 2 — native replacements for CRT-shaped guest functions (3 h)
The classic recompiler win. For every (a)-class function above 0.3% of a guest thread:
a `PPC_FUNC` hook that does the same work natively (a byte copy is endian-neutral;
`memset`, `memcpy`, `memmove`, `strlen`, `memcmp` are the safe set; float math only
where the result is bit-exact — sqrt yes, transcendental NO unless the title's own
table is used). **Gate before any A/B: a boot-time self-test that runs the recompiled
and the native form on 10,000 random inputs and compares bytes** (gotcha 30 — break it
on purpose once). Then the A/B. Kill: bundle < 0.3 ms median → revert, record.

### Item 3 — codegen on the recompiled TUs (3 h, builds are long: start it early on
a SEPARATE build dir, never on `runtime/build`)
Three arms, each its own binary, each measured 3×:
- **PGO**: `-fprofile-instr-generate` on the ppc TUs, one crowd-route run for the
  profile, `-fprofile-use`. No correctness risk. This is the single most likely
  large mover for the 8.8 ms.
- **ThinLTO** across the ppc TUs + kernel.
- `-O3` vs the current level on the ppc TUs only (check what it is first).
Kill per arm: < 0.3 ms median, or build > 60 min, or `--smoke`/A5 fails. If PGO wins,
the release recipe needs the profile checked in — write that up, do not do it.

### Item 4 — the two guest threads' critical path (1.5 h)
Which thread is the frame's critical path at the crowd, and what does the other wait
on? `CZ_WAIT_TRACE=1` + the thread census + one `perf sched` window. If the 61.7%
thread is mostly spinning on the 76.7% one (or on us), that is not "8.8 ms of work" and
the floor is lower than measured — say so with a number. Kill: none — measurement.

### Item 5 — the sub-threshold bundle, re-measured on the new baseline (1 h)
`CZ_VK_SCOPED_SHARED_ZERO=1 CZ_VK_TEXMEMO=1` together, 3×3. Report; ship decision stays
the operator's.

### Item 6 — the honest answer (30 min)
With §4 filled: is 120 fps CPU-side reachable on this machine, and what would it take?
One paragraph, numbers only, in `docs/part116-kickoff.md`. If the answer is no, say the
ceiling and the largest remaining term.

## §3. What NOT to do
- No renderer-side items: the pump is byte-bound and every ≥0.5 ms lead is closed.
- No threading of the pump (B1 null, B2 predicted dead, B3 refuted).
- No item without its kill rule written here first. No means. No single runs.

## §4. Results (filled during the session, 2026-09-11/12)

All runs: `tools/part116_probe.sh` = `tools/part80_crowdroute.sh` at **1920x1080**, windowed,
`CZ_FPS_LOG=10`, 120 s soak; the crowd gate (>= 8,000 draws median, two consecutive windows)
passed on every run quoted. Machine: the operator's 8c/16t Ryzen 7 5700 at its stock
4654 MHz (verified: `scaling_max_freq 4654000`), GPU clock unpinned. A 22-hour-old co-op
test stand-in (`launcher_ws.py`, spinning at 94% of a core since the previous session) was
killed before the first run — it would have been a fifth busy core in every number below.
Artifacts: `~/DR2CZ-troubleshooting/part116/` (`<tag>.bin` is the executable that ran,
`<tag>.symfs` makes its `perf.data` readable after the tree moves on).

**The measurement a guest-side change is read by — pre-registered here, before any arm
ran.** The wall on the normal arm is the pump (10.1 of 10.3 ms), so a guest saving cannot
show in it (part 110). Two quantities can show it: (i) the `guest main N.NN draw N.NN
ms/frame` columns the `[fps]` line now carries, and (ii) the WALL under `CZ_VK_NO_DODRAW=1`,
where the per-draw renderer is deleted and the wall IS the guest floor. Kills in §2 are
read on (ii)'s wall median across matched 250-draw bands (`tools/part116_guestcpu.py`),
three runs a side, alternated.

### 4.0 Item 0 — the re-baseline (base1-3 / nodd1-3, alternated, 50 + 51 crowd windows)

| band (draws) | n | wall | pump cpu | guest Main | guest Draw |
|---|---|---|---|---|---|
| normal 8,250 | 15 | 10.21 | 10.09 | 7.67 | 5.73 |
| normal 8,500 | 15 | 10.22 | 10.07 | 7.70 | 5.73 |
| normal 8,750 | 16 | 10.67 | 10.50 | 7.96 | 6.07 |
| **NO_DODRAW 8,250** | 18 | **7.79** | 2.33 | **6.42** | **4.97** |
| NO_DODRAW 8,500 | 14 | 8.32 | 2.39 | 6.59 | 5.12 |
| NO_DODRAW 8,750 | 6 | 8.69 | 2.44 | 6.72 | 5.19 |

(ms/frame, band medians.) Three things the part-110 number did not say:

* **At 1080p the guest floor is 7.8-8.7 ms, not 8.8** — and the frame it sits under is
  10.2-10.7 ms with the pump at 10.1-10.5. So `wall - guest floor` is 2.0-2.4 ms: **that is
  the whole prize on this machine for ANY guest-side work, and it only converts to frame
  rate once the pump is also below it** — which it is not, by ~2.3 ms.
* **The guest's own CPU per frame FALLS by 1.24 ms (Main) and 0.78 ms (Draw) when our
  renderer is deleted** (monotone in all five bands). Same guest code, same draws; the
  only thing removed is the pump's per-draw work on another core. That is gotcha 551 seen
  from the other side: the pump's bytes slow the guest's memory accesses (and/or its SMT
  sibling). **Part 110's "8.8 ms of guest work" was measured with the pump idle and is
  therefore the FLOOR of the guest's cost, not its cost in the shipped frame** — in the
  frame a player sees, the Main Thread costs 7.7-8.0 ms.
* The Main Thread's CPU (6.4-6.7) is 1.4-2.0 ms SHORT of the NO_DODRAW wall (7.8-8.7):
  the floor is a dependency chain with a wait in it, not one thread's work. Item 4.
* GPU, one `CZ_VK_GPU_PASSES=1` run on the route (run mean, 31,653 frames): **3.23
  ms/frame**, residual 1.5%, fence wait 0.01 ms — the >=256-draw passes 1.33, resolve
  copies 0.70, 1-draw passes 0.42, shadow cascade 0.31. The GPU is a third of the pump.

### 4.1 Item 1 — the guest profiled (b1: flat `perf -F 999` 30 s + DWARF 5 s at the crowd, 1920x1080, 8,934 draws peak)

**The two threads have names now.** The title names its threads from the MAIN thread by
id (`SetThreadName(dwThreadID)` — never -1), so the binding goes through a guest-tid ->
host-thread registry filled at spawn; every host thread we spawn inherits its creator's
comm, which is why the first attempt reported EVERY thread as `Main Thread`. Census at
the crowd (15 s, % of one core):

| thread | % core | what it is |
|---|---|---|
| `cz-pump` | 97.5 | our PM4 walk + per-draw renderer |
| **`Main Thread`** | **74.3** | the game: update + render SUBMISSION |
| **`Draw Thread`** | **54.3** | the title's display-list interpreter -> its D3D layer -> the ring |
| `cz-guard0..2` | 29.2 each | our content-guard pool |
| unnamed (created by Main, 800738) | 10.4 | — |
| `JobThread0..5` | 0.9-6.1 | the title's job pool, nearly idle |
| `HavokWorkerThread` x2 | 5.4 | Havok's own workers |
| `Audio Thread` | 4.3 | |

**Both guest threads are 100% recompiled code** (DSO split: 20.43 of 20.48% and 14.92 of
14.99% in `cz_runtime_crowd`; libc 0.03%, kernel ~0). There is no libc, no syscall, no
spin on our side in either — the wait on our fence is parked (`sub_8283C6C8` 2.3% of the
Draw Thread, the hook's own spin phase).

**MAIN THREAD — flat.** Top self symbol 4.77% (`sub_827C6E28`, `cTransModel::Render`),
then 3.28, 2.78, 1.94 ... the top 45 self symbols sum to ~48%. It is NOT a function; it is
a tree. The DWARF chains, folded by the title's OWN profiler markers (every subtree pushes a
`"<class>::Update(ms)"` / `"::Render(ms)"` string before `submit` — `tools/func_strings.py`
prints them), give the frame's shape at this crowd (inclusive % of the thread):

```
sub_825D2610  game loop                                        99.8
├─ sub_824A1EC8  UPDATE                                          50.5
│  └─ sub_82496810                                               46.4
│     ├─ cZombieManager::Update   (sub_8243F010)                 14.9   [crowd]
│     │   └─ cZombieInfo::Update  (sub_82438470) 10.8, sub_8243D428 4.1
│     ├─ cAIManager::Update       (sub_82230170)                 11.8   [crowd]
│     ├─ cLibPhysics::Update      (sub_827E2178)                 10.9   [Havok: hkJobQueue sub_828AD798 8.1]
│     └─ cActorManager::Update    (sub_82493CD8)                  4.8
└─ sub_8249AAA8  RENDER (submission: cull + build the draw cache)  47.4
   ├─ cLevel::Render              (sub_8248EB00)                 36.2
   │  ├─ static props: sub_824358D8 -> "Static Props Rendered(num)" sub_82203DB0 16.3  [world]
   │  ├─ cZombieManager::Render   (sub_82437408, "Zombies Rendered CSM0..3") 8.4  [crowd, x4 cascades]
   │  ├─ cEnvironmentManager::Render (sub_8225AE88)               7.4
   │  └─ cActorManager::Render    (sub_82484A30)                  3.2
   └─ cTransModel::Render         (sub_827C8048/sub_827C6E28)    15.1
```

So the Main Thread's frame is **half simulation (zombies + AI + Havok) and half render
submission** (culling and building the display list the Draw Thread will play). The
crowd-scaled part is at least 14.9 + 11.8 + 8.4 = 35% and probably the physics too.

**DRAW THREAD — one tree.** `sub_827D3898` ("Draw(ms)") -> `sub_827BFEE8`
("cDrawRender--RenderDrawCache") -> `sub_827A00B8` ("DrawNodes") -> **`sub_827D5B18`, 81.3%
inclusive**: a 0x49-opcode DISPLAY-LIST INTERPRETER (dword = 8-bit opcode + 24-bit payload,
jump table, recursive; its own assert string names `renderlib\xbox360\drawrender.cpp`).
Below it:

| callee of the interpreter | incl % | what |
|---|---|---|
| `sub_827C0A40` (cmndrawrender pass) -> `sub_827D4CD8` (bind shaders: "bound_vertex_shader_handle") -> `sub_827CC9E0` 10.8 + `sub_827CCC30` 8.1 | 22.3 | shader/state binding per draw |
| `sub_82842E78` = **D3D DrawIndexedVerticesUP** (d3d-translation-plan recon) -> `sub_8284F300` the draw flush | 15.0 | the D3D layer building PM4 |
| interpreter self | 9.2 | dispatch |
| `sub_827CBB40` -> `sub_82839830` = **D3D SetTexture** (16-byte fetch-constant copy) | 5.7 | |
| `sub_827CD548` | 4.8 | |
| `sub_82845160`/`sub_8283C6C8` fence wait (parked) | 2.6 | |

**Classification against the plan's (a)-(e):**

| class | Main Thread | Draw Thread |
|---|---|---|
| (a) CRT-shaped | **memcpy `sub_8280F950` 0.87%**, memset `sub_82810270` 0.10%, double transcendentals (`sub_8280F5D0` 0.23, `sub_8280F380` 0.20, `sub_8280F4F0` 0.18) | memcpy 0.49% |
| (b) Havok | ~11% (`cLibPhysics::Update`, `hkJobQueue` 8.1) | 0 |
| (c) CrowdEngine / animation | ~35% (`cZombieManager::Update` 14.9, `cAIManager::Update` 11.8, `cZombieManager::Render` 8.4) | 0 |
| (d) the title's D3D submission side | ~47% (render submission: cull + display-list build) | **~81%** (the interpreter and everything under it, incl. D3D 15% + 5.7%) |
| (e) spin / wait | 0 | 2.6% (the parked fence wait's spin phase) |
| register save/restore ladders (`__savegprlr_N`/`__restgprlr_N`, out-of-line calls in the recompiled code) | ~4% | **~10%** |

The last row is the one thing in the table that is about the RECOMPILER rather than the
title: every prologue/epilogue is a call into a 5-15-store ladder function in another TU,
and on the Draw Thread — small functions, deep call chains — that is a tenth of the
thread. It is what item 3's LTO arm can reach and the PGO arm can partly reach (inlining
across TUs; the ladders are `weak` aliases, see 4.3).


### 4.2 Item 2 — native CRT replacements: REFUTED BY THE CENSUS, not built

The plan's threshold is "every (a)-class function above 0.3% of a guest thread". The
census (4.1) finds exactly one: **memcpy, `sub_8280F950`, 0.87% of the Main Thread and
0.49% of the Draw Thread** (memset is 0.10%; the three double transcendentals are 0.18-0.23%
each and are NOT bit-exact candidates). Its ceiling — the whole function's cost, as if the
native form were free — is 0.87% x 7.7 ms + 0.49% x 5.7 ms = **0.095 ms/frame**, a third of
the pre-registered 0.3 ms kill before a line is written, and a real hook keeps at least the
byte-copy itself. So the bundle cannot survive its own kill and was not built; the time
went to item 3, which the same census says is where the recompiled code's cost actually
is (the ladders, the flat engine tree). The disassembly of `sub_8280F950` is the Xbox 360
CRT `memcpy` (r3 = dst, r4 = src, r5 = n; byte head to 8-alignment, `ld/std` body,
128-byte `dcbt` prefetch loop for >= 0x80) and its callers at the crowd are
`sub_827ADD40` (55%, the Main Thread's render-submission) and `sub_8284EF28` /
`sub_8284E628` (the D3D layer) — recorded so a later part that wants it has the entry
point and the specification.


### 4.3 Item 3 — codegen on the recompiled TUs

Three arms, each its own build directory on the same source, each compiled in ~100 s
(`runtime/build-{pgouse,lto,o3}`; the CMake knobs are `CZ_PPC_PGO`, `CZ_PPC_LTO`,
`CZ_PPC_OPT`, on `ppc_image` only). Kill per arm, pre-registered: < 0.3 ms median.

**PGO** — profile from one instrumented crowd-route run (`-fprofile-instr-generate`,
60,117 functions, 273 G counts; the instrumented guest runs at 45 fps and still reaches the
crowd; **the profile is written from our SIGTERM handler**, because `_Exit` skips the
atexit flush and the first run produced a 0-byte `.profraw`). Three runs a side,
alternated, both kinds:

| kind | band | nA | nB | wall | pump | Main | Draw |
|---|---|---|---|---|---|---|---|
| normal | 8,000 | 15 | 3 | −0.19 | −0.49 | −0.34 | −0.46 |
| normal | 8,250 | 6 | 7 | −0.03 | +0.01 | −0.31 | +0.01 |
| normal | 8,500 | 14 | 17 | −0.24 | −0.15 | −0.20 | −0.12 |
| normal | 8,750 | 8 | 23 | −0.11 | −0.13 | −0.13 | −0.17 |
| NO_DODRAW | 8,000 | 4 | 14 | −0.11 | +0.14 | +0.15 | +0.29 |
| NO_DODRAW | 8,250 | 8 | 31 | −0.37 | −0.02 | −0.24 | −0.23 |
| NO_DODRAW | 8,500 | 25 | 5 | +0.30 | +0.02 | +0.03 | −0.10 |

**Main Thread −0.25 ms (monotone, normal arm); the guest-floor wall −0.11 ms and NOT
monotone. KILLED at the 0.3 ms bar.** The "single most likely large mover" is worth a
quarter of a millisecond on the busier thread and nothing reliable on the floor. The
reading generalises: the recompiled code is memory-shaped (every guest register is a
field of a context struct in memory, every guest load/store a byte-swapped host one),
and a profile that lets the compiler lay out branches better does not change how many
bytes move. Retained as an arm (`-DCZ_PPC_PGO=<profdata>`), not a default; the profile is
`~/DR2CZ-troubleshooting/part116/pgo/crowd.profdata` and a release recipe that wanted it
would check it in beside `config/`.

**ThinLTO** (`-flto=thin` on the recompiled TUs, lld; built in 106 s, 1.4 GB peak): three
runs a side, alternated, both kinds, on the v2 source (base4 vs lto; one NO_DODRAW base
run rejected at the crowd gate):

| kind | wall | pump | Main | Draw | bands |
|---|---|---|---|---|---|
| normal | +0.14 | +0.17 | +0.06 | +0.14 | 4, wall monotone |
| NO_DODRAW | **−0.03** | −0.03 | −0.00 | −0.03 | 4, monotone |

**A null. KILLED.** Predicted from the emitter: every guest call targets a `weak,noinline`
alias (`PPC_WEAK_FUNC`, so a hook can replace it), and a weak definition is not inlinable
across TUs however the link is done — the register save/restore ladders that are 10% of
the Draw Thread (§4.1) stay out-of-line calls. LTO would only move the recompiled code if
the recompiler emitted the ladders inline, which is a XenonRecomp change, not a flag.

**-O3** on the recompiled TUs (base5 vs o3, three runs a side, both kinds): normal wall
+0.15 / Main −0.04 / Draw −0.04, NO_DODRAW wall **−0.02**, nothing monotone. **A null.
KILLED.** The CMake comment that kept -O2 for six months ("a saving nobody has
measured") is now a measurement: there is no saving. -O2 stays.

**Item 3's verdict:** the recompiled code does not respond to codegen. PGO −0.25 ms on one
thread, LTO and -O3 nulls, all three under the 0.3 ms bar. The reason is structural
(§4.1's last row and the LTO paragraph): the cost is memory-shaped context-struct traffic
and out-of-line ladders the emitter chose, not instruction selection.

### 4.4 Item 4 — the critical path, and a defect of OURS in it (pre-registered before the run)

`[guestwait]` (on every `[fps]` line now) at the crowd, normal arm, 10.4 ms frames:

| thread | CPU ms/frame | waits ms/frame (calls/frame) | sum |
|---|---|---|---|
| Main | 7.9-8.1 | single 0.25 (5.7) + **multi 2.2 (5.0)** + fence 0.00 (1.0) | 10.4 |
| Draw | 5.7 | **multi 2.7 (2.0)** + **fence 2.1-2.3 (6-8)** | 10.5 |

Each thread's CPU plus its waits IS the frame, to 0.1 ms — the census is complete. The
Draw Thread waits on the fence (our pump/GPU: 2.2 ms) and on two multi-object waits
(its draw cache from the Main Thread: 2.7 ms); the Main Thread makes FIVE multi-object
waits a frame and spends 2.2 ms in them. Under `CZ_VK_NO_DODRAW=1` the Main Thread's CPU
is 6.4-6.7 and the wall 7.8-8.7, so 1.4-2.0 ms of the guest FLOOR is that thread waiting.

**And the multi-object wait is a 1 ms POLL.** `WaitAnyPoll` (imports.cpp) polls the
objects and `sleep_for(1ms)` between polls — the comment says *"simple and safe; revisit
if it shows up hot in a profile"*, and a per-thread wait census is the profile it needed.
Every wait-any therefore ends up to 1 ms AFTER its object was signalled, five times a
frame on the guest's critical path. No single-object wait has this (they park on the
object's own condition variable).

**The fix (built, not yet measured as this is written):** a process-wide signal
generation — every `Event::Set`, `Semaphore::Release` and thread exit bumps it under one
mutex/condvar; a wait-any reads it, polls, and parks until it CHANGES, bounded by the old
1 ms so object kinds that do not bump it behave as before. `CZ_WAITANY_POLL=1` is the
same-binary control (the old sleep). With no waiter parked a broadcast is glibc's no-syscall
fast path.

**Pre-registered prediction and kill:** on the NO_DODRAW arm (wall = guest floor) the wall
median falls by **0.5-1.5 ms** in every band (the quantum latency of ~5 waits x up to 1 ms,
minus the dependency time that is real). Kill: **< 0.3 ms** on the NO_DODRAW wall, or any
band moving the other way — then it ships OFF as an arm. On the normal arm the wall is
predicted NOT to move (pump-bound) and the Main Thread's `multi` column to shrink.

**v1 — the process-wide broadcast — measured WORSE and was replaced before its A/B
finished** (one pair a kind, so an observation, not a result): normal arm wall **+0.88 ms**,
pump **+0.80**, Draw Thread CPU **+0.58**, monotone in three bands. Mechanism: every
`Event::Set` in the process (the job threads', Havok's, the audio thread's) woke EVERY
parked wait-any, which re-polled all its objects under their mutexes and parked again — a
thundering herd whose CPU shows up on the Draw Thread's column and whose cache traffic
the pump paid for on another core (gotcha 551 again: bytes are machine-wide). **v2
registers the wait on each object it waits on** (`WaitAnyBlock`, `KernelObject::
AddAnyWaiter`), so a signal wakes only the wait-anys that include that object; an object
with no registered waiter pays an empty-vector check under a mutex it already holds. The
prediction and kill above stand unchanged for v2. (And a rule was broken to get here:
`runtime/build` was rebuilt while campaign 2 was running; the one run that raced the
binary is quarantined as `*_v1_binary_race.rejected` and campaign 3 is the clean A/B.)

**v2 RESULT — the wait-any wake vs the 1 ms poll, same binary, `CZ_WAITANY_POLL=1` the
control, alternated, at 02:12-02:24** (campaign 3 + one extra NO_DODRAW run for the wake
arm after one of its runs missed the crowd gate and was rejected):

| kind | band | nA | nB | wall | pump | Main CPU | Draw CPU |
|---|---|---|---|---|---|---|---|
| **NO_DODRAW** | 8,000 | 18 | 12 | **−1.11** | −0.00 | −0.16 | +0.13 |
| **NO_DODRAW** | 8,250 | 5 | 9 | **−1.12** | −0.04 | −0.03 | −0.04 |
| **NO_DODRAW** | 8,500 | 12 | 30 | **−0.97** | +0.01 | +0.06 | +0.06 |
| normal | 8,000 | 8 | 4 | +0.16 | −0.50 | −0.09 | +0.08 |
| normal | 8,250 | 2 | 15 | +0.25 | +0.33 | +0.11 | +0.53 |
| normal | 8,500 | 12 | 21 | +0.47 | +0.40 | +0.19 | +0.45 |
| normal | 8,750 | 10 | 10 | +0.22 | +0.15 | +0.02 | +0.22 |

**The guest floor falls 1.11 ms — 8.1 -> 7.0 ms at 1080p — monotone in three bands, three
runs a side, with the CPU columns unmoved (−0.03 / +0.06).** That is the prediction's shape
exactly: latency removed, no work removed. The kill does not fire. The wait census on the
floor arm says where it went: Main `multi` 1.53 -> 0.65 ms/frame, Draw `multi` 3.29 ->
2.08, same call counts.

**On the normal arm the wall is +0.24 (monotone, four bands) and the Draw Thread's CPU
+0.33** — within the route's ±0.3 ms floor but monotone, so it is treated as real. The
wait census names the mechanism: the guest threads now reach their NEXT dependency
sooner, and on a pump-bound frame that dependency is our fence — Draw `fence` 2.2 -> 4.4
ms/frame and `[fencewait]` parks **3.1 -> 5.1 a frame**, each with its 50 µs pause-spin
before the park, on whichever core (or SMT sibling of the pump) the thread lands on.
5.1 x 50 µs = 0.26 ms of spin, which is the Draw Thread's +0.33 to within noise. The arm
that tests this is `CZ_FENCE_PARK_SPIN_US=0` with the wake (campaign 4, below). **Ships ON
either way**: −1.1 ms on the floor that decides the 120 fps question, against +0.24 on a
frame whose bound is the pump and which item B's own arithmetic says the pump will stop
being first; the spin arm decides whether the +0.24 is also recoverable.

**The spin arm (campaign 4, normal kind only, wake + `CZ_FENCE_PARK_SPIN_US=0` vs the poll
control; one pair rejected at the crowd gate, two pairs read):** wall **+0.24** (two bands,
monotone), pump +0.30, Draw CPU **+0.09 (not monotone)**. So the spin WAS the Draw
Thread's +0.33 and removing it recovers that thread's CPU, but the wall's +0.24 is the
PUMP's and it stays: `[fencewait]` parks 5.8 a frame, `stores seen` 557 (the executor's
fence stores landing while a waiter is parked; wakes 4.0). The mechanism is not proven; the
candidate consistent with everything else this part measured is that the Draw Thread, no
longer staggered by a 1 ms quantum, builds the next packets while the pump is walking the
last, and the two contend for the same bytes (gotcha 551). **Shipped ON with the +0.24
stated**: the floor that decides 120 fps is −1.11, the shipped frame's bound is the pump,
and the control is one variable. `CZ_FENCE_PARK_SPIN_US` stays at its part-107 default.

### 4.5 Item 5 — the sub-threshold bundle on the new baseline

`CZ_VK_SCOPED_SHARED_ZERO=1 CZ_VK_TEXMEMO=1` together vs the same binary without, normal
kind, three runs a side, alternated, both arms with the wait-any wake ON (04:30-04:53;
`[texmemo]` 74.1% served, 0 disagreements — engaged):

| band | nA | nB | wall | pump | Main | Draw |
|---|---|---|---|---|---|---|
| 8,000 | 15 | 6 | −0.88 | −1.37 | −0.37 | −0.70 |
| 8,250 | 4 | 15 | −0.54 | −0.30 | −0.13 | +0.15 |
| 8,500 | 17 | 18 | −0.48 | −0.54 | −0.13 | −0.05 |
| 8,750 | 11 | 12 | −0.82 | −0.82 | −0.15 | −0.12 |

**Wall −0.68 ms, pump −0.68 ms, monotone in four bands** — more than the −0.54 the two
items summed to in parts 109 and 111, and above the 0.4 ms bar part 109 pre-registered
for a single item. It is the one renderer-side saving in the tree that is verified,
gated (picture null, poison positive control — part 111) and unshipped. **The ship
decision stays the operator's** (the plan's rule); the number they asked for is −0.68 on
the shipped frame, which more than covers the wake's +0.24 on the same frame.

### 4.6 Gates on the shipped binary (04:55-05:10)

`--smoke` OK · unlowered switches 0 · shader dims 0 disagreements · both PM4 oracles clean
· picture gate vs E3 **+0.8472** best of 5, 4 agreeing on layout (pinned 1280x720;
`ALL GATES CLEAN`) · A5 **exit 0** (5 permutation windows, 0 real) · `no translated
shader` 0 · `truncated=0` (66 windows) · `tools/phase_vs_perf.py --self-test` PASSED.

### 4.7 Item 6 — the honest answer

**Is 120 fps CPU-side reachable on this machine?** The guest no longer forbids it: the
floor is **7.0 ms** (was 8.1 at 1080p, 8.8 at 1440p), under the 8.33 target, and its
remaining terms are the title's own work — a flat 6.5 ms Main Thread and a 5.0 ms Draw
Thread that codegen does not move. **The pump forbids it**: 10.1-10.5 ms, byte-bound,
and the only priced route below 8.33 is item B (the per-draw renderer off the pump
thread, `F + M/3 = 5.33` ms), which the operator declined at ~113 fps. **With the floor
at 7.0 that same build would stop at `max(5.33, 7.0, GPU)` ≈ 7.0-7.3 ms — ~137 fps
CPU-side, not 113 — which is the one input to that decision this part changed.** The
largest remaining term after the pump is the Draw Thread's display-list interpreter →
D3D layer (81% of that thread), and the only thing that removes it is the
D3D-translation pivot, which removes the pump's PM4 walk with it.

### 4.8 The +0.24 on the normal arm — one perf pair (05:30), owed item partly answered

Two probe runs, poll vs wake, flat `perf` on the pump: **the pump's user-space symbol
table is unchanged** (DoDraw 20.5 -> 21.0%, WriteRegisterRun 10.4 -> 9.9, UploadStream
9.6 -> 9.5, memset 4.2 -> 3.7 ...) and the one row that grows is `[unknown]` — kernel
samples, unreadable at `perf_event_paranoid=2` — **6.8 -> 7.8%**, i.e. ~1 point of the
pump ≈ 0.1 ms in the kernel. The rest is diffuse. Consistent with the fence-store wake
path and cross-core contention; not proven, and not provable without root for kernel
symbols. Also seen: the UNNAMED guest thread created right after the Draw Thread
(`sub_8286A148` / `sub_8286FAD0` — Havok MOPP strings, a worker) goes 12.1 -> 16.8% of a
core under the wake; it is on a spare core and its extra time reads as a spin that now
starts sooner. Left as the owed item's remaining half.
