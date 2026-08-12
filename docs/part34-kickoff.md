# Part 34 hand-off (for part 35). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part33-kickoff.md` for "where the
port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays its cinematics through. Part 34
executed part 32's item 0: **the 4x MSAA Y factor for window coordinates is now the
default**, which is the shadow-cascade fix shipping — the atlas reads 0.0038% zero with
all 1024 rows covered (was 46.8750% / 512), the outdoor picture moves the way a graded
shadow term predicts (distinct colours +8.30% at 5.2x the null), and nothing else
regressed. It also delivered §6ba's owed exposure re-measure — **there is no exposure
discrepancy left to explain** — and re-ran every gate part 33 left owed. What the shadow
work needs now is not code but **the operator's three-way verdict**.

## WHAT PART 34 DID — do not rebuild any of this

Full record: `docs/phase5-notes.md` §6bh. Open-items item 3 is rewritten.

* **The default flip** (commit e10df05): the Y factor for 4x window coordinates is on;
  `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` is the same-binary control arm (the part-33
  renderer); the part-32 arm variable `CZ_VK_MSAA_WINDOW_SCALE_Y` is RETIRED and no
  longer read. The reconciliation of §6bf's scene-tile objection: a clear rect is in
  the CLEAR declaration's own pixel space, so both axes scale by the draw's OWN
  declared sample factors — no rule needs the render declaration at clear time. The Y
  over-clear past a shorter surface into the shared stand-in is the SAME approximation
  the X factor has always applied to the 640x360 post surface, and the gates are the
  measurement that it stays harmless. The exact form remains a stand-in at SAMPLE
  resolution in both axes with downsampling resolves — open, not foreclosed.
* **Gates, all on one binary, arms differing by one env var**: title-boot atlas
  46.8750% -> 0.0038% zero (four-decimal reproduction of §6bf, and the differential IS
  the engagement proof — the counter dump does not survive a `timeout` kill); the
  all-surface snap-dump diff shows no regression (scene + post chain gain mean luma
  +7-9 and distinct colours +12-18%, the predicted direction — part 32's "title picture
  unmoved" was measured on the garbage-normals renderer and is superseded); outdoor era
  medians over three matched-depth 420 s runs: **distinctColours +8.30% at 5.2x the
  1.60% null**, coverage inside the null, meanLuma +8.10% at only 2.4x its 3.37% floor
  (quote distinct colours, not luma).
* **§6ba's exposure re-measure**: all three arms identical — frame 3000 = 0.211
  (part 31 recorded 0.2146), era range 0.200-0.354, mean 0.2755, one value per frame
  re-confirmed. Hardware's 0.298 (`w7`) and 0.331 (`w1`) sit INSIDE our adaptive range.
  The discrepancy as recorded ("ours 1.0/pinned vs hardware 0.33") does not describe
  the fixed renderer. Owed: one matched-location reading (trace beside an operator F9
  at `w1_spawn`) — free on the next operator session, not worth its own.
* **Owed gates from part 33, all re-run clean**: A5 kernel diff exit 0 (3 permutation,
  0 real), `truncated=0` (151/151), both PM4 oracles exit 0, `CZ_VK_VALIDATION=1`
  tally unchanged (zero 08733; the 6 `topology-08773` predate part 33), capture-E
  correlation **+0.958 identity** — statistically the recorded +0.9597; the part-33/34
  fixes do not live on a 2D title card, so "expect improvement" was wrong to expect
  there.
* **Part 33 had no `port-history.md` entry; part 34 wrote it** (with a note saying so),
  plus its own.

## READ THIS BEFORE MEASURING ANYTHING

Everything from parts 26-33's lists stands, plus:

* **Any shadow/atlas number recorded before part 34 was taken with half of every
  cascade rejected.** The control arm reproduces it exactly; do not compare a new
  measurement against a pre-part-34 baseline without saying which arm it was.
* The era-median noise floor is a property of the PAIR: this block's three runs reached
  matched depth (12,114-12,138 era frames) and gave a 1.60% distinct-colours null;
  part 32's block did not (4,043-9,303) and gave 0.35% on colours but 11.83% on luma.
  Run the null pair every time; never inherit a floor.

## WHERE TO START

0. **THE OPERATOR, one session, two questions riding together.** LAUNCH IT FOR THEM
   (the memory note is right: wire the instruments, let them drive):
   * **The three-way shadow verdict** at one Case 0-2 crowd spot, camera unmoved
     between shots, F9 each time (`CZ_CAPTURE_KEY=<dir>` per arm):
     default / `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` / `CZ_VK_NO_ADDR_TILE_FOLD=1`.
     Name the property first (gotcha 278): the EXTENT and CONTINUITY of the shadowed
     region — both control arms should show a hard camera-locked boundary across the
     world; the default should show none.
   * **The exposure matched-location reading**: `CZ_VK_EXPOSURE_TRACE=<file>` on the
     default arm, F9 at the `w1_spawn` spot, read the trace at the capture frame
     against hardware's 0.331368. Closes §6ba entirely.
1. **Re-ask the parked picture items on this renderer** (unchanged from part 33's
   list): LOD/00i (explicitly parked behind 00f), NPC part meshes (3d), mipmaps, the
   colour-grading LUT, and the cube-decline defect (the `s3`/`s4` duplicate — separate,
   still open, 9 enumerated cases).
2. **The EDRAM stand-in at sample resolution in both axes** — the exact form of part
   34's approximation, sized at a session (taller image, viewport/scissor scaling for
   2x/4x renders, downsampling resolves). Worth doing when a defect is traced to the
   over-clear class, not before.
3. `docs/perf-cpu-plan.md`'s CPU/GPU overlap (gotcha 231) — still the largest
   performance term — and the rest of `docs/open-items.md`.

## Gates, on this binary (commit e10df05)

* `--smoke` OK. Atlas 0.0038% zero / 1024 rows (control arm 46.8750% / 512).
* Outdoor era medians: distinctColours +8.30% at 5.2x null; coverage inside null.
* `CZ_VK_VALIDATION=1` plain boot: zero 08733, 6 topology-08773 (pre-existing).
* Capture-E: +0.958, identity orientation (frame 576 of an every-64th dump; the title
  lands on a different frame each boot — try 448..640).
* A5 diff `--include-high-frequency`: exit 0, 3 permutation windows, 0 real.
* `CZ_RING_TRACE=1` boot: truncated=0. Both PM4 oracles on B1: exit 0.
* `no translated shader` = 0 on every run of the day; cache still 417.

## The artifacts

Scratch runs live in the session scratchpad (`boot_default`, `boot_noy`, `fs_*.txt`,
`exp_*.txt`, `frames/`, `gate_boot.log`, `atlas_stats.py`) — tmpfs, so quote the numbers
from §6bh, not the files.
