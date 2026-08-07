# D3D phase C, part 9 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. `docs/d3d-translation-plan.md` carries the phase C
strategy and, at its end, the **"Phase C part 8"** section — read that first; it is the
record of the session this file hands off from. `docs/d3d-phase-c8-kickoff.md` is the
previous hand-off: **all four of its items are answered**, and for the first time since
the draw arm was built in session 13 there is no known blocker in front of the phase C
completion criterion.

## What part 8 changed, in one paragraph

The replay had an EXIT all along and phase C was losing a race to it. `sub_8284B9C0`
(already in the hook table as "the frame-end async submit") clears the async marker
`dev+0x3460`, **resets all seven worker token streams** (`sub_82845290` x7) and re-arms
with the freshly emptied head — and `Resolve` calls it from its COMMON TAIL, so every
Resolve throws away every segment descriptor a previous frame left behind. On the PM4
control arm the thousands of dwords of tile content in front of the arm block keep the
command processor from reaching its `INTERRUPT` before that reset lands; phase C
redirects that content away, the segment is 93 dwords of pure protocol, and the CP gets
there first. The fix zeroes `dev+0x3460` across the guest's own close/kick — guarded on
`dev+0x2B04 == 0`, the same test `sub_82845AC0`'s own fork makes, and restored
afterwards — so the arm-carrying segment goes RING DIRECT instead of into the stream
that arm just handed the ISR. `CZ_D3D_NO_ARM_SEG_DIRECT=1` is the pre-fix arm.

## The state you inherit, measured

Same binary, arms ALTERNATED within each round, 120 s each, 5 runs per arm plus 3 PM4
controls — ranges over runs, not one run (gotchas 50/51/159):

| | pre-fix | **fixed** | PM4 control |
|---|---|---|---|
| `ints/arms` (CP executions per arm block) | 53.8 - 64.3 | **1.000, 5 of 5** | 0.998 - 1.000 |
| `distinct` token buffers | 2 - 6 | 2 - 719 | 300 - 504 |
| `resolve`=`reseed` | 2 - 6, frozen at 7 s | 2 - 2,381 | 640 - 2,274 |
| `dev+0x2B04` | -3,555 to -4,167 | **0 or 1** | 0 - 2 |
| deepest file | #60, **5 of 5** | **#83, 4 of 5** | #83, 3 of 3 |
| A1 gate | 82-prefix, never better | **exact 84-prefix** | exact 84-prefix |

`truncated=0`, `max` hold streak 1-2 and **zero crashes in all thirteen runs**. The A1
84-prefix on the draw arm is the port's best draw-arm gate result and matches the
control arm exactly; A1's long-known position-71 window permutes 2 of 5 on the fix arm
against 3 of 5 pre-fix, i.e. no different between them.

## The picture, which part 8 ran and which is now at PARITY, not at correctness

Both arms were run headless with `CZ_VK_FRAME_DUMP` and both reach the title screen.
The phase C frame renders **PRESS START and the CAPCOM copyright line** — the first
title screen this port's D3D translation layer has produced. Against the PM4 arm's own
title-screen frame as the reference, `tools/frame_signature.py` reports **identity,
correlation +0.893, runner-up beaten by 0.914**, so the arms agree on layout and both of
that tool's negative-control gates pass (gotcha 137).

Neither arm matches capture E2 (+0.06 draw, +0.10 PM4). **The 3D background and the
DEAD RISING 2 wordmark are black on BOTH arms**, so that is a phase-5 renderer gap the
pivot neither caused nor fixed, and it is now the top open item. `docs/phase5-notes.md`
§7 is the enumerated gap list.

## Where part 9 starts, in order

1. **The missing scene, on the arm of your choice.** Because both arms are black in the
   same way, this can be worked on the PM4 control arm — which is faster, simpler and
   has every phase-5 instrument (`CZ_VK_SNAP_DUMP`, `CZ_VK_RESOLVE_TRACE`,
   `CZ_VK_SHADER_CENSUS`) — and the result checked on the draw arm afterwards. Read
   gotchas 127/129/130/133 before believing any single frame: the title screen is an
   ANIMATED 3D scene, one frame is one sample, and `frame_signature.py` is the only
   instrument in this project that can see a whole-frame TRANSFORM.
   ```
   python3 tools/frame_matched_diff.py --a pm4run1 pm4run2 --b d3drun1 d3drun2
   ```
2. **The second stall at the same era, now visible.** One draw-arm run in five still
   stops at #60 — with `ints/arms = 1.000`, `dev+0x2B04 = 0` and `distinct = 2`, i.e.
   with no replay at all. That is a different fault from part 8's, the loud one was
   hiding it, and the chain line tells the two apart at a glance.
3. **The walker's `case 0x54:` INTERRUPT block is DEAD on every arm.** Its first-16
   print (`[d3ddraw] INTERRUPT #N at position`) is unconditional and produces **zero**
   lines across six runs, pre-fix and post-fix alike — parts 2, 3 and 5 moved all four
   arm-block emitters to the real ring, so no INTERRUPT packet reaches the private
   scratch any more. Part 8 deliberately did NOT delete it: its own comment records
   that removing the ring-transport fallback once cost ~150 frames of the movie era,
   and the routing has changed twice since that measurement. Deleting it is a clean,
   reversible decision for a session that is not also changing the routing.
4. **`MirrorIsPoisoned()` is still inert**, zero skips on every arm, unchanged by the
   fix. Same disposition as above.
5. **The kernel gates are exhausted as a forward oracle.** A1's position 93 is not the
   next piece of work (finding 49, gotcha 107). Going further needs a gameplay
   comparison built from A2, which is a different tool, not a longer run.

## Traps this session paid for — do not re-buy them

* **A structural difference and a race look identical in a log.** "The arm block is in
  the walked stream" is true on BOTH arms; only the time it spends there differs. Every
  comparison taken after the draw arm froze reads as structural and is unfalsifiable.
  The number that settled it was taken at the first tick either arm runs a Resolve at
  all: 1.6 kicks per Resolve on the control arm, 16 on the draw arm.
* **A ratio needs both counters moving** (gotcha 161, again). `resolve` was added to the
  chain line purely so `reseed` has a denominator that cannot freeze independently — the
  two are equal to the unit on every tick of both arms, which is the log's own proof
  that Resolve's tail always reaches the reseed.
* **The exit was in code already in the hook table.** `sub_8284B9C0` had been hooked
  since phase C part 3 and described as "the only `+1` `dev+0x2B04` ever gets". Nobody
  had read past that to the seven `sub_82845290` calls above it. When a loop looks
  unbounded, read its participants end to end before designing a brake.

## Standing gate results to compare against

This binary, default flags, both arms:

* `--smoke` OK.
* A5: **exit 0, 0 real windows, on every run of every arm** (7 gated).
* A1: **exact 84-prefix on both arms**; position 71 permutes 2 of 5 (draw) and 0 of 3
  (PM4) — gate over the saved campaign logs rather than one run (gotcha 95).
* Both PM4 capture oracles clean; `truncated=0`; `max` hold streak 1-2.
* Both arms reach `#83 game:\data\skeleton\cinezombie.big` and the title screen.
