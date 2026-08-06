# D3D phase C, part 4 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. `docs/d3d-translation-plan.md` carries the phase C
strategy and, at its end, the **"Phase C part 3"** section — read that first; it is the
record of the session this file hands off from. `docs/d3d-phase-c3-kickoff.md` is the
previous hand-off; **its blocker statement is now known to be wrong in one decisive
way** (see below) and it should be kept only for the recipes it lists.

## The single most important correction

The part 3 hand-off said the boot parks because `dev+0x2B04` "never returns to 0" and
proposed reconciling a worker that is "woken constantly with almost nothing to drain".

**The counter is NEGATIVE.** `[fence] spin- counter=4294966744` = **-552**, and falling.
`sub_82846210` tests `!= 0`, so this is not a wait that is slow — it is a wait that has
been made impossible. Over one 240 s boot the counter takes **6 increments and 18,900
decrements**. The worker is not starved of work; it is draining far more than the title
ever submitted.

Every framing built on "the worker has nothing to drain" is retracted.

## What is actually happening, measured three ways

The command processor is **replaying the callback hand-off block** — the same packets,
millions of times.

`CZ_PM4_MEM_WATCH=BBF39470` (the ISR mirror's callback slot, `[[dev+0x2A94]] + 0x10`)
over 200 s:

| value written | count |
|---|---|
| `0BADF00D` (the re-poison) | 4,076,035 |
| `8284AAD0` (the worker kick) | 2,717,263 |
| `82841878` (the frame tick) | 1,358,673 |
| `827CC628` / `827CC640` | 49 each |

The guest calls the armer `sub_82845BA0` **405 times** in that run.

Corroboration, all from `CZ_RING_TRACE=1`:

* through the boot movie the ring carries ~390 packets and ~48 draws a frame; from
  around frame 384 it goes to **1.25 M packets and 135,000 draws per second** with the
  `XE_SWAP` count frozen and every guest thread parked;
* `sub_828455C0` (the ring submitter) is called **106,160,000+** times, always from the
  D3D worker, always `count=1`, always cycling the **same three segments** — 93, 11 and
  23 dwords. The 93-dword one contains the `sub_8284AAD0` arm block;
* the fence-completion word freezes at a constant (`[wb+0]=00000795`) at exactly the
  frame the runaway starts, because the replayed stream rewrites one stale
  `EVENT_WRITE`. **A fence pinned to a constant while `emitted` climbs is the signature
  of replay**, not of a slow GPU.

`truncated=0` and the indirect-buffer verify stays clean throughout: the parser is
right and the bytes are wrong (gotcha 88, third time).

The loop, stated once: a segment that contains an arm block reaches the worker's token
stream → the worker submits it to the ring → the CP executes the arm and its
`INTERRUPT` → the ISR runs `sub_8284AAD0`, which pushes the SAME token-buffer pointer
onto the worker's ring and `KeSetEvent`s it → the worker walks that buffer again from
`buffer+4` → resubmits the same segment. Gain one, no seed required.

## The two candidates, and the second is the one that fits phase C's rule

1. **The re-poison lands but the replay re-arms.** Then the seed is the first DUPLICATE
   ISR delivery, and it is findable while the numbers are still small: count
   `[fence] arm` against `[fence] isr` per callback through the movie era.
2. **The queued segment should not contain the arm block at all.** On hardware the arm
   is what WAKES the worker; the segments the worker then submits should be the ones
   AFTER it. Redirected emission does not only move packets, it moves **boundaries** —
   the content that would normally have separated the arm from the segment boundary is
   exactly what phase C redirects away, so our reserve service's close/kick can draw
   that boundary in a place hardware never would, leaving the arm inside the segment its
   own wake-up resubmits. `[fence] close` already prints `seg` and `cursor` with SCRATCH
   labels; it needs the ARM cursor compared against the segment extent, which is one
   more field.

A boundary is read by the title too. That is part 2's rule applied to a thing that is
not a packet.

## What changed in the code this session

Both changes are correct on their own evidence and **neither cures the replay** — say so
when quoting them.

1. **`sub_82841AD0` moved from Redirect to RealRing.** The Phase A name
   "PreSwapResolve" is retracted: it RESOLVES NOTHING. End to end it emits a type-0
   write of reg `0x0579`, a `WAIT_REG_MEM` on the ISR mirror, the `sub_82845BA0`
   arm/`INTERRUPT`/re-poison block, and a second `WAIT_REG_MEM`. No draw, no clear, no
   copy, no state. Redirected, **all 405 of a boot's armings landed at cursor
   `BFBEB024`, in the private scratch** — the exact picture part 2 diagnosed and fixed
   for `sub_82846288` alone. Four functions emit that block (`82841AD0`, `82846288`,
   `82849F00`, `8284B9C0`); part 2 moved one, this session moved two more.
   `CZ_D3D_REDIRECT_PRESWAP=1` is the same-binary pre-fix arm.
2. **`sub_8284B9C0` hooked and moved to RealRing.** The probe caught all six of its
   calls running with `cursor=BFBEB014`, the scratch, because its only redirected caller
   is Resolve. It is also the only site that arms `sub_8284AAD0` and the only caller
   that passes `r7 = 1` to `sub_82845AC0` — i.e. the only `+1` the counter ever gets.
   Its hook lives in `d3d_hooks.cpp` (a service, not a probe) and borrows the fence
   probe's budget through `runtime/cpu/fence_probe.h`.

## The instrument you inherit, now with its consumer half

`CZ_FENCE_PROBE=1` (`runtime/cpu/guest_probe.cpp`), still one flag, still on BOTH arms:

```
[fence] emit    sub_828459D0  fence block: cursor, fence value, writeback, CPU fast path
[fence] submit  sub_82845AC0  segment submit: addr, dwords, dev+0x2B04, worker-vs-ring
[fence] close   sub_82845DE0  close/kick: cursor & segment start, both SCRATCH-labelled
[fence] arm     sub_82845BA0  callback arming: CURSOR (SCRATCH-labelled), flags, cb, arg
[fence] isr     sub_82844D38  the graphics ISR: source, mirror, callback, argument
[fence] drain   sub_8284A960  NEW — the 0xC0000000 sentinel: depth, counter, obj+0x48
[fence] fsubmit sub_8284B9C0  NEW — the frame-end async submit: tiles, cursor, counter
[fence] spin-/+ sub_82846210  NEW — the counter spin, on entry and on return
[fence] ringsub sub_828455C0  NEW — the ring submitter, with a running entry TOTAL
```

`CZ_FENCE_PROBE=<N>` sets the line budget (default 40,000). **Set it high**: the stall is
at the END of a boot. Source-0 (vblank) ISR lines are counted, not printed — they were
14,340 of one run's 16,245 and enough `fprintf` to move how far a boot got in fixed wall
time.

Every cursor argument is labelled ` SCRATCH` or not. That label is the whole of "who
reads what this emits", and without it a probe printing a cursor prints a number nobody
can classify.

## The struct, proven rather than assumed

`obj = dev + 0x2AC4`, because `sub_82845AC0` locks `dev+0x2B08` and `sub_8284A960` locks
`obj+0x44`. Therefore:

| field | address | meaning |
|---|---|---|
| `obj+0x10/0x14` | `dev+0x2AD4` | the interpreter's callback / user data (set by a `0x8C000000` token) |
| `obj+0x24` | `dev+0x2AE8` | the shared stream cursor |
| `obj+0x38` | `dev+0x2AFC` | one bit per CPU, `0x01000000 << cpu` |
| `obj+0x3C` | `dev+0x2B00` | interpreter nesting depth: `++` per queue pop, `--` per sentinel |
| `obj+0x40` | **`dev+0x2B04`** | **THE COUNTER** the engine spins on |
| `obj+0x44` | `dev+0x2B08` | the lock |
| `obj+0x48` | `dev+0x2B0C` | the tile RESUME cursor |
| `obj+0x164` | `dev+0x2C28` | the current bin mask |

`obj+0x48` is **not** a bug: it is written at `8284B544` in `sub_8284B228` when a token's
mask ANDs nonzero against `[obj+0x164]`, i.e. the worker walks one token stream **once
per tile** (gotcha 118's two tiles), resuming where the last tile stopped. That is
exactly why the counter's decrement is guarded by `[obj+0x48] == 0` — only the last
tile's walk retires the segment. Our drains alternate nonzero/zero in pairs, one
decrement per pair, which is the design working correctly. **The decrement rate per walk
is right; the number of walks is not.**

## A hypothesis retired for the second time, and why that was not redundant

`CZ_PM4_STOP_ON_WAIT=1` was retired by the part 3 hand-off. That measurement was taken
while the arm blocks were in the SCRATCH, where the walker's own `0x3C` handler never
stalls — the flag **could not apply to them**. With the blocks now in the ring it
genuinely gates them, so it was re-run: still runaway (2.9 B packets, 504 M draws). It
stays retired, now on a premise that survives the change. Gotchas 13 and 79, in our own
notes rather than a capture's.

## Housekeeping still owed (carried forward from part 3, none of it done)

- The investigation scaffolding in `d3d_hooks.cpp`/`d3d_draw.cpp` (the `[d3d] sync-wait`
  print, the reserve prints capped at 12, the INTERRUPT disposition prints capped at 16).
- The walker's whole `case 0x54:` block — its own ISR replication, the mirror mutex, the
  dual-transport fallback. It is **still reached** on the current binary (the
  `[d3ddraw] INTERRUPT #N at position` lines still print), so do NOT delete it yet;
  part 2's claim that walker deliveries went to zero was true only for the arms
  `sub_82846288` emits. Re-check the counter after the remaining two emitters
  (`sub_82849F00`, and whatever the movie player reaches) are accounted for.
- Boot wall-time on the draw arm is still several times the PM4 arm's.
- Phase A label retractions to date: `sub_82845230` is the per-frame GPU sync WAIT (not
  InsertCallback); `sub_8283E950`/`EAF8` are sampler filter/aniso setters (not
  SetShaderConstantF); `sub_82846288` is the CALLBACK ARMER (not a fence/throttle);
  `sub_82838568`/`82838D10` are BeginTiling/EndTiling (not "Unknown"); **and
  `sub_82841AD0` is a pure GPU/CPU hand-off emitter, not "PreSwapResolve"**.

## The picture gate, still open and still the phase C completion criterion

Unchanged. Once the boot settles at `prologue_z01.big`:

```
CZ_D3D_DRAW=1 CZ_NO_WINDOW=1 CZ_VK_FRAME_DUMP=dir ...
python3 tools/frame_signature.py --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" <frame>
python3 tools/frame_matched_diff.py --a pm4run1 pm4run2 --b d3drun1 d3drun2
```
