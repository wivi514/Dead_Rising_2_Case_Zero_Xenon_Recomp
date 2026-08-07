# D3D phase C, part 7 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. `docs/d3d-translation-plan.md` carries the phase C
strategy and, at its end, the **"Phase C part 6"** section — read that first; it is the
record of the session this file hands off from. `docs/d3d-phase-c6-kickoff.md` is the
previous hand-off: its items 1 and 2 are **answered**, item 3 needed no action, and its
housekeeping list is still owed.

## What part 6 changed, in one paragraph

The brake is **on by default** (`CZ_PM4_NO_STOP_ON_WAIT=1` is now the control arm),
promoted on 40 runs rather than part 5's one: the PM4 arm holds 2,446 frames ±1 with
the swap queue's head equal to its tail 10 of 10, the phase C draw arm holds
3,614–3,670 against a bimodal 332…3,451,841 free-running, and `truncated=0` with zero
crashes across all 40. Part 4's prediction that the draw arm's replay would COLLAPSE
under the brake is **retired**: per frame the callback hand-off's amplification falls
~39× (430 → 11.1 kicks/frame against a healthy 1.9) while the raw
deliveries-to-armings ratio stays at **~300×**. The brake contains the symptom and
does not touch the cause. The other half of the session was the harness: three of the
four numbers the decision would have been scored on could not have scored it, and the
counter written to replace them was wrong twice (gotchas 157–160).

## Where part 7 starts, in order

1. **The ~300× amplification is the open question, and it is now the ONLY one in the
   hand-off that matters.** One guest arming produces about three hundred
   `sub_8284AAD0` deliveries on the draw arm and about half an arming's worth on the
   PM4 arm. That ratio is unchanged by the brake, by part 5's per-CPU acknowledge, and
   by parts 2–3's emitter moves. Measure it the way part 6 did — `CZ_FENCE_PROBE=400000`
   with `CZ_PM4_MEM_WATCH=BBF39470`, 200 s, **both arms, same binary** — and check the
   budget did not saturate before believing any count (part 6's three runs came in at
   295k/145k/116k of 400k, so they were counts).
   The specific question part 6 could not reach: **what does one delivery do that makes
   the next one happen?** The control arm's loop has gain < 1 because the guest arms
   with a new token buffer every frame (part 4). Print the token-buffer POINTER at each
   delivery on the draw arm and see whether it advances at all — `[fence] kick` already
   flags a kick that repeats the previous one.
2. **Then re-ask whether the draw arm can reach the control arm's depth.** It stops at
   `#60 models\zombies.big` in all 20 draw-arm runs while the control arm reaches
   `#83 skeleton\cinezombie.big` in all 20. That gap is stable, it is not the brake's
   doing, and it is the phase C completion criterion's real blocker.
3. **Housekeeping, still owed and now partly decidable.** The walker's `case 0x54:`
   block (its own ISR replication, the mirror mutex, the dual-transport fallback) is
   still reached — check `walker: INTERRUPT callback delivered in-position` under
   `CZ_VK_STATS=N` before deleting anything. `MirrorIsPoisoned()` did NOT become
   deletable: it records zero skips across all 40 part-6 runs **including brake-off**,
   so it is inert independently of the brake and something else explains why its case
   no longer occurs. The investigation scaffolding in `d3d_hooks.cpp` / `d3d_draw.cpp`
   (the `[d3d] sync-wait` print, the reserve prints, the INTERRUPT disposition prints)
   is unchanged.

## The instruments you inherit

Everything from part 6, plus one new line in `CZ_RING_TRACE`:

```
ring: waits unmet=N held=N streak=N max=N
```

`max` is the longest run of CONSECUTIVE ticks the ring has spent on ONE wait, and it
is the number that says paced vs parked. Calibration, measured:

| state | max streak |
|---|---|
| PM4 arm, brake on (paced) | **1** |
| phase C draw arm, brake on (paced) | **2** |
| deliberately parked (`CZ_ISR_SINGLE_CPU=1` + brake) | **5,491** |
| brake off | 0 (it never holds) |

**Do not replace this with a release COUNT.** Part 6 tried, twice. A release detected
by "the stall's address changed" reads the two arms differently, because phase C
re-emits its hand-off block at a FIXED private-scratch address every frame while the
PM4 arm's blocks rotate through ring addresses — identical behaviour scored 100% on one
arm and 4.9% on the other, and produced a retracted finding (gotcha 157).

## Numbers this project quoted that part 6 corrected

* **The boot opens 84 files, not 64**, and ends at `#83 game:\data\skeleton\cinezombie.big`
  — through `cinematics.big` and `700_prologue_intro.big`. The old figure was the
  `NtCreateFile` print cap, which sat exactly at the boot's depth, so
  `prologue_z01.big` looked like the end (gotcha 109, rewritten against its emitter).
  **Any table in this project's docs written before part 6 that names `prologue_z01.big`
  as a depth is quoting the cap.**
* **The draw arm's free-running frame count is bimodal**, 332…3,451,841 over ten runs.
  Part 5's "1,745 frames" and "2,856,448 frames" are two modes, not two measurements.
* **Free-running overflows the flip queue in 10 of 10 control-arm runs** (head 25–29,
  tail ~3,679). The unpaced state was never healthy.

## Standing gate results to compare against

This binary, default flags (i.e. brake ON), both arms:

* `--smoke` OK.
* A5: **exit 0, 0 real windows, both arms.**
* A1: exact **82-prefix** on the draw arm; the control arm hits the long-known
  position-71 permutation in **4 of 10** runs against **1 of 10** brake-off (Fisher
  p ≈ 0.30 — not distinguishable, and no cost in depth or in A5). Gate over the saved
  campaign logs rather than one run; 40 of them are the free control (gotcha 95).
* `pm4_packet_lengths.py` 0 disagreeing over 24.5 M packets; `pm4_indirect_walks.py` OK
  over 28,726 buffers; `truncated=0` in all 40 runs.

## The picture gate, still open and still the phase C completion criterion

Unchanged, and still blocked behind item 2 — the draw arm has to settle at the title
screen first:

```
CZ_D3D_DRAW=1 CZ_NO_WINDOW=1 CZ_VK_FRAME_DUMP=dir ...
python3 tools/frame_signature.py --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" <frame>
python3 tools/frame_matched_diff.py --a pm4run1 pm4run2 --b d3drun1 d3drun2
```
