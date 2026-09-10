# Part 111 kick-off — where part 110 left it

**READ `docs/perf-plan-part110.md` §7 FIRST.** It is four pages and it is the whole state.
This file says only what to do next and what not to re-derive.

## §0. THE DECISION IS MADE: BUILD IT. `docs/perf-plan-part111.md` IS THE PLAN.

The operator was shown the ceiling — item B tops out at **~113 fps** and cannot reach the
120 they asked for — and said build it anyway (2026-09-10). So the ceiling is accepted, not
overlooked, and the work is to capture as much of the ~2.3 ms as survives the code.

**Read `docs/perf-plan-part111.md` before anything else.** Its §1 is the finding that
reorders part 110's sketch (the constants cannot be deferred, because their source is the
register file the pump itself overwrites — the VS window changes on 98.3% of draws), and
its §0 carries the budget that decides when to stop: **item B has ~2.1 ms of USEFUL
headroom, not 5.6**, because the wall stops responding once the pump reaches the guest's
8.8 ms floor.

Order of work: **§3's census, then B1 (pre-zero the arena — hazard-free, and its kill rule
refutes the whole design for one day's work), then B2 (streams and textures, where the
money is), then stop at 8.8 ms.** B3 (the constants) is expected to be refuted by the
census and never built.

The two verified sub-threshold items are independent of all of this and still shipped OFF:
`CZ_VK_TEXMEMO=1` (−0.33 ms) and `CZ_VK_SCOPED_SHARED_ZERO=1` (−0.21 ms). **Note that the
second one is an ALTERNATIVE to B1, not an addition** — see the plan's §4.1.

## §0b. The original decision framing, kept because the numbers in it are the case


Part 110 answered both of their asks. One of the answers is a decision they have to make
before any code is written.

**Should item B — moving the per-draw renderer work to the four idle cores — be built?**

|  | pump cpu | wall | fps |
|---|---|---|---|
| today, at the operator's crowd | 10.93 ms | 11.07 ms | 90 |
| item B, implemented perfectly | 5.33 ms | **~8.8 ms** | **~113** |
| the target they set in part 109 | — | 8.33 ms | 120 |

* **It is GO by the plan's own pre-registered rule.** `F` (the serial floor) = 2.49 ms,
  `M` (the movable per-draw work) = 8.44 ms, `F + M/3` = **5.33 ms** against a kill of
  8.0. Measured, six alternated runs, 102 crowd windows an arm, monotone in all six
  matched bands, and cross-checked against part 109's independent `perf` symbol reading
  of the walk (2.36 ms).
* **It cannot reach 120 fps.** With the whole per-draw renderer deleted and the GPU idle,
  the frame is still 8.8 ms. That is the title's own recompiled code on two threads
  (76.7% and 61.7% of a core), and it is not our hand-off latency: cutting the pump tick
  from 100 µs to 25 and to 10 moves it by 0.01 ms.
* **So the trade is: a substantial threading build for ~2.3 ms and ~+23 fps, stopping
  short of the goal.** For scale, 2.3 ms is four times everything part 109's whole night
  found (0.54 ms). The build is §3.3's design, §3.4's three stages, ThreadSanitizer or a
  per-structure race argument, and a thread budget with no spare slots.

**If YES:** §3.2's mutation census runs first, unchanged. It can refine how much of the
2.3 ms survives sharding; it cannot move the 8.8 ms ceiling, because that ceiling is the
other term of a `max()`.

**If NO:** two verified items are still sitting OFF —`CZ_VK_TEXMEMO=1` (−0.33 ms) and
`CZ_VK_SCOPED_SHARED_ZERO=1` (−0.21 ms), both correct, both gated, both shipped off only
because they missed a bar set when the plan still expected 1.5 ms items. Flipping either
default is one line.

## §1. The new subject on the board, and nothing in this project has ever looked at it

**The 8.8 ms floor is the GUEST's own recompiled code and it is now the largest single
term in the frame.** Two threads, diffuse `__imp__sub_*`, no hotspot on the busier one
(76.7% of a core = 6.75 ms/frame); the second is 61.7% with `__imp__sub_827D5B18` at 14.7%
of it. Neither is saturated, so the 8.8 ms is a **dependency chain**, not a throughput
limit — 2.05 ms of it is the gap between the busiest thread and the wall.

Every performance part this project has run has been about the renderer or the command
processor. This is a different subject and it has never been decomposed. It is also the
one that decides whether 120 fps is reachable at all, on any hardware.

Cheap first questions, in order:
1. What are those two threads? The guest names its threads through an exception channel we
   log (`JobThread0..5`, `cAsyncFileSystem`, ...) — bind the host TID to the guest name.
2. Is the 2.05 ms gap a hand-off between the two guest threads, or between them and us?
   `CZ_WAIT_TRACE`, and the fence-wait counters part 107 built, already exist.
3. Does the floor scale with the crowd? If it is the zombie simulation, it is a Case West
   finding too.

## §2. What is in the tree that was not before

* **`tools/phase_vs_perf.py`** — the standing cross-check. **Run it once a part**, beside
  `--smoke` / A5 / `truncated=0`. `--self-test` is its positive control and must keep
  flagging `streams`. Prefer a `perf` capture from a **clean** run at matched draws over
  one from the profiled run itself: the profiler's 4-6 ms lands in `DoDraw` and dilutes
  every symbol share (`pm4` reads 0.93x clean, 0.56x same-run).
* **`CZ_VK_NO_DODRAW=1`** — the serial-floor ceiling arm. Destructive, announced, counted
  per FPS window. **Read the pump's CPU per frame and never the wall.**
* **The `[fps]` line carries `pump cpu N.NN ms/frame (M% of a core)`** in every run,
  profiled or not — one `clock_gettime` per window. `tools/part110_pumpcpu.py` bands it.
* **`CZ_VK_PROFILE`'s COVERAGE line** — read its warning, not only its number: coverage is
  ~100% and the table still misattributes (gotcha 547).
* **`-DCZ_WHOLEFUNC=1`** — A.2's sampled whole-function timers. A build carrying them is
  ~0.5 ms/frame slower; read its shares, never its milliseconds. Default builds carry no
  code (their `.text` is byte-identical to the binary before the probe existed).

## §3. Two rules part 110 paid for

* **Archive the executable beside every `perf` capture.** Three of part 109's four are
  unreadable today (gotcha 550).
* **Do not rebuild while a campaign is running.** `tools/part80_crowdroute.sh` copies
  `cz_runtime` at the start of each run, so a mid-campaign build silently changes the arm.
  Part 110 did this once and caught it only because the arm prints an engagement banner.

## §4. Owed, unchanged from part 108

The Windows EYE tests (`windows-test-list` items 2-10) still need a `cz_play` session.
