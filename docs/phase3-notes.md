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

## 9. After phase 3: the save-data layer (A1 positions 86-92)

The press opened the next blocker and it was implemented in the same session.
`runtime/kernel/content.cpp` is the code and its header comment is the derivation; this
section records what the work *found*, which is the part that transfers.

### Finding 46 — when the capture cannot answer, the title's own SDK wrapper can

Every question this layer raises is about a RETURN value: what shape is the "private
enum structure", what does an enumeration step hand back, what does the title compare
it against. Xenia's log prints arguments on entry and never returns (finding 29), so
A1 and A3 answer none of them.

The title answers all of them, because it contains the XDK's statically-linked
`XamEnumerate` wrapper — `sub_825D9460` — and its task body `sub_825D9358`. Reading
those two functions end to end produced the complete protocol without a single guess:

- an enumerated item is **0x138 bytes** (the wrapper rejects any other buffer size, and
  its caller writes 0x138 into its own item-size out-parameter — two independent
  statements of the same number);
- the private structure carries the **XAM app id at +0 and message id at +4**, and the
  wrapper *checks* that +4 is `0x0002000E`;
- the task builds a 32-byte message and calls `XMsgInProcessCall`, then copies
  **0x134 bytes** from a scratch buffer into the caller's item and the dword at
  **scratch+0x140** into item+0x134 (0x134 + 4 = 0x138, closing the circle);
- on a NEGATIVE return it skips the copy and completes with
  `XMsgCompleteIORequest(ovl, r11 & 0x65B, hresult, len)` — which is A1's
  `(7018F3C0, 0000065B, 80070012, 00000000)` executing, with
  `0x80070012 = HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES)`.

The pleasant consequence: because the guest only ever reads +0, +4, +0x0C, +0x10 and
the *address* +0x18 and hands them straight back to us, **the private structure's
layout is ours to choose.** It is defined so a memory dump is legible, not to match an
SDK header we do not have.

### Finding 47 — a stubbed query made every save invisible, with no error anywhere

This is the session's most transferable defect, and it is gotcha 42's shape with the
volume turned all the way down.

`XamGetExecutionId` was a generated honest-failure stub. It looks like bookkeeping.
It is not: `sub_825D8E60` calls it and compares `[execInfo+12]` — the **title id** —
against the title id of the item just enumerated, and `sub_825D9358` uses that as the
enumeration's FILTER. An item that fails it is not rejected loudly; the task consults a
table of additional accepted ids (`sub_825D8EC0` — that is how the full Dead Rising 2
and Case Zero share progress) and then **pulls the next item**. So:

> a save with the wrong title id is skipped, and an enumeration of nothing but
> wrong-id items is byte-for-byte indistinguishable from an empty one.

Both halves of that were live here at once. The stub never wrote its out-parameter, so
the comparison was against stack garbage; and the enumerator wrote **0** into the
item's title-id field, because at the time that dword was the one part of the layout
the derivation had not reached. Measured, same binary, one save present:

| item title-id field | what happened |
|---|---|
| `0` (before) | one item enumerated, filtered out, run ends `result=1627 extended=80070012` — *looks exactly like an empty save list* |
| `XexTitleId()` (after) | one item enumerated, **`result=0 extended=00000000`** — accepted |

The instrument that separates them is one line: printing
`XMsgCompleteIORequest`'s arguments, which A1 hands us verbatim to compare against.
Without it, "the title accepted our save" and "the title threw it away" produce the
same log — one enumerate step and one completion.

### Finding 48 — the save directory is part of the gate's configuration

A1 was captured with **no save present**, and Xenia says so in as many words:
`XamContentAggregateCreateEnumerator: added 0 items to enumerator`. With one save
present our boot calls `XamGetExecutionId` between positions 90 and 91 — a call A1
never makes, because A1 never had an item to filter — and the gate duly reports a
divergence.

Nothing is wrong in either run. The gate is comparing against a capture taken under a
condition, and the save directory is now one of those conditions, exactly as
`license_mask` is (gotcha 20 — the trial trap). **Run the A1 gate with an empty save
root**, and read a leftover save directory as a configuration difference rather than a
regression.

### Measured

With no save present, which is A1's condition:

```
PREFIX MATCH: our 92 calls are an exact prefix of Xenia's 93.
We stopped before 'KeQueryBasePriorityThread'.
```

**92 of 93, no divergence at all** — the best A1 result this port has produced, up from
84 before phase 3 and 85 after the press. The whole chain lands in A1's order:
`XamShowDeviceSelectorUI` → `XamGetPrivateEnumStructureFromHandle` → `XamAlloc` →
`XamTaskSchedule` → `XamGetOverlappedResult` → `XMsgInProcessCall` →
`XMsgCompleteIORequest` → `XamContentGetDeviceData`.

- A5 `--include-high-frequency`: **exit 0**, every mismatch a permutation.
- `ring: indirect buffers truncated=0`; no crash in any run.
- `cz_runtime --smoke`: passes. 161 of 244 imports real, 83 stubs (was 155/89).
- Position 71 still permutes on some runs. It did so 3 of 3 on the *pre-phase-3*
  binary in this session's alternated A/B, so it is the scheduling-sensitive window
  gotcha 86 already records, not anything this work introduced.

### Four of finding 34's never-executed imports have now executed

Gotcha 67 says an implemented import is a prediction until it runs. These are no longer
predictions: `XamTaskSchedule` (which really does run guest code — `sub_825D9358` — on
a new thread), `XamGetOverlappedResult`, `XMsgInProcessCall` and `XMsgCompleteIORequest`
all execute on this path, and the overlapped they hand around completes correctly.

### What is implemented but still unrun

- `XamContentCreateEx` / `XamContentClose` — the mount that makes `save:` mean a host
  directory. Derived from A3 (root name `save`, one file `DR2P000.DSF` of exactly
  303,104 bytes in a single `NtWriteFile`) and not exercised, because reaching a save
  point needs gameplay. Saves live in a **sibling** of the package directory
  (`assets/save/`, or `CZ_SAVE_DIR`), never inside it: `assets/game/` is extractor
  output and re-running the extractor must not be able to destroy player data.
- The imported `XamEnumerate`. The title reaches the enumerator through its own
  statically-linked wrapper on the path we can see; this is the other door into the
  same object.

## 10. Finding 49 — position 93 is not an import, and it is hardware's ERROR path

A1's last call, `KeQueryBasePriorityThread`, looks like the obvious next thing to
implement. It is not implementable, because **it has been implemented since phase 1**.
Reaching it is the whole problem, and four cheap steps established what it actually
takes — none of which could be guessed from the name.

**1. One call site, and it is the XAPI.** `KeQueryBasePriorityThread` is called from
exactly one place in the image: `sub_825DBA20`, which is `GetThreadPriority` (it maps
16 → 15 and −16 → −15, the Win32 priority mapping, and brackets the call with
`ObReferenceObjectByHandle(0xFFFFFFFE)` / `ObDereferenceObject` — matching A1 line for
line).

**2. One game-side caller, and it is a work-queue drain.** `sub_828576D8`:

```
r28 = this + 0x3EA0                  ; the queue
if (!pop(r28, &item)) return         ; nothing to do — the common case
old = GetThreadPriority(-2)          ; <-- position 93 is here
SetThreadPriority(-2, 15)            ; boost while draining
... dispatch each item through two vtables ...
SetThreadPriority(-2, old)
```

So position 93 is reached only when that queue is **non-empty**, and A1 reaches it
**exactly once in an entire boot** — on the audio thread (Xenia's `F800010C`, the one
that does the XMA `MmMapIoSpace` at gate position 84), at log line 122,563, *after* the
save enumeration.

**3. Our run never enters the drain at all.** `CZ_QUEUE_PROBE=1` over a 200 s run: zero
entries. That distinction matters and no amount of reading gives it — "entered
thousands of times with an empty queue" and "never entered" are different problems.
Probing the drain's seven call sites then showed **three of seven callers do run**
(`sub_828587B0`, `sub_828589D0`, `sub_82859888`, all on guest thread 0xF00) and none of
them reaches it.

**4. Why not — and this is the part that reframes the goal.** The guard at the site
inside a caller that does run:

```
82858C30  cmpwi cr6, r30, 0
82858C34  bge   cr6, 0x82858c40     ; r30 >= 0 -> SKIP the drain
82858C3C  bl    0x828576d8          ; runs ONLY when r30 < 0
```

`r30` is that function's HRESULT, set to `0x88960001` by its own validation checks. **The
drain is the failure/cleanup path.** Across all seven sites: three are guarded on
`r30 < 0`, one runs on a loop completing, and three — in a different subsystem
(`sub_82874BD0`, `sub_82875588`, `sub_82876080`, which pass `[this+0x30]`) — are
unguarded. Our run executes callers only from the *guarded* group.

So matching position 93 means **reproducing a failure hardware had**, in an audio path
we do not drive yet, or reaching the unguarded subsystem, which needs the game to get
further than the frontend. Either is phase 6 territory (audio output and XMA decoding),
not an import.

A related detail from the same thread, worth recording because it looks like a bug and
is not: A1's audio thread opens fifteen `data\audio\fx_*.big` and `zombiance*.big`
banks at line 20,376 and **every one fails** (`C000003A`). Those files are not in the
package — hardware probes for optional banks that were never shipped. Our run does not
attempt them at all, which is a real divergence, but "hardware opened 15 files we
don't" would be exactly the wrong reading of it: hardware opened nothing.

Two method notes:

- **The adjacency trap fired again and was caught by the thread id** (gotcha 68). A1
  puts `KeQueryBasePriorityThread` in the middle of `NetDll_select` traffic, which
  suggests networking. It is not: `select` runs on thread `F8000008` 1,108 times, the
  priority query on `F800010C` once. One `grep` on the thread id killed a whole
  afternoon's worth of plausible socket work before it started.
- **`CZ_QUEUE_PROBE` cost one build and two runs and retired the entire question.**
  Reading the call graph said "this could be reached"; the probe said "it is not, and
  neither is anything that would call it". Kept in `guest_probe.cpp` as the second
  worked example, alongside finding 27's.

### A defect found on the way

The file-open logger prints successful opens only for the first 64 (`n < 64 ||
FileTrace()`), and failures unconditionally. So "our boot opens 64 files" — a number
this project has quoted since finding 37 — is a **logging cap, not a count**. Failures
are complete; successes are truncated. Nothing depends on it today, but the next person
to count files off a default-configuration log would have been wrong.

## 9a. Status

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

## 11. Finding 50 — the crash 53 files past every gate was the title's own assertion

Session 25 (phase C part 13). Part 12 recorded, as a frontier rather than a regression,
that holding `CZ_FAKE_PRESS_SEQ=START,A,A` walks the boot through the menu into the real
game load and SIGSEGVs at file **#137 `game:\data\audio\Prologue.txt`**, with
`lr=827885DC` and `r4` pointing at the string `deadrising/asserts.php`. It is closed, and
the fault was never a memory bug.

### Why it did not look like an assertion

`sub_82788478`'s tail is:

```
82788648  0FE00016  twui r0, 0x16        <- trap ALWAYS
8278864C  93400000  stw  r26, 0(0)       <- store to guest address 0
```

`twi`/`twui` is PowerPC's unconditional trap, and it is how this engine's `dbAssert`
stops. **XenonRecomp lowers it to nothing** — the generated C++ carries the comment
`// twi 31,r0,22` and no code — so the trap does not happen and the deliberate null
store one instruction later is what faults. The crash reporter then truthfully says
"faulting GUEST address 00000000 (the NULL page)", which reads as a wild pointer in
guest code and says nothing about an assertion. Worth carrying to Case West: **in a
recompiled image a guest ASSERT presents as a null-pointer crash**, and the tell is a
`twi` immediately above the faulting store.

The strings the failure path builds name it outright, once the right `lis`/`addi` pairs
are followed (gotcha 144's rule — a 32-bit constant is never one instruction):

```
0x820B0BC8  '0 && "Bad file digest.  Please re-link the executable and try again."'
0x820B0B80  'c:\bcg\deadrisingprologue\library\systemlib\xbox360\digestmanager.cpp'
0x82009B88  'dbAssert: %s\n%s:%d\n'
```

### The chain, and how each link was separated from the next

The line immediately above the crash in every log for four sessions was
`XexGetModuleSection module=88000260 name='Digest' -> not found`. Three links, each
fixed and measured before the next was visible; **none of them changes an observable
alone**, which is why they are one commit.

1. **`XexGetModuleSection` answered nothing, ever.** Its comment said "We have no module
   image to hand back a section from, so STATUS_NOT_FOUND is the truthful answer" — true
   when written, about a runtime with no loader, and never re-asked (gotcha 13). The
   names are XEX **resources**, they live in an optional header of the XEX this runtime
   already publishes, and A1's own header dump lists them:

   ```
   Serial2  82AF0000-82AF0020, 32b     Serial   82AF0080-82AF00BF, 63b
   Digest   82AF0100-82AF011C, 28b     58410A8D 82B00000-82B3072B, 198443b
   ```

   `XexFindResource` walks `XEX_HEADER_RESOURCE_INFO` (key `0x000002FF`: a `blockSize`
   dword then `{ char name[8]; be32 address; be32 size }` entries). Measured:
   `Digest -> 82AF0100 28 bytes`, matching A1 exactly. Names are NUL-PADDED, not
   NUL-terminated, so `Serial` and `Serial2` separate only under an 8-byte compare.

2. **Guest memory at that address held twenty-eight zero bytes**, because `main.cpp`
   skipped `.idata` — and `.idata` is 82AF0000..82AF047A, i.e. exactly where this XEX
   keeps those resources. The skip was a NAME list, `.reloc / .XBLD / .edata / .idata`,
   under the comment "not read at runtime and, in this XEX, their source ranges over-run
   the loaded image buffer". Only the second clause is a real condition and it applies
   to exactly one of the four: the loader points each section at
   `image.data.get() + VirtualAddress` inside one `image.size`-byte buffer, and `.reloc`
   is 0xB00200..0xBBA0D4 against an image size of 0xB40000. `.idata` ends 0x4FB86 inside
   it. **A name list cannot state the condition it stands in for**, so the next reader
   inherits the conclusion without the test; it is now the bounds check itself, and it
   prints which sections it dropped and why.

3. **The guest still compared twenty zero bytes**, because `sub_82822420/28/30` are
   one-instruction tail-call thunks (`b 0x829C3084`, `b 0x829C3094`,
   `li r5,0x14; b 0x829C30A4`) onto the **`XeCryptShaInit` / `XeCryptShaUpdate` /
   `XeCryptShaFinal` imports**, and all three were generated honest-failure stubs. The
   callee is invisible in the caller's disassembly (gotcha 64), which is why four
   sessions of looking at this crash never named them.

### Why a stub was the wrong shape here

This is gotcha 59's family, and the sharpest instance of it this port has produced.
There is **no SHA-1 digest that means "not implemented"**: the result is a value the
caller consumes, not a status it can test, so a stub that returns without writing its
output is not failing honestly — it is answering "the digest of your file is twenty
zero bytes", confidently and wrongly. The caller then does the only sensible thing with
that answer, which is to refuse to run.

Implemented for real, over the console's own 88-byte state layout
(`{ be32 count; be32 state[5]; u8 buffer[64] }`). The partial block has to live in the
state across calls, because this title hashes the file and then the file's **length** as
a separate four-byte update — an implementation that only handled one call would produce
a plausible wrong digest on every real use. `XeCryptSha`, the one-shot, is implemented
too and has not run (gotcha 67).

### The instrument, and why a debugger could not do it

`CZ_DIGEST_PROBE=1` hooks the verify and `SHA1_Final` through the alias seam. It exists
because **gotcha 57 applies to breakpoints, not just to crash dumps**: two attempts to
read the computed digest off the guest stack under `gdb` returned twenty zero bytes and
the assert-REPORTING path's registers, because the compiler keeps `PPCContext` fields in
host registers and `ctx.rN` read in the middle of a recompiled function is stale. At a
function's ENTRY they are fresh, which is exactly what a `PPC_FUNC` hook gets for free.

The probe also recomputes the engine's own string hash (`h = h*0x21 ^ (signed char)c`)
in HOST code, which is what makes it an oracle rather than a description — had the
guest's hash of the same bytes disagreed, the defect would have been in the recompiled
hash function and no amount of reading the table could have said so. Both give
`73E624A9` for `data/audio/Prologue.txt`, which is the table's single entry.

### Measured

```
[digest] verify 'data/audio/Prologue.txt' buffer=A3316830 length=232137
         section='Digest'  host hash=73E624A9
[digest]   SHA1_Final -> 19962B38 5180629C 66FDE711 E72AE257 6586FF23
[digest] verify 'data/audio/Prologue.txt' -> 1
```

That digest is byte-for-byte what the XEX ships, and it is reproducible outside the
runtime: `sha1(file_bytes || be32(len))` over `assets/game/data/audio/Prologue.txt` is
the same twenty bytes. Two witnesses.

| | before | after |
|---|---|---|
| deepest file with `START,A,A` held | **#137 + SIGSEGV** | **#154 `skeleton\childfullbody.big`, zero faults** |

Gates unchanged: `--smoke` OK; A1 exact 84-prefix; A5 exit 0, 2 windows, 0 real;
`truncated=0`; deepest file on a no-input boot still #83 `cinezombie.big`.

### The new frontier

The boot now runs 300 s without faulting and stops loading at **#154**, in a run of
`skeleton\child*.big` files, presenting a black screen. Nothing is crashing and
`truncated=0`; whether that is a stall or simply the next thing to implement is not yet
measured, and it is 71 files past anything this project had reached.


## 12. Finding 51 — the black save panel is a THUMBNAIL, and a real save is rejected

Phase C part 14, closing part 13's last open menu item. `CZ_SAVE_DIR=/tmp/c14_save` with
A3's actual `DR2P000.DSF` (303,104 bytes, from `Xenia logs/A3_save_content/`) laid out
as `<root>/DR2P000.DSF/DR2P000.DSF`, `CZ_FAKE_PRESS_SEQ=START,A`.

**Our content layer enumerates it correctly.** `content enumerator ...: 1 item(s)`,
`enumerate: item 'DR2P000.DSF' (device 1 type 1)`, and
`XMsgCompleteIORequest(result=0)` — the accepted shape gotcha 105 is about, against the
`result=1627 / extended=80070012` that ends the enumeration.

**The title then reaches the save-slot panel and refuses the save.** SLOT 1 is labelled
**`Damaged Content`** and the screen puts up `Load failed! Please check your storage
device and try again.` So the panel this port has been calling "three black rectangles"
since part 12 is the save's THUMBNAIL, and black is the CORRECT picture for a slot the
title has no valid content for — which is what part 13's three hardware watchpoints were
really saying when they recorded zero writes to `0364B000`. Part 12's item closes as a
renderer question.

**What it uncovered is a save-layer gap, and the log names it in one line.** Immediately
after the enumeration completes:

```
[kernel] XexGetProcedureAddress module=30002000 ord=0x271 -> NOT_FOUND
         (not one of the seven A1 resolves)
```

`imports.cpp`'s `kResolvable` is `{ 0xAFF, 0xB00, 0xB0B, 0xB10, 0x305, 0x30B, 0x279 }`,
derived from A1 — and A1 was captured with **no save present**, so it never walked this
path. A3 did, and it resolves an **eighth** ordinal:

```
d> F8000008 XexGetProcedureAddress(30002000, 00000271, 82A5C87C(00000000))
d> F8000008 XexGetProcedureAddress(30002000, 00000279, 82A5C880(00000000))
```

adjacent mint slots, so hardware resolves both. Ours answers `NOT_FOUND` for `0x271` and
the load fails. This is gotcha 45's shape again — a capture-derived list is only as
complete as the path that capture walked — and the reason it stayed invisible for three
sessions is gotcha 106: the A1 gate is run with an EMPTY save root by design, which is
exactly the configuration in which this call never happens.

**Deliberately NOT fixed here.** Minting an honest-failure stub for `0x271` is one line
and would be a guess: what the ordinal IS has not been established, and a stub whose
return value the title consumes rather than tests is gotcha 59/201's trap. The next step
is to name it from the guest's own call site — note `tools/gdis.py --find-uses 0x271`
finds nothing, so the ordinal is not built by a plain `li` and the caller has to be read
from the `XexGetProcedureAddress` call site itself.

There is a second, independent question underneath: A3's save was made under the fork's
profile GUID, and an Xbox 360 save is signed per profile. "Damaged Content" may be the
right answer to THIS file even with `0x271` implemented — so the fix and the test need
separating, and the honest test of the fix is whether the title gets far enough to read
the file at all (our log shows it never opens `save:\DR2P000.DSF`).

---

## 13. Finding 52 — the save could not write, in two independent ways, and neither was visible

Part 16 fixed `XUserWriteAchievements`, part 17 watched the failure MOVE — the mount now
succeeds, `XamContentCreateEx('save','DR2P000.DSF', flags 00001012) -> mounted` — and
recorded the next question honestly as unanswered: the operator's log had the mount and
its `XamContentClose` about ten lines apart with no file activity between them, and
`NtCreateFile` successes print only for the first 512 and then every 64th, so **that
silence was a printer limit, not a fact about the title** (gotcha 109).

The answer did not need a save run. It is in the file layer, and it is two defects that
each independently guarantee the save writes nothing:

1. **`NtCreateFile` ignored `createDisposition` entirely and opened every handle
   `"rb"`.** A3's save open is
   `NtCreateFile(..., access 40100080, ..., share 0, disposition 00000005, options 64)`
   — `FILE_OVERWRITE_IF` with `GENERIC_WRITE`. Our resolver looks the path up with
   `VfsResolveExisting`, which returns empty for a file that does not exist, and the
   function returned `STATUS_NO_SUCH_FILE`. The save's very first call failed.
2. **`NtWriteFile` was a generated honest-failure stub.** It has been in the import
   table since day one and had a stub since day one, and its only trace is one
   `[kcall]` line.

Either alone produces exactly the symptom that was reported, which is why the symptom
could not discriminate. **An import list is not a feature list** (gotcha 67, and
`docs/open-items.md` item 9 is a standing list of this exact mistake): a stub that fails
honestly is still a feature that does not exist, and the honesty is what makes it quiet.

### What the fix is, and it is all derived from A3

`runtime/kernel/file_imports.cpp` now honours the six NT dispositions, derives the
`fopen` mode from the guest's own `desiredAccess` + disposition rather than from the
device name, sets `IO_STATUS_BLOCK.Information` to the right one of
`FILE_CREATED`/`FILE_OPENED`/`FILE_OVERWRITTEN`/`FILE_SUPERSEDED` (a guest that opens
`FILE_OPEN_IF` learns from that field alone which it got), and implements `NtWriteFile`
as the mirror of `NtReadFile`.

Two details that are evidence rather than convention:

* **The read path and the write path use DIFFERENT completion notifications.** Every
  boot-era `NtReadFile` passes `event = 0` and an APC; A3's `NtWriteFile` passes a real
  event (`F80002C8`, created two lines earlier) and no APC. A layer that implemented
  only the notification it had seen would hang the save at precisely the point the
  boot's reads work. Both are signalled, for the same reason they are in `NtReadFile`.
* **The whole save is ONE write.** A3: `length=0004A000` = 303,104 bytes, and the file
  on disk is exactly 303,104 bytes. There is no append, no `NtSetInformationFile`, and
  no `NtFlushBuffersFile` on that path — so `fflush` after the write is not caution, it
  is the only thing standing between the payload and a save that exists until the
  process dies.

### The third defect, which only a test could have found

`CZ_FILE_WRITE_SELFTEST=1` drives create → write → re-open → read → compare through the
real entry points at startup, on its own mounted device in a temporary directory that it
then deletes. It exists because **the feature is otherwise unreachable from here**: the
only thing in this title that writes a file is the save, the save is reached by playing
to a save point, and no headless recipe in this project reaches one. Shipping the write
path without it would have been shipping a prediction (gotcha 67).

Its first run FAILED, and on something neither A3 nor the code review would have shown:

```
NtCreateFile('selftest:\roundtrip.bin') -> handle A0000000, WRITABLE, disposition 5 (created)
NtWriteFile('selftest:\roundtrip.bin', 303104 bytes @ 0) -> 303104 written
NtCreateFile('selftest:\roundtrip.bin') -> not found          <- the file it just wrote
[selftest] FAILED: NtCreateFile(FILE_OPEN) on the file just written
```

**`VfsResolveExisting` caches NEGATIVE results**, and does so deliberately — a title
that probes for optional files would otherwise re-scan a directory on every miss, and
this one probes `game:\data\capcom.txt` at boot. But the create's own existence check is
what caches the "no", so a file this runtime creates is invisible to every later open
for the life of the mount. That is the save's LOAD half, and A3 shows the title probing
`save:\DR2P000.DSF` with `FILE_OPEN` before it ever writes one — so on the real path the
miss is cached before the save even starts.

`VfsForget(path)` drops one entry and `NtCreateFile` calls it when it creates a file.
Mount and unmount already clear the whole map, which is right for a different reason: a
device pointing somewhere new invalidates every path under it, and `save:` is remounted
per content item.

The test passes now, and — this is the part that makes it a test rather than a
decoration — **it has been shown capable of failing** (gotcha 30). It checks five
independent things, including the negative: a write through a read-only handle must
fail, because opening every handle read-only is exactly the defect above and a test that
did not check it would pass on the broken code.

### CONFIRMED END TO END, the same day, by an operator playing to a save point

The title says **"Game saved successfully."** and the slot panel fills in
(`Day 1 - 07:05 AM / Safe House / Find Katey Zombrex`, with a rendered thumbnail).
`~/DR2CZ-troubleshooting/operator-screenshots/2026-08-08_save-succeeded_slot1.png`.

The log is the whole A3 sequence, in A3's order:

```
XamContentCreateEx('save', content 'DR2P000.DSF', flags 00001012, ...) -> mounted
NtCreateFile('save:\DR2P000.DSF') -> handle BC8D8760, WRITABLE, disposition 5 (created),
                                     access 40100080
NtWriteFile('save:\DR2P000.DSF', 303104 bytes @ 0) -> 303104 written
XamContentClose('save') -> unmounted
```

**And the file is cross-checked against hardware, not merely present.** A3 shipped the
real 360 save (`cz_A3_save_DR2P000.zip`), so the two can be compared directly:

| | A3 (hardware) | ours |
|---|---|---|
| size | 303,104 | **303,104** |
| bytes 0..3 | `875f4820` | `89b6c6a0` |
| bytes 4..31 | `0000006d 0000000a 0000000a 01013f00 00000101 01010101 010101 000000` | **identical** |
| first five non-zero regions | `0x0-0x4, 0x7-0x8, 0xb-0xc, 0xf-0x13, 0x16-0x1d` | **identical** |
| final non-zero region | `0x49ffc-0x4a000` | **identical** |
| non-zero bytes | 19,942 | 9,582 |

Only the first four bytes differ, which is a checksum or a timestamp, and the non-zero
byte count differs because A3's save is a played session while ours is a fresh Day 1
07:05 AM game at 0 PP and $2,000. The header layout and the trailing four-byte field
agree exactly. That is a much stronger statement than "a file appeared": the title wrote
a structurally correct save through our file layer.

### What this RETRACTS

**Open-items 1b — "the save fails on one unhandled XAM message,
`[xam] no handler for app FB message 000B0008`" — did not happen.** That message does
not appear anywhere in the successful save run. The message that IS there,
`no handler for app FA message 0007001B`, fires early and did not stop anything. 1b was
measured end to end and its chain (E_FAIL -> overlapped 0x80004005 -> the poll at
825D6094 tearing down) was real when it was written; it was fixed by part 16's
`XUserWriteAchievements` work, and the item outlived the defect. A finding with a
complete causal chain can still be about a path the title no longer takes.

### What is still NOT known: the LOAD half

This run saved and did not re-launch, so nothing has yet read the file back. That is now
a much better test than it was, because **the save on disk is OURS** rather than A3's —
and A3's was made under the fork's profile GUID, which is the confound that made
open-items 2's `Damaged Content` ambiguous (a 360 save is signed per profile, so
"Damaged Content" may have been the right answer to that file). One relaunch and a Load
Game answers both halves at once, and the XAM ordinal `0x271` that item names should
appear in the log when it does.
