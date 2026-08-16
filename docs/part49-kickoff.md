# Part 49 kickoff — the performance target is MET; ask the operator what comes next

Written at the close of part 48 (2026-08-16). **This is the LIVE hand-off**,
superseding `part48-kickoff.md`.

## START HERE — and the first action is a QUESTION, not a measurement

**Part 48 met the target the last three parts were aimed at.** The operator's own
frame, at the gas-station spot they name as worst, soaked with a stationary camera:

| | part 46 | part 47 | **part 48** |
|---|---|---|---|
| frame at ~7,000 draws | 61.7 ms | 42.8 ms | **33.6 ms** |
| fps | 16.2 | 23.4 | **29.8** |

`docs/perf-plan-part48.md` opens with *"the Xbox 360 shipped this game at 30 fps,
i.e. a 33 ms frame"*. That is now true at the worst place they know.

**So the standing instruction — *"For now performance is the most important"* — has
been satisfied, and it should not be assumed to still hold.** Ask them. The two
things they have explicitly deferred are picture items they filed themselves:

* **00m — decals**, new in part 47, never characterised.
* **00n — a sign and some items wrong at distance**, the tail of 00i.

Both were re-confirmed as still present in part 48's session (*"Some looked wrong but
they are not new"*), and both are PRE-EXISTING rather than caused by any performance
work. If they want the picture next, those two are the queue. If they want more
speed, §"What is left in the frame" below is ranked and every number in it is from
their own machine.

## What part 48 established

* **Both of its wins were found by SPLITTING A PROFILER PHASE, and neither by reading
  code** — which is now three items in two parts (gotcha 327). Splitting `other`
  found a `getenv` on the per-draw path within minutes of first printing; applying the
  gotcha written for that to `pm4.cpp` found a `getenv` **on the PM4 walk, once per
  type-3 packet, ~29,000 times a frame**, worth **4.5 ms** on the operator's frame.
* **`Pm4_OpcodeCount` had been incremented on every packet since phase 4 and read by
  nothing.** Printing it (item 1a) showed **`SET_BIN_MASK_LO` is the most frequent
  packet in the stream** — a third of all type-3 packets, half again as many as there
  are draws — and that **28.7% of packets are type-2 ring filler** doing no work.
* **Part 47's four-lane guard fold is worth 6.9 ms on their machine**, against ~6.8
  predicted. It halves `rec.vertex`.
* **An item can be perfect on its own counter and a net loss** (gotcha 330). Item 2b
  gave the stream cache a generation stamp: 97.7% node reuse, 45x fewer allocations,
  and `record` 8.5% slower, because 22,000 lookups a frame paid for 1,800 cheaper
  inserts. Built, measured, reverted the same day.
* **A NULL-CONTROL ARM is what makes any of these numbers believable** (gotcha 331).
  The 3,000-8,000 draw band is NOT narrow enough for a per-draw statistic — `record`
  varies 1,204 → 1,033 ns/draw across it. Put an arm in every campaign that *cannot*
  move the statistic you care about; whatever it reads is the floor. On the operator's
  session it read +1.5-4.9% while the fold read +99.4%.
* **Item 1b (per-thread PM4 census counters) is real but small**: ~3 ns/packet against
  a predicted 20-40. Consistent 3/3, verified 0-of-135 against the incumbent with a
  poison arm. Kept.
* **Item 2d (the vertex/index bind cache) is NOT the loss part 47 feared**: +0.8% on
  `record` against a ±2% floor, inconsistent across runs. Keep it. The headless route
  cannot separate it and it is not worth an arm of the operator's time.

## What is left in the frame, ranked on THEIR numbers

At 33.6 ms and ~7,000 draws:

| term | ms | the item inside it |
|---|---|---|
| PM4 walk | ~9.5 | ~100,000 packets at 95 ns. `SET_BIN_MASK_LO` is 12,000/frame of them |
| `record` | ~9.5 | **`rec.vertex` is 5.4 ms of it** |
| `other` | ~5.0 | `residual` 205 + `pipeline` 122 + `begin` 107 + `fetch` 112 ns/draw |
| `textures` | ~3.6 | closed in part 47, staying closed |

Three leads, all created by part 48 and none of them costed:

1. **The cross-frame store's content guard still reads 63-72 MB EVERY FRAME**, inside
   `rec.vertex`. Part 47 made that hash four times faster; it did not make it
   **smaller**, and the fold arm proves the hash is most of the section. The question
   nobody has asked is why 3,100 first-touch streams a frame need 72 MB hashed at all —
   i.e. attack the BYTES, not the rate. This is the largest single item left.
2. **`oth.begin` is 107 ns/draw ≈ 0.75 ms a frame** for `BeginFrame` + `BeginRendering`,
   which are supposed to be once-per-frame work. Either the guard is expensive or they
   are doing real work per draw; nothing has ever measured it.
3. **`oth.residual` is 205 ns/draw ≈ 1.4 ms and is still unnamed after TWO splits.**
   Split it again — that is what has worked three times.

## Standing state

* **Runtime defaults changed in part 48**: the PM4 census counters are per-thread; the
  PM4 walk's and the renderer's per-draw `getenv`s are hoisted. Arms:
  `CZ_PM4_ATOMIC_COUNTERS=1`, `CZ_PM4_ENV_PER_PACKET=1`. Item 2b was reverted, so
  `CZ_VK_STREAM_CACHE_CLEAR` no longer exists (`docs/instruments.md` keeps it struck
  through as a named dead end).
* **New instruments, all always-on under `CZ_VK_PROFILE`**: the PM4 opcode/type census,
  the `other` split (7 terms), and the census verifier `CZ_PM4_VERIFY_COUNTERS` with
  its poison arm.
* **Tooling**: `part48_campaign.sh` (many arms, one pinned binary, one shared
  baseline), `part48_perf_ab.sh` (the pinned-binary A/B), `part48_walk_read.py`
  (ns/packet, with a packet-mix admissibility check), `part48_draw_read.py` (ns/draw,
  narrow band, **`--null` arm required**), `part48_operator_session.sh` (three arms,
  and it now REFUSES to start if another run is alive).
* **Artifacts**: `~/DR2CZ-troubleshooting/part48/campaign/` (12 headless runs, four
  arms) and `~/DR2CZ-troubleshooting/part48-operator/` (their three arms, with the
  gas-station soak).
* **Gates at close**: `tools/part47_gates.sh` — all clean, E3 at **+0.877, LAYOUT
  AGREES**. Run before handing the operator anything.
* **A process lesson that cost the operator a session**: `pgrep -x cz_runtime` cannot
  find a leftover named `cz_runtime_envperpacket`, because Linux truncates `comm` to 15
  characters. They noticed by HEARING it. Kill a driver's PROCESS GROUP, not its game.
