# D3D phase C, part 3 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. `docs/d3d-translation-plan.md` carries the phase C
strategy and, at its end, the **"Phase C part 2"** section — read that first; it is the
record of the session this file hands off from. `docs/d3d-phase-c2-kickoff.md` is the
previous hand-off and its blocker is CLOSED; keep it only for the recon it lists.

## What changed in session 14, and what it bought

**The movie deadlock is fixed.** The boot no longer parks at `cinematics.big`. The
cause was not interrupt timing, worker semantics or the movie's content: it was that
the redirect swallowed a block of packets whose reader is the TITLE, not us.

Two changes, both in `runtime/gpu/`:

1. **`D3dDraw_ServiceRealRing`** (`d3d_draw.cpp`/`.h`, wired through a new
   `ServiceResult::RealRing` in `d3d_hooks.cpp`). `sub_82846288` — the Phase A table's
   "fence/throttle-shaped" row, now retracted: it is the **callback armer** — runs with
   the REAL cursor block restored, so its arm / WAIT_REG_MEM x3 / INTERRUPT / re-poison
   block lands in the ring and the title's own ISR delivers it. The walker no longer
   emulates the ISR at all.
2. **The reserve service closes and kicks.** `sub_82845F68` is not "give me space", it
   is close-and-kick, and Resolve's multi-tile path calls it purely for the kick and
   discards the return value. The service now restores the real cursor, runs the
   guest's own reserve, adopts the fresh segment and re-installs the scratch.
   `CZ_D3D_NO_RESERVE_KICK=1` is the same-binary arm for the old behaviour.

Measured, one boot each, same binary:

| | before | after |
|---|---|---|
| ISR delivers `sub_8284AAD0` (the worker kick) | 1 in a whole boot | continuous* |
| walker-delivered interrupts | 200 (all `82841878`) | 0 |
| deepest file opened | #56 `cinematics.big` | #60 `models\zombies.big` |

\* `CZ_FENCE_PROBE` shares a 40,000-line budget across its five hooks. The "before" run
never reached it (17,385 lines in 240 s), so **1** is a true whole-boot count; the
"after" run saturates it, so the count there is a floor, not a ratio.

Gates, both arms, this binary: `--smoke` OK; A1 = exact **84**-prefix on the control
arm and exact **82**-prefix on the draw arm; A5 = **exit 0, 2 permutation windows, 0
real** on both. Unchanged from the phase C best.

## The new instrument you inherit — use it before theorising

`CZ_FENCE_PROBE=1` (`runtime/cpu/guest_probe.cpp`) is the whole producer side of the
fence/callback protocol, on BOTH arms, in one flag:

```
[fence] emit    sub_828459D0  fence block: cursor, fence value, writeback, CPU fast path
[fence] submit  sub_82845AC0  segment submit: addr, dwords, dev+0x2B04, worker-vs-ring
[fence] close   sub_82845DE0  close/kick: dev, cursor, segment start, dwords, flags
[fence] arm     sub_82845BA0  callback arming: CURSOR (scratch or ring), flags, cb, arg
[fence] isr     sub_82844D38  the graphics ISR: source, mirror, callback, argument
```

Capped at 40,000 lines shared across the five. **The comparison that solves things is
the same flag on the PM4 control arm and on `CZ_D3D_DRAW=1`, diffed** — that is what
found the missing kick after three sessions of hypotheses about interrupt races.

## THE BLOCKER: the engine spins waiting for async segments to drain

Every run now parks at `models\zombies.big` (file #60 of 64) with **one guest thread at
99% CPU** — `tid 0x00000F34`, the engine thread — in:

```
sub_82846210 + 0x58:   while ([dev+0x2B04] != 0) ;
```

reached from `sub_8283C898` (the Phase A table's `SuspendNotify_q`) from
`sub_827D3898`. `gdb -p $(pgrep -x cz_runtime); thread apply all bt 8` shows every
other thread parked.

What is known about `dev+0x2B04` (it is the count of outstanding async command
segments, and it is per-title state, not documented anywhere):

- **Incremented** only in `sub_82845AC0` (`[dev+0x2B04] += r7`), and the only caller
  that ever passes `r7 = 1` is `sub_8284B9C0` at `8284BB44` — the frame-end async
  submit, itself called from Resolve (`82838858 + 0x470`) and `sub_82838790`.
- Nonzero also SWITCHES `sub_82845AC0` from direct ring submission to the worker token
  queue, so the first async frame makes every later submission the worker's problem.
- **Decremented** by `sub_8284A960`, called from the token interpreter `sub_8284B568`
  on the D3D worker thread. A whole-image scan finds no `stw` to that offset outside
  `sub_82845AC0` and the GPU-reset path `sub_8284C730`; a hardware watchpoint on the
  control arm named `sub_8284A960` in one hit. **This is how to answer "who writes X"
  in this codebase when the scan comes back empty** — see the recipe below.
- Control arm: oscillates `0 -> 1 -> 2 -> 1 -> 0` continuously. Draw arm: never
  returns to 0.

Two counts from the same probe window sharpen the question:

- the draw arm **ARMS** `sub_8284AAD0` exactly **4 times** in a boot, and the ISR then
  delivers it thousands of times. The mirror stays armed, so every later source-1
  interrupt re-enqueues the same job;
- the draw arm submits **28** segments to the worker token queue where the control arm
  submits **13,498** — redirected emission is precisely what empties those segments.

So the worker is woken constantly with almost nothing to drain, and the counter never
reaches zero. Reconciling those is probably the fix.

Ranked hypotheses, cheapest first. **The first was already run and RETIRED** — do not
repeat it:

1. ~~**The re-poison is not landing, so the stale arming is re-delivered.**~~ The
   arm block ends with a `WAIT_REG_MEM` (wait until the mirror's CPU-bit word reads 0,
   which the ISR's own tail clears) followed by the type-0 write of `0x0BADF00D` back
   into register `0x057C`. Our CP does not stall on an unsatisfied wait by default, so
   the poison could plausibly be landing before or after the wrong thing. **Measured:
   `CZ_PM4_STOP_ON_WAIT=1` changes nothing** — same park at file #60, same re-delivery
   count. The re-poison's timing is not the cause. (It is still worth knowing that the
   arm is re-delivered; that is a consequence of the worker being stuck, not its
   cause.)
2. **The token interpreter is running on a job it cannot drain.** `CZ_JOBQ_PROBE=1`
   already reports `sub_8284B568`'s entry state and printed
   `cb=00000000 *** NULL — this call will fault ***` on every one of its (capped) 4
   entries on the draw arm. Raise that cap and compare against the control arm: if the
   control arm's entries have a real callback and ours never do, the token stream the
   worker is handed is the thing that is wrong, and it is wrong because parts of it
   describe segments whose CONTENT we redirected away.
3. **The tiled path itself.** Resolve takes its multi-tile branch when `dev+0x327C > 1`
   (gotcha 118's two 640-wide tiles) and that branch is the whole async machinery:
   `sub_82838568` = BeginTiling (it calls `sub_8284AB60`, which sets `dev+0x3460` from
   `dev+0x3468` — the async-mode switch), `sub_82838D10` = EndTiling. Both were listed
   as "Unknown" in the Phase A table; they are decoded now (their only caller is the
   movie player `sub_827A00B8`, which brackets them with the complementary bin masks
   `0x15555555` / `0x2AAAAAAA`). If the async path cannot be made to work under
   redirected emission, forcing `dev+0x327C = 1` is NOT a fix — it changes what the
   title renders — but it IS a diagnostic arm that says whether the whole remaining
   blocker is the tiled path.

## The recipe worth keeping: naming the writer of a guest word

When `tools/gdis.py` and an image-wide scan for the offset both come back empty, the
value is being written through a register-held pointer and no static scan can see it.
One hardware watchpoint answers it, and it costs one run:

```
(cd runtime/build && CZ_NO_WINDOW=1 timeout 400 ./cz_runtime > /tmp/w.log 2>&1 &)
sleep 130                       # get past the era you care about
gdb -q -p $(pgrep -x cz_runtime | tail -1) -batch -ex 'set pagination off' \
    -ex 'p (void*)g_memory.base' \
    -ex 'watch *(unsigned int*)((char*)g_memory.base + 0x40004884)' \
    -ex c -ex 'bt 6' -ex c -ex 'bt 6' -ex detach
```

`0x40004884` is `dev + 0x2B04` for this title (`dev = 0x40001D80`, stable across runs;
`CZ_FENCE_PROBE`'s `[fence] close` line prints it). The values print big-endian — `2`
reads as `33554432`.

## The picture gate, still open and still the phase C completion criterion

Unchanged from part 2, and now much closer: the boot reaches `prologue_menu`'s zone
list. Once it reaches `prologue_z01.big` (#63) and settles:

```
CZ_D3D_DRAW=1 CZ_NO_WINDOW=1 CZ_VK_FRAME_DUMP=dir ...
python3 tools/frame_signature.py --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" <frame>
python3 tools/frame_matched_diff.py --a pm4run1 pm4run2 --b d3drun1 d3drun2
```

Note the kickoff-part-2 claim that `CZ_FAKE_START_MS` "does NOT fire under
`CZ_NO_WINDOW=1`" is **wrong as stated** — read `XamInputGetState_x` in
`runtime/kernel/imports.cpp`: the synthetic arm is checked before `Host_PadState` and
works headless. What was true is that it produced zero presses, because the boot was
deadlocking before the frontend ever polled the pad. It may be usable now.

## Housekeeping still owed

- The investigation scaffolding in `d3d_hooks.cpp`/`d3d_draw.cpp` (the `[d3d] sync-wait`
  print, the reserve prints capped at 12, the INTERRUPT disposition prints capped at 16)
  is still there. The INTERRUPT disposition prints are now dead on the content path —
  strip them to counters once the walker's `0x54` case is confirmed unreachable.
- The walker's whole `case 0x54:` block (its own ISR replication, the mirror mutex, the
  dual-transport fallback) is dead code on the fixed path. Its counter reads 0. Delete
  it only after a run confirms that, and say so in the commit — it is the record of
  four failed designs and deleting it silently loses that.
- Boot wall-time on the draw arm is still ~4-5x the PM4 arm's.
- Phase A label retractions to date: `sub_82845230` is the per-frame GPU sync WAIT (not
  InsertCallback); `sub_8283E950`/`EAF8` are sampler filter/aniso setters (not
  SetShaderConstantF); `sub_82846288` is the CALLBACK ARMER (not a fence/throttle);
  `sub_82838568`/`82838D10` are BeginTiling/EndTiling (not "Unknown").
