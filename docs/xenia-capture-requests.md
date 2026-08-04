# Xenia ground-truth captures needed for Case Zero — request list (round 1)

Written 2026-08-04, before any runtime work. Modelled on what the Fable 2 and Asura's
Wrath ports actually consumed. **Nothing has been captured yet.**

Priorities are ordered: **A is enough to start the runtime; B–C unlock the renderer and
the differential debugging that carried both earlier ports through their hardest bugs.**

Everything lands in `Xenia logs/` (gitignored), with one entry per run appended to
`Xenia logs/Xenia_Run_Content.md`: what you did, in order, plus anything unusual
(crashes, missing graphics in Xenia itself, skips). That index mattered constantly on the
earlier ports — a log is only as useful as knowing what the player was doing.

## General rules for all captures

- Keep the **config dump header** Xenia prints at the top of its log — never trim it.
- Note the **Xenia build** (the `Build: ...` line) in the index.
- Vanilla settings unless stated. If you changed anything in `xenia.config.toml`, copy
  the toml next to the log.
- Big logs are fine (Fable 2's Run2 was 1.27 GB). Compress with zstd/7z if convenient.
- **Launch the STFS package directly** (the file under `58410A8D/000D0000/`), not an
  extracted `default.xex` — the content-mount calls at boot differ, and the package is
  what the console sees. Note in the index which you used.

### Two rules learned the hard way on Asura's Wrath — read before capturing

1. **Only `--log_level=3` names kernel calls.** At level 2 Xenia prints handle churn and
   file paths but *no HLE call names at all*. A level-2 log is not a smaller level-3 log;
   it is a different, much less useful thing.
2. **`log_level=3` is still not the whole kernel surface.** Exports tagged
   `kHighFrequency` are logged only with `log_high_frequency_kernel_calls = true`, which
   defaults **off** — on Asura's Wrath that hid 40 of 288 imports, including most of the
   synchronisation surface and `VdSwap`. Diff against a capture without knowing this and
   you will chase divergences that do not exist. It also *deadlocked* that title (the
   cvar puts a disk write inside the lock paths), so treat it as a separate, possibly
   short capture rather than the default.

---

## A. Kernel-call text logs (Canary is fine — this is the priority)

Ground truth for boot order, kernel HLE return values, file I/O, thread creation and
content mounting. Both earlier runtimes were written against these from day one.

```
xenia.exe --log_file=C:\xenia_logs\cz_runN.log --log_level=3 "path\to\58410A8D\000D0000\3A98C6..."
```

### A1 — SHORT boot at maximum verbosity (the single most important capture)
Boot → any logos/intro (let them play, do **not** skip) → title/menu → sit ~60 s → quit
cleanly. This becomes `docs/xenia-boot-flow.md`, the sequence our runtime must reproduce.

### A2 — Into gameplay
Boot → start a new game → play the opening ~5 minutes of Still Creek (get outside, fight
a few zombies, pick up a weapon) → quit. Adds the gameplay-era kernel surface: streaming
loads out of the `.big` archives, physics/audio thread creation.

### A3 — Save / content
Boot → new game → reach the first save point → save → return to the menu → load that save
→ quit. Case Zero saves through `XamContentCreateEx`; Asura's Wrath needed
`savedrive:` → `\Device\Content\N\` and a plain file write would not have worked.
This capture is what tells us the exact shape.

### A4 — Idle at the title screen, long
Boot → title screen → leave it alone for ~5 minutes → quit. A quiet log makes the
per-frame steady state legible against A1's noisy boot.

### A5 — High-frequency kernel calls (separate, may fail)
A1's drive, with `log_high_frequency_kernel_calls = true`. Expect it to be huge and
expect it possibly to hang — if it does, say so in the index and capture as far as it
got. Even a boot-only prefix is valuable: it is the only view of the synchronisation
surface and `VdSwap`.

---

## B. GPU command-stream traces (`.xtr`)

The renderer is written against these. Stock Canary strips the trace writer in Release;
Asura's Wrath used a locally instrumented Canary fork to force it on. If you still have
that fork, use it.

### B1 — Boot → title screen
Same drive as A1. Gives the draw profile per frame, the shader set, the EDRAM layout and
the swap cadence for everything up to the menu.

### B2 — Gameplay
Same drive as A2. Expect an order of magnitude more draws per frame.

**If you can, take a same-run `--log_level=3` log alongside each `.xtr`** so a frame index
can be tied to a file open. Asura's Wrath's E1/E1b did this and it was worth more than
either capture alone.

---

## C. Function-coverage traces (`--trace_function_data`)

Stock Canary does **not** strip this (an assumption in Asura's Wrath's round-1 request
turned out to be wrong, in our favour).

```
xenia.exe --trace_function_data=true --log_file=... "path\to\package"
```

### C1 — Boot → title, C2 — gameplay

This is a **two-sided** oracle and most ports use only one side:

- **Forwards** — *hardware ran an address we have no function for* — recovers missing
  entry points for `config/CaseZero.toml`'s `functions` list (vtable slots and
  runtime-built function-pointer tables that no `bl` points at). Asura's Wrath recovered
  215 this way.
- **Backwards** — *we ran a function hardware never ran* — localises a control-flow
  divergence to a single function, with no debugger and no reproduction.

Treat the capture's function boundaries as **ranges, never identities**: the emulator's
function analysis will not agree with the recompiler's, and 4-byte single-instruction
functions are not comparable at all. Classify by function size before believing any
"divergence".

---

## D. Screenshots

A handful of PNGs at known points — first logo, title screen, first gameplay frame, a
zombie crowd — as the visual target for the renderer. Note the frame index if you can.

---

## Not requested (and why)

- **Audio captures.** The XMA surface is small here and both template ports settled it
  from the kernel logs; if the audio work stalls we will ask for a targeted capture then,
  with a specific question. A capture request without a question it can answer is how
  Asura's Wrath produced two rounds of requests it later had to retract (its gotcha #18:
  *a capture request is a hypothesis with a shelf life*).
