# D3D phase C, part 19 hand-off (for part 20). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c18-kickoff.md`, whose
items 1 and 4 were already done before it was read (part 17 committed
`CZ_VK_SNAP_ON_BLACK` and the overnight session superseded its performance table) — so
**check the git log against a kickoff before working it**, which is gotcha 13 with a
worked example.

## The one-paragraph state of the port

Case Zero boots, renders and plays. Ordinary gameplay is 31 fps and closed at the title's
own pacing; crowds are 22-25 and CPU-bound in our runtime. The picture matches capture E2
at +0.9597 identity. **The port's top rendering defect for the last six parts — the
view-dependent whole-frame black — is solved and was a 128 MB allocator. And SAVE AND
LOAD are a closed round trip**, confirmed by an operator and byte-checked against the
hardware save capture — the first title state in this port that survives a process exit.
What is open: the shadow cascade, mipmaps, NPC part
meshes, the magenta sky / colour-grading LUT, the prologue cinematic, and the CPU half of
the crowd frame.

## Where part 20 starts, in order

1. **`docs/perf-cpu-plan.md` IS NOW FULLY RUNNABLE and it is the biggest single item.**
   Its item 0 — a headless recipe that reaches a crowd — blocked the whole plan and is
   closed: the recipe is in `CLAUDE.md`'s Commands section and reaches **6,400-8,700
   draws a frame** across five runs. §1 (the renderer draw path, 21.4 ms) and §2 (the PM4
   walk, 11.0 ms) are exactly as written and each begins with a measurement rather than a
   change. Two notes the plan does not have:
   * **§1a hypothesis D just got a second reason.** The per-draw constant block is 8 KB
     of the ~27 KB a draw the arena now has to hold; deduplicating identical consecutive
     blocks pays in frame time AND in the resource that used to black the frame out.
   * **Quote the GPU clock with every number.** `sudo nvidia-smi -pm 1` then
     `-lgc 2100,2100`; this machine idles at 210 MHz of 2100 and it is worth 2.9x on the
     GPU term (gotcha 219). It was P8 for the whole of part 19, which is fine because
     part 19 measured no frame times.

2. ~~**THE SAVE**~~ **DONE — save and load are a closed round trip.** Both halves
   confirmed by an operator: the title writes a 303,104-byte file whose header is
   byte-identical to hardware's from offset 4, and reads it back and resumes. Four
   defects, each independently fatal: `NtCreateFile` honoured no disposition,
   `NtWriteFile` was a stub, the VFS cached negative lookups, and xam ordinal `0x271`
   (`XamContentCreateInternal`) was refused because `kResolvable` was built from A1,
   which was captured with an EMPTY save root — the one configuration in which the load
   path never runs. **Open-items 1b and 2 are both retracted**, and 2's per-profile
   signature theory with them: A3's save was never ours to load, so every conclusion
   drawn from trying was about the wrong file.
   Two notes for anyone touching this area. **The pause menu has no save option** and the
   image names saving as `cTriggerVolume::ACTION_SAVE_GAME` — it is a volume you walk
   into, which is why the synthetic-input arm cannot reach it and why this needed a
   human. And the LOAD half **is** headlessly reachable: the slot panel enumerates and
   mounts at boot once a save exists, so `ord=0x271 -> ...` and the `save:` open both
   appear in a plain run.

3. **The remaining picture defects, now that the black is out of the way.** They were all
   competing with it for attention and several were probably contaminated by it — a frame
   that lost its post chain looks like a lot of things. Re-test before investigating:
   the shadow cascade (open-items 3), mipmaps (4), NPC part meshes (3d), the magenta sky
   / colour-grading LUT (6). **The luminance ladder now delivers a non-zero scene
   average** for the first time (§6ao), so anything downstream of exposure is worth
   re-looking at with fresh eyes rather than re-reading the old notes.

4. **One observation from part 19 that is NOT written up as a defect, deliberately.** In
   the safehouse the door renders as a fully saturated white slab (`present_7200` in the
   session's scratchpad, described in §6ap's hunt). It may be correct — bright daylight
   behind a doorway — or it may be a texture served as the white dummy. It has no
   counter behind it and no measurement, so it is an observation. `CZ_VK_SKIP_TEX` to
   name the address is the cheap first move if anyone wants it.

## What part 19 delivered

* **The whole-frame black is the per-frame arena** (`docs/phase5-notes.md` §6ap).
  128 MB against a 161 MB peak; `ArenaAlloc` skips every draw it cannot satisfy and the
  post chain is last in the frame. **128 MB: 160 black frames of 8,216. 512 MB: zero.
  Every one of the 160 is the frame after an `arena EXHAUSTED` line — 160 of 160.** The
  arena grows now; `CZ_VK_NO_ARENA_GROWTH=1` is the pre-part-19 renderer.
* **A snapshot is PITCH-sized and a fetch is WIDTH-sized** (§6ao). Every texture
  coordinate scaled by width/pitch wherever those differ, which emptied the tail of the
  luminance ladder and made the 2x1 scene average identically zero in every era. Fixed
  with right-sized views; 9 created in a boot; predicted lit-column counts match five
  consecutive links exactly, before and after.
* **The save could not write, in two independent ways** (`docs/phase3-notes.md` finding
  52), plus a third — the VFS caches negative lookups, so a file this runtime created
  could never be re-opened — that only a test could find. **Fixed and confirmed the same
  day**: "Game saved successfully", and the file is 303,104 bytes with bytes 4..31
  identical to A3's real 360 save. That retracts open-items 1b, whose complete causal
  chain turned out to describe a path the title no longer takes.
* **`CZ_VK_SNAP_ON_DARK`**, which fires on mean luminance and dumps a BRIGHT REFERENCE
  chain beside the dark one, and **`tools/snap_dump_stats.py`**, which turns a
  61-surface dump into 61 lines.
* **The outdoor/crowd recipe**, closing `perf-cpu-plan.md` item 0.
* Gotchas 224 (a fixed-size per-frame allocator is a rendering defect in disguise),
  225 (a sampler normalises over the image you hand it), 226 (a trigger fires on the
  metric you gave it, not the defect you meant).

## The method notes worth carrying

* **The pair is what refuted the leading hypothesis.** A single dark resolve chain would
  have been read as "the exposure collapsed"; the bright chain from the same location
  showed the scene colour intact in BOTH and every post surface zero in one. Build the
  control into the instrument, not into the analysis.
* **Predict the numbers, then measure them.** The pitch/width defect was confirmed by
  predicting six lit-column counts from one ratio and getting six. That is a much
  stronger result than "the picture looks better", and it took less time.
* **The arena hypothesis came from the frame STATS, not from the pictures.** The black
  frames were the ones with the most draws and the longest wall time, and both columns
  were already in `CZ_VK_FRAME_STATS`. Six parts of picture-led investigation did not
  find it; one `awk` over the columns did.
* **Every instrument this session added was written before the theory it tested**, which
  is the opposite of part 17's method note and is why this one closed the item.
