# Part 50 kickoff — CPU performance, with the frame rate finally readable

Written at the close of part 49 (2026-08-16). **This is the LIVE hand-off**,
superseding `part49-kickoff.md`.

## START HERE

**The plan is `docs/perf-plan-part50.md`.** It is built on the operator's own
whole-map lap at 60 fps — 16,788 frames — and every number in it is from their
machine. Read it, then §6cf of `docs/phase5-notes.md`.

**The operator's instruction is explicit and current**: *"prepare a whole plan to fix
CPU performance issue and we'll start it in a fresh conversation."* Performance is
still the subject. Their two deferred picture items (00m decals, 00n a sign and items
at distance) remain deferred and were re-confirmed as pre-existing in part 48.

**There is no action zero this time.** Part 49 already ran the operator session that
would have been it, and its result is the plan's budget. Start on tier 1.

## What changed in part 49, and it changes how you MEASURE

* **`CZ_FPS_CAP=60` is real, verified, and the operator has played the whole map on
  it.** 62.5 fps below 3,000 draws, 43.5 at 3-5k, 35.7 at 5-7k, only 3.6% of frames
  below 30 fps. Their words: *"seems to be working pretty well"*, and earlier, on the
  speed question that could have invalidated everything, *"the game plays perfectly"*.
* **"Ordinary gameplay is 31 fps and CLOSED" is RETRACTED.** It was true of the shipped
  configuration and false of the title. The 30 fps is the title's own D3D present
  interval, and a 60 fps mode is a configuration it already ships with.
* **FRAME TIME IS A USABLE INSTRUMENT AGAIN**, for the first time since part 30. The
  32 ms floor that made every CPU saving measure as exactly zero (gotchas 237/238) is
  gone. Quote ms and fps directly; the pinned-share statistic was a workaround.
* **THE GPU IS IDLE — `submit.gpu` 0.0% median over 22 windows.** This is 100% a CPU
  problem and there is no GPU item in the plan.
* **At 60 fps every per-FRAME cost is paid twice as often per second** — the guest's
  simulation, the PM4 walk, the readback. Anything per-frame is worth double what the
  part-48 budget said.

## The budget, and where to start

At 5,000-7,000 draws (**60% of their play**), 28.3 ms:

| term | ms | the item |
|---|---|---|
| `outside` | **11.3** | the PM4 walk, **81,106 packets/frame at 100 ns = 8.1 ms**, plus ~3 ms of guest simulation |
| `record` | **7.4** | **`vertex` 662 ns/draw** — the stream guard hashes **63-72 MB every frame** inside it |
| `other` | 4.4 | `residual` 206 ns/draw, still unnamed after TWO splits |
| `textures` | 3.1 | closed in part 47 |

**Tier 1 first, and item 1a is the cheapest thing in the document**: 28.7% of every
packet walked is type-2 ring FILLER doing no work at all — 23,000 full `ExecutePacket`
calls a frame for one-dword no-ops. Skip them as runs.

**The largest single item is the stream guard's BYTES** (plan §2a). Part 47 made that
hash four times faster and **did not make it smaller**, and the operator's own session
priced the fold at 6.9 ms — so this region is known to be worth milliseconds on their
machine.

## What part 49 settled (do not re-derive)

* The 30 fps chain, traced end to end: config `0x82A57ACC` -> `sub_823C8D20` ->
  `sub_827CBB00` -> `dev+13804` -> `sub_82841AD0` -> `sub_82841878` -> `sub_82841760`.
  `dev+13804` of 0 or 1 = interval 1, 2 = interval 2, 4 = interval 3.
* **The cap works by shortening the VBLANK PERIOD, not the interval.** Presents are
  vblank-quantised, so at 16 ms the ladder is 16/32/48 with nothing between and any
  frame over 16 ms falls to 31 fps. `CZ_FPS_CAP=60` sets an 8 ms period and pins the
  title's own interval of 2.
* **It does not double the simulation speed**: locomotion p90 **0.99x** against a
  registered 2.00x prediction, plus the operator's verdict in play.
* **A shorter vblank period costs NOTHING in CPU** — 6.1 vs 6.3 ms of `outside` over a
  four-way sweep at matched draws. An intermediate claim that it cost ~5 ms was
  retracted; it came from comparing two runs taken at different times.
* **Host vsync is now explicitly OFF.** The renderer was created without the vsync flag
  but never told SDL *not* to vsync, and a compositor throttles regardless. It hid for
  48 parts behind the guest's own 30 fps cap (gotcha 332).

## Standing state

* **Runtime defaults changed in part 49**: host presentation vsync is OFF
  (`CZ_HOST_VSYNC=1` restores it), and **the frame rate cap is 60 fps** —
  `CZ_FPS_CAP=30` is the control arm and restores the shipped pacing exactly.
  The operator asked for 60 as the default after playing the whole map on it; a
  PLAYER-FACING option to choose is later, separate work and is not built.
  **Every measurement in this plan was taken at 60 fps**, so a number quoted from
  part 48 or earlier was taken at 30 and is not comparable without saying so.
* **New arms**: `CZ_FPS_CAP=N`, `CZ_PRESENT_INTERVAL=1|2|3`, `CZ_HOST_VSYNC=1`, and
  from part 48 `CZ_PM4_ATOMIC_COUNTERS=1`, `CZ_PM4_VERIFY_COUNTERS[_POISON]=1`.
* **Tooling**: `part48_campaign.sh` (many arms, one pinned binary, one shared
  baseline), `part48_walk_read.py` (ns/packet + a mix admissibility check),
  `part48_draw_read.py` (ns/draw, narrow band, `--null` required),
  `part49_launch60.sh` (a guarded single launch), `part47_gates.sh`.
* **Artifacts**: `~/DR2CZ-troubleshooting/part48/campaign/`, `part48-operator/`,
  `part49-operator/` (the whole-map lap is `cap60c.stats`, 16,788 frames).
* **Gates at close**: all clean at the 60 fps default, E3 **best of five +0.8807**,
  4 of 5 samples agreeing on layout. The gate now takes FIVE captures because part 49
  found it could fail by luck: the backdrop is animated and correlation swings ~0.23
  within one configuration, with one 30 fps sample reading 0.649 against the gate's own
  +0.70 threshold (gotcha 133 sitting inside a standing gate).
* **Two process lessons that each cost real time**: `pgrep -x cz_runtime` cannot see
  `cz_runtime_envperpacket` because Linux truncates `comm` to 15 characters — match on
  a PREFIX; and **launch through a guarded script**, because two instances at once
  measures contention and it happened twice in two parts, the second time because a
  guard that existed was bypassed "just this once".
