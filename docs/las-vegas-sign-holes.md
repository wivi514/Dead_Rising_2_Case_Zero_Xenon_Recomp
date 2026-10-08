# The "Las Vegas 46" highway sign: holes while the camera moves (open item 0zj)

**Status: OPEN, parked 2026-10-08 by the operator ("will come back to it later").** Ours,
not the title's: real hardware is clean (the operator's own console). Long-standing:
present before any change of 2026-10-07/08. The 06:14 launcher capture
(`~/.config/XenonLive/captures/20261008-061428-c8c0`, build `248fe89`) predates the hair
fix and shows it, and the operator says it has "been there a long time". Minor; it was
deliberately left while other things were fixed.

## The symptom

The green highway distance sign ("Las Vegas 46") right outside the safehouse exit, seen
from far down the street (~70 m). **Only while the camera MOVES** does part of the panel
go missing for one frame, roughly 1 frame in 7. **Standing still it is always clean**, and
up close it never happens. The missing part has straight, stepped or torn edges, often a
triangle or a wedge at a corner. Through it you see what is behind: the dirt mound, the
haze, the trees, and the sign's own poles (operator, by eye). Roughly 1 frame in 7 at that
spot while moving.

## Where the evidence is

All under `~/DR2CZ-troubleshooting/play/` (dev-build `tools/play_session.sh` sessions,
`SAFE=1`; every arm's engagement line is in its own `<tag>.log`):

| folder | what it holds |
|---|---|
| `hairfix_1008_0152` | first full-res bursts: bursts 08/09, bad frames 169864, 169866, 170147 |
| `sign_arm*_*` | one folder per control arm (table below) |
| `sign_drawid2` | draw-ID F9, frame 11651 (a GOOD frame), plus the normal burst before it |
| `sign_arm7_depthfloat` | F9 12369, a BAD frame with every resolve snapshot |
| `sign_arm9_fps30` | F9 4791 (good) and 4847 (bad), 30 fps |
| `sign_idxdump` | 37 F9s with `CZ_CAPTURE_DUMP_VS` of the 9,039-index batch; 12690/12694 bad |
| `sign_fulldepth` | F9s with the FULL-PRECISION depth (`*_depth.ppm.f32`); **11215 bad** |
| `sign_zombiedump` | 9 F9s with every crowd-zombie draw dumped (41,148 dumps) |

**Trimmed 2026-10-08** (`~/DR2CZ-troubleshooting/CLEANUP-2026-10-08.md`). Only the
frames cited here are kept:
- `sign_fulldepth` 11215 (bad, with `.f32`) and 10974 (good);
- `sign_idxdump` 12662, 12666, 12690 and 12694, with their dumps;
- `sign_arm7_depthfloat` 12369; `sign_arm9_fps30` 4791/4847;
- `sign_arm4_worldlod0` burst 08; `sign_drawid2` whole; `hairfix_1008_0152` bursts 08/09.

The 41,148 zombie dumps are gone (that theory is refuted). Logs and the PNG/INDEX files
are kept.

## What is ESTABLISHED (measured)

1. **The draw list is the same in good and bad frames.** Same draw keys, render state,
   texture addresses, sizes, mips and bindless slots (census of 170147 against six good
   frames; again at 33599). The only unique keys are CPU-skinned crowd draws whose first
   vertex moves every frame.
2. **The sign is part of draw "1984" in `sign_drawid2`.** That is a 9,039-index triangle
   strip, `vs_fa161b0fde7aa4d5` / `ps_2d2aadd8122f1160`, a merged batch of statics with
   5,290 vertices, two DXT textures, one pass at LEQUAL with depth write, opaque. 2,414
   of its 2,415 painted pixels in that frame are the sign.
3. **That batch's vertex AND index bytes are byte-identical in all 74 dumps**
   (`sign_idxdump`, both tile replays, good and bad frames). Its constants change only
   as the camera does.
4. **The hole is in the scene colour BEFORE the post chain.** It is already present in the
   first scene resolve `0684B000` (12369), so blur, fog and grading are innocent.
5. **The hole lies on the panel's depth plane.** In `sign_fulldepth/f011215`, a plane fit
   to the panel's own pixels has residual rms 6e-8. All 182 hole pixels are within
   1.6e-7 of it, with a median of +1.06e-7: millimetres at 70 m, where 1 m is about
   1.9e-5 of depth here. Poles centimetres behind would read several times further off.
   **So whatever wrote the hole's colour is at the panel's depth to within about 5 mm.**
   The colour is flat peach (the mound and the haze), and the poles still cross over it.
6. **No world draw writes colour after the scene starts without a depth test.** The only
   `dc` with z disabled are four draws before the scene (24-27).
7. **No world draw samples a scene-colour snapshot.** Only the post-chain quads at the end
   of the frame (9402+) do.
8. **The camera is a near-degenerate depth projection.** In c0..c3, the z row and the w
   row differ by about 0.1 out of 180, so depth ≈ 1 − 0.092/w and the cancellation leaves
   millimetre resolution at 70 m. Recorded because any depth-precision theory starts here.

## REFUTED (each arm confirmed engaged in its own log, and each still showed the holes)

| arm | |
|---|---|
| `CZ_VK_NO_DEFERRED_CLEAR=1` | scoped clears |
| `CZ_VK_NO_PARALLEL_GUARD=1` | frame-ahead guards AND parallel record (serial path) |
| `CZ_WORLD_LOD=0` | WORLD DETAIL (seen by eye; 0 of 22 burst frames caught it) |
| `CZ_VK_STREAM_GUARD_EXACT=1` | the stream store serving stale bytes |
| `CZ_VK_NO_STORE_MIRROR=1` | the VRAM mirror racing a copy |
| `CZ_VK_DEPTH_FLOAT=1` | D24 UNORM vs D32F (still breaks on float) |
| `CZ_PUMP_SPLIT=0` | the two-core pump reading late |
| `FPS=30` (`CZ_FPS_CAP=30`) | the title's sim/render lag above 30 fps |
| `CZ_VK_CONST_GATHER=0 CZ_VK_NO_CONST_MEMO=1 CZ_VK_NO_PATCH_MEMO=1` | the constant caches |
| `CZ_VK_MSAA=1` | MSAA, foliage per-sample depth, alpha-to-coverage |

Also refuted by analysis, not by an arm:

- **User clip planes.** `ucp=0` on every draw.
- **A second EQUAL pass of the sign** (the hair mechanism). The sign has one pass. The
  EQUAL passes in the frame are blood and decal layers that share their geometry's own
  vertex shader, plus the hair.
- **The wide patch.** It rewrites only an x or y row, never depth.
- **A post-process or scene-copy overlay.** See points 4 and 7 above.
- **A stretched crowd zombie.** `tools/vsdump_skin.py` replays `vs_b677dc3457f5b41a`'s
  skinning exactly on the dumped inputs. Over 9 F9 frames and 41,148 zombie draws the
  widest zombie spans 0.03 NDC, every vertex's weights sum to 1, and there are no restart
  or out-of-range indices.

## UNEXPLAINED

- **The horizon band.** In the draw-ID map of 11651 (a good frame), pixel value 4656,
  read as census draw 4655, covers 68,614 px: x 892..1719, y 335..473, the left tile
  only, with trees and poles cut out of it, directly behind the sign. The census calls
  4655 a 392-index crowd zombie. Either the draw-ID ↔ census numbering drifts by then
  (it was checked only around draw 1984), or that band is something else. **Settle this
  first when the item resumes.**
- **What is coplanar with the panel.** The hole's writer is at the panel's depth to
  about 5 mm, its colour is the background's, and it is depth-tested.

## The next measurement (decided, not run)

A **draw-ID map of a BAD frame**: `SAFE=1 tools/play_session.sh CZ_VK_DRAW_ID=1`, then F9
every second or two while moving (15-20 presses). Scan each map for pixels inside the
panel's footprint (the batch's IDs) that another draw owns, and read that draw's census
line. This names the writer outright. Check the numbering drift on a known late draw
before trusting an index above ~2,000.

## Instruments built for this (all committed)

- `CZ_CAPTURE_DUMP_VS=<vs hash>` (+ `CZ_CAPTURE_DUMP_VERTS=N`) writes per-draw constants,
  stream bytes and index bytes on an F9 frame (`docs/instruments.md`).
- `tools/vsdump_skin.py` replays crowd-zombie skinning from those dumps.
- A depth snapshot now also writes `<name>.ppm.f32`, full-precision depth for
  screen-sized surfaces. The 8-bit PPM is stretched over the frame's whole range, about
  0.00009 of depth a level here, which is metres at 70 m. It could not tell the panel
  from the poles behind it (gotcha 632).

## Method notes for whoever resumes

- **Read a hole's DEPTH before its colour.** Plane-fit the surface's own pixels, then
  compare the hole. That one comparison split "never drawn" from "overwritten at the same
  depth", which ten arms had not.
- **A vertex-extent replay cannot see bad CONNECTIVITY.** Check the index stream as well:
  restart markers, out-of-range indices (gotcha 633).
- **The operator's eye ("I can see the poles behind it") refuted two of my mechanisms in
  one sentence each.** Ask what is visible THROUGH a hole before building on a picture.
