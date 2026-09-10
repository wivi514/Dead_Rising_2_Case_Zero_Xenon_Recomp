# Picture plan, part 109 — the four operator-reported defects of 2026-09-09

Written at the end of part 108 on the operator's instruction: *"prepare the plan to fix
all of these."* The four are `open-items.md` 0ab, 0ac, 0ad and 0ae. Every one has a
capture from the operator's own session under `~/DR2CZ-troubleshooting/part108/`
(`tile-shadow/` for 0ab, `bugs/` for the rest), each with that frame's 315 resolve
snapshots, its draw census and its pose file. **Nothing here is measured yet**; the
order below is by how well the mechanism is understood and how cheap the first step
is, not by how much the defect matters to a player.

The rules the plan is under, in order of how often they have been broken:

* **Ask the oracle before touching code.** Capture E and the R2 traces show the neon
  lit where it is and near actors lit across the centre, so 0ab/0ae are ours; 0ac and
  0ad have NOT been asked of Xenia (no capture reaches a door transition or that
  camera spot) — 0ac's first step is to look at what hardware draws for the fade.
* **A picture claim needs the operator's eye** (gotcha 190: a gate that needs a human
  is a capture request in disguise — so each item's gate names what the operator must
  see, AND the headless number that stands beside it).
* **Two arms are comparable only where they render the same draw set** (A/B
  admissibility). Every fix below ships with a same-binary control arm.
* **One change per experiment; state the prediction before the run.**

## §0 Order, and why

| order | item | mechanism | first step costs | operator needed |
|---|---|---|---|---|
| 1 | **0ad** door transition at the wrong ratio (21:9 only, until the player moves) | KNOWN to the line: the renderer's wide patch runs on every draw, the guest-side fov substitution fires at ONE call site (the roaming camera's); any other camera class renders un-widened and the patch squeezes it | one traced door | one door at 21:9, then the fix by eye |
| 2 | **0ab + 0ae** near actors black from the screen centre rightward; a light's glow on the opposite side | HYPOTHESIS: a screen-space pass done per tile with the other tile's offset — one root, two symptoms | a script over the saved snapshots (no run) | a 720p pose at the sign, then the fix by eye |
| 3 | **0ac** Chuck's camera-fade copy stretched | OPEN: a draw with another draw's constants, or the title's own fade | an F8 burst at the spot | the burst, and the oracle |

0ad first because its mechanism is already known from the code and a fix is a day
at most; 0ab/0ae second because they are one investigation and the largest visible
defect; 0ac last because it has no mechanism yet and needs both an oracle answer and
a burst before anything can be designed.

## §1 Item 0ad — the door transition at the wrong ratio

### 1.0 What is known (from the code, not yet from a run)

* `cpu/camera_fov.cpp`: the guest-side fov substitution (part 62) hands the game a
  fov widened by `k` (wide mode: k = 9W/16H; narrow mode: 1/k) so its own 16:9
  culling frustum covers the rendered view — **at exactly one call site, lr
  `0x8246E31C`, the OverShoulderCam's FOV property read.** Any camera that does not
  read its fov through that site keeps the game's authored fov.
* `gpu/vk_renderer.cpp` (~23890, the "composite wide patch", part 62 §6cu): in wide
  mode EVERY draw's clip position has its y scaled by k (narrow mode: x by 1/k),
  which is correct ONLY when paired with the substitution — it narrows back what
  the substitution widened. Unpaired, it squeezes a 16:9 frustum vertically: every
  object looks wider than it is. **That is the capture** (`bugs/capture_012535`).
* The operator's two facts fit exactly: it persists after the door until the player
  MOVES (the roaming camera resumes, the site fires again, the pair is restored);
  it only happens at 21:9 (at 16:9 k = 1 and the patch is identity).

### 1.1 The census, before any fix (one run, ~5 minutes of the operator's time)

`CZ_FOV_PARAM_TRACE=1` prints each (call site, value) pair the FOV param getter
serves, once per distinct pair. Walk through a door at 21:9. Prediction: a call site
other than `0x8246E31C` appears during the transition (the door camera's fov read),
and the roaming site's line does not repeat until the first movement. If NO other
site appears, the door camera does not read a fov property at all and builds its
projection elsewhere — then §1.2b is the only fix and §1.2a is void.

### 1.2 Two fixes, in order — (a) is the right one, (b) is the safety net

* **(a) Substitute at the door camera's site too.** Add its lr to the substitution's
  site set (the same base-capture-then-enforce form; the field is state — the +N form
  compounded, part 62). Prediction: the transition renders at the right ratio at
  21:9 and 16:9 alike. Risk: if the door camera's fov node is shared with something
  the cinematic system reads, a cinematic could widen too — the trace's `this`
  addresses say whether the nodes are distinct.
* **(b) Pair the patch with the substitution per frame.** The renderer applies the
  wide/narrow patch only on frames where the substitution fired since the last
  present (a flag the hook sets, the renderer clears at present). Unpaired frames
  render the game's own frustum, stretched to the window by the present blit — the
  pre-part-62 picture, which at 21:9 is "stretched", not "squeezed", and only for
  the frames of a camera class nobody substitutes for. Prediction: 0ad vanishes
  wherever (a) is missing a site, and NOTHING changes on frames the roaming camera
  owns (the flag is set every one of them). This is the fix that also covers the
  next camera class nobody has found yet.
* Ship (a) AND (b). `CZ_NO_GAME_FOV=1` remains the control for both (it removes the
  substitution and, with (b), the patch — the pre-part-62 picture at 21:9).

### 1.3 Gates

* The operator: the same door at 3440x1440, before and after, then a cinematic and
  the roaming camera unchanged. (Their capture 012535's pose is the spot.)
* Headless: `CZ_VK_LIVE_RES_TEST` is not needed; the per-draw counter `draw: wide
  projection patch SKIPPED (no substitution this frame)` (to add with (b)) must read
  > 0 on a door and 0 across a roaming-camera soak. Gotcha 151: a counter that
  cannot be shown to have engaged is not an arm.

## §2 Items 0ab + 0ae — the tile seam

**0ae LEFT THIS SECTION THE SAME EVENING.** The operator clarified the report (the
dark neon letters are the game's flicker; the defect is content past one edge showing
at the opposite edge, on the title screen too), which is a sampler WRAP, and the
fetch constants' clamp modes — deferred since part 41 — are honoured as of the last
build of part 108 (§6ey addendum 7, gotcha 540; `CZ_VK_NO_FETCH_CLAMP=1`). Title
screen verified by the operator; the neon sign is owed. §2 below now covers 0ab alone;
its step 1 (the snapshot halves, `tools/snap_halves.py`, written) still applies.

### 2.0 What is known

* The title renders the scene in two 640-wide tiles at 720p (window scissors
  `0..640` / `640..1280`, `PA_SC_WINDOW_OFFSET` −640 for the right one — the
  constitution note in CLAUDE.md; §6v for the predication that goes with it). Our
  EDRAM is full-size and every tile lands at its true screen position; the renderer's
  own comment (vk_renderer.cpp ~24070) records that the window-coordinate path's
  offset undo has executed ZERO times over a whole boot — every tiled draw takes the
  viewport path.
* 0ab (`tile-shadow/capture_023219`): Chuck lit on the left half, black from x =
  width/2 rightward; the right-edge zombie black entire; the mid-distance crowd fine
  on both halves. Only NEAR actors.
* 0ae (`bugs/capture_028086/028693/030293`): the neon's glow lands on the other side
  of the screen's centre, letters dark on one side and lit on the other, moving with
  the camera. Three frames of one camera pan.
* Both seams are at the screen's exact centre and both are screen-space effects
  (a near shadow/light term; the bloom chain). **One hypothesis covers both**: a
  pass that samples a screen-space buffer per tile with the OTHER tile's window
  offset (or with offset 0 where the title expects −640), so the right half reads
  the left half's data — shadow for 0ab, glow for 0ae. A second, weaker hypothesis
  for 0ab alone: a per-tile constant (a light or shadow matrix) published once for
  both tiles.

### 2.1 Step 1 — the saved snapshots, no run (an hour)

Every resolve snapshot of the four frames is on disk. Write `tools/snap_halves.py`:
for each snapshot of a frame, the mean luma of its left and right halves and the
correlation between the right half and the left half MIRRORED — a screen-space
buffer that should be symmetric under the camera pan (the neon is centred in
028693) but is not, or one whose right half correlates with the left half's
content, names the pass. Report the top asymmetries per frame with the snapshot's
guest address and extent, then find the pass that WRITES that address in the
census/`CZ_VK_GPU_PASSES` extent census (the extents identify half/quarter-res
bloom targets against the full-res shadow mask). Prediction: for 0ae one of the
half- or quarter-res snapshots carries the glow on the wrong side; for 0ab a
full-res single-channel snapshot (the shadow mask) is dark on the right half.

### 2.2 Step 2 — the pass's registers at the pose (one operator run, 10 minutes)

`CZ_VK_VIEWPORT_TRACE`-class output already exists: the `[vkvp]` line prints
viewport, scissor, `winoff`, posScale/posOffset and surface info per distinct
state, 64 lines max. At the diner pose at 720p (`CZ_VK_RES=1280x720`, the arm
where the seam must sit at x = 640 if it is the tile — the falsifier for the whole
hypothesis), read the named pass's draws: which tile's `winoff` they carry and
whether their scissor is the tile's or full-screen. Prediction: the offending pass
draws full-screen with `winoff = 0` while sampling a buffer the title wrote per
tile — or draws per tile with the offset applied to the vertex position but not to
the texture coordinate it derives from `oPos` (the shader's screen-space UV), which
is the classic Xenos tiling trap: `PA_SC_WINDOW_OFFSET` moves the RASTER position,
and a shader computing UV from the interpolated position gets the tile-local one.

### 2.3 The fix, by mechanism

* If the shader derives its screen UV from position: the renderer must hand the
  shader the SCREEN position — either by not undoing the offset in the vertex path
  for that pass (and writing the tile into its true position via the viewport
  instead) or by publishing the tile offset as a constant the translated shader
  adds (XenosRecomp's `g_...` shared constants block has the window offset field
  Xenia uses for exactly this — check `xenos.h`'s shared-constants transcription
  in the vk_renderer.cpp header before adding a new one).
* If it is a per-tile constant published once: the constant gather (part 74) must
  key on the tile identity (`windowOffset` is already in the draw record at
  vk_renderer.cpp:3795, "the TILE identity").
* Control arm: the fix behind `CZ_VK_NO_TILE_UV_FIX=1` (name to taste); prediction
  under the arm: the captures' seams return.

### 2.4 Gates

* The operator: the diner sign from the roof and a grab at the crowd, both at
  3440x1440 and at 1280x720, before and after. Their word is the gate.
* Headless: `tools/snap_halves.py` on the same poses — the named snapshot's
  asymmetry must fall to the null (measure the null on a symmetric scene first,
  gotcha 51: the control is measured NOW, not remembered).
* Sync validation 0 hazards / poison 30, the A5 gate, `truncated=0` — any change to
  a pass's ordering or its barriers re-runs them.

## §3 Item 0ac — Chuck's fade copy stretched

### 3.0 What is known

`bugs/capture_009604`: camera close behind Chuck at the safehouse save door; a
translucent copy of his jacket smeared to the right, sleeves in streaks. The title
draws Chuck translucent when the camera is near (the fade), so the copy is expected;
the streaks are not. The census (`capture_f9604.census`, 1,150 draws) lists every
draw of that frame with its blend state and constant `pc255` — the two Chuck draws
(the same vertex count, one opaque and one blended) are in it.

### 3.1 Steps, in order

1. **The oracle.** No Xenia capture reaches this spot; the R2 traces are single
   frames elsewhere. Ask the operator for a Xenia run at the save door with the
   camera pushed in (one screenshot is enough): if hardware smears the fade copy
   too, this is the title's and the item closes. Do not skip this — the last four
   "our defect" pictures that were not asked of the oracle cost parts.
2. **An F8 burst** at the pose with the camera held (STILL, gotcha: hold the camera
   for a picture gate): a race (a draw picking up another draw's constants — the
   store mirror's generation stamp, the parallel guard, the constant gather)
   FLICKERS frame to frame; a wrong constant holds still. The burst manifest's draw
   fingerprints say which.
3. **Name the draw** with `CZ_VK_DRAW_ID` at the pose; diff the two Chuck draws'
   constant files from the census (bones are `pc` constants; the fade draw should
   carry the same bone block as the opaque one). If they differ, the bisection order
   is `CZ_VK_NO_STORE_MIRROR=1`, then `CZ_VK_NO_PARALLEL_GUARD=1`, then the gather's
   arm (part 74, §6dj) — one run each, the burst as the readout.
4. If the constants agree and the mesh still streaks, it is the VERTEX data: the
   fade draw's stream bound to a stale generation of the skinned buffer (the mirror
   copies last frame's written ranges; a draw whose stream was written THIS frame
   after the copy was queued binds stale bytes) — `CZ_VK_NO_STORE_MIRROR=1` alone
   settles that, and the fix is in the mirror's hit test, not in the fade.

### 3.2 Gates

The operator at the save door with the camera pushed in, before and after; the burst
manifest's fingerprints identical across the burst after the fix (no flicker) and the
picture right by eye. Kill: if step 1 says hardware does it too, close the item and
say so in place.

## §4 What already exists (do not rewrite)

* Captures with snapshot sets, censuses and poses for all four items
  (`~/DR2CZ-troubleshooting/part108/{tile-shadow,bugs}/`, INDEX.md entries).
* `CZ_FOV_PARAM_TRACE=1`, `CZ_NO_GAME_FOV=1`, `CZ_VK_FOV`, the wide/narrow patch
  and its user-clip-plane compensation (`camera_fov.cpp`, vk_renderer.cpp ~23880).
* The `[vkvp]` viewport/scissor/window-offset trace (64 distinct lines) and the
  `draw: window coordinates moved to the tile's screen origin` counter.
* `CZ_VK_GPU_PASSES=1` with the pass EXTENT census; `CZ_VK_DRAW_ID`; F8 bursts with
  fingerprints; F9 with every resolve snapshot; `CZ_VK_NO_STORE_MIRROR`,
  `CZ_VK_NO_PARALLEL_GUARD`, the gather arm.
* `~/DR2CZ-troubleshooting/part108/watched_play.sh <tag> ENV=..` for every operator
  session with a new trace (gotcha 537).

## §5 Owed from the operator, in the order the plan needs them

1. One door at 3440x1440 with `CZ_FOV_PARAM_TRACE=1` (§1.1) — five minutes.
2. The diner sign from the roof at 1280x720, F9 (§2.2) — five minutes.
3. A Xenia screenshot at the save door with the camera pushed in (§3.1) — their
   Windows box.
4. An F8 burst at the same spot in our runtime, camera held (§3.1 step 2).
