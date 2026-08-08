# D3D phase C, part 17 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §§6ah-6ai** for the
instrument work, but read THIS first — part 16 ended with an operator play-through that
changed more than the whole instrumented session before it, and it invalidated several
things the earlier draft of this file said.

## The one-paragraph state of the port

**Case Zero is playable.** A human with a controller reached the safehouse, Still Creek,
the Quarantine Area, the pawnshop and the bar; completed Cases 0-1 through 0-3; escorted
Fausto and Gemini; built combo weapons; killed 41 zombies and banked 9,700 PP. The
picture is broadly correct — interiors, crowds, HUD, menus, cutscenes. What stops a
full play-through is not any single defect but **8-12 fps**, plus a save that does not
write and a handful of visual faults listed below.

## What part 16 actually established

* **Headless gameplay is reachable** (recipe in CLAUDE.md's Commands). START skips
  cutscenes; the Zombrex tutorial's second page needs D-pad LEFT then B. This is what
  makes every gameplay item below self-servable.
* **The shader cache went 337 -> 370, and every one of those came from playing.** A1
  stops at the title screen and A2 is one gameplay session, so the cache only ever
  covers ground someone has walked. **Most "black screen" reports this session were
  missing shaders**, including Still Creek, the Quarantine Area and a cutscene.
  `~/DR2CZ-troubleshooting/ucode-dumps/` holds 366 blobs.
* **The bindless heap was exhausted** (`nextTextureSlot` read exactly 4096 out of a live
  process) and is mitigated to 65536. Recycling is still the real fix.
* **The save's root cause is found and implemented**: XGI `0x000B0008` is
  `XUserWriteAchievements`, and returning E_FAIL for it aborted every save.
  **UNTESTED** — nobody has reached a save point since.
* **Four of the session's headline findings were wrong and are retracted in place.** See
  "What I got wrong" below; it is the most useful section here.

## Where part 17 starts, in order

1. **RE-TEST EVERYTHING ON THE CURRENT BINARY BEFORE INVESTIGATING ANYTHING.** The last
   operator session ran 351 shaders while 370 were on disk, so an unknown share of its
   defects are already fixed. Specifically re-check: Dick's missing body, Fausto's
   missing legs, Gemini's missing hair (3d), the black cutscene leaving the safehouse,
   and the prologue's black cinematic. Cheap, and it decides what is real.

2. **THE SAVE — implemented, never run.** Reach a save point and confirm a file lands in
   `assets/save/DR2P000.DSF/`. A3's is one 303,104-byte file, which makes it a
   byte-comparable oracle the moment ours exists. `CZ_SAVE_PROBE=1` prints the guest's
   own overlapped reads.

3. **PERFORMANCE, now a blocker on EVIDENCE rather than polish** (3f). 8-12 fps is why
   the play-through stopped. Every frame-rate number this project owns is from the
   title screen; nothing has been profiled against a gameplay scene. Known suspects:
   synchronous submit + full readback per frame, per-draw constant uploads at
   ~900-2,500 draws, depth resolves (~6%).

4. **THE VIEW-DEPENDENT WHOLE-FRAME BLACK** (1c). Looking at the gas station blacks the
   entire frame; turning away restores it instantly. NOT missing shaders — it survives a
   run with zero misses. Leading hypothesis is the auto-exposure chain going degenerate
   on a very bright emissive; the operator's gas-station screenshot shows an enormous
   blown-out bloom, and when the heap was exhausted and the scene filled with white
   dummies the frame washed out — so exposure demonstrably tracks scene content.
   `CZ_VK_SNAP_DUMP` dumps that luminance chain.

5. **The magenta sky** (item 6). Unchanged and now well evidenced: the daylight sky is
   pink/magenta while the orange shirt, red car and yellow HUD pips in the SAME frame
   are correct, so it is not a tint or an exposure error. Signature of a wrong
   colour-grading LUT. Interiors look markedly better than exteriors, so it likely
   varies with the grade the title selects per area.

6. **The prologue cinematic** — the only confirmed remaining cinematic failure (1), and
   re-test it first per item 1.

7. Then the standing list: slot recycling (3b), the pause menu sheared in Still Creek
   (3c), mipmaps (4), white ground decals, the case timer bar (3e), XAM ordinal `0x271`
   on the save-LOAD path (2), the shadow cascade (3).

## What I got wrong in part 16, and what it cost

Four retractions in one session, all mine, all from generalising early:

* **"Every cinematic starts and never ends"** — built from two failures, written up as a
  property of the title. Cinematics work; the prologue's is the only confirmed failure
  left. Narrowed three times by ordinary play.
* **"Cutscenes are being auto-skipped"** — they played. The operator could not move the
  camera because a save prompt and tutorial had locked it, and it was pointing at the
  view-dependent black. A borrowed symptom.
* **"PP/levelling is broken"** — Case Zero needs **20,000 PP per level** and the player
  had 9,700. Staying at LV. 1 was correct. I filed it with two competing explanations
  and a designed experiment; the threshold was a number that was knowable first.
* **"The save fails at the content overlapped poll"** — read off a `CZ_KCALL_WHO`
  backtrace. A live memory read showed that block was all zeros, and the probe then
  named a completely different overlapped. **A backtrace names the BRANCH, not the datum
  it branched on.**

And two smaller ones: I described an NPC as having a full body when it was a head and a
hand, and called a character's gloves a defect. Both were corrected by the operator in
one sentence each.

The pattern is worth naming: **every one of these came from reasoning further than the
evidence, and every one was settled by someone playing the game for ten seconds.** The
instruments were not wrong; they were pointed at questions that had already been
answered elsewhere.

## Method notes worth keeping

* **A running game is a measurable artefact.** Two missing shaders were recovered from a
  live process with `gdb -p ... dump binary memory` (verified by FNV-1a against the hash
  the renderer had printed), and the heap state was read with `print`. Both replaced a
  twenty-minute replay with a number.
* **Always run an operator session with `CZ_SHADER_DUMP`.** It captured 16 missing
  shaders for free; without it the first two needed the gdb rescue above.
* **A capped print is not a count and a THINNED print is not a distribution** — the
  constant watch produced two false readings in one afternoon before becoming a
  histogram.
* **Save screenshots straight into `~/DR2CZ-troubleshooting/operator-screenshots/`.**
  Spectacle deletes its temp directory on close; 13 of the first session's shots were
  lost and survive only as descriptions in that directory's `INDEX.md`.

## Where everything is

```
assets/shader_spv/                      370 .spv (+ .meta.json). GITIGNORED — a fresh
                                        clone must rebuild it, and the captures alone
                                        will NOT reproduce these 370 (see below)
~/DR2CZ-troubleshooting/                OUTSIDE the repo, 42 MB
  INDEX.md                              what every screenshot shows, INCLUDING the 13
                                        that were lost — read this before the images
  operator-screenshots/  17 PNG         a human's view, descriptively named. The two
                                        that matter most:
                                          0058_gas-station-huge-blown-out-bloom  (1c)
                                          0101_bar-fausto-no-legs-gemini-no-hair (3d)
  ucode-dumps/          366 .ucode      every microcode blob seen in gameplay. THIS IS
                                        THE ONLY COPY of the 33 shaders that are not in
                                        either capture — rebuild the cache from here,
                                        not from A1/A2:
                                          tools/build_shader_spv.sh ~/DR2CZ-troubleshooting/ucode-dumps assets/shader_spv
  renderer-dumps/        49 PNG         headless frames from the first gameplay run
  logs/                    1            the 2 MB session log the findings came from
```

**The ucode-dumps directory is load-bearing and not backed up anywhere else.** A1 stops
at the title screen and A2 is one gameplay session, so 33 of the 370 shaders exist only
because someone walked into the rooms that load them. Losing that directory means
replaying the game to get them back.

## Gates on the current binary

```
cz_runtime --smoke                                    OK
kernel_call_diff --xenia A1                           exact 84-deep prefix
kernel_call_diff --xenia A5 --include-high-frequency   exit 0, SET MATCH, 0 real windows
ring: indirect buffers truncated=                     0
grep -c "no translated shader"                        0
grep -c "bindless heap full"                          0
deepest file on a no-input boot                       #83 cinezombie.big
```
