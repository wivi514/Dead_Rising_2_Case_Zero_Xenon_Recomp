# Part 116 kick-off — the guest's own 8.8 ms, profiled; a 1 ms poll in our kernel was 1.1 ms of it

**READ `docs/perf-plan-part116.md` §4 FIRST.** It is the execution record: every number
below has its table there. This file says where the port is after the operator's 12-hour
unattended order (the plan's §2), what shipped, and what is owed.

## §0. The honest answer (item 6): is 120 fps CPU-side reachable on this machine?

**Not with what exists today, and the arithmetic now has every term named.** At the
operator's crowd (8,000-9,000 draws, 1920x1080, Ryzen 7 5700 at 4654 MHz):

| term | before part 116 | after | how measured |
|---|---|---|---|
| the pump (our PM4 walk + per-draw renderer) | 10.1-10.5 ms | 10.1-10.5 (+0.24 under the wake, §4.4) | `pump cpu` on the `[fps]` line |
| **the guest floor** (`CZ_VK_NO_DODRAW=1` wall) | **8.1** (part 110 said 8.8 at 1440p) | **7.0** | the NO_DODRAW wall, 3 runs a side |
| — of which the Main Thread's CPU | 6.4-6.7 | unchanged | `guest main` column |
| — of which waits (dependency chain) | 1.4-2.0 | **0.3-0.9** | `[guestwait]` |
| GPU | ~4 ms at 1080p | unchanged | part 106 |
| the shipped wall | 10.2-10.7 | 10.4-10.9 | median |
| the target | 8.33 | 8.33 | 120 fps |

`wall ~ max(pump, guest floor, GPU)`. The guest floor is now **7.0 ms — UNDER the
8.33 ms target for the first time** — so the guest no longer forbids 120 fps on this
machine. **The pump does**: it is 10.1-10.5 ms and it is bound by bytes (part 111,
gotcha 551), and item B's ceiling — the whole per-draw renderer moved off the pump
thread — was measured at `F + M/3 = 5.33 ms` (part 110). So 120 fps CPU-side is
reachable in principle **only** by the renderer-side work parts 110-112 priced and the
operator declined at ~113 fps; with the guest floor at 7.0 the same build would now stop
at `max(5.33, 7.0, GPU)` = **~7.0-7.3 ms = ~137 fps CPU-side**, not 113. That is the one
number this part changes in the part-111 decision. The largest remaining term is the pump.

**What the guest floor is made of**, for the first time (§4.1): the Main Thread is half
simulation (`cZombieManager::Update` 14.9%, `cAIManager::Update` 11.8%, Havok 10.9%) and
half render submission (`cLevel::Render` 36%, `cTransModel::Render` 15%); the Draw Thread
is 81% one function — the title's own 0x49-opcode display-list interpreter
(`sub_827D5B18`) playing that submission into its D3D layer. Both are 100% recompiled
code, flat (top self symbol 4.8%), and insensitive to codegen (PGO −0.25 ms on the Main
Thread, killed; LTO and -O3 in §4.3). The CRT class is one memcpy at 0.9%. **There is no
guest-side item left above 0.3 ms except the structure itself** — the Draw Thread's
interpreter is the title's D3D submission and the only thing that removes it is the
D3D-translation pivot (`docs/d3d-translation-plan.md`), which also removes the pump's
PM4 walk. That is the architectural answer to both floors and it is a phase, not a part.

## §1. What shipped (all ON by default unless stated; every one has its control arm)

1. **The guest's threads are named** (`Main Thread`, `Draw Thread`, `JobThread0..5`, ...,
   ours `cz-*`). The title names them from the main thread by id; the binding goes
   through a guest-tid registry filled at spawn. `perf`, `top -H`, the census all read it.
2. **`guest main N.NN draw N.NN ms/frame` on every `[fps]` line** and a `[guestwait]`
   line after it (per-thread wait time by kind). Free: one clock read a window per thread.
3. **Wait-any wakes on the signal** (`WaitAnyBlock`, per-object registration), not on a
   1 ms tick. **Guest floor −1.11 ms, monotone, three runs a side, CPU unmoved; the shipped
   wall +0.24 ms** (§4.4 — the Draw Thread reaches the fence sooner and the pump pays for
   the contention). `CZ_WAITANY_POLL=1` is the control. The +0.24 is stated, not hidden:
   if the operator would rather have 0.24 ms on today's frame than 1.1 ms on the floor
   that decides the target, the default is one line.
4. **Codegen arms in CMake** (`CZ_PPC_OPT`, `CZ_PPC_PGO`, `CZ_PPC_LTO`), all OFF; the PGO
   profile flushes from the SIGTERM handler.
5. Tools: `part116_probe.sh` (archives the binary + a symfs — perf's build-id cache does
   NOT make an old capture readable), `part116_callers.py`, `part116_guestcpu.py`,
   `part116_ab.sh`, `func_strings.py`; `part50_thread_cpu.py` prints names;
   `part53_symbols.py` parses comms with spaces.

## §2. What is owed

* **An operator session on the shipped default** — nothing here changes a pixel and the
  picture gates are in §3, but the wait-any wake changes WHEN guest threads run, and the
  only test of "does it feel the same" is theirs. If anything is off, `CZ_WAITANY_POLL=1`
  first.
* **The +0.24 ms on the normal arm has a candidate mechanism, not a proven one** (§4.4).
  One run with `CZ_PM4_TICK_US` raised, or a `perf` pair on the pump under both arms,
  would name it. Not done: the night's budget went to the three codegen arms the plan
  ordered.
* Windows: `pthread_setname_np` / `pthread_getcpuclockid` are Linux; the Windows build
  compiles (the calls are guarded) but prints `guest main -1.00` and no `[guestwait]`.
  `SetThreadDescription` + `GetThreadTimes` is a 20-line port when someone is on czwin.

## §3. Gates (run on the shipped binary at the end of the night — see §4.6 of the plan)
