# Part 118 kick-off — the Main Thread: what it is bound by, and each pipeline stage on its own core

**READ `docs/perf-plan-part118.md` FIRST — §1 (the PMU pair), §2 (the wait census by
caller), §4 (placement) and §5 (the honest answer).** This file says where the port is
after the operator's order of 2026-09-12 12:30 (*"reduce the main thread ms ... by
splitting some part you can on other core"*, deadline 16:00), what shipped, and what is
owed.

## §0. Where the frame is (the operator's crowd, 1920x1080, Ryzen 7 5700)

| term | part 117 close | part 118 close | how |
|---|---|---|---|
| the guest's Main Thread | 8.1-8.4 ms CPU | **7.9-8.0** (−0.35 monotone) | `guest main` on the [fps] line, matched bands |
| the guest's Draw Thread | 6.2-6.5 CPU | **5.7-5.9** (−0.58) | `guest draw` |
| the renderer thread `cz-draw` | 8.6-9.1 CPU | **8.4-8.8** (−0.52) | `pump cpu` |
| p99 | 12.1 | **11.0** | [fps] line |
| the wall | 9.0-9.1 | reads 9.0x OR 10.0x — **quantised at 1 ms by the vblank** (gotcha 572) | median |

**The frame is a three-stage pipeline whose stages are within 5% of each other** — the
Main Thread (7.9 CPU + ~1.3 waits, one of them a whole-frame wait on the Draw Thread
through `sub_827CC6A8`), the Draw Thread (5.8 CPU + ~3 on our fence), `cz-draw` (8.5-8.8
CPU, 95% of a core). No single stage's saving moves the wall; the placement default moves
all three.

## §1. What shipped

1. **Thread placement (`runtime/cpu/thread_budget.cpp`, `CZ_GUEST_PIN`) — ON by default
   from eight physical cores with SMT, mode 2**: the Main Thread, the Draw Thread,
   `cz-pump` and `cz-draw` each on a physical core of its own (the four highest in the
   mask; cpu 0 avoided), SMT sibling kept empty, every other thread confined to the rest;
   a `/proc/self/task` sweep after each pin and per [fps] window catches inherited masks.
   `CZ_GUEST_PIN=0` is the control. Measured three runs an arm, matched bands: `cz-draw`
   −0.52, Main −0.35, Draw −0.58 ms/frame, all monotone; p99 12.0 -> 11.0. Linux only.
2. **`CZ_HAVOK_WORKERS=N` (`runtime/cpu/havok_threads.cpp`)** — the title's Havok pool
   size, hard-coded 2 for the 360, hooked at its two constructors. **Stock by default:
   4 measured Main CPU −0.21 and Main waits +0.5, wall +0.46 — killed** (gotcha 573).
3. **`CZ_WAIT_CALLERS=1`** — the [guestwait] census keyed by guest tid + caller + two
   frames of the back chain. It named the Main Thread's waits (plan §2).
4. **`CZ_VK_GUARD_NTA=1`** — prefetchnta ahead of the guard's fold. A null on the Main
   Thread; kept for a PMU pass.
5. `tools/part118_guestprobe.sh` (PMU on the guest's Main Thread + the cpu-placement
   census), `tools/part118_campaign.sh`, `tools/part118_campaign2.sh`.

## §1b. The Windows evening (plan §4d, §4e)

* **The pin on Windows shipped**: czwin's hybrid 12700H −38% on the wall (E-cores).
* **`timeBeginPeriod(1)` shipped**: czamd read a flat 46.8 ms a frame (3 x 15.6) with
  nobody holding the Windows timer; **21 -> 77 fps at the crowd from one call.** The
  Windows release leg has shipped without it since v1.0.0. `CZ_NO_TIMER_PERIOD=1` is the
  control. **This goes in the next Windows release before anything else.**
* The six-core shape: mode 1 on czamd is a small win (p99 −2 ms), default still OFF there.

## §2. What is owed

* ~~**Windows**: no-ops there~~ — **DONE the same evening (plan §4d): on czwin's
  12700H the pin reads −38% on the wall (22-24 -> 13-15 ms at the crowd, 42 -> 70 fps),
  one 3.5-minute run a side.** Owed there: the Windows `guest main`/`draw` columns read
  ~0 (GetThreadTimes on the registered handle — check the handle's access and the
  Main Thread's registration), and czamd's six-core shape (deployed, unmeasured).
* ~~**The operator's session on the default**~~ — **DONE, 15:10-15:40, two sittings on
  `tools/play_session.sh` at their own settings: *"feels smoother, above 100 fps almost
  all the time."*** Their log (`~/DR2CZ-troubleshooting/play/part118_play{1,2}.log`): at
  their crowd, 8,900-9,100 draws, 101-111 fps with the Main Thread at 7.9-8.4 and the Draw
  Thread at 5.3-5.7 ms CPU, p99 11-13 ms; 117-128 fps at 6,800-8,100 draws. No sound
  glitch, no load stutter reported. The verdict the instruments could not give.
* **The six-core and no-SMT shapes** (the default is OFF there, unmeasured).
* **A finer wall**: the vblank quantum hides any change under 1 ms on the wall column
  (gotcha 572). `CZ_VBLANK_MS` cannot go under 1 ms; the mean of the [fps] line is
  finer than its median; the CPU columns are the instrument. And PRINT THE CLOCK once a
  window — today's runs drifted ~6% ninety minutes in (`amd-pstate-epp`,
  `balance_performance`).
* ~~Havok at 3 workers was not measured~~ — one run at 3440x1440 (plan §4c): the same
  tail-wait shape as 4 (+2 single waits a frame), Main CPU no lower. Stock stays.
* The Main Thread's remaining 7.9 ms is the title's own pointer-chasing (31k demand
  DRAM fills a frame, flat); the next lever is architectural (the D3D pivot removes the
  Draw Thread's interpreter half and the PM4 walk, not the Main Thread's simulation).

## §3. Gates on the shipped binary (`ARM=CZ_GUEST_PIN=2 tools/part117_gates.sh`, 14:18-14:49)

`--smoke` OK · unlowered switches 0 · shader dims clean · both PM4 oracles clean · E3
**+0.8415** (4 of 5 agreeing on layout, pinned 1280x720; part 117 read +0.8411) · A5
**exit 0** (5 permutation windows, 0 real — identical to parts 116 and 117) · `no
translated shader` 0 · **synchronization validation 0 hazards** at 5,154 draws on the
outdoor route, **the poison producing 30** · `truncated=0` across every crowd log of the
part · a 10-minute `CZ_AUTOCHUCK=EXPLORER` soak with `CZ_WAIT_TRACE=1` under the
placement default: no fault, no corrupt stream, the map closed twice, **143 fps median at
5,000-5,600 draws** (part 117's soak read 143 at 2,400-3,200). The default's own policy
was checked under `taskset` masks of six and four cores: OFF on both, as designed.
