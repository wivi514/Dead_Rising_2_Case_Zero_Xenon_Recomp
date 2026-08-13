# Part 37 hand-off (for part 37). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part36-kickoff.md` for "where
the port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays cinematics; shadows and the
white-surface plateau are fixed and operator-confirmed. Part 36 did two things: it
**reframed the top picture item (0s)** by proving the "junk" impostor sheets are
byte-identical to hardware's, and it **built the reproducibility layer** the picture
work has always lacked — an F9 capture now records the player's world position and the
camera matrix, textures can be isolated live while an operator plays, and streaming
addresses were measured stable across boots so a census from one boot is usable in the
next. A teleport was attempted, its crash fully diagnosed (wrong thread, not
marshalling), and left unfinished for a named reason: the actor's position fields are
outputs of the physics body, so writing them cannot move anything.

## WHAT PART 36 DID — do not rebuild any of this

Records: `phase5-notes.md` §6bj (the junk-sheet reframe), §6bk (the operator pair),
§6bl (the console's two primitives, disassembled), §6bm (wiring them in), §6bn (the
teleport chased to the end). Gotchas 287-290.

* **Item 0s reframed.** Our live-dumped 400x240 and 1024x64 DXT5 sheets at blotch time
  are **md5-identical to the bytes hardware sampled** in `R3_world/tanker.xtr`, and
  decoded (`tools/tex_decode.py`) they are coherent billboard alpha-cutouts. The
  writer hunt is closed unfired. 226 of 459 blotch-frame textures byte-match hardware.
* **Both quality levels captured in one boot** (`~/DR2CZ-troubleshooting/part36-operator/`):
  `capture_002048` blotched close, `capture_005614` correct at distance.
* **The pose capture.** F9 writes `capture_<frame>.pose`: the player's world position
  (read via the console's own path) and the camera constants. `tools/pose_read.py`.
* **`CZ_VK_TEX_FILTER_FILE`** — isolate/hide textures live, no relaunch, proven
  capable of failing (a filter matching nothing takes the picture to luma 0.0).
* **Streaming addresses are stable across boots**: 703 of 712 shared addresses held
  identical content between two sessions.
* **The player's position is `obj + 0x1C`**, reached by the console's five-step lookup;
  the runtime reads it with no guest call at all.

## WHERE TO START

1. **Item 0s — name the blotched draw.** The tools are all in place now: addresses are
   stable, `CZ_VK_TEX_FILTER_FILE` isolates a texture while the operator stands in
   front of the defect, and the blotch captures carry poses. Pick candidates from
   `capture_f2048.census`, isolate, and see what vanishes. Then decode its texture
   (`tex_decode.py`) and compare with hardware's for the same surface.
   **Do not** pair draws by vertex count across two frames — 115 false differences,
   §6bk.
2. **The teleport, if reproducibility is wanted end to end.** The crash is solved
   (gotcha 289) and the calls run clean on an engine thread. What remains: the actor's
   position fields are OUTPUTS (gotcha 290), so the real placement path is elsewhere.
   **The strong lead: DebugJump already places the player successfully every spawn** —
   follow `DebugJump` / `cSpawnPoint` / `LevelSpawnPoint` to whatever it calls, or
   `cMissionTeleportPlayer` (the image names `missionteleportplayer.cpp`). Both are
   the game's own working code, unlike `setplayerpos`, which was never verified to do
   anything from any state.
3. **The Xenia one-look for item 00i** (promotion distance of the flat-panel shop) —
   owed since part 35, one deliberate look.
4. **Hardware's 16 small colour resolves** in the tanker frame (64x64 x9, 128x64 x4,
   128x128, 512x256) are in no resolve set of ours; resolve write-back to guest memory
   is still unimplemented and is the standing lead for the remaining sub-defects
   (notably the 231 colour fetches served by a DEPTH snapshot).
5. `docs/perf-cpu-plan.md`'s CPU/GPU overlap — still the largest performance item.

## READ THIS BEFORE MEASURING ANYTHING

* Everything from parts 26-35 stands, plus gotchas 287-290.
* **A junk SCORE is not a picture** (287) — decode and look before believing memory
  holds garbage.
* **Content-hash matching exonerates only the matches** (288) — unmatched is
  unadjudicated, not suspect.
* **A frame's FIRST draw is the shadow pass**, so its view matrix is the light's; the
  pose records the biggest draw's matrix too. And this title renders **camera-relative**
  (the scene view matrix solves to the origin), so a pose carries camera ORIENTATION,
  not camera position.
* An F9 capture arms the position read; it is measured safe (0 faults over 32,295 log
  lines against a duration-matched control's 0 over 31,994).

## Gates, on this binary

* `--smoke` OK. No renderer change in part 36 beyond the pose capture; part 34's
  picture gates stand.
* A capture-armed 300 s run: 0 guest faults, 22,964 lines.
* A5 diff not re-run this part — re-run before any claim resting on it.
