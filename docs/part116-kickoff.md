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
| GPU | 3.23 ms at 1080p (one `CZ_VK_GPU_PASSES=1` run) | unchanged | plan §4.0 |
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
   the contention). **THE OPERATOR DECIDED (morning of 2026-09-12, after seeing both
   numbers): OFF by default, `CZ_WAITANY_WAKE=1` engages, and do not go after it again.**
   Flip the default when the pump is under ~8 ms (item B / the D3D pivot); at that point
   the frame is the game's floor and the fix is 8.1 -> 7.0 ms ≈ 17 fps.
4. **Codegen arms in CMake** (`CZ_PPC_OPT`, `CZ_PPC_PGO`, `CZ_PPC_LTO`), all OFF; the PGO
   profile flushes from the SIGTERM handler.
5. Tools: `part116_probe.sh` (archives the binary + a symfs — perf's build-id cache does
   NOT make an old capture readable), `part116_callers.py`, `part116_guestcpu.py`,
   `part116_ab.sh`, `func_strings.py`; `part50_thread_cpu.py` prints names;
   `part53_symbols.py` parses comms with spaces.

## §2. What is owed

* **An operator session on the shipped default** — nothing here changes a pixel and the
  picture gates are in §3, but the wait-any wake changes WHEN guest threads run, and the
  only test of "does it feel the same" is theirs — now moot for the default (the poll
  is what shipped in every release so far); it applies the day `CZ_WAITANY_WAKE` becomes
  the default. Headless evidence so far: 30+ crowd-route runs and one 10-minute
  `CZ_AUTOCHUCK=EXPLORER` roam with `CZ_WAIT_TRACE=1` (outdoors at 8,689 draws, the map
  closed twice, no thread ended, the only >5 s waits the idle JobThreads' — the same
  idle waits the poll arm reports).
* **The +0.24 ms on the normal arm has a candidate mechanism, not a proven one** (§4.4,
  §4.8). A `perf` pair on the pump shows its user-space table unchanged and `[unknown]`
  (kernel) +1 point; naming it needs kernel samples (`perf_event_paranoid` < 2, i.e. the
  operator's sudo) — `perf record -g` on the pump under both arms with kernel symbols is
  the one-run answer.
* Windows: `pthread_setname_np` / `pthread_getcpuclockid` are Linux; every new call is
  behind `!defined(_WIN32)` so the Windows build SHOULD compile (prints `guest main
  -1.00`, no `[guestwait]`; the wait-any wake itself is portable std::), **but czwin was
  unreachable at 05:20 (ssh timed out) so it is NOT verified** — the first Windows build
  after this part is a gate, not a formality. `SetThreadDescription` + `GetThreadTimes`
  is the 20-line port of the two instruments.
* **A history rewrite happened tonight, before the push**: the first commit's `git add
  -A runtime/` swept two untracked release build trees (~600 MB, two 150 MB archives)
  into 8308d11; GitHub's pre-receive rejected the push; the eight local commits were
  rewritten with `git filter-repo` (hashes changed) and `.gitignore` now covers
  `runtime/build-*/`. Nothing of the operator's history was touched (origin was at
  59a03b5 throughout).

## §3. Gates on the shipped binary (plan §4.6)

`--smoke` OK · unlowered switches 0 · shader dims clean · both PM4 oracles clean ·
E3 **+0.8472** (4 of 5 agreeing, pinned) · A5 **exit 0** (5 permutation, 0 real) ·
`no translated shader` 0 · `truncated=0` · `phase_vs_perf.py --self-test` PASSED.

## §4. Item 5 — the bundle, re-measured: −0.68 ms on the shipped frame

`CZ_VK_SCOPED_SHARED_ZERO=1 CZ_VK_TEXMEMO=1` on today's baseline: **wall −0.68, pump −0.68,
monotone in four bands, three runs a side** (plan §4.5). Above the 0.4 ms bar that kept
them off. Still the operator's call, as ordered; the recommendation is ON — it is the
only verified, gated, unshipped renderer saving, and it is larger than the wake's +0.24
on the same frame.

## §5. Rules this part paid for

* **A capture's binary must be archived AND mapped**: perf's build-id cache does not make
  a capture readable after its binary is overwritten (it reads the recorded path, sees
  the mismatch, prints addresses). `<tag>.symfs` does. Gotcha 550's mechanism.
* **A thundering herd measures as a slower NEIGHBOUR**: v1's broadcast cost the pump 0.8
  ms on another core. Wake the waiter, not the world.
* **`pgrep -f "<script>"` matches the shell that runs it** — twice tonight, exit 144 —
  bracket the first character (`"[p]art116_ab.sh"`). And never rebuild `runtime/build`
  during a campaign; one run raced the link and was quarantined.
* **Three items killed BEFORE building by census** (memcpy's 0.095 ms ceiling) or after
  one clean build each (PGO, LTO, -O3) — the night's cost was ~100 s of compile per arm and
  54 min of runs; a plan that pre-registers the kill can afford to try.
