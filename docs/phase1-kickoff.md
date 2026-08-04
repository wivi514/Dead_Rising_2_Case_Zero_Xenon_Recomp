# Phase 1 kickoff — paste this into a fresh conversation

This is the hand-off prompt for the next session. `CLAUDE.md` loads automatically and is
current (phase 0 complete, 2026-08-04); this file adds only the ordering, the gates, and
the pointers a fresh context needs to start phase 1 without re-deriving anything.

---

## The task

Start **phase 1: kernel HLE and guest bootstrap** per `docs/runtime-plan.md`, written
against the A1 capture's call order. Deliverable: the recompiled image boots under our
runtime far enough that our kernel-call sequence is a **prefix-match of A1's out to the
title screen** — that prefix growing is the progress metric, reaching the title screen is
the gate.

## Read before writing code

1. `docs/runtime-plan.md` — phase 1 section, including the three non-negotiable rules.
2. `docs/xenia-capture-analysis.md` — the findings ledger; it wins every disagreement.
3. `Xenia logs/Xenia_Run_Content.md` — what each capture is. A1 is the boot→title L3
   log; A5 is the same drive with high-frequency logging (`NtReadFile` is invisible in
   A1 — finding 2). Remember gotcha 24: Xenia logs at `d>` `i>` `G>` `A>` etc., and a
   `d>`-only filter silently loses `VdSwap`.
4. `~/GithubRepo/Fable2XenonRecomp/docs/runtime.md` — the deepest runtime write-up; the
   module structure to port.
5. `~/GithubRepo/Asuras_Wrath_Xenon_Recomp/runtime/` + its `tools/kernel_call_diff.py` —
   the second application of the same plan, and the gate tool to port.

Licensing: UnleashedRecomp is GPLv3 → **structural reference only**. Guest structs come
from XenonRecomp's `XenonUtils/xbox.h` (MIT).

## Order of work

**1. Honest-failure stubs (first job, before anything else).**
Convert `tools/gen_import_stubs.py` from abort-stubs to honest-failure returns
(`STATUS_NOT_IMPLEMENTED`), each call logged in a diffable format. The phase 1 gate is
the call *sequence*; an abort on the first unimplemented name makes the ordering
unobservable. Rules that are not negotiable:
- Never fake success (Fable 2's XMA context bug cost weeks).
- A stub that returns an error but leaves its **out-parameter** untouched is worse than
  no stub — the guest often ignores the status and reads the buffer anyway. Zero-fill
  out-params.
- The generator already scans the runtime tree for real implementations
  (`GUEST_FUNCTION_HOOK` / `PPC_FUNC(__imp__...)` / `STUB_RET`) and stubs only the rest;
  keep implementing imports by adding real definitions and re-running it.

**2. Guest memory map + o1heap arenas.**
Put the arenas where the console puts them (4 KB vs 64 KB page regions selected via
`MEM_LARGE_PAGES`), and **round every size the kernel reports the way the console rounds
it** — the guest heap manager builds its map from those numbers (gotcha 9). Matching the
console's map also makes our addresses directly comparable to the A-series captures.
Port the arena layout from Fable 2/Asura's Wrath, then check it against what A1 shows
this title actually asking for.

**3. Image load + guest thread bootstrap.**
Map the image at `0x82000000` (load `assets/game/default_image.bin`, or reuse the
XenonUtils loader path `tools/xex_image_dump` already proved on this LZX/devkit-key XEX).
PPCContext setup, TLS, guest stack, then enter `_xstart` / entry `0x825D9F30` through the
`PPCFuncMappings` table. Timebase is already solved (`runtime/cpu/timebase.h`, force-
included over `ppc/` only).

**4. Kernel HLE, in A1's order.**
Implement imports in the order A1 shows the title calling them — not in the order another
port needed them (gotcha 10: the image is the authority on what this title imports; A1 is
the authority on when). Threads, events, semaphores, NT timers, APCs, XAM stubs, and the
VFS over `assets/game/` (the `.big` format is cracked — `docs/big-archive-format.md`;
paths are constructed at runtime, so arbitrary-path lookup, random access within
archives). The likely `.big` reader lives at `0x82764CF8`–`0x82769338` (the `lhbrx`
cluster, finding 14) — useful for hooks/tracing, don't spend time confirming it yet.

**5. The gate: port `kernel_call_diff.py`.**
From `~/GithubRepo/Asuras_Wrath_Xenon_Recomp/tools/kernel_call_diff.py`, adapted to this
runtime's log format and A1's line shapes. Run it early and often — the diverging call is
always the next thing to implement. Measurement discipline: same-binary A/B arms, no
silent caps, every probe needs its own control.

## The `.xtr` decoder track is DONE — do not re-derive it

Completed 2026-08-04 (phase 0.3), before this hand-off. `tools/xtr.py` is the format in
one module; `xtr_walk.py`, `xtr_pm4_census.py` and `xtr_determinism.py` are thin CLIs over
it. Read `docs/xtr-decoder.md` when phase 4 starts, not before. What it settled:

- Finding 10 is closed. B1/B1b are content-deterministic to **0.42%** over the boot+movie
  prefix, but only **80.0% frame-exact** — so **phase 4 gates on per-era aggregates, never
  on frame index**.
- `INDIRECT_BUFFER` is recorded one dword short, and start/end nesting is unbalanced at
  the tail of every capture. Both are replay traps, written up in finding 10b/10c.
- All three GPU captures are intact; all 21 type-3 opcodes in B1 are named.

## Standing constraints

- Commit proactively; end messages with
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- No copyright/license headers in new files.
- Document in `docs/` for an outside reader — Case West is next and will lift these.
- Retract in place when a finding turns out to be an artifact.
- Captures run on the operator's Windows machine — never self-servable; there is
  currently **no outstanding capture request**, and A1–A5/B/C/E should answer phase 1's
  questions without a new one.
- Rebuild: `cmake --build runtime/build -j$(nproc)`; regenerate stubs with
  `python3 tools/gen_import_stubs.py` after any change to the import set; if the
  function list ever changes, the four-tool pipeline order in `CLAUDE.md` applies.
