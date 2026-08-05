# Phase 3 kickoff — paste this into a fresh conversation

Hand-off prompt for the next session. `CLAUDE.md` loads automatically and is current
(phase 0 complete; phase 1 complete bar the items listed below; 2026-08-05). This file
adds only what a fresh context needs to start **phase 3 — window, present seam, input**
without re-deriving anything.

---

## The task

Phase 3 per `docs/runtime-plan.md`: an SDL window, the present seam every later stage
goes through, and real keyboard + gamepad input.

It is listed as "cheap" in the plan and that is still true — but it is no longer just
plumbing, because of what finding 37 established: **the boot reaches the title screen
and waits for a human.** Phase 3 is what lets it move.

## Read before writing code

1. `docs/phase1-notes.md` — findings 36-41, and §5 "where the boot currently stops".
   Finding 37 is the one that defines this phase's gate.
2. `docs/xenia-capture-analysis.md` — the numbered findings ledger; it wins every
   disagreement with any other doc.
3. `Xenia logs/Xenia_Run_Content.md` — what each capture is. For this phase the useful
   ones are **A5** (high-frequency; the only capture that can see input polling — see
   the trap below), **A4** (a five-minute title-screen idle, i.e. exactly the state our
   boot parks in), and **E** (five screenshots, the visual target).

## What is ALREADY DONE — do not rewrite these

This is the important half of this document. Three pieces of phase 3 already exist
because earlier phases needed them, and a session that starts from the plan text alone
will rebuild all three.

**1. All four input imports are implemented, and none of them is a stub.**
`XamInputGetState`, `XamInputSetState`, `XamInputGetCapabilities`,
`XamInputGetKeystrokeEx` — `runtime/kernel/imports.cpp` around line 3350-3540. The
image declares exactly these four; there is no fifth. So the input job is **not writing
the imports, it is feeding real device state into `XamInputGetState_x`**, which today
returns "a connected pad with nothing pressed, forever".

Three details already measured, worth keeping:
- The **packet number** contract: XInput changes it only when the state changes, and a
  title is entitled to skip re-reading when it has not. A constant packet number with a
  changing button field hands the guest a press it may legitimately ignore.
- `XamInputSetState`'s vibration struct is **byte-swapped** relative to the obvious
  reading (`lhz r11,2(r31)` stored at +0) — recovered from the guest, not guessed.
- `XamInputGetCapabilities` is polled for user 0 **and** user 1 (1,108 times each in a
  boot), which is how the title discovers there is one pad.

**2. The present seam's data already flows; only the presenting is missing.**
`VdSwap` (`runtime/gpu/vd.cpp`) emits, per frame:
- the front-buffer **texture fetch constant** — type-0 register `0x4800`, six dwords,
  copied through from the guest verbatim, because those dwords encode the front
  buffer's address, tiling and format and re-deriving them would be asserting a surface
  layout we have not measured;
- an `XE_SWAP` packet (`0xC0036400`, `'SWAP'`, frontBuffer, width, height).

`runtime/gpu/pm4.cpp` case `0x64` receives it and currently only increments a frame
counter. **That case is where a present hooks in** — the address, dimensions and format
are all already in hand there.

**3. The frame clock is real and verified.** Exactly one `XE_SWAP` per frame, checked
against B1 (1,089 swaps over 1,089 frames). A 120 s run currently does ~3,771 of them.

Also note: **phase 4's command processor is already live**, well ahead of the plan's
ordering — 122 M packets a run, zero unknown opcodes, `truncated=0`. Do not read the
plan's phase numbering as the state of the code.

## The gate, and the crutch it retires

`CZ_FAKE_START_MS` synthesises a START press. It is a **measurement arm that fabricates
evidence**, it announces itself on every press, and it must never be on for a gate run
(gotcha 78). It exists because finding 37 needed to answer a question nothing else
could: a boot that has finished and is waiting for a human looks *identical from
outside* to one that has deadlocked — same frame rate, same file count, same kernel-call
profile (gotcha 77).

So phase 3's gate writes itself:

> **The A1 gate advances from position 84 to 85 (`XamShowDeviceSelectorUI`) on a real
> key or button press, with `CZ_FAKE_START_MS` unset.**

That is falsifiable, it is one command, and passing it means the arm is no longer the
only way to move the boot. Keep the arm afterwards — it becomes the control for "was it
really my input that did that".

Secondary gates, all of which must still hold:
```
./runtime/build/cz_runtime --smoke                    # the phase 0.2 link gate
kernel_call_diff --xenia A5 --include-high-frequency  # exit 0, all permutations
kernel_call_diff --xenia A1                           # >= 84-deep prefix
ring: indirect buffers truncated=0
```
Gate at **90-150 s, not 30 s** (gotcha 75), and remember reaching a given position in
fixed wall time is a distribution, not a fact — position 84 lands in roughly 5 runs of 7.

**A blank window is the expected result of this phase, not a failure.** There is no
renderer yet (phase 5). Success is: the window opens, it presents at the swap rate, and
input moves the boot. Say so in the write-up, or the next reader will hunt a renderer
bug that does not exist.

## The trap this phase will walk into

**`XamInputGetState` is `kHighFrequency`, so A1 is the wrong oracle for input.**

| capture | `XamInputGetState` | `XamInputGetCapabilities` |
|---|---|---|
| A1 (plain L3) | **1** | 5,503 |
| A5 (high-frequency) | **12,365** | 15,452 |

Read A1 alone and the obvious conclusion is that this title barely polls the pad and
discovers input some other way. It is wrong, and it is the same shape as gotcha 47 (the
`NtReadFile` that only A5 can see) and gotcha 45 (A5 is *not* a superset of A1 — 11
names appear only in A1, so check both).

For polling behaviour at the menu specifically — which is the state our boot actually
sits in — **A4 is the capture**, a five-minute title-screen idle.

## Where the boot is when you start

- Reaches the title screen: 64 files opened through to `prologue_menu\prologue_z01.big`,
  rendering ~1,982 draws/frame against A1's title-screen ~2,540, ~32 fps.
- A1 gate: exact 84-deep prefix of Xenia's 93. Position 85 needs input.
- A5 gate: position 119, its last, zero real mismatch windows.
- 155 of 244 imports real; 89 generated honest-failure stubs.
- Stability: 0 crashes in 20 runs at 120 s; 0 truncated indirect buffers.
- Host CPU 121% since finding 41.

## Also outstanding (do not let these silently become phase 3's job)

1. **Unexercised imports** (gotcha 67 — implemented is a prediction, not a result).
   Finding 34's eight have never run, `XamTaskSchedule` in particular (it runs guest
   code on a new thread). Of finding 36's seven, five run and two do not — both
   teardown paths. Some of these will get exercised for free once input lets the boot
   go further, which is an argument for doing phase 3 first, not for merging them.
2. **The save-data layer** — `XamContentCreateEnumerator`, `XamEnumerate`,
   `XamGetPrivateEnumStructureFromHandle`, `XamContentCreateEx`, `XamContentClose`.
   A1 position 86 wants these. Deliberately deferred: they are the phase 8 file layer.

## For phase 5 later, so it is not re-derived

The renderer's inputs are already in hand and the disc shader banks are a dead end
(finding 6, retracted in place). Xenia's `dump_shaders` gave **455 raw Xenos microcode
blobs** — 120 frontend/menu from A1, 335 gameplay from A2 — which is exactly what
XenosRecomp consumes. And the GPU gate must key on **per-era aggregates, never absolute
frame index**: two *hardware* runs of the same drive agree frame-exactly only 80.0% of
the time, so a frame-indexed gate would report ~20% divergence against a correct
renderer (finding 10, gotcha 38). Noise floor: 0.42% worst aggregate, 0.19% on draws.

## Standing constraints

- Commit proactively; end messages with
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- No copyright/license headers in new files (ask first).
- Document in `docs/` for an outside reader — **Dead Rising 2: Case West is the next
  port and will lift these documents**. Say what the idiom was, not just what changed.
- Retract in place when a finding turns out to be an artifact.
- UnleashedRecomp is GPLv3 → structural reference only. Guest structs come from
  XenonRecomp's `XenonUtils/xbox.h` (MIT).
- Captures run on the operator's Windows machine and are **never self-servable**. There
  is currently no outstanding capture request; A1-A5/B/C/E should cover this phase.
- Measurement discipline: same-binary A/B arms, a rate rather than a run against
  anything intermittent, and the control for "did my change do this" is the old binary
  run **now** (gotchas 50, 51, 86, 95).
- Rebuild: `cmake --build runtime/build -j$(nproc)`; `python3 tools/gen_import_stubs.py`
  after any change to the import set. If the *function list* ever changes, the
  five-tool pipeline order in `CLAUDE.md` applies in full.
