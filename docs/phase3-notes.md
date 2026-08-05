# Phase 3 — the window, the present seam, and real input

Written 2026-08-05 (session 10). The plan is `docs/runtime-plan.md` §"Phase 3"; the
hand-off that started this work is `docs/phase3-kickoff.md`, and it was accurate —
three of the pieces it said already existed did exist and were not rewritten.

This file records what the work found that neither document predicted, and it is
written for someone porting a *different* Xbox 360 title: almost nothing here is
about Dead Rising 2.

**A blank window is the correct result of this phase.** There is no renderer until
phase 5. Success is: the window opens, it presents at the guest's own swap rate, and
a real press moves the boot. Anyone reading this later and hunting a renderer bug is
hunting something that has not been written yet.

---

## 1. What phase 3 actually had to build

The kickoff's most useful paragraph was the one listing what was already done, and it
held up:

- **All four input imports were already implemented** (`XamInputGetState`,
  `XamInputSetState`, `XamInputGetCapabilities`, `XamInputGetKeystrokeEx`). The job
  was never to write them. It was to put a device behind
  `XamInputGetState`, which until now answered "a connected pad with nothing pressed,
  forever".
- **The present seam's data already flowed.** `VdSwap` emits the front buffer's fetch
  constant and an `XE_SWAP` packet carrying `'SWAP'`, front buffer, width, height
  (finding 39); `gpu/pm4.cpp` case `0x64` received it and incremented a counter.
- **The frame clock was already real and verified** against B1 — exactly one `XE_SWAP`
  per frame.

So the new code is one module, `runtime/host/window.{h,cpp}`, plus four wiring edits.

## 2. Finding 42 — a window is a thread, and it is *the* thread

SDL's video subsystem must be initialised, pumped and presented from the thread that
created the window. Until this phase that thread was busy: `main()` called
`GuestThread::Run` directly and did not return for the life of the process.

One of the two had to move, and **the guest is the one that moves**:

- SDL's main-thread rule is a hard platform requirement elsewhere (macOS will not
  deliver events off the main thread at all) and a works-until-it-does-not on
  X11/Wayland. Satisfying it costs one `std::thread`.
- The guest side was already proven thread-agnostic. Every one of Case Zero's ~19
  other guest threads has always run through exactly this path —
  `GuestThreadHandle` spawns a `std::thread` and calls `GuestThread::Run` on it. The
  main guest thread is not special to the runtime; it is special to the *title*,
  which identifies threads by its own ids and never asks what host thread it is on.

What it must keep is its **order**: it is still the first guest thread created, so it
still takes the first guest thread id. That is the property a gate keyed on
first-occurrence order can actually notice, and it is preserved.

The generalisable form: *when a host-side subsystem has thread affinity and the guest
does not, move the guest.* A recompiled title's threads are already an abstraction you
own; the windowing system's are not.

## 3. Finding 43 — route the present through the command stream, not around it

The tempting shortcut is to present from inside `VdSwap`: it is a kernel export, it
already has the front buffer address, and it is called once a frame. It would have
worked, and it would have been wrong in a way that only shows up later.

The present is instead driven from **`pm4.cpp` case `0x64`**, i.e. from the point
where the command processor *reaches* the swap packet that `VdSwap` wrote into the
ring. The descriptor is read out of the packet body, not passed down a side channel.

Three things fall out of that, none of them aesthetic:

1. **A present can only happen at a stream position the parser actually reached.**
   Findings 38-39 were entirely about the difference between "the kernel wrote a
   packet" and "the command processor executed it": for a whole session the ring
   carried a fence packet that our walk dropped, and a thread waited on it forever.
   A present seam fed from `VdSwap` would have kept counting frames through exactly
   that failure — the window would have looked healthy while the GPU was desynced.
2. **The frame count in the window title is the same number `CZ_RING_TRACE` prints**,
   because both are `Pm4_FrameCount()`'s cause rather than two independent counters
   that can disagree.
3. It is what hardware does. The swap is a packet; the display side sees it when the
   command processor gets there.

Cost: one include of `host/window.h` in `pm4.cpp`. That file is deliberately isolated
(it must stay reusable by an offline replay harness with no runtime attached), and one
function-declaration header keeps that true.

## 4. Finding 44 — the packet number is a contract, and it is the whole input protocol

`XINPUT_STATE::dwPacketNumber` changes **only when the state changes**. A title is
entitled to compare it against the previous poll and skip re-reading the gamepad
struct entirely.

That makes two obvious implementations both wrong:

- **A counter that ticks every poll** tells the title its input changed 12,365 times a
  boot. Not fatal, but it defeats the optimisation the field exists for, and it makes
  any future "did the guest see my press" question unanswerable from a log.
- **A constant** with a changing button field hands the guest a press it may
  legitimately ignore — and the title is *correct* to ignore it. This is the failure
  mode that looks like "input does not work" and is really "input works and was
  filtered".

So the number moves in exactly one place, `PublishPad`, and only when the published
state differs from the last one. The event loop recomputes the pad state every
iteration and throws away identical results.

`CZ_INPUT_TRACE=1` prints every published packet with its button mask. It is an
instrument, not an arm: it fabricates nothing and only reports what the guest is about
to be told.

## 5. Finding 45 — the two conversions between SDL and XInput, and why only one is a bug risk

The button maps are a rename — `SDL_GameController` *is* the 360 pad's layout,
generalised, so the mapping is one-to-one and uninteresting. Two things are not
renames:

- **The stick Y axis is inverted.** SDL's Y points down, XInput's points up. Getting
  this wrong produces a game that works perfectly except that up is down — which reads
  as a guest bug, in a subsystem nobody suspects, months later.
- **Triggers are scaled.** SDL reports 0..32767, XInput 0..255 (`>> 7`).

And one deliberate non-conversion: **no deadzone is applied anywhere.** XInput hands a
title raw axis values and every Xbox 360 title applies its own deadzone, Case Zero
included. Filtering in the runtime would be inventing an input characteristic the
console does not have, and its symptom — a slightly unresponsive stick — is exactly
the kind of thing that gets blamed on the game for a whole session.

The honest consequence, recorded so it is not later "fixed": with a physical
controller attached, stick jitter around rest changes the raw state, so the packet
number ticks and `CZ_INPUT_TRACE` becomes very noisy. That is what the hardware does
too — XInput's packet number moves on any raw change, drift included — so the
tolerance belongs in the title's deadzone, not in ours. On a keyboard-only run the
trace stays silent until a key is actually pressed, which is what makes it usable as
the witness for the gate.

## 6. The instruments and arms this phase adds

| name | what it is |
|---|---|
| `CZ_NO_WINDOW=1` | **The control arm.** No window, no present, no pad; `XamInputGetState` answers with its documented neutral pad. Same binary, same boot, minus input. |
| `-DCZ_WINDOW=OFF` | Configure-time headless build. Announces itself on every startup. |
| `CZ_INPUT_TRACE=1` | Every published pad packet with its button mask — the witness that a press reached the guest. |
| `CZ_FAKE_START_MS=N` | **Unchanged, and kept.** Now that real input exists it stops being the only way to move the boot and becomes the control for "was it really my press that did that". Still fabricates evidence; still must never be on for a gate run (gotcha 78). |

The headless arm reports the pad **connected**, not absent, and that is the honest
reading of the hardware: a console with a pad plugged in and nobody touching it is
precisely a connected device reporting nothing pressed. Reporting `NOT_CONNECTED`
would send the title down its reconnect-the-controller path — a *different* boot,
which is the one thing a control arm must not be.

Two smaller decisions, both recorded because they are the kind of thing that gets
"cleaned up" later:

- **No `SDL_RENDERER_PRESENTVSYNC`.** The guest's swap rate is the frame clock. A
  vsync-paced present adds a second clock that silently becomes the slower of the two.
- **`SDL_HINT_NO_SIGNAL_HANDLERS=1`.** SDL would otherwise turn SIGINT/SIGTERM into an
  `SDL_QUIT` event, which sounds like an improvement and is not: every gate run in
  this project is `timeout N ./cz_runtime`, and routing SIGTERM through our event loop
  makes process termination depend on that loop still being alive.

## 7. Measured

### The control that gotcha 86 requires: the old binary run *now*

Not the old binary's remembered numbers. `git stash` of the four wiring edits, a
rebuild, a copy to `/tmp/cz_old`, restore, rebuild to `/tmp/cz_new` — then **six runs
of 100 s with the arms alternated**, old/new/old/new/old/new, so that anything drifting
across the afternoon hits both.

| arm | A1 depth | position 71 permuted | A5 gate | frames / 100 s | `truncated` | title screen |
|---|---|---|---|---|---|---|
| pre-phase-3 | 84, 84, 84 | 3 of 3 | exit 0 ×3 | 3182, 3183, 3183 | 0 ×3 | 3 of 3 |
| phase 3 | 84, 84, 84 | 1 of 3 | exit 0 ×3 | 3152, 3151, 3152 | 0 ×3 | 3 of 3 |

Two things to read off it, and one not to.

**The position-71 permutation is on BOTH binaries.** It fires 3 of 3 on the *old* one.
Had phase 3 been measured alone it would have looked like a regression the window
introduced; it is the same scheduling-sensitive window that gotcha 86 and finding 41
both already recorded, and the correct response is the one those findings prescribe —
run the old binary now, alternated. Do **not** read "1 of 3 vs 3 of 3" as an
improvement either: n=3 on a coin cannot see a difference in the coin.

**The window costs about 1% of the frame rate**, and the same-binary arm proves it is
the window rather than the wiring: `CZ_NO_WINDOW=1` on the *phase 3* binary produces
**3183 frames**, i.e. the pre-phase-3 number to the frame. So the delta is an event
loop, a clear and a present sharing the machine, not a change to how the guest runs.

Everything else is unchanged: `--smoke` passes, both gates hold, `truncated=0` on every
run, and every run of both arms reached the title screen.

### The present seam

- One present per `XE_SWAP`, sourced from the packet body rather than from `VdSwap`.
- First present of a run: `front buffer A0E48000, guest says 1280x720` — the guest
  states its own front-buffer dimensions in every swap, so the window is sized from
  those and not from our default.
- ~31.5 presented frames/second over a 100 s run, which is the guest's own rate.

## 8. The gate — PASSED, on one real press

> **The A1 gate advances from position 84 to 85 (`XamShowDeviceSelectorUI`) on a real
> key or button press, with `CZ_FAKE_START_MS` unset.**

This gate could not be closed from inside the machine, and that is a property of the
gate rather than an obstacle to it: the whole point is to retire the synthetic-input
arm, so any press the session could manufacture is the very thing being retired. There
is no `xdotool`/`ydotool` on this host, and installing one would only move the
fabrication one layer down — a synthesised X11 key event is still not a human. So it
was scheduled like a Xenia capture (gotcha 103), with everything that did *not* depend
on it finished and committed first.

**The operator focused the window and pressed Enter. The log:**

```
[kernel] XamInputSetState(user=0, motors 0/0)
[host] pad packet 2: buttons=0010 triggers=0/0 L=(0,0) R=(0,0)     <- START down
[kernel] XamInputSetState(user=0, motors 0/0)
[kernel] XMACreateContext -> context 1 at BFFEB040 (phys 1FFEB040)
[kernel] MmMapIoSpace(bus=2, phys=1FFEB040, 64 bytes, protect=404) -> BFFEB040
[kernel] XamInputSetState(user=0, motors 0/0)
[kcall] XamShowDeviceSelectorUI                                     <- position 85
[kernel] XexGetProcedureAddress module=30002000 ord=0x279 -> 824A52E0
[host] pad packet 3: buttons=0000 triggers=0/0 L=(0,0) R=(0,0)     <- START up
```

Five lines from press to advance, and the release had not even happened yet. The A1
gate reads **85 `XamShowDeviceSelectorUI` ↔ `XamShowDeviceSelectorUI`** where every
run before it stopped at 84. `CZ_FAKE_START_MS` appears zero times in the log; the run
had no synthetic input of any kind.

The `[host]` lines are worth reading as their own confirmation of §4: **two** packets
for one press, not two hundred, over the ~600 polls that happened while the key was
down. The packet number moved on the edges and nowhere else, which is the contract.

The four arms, three of them now observed:

| run | result |
|---|---|
| real press | pad packet logged, A1 advances 84 → 85 — **observed, gate PASSED** |
| no press | no pad packet, A1 stays at 84 — **observed** (a 420 s run with nobody at the keyboard) |
| `CZ_NO_WINDOW=1` | no pad packet, A1 stays at 84 — **observed** (the control arm) |
| `CZ_FAKE_START_MS` | advances, and says on every press that it fabricated it |

The negative arm is worth keeping in the record. It is the correct negative result, and
it shows the witness works in the direction that matters: no press, no packet, no
advance, and no ambiguity about which.

### What the press revealed about the next blocker

Immediately after `XamShowDeviceSelectorUI`, the title resolves a XAM export
**dynamically by ordinal** — `XexGetProcedureAddress(module=30002000, ord=0x279)` —
and our runtime answers with a minted thunk that fails honestly.

That is not a divergence. **A1 line 111,986 makes exactly the same call**, four lines
after its own `XamShowDeviceSelectorUI`, so we are in the right place in the sequence.
What A1 does next settles where the boot goes from here:

```
XamUserGetXUID  ->  KeResetEvent  ->  XamContentAggregateCreateEnumerator
  ->  XamGetPrivateEnumStructureFromHandle  ->  XamAlloc  ->  NtCreateEvent
  ->  ObReferenceObjectByHandle  ->  XamTaskSchedule  ->  XamGetOverlappedResult
  ->  XMsgInProcessCall  ->  XMsgCompleteIORequest  ->  NtClose  ->  NtClose
```

which is positions 86-92 and is **precisely the save-data layer that was deliberately
deferred** out of finding 34 as the phase 2/8 file work. Phase 3 hands off exactly
where the plan said it would, and it hands off to a list of imports that are already
named. Note also that `XamTaskSchedule` is on it — one of finding 34's eight
implemented-but-never-executed imports (gotcha 67), so that debt starts getting paid
by the next phase rather than needing its own.

## 9. Status

Phase 3 is **complete** and every gate passes:

- **The phase 3 gate: A1 84 → 85 on a real press, `CZ_FAKE_START_MS` unset.** Passed.
- `cz_runtime --smoke` — the phase 0.2 link gate, still passing.
- A5 `--include-high-frequency` — **exit 0**, all three mismatch windows permutations.
- A1 — the full 84-deep prefix on both arms, 85 with a press.
- `ring: indirect buffers truncated=0` on every run.
- The window opens, presents at the guest's swap rate, and reports a live frame count.
- Real keyboard and SDL game-controller state reaches `XamInputGetState`.
- No crash in any run of this session.

The synthetic-input arm is retired as the *only* way to move the boot, and kept as the
control for "was it really my press that did that".
