# Part 30 hand-off (for part 31). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part29-kickoff.md` for "where the
port is". `d3d-phase-c28-kickoff.md` remains the record of how the white-surface chain was
built, but **its item 0 is answered and two of its eight steps are now wrong** — read the
section below before following anything in it.

**Check the git log against this file before working an item** — gotcha 13, and it has
cost this project a session three times.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays its cinematics through. Part 30
spent itself on the white-surface defect and on one owed measurement. It did not fix the
white surfaces. What it did was **read the constants** — the eight-step chain part 27 built
had never looked at a single value in the tone curve it was reasoning about — and the
numbers retire two of its steps, confirm a third from an independent direction, and leave
the item pointed at 23 shader constants nobody has compared.

## WHAT PART 30 DID — do not rebuild any of this

Full record: `docs/phase5-notes.md` §6ba, and `open-items.md` 00f (retracted in place).

* **`tools/xtr_draw_constants.py`** — hardware's pixel-shader ALU constants for a named
  draw, with per-register provenance (`set` / `unset` / `UNRECOVERABLE`). Part 27 asked
  ONE of the seven captures, got `UNRECOVERABLE`, and recorded it as needing a new
  capture. Five of the other six answer, from data on disk for weeks (gotcha 274).
* **The tone curve every one of the 48 emitting shaders ends in, with numbers.** With
  `x = colour * pc(14).w`: `out^2 = (max(0.25x + 0.75, 1.0) - saturate(1-x)^2) * 0.5`,
  which is 0 at `x=0`, exactly **180** at `x=1`, and 255 only at `x=5`.
* **Our constants are hardware's, to the digit** — a pre-registered prediction that they
  were not, tested and refuted. The literal pool is PER SHADER and ours carries each
  shader's own: `ps_ad65b98593f95926` reads the same four numbers one register lower than
  `ps_7d2f8f33deec1b65` does.
* **Our translation of `ps_ad65b98593f95926` is instruction-for-instruction identical to
  the capture's own disassembly of it**, including all six `_sat` modifiers. That was
  part 29's named next step. The emitter is exonerated and the clamp is on an INPUT.
* **A real runtime defect fixed:** since phase A/V, every headless run WITH SOUND ignored
  `timeout`. `Host_WindowInit` sets `SDL_HINT_NO_SIGNAL_HANDLERS` below the
  `CZ_NO_WINDOW` early return, and the audio device is a second, independent SDL entry
  point. Exit 124 at 20 s with `CZ_NO_AUDIO_OUT=1`, still alive at 180 s without it.
* **`CZ_VK_PS_CONST_SCALE="14.w=4"`** — the arm part 31 needs, built, documented and shown
  to engage (3,391,676 draws scaled in a 70 s run; a deliberately malformed clause is
  named rather than dropped).
* **THE XMA DECODER COSTS NO FRAME TIME — part 29's item 0b is closed.** Three runs an
  arm, alternated, decoder shown to engage on the route first, null measured within the
  control arm. Every bin in both arms medians **32.0 ms**; largest mean difference 0.2%
  against a 0.6% null; the `>33 ms` share is 1.05-1.12% with the decoder and 0.99-1.17%
  without, so the arms overlap completely. **Quote it with its bound**: the workload is
  pinned at the two-vblank floor in both arms, so this says the decoder does not push
  frames off the cap, not that it is free in absolute CPU terms. A frame rate from a build
  with audio in it no longer needs qualifying. `docs/phase-av-notes.md`, part 30 section.

## READ THIS BEFORE MEASURING ANYTHING

Everything from parts 26-29's lists stands. Part 30 adds three, all in `docs/gotchas.md`:

* **272 — a process-wide policy set beside one subsystem's first use of a library stops
  holding when a second subsystem gains its own entry point.** The symptom here was a
  longer SUCCESSFUL run, which nothing reports: no error, no log line, no non-zero exit.
* **273 — a threshold probe cannot tell "below" from "equal", and a transfer function's
  flat spot makes a varying input look like a constant.** Both bit the same defect.
  `XE_FLOOR_PAINT` asked whether `0.25x + 0.75` falls below 1.0, which it can only do for
  `x < 1`, and the surfaces under test sit at `x = 1`. Separately, `d(out)/dx` vanishes
  there, so a 10% spread in the colour quantises to ONE 8-bit value — 52,840 pixels at
  exactly `rgb(180,180,180)` does not show that the surface is unshaded.
* **274 — an oracle that cannot answer is a fact about that member, not the population.**
  Ask the other captures before asking for a new capture.

And one that is not a gotcha but will waste an hour if forgotten: **the literal pool is
per shader.** A slot number lifted from one shader's disassembly and looked up in
another's constants produces a tone map that outputs black, convincingly.

## WHERE TO START

1. **THE WHITE SURFACES, and the question is now specific.** The tone curve is correct on
   both sides and the translation is exact, so the defect is in the colour arriving at the
   epilogue: these surfaces sit at full exposure **in a pitch-black room**, unmodulated by
   lighting or time of day. Two steps, in order:

   a. **Run the arm that was built for this.** `CZ_VK_PS_CONST_SCALE="14.w=4"` moves the
      surfaces to a part of the curve whose derivative does not vanish. A plateau that
      stays a single spike under 4x is a pinned colour; one that spreads was a shaded
      surface the curve was hiding. Read it off a scene-buffer snapshot
      (`CZ_VK_SNAP_DUMP` + `CZ_VK_SNAP_FRAME`, both headless), not off the presented
      frame. **State which outcome you expect before running it.**

   b. **Compare the 23 unchecked constants.** The ground shader reads 32; nine have been
      compared. `c28..c39` is a twelve-register block with the shape of a light array,
      `c40/c41/c42` are the `dp4` rows of a shadow projection, and `c23`/`c27`/`c67`
      multiply the term the fog LERP then consumes. Since the vertex data and all three
      DXT1 textures already match hardware bit for bit, and DXT1 cannot carry a value
      above 1 on either side, hardware's extra range has to arrive through a constant.
      One run of `CZ_VK_PSBIND_PC` against one of `tools/xtr_draw_constants.py`, at a
      matched location — the operator's route reaches `w1_spawn`.

2. The rest of `docs/open-items.md`, and `docs/perf-cpu-plan.md`'s largest item — the
   CPU/GPU overlap work (gotcha 231), still the biggest performance term.

## Gates, on this binary

* `--smoke` OK. The recompilation is untouched (nothing outside `runtime/gpu`,
  `runtime/host` and `tools/` changed).
* `timeout` verified working again in BOTH arms, by the two-arm test that exposed the
  defect.
* `tools/shader_dim_census.py` exit 0 — the ucode parse and the translated SPIR-V agree on
  every shader, 97 cube modules, the one known sidecar without `tfetchDims`
  (`ps_926c15dd20571cf1`, microcode lost) still the only one.
* Six 420 s renderer runs presented 13,051-13,057 frames each and reached 7,947-9,476
  draws, so the renderer is healthy on the outdoor route on this binary.

**Not re-run and owed before any claim resting on them**: the A5 kernel-call diff,
`truncated=0`, the two PM4 capture oracles, the capture-E picture correlation, the Vulkan
validation tally, and the shader-cache name diff. Nothing this part touches the PM4
executor or the import set, but that is an argument and not a gate.
