# D3D phase C, part 18 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Part 17 ended when the session's shell died mid-work,
so **this file was written without the ability to build, test, or commit anything after
a certain point** — the "unverified" section below is load-bearing, not a formality.

## The one-paragraph state of the port

Case Zero is playable at **~15 fps** (was ~10). The picture is broadly correct. What is
open: a save that mounts and then does not write, a view-dependent whole-frame black in
Still Creek, the magenta sky, and the prologue cinematic. Two of part 16's defect
reports were retracted this session by ordinary play — the pause menu and Dick's missing
body — which continues the pattern part 17's kickoff named.

## Where part 18 starts, in order

1. **BUILD AND VERIFY `CZ_VK_SNAP_ON_BLACK` — it has never compiled.** It is written into
   `runtime/gpu/vk_renderer.cpp` and is uncommitted. Before trusting it, run its positive
   control, because its FIRST version could not fire and its control could not reach its
   own trigger (gotcha 30, and the fix is why the two thresholds are separate):
   ```
   cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_VK_SNAP_FRAME=999999 \
     CZ_VK_SNAP_ON_BLACK=99 CZ_VK_SNAP_ON_BLACK_LIT=20 CZ_VK_SNAP_DUMP=/tmp/x \
     timeout 120 ./cz_runtime 2>&1 | grep SNAP_ON_BLACK      # must print a line
   ```
   If it fires, commit it. If it does not, the instrument is wrong — do not use it to
   conclude anything about the black frame.

2. **THE VIEW-DEPENDENT BLACK now has a BEFORE frame, and the direction names the
   mechanism.** The operator captured the pair: Still Creek in daylight with an enormous
   blown-out white glow mid-frame, then the SAME spot with the camera moved slightly
   left and the entire frame black, window title still counting frames at 9 fps.
   **A degenerate auto-exposure that saturated would go WHITE. This goes BLACK**, which
   is what `exposure = key / averageLuminance` does when the average comes back enormous
   or non-finite — i.e. the 64x64 luminance reduction eating a very bright emissive.
   `CZ_VK_SNAP_ON_BLACK` exists to capture that chain; the 64x64 surfaces in the dump are
   the reduction pyramid. Both screenshots were LOST to Spectacle's temp cleanup;
   `~/DR2CZ-troubleshooting/INDEX.md` has the descriptions.

3. **THE SAVE: the part-16 fix WORKED and the failure MOVED.** `XUserWriteAchievements`
   no longer returns E_FAIL, and `XamContentCreateEx('save','DR2P000.DSF',flags 00001012)
   -> mounted` succeeds. The mount and its `XamContentClose` are ~10 lines apart in the
   operator log with no file activity between them.
   **DO NOT read that as "it wrote nothing".** `NtCreateFile` successes print only for
   the first 512 and then every 64th (`runtime/kernel/file_imports.cpp:207`), and that
   run was thousands of opens deep by the time it saved. The question is unanswered, and
   the run that answers it is one save attempt with `CZ_FILE_TRACE=1 CZ_SAVE_PROBE=1`.
   Worth doing first: add an UNCAPPED log line for any file operation on the mounted
   `save:` device. Saves are rare, so it costs nothing and it cannot be capped away.
   There is also a new unhandled message in the log, early and not yet tied to the save:
   `[xam] no handler for app FA message 0007001B (0-byte buffer) — returning E_FAIL`.

4. **PERFORMANCE — the renderer is NOT where the frame goes, and this is measured.**
   `CZ_VK_PROFILE=N` splits a frame by phase. At gameplay, ~1,900 draws, 87 ms/frame:
   ```
   draw 8.6% [constants 0.5 streams 2.2 textures 1.0 record 5.0 other 0.0]
   submit 32.6%   readback 0.4%   outside 58.5%
   ```
   with **134% process CPU**, i.e. one saturated thread. So it is 33% GPU wait and 58%
   guest code + command processor, and **the entire renderer is under a tenth of the
   frame**. Making the renderer infinitely fast buys ~1.09x. Anyone planning renderer
   optimisation should read that table first — the per-draw constant upload, which is
   the obvious suspect and was mine, is **0.5%**.
   The next real lever is overlapping the 28 ms GPU wait with the CPU (double-buffered
   command buffers), which changes "which frame is on screen" into a question with two
   answers — read `SubmitAndWait`'s comment before touching it.

5. Then the standing list: the magenta sky / colour-grading LUT, the prologue cinematic,
   slot recycling (3b), mipmaps (4), white ground decals, the case timer bar, XAM ordinal
   `0x271` on the save-LOAD path, the shadow cascade (3).

## What part 17 delivered

Committed, built, and gated (`--smoke` OK, A1 exact 84-deep prefix, A5 exit 0 with 0 real
windows, `truncated=0`, zero shader misses, `#83 cinezombie.big`):

* **Analog sticks in the synthetic-input arm.** `LSUP/LSDOWN/LSLEFT/LSRIGHT` walk Chuck,
  `RSUP/RSDOWN/RSLEFT/RSRIGHT` aim the camera. A stick entry HOLDS for its whole interval
  where a button entry taps for 150 ms — a tap-shaped stick moves Chuck a few centimetres
  and would read exactly like a stick that does not work. Verified: `LSUP` walks him from
  the middle of the safehouse to the door and raises its "Open (B)" prompt; `RSRIGHT`
  rotates the camera ~90°. This closes the other half of gotcha 190: menus were reachable
  headless and the WORLD was not.
* **A frame profiler and a `msec` column** on `CZ_VK_FRAME_STATS` (appended, so every
  existing tool's column indices still hold).
* **The readback buffer was WRITE-COMBINED.** `FindMemoryType` returns the first type
  matching its mask, and the first `HOST_VISIBLE|HOST_COHERENT` type on a discrete GPU is
  write-combined — right for every other mapped buffer here, which are CPU-write-only,
  and exactly wrong for the one buffer the CPU READS. Presenting a frame read 3.7 MB back
  uncached at ~230 MB/s. Measured, same binary, both arms: readback **15.7% -> 0.4%**,
  frame **103 -> 87 ms**, **9.7 -> 11.5 fps**. The operator sees ~10 -> ~15 fps windowed.
  `CZ_VK_READBACK_UNCACHED=1` is the control arm.

## Retractions from part 17

* **The pause menu in Still Creek is FINE.** Part 16's item 3c recorded it sheared into a
  trapezoid with stray polygons against a correct safehouse one, treating world state as
  the variable. It is pixel-correct on the part-17 binary. Cause not isolated (the cache
  went 351 -> 370 and part 13's `VGT_INDX_OFFSET` fix both landed in between), so it is
  recorded the way the combo-weapon cinematic was: broken on the old binary, works now.
* **Dick renders.** Item 3d's "floating head and one hand" is at least partly gone on the
  current binary. Re-check Fausto's legs and Gemini's hair before working that item —
  part 17's kickoff predicted exactly this, and it was right.

## The method note that cost the most

**I picked a suspect before measuring and it was wrong by a factor of forty.** The
per-draw constant upload copies 8 KB into mapped memory for every one of ~1,900 draws —
~19 MB a frame — and I had it as the prime suspect on the strength of that arithmetic.
It is 0.5% of the frame. The profiler took twenty minutes to write and immediately named
a completely different mechanism in a subsystem I had not looked at. Write the instrument
before the theory; this project has a gotcha for it (80) and I still did it backwards.

**And an operational one that will bite every future session on this machine: `/tmp` is a
TMPFS, so everything in it is RAM.** It is 32 GB and was sitting at **24 GB**, almost all
of it scratch from earlier sessions of this port (`/tmp/claude-1000` 9.0 G, `f13_*`
~1.8 G, `aw.dis` 693 M, `c3_watch.log` 405 M). `cz_runtime` maps a large guest memory
space from a memfd, which is also RAM — so running the binary on top of a full tmpfs
pushes the machine into memory pressure and the OOM killer reaps things. Observed twice:
the session's shell died within seconds of starting a run, and the operator's screenshot
tool failed with "Error while writing temporary local file", which lost the two frames of
the black-transition pair — the most valuable evidence of the session.

**Check `df -h /tmp` before a run, not after a mystery.** And note the misdiagnosis, which
is the reusable part: part 17's own frame dumps (~1 GB of uncompressed PPMs) were
initially blamed for the whole thing, and deleting them took `/tmp/c17` to 7 MB while the
tmpfs stayed at 24 GB. A contribution you can see is not automatically the cause, and the
one-line check that settles it (`du -sh /tmp/* | sort -rh`) was not run until the same
symptom had appeared twice. Dumps still belong in the session scratchpad and should be
deleted as soon as they are read — that part of the advice stands, it is just not
sufficient.
