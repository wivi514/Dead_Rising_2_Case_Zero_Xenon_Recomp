# D3D phase C, part 15 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §6ae** first — it is the
record of the session this hands off from. `docs/d3d-translation-plan.md`'s
**"Phase C part 14"** is the same story in one page.
`docs/d3d-phase-c14-kickoff.md` is the previous hand-off: its **items 1, 2, 3 and 4 are
closed or answered**, items 5 and 6 are unchanged and repeated below.

## What part 14 changed, in one paragraph

Part 14's list was the frontier at file `#154`, then the blur, then three more picture
differences. The frontier dissolved on measurement — the file counter stops climbing
because the title stops OPENING files, not because it stops loading — and the blur turned
out to contain two of the other three items. `RB_COPY_CONTROL`'s low three bits are
`copy_src_select`, **18.4% of this title's resolves copy the DEPTH buffer**, and this
renderer read that field nowhere: the depth-of-field pass had been computing its circle
of confusion out of the scene's own colour since phase 5, saturating it, and compositing
full blur over every pixel at every depth. Fixing it made the title screen sharp and
brought back the sign lettering, the bunting and the gas-station signage in one change.
It also produced a retraction with a long reach: `06BE4000`, documented as "the scene
surface" since phase 5 and used for every renderer A/B in this port, is the scene DEPTH —
it held colour pixels only because of the defect.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`, no input, empty `CZ_SAVE_DIR`, one 150 s boot:

* `--smoke` OK. A1: **exact 84-prefix**. A5: **exit 0, 2 windows, 0 real**.
* `truncated=0`; deepest file **#83 `game:\data\skeleton\cinezombie.big`**.

The renderer, two runs per arm, arms alternated, 85 s each, no input, era medians:

| | `CZ_VK_NO_DEPTH_RESOLVE=1` | default |
|---|---|---|
| mean-\|gradient\| (`tools/frame_sharpness.py`) | 1.185 / 1.204 | **7.640 / 7.666** |
| distinct colours, scene colour `0684B000` | 72,740 / 72,711 | **85,555 / 85,752** |
| coverage, scene colour | 99.61 / 99.62 | 99.62 / 99.62 |
| frames per 85 s | 859 / 848 | 803 / 811 |

With `CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A`, 300 s: the boot walks title ->
menu -> NEW GAME -> the game's own loading screen (the `TIP: Combo Weapons give extra PP`
card renders correctly) -> the prologue, reaching **#154
`game:\data\skeleton\childfullbody.big`** with **zero faults** and `truncated=0`, and
loading on past it out of already-open containers for another ~40 s. From frame ~943 the
presented frame is **0.00% non-black** for the remaining ~1,800 frames.

## Where part 15 starts, in order

1. **THE FRONTIER IS A BLACK SCREEN AT THE PROLOGUE, and its first black link is already
   named.** It is a compose failure, not a stall: ~1,200 draws a frame, the ring chain
   healthy, `truncated=0`, every `[wait]` an idle worker or one of the two threads the
   title blocks by design. `CZ_VK_SNAP_DUMP` at frame 1100 of such a run gives the
   dependency graph:

   | surface | non-black |
   |---|---|
   | `0684B000` scene colour | **100%** |
   | `06BE4000` scene depth | 99.9% |
   | `1439B000`/`143BB000`/`143DB000`/`143FB000` shadow cascades (depth) | 58% each |
   | `14338000`/`14359000`/`1437A000` colour-grading LUTs | 99.8% |
   | `149DC000` -> `14A54000` -> `14A72000` -> ... downsample pyramid | 100% .. 3% |
   | **`1439B000` (the COLOUR resolve to that address — the tone map)** | **0.00%** |
   | `147C0000` DOF blur | 0.00% |
   | `00E48000` FRONT BUFFER | 0.00% |

   So a live pass with live inputs produces black, and everything downstream of it is
   black — the same SHAPE as §6s, found with the same instrument. Note `1439B000` is
   BOTH a shadow cascade's depth destination and the tone map's colour destination in
   this frame; part 14 made those two separate snapshots, so `snap_1439B000_*_depth.ppm`
   and `snap_1439B000_*.ppm` are different surfaces and the black one is the colour.
   The obvious next step is the tone map's own inputs at THAT frame, from the resolve
   trace's `sampled snapshots` line — and note the budget is shared across frames, so
   `CZ_VK_RESOLVE_TRACE=N` should be armed at the frame you actually want with
   `CZ_VK_RESOLVE_TRACE_PASSES` set high.

2. **XAM ordinal `0x271` is resolved on the save-LOAD path and we answer NOT_FOUND**
   (`docs/phase3-notes.md` finding 51 — written this session and worth reading whole).
   With A3's real save installed (`Xenia logs/A3_save_content/cz_A3_save_DR2P000.zip`,
   laid out as `<CZ_SAVE_DIR>/DR2P000.DSF/DR2P000.DSF`), our content layer enumerates it
   correctly — `1 item(s)`, `XMsgCompleteIORequest(result=0)` — and the title reaches
   the save-slot panel, labels SLOT 1 **`Damaged Content`** and puts up `Load failed!
   Please check your storage device and try again`, having never opened the file.
   `imports.cpp`'s `kResolvable` is the SEVEN ordinals A1 resolves, and A1 was captured
   with no save present; A3 resolves an eighth at an adjacent mint slot. Do NOT mint a
   stub for it blind — a return value the title consumes rather than tests is gotchas
   59/201's trap — name it from the guest's own call site first, and note that
   `tools/gdis.py --find-uses 0x271` finds nothing, so the ordinal is not built by a
   plain `li`. The profile-signature question (an Xbox 360 save is signed per profile,
   and A3's was made under the fork's GUID) is separate and may make "Damaged Content"
   the right answer to THAT file even once the ordinal exists.

   **This closes part 12's black panels.** They are the save's THUMBNAIL, and black is
   the correct picture for a slot the title has no valid content for — which is what
   part 13's three hardware watchpoints were really saying.

3. **The last picture difference: colour is flat and green-shifted** (§6ad item 2). Much
   improved by part 14 and not closed. §6s proved this frame depends completely on the
   colour-grading LUT, which is the thing to look at. Judge any change over an ERA
   (gotchas 127, 133) and remember that a colour shift IS visible to
   `frame_compare.py`'s luminance and colour-count columns, unlike a blur.

4. **The conservative screen extent is still a placeholder** (unchanged since part 11).
   `WriteScreenExtent` answers "this draw may have touched anything", which makes bin
   predication a no-op and costs work. **Do not do this speculatively** — the cost has
   still not been shown to matter.

5. **The depth-resolve cost, if it ever matters.** ~6% of the frame rate, from four
   1280x720 shadow-cascade depth copies plus the scene depth's two tiles every frame.
   The obvious refinement is to snapshot a depth resolve only when some fetch has ever
   named that address with a depth format — but that is an optimisation with no measured
   problem behind it, so it needs evidence first.

6. **The kernel gates are exhausted as a forward oracle** (unchanged since part 9). A1's
   position 93 is not the next piece of work (finding 49, gotcha 107). Going further
   needs a gameplay comparison built from A2 — and the prologue run is the first this
   port has had that would exercise one.

## Traps this session paid for — do not re-buy them

* **A resolve has a SOURCE.** `RB_COPY_CONTROL & 7`: 0..3 a colour target, 4 the DEPTH
  buffer. Reading only the destination was worth 18.4% of this title's resolves and the
  entire frame's focus. The general form: when a subsystem is the suspect, census the
  CAPTURE by the field you are not reading — `tools/xtr_resolve_census.py` is the worked
  example and it took an hour.
* **A blur is invisible to every aggregate over pixel VALUES.** Coverage moved 0.01 pp
  between a uniformly out-of-focus frame and a sharp one — inside `frame_compare.py`'s
  own band. Gotcha 135 (the vertical flip) in a second disguise. When an operator can
  see something a purpose-built metric cannot, the metric is measuring the wrong
  quantity. `tools/frame_sharpness.py` measures the spatial derivative and separates
  those arms 6.47x.
* **A defect can validate the very label it corrupts.** `06BE4000` was checked as "the
  scene surface" repeatedly and always contained the scene, because our broken depth
  resolve put the colour buffer there. Only a field neither had ever printed separated
  them.
* **One address can be two surfaces in one frame** (`1439B000`: shadow cascade depth AND
  tone map colour). The fix is a wider KEY, not a rebuild — a rebuild is a device-wait
  and a fresh bindless slot twice a frame, i.e. gotcha 192's descriptor exhaustion.
* **A documented knob can not exist, for the second time in three sessions.**
  `CZ_VK_RESOLVE_TRACE_PASSES` was in CLAUDE.md from part 12 and read nowhere, and the
  budget it names still guarded only the header line — the exact defect part 9's note
  says it fixed. `grep -n <KNOB> runtime/` before quoting one.
* **A file-open counter is not a loading counter.** `#154` stopped climbing because the
  title stopped opening NEW files; `CZ_FILE_TRACE=1` shows the loading continuing out of
  containers already open.
* **Run timed arms serially** (gotcha 183) — unchanged.

## New instruments and arms

```
CZ_VK_NO_DEPTH_RESOLVE=1      snapshot the COLOUR target even for a DEPTH resolve, i.e.
                              the pre-part-14 renderer: the whole frame uniformly out of
                              focus at every depth, no sign lettering, no bunting
CZ_VK_RESOLVE_TRACE_PASSES=N  now real (default 20), and it budgets every line a pass
                              prints rather than the header alone
```

```
tools/xtr_resolve_census.py   every resolve in a capture by SOURCE and destination —
                              the oracle that named the depth resolves and the scene's
                              real colour address
tools/frame_sharpness.py      median mean-|gradient| per arm over an era — the only
                              metric in this project that can see a blur
```

`CZ_VK_SNAP_DUMP` now writes depth snapshots as `*_depth.ppm`, stretched between the
surface's own min and max (a perspective depth buffer is all within a hair of 1.0, so a
linear grey would be a white rectangle), and prints the true 24-bit range beside each.
