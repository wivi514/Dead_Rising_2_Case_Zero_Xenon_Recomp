# Part 29 hand-off (for part 30). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `phase-av-kickoff.md` for "where
the port is"; that file remains the record of what phase A/V built, and
`d3d-phase-c28-kickoff.md` is still the authority on the WHITE-SURFACE chain, which
neither part touched and which is still the top RENDERING defect.

**Check the git log against this file before working an item** — gotcha 13, and it has
cost this project a session three times.

## The one-paragraph state of the port

The game boots, renders, plays and makes sound. The prologue cinematic's ping-pong
(`open-items.md` 00j) is **diagnosed, not fixed**: the mechanism is settled with
measurements and a three-way arm, and the defect has moved one level up into the audio
pipeline, where part 30 should start. The renderer's white surfaces are untouched.

## WHAT PART 29 DID — do not rebuild any of this

Full record: `docs/phase-av-notes.md` part 29 section, and `open-items.md` 00j.

* **Found what writes the cinematic's time.** `sub_82475718` is the clock;
  `sub_82478FC8` stores its return into `[cine+0x1698]`. It switches on a mode word
  (global `0x829DC320`, **shipped as 2**): `0` raw scene time, `1` the audio stream
  position, `2` `PID(audio position)`. `sub_824741D8` is the PID and the image names it
  itself, plotting `Cine.Audio P-gain / I-gain / D-gain / MV (ms)` in its own tail. It
  returns **setpoint minus an accumulator**, which is why the scene can run backwards.
* **Measured it.** `CZ_CINE_TIME=<file>`: mode 2 on all 2,212 lines, the PID ran on
  2,208, `setpoint` climbs linearly forever, **`audioPos` freezes at 4.906667 s**, `ret`
  hunts 4.91 <-> 5.27 s.
* **Joined the camera to the clock.** Median spread of `ret` within one
  `cameraFingerprint` is **0.0052 s**; the same statistic at deliberately wrong
  alignments is 0.042-0.377 s. The camera palindrome is the clock's palindrome.
* **The arm: `CZ_CINE_AUDIO_MODE=0|1|2`**, every setting a path the title implements.
  Mode 2 LOOPING (15 poses, ratio 120) · mode 1 **FROZEN** for 338 s · mode 0 no loop,
  the run reaches gameplay. **Mode 1 was predicted in the commit before it was run.**
* **Two tool fixes with their own evidence**: `gdis --find-uses` could not see a global
  that is LOADED, only one whose address is taken, and answered "0 sites" for a global
  with 301; `frame_loopiness.py` now prints era quarters because the whole-file number
  understates this defect 6x.

## READ THIS BEFORE MEASURING ANYTHING

Everything from parts 26-28's lists stands. Part 29 adds three:

* **A SCANNER'S ZERO IS A STATEMENT ABOUT THE SCANNER** until you have shown it can
  match the shape you are asking about. `--find-uses` read 0 for an address referenced
  301 times, because it only knew `lis`+`addi` and the compiler folds the low half into
  the memory operand for anything it is about to dereference. The control costs one
  command: re-run it on an address whose matching site is already on screen.
* **A WHOLE-FILE SCORE AVERAGES OVER ERAS.** `runs/distinct` for 00j has been quoted as
  6.13 for two parts. Menus 1.01 · cinematic era 38.27 · steady state 15 poses at 120.
  Quote quarters. And the gate cannot tell a stalled scene from a **parked player** —
  `CZ_FAKE_PRESS_SEQ` ending in `NONE` leaves Chuck standing still and one camera pose
  is then correct. Read draws beside it. (This caught the author of part 29 calling a
  mid-run file healthy.)
* **WHEN TWO COMPONENTS YOU BUILT AGREE, YOU HAVE MEASURED YOUR OWN CONSISTENCY, NOT THE
  GROUND TRUTH.** Part 29 recorded "the clip ended" because the guest's reported position
  agreed with our decoder's output to 448 sample-frames. Both numbers were right and the
  conclusion was wrong: the clip is 316 s and we stream 4.9 s of it. The asset was one
  `CZ_FILE_TRACE=1` away. The discriminator had even been identified and written down —
  the error was recording the likelier branch as a finding instead of leaving it open
  until the third party answered. **An oracle has to be something you did not write.**
* **NAME WHICH FACT YOU MEAN BEFORE ORDERING TWO EVENTS.** "The audio stopping" was one
  phrase covering two things with opposite orderings: the position the guest *reports*
  stops first and is upstream of the stall; audible output stops ~5.5 s later and is
  downstream. Part 28's "do not chase the silence" is right about the second and wrong
  about the first, which is where the defect lives.

## Gates, on this binary

* `--smoke` OK; the recompilation is untouched (nothing outside `runtime/cpu` and
  `tools/` changed).
* The 00j repro reproduces exactly on the current binary: **6.14 / 0.58 / distinct 1169**
  against the recorded 6.13 / 0.58 / 1170, and the probe binary reproduces it again
  (6.14 / 0.58 / 1170) — so **the instrument does not perturb its subject**.

**Not re-run and owed before any claim resting on them**: the A5 kernel-call diff,
`truncated=0`, the two PM4 capture oracles, the capture-E picture correlation, the
Vulkan validation tally, and the shader-cache name diff. Nothing in this part touches
`gpu/` or the import set, so all are almost certainly clean — but that is an argument,
not a gate.

## WHERE TO START

0. **WHY DOES THE GUEST STOP STREAMING THE CINEMATIC AUDIO? — the live item.** The chain
   from the symptom down to it is fully read and every link is measured:

       sub_82759170   the cinematic asks the audio system — message ReqID 0x106, "Time"
       sub_827213C8   the audio system looks the voice up in [sys+0xA8], returns entry+8
       sub_82721530   which refreshes every entry each tick from sub_8270F768(voice)
       sub_8270F768   voice state 2/3 -> sub_82764C48; ELSE a wall clock
       sub_82764C48   SamplesPlayed / sampleRate, from an XAUDIO2_VOICE_STATE-shaped struct

   `4.906667 x 48000 = 235,520` samples exactly `= 1,840 XMA subframes of 128`. The
   voice pins there while still reporting itself playing, so `sub_8270F768` never takes
   the wall-clock branch that would let the cinematic carry on.

   **The discriminator has been RUN and the answer is that we stream 1.1% of the clip.**
   `CZ_FILE_TRACE=1` names the asset: `game:\data\audio\cinematics.big` entry
   **`39694.xma`, 24,377,344 bytes**, read from its exact start. Summing `frame_count`
   over all 11,903 XMA2 packet headers gives 89,007 frames of 512 samples =
   **316.5 s (5:16) as three interleaved 2-channel streams** — 5.1 audio, which the
   operator's ~5:10 confirms and which explains why contexts 5, 6 and 7 share the input
   buffer `02584000` (three stereo contexts is how the 360 decodes six channels).

       music.big        47 reads, 128 KB each, alternating two buffers, FOREVER
       cinematics.big    ONE 128 KB read into A2584000, one into A25AA000, then nothing

   We decode the first buffer's **4.916 s of a 316 s clip**. Music double-buffers
   correctly on the same machinery for the whole run, so **the difference between those
   two streams is the defect.** Start there: which context fields the title polls for
   each, and whether our decode walk leaves the cinematic contexts in a state the music
   context never reaches. **A retire/refill rule written for one context per buffer is
   wrong for 5.1 by construction** — three contexts share `02584000` — and
   `kernel/audio.cpp` is where to check that first.

   **The free gate for the whole item is unchanged and needs no operator**:
   `tools/frame_loopiness.py` on a prologue run, reading the QUARTERS. A fix takes the
   steady-state quarters from `runs/distinct ~= 120` to `~= 1` with `runs/frames` well
   above 0.15.

   **Four explanations are refuted with measurements. Do not re-buy them:** our
   output-ring `write == read` ambiguity (built, predicted, run, refuted, reverted); the
   audible audio stopping (downstream, ~5.5 s later); the animation "end sync point" the
   engine narrates (counted — ten entries in a 400 s run, an even split, and the
   containing function stops being called while the loop continues); and **PID
   mistuning** — the gains are shipped values and the controller is behaving correctly
   for a frozen input, so do not touch P/I/D.

0b. **THE TWO OPERATOR QUESTIONS FROM PHASE A/V, still unanswered.**
   * **Do the other cinematics play through?** Part 29 measured the prologue only. The
     combo-weapon award is the cheapest single check, and a cinematic that completes
     would show the ping-pong is prologue-specific — or, given the mechanism, that the
     other cinematics simply have no `sync:` audio event.
   * **Is the frame rate unchanged with the decoder in?** Still unmeasured.
     `CZ_NO_XMA_DECODE=1` is the arm; three runs an arm, alternated, null first, read
     medians and the pinned-to-16 ms share (gotchas 229, 237). Budget an hour.
1. **THE WHITE SURFACES — still the top RENDERING defect, untouched by parts 28 and 29.**
   `d3d-phase-c28-kickoff.md`'s eight-step chain is the live state and its item 0 is the
   next instruction: what pins `c` at `1/pc(14).w`. Read our translated
   `ps_ad65b98593f95926` against the capture's disassembly of it. Note the knock-on:
   item **00i (LOD pops in late) is parked BEHIND this**.
2. The rest of `docs/open-items.md`, and `docs/perf-cpu-plan.md`'s largest item — the
   CPU/GPU overlap work (gotcha 231), still the biggest performance term.

## What is worth knowing that is not in a commit message

* **The cinematic system's static map is now complete enough to skip an hour.**
  `cinematicmanager.cpp` is named at `0x82063030` and its assert sites put the module at
  roughly `0x82474000-0x8247A400`. The clock is `sub_82475718`, the PID `sub_824741D8`,
  the caller `sub_82478FC8` (two call sites, `0x82479254` and `0x8247938C`), and the
  scene's time lives at `[cine+0x1698]` with the wall-clock accumulator at `[cine+0x16EC]`.
  The PID's config block: `+0` setpoint, `+4` mode, `+8/+c/+10` P/I/D, `+18` deadband,
  all copied from `0x829DC320..0x829DC338` (P 0.025, I 0.005, D 0.0001, deadband 5 ms).
* **The debug-graph API is a naming oracle and it is reusable.** `sub_8276B638` interns a
  named channel and `sub_8276B5F8` plots a float into it. Any function that ends in a run
  of those calls is telling you what its own locals mean — that is how `sub_824741D8` was
  identified as a PID rather than guessed at. Grep for the pattern next time a float
  pipeline needs naming, and it should transfer to Case West unchanged.
* **`gdis --find-uses` now prints the mnemonic with each hit.** For a data address the
  question is nearly always "who WRITES this", and picking the two `stw` sites out of 301
  by eye is a five-second answer once they are labelled.
