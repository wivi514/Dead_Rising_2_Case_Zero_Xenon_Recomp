# D3D phase C, part 6 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. `docs/d3d-translation-plan.md` carries the phase C
strategy and, at its end, the **"Phase C part 5"** section — read that first; it is the
record of the session this file hands off from. `docs/d3d-phase-c5-kickoff.md` is the
previous hand-off; its step 1 is **answered** and its step 2 is **done**, so keep it only
for its struct table and its recipes.

## What part 5 changed, in one paragraph

Part 5 answered part 4's question — *who writes the zero the hand-off block waits for?* —
and the answer was not in the ring at all. It is the **guest's own vblank ISR**, behind a
GPU MMIO **read** at `0x7FC86544` bit 0 that our runtime had left at zero for the life of
every process this port has ever started. Behind that bit sits the title's swap queue
(16 records of `{surface, due tick}`, a vblank tick at `dev+0x4174`, a walker
`sub_82841760` whose only caller is the ISR). Asserting the bit starts the walker; that
is `gpu/vd.h`'s `kDisplayControllerGate`, on by default, `CZ_NO_VBLANK_GATE=1` to turn it
off. A second, independent defect turned up the moment the first was fixed: the graphics
interrupt is addressed to a **set** of hardware threads via a six-bit mask in the mirror's
first word, and our single ISR thread could only ever acknowledge one of them. It now
takes the interrupt once per named CPU (`CZ_ISR_SINGLE_CPU=1` restores the old
behaviour).

With both, plus part 4's stall-with-resume, `CZ_PM4_STOP_ON_WAIT=1` **works**: the
control arm runs the real GPU/CPU hand-off protocol, paced by the guest, and boots to the
title screen with `truncated=0` and a swap queue whose head equals its tail.

## Where part 6 starts, in order

0. **Know the cost you inherit before you read any default-flags number.** With the
   brake OFF, the per-CPU acknowledge makes the draw arm's runaway spin *harder* —
   1,745 `XE_SWAP` frames in 100 s became 2,856,448, because each interrupt now produces
   several worker kicks instead of one. Same flywheel, more gain. The control arm is
   untouched (3,091 vs 3,088 frames). So a default-flags draw-arm measurement taken
   before item 1 lands is measuring an amplified fault, not the runtime.
1. **Turn one run into a rate, and then decide the default.** The paced boot above is
   **one run**. Gotchas 50-51-86 apply with full force, and `CZ_PM4_STOP_ON_WAIT` is
   still off by default only because nobody has measured it properly yet. The measurement
   is the standard one: same binary, arms alternated, ~10 runs of 120 s per arm, on
   BOTH the PM4 control arm and `CZ_D3D_DRAW=1`, scoring deepest file, `XE_SWAP` frames,
   `truncated=`, and whether the swap queue's head tracks its tail. If it holds, promote
   it — a command processor that cannot run ahead of the CPU is the faithful behaviour,
   and it is what `MirrorIsPoisoned()` in `gpu/vd.cpp` exists to paper over. If the
   promotion sticks, that whole poison-skip path becomes deletable, and saying so in the
   commit is worth more than the lines it removes.
2. **Then re-ask phase C's own question, which is now askable for the first time.**
   Part 4 stated it precisely: *does a CP that cannot run ahead stop the draw arm's
   runaway?* Part 3 measured the runaway as 8,152,069 writes to the ISR mirror's callback
   slot against 405 guest armings, `sub_828455C0` called 106 M times, and
   `dev+0x2B04` going negative. Re-run those exact instruments
   (`CZ_FENCE_PROBE=400000`, `CZ_FENCE_RINGSUB`, `CZ_PM4_MEM_WATCH` on the callback slot)
   on the draw arm **with the brake on** and diff against part 3's numbers. The prediction
   is that the replay collapses, because the flywheel's gain-one loop can no longer spin
   faster than the guest produces frames — but that is a prediction, and part 4 is a
   lesson in what happens when one of those is believed early.
3. **The swap queue still overflows whenever the brake is off**, and that is now
   understood rather than mysterious: `head{due=...}` in `CZ_SWAPQ_TRACE` runs hundreds
   of ticks ahead of `tick`, because `sub_82841878` hands out due times from a running
   cursor (`dev+0x417C`) that advances at the title's frame rate while the tick advances
   at the pump's 62/s. It is a symptom of an unpaced title, not a separate bug — do not
   "fix" it by clamping the due time.

## The instruments you inherit

Everything from part 4, plus:

```
CZ_NO_VBLANK_GATE=1   do not assert 0x7FC86544 bit 0 — the pre-part-5 runtime, in which
                      the swap-queue walker has no caller. The control arm for the gate
CZ_SWAPQ_TRACE=1      the swap queue once a second: gate, vblank tick, records retired,
                      head/tail, the HEAD RECORD's own surface and due tick, the
                      rendezvous word [mirror+4], the acknowledge bitmap [mirror+0], and
                      the count of per-CPU ISR deliveries
CZ_ISR_SINGLE_CPU=1   one ISR delivery per interrupt, as CPU 2 — the pre-part-5
                      behaviour. With it on and the brake on, the ring parks at frame 7
                      with ack[mirror+0]=00000010 forever
```

Read `head{surface=... due=...}` together with `tick`: `due > tick` is the walker's stop
condition and therefore the whole explanation of a pinned head.

## The GPU MMIO surface, complete

Five instructions in the whole 8.8 MB image. Written down because a *read* has no writer
to grep for and this took a session to find (gotcha 153):

| address | R/W | what | site |
|---|---|---|---|
| `0x7FC80714` | W | `CP_RB_WPTR`, the ring kick | `sub_82845698` |
| `0x7FC83214` = 7, `0x7FC83408` = 0x800 | W | engine enable | `sub_8284C770` |
| `0x7FC86110` | W | `D1GRPH_PRIMARY_SURFACE_ADDRESS` | `sub_82841760`, `sub_82841878` |
| `0x7FC86544` | **R** | display controller gate, bit 0 | `sub_82844D38` only |

## The swap queue and the ISR mirror, proven rather than assumed

`mirror = [dev+0x2A94]`, a guest VA (`BBF39460` in every run so far, i.e. `SCRATCH_ADDR`
itself — the mirror base IS scratch register 0's slot).

| field | meaning | proof |
|---|---|---|
| `mirror+0` | six-bit per-CPU acknowledge bitmap | armed by `SCRATCH_REG0 = (flags>>8)&0x3F`; cleared bit-by-bit at 82844D88-82844D98 |
| `mirror+4` | the GPU/CPU rendezvous word | set to 1 by `sub_82841AD0`'s packet; cleared by `sub_82841760` / `sub_82841878` |
| `mirror+0x10/0x14` | armed callback + argument (`SCRATCH_REG4/5`) | read and called at 82844D50-82844D68 |
| `dev+0x4174` | vblank tick | `++` at 82841784, walker only |
| `dev+0x417C` | running "next due tick" cursor | 82841994 |
| `dev+0x4188` | records retired | 828417D8 / 828419DC |
| `dev+0x418C` | 16 × `{surface, due tick}` | index `(x<<3)&0x78` at 828417B0 |
| `dev+0x420C` / `dev+0x4210` | head / tail, free-running | 8284180C / 828419C8 |
| `dev + cpu*0x6C + 0x2C40` | the D3D worker's **per-CPU** job ring, count at `+0x2C94` | `sub_8284AAD0` at 8284AB14 |

`sub_82841AD0(dev, surface)` is the hand-off emitter, and its two shapes matter:
* `surface == 0` → emit `SCRATCH_REG1 = 1`, `WAIT mirror+4 == 1`, then arm.
* `surface != 0` → arm, then `WAIT mirror+4 == 0` — the packet the brake stalls on.

Phase A label retractions to date are unchanged from part 5's list, plus: `sub_82841878`
is the **flip / hand-off completion callback** (the one every arm block installs in
`SCRATCH_REG4`), and `sub_82841760` is the **swap-queue walker**, called only from the
graphics ISR's vblank path.

## Housekeeping still owed (carried forward, none of it done)

- The investigation scaffolding in `d3d_hooks.cpp` / `d3d_draw.cpp` (the `[d3d] sync-wait`
  print, the reserve prints capped at 12, the INTERRUPT disposition prints capped at 16).
- The walker's whole `case 0x54:` block — its own ISR replication, the mirror mutex, the
  dual-transport fallback. Still reached on the current binary, so do NOT delete it yet;
  but item 1 above may retire it, and if it does, say so.
- Boot wall-time on the draw arm is still several times the PM4 arm's.

## The picture gate, still open and still the phase C completion criterion

Unchanged. Once the boot settles at `prologue_z01.big`:

```
CZ_D3D_DRAW=1 CZ_NO_WINDOW=1 CZ_VK_FRAME_DUMP=dir ...
python3 tools/frame_signature.py --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" <frame>
python3 tools/frame_matched_diff.py --a pm4run1 pm4run2 --b d3drun1 d3drun2
```
