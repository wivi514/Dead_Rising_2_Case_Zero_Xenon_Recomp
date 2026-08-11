# Part 29 hand-off (for part 30). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `phase-av-kickoff.md` for "where
the port is"; that file remains the record of what phase A/V built, and
`d3d-phase-c28-kickoff.md` is still the authority on the WHITE-SURFACE chain, which
neither part touched and which is still the top RENDERING defect.

**Check the git log against this file before working an item** — gotcha 13, and it has
cost this project a session three times.

## The one-paragraph state of the port

The game boots, renders, plays and makes sound, **and cinematics now play to completion
with audio — four operator-confirmed in one session, including the combo-weapon award,
which awards the weapon. No cinematic is known to fail.** `open-items.md` 00j is CLOSED: the
ping-pong was our XMA packet walk ignoring `packet_skip`, so every context of a 5.1 group
decoded the other streams' packets as its own and wedged the voice group after one buffer.
The renderer's white surfaces are untouched and are now the top open defect.

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

* `--smoke` OK; the recompilation is untouched (nothing outside `runtime/cpu`,
  `runtime/kernel/audio.cpp` and `tools/` changed).
* **The shader cache grew 411 -> 417** on the operator's session: the cinematic era, now
  that it actually plays, binds six pixel shaders no run had ever reached, and the run
  logged `no translated shader` six times before they were translated in. Gotcha 13 again
  — "the cache is complete" had a shelf life and the thing that expired it was fixing an
  unrelated defect. `tools/shader_dim_census.py` exit 0, 417 modules, 0 disagreements.
* The 00j repro reproduces exactly on the current binary: **6.14 / 0.58 / distinct 1169**
  against the recorded 6.13 / 0.58 / 1170, and the probe binary reproduces it again
  (6.14 / 0.58 / 1170) — so **the instrument does not perturb its subject**.

**Not re-run and owed before any claim resting on them**: the A5 kernel-call diff,
`truncated=0`, the two PM4 capture oracles, the capture-E picture correlation, the
Vulkan validation tally, and the shader-cache name diff. Nothing in this part touches
`gpu/` or the import set, so all are almost certainly clean — but that is an argument,
not a gate.

## WHERE TO START

0. ~~**THE CINEMATIC PING-PONG.**~~ **CLOSED — see `open-items.md` 00j for the full
   record.** Two commits: walking each XMA stream's own `packet_skip` chain (the fix), and
   only switching input buffers when the other is valid (necessary, moved the gate by
   nothing on its own). Gate before/after on the prologue: cinematic-era `runs/distinct`
   **120 -> 1.00 in every quarter**, audio clock **4.906667 s frozen -> 310.7 s of a
   316.5 s track**, `audio/cinematics.big` read **2 -> 201** times. Operator: both the
   first and second cinematics played to completion with sound.

   **What to carry forward rather than re-derive:** 5.1 on this hardware is several
   interleaved 2-channel streams in ONE packet stream with one XMA context per pair, so
   several contexts legitimately share an input buffer and each must follow its own skip
   chain. **This is a Case West item on day one**, alongside gotcha 267's physical
   addresses.

0b. ~~**THE TWO OPERATOR QUESTIONS FROM PHASE A/V.**~~ **ONE ANSWERED, ONE STILL OPEN.**
   * ~~Do the other cinematics play through?~~ **YES — four confirmed on the fixed build
     in one operator session:** the prologue, one more, the walk out of the safehouse,
     and **the combo-weapon award, which awards the weapon**. That was the last cinematic
     with any recorded failure against it (`open-items.md` 1), so **the population of
     known-failing cinematics is now empty.** Say that as "none known to fail", not "all
     of them work" — the session did not visit every cinematic in the game.
   * **Is the frame rate unchanged with the decoder in? STILL UNMEASURED, and it now
     matters more**, because the decoder does strictly more work than when the question
     was asked: it used to wedge after one buffer and now streams a 24 MB asset through
     three contexts for five minutes. `CZ_NO_XMA_DECODE=1` is the arm; three runs an arm,
     alternated, null comparison first, and read medians and the pinned-to-16 ms share
     rather than means (gotchas 229, 237). Budget an hour. **Do this before anyone quotes
     a frame rate from a build with audio in it.**

1. **THE WHITE SURFACES — NOW THE TOP DEFECT IN THE PORT, untouched by parts 28 and 29.**
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
