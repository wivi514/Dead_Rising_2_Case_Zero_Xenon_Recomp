# Phase A/V hand-off (for the next part). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c28-kickoff.md` for
"where the port is"; that file is still the authority on the WHITE-SURFACE chain, which
this part did not touch and which is still the top rendering defect.

**Check the git log against this file before working an item** — gotcha 13, and it has
cost this project a session three times. This part is itself a worked example: it closed
an item (`open-items.md` 1d) whose text said, in bold, that audio had been "retired with
arms and not to be re-bought".

## The one-paragraph state of the port

The game boots, renders, plays, **and now makes sound** — operator-confirmed by ear.
Phase A/V wired ffmpeg's XMA2 decoder to the title's own hardware context array and an SDL
device to its mixer, and the same fix unfroze the prologue cinematic, the port's oldest
open blocker, open since part 15.

**"Unfroze" is the exact word and not a synonym for "fixed".** The prologue no longer sits
black on one camera for 10,513 frames; it now RENDERS, and it ping-pongs — forward ~1 s,
backward ~1 s, forever, from the moment a character speaks. That is a new defect
(`open-items.md` 00j) which only exists because there is now audio, it is item 0 below,
and three of its candidate explanations are already eliminated. The renderer's
white-surface defect is untouched and remains the top RENDERING item.

## WHAT PHASE A/V DID — do not rebuild any of this

Full record: `docs/phase-av-notes.md`. Short form:

* **Sound works, and the OPERATOR HAS CONFIRMED IT BY EAR.**
  `runtime/audio/xma_decoder.cpp` (ffmpeg `AV_CODEC_ID_XMA2`, lifted from the Fable 2
  port), `runtime/audio/audio_out.cpp` (SDL), and the context walk in
  `kernel/audio.cpp`. `maxpeak` 0.000000 -> 0.108854; non-silent 0 -> 15,991 of 18,433.
  **If a future session hears nothing, check the MIXER before the code**: the first
  operator test reported silence and the cause was pipewire holding the `cz_runtime`
  sink input at `Mute: yes`, at 100% volume on the correct sink, because wireplumber
  remembers per-application state and this application had never made a sound before.
  `pactl list sink-inputs` is the one-line check.
* **The defect underneath was one address.** XMA context buffer pointers are PHYSICAL
  (the APU is a DMA device); our flat map puts the physical arena in a window at
  0xA0000000. Reading them literally gave a page of zeros, which decodes to silence and
  reproduces the symptom exactly. Gotcha 267 — **a Case West item on day one**.
* **The prologue cinematic was waiting on that silence.** Same binary, one variable
  (`CZ_NO_XMA_DECODE`): longest frozen camera run 10,513 -> 159 frames, presented
  coverage 15.00% -> 99.94%.
* **New arms:** `CZ_NO_XMA_DECODE=1`, `CZ_NO_AUDIO_OUT=1`, `CZ_XMA_DECODE_LOG=1`, and
  `CZ_XMA_PROBE` widened to all 16 context dwords plus a buffer CONTENT scan.
* **Gotchas 267, 268 and 269** — the physical-address trap, "your own stub is an oracle",
  and "a probe that reports from inside the function it counts goes silent exactly when
  the interesting thing happens". The last one bit on the very run that produced it.
* **`CZ_CINE_PROBE=1`** — counts the cinematic sync-point predicate and its containing
  tick, with the positive control on the same line.

## READ THIS BEFORE MEASURING ANYTHING

Everything from parts 26-28's lists still stands. Phase A/V adds two:

* **YOUR OWN STUB IS AN ORACLE, AND IT REFUTES LESS THAN YOU THINK** (268). Part 16 ran
  `CZ_XMA_NULL_DECODER` in three configurations of one binary against the frozen prologue
  and recorded "refuted, not merely unconfirmed". It was a true hypothesis. A null
  implementation reaches only the states its author believed were load-bearing — here the
  predicate the title polls, and nothing downstream of PCM existing. **When a fake arm
  returns a negative, write down what it cannot do next to the conclusion.**
* **PRINT THE DESTINATION, NOT JUST THE COUNT** (267). A read that reports the right byte
  count into the wrong place is indistinguishable from a correct one. One `%08X` on the
  `NtReadFile` trace is what closed a defect that had survived 28 parts.

## Gates, on this binary — all re-run this part

* `--smoke` OK.
* **A5 kernel-call diff: exit 0, 3 permutation windows, 0 real.**
* Headless gameplay recipe reaches **file #185**, 200/200 distinct cameras in the last 200
  frames, max 3,162 draws.
* `no translated shader` = **0**.
* Shader cache name diff: only `ps_926c15dd20571cf1` differs, which is the known
  lost-microcode entry. `tools/shader_dim_census.py` exit 0, all 411.
* **The cache did NOT grow this part** — 411 before and after, including a run in which
  the prologue cinematic RENDERED for the first time. That is a real (small) finding:
  the newly-visible era binds shaders we already had.

**Not re-run and owed before any claim resting on them**: `truncated=0`, the two PM4
capture oracles, the capture-E picture correlation, and the Vulkan validation tally.
Nothing in this part touches `gpu/`, so all four are almost certainly clean — but that is
an argument, not a gate.

## WHERE TO START

0. **THE CINEMATIC PING-PONG — `open-items.md` 00j, and it is the live item.** Operator
   report on this binary: the prologue cinematic plays, and the moment a character speaks
   the scene runs forward ~1 s, BACKWARD ~1 s, and repeats forever. It reproduces with no
   operator and the gate is free — `runs / distinct` on the `cameraFingerprint` column,
   6.13 against a healthy 1.09, with `runs / frames` beside it to tell "looping" from
   "frozen" (see the item; reading one without the other scores a frozen camera as
   healthy). **`distinct` reads 1170/1170/1169/1173 across four runs on two binaries** —
   the same fixed pose set every time, so this is deterministic and cheap to test.

   **Three explanations are already eliminated, each with a measurement. Do not re-buy
   them:** our output-ring `write == read` ambiguity (built, predicted, run, refuted, and
   reverted — `distinct` did not move by one); the audio stopping (proved DOWNSTREAM, the
   stall precedes the silence by ~5.5 s); and the "end sync point" wait the engine itself
   narrates (counted with `CZ_CINE_PROBE=1` — ten entries in a 400 s run, an even
   received/not-received split, and the containing function stops being called entirely
   while the loop continues).

   **What survives:** the title's own `Cine.Audio` PID controller (`P-gain`/`I-gain`/
   `D-gain`/`MV`/`Cor Latency`, all in `cinematicmanager.cpp`'s region) is real and
   unevidenced — nothing shows it is even running. **The honest next move is not another
   consumer-side guess: the palindrome means some clock DECREMENTS, so find what writes
   the cinematic's time each frame.** That is a findable value, not an inference from a
   string table.

0b. **THE OTHER TWO OPERATOR QUESTIONS, still unanswered.**
   * **Do the cinematics all play through now?** Phase A/V measured the PROLOGUE, which
     now loops rather than freezing. `open-items.md` 1's retraction says Katey Zombrex,
     the bike-frame delivery and the combo weapon already completed on the part-19
     binary. **The combo weapon being AWARDED without skipping is the cheapest single
     check**, and it is now more interesting than before, because a cinematic that
     completes would show the ping-pong is prologue-specific.
   * **Is the frame rate unchanged?** The decoder is a 1 ms poll thread doing real DSP on
     a machine whose crowd frames are already CPU-bound. Still unmeasured;
     `CZ_NO_XMA_DECODE=1` is the arm. See item 2.
1. **THE WHITE SURFACES — still the top rendering defect and untouched by this part.**
   `d3d-phase-c28-kickoff.md`'s eight-step chain is the live state, and its item 0 is the
   next instruction: what pins `c` at `1/pc(14).w`. Read
   `d3d-phase-c28-kickoff.md` for that whole section; nothing in it has moved.
   Note the knock-on: item **00i (LOD pops in late) is parked BEHIND this**, because the
   oracle comparison is confounded while world surfaces are flat grey.
2. **THE FRAME-TIME COST OF THE DECODER, unmeasured.** One 1 ms poll thread, up to a
   handful of libavcodec decodes per tick, on a title whose crowd frames are 75% CPU. The
   arm exists (`CZ_NO_XMA_DECODE=1`) and the method is fixed: `tools/frame_perf_bins.py`,
   three runs an arm, alternated, null comparison first, and **read medians and the
   pinned-to-16 ms share, not means** (gotchas 229, 237). Budget an hour. Do this before
   anyone quotes a frame rate from a build with audio in it.
3. **A3 from the plan, still open**: `AudioEventName = "sync:39791"` — whether the
   cinematic audio event RESOLVES is untested. Cinematics advance now, so this is no
   longer blocking; it is worth one `tools/gdis.py` pass only if a specific cinematic
   misbehaves.
4. **`starve1-2` per context per 5 s.** Small, non-zero, and means the guest had ring
   space while we produced nothing that tick. Attributable rather than mysterious, which
   is why the counter is there. Not worth chasing unless the operator reports dropouts.
5. The rest of `docs/open-items.md`, and `docs/perf-cpu-plan.md`'s largest item — the
   CPU/GPU overlap work (gotcha 231) — which is still the biggest performance term.

## What is worth knowing that is not in a commit message

* **The cinematic system is mapped now, statically, and none of it was needed.** If a
  cinematic defect returns, this saves the first hour: `cinematicmanager.cpp` is named at
  `0x82063030`; `cine_anim_over` (`0x82076C44`) is compared at `0x824E3630` in an
  animation-event dispatcher; `CineStart`/`CineFinish` intern to `0x82A58F34`/`0x82A58F38`
  at `0x829A072C`/`0x829A0764`.
* **Four cinematic debug flags exist and every one has a reader.** Bound BY DATAFLOW, not
  by nearest-store — the compiler interleaves the next name's `addi` before the previous
  result's `stb`, so the naive pairing is off by one (this is the trap the memory note
  about symbol tables describes, met in the wild):
  `disable_cinematics_capture` 0x82A57D36, `allow_cinematics_skip` 0x82A57D37,
  `skip_cinematics` 0x82A57D38, **`show_cinematic_info` 0x82A57D39** (its reader is
  `0x82479000`, an on-screen overlay). None is in `debug_tunables.cpp`'s curated table
  yet; adding `show_cinematic_info` is ten minutes if a cinematic question ever returns.
* **My reader-count scan under-counts, and I know because I ran a control.** Scanning for
  `lbz` with an exact displacement found 1 reader for `debug_show_loading_time`, where the
  curated table records 6. So "1 reader" above is a floor establishing the flag is
  connected, not a census. Use `debug_tunables.cpp`'s own method if the number matters.
