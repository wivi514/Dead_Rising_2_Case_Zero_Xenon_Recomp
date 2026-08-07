# D3D phase C, part 8 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. `docs/d3d-translation-plan.md` carries the phase C
strategy and, at its end, the **"Phase C part 7"** section — read that first; it is the
record of the session this file hands off from. `docs/d3d-phase-c7-kickoff.md` is the
previous hand-off: **its item 1 is answered and RETRACTED**, its item 2 is now a named
mechanism rather than a symptom, and its item 3 (housekeeping) is still owed untouched.

## What part 7 changed, in one paragraph

There is no ~300x amplifier and there never was. `cpu/chain_stats.h` counts the
GPU/CPU hand-off link by link on every run, and on the PM4 control arm every ratio is
one or a constant — `ints/arms` = 0.9997, `isr/ints` = 1.000, `walks == kicks ==
drains` over 173 s. The draw arm's `arms` column **freezes seven seconds into the
boot** while the numerator keeps counting, so the figure is a stopwatch: 1.8x at 8 s,
30x at 78 s, ~300x if you run for 200 s (gotcha 161). The freeze itself is now traced:
it is ONE event at the **first tiled frame**, the engine blocks in its per-frame GPU
sync, and the fence completion word it waits on is on a **nine-value carousel** that
resets every lap — 440 laps in 4,000 stores, and visibly regressing (1023 -> 1017)
between two consecutive lines of an existing trace. What the command processor is
replaying is the **arm block's own 93-dword segment**, resubmitted 132 times against
the control arm's worst case of 11.

## The mechanism, so part 8 does not have to re-derive it

> The first multi-tile Resolve puts an arm block inside a segment that reaches the D3D
> worker's token stream. Every walk of that stream resubmits the segment; every
> resubmission makes the CP execute the arm block's INTERRUPT again; every interrupt
> makes the ISR kick the worker with the same token buffer. Gain exactly one, and the
> control arm does the same thing — but the replayed segment also carries that frame's
> `EVENT_WRITE` fences, so each turn rewrites the fence completion word with STALE
> values. The engine's next fence wait is then waiting for a number that word will
> never hold. The engine stops, the token-buffer pointer stops advancing with it, and
> the loop loses its only exit.

The three functions, at Case Zero's addresses:

```
82838A50  lwz  r11,0x3500(r31)   ; head of the D3D worker's token stream
82838A94  bl   sub_82846288      ; ARM sub_8284AAD0 with that stream as its argument
82838AA8  bl   sub_82845F68      ; the reserve = CLOSE AND KICK
82838AD0  stw  r11,0(r3)         ; the 0x88000000 token: queue that segment to the worker
```

## Where part 8 starts, in order

1. **The arm block and its `INTERRUPT` must not be inside a segment the worker
   resubmits.** Gotcha 147 restated as a requirement, and the one link in the chain our
   runtime has any say over. Parts 2, 3 and 5 each moved an emitter between streams on
   exactly this reasoning; this is the fifth and the one that matters. The design
   question is whether the segment boundary can be drawn to exclude the arm — the
   guest's own `sub_82845F68` runs at 82838AA8, i.e. **between** the arm and the
   `0x88000000` token, so the ordering may already be there and the extent may be the
   only thing wrong (`[dev+0x3B20] .. [dev+0x30]+4`, and phase C moves `dev+0x30`).
2. **Or: reject a completion-word store that REGRESSES**, as an experiment. Cheap, one
   run, and it would name the cause by making the engine's wait satisfiable again. Treat
   it as a diagnostic and not a fix — a command processor that second-guesses a packet's
   value is not a faithful one — and put it behind a flag that announces itself.
3. **Then re-ask the depth question, in the right units.** `#60 models\zombies.big` is
   not a loading depth; it is the first tiled frame. The right measurement after any fix
   is "how many tiled frames does the draw arm complete", which the chain line's
   `distinct` column answers directly: 2 today, and anything above a few hundred means
   the loop is advancing.
4. **Housekeeping, still owed and unchanged from part 7's item 3.** The walker's
   `case 0x54:` block (its own ISR replication, the mirror mutex, the dual-transport
   fallback) is still reached — check `walker: INTERRUPT callback delivered in-position`
   under `CZ_VK_STATS=N` before deleting anything. `MirrorIsPoisoned()` records zero
   skips on every arm and is inert independently of the brake; it stays until something
   explains why its case no longer occurs.

## The instruments you inherit

Everything from part 6, plus the two lines part 7 added to `CZ_RING_TRACE` — free, and
on every run including ones saved for something else:

```
ring: chain arms=N ints=N isr=N kicks=N (distinct=N repeat=N) walks=N drains=N
           segsub=N/queued=N ringsub=N/ents=N
ring: engine counter[dev+2B04]=%d depth[dev+2B00]=%d
```

Calibration, measured, 173 s per arm:

| | arms | ints | isr | kicks | distinct | walks | drains |
|---|---|---|---|---|---|---|---|
| PM4 control | 13,676 | 13,672 | 13,672 | 7,151 | 545 | 7,151 | 7,151 |
| phase C draw | **227, frozen at 7 s** | 6,903 | 6,903 | 4,486 | **2** | 4,486 | 4,486 |

`distinct` is the load-bearing column: it is how many DIFFERENT token buffers the loop
has iterated on, and part 4 established that this hand-off converges only because that
pointer advances. Two is a replay; five hundred is a pipeline.

The other three that earned their keep this session:

* `CZ_PM4_MEM_WATCH=<fence writeback VA>` on the draw arm, then
  `grep MEM_WATCH | tail -4000 | grep -oE "<- [0-9A-F]+" | sort | uniq -c`. Nine values
  x 440 is what a carousel looks like; a healthy word gives a long tail of singletons.
* `CZ_FENCE_PROBE=400000`'s `ringsub` entry list, `grep -oE "ents: [0-9A-F]+/[0-9]+" |
  sort | uniq -c | sort -rn`. The only place a replayed segment states its own address.
* `[d3d] sync-wait` (always on in draw mode). Two consecutive lines are enough to catch
  the completion word going backwards, which no single sample can show.

## Traps this session paid for — do not re-buy them

* **`CZ_PM4_NO_CP_INTERRUPT=1` cannot test the replay's causality.** It looks like the
  arm — no source 1, no kicks, no replay — and the boot deadlocks at `boot.bct` (file
  #5) with `arms=1 ints=1 isr=0`. The protocol needs the interrupt from frame 1.
* **`[obj+0x48]` non-null at half the drains is NORMAL.** 1,732:1,731 on the control arm
  against 3,576:3,575 on the draw arm. A paragraph was drafted against it before the
  control arm was run.
* **"6 increments against 1,873" is not a starved increment side.** Normalised it is 1.0
  per frame against 3.0 per tiled frame. Same frozen-denominator trap as the 300x, in
  the same session, one screen further down — normalise EVERY total by something that
  is still moving on both arms.

## Standing gate results to compare against

This binary, default flags (brake ON), both arms:

* `--smoke` OK.
* A5: exit 0, 0 real windows, both arms.
* A1: exact 82-prefix on the draw arm; the control arm's position-71 window permutes
  4 of 10 brake-on against 1 of 10 brake-off (Fisher p ~= 0.30). Gate over the saved
  campaign logs rather than one run.
* Control arm reaches `#83 game:\data\skeleton\cinezombie.big`; `truncated=0`; `max`
  hold streak 1 (control) / 2 (draw).

## The picture gate, still open and still the phase C completion criterion

Unchanged, and still blocked behind item 1 — the draw arm has to reach the title screen
first:

```
CZ_D3D_DRAW=1 CZ_NO_WINDOW=1 CZ_VK_FRAME_DUMP=dir ...
python3 tools/frame_signature.py --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" <frame>
python3 tools/frame_matched_diff.py --a pm4run1 pm4run2 --b d3drun1 d3drun2
```
