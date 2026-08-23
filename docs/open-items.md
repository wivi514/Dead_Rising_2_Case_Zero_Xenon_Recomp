# Open items, in order

**Split out of `CLAUDE.md` on 2026-08-08.** The actionable backlog. Items struck through
are closed or retracted and are kept because a retraction is worth as much as a finding
— several of these were "fixed" by something else entirely, and the record of what was
NOT the cause is what stops the next session re-buying it.

Next, in order:

0v. **RT SHADOWS — ROUTE (a) IS CLOSED AS UNWORKABLE; ROUTE (b) IS BUILT, AND
   PART 67 FOUND AND FIXED THE REASON IT PRODUCED NOTHING. ONE OPERATOR SESSION
   IS OWED AND IT IS SCRIPTED.** The records are `phase5-notes.md` §6cv (route
   (a), §7j is the verdict), §6cw (route (b) built, part 65), §6cx (part 66 —
   the ordering finding and the primary-ray receiver) and **§6cy (part 67 — the
   position streams are OBJECT-SPACE, which is the whole of "the TLAS is a
   ground plane")**; the hand-off is `docs/part68-kickoff.md`; the arms are in
   `instruments.md`; the lessons are gotchas 381-386, 387-390, 391-397 and
   **398-400**.

   **PART 67'S FINDING, which supersedes the part-67 kickoff's §1 entirely: no
   filter is eating the buildings — every one of them is collected and every one
   of them is at the WORLD ORIGIN.** `Collect` gates on
   `SceneXformForm(c0..c3) == 2` and §6cs read that as "so the position stream
   is world-space"; c0..c3 is the CAMERA's view-projection and is the same
   matrix whether the shader feeds it a world position or an object position it
   transformed one line earlier, which is what this title's world shaders do
   (a row-major 4x3 at `vc(8..10)`). Over the twenty `.xtr` traces and 46,820
   accepted draws, the fraction whose box intersects the frustum it was drawn
   into is **11.7% untransformed and 98.6% placed**, and 100% of them carry a
   non-identity world translation. The `tlasInst=216..722` three parts read as
   "the collector is dropping the buildings" was the DISTINCT MESH count.

   **Shipped**: every TLAS instance now carries the draw's own object->world
   matrix, from the constant rows `config/rt_world_xform.json` names for that
   shader (read out of the microcode by `tools/rt_world_xform_census.py`, which
   is also a coverage gate). `CZ_VK_RT_OBJ_XFORM=0` is the same-binary control.

   **PART 68 RAN THAT SESSION AND TWO MORE.** Every pre-registered prediction was
   met — the hemisphere probe 0.987 -> 0.650, the shipped path 0.8% -> 14.6%
   shadowed, with the `CZ_VK_RT_OBJ_XFORM=0` controls reproducing part 66 exactly
   — and the placement was then EXONERATED offline against hardware
   (`tools/rt_placement_render.py` lands 2.9M placed vertices on Chuck, the
   zombies, the lamp posts and the kerb of hardware's own frame).

   **The remaining defect is the POPULATION, and it has two halves.** The ray
   structure rendered from the player's camera is a flat plain with distant
   buildings — the primary ray sails past the foreground and the factor computed
   behind it is painted onto near pixels ("misaligned", "seen through walls").
   `dyn` costs 17-41% depending on where the camera is (`CZ_VK_RT_DYN_SETTLE=N`
   admits the settled ones: `tlasInst` 2586 -> 3006, zero flushes), and the actors
   are misplaced by the palette approximation — measured on the factor image at
   **100.5 edge pixels per 1000 in the crowd against 10.6 on open road**.
   `CZ_VK_RT_NO_PALETTE=1` removes that artifact and **60% of the world's
   occluders** with it, so exclusion is a diagnostic and the blend is required.

   **THE PLAN IS `docs/rt-remix-plan.md`**, taken from `docs/rtx-remix-prior-art.md`:
   five items in the order they enable each other — bind the BLAS to the persist
   store (one usage flag), an identity that survives a content change, BLAS refit,
   the palette blend, then retire the workarounds AND re-ask part 67's
   exonerations against the new structure (gotcha 172: the sun, ray length and
   bias were all cleared against a pile at the origin, which is no test).

   **PART 69 BUILT ITEMS 0-3, RAN THE SESSION, AND THE ANSWER CHANGES THE SUBJECT:
   THE OCCLUDER SET IS NO LONGER THE DEFECT.** The records are `phase5-notes.md`
   §6da (what was built) and **§6db (the session and the two findings)**; the
   document to execute is **`docs/part69-night-plan.md`, §3**; the hand-off is
   `docs/part70-kickoff.md`; the lessons are gotchas 403-410.

   **THE PRIMARY RAY RESOLVES THE REAL WORLD.** `CZ_VK_RT_FACTOR_DEBUG=18` renders
   the primary ray's hit distance, and at `CZ_VK_RT_DYN_SETTLE=0` (`tlasInst=3366`,
   `flushes=0`) it is a recognisable depth image — Chuck's silhouette, both lamp
   posts with their arms, the power lines, the gantry, the fence, the hills behind,
   octiles spread across the range. Part 68 read the same instrument class as *"a
   flat plain with distant buildings — no vans, no wrecked cars, no fence, no
   Chuck"*. The population work fixed exactly that.

   **AND THE SHADOWS ARE STILL WRONG.** The operator, on the shipped pair: *"the
   shadows was under them not placed in the right way and passing through thing it
   shouldn't"*. The settle-0 arm — the first time the ACTORS were in the structure —
   doubled the shadowed share (18.5% -> 39.7%) and produced a flat slab with a hard
   straight boundary across a container, tyres, cars, a fence and the ground,
   bending at none of them. **Four rounds of occluder work have not moved that
   signature** (gotcha 409).

   **THE LIVE LEAD IS THE SUN**, censused rather than argued: windowed runs latch
   `(-0.364 0.546 -0.755)` and headless runs `(-0.371 0.557 +0.743)` — two
   components agreeing to 2% while the third flips sign, which no day/night cycle
   produces. **Every headless RT measurement this feature has ever made was taken
   with the mirrored one**, including part 66's 0.987 and part 67's 0.650. Settle
   which is right using the title's OWN raster shadows as the oracle before touching
   anything (gotcha 410).

   **Items 1 and 2 are PROVEN on the operator's machine**: `tlasInst=5001
   blas=8029 (148.6 MB) built=8058 flushes=0` at settle 0, the configuration that
   before them climbed to the 1 GB cap. Item 4 (the settle default, and re-asking
   part 67's exonerations) is still open.

   **Two errors are recorded in place**: the pre-registered edge-density gate is
   RETRACTED — its SIGN was wrong, since several separate actor shadows produce more
   boundary than one smear — and `tools/part69_menu_flicker.sh` shipped a control
   arm that never engaged (gotcha 408).

   **The palette approximation was not an approximation.** The plan's item 3 began
   with an offline census rather than a build (`tools/rt_palette_census.py`), and
   over 2,786 palette draws in the gas-station trace **ZERO reference a single
   matrix** — median 19 distinct entries, maximum 28, in steps of three. §6cx's
   "right for the static world and wrong for a skinned actor" is wrong for both.
   Entry 0 applies a RIGID transform, so it preserves shape and only position can
   be wrong; measured, it collapses the median draw's extent to **1.51 units where
   the blend assembles it at 8.75**, and moves 22.4% of draws by more than a world
   unit (90th percentile 15.46). The blend is now baked into the BLAS vertices with
   the instance carrying only the outer stage. `CZ_VK_RT_NO_BAKE=1` is the arm.

   **And the frustum oracle could not judge it.** The test that carried part 67
   (what fraction of vertices lands in the frustum the draw was issued into) reads
   **96.55% for entry 0 and 96.38% for the blend** — a saturated statistic, because
   collapsing a batch onto one of its own members moves a vertex by metres against
   a hundreds-of-metres test. The width and displacement statistics above are the
   ones with range, and `tools/rt_placement_render.py --blend` is the visual oracle.

   **Items 0-2 shipped with the identity fix the counters demanded.** A counter
   added only to make a suspected case visible (`palConflict`) read **2,364,245
   against 4,718,587 placements**: half of every palette draw is the SAME vertex
   buffer redrawn in the same frame under a DIFFERENT palette, which is the
   batching mechanism itself. A baked mesh's identity therefore includes its
   occurrence ordinal within the frame, and at a matched 4,097 RT passes on the
   headless outdoor route that took **`tlasInst` 682 -> 3,356** with `conflict`
   falling to 2,227. The structure is stable at `CZ_VK_RT_DYN_SETTLE=0` — the
   configuration that used to climb to the cap — with `blas=4493 (78.6 MB,
   built=4509, flushes=0)`, which is item 2's own gate met headlessly.

   **PART 66'S FINDING, which supersedes the part-66 kickoff's §0 entirely:
   THIS TITLE HAS NO SCENE Z PREPASS.** The factor pass reconstructed the
   receiver from the scene depth buffer and fired at the title's own first
   atlas-sampling draw, on the strength of §6u's "233,155 depth-only draws over
   a boot". Walked in ORDER across all twenty `.xtr` world traces
   (`tools/rt_depth_order_census.py`), those depth-only draws are the SHADOW
   CASCADE — a different EDRAM depth surface, its own pitch, edram_mode 5,
   colour mask 0, ~969 a frame — and the scene pass has no prepass at all: its
   **first draw already samples the cascade atlas**, with 0 depth-writing draws
   before it and ~5,200 (2.0M verts) after it. So the depth buffer was at its
   CLEAR VALUE every time the pass ran, which is exactly what ladder modes 8
   and 9 measured. The part-66 kickoff's "the live suspect is the pass's
   texture bindings" is **refuted**, and its mode 12/13 colour evidence is
   **retracted** — `g_colour` was never bound (a three-element write array
   passed with a count of two, which the validation layer reported on the first
   draw of every RT run).

   **The fix, shipped**: the receiver is now the closest hit of a PRIMARY RAY
   from the camera into the same TLAS the shadow ray uses. The TLAS is built
   from the PREVIOUS frame's draws, so it is populated whatever the title's
   draw order is — impossible by construction rather than timed around. It also
   removes the per-resolve invalidation (3.01 passes a frame become 1). Its
   known cost: pixels covered by skinned actors or alpha-tested foliage take the
   factor of the surface behind them, because those meshes are not in the TLAS —
   the same hole that already makes them cast no shadow, and buying them back
   fixes both halves at once. `CZ_VK_RT_FACTOR_SOURCE=depth` is the control arm
   and **its expected result is no shadows at all**.

   **THE FIVE OPERATOR SESSIONS RAN, AND THEY LOCATED THE DEFECT.** §6cx §7-8.
   In order: the operator found a **427-pixel vertical misalignment** in one
   sentence on the first arm (fixed; part 65's spatial control was a horizontal
   stripe and structurally blind to it, gotcha 394); `CZ_VK_RT_FACTOR_READBACK`
   then read our own factor image directly and settled four links in ninety
   minutes — poison 100.0% shadowed (the instrument's own control), the stripe
   pair a clean transpose in both axes, the primary ray finding a receiver on
   **85.2%** of the screen with a mask matching the captured frame's skyline, and
   the real path at **0.9%**; four arms on the shadow ray exonerated its length,
   its bias and its direction; and mode 20 — hemisphere occlusion from eight
   FIXED directions, the sun not involved — read **97.3% fully open, mean 0.987**.

   **THE ANSWER: no direction above a receiver is occluded, so no sun vector
   could ever have produced a shadow. The TLAS is effectively a ground plane.**
   The sun, the ray, the bias, the length, the world reconstruction (mode 2's
   checker is perspective-correct and world-locked), the alignment and the
   injection are all exonerated by measurement.

   **Two retractions from those sessions, both mine**: "the ray length is the
   biggest effect yet" was read off a frame-1228 partial file and reads 0.9%
   complete (gotcha 384, quoted at the operator earlier in the same session);
   and session 3's skyline silhouette was read as proof the TLAS contains the
   world — **a bare ground plane produces the identical silhouette** (gotcha 395).

   **PART 67'S FIRST MOVE IS A CENSUS, NOT A BUILD, and both halves are
   offline**: what the ~700 accepted instances ARE (a histogram of their vertical
   extents says in one pass whether the structure is a flat sheet), and which
   filter eats the buildings — `collected=10.9M skips: dyn=19.0M alpha=3.2M
   bounds=5026`, i.e. `dyn` is 57% of every draw the collector sees and `alpha`
   10%. `tools/rt_depth_order_census.py` is the model: the `.xtr` traces say what
   hardware draws for the scene, and our own counters say what we did with each
   class.

   **WHAT PART 65 SHIPPED.** Route (b) is complete end to end: the census (126 pixel shaders, 140
   fetch slots, measured against hardware's own register file in twenty `.xtr`
   world traces — the plan guessed "a dozen"), the shader substitution
   (`tools/patch_rt_shadow_hlsl.py`, a second SPIR-V cache, exactly 126 of 449
   modules differ), the factor pass (`runtime/gpu/rt_factor.hlsl`) and the tier
   ladder (LOW half-res 1 ray / MEDIUM full-res 1 ray / HIGH four rays over the
   sun's disc). **What is owed is one operator session**,
   `tools/part65_operator_session.sh` — arm 1 is the poison positive control and
   GATES the other two, arm 2 is the live panel toggle on one scene. The
   questions are about SHAPE (do lit surfaces stay lit, do shadows sit under
   their casters, is there acne, do shadows detach), because that is the class
   part 64 proved a headless statistic cannot answer.

   **Proven and not to be re-derived**: the shadow ATLAS SNAPSHOT is where the
   title's shadow term reads and needs NO shader patch (`CZ_VK_SHADOW_FILL=0.0`
   → outdoor median luma 80.61 → 61.43, `=1.0` → 81.46); the convention is
   STANDARD (near = occluder); the whole BLAS/TLAS/ray-query path reaches that
   term (`CZ_VK_RT_POISON=1` → 61.18 against the fill's 61.43); and the plumbing
   engages and holds (~1,400 BLASes, ~33 MB, zero flushes/collisions).

   **Closed as unworkable**: route (a) SELF-SHADOWS by construction — writing
   the map means every receiver in it is compared against itself, with no
   receiver-side offset available. Five independent knobs (occluder set, union
   vs replace, the light-matrix binding, a bounds gate, a 6.7x bias sweep) all
   land at 64-66 median luma against OG's 80.61. Stills show every LIT surface
   greyed. Do not re-open this by tuning.

   **Real defects found and FIXED along the way, each on a count that does not
   drift, none of which moved the picture** — keep them, re-buy none:
   junk ±6.3M-unit geometry entering the BLAS (gated); the title's cascade being
   52.8% EMPTY because it draws casters not receivers (`CZ_VK_RT_CASTERS`, now
   the default); and the light matrix bound by RECENCY when the cascade pass
   carries several distinct c0-3 per slice (0 slices with one against 28,704
   with several) — now bound by DATAFLOW with the scene pass as oracle
   (`CZ_VK_RT_ANY_MATRIX=1` is the control).

   **The one untested hypothesis**, kept as the sole reason to revisit route (a):
   a slice's content written to the right atlas QUARTER but paired with another
   cascade's matrix. The distinctness counter cannot catch it — it verifies one
   matrix per slice, not that the matrix belongs to that slice. The test is a
   per-cascade-index content comparison between our traced quarter and the
   raster quarter it replaced.

   **Route (b), BUILT in part 65** (§6cw): compute the factor per RECEIVING
   PIXEL in screen space and patch the atlas-sampling pixel shaders to read it.
   The defect is impossible by construction, and it is the only route that can
   do soft or per-pixel shadows. Everything part 64 built is reused unchanged,
   and three of that part's hardest problems do not exist here — the
   slice<->matrix pairing (only the sun's DIRECTION is used), the depth
   convention (nothing is written to a depth buffer) and the occluder set (the
   reason to prefer the title's own casters was a property of writing the map).
   `CZ_VK_RT_ROUTE=a` restores part 64's atlas trace as the control arm.

0u. **THE DoF COMPOSITE — DOWNGRADED IN PART 42: the "hardware contradiction"
   mostly dissolved, and what remains is two bounded residues.** Part 41's
   framing ("hardware runs the same shader/constants yet its 40-60 m storefront
   is legible — a compensating term is unlocated") was measured in part 42 and
   came apart: (a) OUR constants match hardware's on every recoverable register
   to the printed digit (`CZ_VK_PSBIND_PC` — the instrument already existed;
   pc81 differs only as the focus-dependent placement of the same 80-wide
   band); (b) the worked alpha math gives ~95% blur at 50 m on BOTH platforms
   (`saturate(2×(viewZ−pc81.z)/80)`); (c) the gather's tap radius is ~ZERO on
   both sides (s1 measures neutral 0.5, so blur640 ≈ the half-res downsample);
   (d) **hardware's own R4 PNGs are soft at range** — the "legible storefront"
   was high-contrast signage surviving a 95% half-res lerp, read against OUR
   patternless flat-textured walls, i.e. item 00i wearing this item's clothes.
   `docs/phase5-notes.md` §6bv. What stays open, at reduced priority:
   * the fmt6 byte-split depth serving (the gather's 8 depth-edge taps read
     depth AS 8_8_8_8 bytes and we serve the float image — part 36's
     231-fetch class). Shapes edge weights/halos, not the field-wide blur.
     Still the right fix to build, with its own arm and counter.
   * pc255.x at the gather (the taps' depth-compare threshold) is 0 on our
     side and UNRECOVERABLE from the traces (loads from CPU-written
     `032B6000` — NOT a resolve destination, that theory is refuted; the
     provenance-printing `xtr_draw_constants.py` now names such addresses).
   Do not clamp CoC or hack constants; the composite is behaving as designed
   on both platforms.

0s. **THE STRIPED-MATERIAL CLASS — the top picture item as of part 35, fully
   evidence-bounded, and the next move is named: TRACE THE WRITER.** One streamed
   quality level of an asset renders as black/white banded garbage — the tanker up
   close, Dick at distance, the pawnshop's window boards — stable, stuck to the
   surface (operator strafe test), painted by the scene pass, a different level on
   different boots. `docs/phase5-notes.md` §6bi is the full record; captures and
   live-process texture dumps in `~/DR2CZ-troubleshooting/part35-item1-operator/`.
   **Five theories died by measurement in one session — do not re-buy any**: the
   shadow term (atlas 0.0006% zero at the blotch frame, patches stick under strafe);
   a VFS positional-IO race (fixed on principle in d65874d, overlap counter 0 across
   two sessions, prediction retracted); "the tanker wears a pickup's atlas"
   (misattribution — that draw is a real pickup); the snapshot age fallback (cannot
   fire, no age limit); the texture cache freezing changed content (content guard:
   **4 stale of 92,730,622 hits**, 0.00%).
   **What stands**: guest memory at the sampled address GENUINELY holds the garbage at
   blotch time (live dumps seconds after the operator's F9 — gotcha 285 for why the
   timing matters); the affected textures include runtime-composed impostor/billboard
   sheets (odd extents, DXT5, 4-vertex quads) that exist nowhere on disc and are never
   resolve destinations — so the CPU composes them, from sources not yet named, and
   composes junk. Every reader is exonerated; the defect is in the writer (gotcha 286).
   **Next, in order**: (1) `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` on the outdoor route — the
   engine narrates its streaming and may name the compositor outright; (2) a write
   watch on one sheet's page when its fetch first appears, naming the composing
   function for `gdis`; (3) read that function's SOURCES — if it reads something our
   runtime never writes (a resolve's guest-memory copy is the standing suspect class:
   Xenos resolves write to memory, ours never do), the one-way arrow from part 25's
   "resolve pixels are never written back" note finally has its victim.
   Sub-defects enumerated by the same census, unclaimed: 231 colour fetches served by
   a DEPTH resolve snapshot; 245 all-zero-at-upload frozen entries (mostly benign per
   the guard, but uncounted against the picture); the flat-colour placeholder uploads
   (the far-LOD look of item 00i, possibly the game's own behaviour).

   **PART 36 REFRAMED THIS ITEM — the writer hunt as scoped is DEAD, and two of its
   premises are retracted** (`docs/phase5-notes.md` §6bj; gotchas 287-288):
   * **"Guest memory genuinely holds the garbage" is RETRACTED for the sheets.** Our
     live-dumped 400x240 and 1024x64 DXT5 sheets at blotch time are **byte-identical
     (md5) to the bytes hardware sampled** for the same material in the R3 tanker
     trace. Decoded tiled (`tools/tex_decode.py`), they are coherent billboard
     alpha-cutouts: white colour endpoints, content in ALPHA — exactly what the
     junk-scorer must flag and exactly what the part-35 kickoff's own warning said to
     check. There is no junk in them and therefore no writer to hunt for them.
   * **"Hardware draws no impostor sheets there" is refuted**: the full tanker.xtr
     census reads 3,514 DXT5 + 3,040 DXN fetches (112/53 distinct), including the
     odd-extent sheet class. The earlier "3 DXT5, 0 DXN" was a filtered first pass.
   * The "weird" 110AD000 512x512 DXT1 dump ALSO decodes coherently (tiled+swap16):
     a structured white-slat/boards texture, real content, not noise — but its bytes
     appear NOWHERE in hardware's tanker frame (728 textures, 4K-prefix search), and
     the same address is bound as 4x4 in one frame and 512x512 in another. **The live
     question is now WHICH texture the blotched surface actually samples and whether
     that content is a real asset bound at the wrong quality level** — wrong-binding,
     not composed-junk, fits every observation (stable, UV-stuck, one level per asset,
     varies per boot).
   * Next: (1) name the DRAW painting the blotched tanker pixels in capture_f43675
     (the operator PPM localizes it; the census has every draw's fetches), decode ITS
     s0 with `tex_decode.py`, and compare against hardware's tanker draw (e.g. draw
     4184, verts=18193, s0=11995000 512x512 DXT1 in tanker.xtr); (2) settle whether
     the 16 small colour resolves hardware issues in the frame (64x64 x9, 128x64 x4,
     128x128, 512x256 — NOT in our 61-entry resolve set) appear in our PM4 stream,
     since resolve write-back to guest memory is still a live one-way arrow for the
     OTHER sub-defects; (3) the content-match census (226 of 459 textures
     byte-identical, gotcha 288) is the triage list to shrink, not a suspect list.

   **PART 37: THE MECHANISM IS FOUND, THE FIX IS DEFAULT, AND THE BLOTCH CLASS IS
   CLOSED PENDING THE TOUR** (`docs/phase5-notes.md` §6bo; gotchas 291-292). The
   blotched draw was named by content-ID (hardware's `14790000` truck skin = our
   `109FC000`; the draw is verts=5896 vs_fa161b0fde7aa4d5/ps_c3ae0ec7855c4a18) and
   every input it reads is byte-identical to hardware's. The defect was OURS and
   upstream of the draw: `g_SwappedTexcoords` double-corrected 16-bit texcoord
   attributes whose microcode already carries the compensating .yx destination
   swizzle, so the material's baked-LIGHTMAP UV (16_16, TEXCOORD2) arrived transposed
   and the lightmap's black prop-shadow shapes painted the surface — the "stripes"
   were never in any texture. Mask now defaults to zero (= hardware semantics);
   `CZ_VK_TEXCOORD_SWAP=1` is the control arm that repaints the blotch. Verified by
   same-binary A/B at the reproduced site (headless — the blotch site IS the Case 0-2
   spawn), same F9 index, blotch -> clean. The §6bk "body/cab draw" attribution is
   retracted (street clutter, gotcha 291). **Still open in this item:** the 16 small
   colour resolves / resolve write-back (sub-defect lead unchanged); the 231 depth-fed
   colour fetches; and one look each at Dick-at-distance and the pawnshop boards on
   the fixed renderer to confirm the class closes (the mechanism predicts they do:
   both are lightmapped-material surfaces).

   **PART 38 CLOSED THE RESIDUE'S LARGEST MEMBER TOO: the per-boot "wrong quality
   level" randomness was the TEXTURE CACHE, and the repair is now the default.** The
   once-only upload cache served a streaming-recycled address's FIRST occupant forever
   (the tanker cylinder wore a brick wall; guest memory held a pickup atlas by dump
   time — three occupants of one address in one session). Guard+revalidate default on,
   `CZ_VK_NO_TEX_REVALIDATE=1` the control arm; operator-confirmed across a full
   evening (§6bp, gotcha 293). The class-closure tour also confirmed Dick and the
   pawnshop clean (part-37 mechanism). What remains of 0s: the 16 small colour
   resolves / resolve write-back lead, and the 231 depth-fed colour fetches.

0t. **THE SHARD TREES — foliage renders leaf cards as solid plates; the cutout never
   happens.**

   **PART 46 — THE REMAINING HALF IS ALPHA-TO-MASK, AND IT IS EXACTLY EMULABLE HERE.**
   Part 40 fixed the register index and the solid plates went away; what is left is
   the operator's "hard black shards among correct leaves", and part 46 attributes it.
   `docs/phase5-notes.md` §6ca. Four things changed in this item:
   * **The repro is now the TITLE SCREEN** — the menu backdrop is this same Still Creek
     material, reached in 120 s with `CZ_FAKE_PRESS_SEQ=NONE,NONE,NONE,F9,NONE` and no
     input, on a near-static camera; and **hardware's own picture of it is already in
     the repo**, `Xenia logs/E_screenshots/E3_title_background_stillcreek.png` (content
     box x 0..1399, y 96..880 -> resize to 1280x720 and it registers). The item no
     longer needs an operator OR a new capture. Gotcha 319.
   * **The draw-ID pass that part 40 asked for exists and was used.** The dark shards
     and the correct gold leaves in one canopy come from **the SAME DRAW** (menu frame,
     draw 2329, `vs_716ff2d14e06fa52 / ps_03533a74cbd5228c`, `cc=AA00001C`), which
     retires every per-draw input at once — constants, textures, render state,
     pipeline. Gotcha 318. Note this is a THIRD material: the part-46 kickoff named
     `ps_69a5c3be9359b87c`/`ps_8602b5fd69289893` from a gameplay capture, and neither
     is the menu tree's.
   * **The state read is byte-equal to hardware** across all 19 round-2/3/4 traces —
     the three leaf `RB_COLORCONTROL` values (`AA000007`, `AA00000C`, `AA00001C`),
     their blend controls, and `RB_ALPHA_REF = 0.0` on every leaf draw. And the
     `k_10_11_11` normal decode is right: hardware's own streams decode to unit length
     on 512 of 512 sampled vertices, with 74.8% at N.L > 0.
   * **The mechanism.** The canopy draws are GREATER at **ref = 0** plus ALPHA_TO_MASK
     on a **DXT4/5** albedo, i.e. fractional alpha. At ref 0 the alpha test keeps
     essentially everything, so A2M is doing all of the work — and we do not emulate
     it, on a written excuse ("the draws hardware sets it on also set the alpha test,
     so the clip covers them") that is false exactly at ref 0. Gotcha 317. The soft
     feathered fringe the artist authored at alpha 0.05..0.5 is written at FULL
     opacity into an opaque-blended target; hardware spreads it over the 4x MSAA
     coverage mask and resolves it soft. The luminance distributions agree with E3
     (p05/p95 0.291 vs 0.326), so this is a COVERAGE defect, not a brightness one.
   * **Part 38's plan said "as a threshold/dither discard, since we have no real MSAA".
     Better than that is available:** we rasterise 1-sample into an EDRAM image scaled
     2x in BOTH axes for a 4x surface, so one of our pixels IS one guest sample and the
     2x2 sample grid is `(int(iPos.x)&1, int(iPos.y)&1)`. The coverage can be
     reproduced sample for sample, and `RB_COLORCONTROL`'s top byte `0xAA` is hardware's
     own `alpha_to_mask_offset0..3 = 2,2,2,2` ordering.
   **NEXT, and it is build-and-measure, not investigate:** make the alpha threshold
   per-sample for A2M draws (an ordered 2x2 dither in place of the scalar
   `g_AlphaThreshold`, which is a macro in XenosRecomp's `shader_common.h`, so it can
   ship as a `CZ_DXC_DEFINES` + `CZ_SHADER_SPV` arm before it is ever a default), then
   A/B the menu frame against E3 on a NAMED property — the canopy box's p05/p95 ratio
   and its hard-edge count, not "does it still look wrong".
   **THE MECHANISM IS NOW DEMONSTRATED, not just named** (§6ca addendum). The dither
   was built — `XeAlphaTestThreshold()` in XenosRecomp's `shader_common.h` behind
   `XE_ALPHA_TO_MASK`, the emitter calling it, and the runtime publishing an A2M flag
   at shared+284 — and run at the menu frame against E3:
   * **the hard dark plates break up into feathered foliage with sky through it**, and
     the named property lands on the oracle: canopy p05/p95 **0.291 -> 0.324** against
     hardware's **0.326**;
   * **three null arms are byte-identical over the canopy** (default cache, the new
     emitter with the define OFF, and the define ON with the gate declining every
     draw — md5 `f4a1a593a15b3e27b40d59136aadf622` on all three), so the emitter
     change and the published flag are proven no-ops and the effect is the dither;
   * hard-edge share goes 3.13% -> 4.92% against hardware's 0.21%, which is the
     expected stipple of dithering with no sample grid underneath.
   **AND THE SURFACE IS 2x, NOT 4x — the counter is what found it.** The first gate
   published the flag only for `RB_SURFACE_INFO` msaa==2 and published ZERO, against
   187,621 draws taking the 4x window scale in the same run; renaming the counter to
   carry the sample count reads **msaa=1 (2x) on 69,390 A2M draws, msaa=0 on 518, 4x
   on none**. So the foliage is drawn into a 2x surface our renderer does not
   sample-expand, and `CZ_VK_A2M_ANY_SURFACE=1` (the arm above) is a DIAGNOSTIC that
   dithers at pixel granularity, not a fix.
   **THE FIX, named:** sample-expand 2x surfaces the way `msaa==2` ones already are
   (Xenos 2x is a vertical sample pair, so a 1x2 dither over a Y-expanded image is
   exact and the existing resolve averages it), or rasterise A2M draws with real
   Vulkan MSAA + `alphaToCoverage`. The shader half is built and controlled; only the
   surface expansion is missing.
   **Demoted, not closed:** the denormal/NaN packed-normal suspect (10.5% of
   hardware's leaf normals are denormal as float32, 2.5% NaN, and they are the
   straight-up ones). The `XE_NAN_VS_KILL_IN` arm DOES fire on this tree — 5.41% of
   canopy pixels change — so NaN-class vertices are present and their exponent bits
   reach the shader; it cannot speak for the mantissa or for the denormal half. With
   A2M reproducing and removing the symptom, this is no longer a leading explanation.

   **PART 39: THE SUSPECT IS REFUTED AND THE ITEM NOW NEEDS ONE CAPTURE, NOT AN
   INVESTIGATION.** RB_COLORCONTROL read across all eight R4 traces — **40,703 draws** —
   says hardware enables neither the alpha test (bit 3) nor **ALPHA-TO-MASK (bit 4)**
   anywhere in that area. So the emulation this item asked for would have been built
   against a mode the title does not use, and part 38's alpha-test wiring is correct and
   simply inert here (which is why the trees did not change). Nor is it a shader `kill`:
   exactly **1 of R4's 208 pixel shaders** has one, and the 324-of-324 our own bank
   reports is XenosRecomp's unconditional clip, not a material census (gotcha 296).
   Hardware's blends at these draws are 5,109 opaque / 666 SRC_ALPHA / 31 other; our
   shard-tree draws are opaque too.
   **PART 39 RETRACTION — THE MATERIAL BELOW WAS MISIDENTIFIED AND EVERY CONCLUSION
   DRAWN FROM IT IS VOID.** I picked the "tree" draws out of an operator census by a
   FORMAT SIGNATURE (draws binding a DXT5 + DXN pair, 176 of them, `ps_790283523afcaf20`
   on texture `0A2E4000`) and never decoded the texture. Decoded, `0A2E4000` is **HAIR** —
   brown strands, a character material. So the whole chain built on it is withdrawn: the
   md5 pairing, the "blend split 118/58 matches exactly", the vertex-histogram match, and
   the conclusion that state and inputs agree and the fault must be in the shading. None
   of it was about trees. This is gotcha 291 again, one level up: I paired by a SIGNATURE
   instead of by CONTENT, which is the same error the tanker taught, and the fix is the
   same — DECODE AND LOOK before building an argument on a texture (gotcha 287).

   **ALSO RETRACTED: "hardware and we agree on the render state".** That comparison read
   the registers THE GUEST SET on both platforms. It is the same game, so those agree by
   construction and the check could never have found anything. What matters is what each
   RENDERER does with them, which was not examined. Any "state matches" claim in this
   port made by diffing guest registers across the two platforms needs re-reading in that
   light.

   **WHAT SURVIVES, and it is little:** hardware's own frames show soft, fine, leafy trees
   at the distance where ours shows hard plates, so the defect is ours at a matched
   distance with a picture oracle. Nothing else about the tree material is established.

   **WHAT THE EVIDENCE NOW SUGGESTS, stated as a hypothesis and not a finding:** the
   shard shapes have long straight polygon edges, and the buildings at the same distances
   go flat-panelled at the same time. One mechanism would explain both — **we draw the
   LOW-LOD asset where hardware draws the full one**. The archives carry explicit LOD
   variants for exactly these objects (`z01_ash_tree_LOD.tex`, `z01_maple_tree_LOD.tex`,
   `z01_street_lamp_LOD.tex`), and item 00i's own analysis found LOD here is STREAMING,
   not a distance curve. That is the next thing to test, and it must be tested by
   identifying the tree's draws FIRST.

   **THE DRAW-ID PASS WAS BUILT AND THE TREE IS NOW IDENTIFIED BY MEASUREMENT.** One
   operator F9 at a shard tree, `CZ_VK_DRAW_ID=1`, and the canopy region resolves to
   exactly TWO draws:

   | share of the canopy | draw | shader | textures |
   |---|---|---|---|
   | **57.1%** | 1306 | `ps_1f93b74b9a4fa389` | **NONE — it binds no texture at all** |
   | 40.7% | 1204 | `ps_790283523afcaf20` | `0A2E4000` DXT1 + DXT5 + DXN, the foliage set |

   **The untextured draw covers the MAJORITY of the tree**, and an untextured draw paints
   flat polygons — which is what a shard is. That is the first mechanism-shaped fact this
   item has ever had.

   **It is not a translation defect.** `ps_1f93b74b9a4fa389` is 72 bytes of microcode and
   hardware's own disassembly of the same hash contains **no `tfetch` either** — it writes
   `oC0` from constants (`mul r0.w, c48.x, c255.x` / `mad_sat oC0.xy, ...`). Our
   `tfetchConsts: []` is correct. Hardware sets seven fetch constants at those draws, but
   the shader reads none of them: the `.xtr` tool lists every fetch constant that is SET,
   not the ones a shader uses, and that distinction matters here.

   So the question is now sharp and answerable: **hardware draws this same untextured
   shader 41 times in the same area and its trees still look leafy, so where does that
   draw's output GO on hardware?** The shader writes a 2-channel-plus-zeros pattern that
   looks like an auxiliary target (velocity / a G-buffer channel), not a colour — and if
   hardware routes it to a different render target while we paint it into the scene
   colour, that is the defect, and it would explain the flat plates exactly. The next
   step is to read the render-target state (`RB_MODECONTROL`, `RB_COLOR_INFO`,
   `RB_SURFACE_INFO`) at those draws on BOTH sides.

   **A CORRECTION TO THE RETRACTION ABOVE:** `0A2E4000` — which I decoded as "hair" and
   used to void the whole earlier identification — IS bound on the tree canopy, by the
   draw that owns 40.7% of it. So the signature-based pick of `ps_790283523afcaf20` was
   evidently on the tree after all, and my reading of its texture as a character material
   was an eyeball error on a sheet of brown strands that is as likely twigs or leaf
   clusters. What the retraction got RIGHT stands: identifying it by signature was
   unjustified, and the guest-register comparison proved nothing. What it got wrong was
   concluding the material was unrelated. Both errors have the same cure, which is this
   instrument.

   **PART 40 — THE CORRECTION ABOVE IS ITSELF WITHDRAWN, AND THE ORIGINAL READING WAS
   RIGHT: `0A2E4000` IS HAIR.** Hardware's `ps_34524bb64374d20e`/`ps_790283523afcaf20`
   pair — the same pair, 296 blended draws in R4 trace 01 — binds `0E078000`, and dumped
   out of the trace and decoded it is unmistakably a **hair sheet**: long brown strands
   in tuft-shaped islands on a transparent field. So that shader pair is the crowd's hair
   on both platforms, our material mapping agrees with hardware's for it, and it is not
   foliage. Reading a 256-pixel thumbnail as "as likely twigs" was the error; the fix,
   again, is to decode at full size and look (gotcha 287).

   **THE UNTEXTURED-DRAW FINDING DOES NOT GENERALISE.** `ps_1f93b74b9a4fa389` owns 17.46%
   of the screen in that ONE tree frame and **0.00% in nineteen of the 22 walk frames**
   (mean 0.83%). One frame is one sample (gotcha 133), and it was called "the first
   mechanism-shaped fact this item has ever had" on the strength of it. The render-target
   question above is still worth asking, but it is no longer the tree's explanation.

   **THE FOLIAGE MATERIAL, IDENTIFIED BY THE SHAPE IT COVERS AND NOT BY A SIGNATURE:**
   `ps_8452bb656149204e` + `vs_716ff2d14e06fa52`, alpha-blended `07060706`, and a second,
   `ps_e2c3ca8c13351984`. Masking the first material's footprint in frame 018379's ID map
   draws a tree: a canopy of overlapping quads at top-left plus a distant tree as a grid
   of quads, 4.78% of the frame over 12 draws.

   **AND ITS TEXTURES ARE INNOCENT.** `CZ_VK_TEX_DUMP_PS` (part 40, keyed on the shader
   hash because an address does not survive a reboot — gotcha 306) pulled the material's
   own textures out of a headless run: a **256x256 DXT5 leaf sheet**, a **128x256 DXT1
   bark** strip and a **256x512 DXT5 branch card**. All decode cleanly through our own
   untiler, and the **DXT5 alpha planes are perfect** — the leaf sheet's alpha is a
   per-leaf cutout mask, the branch card's a clean branch silhouette. The bytes the
   sampler sees are correct, so "the leaf texture is broken" is refuted.

   **THREE MORE REFUTED.** The **mip chain**: the operator's `arm1_mips`/`arm2_nomips`/
   `arm3_fullmips` show the same shard tree in the same place, and with no mips there is
   only level 0 to sample. **`exp_adjust`**: parsed by XenosRecomp and read by nothing
   (gotcha 295's pattern), CLAUDE.md's "zero everywhere" measured on the Fable 2 bank and
   never repeated here — `tools/expadjust_census.py` repeats it through
   `synth_shader_container.py`'s own CF walk and gets **345 vertex fetches over 99 vertex
   shaders, zero on every one**. **Part 37's 16-bit texcoord swap**: not applicable, the
   foliage UV is **fmt 37, a plain float2**. (A `CZ_VK_TEXCOORD_SWAP` A/B run before that
   was checked is INADMISSIBLE — neither arm had a tree in frame.)

   **THE DEFECT REPRODUCES HEADLESSLY** at the Case 0-2 DebugJump spawn (trees behind the
   camp fence), and F9 can be pressed headlessly too, so census + pose + picture +
   snapshots + ID map are all available unattended. This item no longer needs an operator.

   **PART 40 — SOLVED, PENDING THE OPERATOR'S TOUR. The whole item was one wrong
   register index.** `xenos.h` read RB_COLORCONTROL at 0x2205 (which is
   RB_BLENDCONTROL1); it is 0x2202. So part 38's alpha test never fired (its counter
   read zero in every log, unread), and part 39's "hardware enables neither the alpha
   test nor alpha-to-mask across 40,703 draws" was a census of the wrong register —
   at 0x2202 hardware enables the test on **4,975 of 40,703 draws (12.2%)**: the
   foliage, the fences, the hair, the horizon sheets, and 1,787 draws of the
   shadow-caster shader `ps_34524bb64374d20e`, whose whole body is "sample the
   material's alpha, clip against RB_ALPHA_REF" — an alpha-tested shadow map. With
   the index fixed, the same-binary A/B at the headless treecam viewpoint
   (`CZ_VK_NO_ALPHA_TEST=1` = the old renderer) shows the shard plates GONE: lit,
   textured, cutout canopies. Both halves of the defect were the same missing test —
   the cards had no cutout in the scene, and the caster stamped solid quads into the
   shadow atlas whose projected shadows were the plates themselves. The decomposition
   that got there (worth keeping for the method): `CZ_VK_NO_DEPTH_FETCH=1` lit the
   plates, proving the darkness was the shadow term; the same arm still showed opaque
   cards, proving the cutout was separately missing; the atlas snapshot showed the
   solid diamond cards the caster stamped. `docs/phase5-notes.md` §6bs, gotcha 308.
   Remaining, counted not guessed: func EQUAL (the two-pass core redraw, ref 1.0) and
   A2M-without-test are un-emulated; GREATER-at-ref-0 differs from our GEQUAL-shaped
   clip only at alpha exactly 0 on blended draws.

   **THE NEXT MEASUREMENT, NAMED:** this material's PIXEL CONSTANTS have never been
   compared against hardware. `oC0.w = pc(1).w * s0.a`, and a `pc(1).w` above 1 saturates
   the alpha and turns every leaf card into the opaque plate we see; `oC0.rgb` branches on
   `pc(20).xyz` before the shared tone epilogue. Neither foliage shader appears in ANY of
   the eight R4 traces, so this needs a round-5 trace standing at a main-road tree —
   `docs/phase5-notes.md` §6br.

   **THE INSTRUMENT THIS NEEDS, and its absence is why two sessions have guessed:** there
   is no way to point at a pixel and be told which DRAW painted it. Every identification
   so far has been inference from shader/texture/extent signatures, and it has now been
   wrong twice. A draw-ID pass — render each draw's index to a side buffer, dump it beside
   the F9 picture — converts "which draw is that tree" from an argument into a lookup.
   Build that before the next attempt.

   Operator captures `part38-operator/arm1_default/capture_f28446` (near
   trees as angular shards, black backfaces) and every outdoor frame since phase 5 in
   hindsight. The leaf material (e.g. ps_c9ca4f73ba93d023, DXT5 albedo + DXN normal)
   needs its alpha channel to discard pixels. Part 38 built the RB_COLORCONTROL ALPHA
   TEST (enable bit 3, GREATER/GEQUAL -> the shaders' long-dormant
   SPEC_CONSTANT_ALPHA_TEST clip, RB_ALPHA_REF -> shared+272) — it engages without
   regression and the trees are UNCHANGED, so foliage does not use the RB alpha test.
   **Suspect: ALPHA-TO-MASK** (RB_COLORCONTROL bit 4, Xenos alpha-to-coverage on the
   4x MSAA surface), unbuilt. **The oracle is already on disk**: R4's traces carry
   hardware's full register state at the foliage draws — read RB_COLORCONTROL there
   first (a small xtr-tool extension), and only then build the emulation (as a
   threshold/dither discard, since we have no real MSAA). Hardware's PNGs show the
   target: soft leafy cutouts. `CZ_VK_NO_ALPHA_TEST=1` is the arm for what part 38
   did build. §6bp.

00. ~~**CUBE MAPS ARE NEVER BOUND.**~~ **BUILT IN PART 25, AND ROUGHLY HALF-DONE BY
   VOLUME.** Cube maps are uploaded as six faces and bound into descriptor set 2;
   `CZ_VK_NO_CUBE=1` is the same-binary control arm. Three things are owed and they are in
   priority order:

   1. **THE OPERATOR'S VERDICT — and part 26 measured outdoors, so the question to ask is
      now much sharper.** Six 420 s DebugJump runs, one block, arms alternated, era medians
      over ~12,170 frames each above 1,800 draws:

      | arm | median mean luma |
      |---|---|
      | default — every cube bound, `06805000` from its resolves (3 runs) | 56.593, 56.907, 56.738 |
      | `CZ_VK_NO_CUBE_SNAPSHOT=1` — that one map white (2 runs) | 56.291, 57.086 |
      | `CZ_VK_NO_CUBE=1` — no cube map at all (1 run) | **59.469** |

      **Removing every cube map is 8x the baseline band with no overlap**, so the outdoor
      instrument is sensitive and cube maps as a class measurably darken this scene (white
      dummy reflections add light). **Removing only the rendered map does not separate** —
      its two runs straddle the band — so its contribution to the frame's median is under
      ~0.5%, which is a BOUND and not a null. That is not a contradiction with its 35.9%
      share of cube fetches: a fetch count is not a screen area (gotcha 257).
      **So ask the operator about SURFACES, not about the frame**: a car bonnet, a shop
      window, the gas-station forecourt, outdoors, three configs on the same binary
      (default / `CZ_VK_NO_CUBE_SNAPSHOT=1` / `CZ_VK_NO_CUBE=1`). Indoors the earlier
      four-config block already said binding cube maps changes nothing measurable in the
      safehouse and prologue, with the magenta positive control at 12.7x its null.
   2. ~~**THE CUBE SNAPSHOT PATH**~~ **BUILT IN PART 26, and the face layout it rests on was
      measured rather than assumed.** `06805000` is now assembled from the six resolve
      snapshots at `06805000 + i * 0x4000` into a six-layer `VK_IMAGE_VIEW_TYPE_CUBE` image
      in set 2, and refreshed by each face's own resolve in that resolve's own command
      buffer. The refresh is load-bearing: the title re-renders that map continuously
      (8,850 face refreshes in a 240 s run), so a one-shot fill would have frozen the
      environment at its first fetch. The census printed with it names each face address and
      whether a snapshot was found there — it could have refuted the stride model face by
      face and did not, all six filled. **358,767 of 999,508 cube fetches (35.9%) on a
      240 s DebugJump run** are served from it where all of them read the white dummy
      before; part 25's "55% of the opening hour" is a different population and the two
      numbers are not the same claim. `CZ_VK_NO_CUBE_SNAPSHOT=1` is the same-binary arm.
   3. ~~**A HARNESS THAT CAN REACH AN ADMISSIBLE OUTDOOR FRAME.**~~ **THE ROUTE IS BUILT AND
      THE FILTER IS UNSATISFIABLE ON IT — measured in part 26, and the conclusion is that
      the FILTER has to change, not the recipe.** The route works: it lands in a crowd at
      7,300 draws and **93% of a 420 s run's frames are above 1,800 draws**, with two runs'
      draw counts agreeing to a median relative difference of **1.4%**. But run
      `tools/frame_determinism.py` on two runs of ONE configuration and the answer is
      **422 of 13,056 frames matched, none above 141 draws, and 0 of 12,174 outdoor frames
      matched** — by present index or by content. A crowd of animated actors does not
      render the same draw list twice, so exact fingerprint equality selects for stasis
      (gotcha 254).
      **The replacement is an ERA AGGREGATE with the null measured from that same pair**:
      over the 12,000+ frames above 1,800 draws, the median mean-luma reproduces to 0.94%
      and the median distinct-colour count to 0.76%; coverage saturates at 99.67% and is
      useless. `docs/measurement.md` has the protocol. That is what unblocks items 00, 3, 4
      and 6 — the thing the route was built for — and it is what part 26's cube A/B is read
      with.

   See "what part 25 did" at the end of this item.

   The original statement, kept because the census in it is the reusable part:

   **91 OF 395 SHADERS SAMPLE ONE AND EVERY ONE OF THEM GETS
   THE 1x1 WHITE DUMMY, ON EVERY DRAW, SINCE PHASE 5.** Found in part 23 by census after
   an operator reported wrong textures throughout the game. **This is the top picture item
   and it is fully specified.**

   The chain, all of it established by reading the code and censusing the shader bank —
   no run was needed and none of it is inference:

   * The shared constants the translated shaders read carry **four** descriptor-index
     arrays, one per HLSL register space: `kSharedTex2D` (offset 0), `kSharedTex3D` (64),
     `kSharedTexCube` (128), `kSharedTex1D` (288), matching descriptor sets 0/1/2/4.
   * `bindTextures` (`vk_renderer.cpp:4262`) writes **only `kSharedTex2D`**, for every
     fetch the shader declares, whatever its dimension. Grep the other three constants:
     they appear at their own definitions and nowhere else.
   * The block is `memset` to zero every draw, so a cube fetch reads index 0 — the dummy.
     Not stale, not undefined: reliably white.
   * It cannot do better, because **`t.dimension` is hardcoded to 1 (2D)** at
     `vk_renderer.cpp:2160` with the comment "dimension is taken from the shader" — and
     `ShaderMeta::tfetchConsts` is a flat `std::vector<uint32_t>` of slot numbers with no
     dimension in it. The dimension is taken from nowhere.

   **The census over all 395 sidecars and SPIR-V modules in the cache** (parse the
   `OpDecorate ... DescriptorSet n` words; the script is in part 23's write-up):

   | descriptor set | modules referencing it |
   |---|---|
   | 0 `Texture2D[]` | 303 |
   | 1 `Texture3D[]` | **0** |
   | 2 `TextureCube[]` | **91** |
   | 3 `Sampler[]` | 303 |
   | 4 `Texture1D[]` | **0** |

   So 23% of the shader bank samples a cube map and always gets white. Cube maps here are
   the environment/reflection maps, so every reflective surface multiplies its specular
   term by pure white — which is the operator's "unicorn colour" file cabinet, the wrong
   dumpster colour, and plausibly a share of "colour is flat" (item 6).
   **Unlike every cache theory this defect is permanent and universal rather than
   accumulating**, which is what matched the operator's report that these look wrong from
   the moment they are first seen.

   **The fix, in three parts, and part 1 is the real work:**
   1. Carry the per-slot texture DIMENSION into the sidecar metadata. The shader knows it
      — it indexes the cube array — so the source is either `tools/synth_shader_container.py`
      reading it out of the translator, or a SPIR-V parse at load. Note a single shader
      may sample both a 2D and a cube, so this must be per fetch slot, not per module.
   2. Create cube textures as six array layers with a `VK_IMAGE_VIEW_TYPE_CUBE` view and
      register them in **set 2**, not set 0. `CreateImage` already takes a view type and
      layer count; `R->dummyCube` already exists.
   3. Write `kSharedTexCube[constIdx]`.

   **WHAT PART 25 DID, and what it measured on the way.**

   All three parts are built. `tools/synth_shader_container.py` records the fetch
   instruction's dimension (word 2, bits 14..15) as `tfetchDims`, positionally against
   `tfetchConsts`; `bindTextures` switches on it and publishes into one of the four
   arrays; `UploadTexture` takes the shader's dimension, reads six faces at a stride of
   one face's tiled footprint, and registers a `VK_IMAGE_VIEW_TYPE_CUBE` view in set 2
   out of its own slot space (`R->nextCubeSlot`).

   * **`tools/shader_dim_census.py` is the gate, and it is two-sided.** The dimension is
     derivable twice independently — our ucode bit parse, and DXC's
     `OpDecorate ... DescriptorSet` words in the translated SPIR-V — and over the rebuilt
     cache the two agree on every shader: **298 modules / 973 slots 2D, 92 modules /
     92 slots cube, zero 1D and zero 3D.** Moving the parse one bit makes it flag all 15
     shaders of a test subset and exit 1, so it has been shown capable of failing.
   * **The fetch constant's own dimension field was LOCATED BY MEASUREMENT**
     (`CZ_VK_DIM_CENSUS=1`), not by recollection — which had it at bits 7..8 and was
     wrong. Partitioning 842,556 2D and 47,574 cube fetches by the shader's answer and
     accumulating each class's always-set / always-clear bits, exactly two dwords
     separate them: **dword5 bits 9..10 read 1 for every 2D fetch and 3 for every cube
     one**, and **dword2's top six bits read 5 for every cube and 0 for every 2D**, which
     is the stack depth stored minus one, i.e. six faces. The second was a prediction
     stated before the run from Xenia's published layout, so the run could have refuted it.
   * **The two sources are cross-checked on every fetch from now on**, and they do
     disagree — a fetch constant saying 2D under a shader saying cube. Those are served
     the dummy and counted, because reading six faces out of a surface the guest describes
     as one would build a cube from five slabs of whatever follows it.
     **The share is NOT yet known and the first number written here was misleading.** The
     boot-to-gameplay recipe gave 114 against 337,602 agreeing (0.03%); the SAME BINARY on
     the deeper outdoor recipe declined **90,984**, with no total to divide by because
     nothing was counting cube fetches. A denominator counter was added afterwards
     (`texture: CUBE fetch`) — this is gotcha 242 again, a statistic fitted to the one
     population the instrument happened to reach, and it is recorded rather than quietly
     corrected because it is the third instance.
   * **TWO of the seven cube maps upload entirely BLACK, and for two different reasons.**
     `06805000` (64x64) is a **resolve destination** — the title renders that environment
     map itself, so guest memory there is zero and always was (`uploaded BLACK, guest
     memory STILL zero`). It is now DECLINED to the dummy rather than uploaded, because
     presenting zeros as a black reflection is a lie dressed as data, and the dummy is
     also exactly the picture that surface had before part 25; `CZ_VK_CUBE_FROM_GUEST=1`
     keeps the zeros as the arm. **The real fix is a cube snapshot path — six resolves
     into six layers — and it is this item's remaining half.** `01330000` (4x4) is the
     other and is a different defect entirely: `uploaded BLACK, guest memory is NON-ZERO
     NOW`, i.e. the texture arrived after our one and only upload and the fetch-constant
     cache froze it black.
   * **A latent barrier defect was found and fixed on the way**: `Barrier` had
     `layerCount = 1` hardcoded, correct for every image this renderer had until cube maps
     arrived. Layers 1..5 would never have left `TRANSFER_DST`, and the likely
     presentation — one correct face and five wrong ones — reads as a decode bug rather
     than a barrier one. **It was already live in `R->dummyCube`**, a six-layer image
     since phase 5, so five of its faces were written and sampled in
     `VK_IMAGE_LAYOUT_UNDEFINED` the whole time: the claim above that a cube fetch got a
     "defined white texel" was true of the +X face only.
     **Note the validation layer is NOT INSTALLED on this machine**
     (`sudo dnf install vulkan-validation-layers`), so "zero validation errors" in any log
     here means nothing at all (gotcha 25).
   * **One open thread: `06805000` (64x64, `k_8_8_8_8`) is a cube map at an address this
     renderer has a RESOLVE SNAPSHOT for**, and it is the only one of them that is. A
     resolve's pixels are never written back to guest memory, so if the title is rendering
     that cube dynamically we are feeding it zeros. Serving a snapshot to a cube fetch is
     refused (a snapshot is a 2D image in set 0, so its slot number means nothing in set 2)
     and counted. **The measurement that settles it is one row of `CZ_VK_TEX_CENSUS`:
     whether `06805000`'s uploads come out all-zero.** If they do, the fix is a cube
     snapshot path — six resolves into six layers — not anything in the decode above.

   **Two of this investigation's own hypotheses died in the same census and are recorded
   so nobody re-buys them:** the colour-grading LUT is NOT a `Texture3D` (zero modules use
   set 1), so that is not the route to item 6; and the `if (constIdx >= 16) continue;`
   silent skip at `vk_renderer.cpp:4265` **never fires** — 0 of 1,076 declared fetch slots
   in the whole bank are >= 16. That skip is still a silent drop with no counter and
   should get one, but it is not a defect anyone is seeing.

00d. **THREE OF THE FIVE VULKAN VALIDATION DEFECTS ARE CLOSED (part 26); TWO REMAIN.**
   The tally on the outdoor route is now **20 `VkGraphicsPipelineCreateInfo-Input-08733`
   and 6 `VkGraphicsPipelineCreateInfo-topology-08773`, and nothing else** — quote that as
   the standing gate.

   * `VkImageMemoryBarrier-image-03320` (20) and `VkImageViewCreateInfo-subResourceRange-01021`
     (4) were both found by READING once the layer named them, and both went to 0:
     `Barrier` now names DEPTH and STENCIL together on a depth/stencil format, and
     `CreateImage` takes the image type from the VIEW type instead of from the depth extent.
   * `vkCmdDraw-None-09600` (14) needed a run, and it needed OBJECT NAMES to be readable at
     all — `VkImage 0x2350000000235` names none of the five places this renderer creates
     images. `VK_EXT_debug_utils` now comes in with the layer and the next run printed
     `[resolve snapshot 14A7A000 96x45 slot 32]`, with the other thirteen a halving chain
     (64x22, 32x11, 32x5, 32x2, 32x1) — one bloom pyramid, every snapshot created
     mid-frame. **The defect was the PUBLISH ORDER**: a snapshot's descriptor was written
     before the fill-and-transition recorded into the frame's command buffer, so between
     the two there was a descriptor claiming `SHADER_READ_ONLY` on an image still in
     `UNDEFINED` — undefined content for anything indexing it, and with a bindless heap
     "nothing indexes it" is an argument rather than a guarantee (gotchas 255, 256).
     Transitioning in an immediate submit before publishing takes it to 0.
   * **Still open, and both are pipeline-creation rather than per-draw:** `Input-08733`
     (a vertex attribute at Location 15 declared `R32_UINT` where the shader input is a
     float `vec4`) is very likely the deliberate `USCALED`/`SSCALED` decision seen from the
     layer's side — read it against gotcha 122 and MEASURE before changing it.
     `topology-08773` is a `POINT_LIST` pipeline whose vertex shader never writes
     `PointSize`, i.e. point size is undefined for those draws; 6 pipelines.

   The original table and its reading, kept because the argument for the layer is in it:

00d-history. **FIVE VULKAN VALIDATION DEFECTS, found the hour the layer was installed.** For the
   whole of phases 5 and C this project ran without `VK_LAYER_KHRONOS_validation`, so every
   `VUID` grep in every log returned zero for the reason gotcha 25 exists. The operator
   installed it at the end of part 25 and ONE 12,802-frame session reported all of these:

   | VUID | n | what it is |
   |---|---|---|
   | `VkImageMemoryBarrier-image-03320` | 20 | a `D24_UNORM_S8_UINT` barrier whose `aspectMask` is `DEPTH_BIT` only; without `separateDepthStencilLayouts` it must name depth AND stencil |
   | `VkGraphicsPipelineCreateInfo-Input-08733` | 20 | vertex attribute at Location 15 declared `R32_UINT` where the shader input is `vec4` of `float32` |
   | `vkCmdDraw-None-09600` | 14 | a sampled image is still `VK_IMAGE_LAYOUT_UNDEFINED` when a draw reads it |
   | `VkGraphicsPipelineCreateInfo-topology-08773` | 6 | a `POINT_LIST` pipeline whose vertex shader never writes `PointSize` |
   | `VkImageViewCreateInfo-subResourceRange-01021` | 4 | a `VIEW_TYPE_3D` view created on a `IMAGE_TYPE_2D` image |

   **The last one is ours and is trivially confirmable by reading**: `makeDummy(R->dummy3D,
   VK_IMAGE_VIEW_TYPE_3D, 1, 1, ...)` passes `depthExtent = 1`, and `CreateImage` only
   builds a 3D image when `depthExtent > 1` — so it makes a 2D image and asks for a 3D view.
   It has been failing since phase 5. That is the THIRD latent defect in this one
   dummy-creation path found in part 25 (the others: `Barrier`'s hardcoded `layerCount = 1`,
   and a four-byte upload feeding a six-layer copy), and the first two were found by reading
   rather than by symptom — which is the argument for the layer, not against it.

   **`vkCmdDraw-None-09600` is the one to chase first.** It is the live form of the class
   the hardcoded `layerCount` produced, so something still reaches a draw untransitioned,
   and an undefined layout is undefined CONTENT — a wrong picture with no counter anywhere.
   14 occurrences in one session.

   `Input-08733` is worth reading against gotcha 122 and the `USCALED`/`SSCALED` note in
   `CLAUDE.md` before assuming it is a defect: an integer delivered into a float input is
   deliberate here, and this may be the same decision seen from the validation layer's side.
   Measure before changing it.

   **Standing gate from now on: `CZ_VK_VALIDATION=1` on at least one run per session, and
   quote the tally.** It costs nothing and this table is what a silent renderer looks like
   after eight parts of not being able to ask.

00j. ~~**CINEMATICS PING-PONG ONCE A CHARACTER SPEAKS.**~~ **FIXED IN PART 29, AND
   OPERATOR-CONFIRMED: the prologue cinematic plays to completion with sound.** Two
   commits, one of which mattered. Kept in full because the trail retired five candidate
   explanations and the last one was found by an operator sentence, not by the port.

   **The fix.** A 2 KB XMA2 packet header is
   `frame_count:6 | frame_offset_in_bits:15 | packet_metadata:3 | packet_skip:8`, and
   `packet_skip` is how many packets to step over to reach the next packet **of the same
   stream**. Our decode walk advanced by one packet. That is exactly right for mono and
   stereo — this title's music, SFX and one-shot voice lines are all of those, `skip` is
   0 throughout them, and the code path is byte-identical before and after. It is wrong
   for 5.1: the 360 decodes six channels as several interleaved 2-channel streams in one
   packet stream, one XMA context per pair, which is why contexts 5, 6 and 7 all point at
   input buffer `02584000` — an arrangement this project had noticed twice, called odd,
   and left unexplained. Walking `+1` made each context decode every other stream's
   packets as its own.

   **The magnitude, measured off the asset.** `39694.xma`'s 11,903 packet headers sum to
   89,007 frames of 512 samples = **316.5 s as three streams**, which the operator's
   stopwatch (~5 min 10 s) confirms. One 128 KB buffer is 64 packets carrying 519 frames
   = **1.845 s of programme**, and the skip chain from packet 0 visits 0, 4, 8, 12 ...
   reaching **20 of those 64**. We produced **4.916 s** per context from it — 2.66x too
   much audio. Each context therefore filled its 25-block output ring about three times
   faster than the title's mixer drained it; after one buffer the whole 5.1 group was
   ring-full and wedged, `SamplesPlayed` stopped, and the cinematic's PID clock was left
   tracking a frozen input.

   **The result, on the free gate:**

   | | cinematic era | audio clock reached |
   |---|---|---|
   | before | runs/distinct **120**, 15 camera poses, LOOPING | 4.906667 s, frozen |
   | after | runs/distinct **1.00** every quarter, 9,688 poses, advancing | **310.7 s** of 316.5 |

   `audio/cinematics.big` is read **201** times where it was read twice, and the run
   leaves the prologue into gameplay (draws 1,354-2,123 across the session, against 1,238
   forever). Ten contexts decode concurrently late in the run, `refused=0`.

   **AND THE OPERATOR SESSION IS THE STRONGEST EVIDENCE IN THE ITEM**, because it is a
   human watching and a machine recording the same thing. Three cinematics played to
   completion by eye and ear — the prologue, one more, and the walk out of the safehouse —
   and `CZ_CINE_TIME` recorded exactly three playback segments, each running from zero to
   its end:

       segment 1   audio clock  0.00 -> 310.68 s     (of a 316.5 s track)
       segment 2   audio clock  0.00 ->  60.26 s
       segment 3   audio clock  0.00 ->  17.53 s

   over 21,172 frames scoring `runs/distinct` **1.00-1.01 in every quarter, advancing** —
   no looping anywhere in the session. The clock restarting at 0.00 three times is the
   shape a working cinematic system has and the frozen build could never produce.

   **A knock-on worth expecting elsewhere: the cache grew 411 -> 417.** A cinematic that
   plays binds six pixel shaders no run had ever reached, and the session logged
   `no translated shader` six times before they were translated in. Nothing new appeared
   for the second or third cinematic (`no translated shader` stayed at 6 unique for the
   whole session, and the dump-vs-cache name diff is clean), but **every era this port
   opens for the first time is a shader gap nobody has counted** (gotcha 13).

   **The second commit, which was necessary and changed nothing visible.** The walk
   retired a spent input buffer by unconditionally switching to the other one. This title
   never uses buffer 1 — censused over a whole run, 136 context dumps, `in1Ptr` 0 in every
   one and `in1Valid` never set; it re-arms buffer 0 in place and swaps only the pointer.
   So the switch moved a context to a buffer that does not exist and nothing could move it
   back, because the walk reads `currentBuffer` to decide where to look. `ctx7` was caught
   in exactly that state, `valid=00 cur=1`, unchanged across 28 dumps. Fixed by switching
   only when the other buffer is valid, which is what the hardware does and a no-op for a
   real double-buffered stream. On its own it did not move the gate.

   ---

   **The trail, kept for its refutations.**

   **READ THIS BLOCK FIRST; everything below it is the trail that got here, kept
   because three of its refutations are still worth not re-buying.**

   The chain, every link measured rather than argued:

   1. `sub_82475718` IS the cinematic's clock. `sub_82478FC8` calls it twice per
      cinematic update and stores its return **straight into the scene's time** at
      `[cine+0x1698]`. That is the "what writes the cinematic's time each frame" the
      part-28 hand-off asked for, and it was found by asking `gdis --find-uses` who
      touched the manager's singleton — a query that had been returning **0 sites**
      because the scanner could not match `lis`+`lwz`. It is 301. See the tool commit.
   2. It is a three-way switch on a mode word, read from the read-only global
      `0x829DC320`, **shipped as 2**:
      `0` raw scene time · `1` the audio stream position · `2` `PID(audio position)`.
   3. `sub_824741D8` is that PID, and the image names it itself — its tail plots four
      values through the debug-graph API under `Cine.Audio P-gain / I-gain / D-gain /
      MV (ms)`. Decoded: `MV = P*err + I*integral + D*(err-prevErr)`, accumulated into
      `[pid+0x28]`, and it **returns `setpoint - accumulator`**. Nothing in it is
      monotonic. Gains as shipped: P 0.025, I 0.005, D 0.0001, deadband 5 ms.
   4. `CZ_CINE_TIME=<file>` logs that value at its source. On the prologue, 2,212
      lines, **mode 2 on every one and the PID ran on 2,208**:

          setpoint climbs linearly forever      0.06 -> 122.7 s
          audioPos FREEZES at 4.906667 s        after 4 s, and never moves again
          ret = setpoint - acc                  hunts 4.91 <-> 5.27, period ~11 s

   5. **The camera is a function of that clock, and the join proves it rather than
      asserting it.** Interpolating `ret` onto every frame of the same run, the median
      spread of `ret` within one `cameraFingerprint` is **0.0052 s**; at deliberately
      wrong alignments the same statistic reads 0.042-0.377 s. So the camera
      palindrome IS the clock's palindrome.
   6. **The three-way arm settles causality**, and every setting is a path the title
      itself implements (`CZ_CINE_AUDIO_MODE=0|1|2`, same binary, same recipe):

      | arm | cinematic era | reading |
      |---|---|---|
      | 2 — shipped, PID | **LOOPING**, 15 poses, runs/distinct **120** | the defect |
      | 1 — scene time := audio position | **FROZEN** at 4.906667 for 338 s | the input really is stuck |
      | 0 — no audio sync | no loop; the scene ends and the run reaches gameplay | the correction is what loops |

      Mode 1 is the sharp one and it was **predicted before it was run**: hand the
      scene the frozen position directly and it should freeze, not oscillate. It did.
      Mode 0 is confounded and must not be read as a fix — with no audio sync the
      first call site hands over an uninitialised scene time of ~138,181 s, so the
      cinematic ends immediately. Mode 2's `if (input == 0) return 0` guard is what
      normally protects against that.

   **SO THE DEFECT IS NOW ONE LEVEL UP AND IT IS OURS: why does the audio stream
   position stop at 4.906667 s?** The chain to it is fully read:

       sub_82759170   cinematic asks the audio system, message ReqID 0x106, field "Time"
       sub_827213C8   audio system looks the voice up in [sys+0xA8], returns entry+8
       sub_82721530   refreshes every entry each tick from sub_8270F768(voice)
       sub_8270F768   voice state 2/3 -> sub_82764C48; otherwise a wall clock
       sub_82764C48   **SamplesPlayed / sampleRate** — two virtual calls whose result
                      struct is XAUDIO2_VOICE_STATE-shaped (SamplesPlayed at +8)

   4.906667 s x 48000 = **235,520 samples exactly = 1,840 XMA subframes of 128**. The
   voice plays exactly that many and stops, while still reporting itself playing — so
   the wall-clock fallback never takes over either.

   **THE GUEST STOPS STREAMING THE CINEMATIC AUDIO AFTER ONE BUFFER — 1.1% of it.**
   `CZ_FILE_TRACE=1` names the asset: `game:\data\audio\cinematics.big` entry
   **`39694.xma`, 24,377,344 bytes**, read from its exact start. Summing the
   `frame_count` field of all 11,903 XMA2 packet headers gives 89,007 frames of 512
   samples = **316.5 s (5 min 16 s) as three interleaved 2-channel streams**, i.e. 5.1
   audio — which the operator's ~5:10 confirms and which independently explains why
   contexts 5, 6 and 7 all sit on the same input buffer `02584000`. The file trace shows
   the asymmetry plainly:

       music.big        47 reads, 128 KB each, alternating two buffers, FOREVER
       cinematics.big    ONE 128 KB read into A2584000, one into A25AA000, then nothing

   262,144 bytes of 24,377,344 ever reach memory. We decode the first buffer's **4.916 s
   of a 316 s clip**, `SamplesPlayed` pins 448 sample-frames behind that, and the whole
   chain above follows. Music double-buffers correctly on the same machinery for the
   whole run, so this is a specific defect, not "streaming is broken".

   **RETRACTED, from earlier in this same part: "the clip ENDED, it was not starved."** With
   `CZ_AUDIO_TRACE=1 CZ_XMA_DECODE_LOG=1` beside the clock probe, the three dialogue
   contexts (5/6/7, `stereo=1`, 48 kHz) decode across two 5-second windows and never
   appear again; their totals as stereo-interleaved seconds are **5.03, 5.04, 4.92**
   against a frozen `Time` of **4.906667**. Our decoder stays healthy for the rest of
   the run (`refused0`, ctx0 still producing ~508k samples per window) while the
   guest's mixer output plateaus at driver frame ~12,288. So the stream ran to its end,
   we delivered all of it, and **nothing told the title it was over** — the voice sits
   in the state-2/3 "playing" branch forever and the wall-clock fallback that would let
   the cinematic carry on never fires. It could only appear now: before phase A/V
   nothing decoded, so no voice ever reached the end of a stream.

   That conclusion was drawn from our decoded length agreeing with the guest's reported
   position — and **agreement between two components we built measures our own
   consistency, not the ground truth.** The asset was one `CZ_FILE_TRACE=1` away and had
   not been asked. The discriminator had been correctly identified and written down; the
   error was recording the likelier branch as a finding instead of leaving it open.

   **The next move, and it is the first thing part 30 should do: find why the guest
   stops issuing reads for this stream while it keeps issuing them for music.** Both are
   128 KB double-buffered XMA streams on the same machinery, one runs for the whole
   session and one stops after the first fill, so the difference between them is the
   defect. Start by diffing what the title does for the two: which context fields it
   polls, and whether our decode walk leaves the cinematic contexts in a state the
   music context never reaches. Note the three-contexts-one-buffer arrangement — a
   retire/refill rule written for one context per buffer is wrong for 5.1 by
   construction, and `kernel/audio.cpp` is where to check that first. The title's own mixer maintains it, and part 28 already
   measured that mixer going silent in the same era. Note the ordering nuance this
   part exposes: "audio" is TWO facts here — the position the guest reports (stops
   first, upstream of the stall) and audible output (stops later, downstream, as the
   already-queued ~5.5 s plays out). The old note "do not chase the silence, it is
   downstream" is right about the second and wrong if applied to the first.

   **A MEASUREMENT CORRECTION THIS PART OWES: the recorded `runs/distinct = 6.13` for
   this defect is diluted 6x and every previous reading has it.** A prologue run spends
   ~1,870 frames in menus before the cinematic, contributing 1,010 of the 1,170
   distinct poses and almost none of the runs. Split by era on the same file:
   whole run **6.14**, menus **1.01**, cinematic era **38.27**, and in steady state,
   by quarters, **15 distinct poses at ratio 120**. `tools/frame_loopiness.py` now
   prints quarters unconditionally, because a partial fix would move 6.14 toward 1 and
   read as "nearly fixed". **Quote the quarters, not the whole-file number** — and note
   the gate cannot tell a stalled scene from a PARKED PLAYER, which is what quarters
   3-4 of the mode-0 arm are (`CZ_FAKE_PRESS_SEQ` ends in `NONE`, so Chuck stands
   still and one camera pose is correct behaviour). Read draws beside it.

   ---

   **The trail below is superseded on the mechanism but keeps three refutations that
   are still live: the output-ring ambiguity, the end sync point, and the audio
   stopping. Do not re-buy any of them.**

   **The symptom, from the operator:** the prologue cinematic plays, and the moment
   Rebecca Chang starts speaking the scene advances ~1 s, runs BACKWARD ~1 s, forward
   again, forever. The subtitle re-appears on each forward pass. Skipping to the next
   cinematic reproduces it after about the same delay.

   **Reproduced headlessly, in data already captured** (`cine2.txt`, the phase A/V arm
   run) — this needs no operator: 12,429 frames, **7,175 camera runs over only 1,170
   distinct camera fingerprints**, with many values recurring *exactly 463 times*. A
   scene that plays normally has runs ~= distinct. Dumping the sequence shows a clean
   **palindrome**, ~26 runs (~0.87 s) per cycle, 463 cycles:

       243a fb9e 0e25 fd9d 3709 24b4 a812 ccd5 79fc a0a0 79fc ccd5 75c9 24b4 3709 fd9d 0e25 fb9e 243a ...

   So the gate for a fix is free and automatic: **`camera runs / distinct cameras`
   should fall to ~1**. It reads 6.1 today.

   **The mechanism, from the image.** The cinematic manager
   (`c:\bcg\deadrisingprologue\source\common\cinematic\cinematicmanager.cpp`,
   named at `0x82063030`) registers six tunables:

       82062FAB  Cine.Audio MV (ms)            <- manipulated variable
       82062FC0/FD8/FF0  Cine.Audio D-gain / I-gain / P-gain (ms)
       82063078  Cine.Audio Cor Latency (ms)   <- read at 0x82475880
       82063094  Cine.Audio Abs Latency (ms)

   **The cinematic slews its own playback RATE to track the audio stream's latency** —
   which is what the script's `cCineAudioEvent ... AudioEventName "sync:39791"` and its
   `sync:`-named camera track mean. A smooth symmetric oscillation that begins exactly
   when a synced stream starts is what that loop does on a bad latency measurement, and
   a PID output can go NEGATIVE, which is the scene running backwards.

   **What the live process says** (sampled at 50 Hz with `process_vm_readv`, no ptrace
   stop, while the operator was stuck in the loop):
   * four MONO 48 kHz dialogue voices appear when speech starts (ctx4-7), alongside the
     stereo music voice (ctx0);
   * their `output_buffer_valid` (dw[1] bit 31) **toggles ~94 times in 8 s (~12 Hz)**;
   * ctx0's never toggles at all;
   * every voice's `input_buffer_read_offset` advances monotonically — the streams are
     going forward, so it is the SYNC that reverses, not the audio.

   **THE RING HYPOTHESIS WAS BUILT, TESTED AND REFUTED. Do not re-buy it.** The repair
   was made properly (reserve one slot so `write` can never land on `read`, making
   "equal" unambiguously empty) and measured on the prologue with the same recipe:

   | | runs/distinct | distinct | ring-full rate |
   |---|---|---|---|
   | before | 6.13 | **1170** | ~12/s |
   | after  | 6.14 | **1170** | ~99/s |

   An IDENTICAL `distinct` is the same scene revisiting the same pose set the same way
   — the loop did not move at all. The change was reverted; the ambiguity below is real
   and is now documented in `kernel/audio.cpp` with this measurement beside it, to be
   fixed one day on its own evidence and with its own arm (the damage it can do is
   overwriting ~32 ms of unplayed audio, which is a click, not a loop).

   **Two method notes from getting it wrong.** The repair needed a post-loop "is the
   ring full" test, and the one it used — `freeBlocks <= frameBlocks` — is exactly the
   loop's own EXIT condition, so it was vacuous and fired on every fill; that is the
   same shape as a bounds check that cannot fail, met twice in one part. And the gate
   needed TWO numbers: `runs/distinct` alone scores a FROZEN camera at 1.01, which
   would have called the pre-audio prologue healthy. Read it with `runs/frames`
   (gameplay 1.09 / 0.65; ping-pong 6.13 / 0.58; frozen 1.01 / 0.08).
   **`tools/frame_loopiness.py`** — it prints both numbers and a verdict, and it
   classifies all three states correctly on the four runs that exist (LOOPING, LOOPING,
   FROZEN, advancing), which is the positive control for the gate itself.

   **THE ENGINE NAMES THE CONDITION ITSELF, and this supersedes the PID reading as the
   leading explanation.** `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` on the prologue prints:

       [guest] WAITING: end sync point not received yet!
       [guest] ((((((((((((((((((((((((((((((( ForceClearAnimSyncPartner()

   The site is `0x824A10D4`, and reading its guard is what makes this load-bearing
   rather than suggestive — the print sits on the FAILING side of a branch whose
   success side, at `0x824A10E0`, is a virtual call taking a float in `f1`
   (`lwz r11,0(r31)` / `lwz r11,8(r11)` / `bctrl` with `fmr f1,f31`), i.e. the
   cinematic's `Update(delta)`. **So while the end sync point is missing, the
   cinematic's update is SKIPPED.** The predicate is `sub_8249EEA8`; the surrounding
   system is an animation sync-point cache with types, callbacks and user pointers
   (`mSyncPointCache[type].mCallback == callback && ...`, asserted at `0x82084370`,
   used at `0x82581B00`).

   **And the operator confirms the audio half from the other side: "cinematic audio
   works at the start and stops right when it starts ping-ponging."** The log agrees to
   the second. Audio output does not degrade — it STOPS, completely and permanently:

       frame  9216  nonsilent=6758  +488
       frame 12288  nonsilent=7678  +472
       frame 16384  nonsilent=7678  +0      <- and +0 for the remaining 62,000 frames

   Frame 12,300 x 5.333 ms = 65.6 s; the cinematic era starts at video frame ~1,925
   ~= 62 s. **The guest's mixer stops mixing** — this is not our output path, and our
   decoder keeps filling rings normally throughout (`refused0`, ~494 frames/5 s).

   **RESOLVED BY COUNTER, AND IT IS A NEGATIVE: THE END SYNC POINT IS NOT HOLDING THE
   LOOP.** `CZ_CINE_PROBE=1` counts the predicate `sub_8249EEA8` (whose zero return IS
   the branch condition — `bl / clrlwi. r11,r3,0x18 / beq`) and entries to
   `sub_824A0FC0`, the function containing both that call and the print. Over a
   windowed run sitting in the ping-pong:

       [cine] sub_824A0FC0 ticks=1  | end-sync-point asked=4  NOT-received=2 received=2
       [cine] sub_824A0FC0 ticks=10 | end-sync-point asked=15 NOT-received=8 received=7

   ...and then nothing, for the rest of the run, while frames kept advancing at ~31 fps.
   **Ten entries in total and an even received/not-received split.** The branch is not
   hit continuously, the predicate is not stuck, and the containing function stops being
   called entirely while the scene keeps ping-ponging. The positive control is inside
   the same line — `received=7` proves the counter can report the other answer, so a
   zero would have meant something (gotcha 30).

   **So the sync point is at most an event near the START of the loop, not the condition
   sustaining it**, which is the second of the two readings the caveat below offered.
   Whatever moves the camera back and forth is elsewhere, and `sub_824A0FC0` is not it.
   Do not spend another session on the sync-point path without new evidence.

   **A PROBE THAT REPORTS FROM INSIDE THE FUNCTION IT COUNTS GOES SILENT EXACTLY WHEN
   THE INTERESTING THING HAPPENS.** This one only stayed readable because frames were
   visibly advancing while it said nothing, so "no report" could be read as "not
   called". That was luck, not design. Drive a probe's reporting from a clock that runs
   regardless — the graphics pump — or its most important state is the one it cannot
   express.

   **The superseded caveat, which pointed the right way:**
   **CAVEAT ON THE WAITING LINE, AND IT IS MINE TO FLAG: IT FIRES EXACTLY ONCE.**
   Over a 15-minute windowed run with the diagnostic layer on and the scene
   ping-ponging throughout, `WAITING: end sync point not received yet!` appears **1**
   time and `ForceClearAnimSyncPartner()` **2**. A cinematic permanently parked on that
   branch, with the code path running per frame, would print it thousands of times.
   So either the print site latches, or **the path is taken once and the ping-pong is
   sustained by something else**. Nothing here distinguishes those, and the second
   reading would make the sync point an event at the START of the loop rather than the
   condition holding it — a materially different defect. **Establish which before
   building anything on it**: the guard at `0x824A10BC`/`0x824A10D0` shows no latch, so
   the cheap check is a hook on `0x824A10D4` that COUNTS rather than prints.

   Corroborating the loop's determinism across all three runs: `distinct` is 1170,
   1170 and 1169 on two different binaries. The same fixed pose set every time.

   **AND THE ORDER IS SETTLED — the audio stopping is a CONSEQUENCE, not a cause.**
   One grep on the run that already existed, which is why it was worth writing the
   question down as an orderable one:

   | log line | event |
   |---|---|
   | **21184** | first `WAITING: end sync point not received yet!` |
   | 21479 | audio still rising, non-silent 6756 |
   | 22175 | audio still rising, 7189 |
   | **22398** | last rise, 7676 — silence from here on |

   The stall precedes the silence by ~1,200 log lines and 1,024 driver frames (~5.5 s),
   which is the already-queued dialogue playing out. The scene stops advancing, so it
   never fires its next audio event, and the mixer runs dry. **Do not chase the silence
   — it is downstream.** (Note what this does NOT say: the decoder still enabled all
   the progress there is, since with `CZ_NO_XMA_DECODE=1` the cinematic never starts at
   all. Audio got the scene moving; a sync point stops it.)

   **The PID reading is demoted, not discarded.** `Cine.Audio Cor Latency` and its
   gains are real and are in this manager, but the engine's own message points at an
   ANIMATION sync point, and nothing yet ties the oscillation to the controller. Do not
   quote the PID as the mechanism without evidence that it is running.

   **Next, in order, all cheap:** probe `sub_8249EEA8`'s inputs with `CZ_ARG_PROBE`
   (the worked example that closed finding 27); find who DELIVERS a sync point and
   whether an audio completion is one of its types; and check whether the mixer's
   silence begins before or after the first `WAITING` line, which orders cause and
   effect and is one `grep -n` on a run that already exists.

   **What is now certainly true and worth stating plainly:** this is a GUEST-side stall
   that our audio work exposed, not a rendering defect and not our output path.

   **The older PID-input note, kept because it is still a live thread:**

   **So the next step is the PID's INPUT, not our ring.** Read what `0x82475880`'s
   function computes as `Cor Latency` and compare it against what our decoder makes
   true; `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` on the prologue is the cheap first look,
   because the cinematic manager is a printf-heavy module and nobody has run it there
   yet. Note the timescales while doing it: the oscillation is ~0.87 s and the ring
   handshake ~12 Hz, two orders apart, so something integrates in between — which is
   what an I-gain does.

   **The original hypothesis, kept because the ambiguity it names is genuine:**
   `XmaFillOutput` in `kernel/audio.cpp` treats `write == read` as **EMPTY** at the top
   (`freeBlocks = blocks`) and as **FULL** at the bottom (`setOutValid(false)`). One
   state, two opposite meanings, in one function. Whatever the title computes its
   latency from, a ring whose fullness we report ambiguously is a bad input to a control
   loop. The standard repair is to never let `write` catch `read` — reserve one frame of
   slack so `write == read` unambiguously means empty.
   **State the prediction before building it**: the dialogue voices' `output_buffer_valid`
   stops toggling at 12 Hz, and `runs/distinct` falls from 6.1 toward 1.

   **Do not assume the ambiguity is the whole answer.** The oscillation period is ~0.87 s
   and the ring handshake is ~12 Hz, two orders apart, so something integrates in
   between — which is exactly what an I-gain does. If the ring fix does not close it,
   the next step is to read what `0x82475880`'s function computes as `Cor Latency` and
   compare it against what our decoder makes true, and `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1`
   on the prologue is the cheap first look because the manager is a printf-heavy module.

   **Workaround for anyone who just wants to play past it:** the title ships a
   `skip_cinematics` debug flag at **0x82A57D38** (bound by dataflow; reader at
   0x8247A898). It is not in `debug_tunables.cpp`'s curated table yet; adding it is ten
   minutes.

00e. ~~**THERE IS NO SOUND**~~ **CLOSED IN PHASE A/V — THE GAME MAKES SOUND.**
   `maxpeak=0.108854` and 15,991 of 18,433 frames non-silent on a plain boot, against
   `0.000000` and 0 with `CZ_NO_XMA_DECODE=1` on the same binary. An SDL device opens on
   pipewire and holds a steady 27 ms queue.

   **This item's ordering was right and is why the diagnosis was two lines.** Step 3 said
   output LAST; step 2 said the cause is upstream of the mixer; the caveat about telling
   silence from blindness is what made either statement trustworthy. Following that order
   meant the decoder went in before any device, so when it produced nothing there was
   exactly one place to look.

   **What was actually wrong was neither of the two things this item predicted.** The
   decoder was necessary but not sufficient: the XMA context's buffer pointers are
   PHYSICAL addresses (the APU is a DMA device) and our flat map puts the physical arena
   in a window at 0xA0000000, so reading them literally gives a page of zeros — which
   decodes to silence and reproduces the symptom exactly. Found by printing the
   DESTINATION of `NtReadFile`. `docs/phase-av-notes.md` §3, gotcha 267, and it is a Case
   West item on day one.

   **The trap this item warned about does not apply to us** — our pump has always been
   `sleep_until` on an accumulating deadline, not Fable 2's `sleep_for`, and it measures
   187.4-187.6 callbacks/s against the 187.5 that 48 kHz needs. The warning below is
   retracted; what was missing was the counter, not the fix.

   The original item follows, because its reasoning is the reason this went quickly.

00e-original. **THERE IS NO SOUND, AND THE FIRST QUESTION IS NOT THE OBVIOUS ONE.** Operator
   request, part 25. The obvious plan — port Fable 2's `audio_out.cpp` and open an SDL
   device — is probably NOT the first step, and one measurement says why.

   **What already exists here.** `runtime/kernel/audio.cpp` (662 lines) implements the
   XAudio render-driver client and the XMA context array, and it works: a headless run
   reports `render driver pump started (5333 us/frame, 256 samples x 6 channels)`,
   `XMACreateContext -> context 0 at BFFEB000`, and ~27,000 frames submitted in 150 s. The
   module's own header says what is absent: "There is no audio OUTPUT here and no XMA
   decoding. Submitted frames are counted and dropped." That is a real null sink, not a
   fake success (gotcha 5).

   **THE MEASUREMENT (`CZ_AUDIO_TRACE=1`, boot to gameplay via the DebugJump route): every
   sampled frame has `peak=0.0000`.** 53 sampled frames across ~27,000 submitted. **The
   guest is handing us silence**, so opening an output device first would produce nothing
   and the natural next conclusion would be "our output path is broken" — a session lost to
   the wrong subsystem.

   **CAVEAT, and fix it before trusting the above.** `peak` stays 0.0000 both when the
   frame is genuinely silent AND when `frame` is null, because the scan sits inside
   `if (frame)` and the variable is initialised to zero. Exactly-zero across every sample
   argues for real silence (a wrong pointer or a byte-order error gives nonsense values,
   not clean zeros) but the instrument cannot currently distinguish them. **One line: count
   the null-frame case separately.** This is the same blind spot as gotcha 151, in the one
   place it would send the whole item in the wrong direction.

   **PART 26 SETTLED IT: THE GUEST IS GENUINELY HANDING US SILENCE.** The instrument was
   rewritten rather than patched — every frame is scanned instead of one in 512, null
   frames are counted separately from silent ones, the first non-silent frame and the
   running maximum are reported, each buffer's guest address is printed, and the scanner
   is SELF-TESTED at pump start on a synthetic frame of big-endian 0.5f (a scanner that
   reads the wrong byte order reports zeros on any input, which is indistinguishable from
   silence — gotcha 30). Boot to the outdoor world:

       audio scan self-test: zero frame peak=0.0000, loud frame peak=0.5000
       audio frame 3072 ... frames=3073 null=0 non-silent=0 (first at 0) maxpeak=0.000000

   `null=0` says the mixer hands us real buffers; every sample of every one is zero; and
   the scanner demonstrably reports 0.5000 when there is something to report. **So step 1
   below is closed and the direction stands: the silence is upstream of the mixer, and the
   next step is XMA decode — not an output device.**

   **Then, in order:**
   1. ~~Settle silence-vs-blindness with that counter.~~ **CLOSED, part 26 — genuinely
      silent, measured with a two-sided instrument.**
   2. If genuinely silent, the cause is upstream of the mixer: XMA contexts are allocated
      but nothing DECODES, so every voice is empty. That is Fable 2's `audio/xma_hw.cpp`
      (430 lines, the hardware register contract) and `audio/xma_decoder.cpp` (192, ffmpeg)
      — and `~/GithubRepo/Fable2XenonRecomp/docs/audio-xma.md` is titled "why nothing the
      game mixed was audible", which is this symptom exactly.
   3. Output last: `audio/audio_out.cpp` (175 lines) takes planar big-endian float32,
      6 planes of 256 samples, downmixes to stereo and queues to SDL — **the same frame
      format this title submits**, so it should drop in.

   **A TRAP THAT APPLIES TO US VERBATIM, TODAY.** `audio-xma.md` has a section called "the
   pump was timer-driven, and the timer was wrong": `sleep_for(5333us)` overshoots and
   delivers ~184 frames/s where 48 kHz needs 187.5, and that ~2% deficit starves the device
   into a periodic stutter. **Our pump is timer-driven at exactly 5333 us.** Fable 2's fix
   is demand-driven pacing via `Audio_QueuedFrames()`. Expect the stutter on day one and do
   not diagnose it as a decode bug.

   **Licensing is clear**: Fable 2 is this workspace's own port, so its audio code can be
   lifted directly — unlike UnleashedRecomp, which is GPLv3 and structural-reference only.
   Per `docs/reusability.md`, extract only what is proven in BOTH ports and only after the
   second implementation forces the seam: copy into `runtime/audio/` here first, and leave
   any shared-library question to Case West.

00k. ~~**THE UI TEXT LAYER IS STALE**~~ **CLOSED IN PART 46, OPERATOR-CONFIRMED.**
   The fix is exactness EARNED per stream rather than bought by size: a stream the
   cross-frame store catches CHANGING is hashed exactly from then on, and one the
   cheap sampled guard is proved able to see is demoted back to it.
   `CZ_VK_GUARD_BUDGET` is the default; `CZ_VK_NO_GUARD_BUDGET=1` is the
   same-binary control arm, and the operator ran both in one session — the fix
   arm's HUD stayed correct throughout ("Ui stay good the whole time", then "Hud
   stay good and all" on the cheaper variant), the control arm's broke.
   **THE TWO WRONG TURNS ARE THE VALUABLE PART, do not re-buy either.**
   (1) *"Raise the guard's byte bound"* — the recommendation in two successive
   kickoffs — is **REFUTED**: at 256 KB the HUD still dropped out, and since part
   45's unlimited arm did fix it the UI buffer is ABOVE 256 KB, where the size
   histogram prices exactness at 121+ MB/frame. Size is the wrong discriminator.
   (2) The lazy promotion left a window at the start of a session (a stream is
   sampled until its first VISIBLE change) which the operator hit and which then
   SELF-HEALED — that self-healing was the first evidence the mechanism was right.
   Closing it by probing new entries had to be BUDGETED: unbounded it cost 838
   streams and 66.8 MB/frame, i.e. as much as hashing everything.
   **What is still owed**: it costs ~30 MB/frame on the operator's session, and
   their own A/B priced the earlier version at +22.7% in the 4500-6000 draw bin.
   That cost is folded into `docs/perf-plan-part47.md`, not treated as done.
   `phase5-notes.md` §6cc addendum.

00k-old. (the part-45 characterisation, kept for the chain) **THE UI TEXT LAYER IS STALE — MECHANISM CONFIRMED (part 45): it is the
   cross-frame stream store's GUARD, i.e. item 00c recurring above the 16 KB
   bound that fixed it. What remains is the FIX, and it has a cost to solve
   for.** A matched operator A/B settles it (§6bz addendum 2): the STATUS
   screen's KEY ITEMS tab, same save state, one env var apart — default guard
   renders the ATTRIBUTES tab's labels and none of its own content; with
   `CZ_VK_STREAM_GUARD_EXACT=1` it renders `Still Creek Map / Zombrex / Shed
   Key` correctly. **The fix cannot simply be "always exact": that arm read
   63.76 MB/frame in the guard against the default's 9.28**, which feeds
   straight into the performance regression of item 00l. Options to measure:
   raise the bound (free to try — `CZ_VK_STREAM_GUARD_BYTES=N`, no rebuild),
   or make exactness a property of the stream KIND rather than its size, since
   the store already distinguishes declared binding / index buffer / dependent
   fetch. **The frame-to-frame variation is explained by the guard too** — 8
   sampled blocks of 64 bytes catch an edit only when one lands on it — so
   part 45's TEAR reading is RETRACTED and `CZ_VK_FRAMES_IN_FLIGHT=1` is NOT
   the arm to run.
   Original characterisation follows.

   **Operator-reported, pre-existing, and it ACCUMULATES WITH SESSION LENGTH.** Reported at part 45's operator session with
   32 F9 captures (`~/DR2CZ-troubleshooting/part45-operator/ui_fixed/`), on the
   FIXED shader cache, and confirmed by the operator as long-standing rather than
   new. Symptoms, all three together: colour changes MID-WORD (`Whee|l`,
   `ATT|ACK`, `Wh|eel: RETURNED`), glyphs missing (`E___ne` for "Engine"), and the
   PREVIOUS screen's text persisting — the STATUS screen's SKILLS tab renders the
   ATTRIBUTES tab's labels, and the pause menu's `LEADERBOARDS / ACHIEVEMENTS /
   QUIT` stay painted over gameplay after it is closed. **Static text in the same
   frames is perfect**; only text whose content changes is wrong.
   Since §6ab established that the whole text layer is ONE dynamic vertex buffer
   sub-allocated per run by `VGT_INDX_OFFSET`, the reading is that the draws are
   this frame's and the VERTEX DATA is an older copy of that buffer.
   **Eliminated already** (§6bz): draws are not being dropped (both bounds-check
   counters read ZERO across 54.7M draws); the ALU constant window is read per
   draw, not assumed; and **a fresh headless session cannot reproduce ANY of it** —
   main menu, save-slot, Help & Options, the pause menu itself and the in-game
   banner all render correctly in short runs, which is a fact about the mechanism
   (it accumulates) rather than a failure to look.
   **Leading suspect**: the cross-frame stream store's guard, exact only to 16 KB
   and sampled above it — the profiler reports 1,785-3,296 streams/frame over the
   bound at a menu and the store reaching 12,162 entries in 20 s, and a missed edit
   is then served FOREVER because the guard keeps agreeing. That is item 00c's
   mechanism at a larger buffer. Not yet a finding: no arm has run against a
   reproducing instance.
   **Next, in order**: (1) the picture-free measurement — same route with the
   default guard and with `CZ_VK_STREAM_GUARD_EXACT=1`, comparing the `stale`
   counter, since any excess the exact guard catches is an edit the sampled one
   missed; (2) an operator arm, cheap because they reproduce in ~2 minutes —
   `CZ_VK_STREAM_GUARD_EXACT=1` separates "the store" from "the store's GUARD",
   which decides whether the fix costs 4.7 ms of a crowd frame or nothing.
   A positive control that ran `CZ_VK_STREAM_GUARD_BYTES=64` against Help & Options
   proved nothing and is recorded as such: that screen is STATIC, so the arm could
   not have failed (gotcha 30).
   **REFINED, and it changes the leading suspect**: three consecutive captures of
   ONE screen garble DIFFERENTLY (3.5-4.3% of pixels apart, a different fragment
   of the same strings surviving each time). A cache serving one stale copy would
   garble identically, so the better reading is a TORN buffer — copied while the
   guest is still writing it — which also predicts the observed scaling with
   session length (a tear needs reader and writer to overlap, and our command
   processor's LAG is what grows). Arms in order: `CZ_VK_FRAMES_IN_FLIGHT=1`
   (pre-part-23, removes a frame of lag), then `CZ_VK_STREAM_GUARD_EXACT=1`.
   **THE CHEAP HEADLESS CHECK FOR THIS CLASS, handed over by the operator**:
   press START at the title screen and a card appears for a second or two
   **completely EMPTY** — trim drawn, not one glyph inside
   (`part45-operator/ui_fixed/capture_001343.ppm`). It needs no play session,
   unlike every other instance, so it is the first thing to look at whenever this
   class is suspected.
   **UNVERIFIED**: an operator session ran on `CZ_VK_STREAM_GUARD_EXACT=1` and
   they said "everything works fine", but took no captures and the sentence
   equally reads as "no defects other than the trees". Ask before recording.

00l. **PERFORMANCE — EXECUTED IN PART 47 AND CONFIRMED BY THE OPERATOR ON THEIR OWN
   MACHINE. The live plan is now `docs/perf-plan-part48.md`.**
   **Their two-arm A/B, one binary, their route, matched on draw count: 64.1 ->
   42.8 ms, 15.6 -> 23.4 fps, `textures` 25.19 -> 4.45 ms**, with the picture
   unchanged and no staleness reported. Headlessly the crowd frame reaches the
   two-vblank floor (42-46 -> 32 ms, pinned share 5-13% -> 73-85%).
   **What remains, in `docs/perf-plan-part48.md`**: their PM4 walk at 16.6 ms
   (and its first item is an INSTRUMENT — `Pm4_OpcodeCount` has been counted since
   phase 4 and read by nothing, so nobody can say which packets those are);
   `record` re-measured after the guard fold; `other` at 4.19 ms and
   uninstrumented; and two items deliberately left alone (the bounded-prefix
   texture-guard lever, and multithreaded recording).
   **OWED**: the operator's confirmation of the guard fold, and an ISOLATED A/B of
   the vertex/index bind cache — the one part-47 change never measured alone, and
   `record` came out ~1 ms higher on the part-47 arm in both datasets. If it is a
   loss, delete it.
   The part-47 text follows.

00l-part47. **PERFORMANCE — the part-46/47 plan, kept for its derivations.**
   **Read `docs/perf-plan-part47.md`'s STATUS header first**; the plan's budget
   and ranking stand, its price for item 1.1 does not.
   **The plan's own named first run settled its top item and repriced it upward.**
   `CZ_VK_NO_TEX_REVALIDATE=1` on the outdoor route: `textures` is **15.9 ms with
   the revalidation guard and 2.3 ms without**, against the 8-11 ms estimated —
   the guard is nearly the whole texture phase. In the 5,000-8,000 draw bin the
   frame goes from 47-48 ms at 23-24% pinned to **32-33 ms at 67-94% pinned**,
   i.e. onto the two-vblank floor. That arm is not shippable (it is the defect
   part 38 fixed); it is the upper bound, and it cost one run.
   **The fix is a CADENCE change**: the guard runs once per frame per cache entry
   rather than once per texture fetch per draw — **93.4% of checks skipped, 15.1x
   less hashing**, arm `CZ_VK_TEX_GUARD_EVERY_FETCH=1`. Landed with it: the
   texture path's counters (`Count`→`COUNT`), the per-fetch linear scan behind its
   readers' gate, bulk register runs in the PM4 walk (verified against the code
   they replace — 0 mismatches over 152 M dwords, 100.0% bulk), the state cache
   extended to the vertex and index binds (51%/39% repeat rate), and the per-fetch
   sampler lookup as a flat table.
   **What remains**: tier 3.2 (multithreaded recording, deliberately last), and
   the one lever inside 1.1 that trades detection for cost —
   `CZ_VK_TEX_GUARD_BYTES=N` exists with its default UNCHANGED and the histogram
   that prices it prints with the stats. **AND THE OPERATOR'S OWN CONFIRMATION IS
   OWED**: `tools/part47_operator_session.sh` is the two-arm driver, because the
   headless route understates their draw path by ~2x.
   The part-46 text follows.

00l-part46. **PERFORMANCE — the part-46 measurement that produced the plan:** Part 46 profiled the operator's real windowed
   frame for the first time: **61.7 ms at 7,231 draws**, textures 26.5 ms, PM4
   walk 14.2, record 10.9, GPU 34% utilised with `submit.gpu` 0.0 — a pure CPU
   problem. Target 33 ms, because the 360 shipped this game at 30 fps.
   **The headline: the texture revalidation guard read 366 GB over one session
   (92.9 MB/frame) to catch 986 real changes out of 26.8M checks — 0.0037%.** Its
   upper bound is knowable in ONE run, `CZ_VK_NO_TEX_REVALIDATE=1`, and that is
   the first thing to do before any code is written.
   The three suspects this item originally named (part 41's samplers, part 44/45's
   mips, part 45's liveness fix) are all EXONERATED — but on the HEADLESS route,
   which understates the operator's draw path by ~2x, so they are cleared on a
   workload half as expensive as the real one. §6cb + addendum, §6cc.
   The original text follows.

00l-old. **PERFORMANCE REGRESSION over parts 41-45 — operator-reported, unmeasured.**
   *"The performance degraded with all the fix you did in the last few days."*
   Part 46's second item, immediately after the trees, by their own ordering.
   Suspects in order of introduction: part 41's per-fetch samplers; part 44/45's
   mip-chain and packed-tail uploads; and **part 45's liveness fix, which added
   interpolants to 217 of 333 pixel shaders** — more varyings means more
   interpolation and more register pressure, and `assets/shader_spv_pre45` makes
   that a ONE-VARIABLE same-binary A/B via `CZ_SHADER_SPV`.
   Measure per `docs/measurement.md`: `CZ_VK_PROFILE` for the phase split, three
   runs an arm alternated, MEDIANS and the share of frames within 1 ms of a 16 ms
   multiple — never the mean, which measures this title's vblank pacing floor
   (gotchas 237/238). Noise floor 10-13% at one run a side; sample the GPU clock
   with `tools/gpu_clock_sample.py` rather than assuming it.

00m. **DECALS — operator-reported, part 47, NOT investigated and NOT introduced by
   the performance work.** Their words: *"still has the issue with decals that I
   think I didn't warn you about"* — i.e. a standing defect that had never been
   filed, reported alongside the confirmation that part 47's performance changes
   left the picture *"pretty much the same as last time"*. **No captures yet and
   none requested**; the operator offered and the answer was to stay on
   performance. What is known: it pre-dates part 47, and it is theirs to
   characterise when the performance work is done.
   The likely handle when it is picked up: decals are a separate draw pass with
   their own blend state, so `CZ_VK_DRAW_CENSUS` on a frame containing one plus
   `CZ_VK_DRAW_ID` should name the draws in a single capture, and the title
   screen / menu backdrop is worth checking first for a self-servable repro
   (gotcha 319).

00n. **A SIGN AND SOME ITEMS STILL WRONG AT DISTANCE — operator-reported, part 47.**
   *"some sign and item that still got issue with distance but this is not
   introduced by your performance fix."* This is the tail of item 00i's
   flat-at-range class, most of which part 45's interpolant-liveness fix closed
   on the operator's own A/B. **Do not read it as a regression**: they said
   explicitly that it is not, and the part-47 arms are a same-binary control if
   that ever needs testing. Related: the parked mip-selection overshoot, whose
   decisive arm (`CZ_VK_NO_MIPS=1` on the FIXED shader cache) has still not been
   re-run since part 45 — that is the first measurement to make here, not a new
   investigation.

00f. **WHITE PATCHES ON WORLD SURFACES — an operator session's evidence, and it is at
   least TWO defects.** Reported part 26 and reproducible on every run: large blown-out
   patches of ground with hard polygon edges (spawn area, gas-station forecourt), fully
   white newspaper boxes, white parts of a cactus, a blown-out bathroom window, a mirror,
   a cash register, a door side and a gas-station sign. Six `CZ_VK_DRAW_CENSUS` frames are
   in `~/DR2CZ-troubleshooting/part26-operator/`.

   **SOLVED IN PART 33 — `docs/phase5-notes.md` §6bg is the record, and everything below
   is history of the investigation.** The plateau was the shared tone epilogue evaluated
   at **`x = NaN`** (`max(NaN,K1) = K1`, `saturate(NaN) = 0`, so `out = sqrt(K1*K2)` =
   exactly 180/255, invariant under every constant arm — which is why parts 27-31's four
   whole-frame arms all read "unmoved"). The NaN entered in the VERTEX FETCH: this title
   wraps its fmt16 `k_10_11_11` packed normals as TEXCOORD, whose shader input is
   `float4`, while the runtime bound the attribute `R32_UINT` — a pipeline type mismatch
   (`VUID-VkGraphicsPipelineCreateInfo-Input-08733`, 10 pipelines, 37 vertex shaders)
   that delivered the packed dword's bits AS a float: NaN wherever bits 30..23 are all
   ones, garbage lighting on every fmt16 mesh otherwise. Fixed by XenosRecomp 4621beb
   (in-shader `XeUnpack_10_11_11` for float-usage fmt16 elements) + runtime 7889e99
   (fmt16 binds `R32_SFLOAT`): plateau **1,092 px -> 0** at the same route and frame,
   scene mean luma 35.5 -> 44.7, distinct colours 80k -> 112k, and the crowd's blotchy
   flat-lit patches are gone. **THE OPERATOR'S CONFIRMATION IS IN, same day: all seven
   part-27 locations toured and captured on the fixed renderer, scene-buffer plateau
   ZERO in every one** (`~/DR2CZ-troubleshooting/part33-operator/`, table at the end of
   §6bg). Still owed: a re-measure of the exposure discrepancy (ours 1.0 vs hardware
   0.298-0.331) now that the scene auto-exposure adapts to a correctly lit world. The
   cube-decline prop defect (the `s3`/`s4` duplicate served the dummy) is a SEPARATE
   defect and remains open below.

   **READ THIS FIRST — PART 31 RETIRED THE MODEL EVERYTHING BELOW IS WRITTEN IN.**
   Parts 27, 28, 30 and the first half of 31 all read the plateau as *the shared tone
   curve evaluated at `x = colour * pc(14).w = 1`*. It is not. Four whole-frame arms in a
   row leave the pixels at exactly `rgb(180,180,180)` untouched, and the fourth is
   decisive: `CZ_VK_PS_CONST_SCALE="14.w=0.25"` engaged on 11,835,619 draws and took the
   scene buffer from mean luma 35.07 to 18.30, and **zero pixels landed on 119**, which is
   where that curve sends `x = 1` when the exposure is quartered. They stayed on 180. A
   value produced by that curve cannot be invariant under scaling its exposure. The other
   three arms — the sun `c24`, the additive `c67.w` term, and the whole multiplicative
   path `c1.xyz` (an arm that blacks out 61.5% of the frame) — say the same about the
   ground shader's colour terms. `docs/phase5-notes.md` §6bd and §6be.

   So `180 = 255 * sqrt(0.5)` is the coincidence to explain, not the explanation, and
   **the next step is a per-draw instrument rather than another whole-frame arm**: dump
   `CZ_VK_DRAW_CENSUS` on a frame with a large plateau and skip candidate draws with
   `CZ_VK_SKIP_TEX` until the 180 pixels go. Do not re-buy any of the four arms above.

   Two things below are confirmed rather than retired, both re-measured in part 31 on a
   DAYLIGHT frame rather than the night captures part 30 withdrew: the plateau is a hard
   pin (1,348 px at exactly 180 and **zero** at grey 181, 182 or 183, in a frame with mean
   luma 36.3 and 63,398 distinct colours), and the 32 pixel constants the ground draw
   reads are hardware's — every one that is not a function of the camera, including
   `pc(21)`, a point light's world position, to the printed digit (§6bb). The constants
   are exonerated as a class.

   **ESTABLISHED (in the retired model — read the paragraph above first)**
   * **It is not the tone map.** The white is already in the SCENE buffer (`0684B000`)
     before any post pass, and that buffer's maximum is **180**, not 255 — so nothing is
     clipping. The presented frame is the same picture, graded.
   * **One prop defect is confirmed and has a mechanism.** In frame 10211 a draw binds the
     SAME texture twice — `s3=11C12000 slot=4000` and `s4=11C12000 slot=0(DUMMY)` —
     because the shader declares that slot a CUBE map while the fetch constant describes a
     2D texture. Part 25 declines that case (reading six faces out of a surface the guest
     calls one would build a cube from five slabs of neighbouring memory), and declining
     means the 1x1 **white** dummy. A reflection term multiplied by white blows the surface
     out. `01330000`, the 4x4 already on file as uploading black, is served the dummy on
     eight alpha-blended draws in the same frame.
   * **The ground is a different defect.** Its draws bind three real DXT1 textures at real
     slots plus the shadow cascade twice, with `vs=36eef2c94b4a065c ps=ad65b98593f95926`
     — identical at the spawn and at the gas station, so it is ONE material misbehaving in
     places rather than a per-location accident. `CZ_VK_DRAW_PROBE` on that shader shows
     texture coordinates that VARY sensibly across vertices (0.117, 0.125, 0.110 ... in u),
     so the coordinates are not constant.

   **REFUTED, and both were mine**
   * "The ground is untextured." Drawn from ONE 240x150 crop that happened to be flat.
     Other ground regions of the same frame read stdev 63-72 with hundreds of distinct
     colours, i.e. as textured as a character. The defect is patches, not the material.
   * "Four of the six censuses show zero dummy binds." Those censuses each dropped 105-215
     silently TRUNCATED lines, so absence proved nothing (fixed; see the commit).

   **THE RETAKE, AND A POSITIVE CONTROL WITH A ZERO NULL (still part 26).** Three censuses
   on the fixed binary, headless, zero truncated lines: the dummy pattern is stable and
   identical in all three frames — 4 draws, `vs=2f13eecec64e508e`, 1,418 vertices, binding
   `sN` and `sN+1` to the SAME 128x128 texture with the second declined to the dummy.

   Then `CZ_VK_CUBE_POISON=1` against a base arm, both dumped with F9, read as a per-pixel
   diff on the SCENE buffer rather than by hunting for magenta (gotcha 248 — the sample
   TINTS, and a semantic detector reads 0.00% where the diff reads plenty):

   | comparison | mean \|RGB\| | pixels differing > 30 |
   |---|---|---|
   | within base — the null | **0.0000** | 0.00% |
   | within poisoned — the null | **0.0000** | 0.00% |
   | base vs poisoned | **1.4321** | **2.18%** |

   **Two frames 23 seconds apart in one arm are PIXEL-IDENTICAL** standing still at this
   spot, so the noise floor is exactly zero and 2.18% is unambiguous. **The cube dummy
   reaches the picture.**

   **AND IT LANDS ON THE CROWD, NOT ON THE REPORTED SURFACES** (`cube_poison_diff_overlay.png`
   in `~/DR2CZ-troubleshooting/part26-retake/`): every changed pixel is on zombies and
   pedestrians. The ground, the vans, the tents and the props are untouched. With the
   counters — `draw: cube fetch got the dummy` **14,670 of 5,242,455 (0.28%)** — that is
   the whole of it: a small share of CHARACTER cube fetches is declined and reads white.
   The 1,418-vertex draws are characters; part 26 assumed "props" from the operator's frame
   and never checked, which is what the overlay corrected.

   **So the operator's white ground and white props are NOT the cube dummy**, and that is
   now shown by a positive control rather than by an absent counter. They remain open with
   every earlier hypothesis (post-processing, missing textures, constant UVs, the dummy)
   refuted.

   **THE ORACLE SAYS THESE ARE OURS.** The operator has played this title on Xenia and
   reports NONE of these surfaces are wrong there. That is the check nobody had run: every
   measurement in this item assumed the report described a defect rather than the game's
   own appearance, on one observer and no oracle. It does describe a defect, and it is in
   this runtime.

   **EVERY CHEAP EXPLANATION IS NOW REFUTED, each by a measurement rather than an
   argument** — worth listing, because the value left in this item is not re-buying them:

   | hypothesis | what killed it |
   |---|---|
   | tone map / exposure clipping | the white is in the SCENE buffer, whose max is 180, not 255 |
   | a missing or unbound texture | the ground binds three real DXT1s at real slots plus the shadow cascade twice |
   | constant texture coordinates | `CZ_VK_DRAW_PROBE` shows UVs varying sensibly across vertices |
   | the 1x1 white dummy | **all four dummy heaps poisoned magenta — the ground stayed white** |
   | pixels never written, showing the clear | `RB_COLOR_CLEAR` is **black**; unwritten pixels would be black |
   | an HDR EDRAM format read as 8-bit | `rtFmt=0` (`k_8_8_8_8`) on 600 of 600 passes |
   | a ground texture that decodes flat | the new uniform-texture counter finds 9 in a run, **none of them the ground's** |

   **WHERE IT MUST THEREFORE BE.** The inputs are all correct, so the defect is in the
   SHADING or in render state this runtime ignores. ~~One concrete lead, visible in every
   census and not yet followed: the ground mesh is drawn **twice with `mask=F`** in the same
   frame, same vertex shader and same textures... If one of those passes is meant to combine
   with the other rather than overwrite it, the surviving pass is whatever was drawn
   last.~~ **REFUTED IN PART 27, AND IT WAS TILING.** `CZ_VK_DRAW_PROBE` prints the
   scissor, and the two draws of the ground mesh carry `scissor 0,0 640x720` and
   `scissor 640,0 640x720` — this title's left and right 640-wide halves, which CLAUDE.md
   already records from the other end. There is no second pass to combine (gotcha 265).
   The `mask=0` draws remain unexplained and are the depth/shadow prepasses.

   **PART 27 CLOSED THE LAST INPUT, AND EVERY COMPARABLE ONE NOW MATCHES.** The vertex
   data was the input part 26 could not compare; `tools/xtr_draw_vertices.py` decodes
   hardware's streams out of `w1_spawn.xtr` in the same shape `CZ_VK_DRAW_PROBE` prints
   ours, and for the 25,234-vertex ground draw all five attributes agree to the printed
   digit over the first six vertices:

   | attribute | hardware | ours |
   |---|---|---|
   | loc0 POSITION fmt57 slot95 stride3 | `-23.79100, 3.10100, 25.04500` | identical |
   | loc4 fmt37 slot94 stride2 off0 | `0.11789, 0.49056` | identical |
   | loc5 fmt16 slot93 stride4 off0 | `FEA017E2` | identical |
   | loc6 fmt37 slot93 stride4 off1 | `0.11789, 0.49056` | identical |
   | loc7 fmt25 slot93 stride4 off3 | `4BB10240` | identical |

   **And the recorded anomaly is not one.** loc4 and loc6 decoding identically — two float2
   attributes from two different streams carrying the same values — is what HARDWARE does
   too. The guest genuinely duplicates that texture coordinate into both streams. That
   closes the lead by showing the mechanism is real and shared, not by failing to find it.

   **The SHADER CONSTANTS were compared too**, which part 26's list also missed, and they
   match wherever the capture can answer: `pc(1)`, `pc(22)`, `pc(45)`, `pc(46)` identical;
   `pc(14)` is a world position and differs with the camera, as it must; `pc(0).w` reads
   1.0 on hardware and 0.0697 for us but the ground pixel shader never reads `c0`.
   `pc(253..255)` are **UNRECOVERABLE from this capture** — 81 of `w1_spawn`'s 620
   `LOAD_ALU_CONSTANT` packets read memory the trace does not carry, and the tool now says
   so instead of printing the stale value (gotcha 263). Those three are the only inputs
   still unknown, and `c253/c254/c255` ARE read by `ps_ad65b98593f95926`, so **a capture
   that carries the constant-buffer memory would close the input list completely.**

   **SO THE GROUND DEFECT IS IN THE SHADING.** Every input that can be compared matches:
   shader pair, vertex count, bindings, texture contents, render state, vertex data, and
   the recoverable constants. The next step is to read our translated
   `ps_ad65b98593f95926` against the capture's own disassembly of it —
   `~/DR2CZ-troubleshooting/r2-shaders/shader_D007C18389DF0E55.ucode.frag`, 187 lines,
   located by hashing the dword-swapped `.ucode.bin` (gotcha 261).

   **PART 27, FROM THE OPERATOR'S NIGHT CAPTURES: THE WHITE IS A CONSTANT, AND ITS VALUE
   IS rgb(180,180,180).** This is the sharpest the item has ever been and it came from two
   things the earlier work did not have — `DISABLE TIME OF DAY`, which put the world at
   night so an unlit surface stops hiding in a lit scene, and `CZ_CAPTURE_KEY`, which
   captures the picture, the census and all 67 resolve snapshots of ONE frame.

   * **The surfaces are not modulated by scene lighting at all.** In the slot-machine room
     90.1% of the presented frame is at or below luma 40 and 5.91% is fully saturated
     255,255,255. The cabinets are pure white in a pitch-black room. A reflection term
     multiplied by a white cube map would go dark when the sun does; these do not.
   * **In the SCENE buffer they are one exact colour.** `0684B000`, the slot-machine
     frame: **52,840 pixels at exactly rgb(180,180,180) — 5.73% — and the next most common
     colour above luma 150 has TWO pixels.** That is a plateau, not a bright surface.
   * **The same constant at every one of the seven locations**, 1.81% to 15.36% of the
     frame, and in five of the seven the whole 1280x720 buffer never exceeds 180:

     | frame | place | scene mean | scene max | px at exactly (180,180,180) |
     |---|---|---|---|---|
     | 2714 | w1_spawn | 50.5 | 180 | 141,564 (15.36%) |
     | 4833 | w3_pawnshop | 48.5 | 180 | 114,381 (12.41%) |
     | 6363 | w6_register_door | 26.4 | 180 | 63,562 (6.90%) |
     | 5409 | w2_gasstation | 36.1 | 255 | 53,256 (5.78%) |
     | 6668 | w7_slotmachine | 27.2 | 180 | 52,840 (5.73%) |
     | 7103 | w4_bathroom | 20.2 | 255 | 15,822 (1.72%) |
     | 4350 | w5_newsboxes | 36.5 | 180 | 16,692 (1.81%) |

   * **The tone map then maps 180 to exactly 255**, and it is faithful in doing so: 96.1%
     of the presented frame's white pixels were already >= 150 in the scene buffer, with
     scene luma mean 173 and **max exactly 180**. So the tone map is the AMPLIFIER and not
     the cause — part 26's "it is not the tone map" survives, but its supporting sentence
     ("the white is already in the scene buffer, whose maximum is 180") was describing this
     plateau without recognising it as one.

   ~~**THE ARITHMETIC, AND THE PREDICTION IT MAKES.** 180/255 = 0.70588, and sqrt(0.5) =
   0.70711, i.e. **255 * sqrt(0.5) = 180.3**. A shader writing a literal **0.5** into a
   surface encoded with **gamma 2.0** lands on exactly 180... if this is the mechanism it
   is the Xenos `k_8_8_8_8_GAMMA` encode.~~ **RETRACTED IN PART 30 — the arithmetic is
   right and the gamma is imaginary.** 180 is `255 * sqrt(0.5)` because the shader's last
   instruction is a `sqrt` and the value under it is `0.5`; the surface is an ordinary
   UNORM one and no gamma encode is involved. Both halves of the prediction were then
   tested and both failed: our translation of `ps_ad65b98593f95926` is **instruction for
   instruction identical** to the capture's own disassembly of it
   (`~/DR2CZ-troubleshooting/r2-shaders/shader_D007C18389DF0E55.ucode.frag`), including
   all six `_sat` modifiers, and no constant `0.5` is written anywhere — the 0.5 is
   `K1*K2` from the tone curve's own constants.

   **PART 30 READ THE CONSTANTS, AND 180 IS HARDWARE'S VALUE AT FULL EXPOSURE.**
   `tools/xtr_draw_constants.py` recovers the pixel-shader ALU constants from five of the
   seven captures (part 27 asked only `w1_spawn`, got `UNRECOVERABLE`, and recorded that
   the capture cannot answer — it cannot answer *there*). The epilogue every one of the 48
   shaders shares is, with `x = colour * exposure`:

       out^2 = (max(A*x + B, K1) - saturate(K1 - x)^2) * K2
       hardware: A = 0.25, B = 0.75, K1 = 1.0, K2 = 0.5   (68 draws, five captures)
       ours:     A = 0.25, B = 0.75, K1 = 1.0, K2 = 0.5   (64 distinct bindings)

   | x | 0 | 0.5 | 0.9 | **1.0** | 1.05 | 1.5 | 3 | 5 |
   |---|---|---|---|---|---|---|---|---|
   | 8-bit | 0 | 156 | 179 | **180** | 181 | 191 | 221 | 255 |

   The constants are ours as well as hardware's, so the tone curve is not the defect. The
   literal pool is **per shader** and both pools are correct here: `ps_ad65b98593f95926`
   reads the same four numbers one register lower than `ps_7d2f8f33deec1b65` does.

   **What this retires, corrected.** ~~They are not shaded at all. Any hypothesis of the
   form "term X is too large" is dead.~~ Too strong: the curve's derivative vanishes at
   `x = 1`, so **180 is the whole band `x` in [0.9055, 1.0080]** and a flat 180 is equally
   consistent with a normally-shaded surface sitting at full exposure. What survives, and
   it is the sharper fact, is that these surfaces sit in the band `x` in [0.905, 1.008]
   **at night, in a pitch-black room, unmodulated by lighting or time of day**. (The
   frame MAXIMUM being 180 in five of seven captures is NOT additional evidence and was
   briefly written up as if it were: those five are the `DISABLE TIME OF DAY` captures,
   where the defect is by construction the brightest thing in the frame.) The lit colour
   tracking `1/exposure` is part 28's `c = 1/pc(14).w`, now reached from the
   picture rather than from a paint probe that could not have reported otherwise (the
   probe asked whether `0.25x + 0.75` falls below 1.0, which it can only do for `x < 1`).

   **And the clamp is not in the shader** — six `_sat` in hardware's disassembly, six
   `saturate()` in ours, on the same six instructions. It is on an input. The next step is
   named at the end of `docs/phase5-notes.md` §6ba: an exposure arm (`pc(14).w` overridden
   at bind time) separates "a genuine product at full exposure" from "`c` pinned at `1/E`
   by something upstream" in one run, and our `pc(14).w` reading 1.0 where hardware reads
   0.298-0.331 is the ratio that clamp would predict.

   **THE OPERATOR'S TAG-PAINT SESSION NAMED THEM: 48 PIXEL SHADERS, ONE SHARED EPILOGUE.**
   59 captures roaming the map with `assets/shader_spv_tagpaint`, decoded with
   `tools/shader_tag_decode.py` over the scene buffers — 1.5 M painted pixels, 48 shaders
   named, one 16-bit tag collision correctly flagged AMBIGUOUS, and a noise floor of a
   single pair (pure white, 107,571 px) correctly separated from the tags.

   | px painted | shader |
   |---|---|
   | 703,376 | `ps_7d2f8f33deec1b65` |
   | 170,051 | `ps_e2c3ca8c13351984` |
   | 144,217 | `ps_861d32db6c0f0556` |
   | 136,450 | `ps_d4609f2df48bcc48` |
   | ...44 more, 66,142 down to 23 | |

   **48 shaders is not 48 defects.** Their disassemblies end in the same four
   instructions, with only the constant SLOTS differing — the compiler allocates the
   literal pool per shader:

       max  r0.xyz, r0.xyz, K1        ; K1 = c255.w / c254.w / c255.y ...
       mad  r0.xyz, -r1, r1, r0.xyz   ; -= r1*r1
       mul  r0.xyz, r0.xyz, K2        ; K2 = c254.z / c255.x ...
       sqrt oC0.rgb = sqrt(|r0.xyz|)

   So the plateau is that epilogue's **FLOOR**: with `r1 = 0` and the colour under `K1`,
   the output is `sqrt(K1 * K2)` — a constant, independent of everything the shader
   computed above it. On the ground shader `K1 = 1.0` and `K2 = 0.5`, so
   `sqrt(0.5) = 0.70711 = 180/255`. **The same 180 at seven locations across 48 materials
   is one shared idiom hitting its floor**, which is why no per-material theory ever fit.

   ~~**This moves the item into the TRANSLATION LAYER**, and if it is XenosRecomp's
   rendering of the idiom it is a Fable 2 defect too.~~ **CHECKED AND RETRACTED THE SAME
   DAY.** The generated HLSL for `ps_7d2f8f33deec1b65` renders the epilogue one-to-one
   against the microcode, with no reinterpretation to be wrong about:

       r1.xyz = saturate(-r0.xyz * pc(14).www + pc(254).www);
       r0.xyz = r0.xyz * pc(252).www;
       r0.xyz = r0.xyz * pc(14).www + pc(252).xxx;
       r0.xyz = max(r0.xyz, pc(254).www);
       r0.xyz = -r1.xyz * r1.xyz + r0.xyz;
       r0.xyz = r0.xyz * pc(255).xxx;
       oC0.rgb = sqrt(abs(r0.xyz));

   **So there is no emitter defect here and nothing for Fable 2 to inherit.** Worth having
   asked — the emitter is shared and a defect in it would have been two ports' worth of
   picture — but it costs one command to check and the answer is no.

   **AND 180 IS THIS OPERATOR'S KNEE, WHICH IS WHY IT IS THE SAME EVERYWHERE.** Probed on
   the top offender: `pc(14).w = 0.1`, `pc(252) = (0.75, ., ., 0.25)`, `pc(254).w = 1.0`,
   `pc(255).x = 0.5`. Substituting, with `c` the colour arriving at the epilogue:

       r1  = saturate(1 - 0.1c)          ->  0 at c = 10
       c'  = 0.025c + 0.75               ->  1 at c = 10
       out = sqrt((max(c',1) - r1*r1) * 0.5)

   **Both branches meet at exactly `c = 1/pc(14).w`**, and there the output is
   `sqrt(K1*K2) = sqrt(0.5) = 180/255` — for ANY exposure, because the constants are
   chosen to keep the curve continuous at the knee. That is why the plateau is the same
   180 in daylight and at night, at seven locations, through 48 shaders: **it is not a
   clamp and not a constant the shader writes, it is the one value this operator produces
   when its input sits exactly on the knee.**

   **So the defect is UPSTREAM of the epilogue**: something pins the colour reaching it at
   exactly `1/pc(14).w`. The epilogue is only where that becomes a flat grey.

   **AND THAT IS NOW MEASURED, NOT INFERRED.** Two readings fitted the plateau and produced
   the same output, so nothing downstream could separate them: either the colour sits
   exactly ON the operator's knee (`c' == K1`, the max a TIE), or it is BELOW the knee with
   the `-r1*r1` term dead (`c' < K1`, the max taking its FLOOR). The discriminator has to
   be recorded where the max happens, so `XE_FLOOR_PAINT` instruments every `max` whose
   operands differ — `max(a, a)` is this compiler's `mov` and would saturate the flag —
   and paints MAGENTA if any took its floor, GREEN if none did:

   | | px |
   |---|---|
   | GREEN — no max took its floor | **1,628** |
   | MAGENTA — a real max took its floor | **1** |
   | left at (180,180,180) | 0 |

   **So `c' == K1` exactly.** With `c' = 0.025c + 0.75` and `K1 = 1.0`, that is
   **`c = 10.0`, which is exactly `1/pc(14).w`** — the colour arriving at the epilogue is
   pinned at the reciprocal of the exposure constant. Not stale, not NaN, not a floor: a
   value that lands precisely where the operator's two branches meet.

   **That is the shape of a CLAMP to the maximum representable pre-exposure value** —
   `min(colour, 1/exposure)` or the equivalent — somewhere upstream in the shared lighting
   path. Next is to find what performs it, and whether hardware clamps to the same place.
   ~~Note `clamp(rcp(x), FLT_MIN, FLT_MAX)` is XenosRecomp's own rendering of `rcp` and is
   the first thing to look at.~~ **CHECKED AND ELIMINATED.** Two ways it could have been
   wrong, and it is neither:

   * **The name lies but the value does not.** `FLT_MIN` is `#define FLT_MIN
     asfloat(0xff7fffff)` = **-3.4028235e38**, i.e. minus FLT_MAX — not C's smallest
     positive normal. So the clamp is symmetric and a negative reciprocal keeps its sign.
     Worth stating out loud: had it carried the C meaning, every `rcp` of a negative
     number would collapse to +1.2e-38, silently, in **both ports**. It does not.
   * **The finite maximum does not land on the knee.** `rcp(0)` gives +inf, clamped to
     +3.4e38. Pushed through the epilogue that is `c' = 0.025 * 3.4e38`, and
     `sqrt(c' * 0.5)` overflows the 8-bit target to **255**, not 180. A saturated pixel
     and a knee pixel are different values and the plateau is the knee.

   So the pin at `1/pc(14).w` is not manufactured by the `rcp` clamp. The value feeding the
   epilogue is a fog LERP — `c = (r2 - pc19) * r0.x + pc19`, with `pc(19)` the fog colour
   and `r0.x` a fog factor built from `pc(18)`.

   **THE FOG FACTOR IS FINE, THE EPILOGUE'S INPUT READS ZERO, AND THAT CONTRADICTS THE
   FLOOR MEASUREMENT. Recorded unresolved.** `ps_7d2f8f33deec1b65` was hand-instrumented
   (it is 47% of the plateau on its own, and a per-register probe cannot be emitted
   generically because the register number is a property of the compilation) to encode two
   intermediates as a 16-bit log2 fixed point in the painted pixel:

   | probe | reading on plateau pixels |
   |---|---|
   | `r0.x`, the FOG FACTOR | **0.90 - 0.91** — entirely sane, no runaway |
   | `c`, the value ENTERING the epilogue | **<= 1e-20**, i.e. zero (1e-20 is the probe's own clamp floor) |

   With `c = 0` the epilogue gives `r1 = saturate(1) = 1`, `c' = 0.75`,
   `max(0.75, 1.0) = 1.0`, `1.0 - 1*1 = 0`, `sqrt(0) = 0` — **BLACK, not 180**. And that
   `max` would be taking its FLOOR, where `XE_FLOOR_PAINT` measured 1,628 green (no floor)
   against 1 magenta.

   **Two of this session's own measurements disagree.** The first guess was that they are
   different POPULATIONS — the floor paint instrumented all 48 plateau shaders, the fog
   probe exactly one. **That was checked and it is NOT the explanation.** The floor paint
   restricted to `ps_7d2f8f33deec1b65` alone, five frames:

   | | px |
   |---|---|
   | MAGENTA — a real max took its floor | **0** |
   | GREEN — no max took its floor | **1,157** |
   | still (180,180,180) — the other 47 shaders | 5,068 |

   Same answer on the one shader. So the two instruments genuinely contradict each other on
   the same pixels, and one of them is wrong.

   **IT IS THE HAND PROBE, AND THE REASON IS STRUCTURAL RATHER THAN A PREFERENCE.** The
   translated shader is a `switch` over exec blocks inside a loop, so a block can execute
   MORE THAN ONCE per pixel. `xe_floor` accumulates with `f = f || ...` — monotonic and
   order-independent, so no later iteration can undo what an earlier one saw. `xe_c = r0.x`
   is last-write-wins, so on a shader that re-enters the block it records whichever
   iteration ran last, which need not be the one that fell through to the epilogue.
   **A last-write probe and an accumulating flag are not interchangeable instruments on
   looping control flow**, and only the accumulating one is safe here.

   So the floor reading stands: no max took its floor, therefore `c' >= K1`, therefore
   `c = 1/pc(14).w`. The `xe_c ~ 0` reading is an artifact of the probe's own semantics and
   the fog-factor reading (0.90, from the same probe, same block) inherits the same doubt —
   it is *plausible* and it is not *established*. Re-probing needs an accumulating form:
   min and max of the value across all iterations, not the last one.

   **One honest correction:** `ps_ad65b98593f95926`, the ground draw's pixel shader that
   part 27 read line by line, is **NOT in the list**. The white ground is painted by one
   of the other 48 — most likely the 703 K-pixel leader — so the shader I was reading was
   the right *idiom* and the wrong *module*. The epilogue is shared, so the reading
   transfers; the attribution did not.

   **NEXT, in order**
   0. **The character cube fetches that are declined** — 0.28%, visible on the crowd, and
      the only part of this item with a mechanism. Decide what the honest fallback is: our
      white dummy maximises a multiplicative reflection term, black would kill it, and
      replicating the 2D texture across six faces is what the guest's own data supports.
      Measure the three against the poison overlay's pixels.
   1. Retake the censuses on the fixed binary — `CZ_VK_DRAW_CENSUS` now writes one file per
      frame and marks truncation. The outdoor ones need no operator.
   2. `CZ_VK_CUBE_POISON=1` as the visual control for the prop defect: if the white
      newspaper boxes and window turn MAGENTA they sample the cube dummy, and if they stay
      white that mechanism is dead. One operator run, two minutes.
   3. For the ground patches, bisect with `CZ_VK_SKIP_TEX` / `CZ_VK_ONLY_TEX` on the three
      diffuse addresses (`0DC01000`, `0DC31000`, and the per-area third), which is what
      turns "that polygon is white" into "that polygon is texture X".
   4. The mirror, register, door and gas-station sign are indoors or off the DebugJump
      route, so they need the operator once the outdoor ones are understood. **The
      slot-machine frame was lost to the overwriting bug and is worth one press.**

00g. **THE ROUND-2 CAPTURES, AND WHAT THEY SETTLED.** Seven single-frame F4 traces, one
   per defective surface, delivered 2026-08-10 with a screenshot each and the session's
   whole `dump_shaders` output. `Xenia logs/R2_world/`, notes in
   `R2_WORLD_CAPTURE_NOTES.md`, read with `tools/xtr_draw_bindings.py`.

   **A single-frame trace is self-contained** — an `EdramSnapshot`, then a `MemoryRead`
   carrying the ACTUAL SAMPLED BYTES for every texture, vertex and index buffer the frame
   touched. It replays standalone, so a texture can be reconstructed without seeking a
   long stream, and each file pairs unambiguously with the place it was taken. That is a
   better artifact than the continuous stream this project asked for, and it is what the
   next capture round should ask for.

   **1. OUR SHADER COVERAGE IS COMPLETE.** All **357** distinct shaders in the capture are
   already in our cache of 410 — zero gaps across all seven locations. (They read as 357
   NEW at first: Xenia writes `.ucode.bin` DWORD-SWAPPED relative to the guest's
   big-endian bytes. The hash self-test — 410 of 410 of our own dumps reproduce their own
   filenames — is what proved the function right and sent the search to the data.)

   **2. THE WHITE GROUND MATCHES HARDWARE ON EVERY INPUT.** The same draw is identifiable
   across both stacks by shader hash and vertex count:

       HARDWARE  draw 1003  verts=25234  vs_36eef2c94b4a065c  ps_ad65b98593f95926
       OURS      draw  825  verts=25234  vs 36eef2c94b4a065c  ps ad65b98593f95926

   Same bindings (two 512x512 DXT1, one 128x128 DXT1, the 4096x1024 shadow cascade twice;
   hardware also holds constants in slots 4/6/7 that the shader's sidecar never declares).
   The albedo hardware actually read was extracted — 131,072 bytes, exactly a 512x512
   DXT1 — and decodes to a detailed ground atlas (mean 91, stdev 41, 1,340 distinct
   colours). **And the render state matches**: `RB_BLENDCONTROL0 00010001`,
   `RB_COLORCONTROL 00018004`, `RB_COLOR_MASK F`, `RB_MODECONTROL 4`, `RB_COLOR_INFO 0`,
   and a depth control whose stencil enable is clear.
   **Same shader, same textures, same contents, same state, different picture.** The
   defect is in the SHADING or in the VERTEX DATA feeding it — the only input not yet
   compared, and the one with an anomaly already recorded (two texcoord attributes at
   different dword offsets decoding identically). The trace carries hardware's vertex
   buffers, so that comparison is available and is the next step.

   **3. OUR CUBE DECLINES FIRE ON A CONDITION HARDWARE NEVER SHOWS.** For every draw in
   the gas-station frame where one of our 95 cube-declaring shaders is bound, the guest's
   fetch constant reads **stack depth 5 (six faces) and dimension 3 (cube), on 414 of 414
   draws — no disagreements at all.** Our runtime serves the white dummy to ~14,670 cube
   fetches a run precisely BECAUSE the shader and the constant disagree, so we are
   generating that disagreement ourselves: either our register file has lost a constant
   the guest set, or our dimension decode misreads a case this frame does not contain.
   **This is the mechanism behind the white glass and the blown-out bathroom window**,
   both confirmed dummy-samplers by the magenta test — and it means the fix is upstream of
   the decline rather than in what the decline chooses. Hardware also binds real, square
   environment maps in those slots throughout: 32x32 and 128x128 DXT1, 64x64 and 4x4 8888.

   **PART 27: THE CONCLUSION SURVIVES, THE MEASUREMENT BEHIND IT DID NOT, AND THE
   MAGNITUDE WAS OFF BY 10x.** Three corrections, all measured:

   * **"414 of 414" could not have found a disagreement.** It selected draws where a
     cube-declaring shader was bound and then counted the constants that ALREADY read
     cube; a disagreeing slot reads 2D and was outside the population by construction
     (gotcha 264). `tools/xtr_cube_agreement.py` asks it the way the runtime asks it —
     per fetch slot the shader's own sidecar declares — and gets **0 of 13,203 fetches
     disagreeing** on the gas-station frame. Same conclusion, now falsifiable.
   * **The disagreement IS ours, and now there is a positive identification.** The
     runtime's `CZ_VK_DIM_DISAGREE` census enumerates the whole population of a 400 s
     outdoor run: **9 distinct (shader, slot, texture) cases over two textures** —
     `01330000` (4x4 `k_8_8_8_8`) at slot 1 or 3, and `10C38000` (128x128 DXT1) at slot 4.
     The slot-4 cases carry `vs_2f13eecec64e508e` with `ps_3da9454d30a3a225`,
     `ps_ad2d8362c47d9e45` or `ps_71d569a46634d72a` — **and those exact shader pairs
     appear in the captures with a real cube constant at slot 4** (`0E751000`, 128x128
     DXT1, dimension 3, stack depth 6). So hardware has a cube there and we do not.
   * **What we have there instead is an exact duplicate of slot 3.** The full 32-slot
     dump at one of those draws reads `s3 10C38000 128x128 dim=1` and `s4 10C38000
     128x128 dim=1` — the same descriptor twice, where hardware's s3 and s4 are two
     different textures. That is the 00f "binds the same texture twice" observation seen
     from the register file. It is **not** a decode error: the same decode reads
     `s6 06805000 64x64 dim=3 depth=6` correctly in the same dump.
   * **The share is 0.05%, not 0.28%, and the decline had a second cause nobody had
     named.** On the outdoor route, `cube fetch got the dummy` is **3,210 of 1,903,592
     cube fetches (0.17%)** and now splits exactly: **2,182 because the fetch constant at
     that slot is NOT A TEXTURE at all** (type != 2 — the guest never set it) and **1,028
     for the dimension disagreement**. 2,182 + 1,028 = 3,210, so nothing is unattributed.
     Part 26's 14,670 was a pre-cube-snapshot binary and should not be re-quoted.
     **The larger cause is the unset slot, and it was invisible because the "not a
     texture" counter was shared with every 2D fetch.**

   **What is still owed here** is why the guest binds slot 3's texture (or nothing) into a
   slot hardware gives a cube map. Both remaining candidates are guest-side rather than
   renderer-side: the environment map that material wants was never created in our
   runtime, or it was created and the engine's own bind was skipped. `01330000` is the
   4x4 already on file as "uploaded BLACK, guest memory NON-ZERO NOW" (item 00 point 2),
   so the two are plausibly one defect in this title's texture creation.

00h. **THE ASSET EXTRACTION IS COMPLETE, AND THE 304 NOT-FOUNDS ARE THE TITLE PROBING FOR
   CONTENT THIS PACKAGE NEVER SHIPPED.** Asked in part 27 — could a missing `.big` be why
   surfaces are white?

   * **Extraction: 256 of 256 files, zero missing, zero wrong size, all 146 `.big`
     archives present, 816.5 MB.** (The first run of this check reported "234 missing" and
     was MY bug — it keyed the disk side on basenames and the listing side on full paths.
     The tell was that it also reported zero wrong sizes, and 234 missing files with no
     size mismatches is not a plausible extraction failure.)
   * **Zero case mismatches.** Not one requested path exists on disk under a different
     case, so our VFS is not failing a case-sensitive lookup — the plausible port defect.
   * **The misses are probe-then-fallback**, which is what a cut-down Dead Rising 2 looks
     like: `data/anim/weapon` misses 66 files and has exactly one on disk
     (`allweapons.big`); `data/audio` misses 60 `fx_*.big` and ships the combined
     `streamfx.big`.
   * **And it cannot be a difference from hardware anyway: Xenia launches the SAME STFS
     package**, so the emulator sees the identical 256 files and misses the identical
     paths. A file absent from the package is absent on both sides.

   **THE COUNTER THAT MAKES THE ONE REAL MISS VISIBLE** is now in
   `runtime/kernel/file_imports.cpp`. 304 expected misses drown any genuine one, so each
   distinct path is classified ONCE and cached:

   | class | meaning | on a no-input outdoor run |
   |---|---|---|
   | PROBE | the parent directory is empty or absent — content never shipped | 300 |
   | SIBLING | the parent EXISTS AND HOLDS FILES, so a missed extraction would land here | **4** |
   | REGRESSED | we opened this exact path earlier IN THIS RUN and now cannot | **0** |

   SIBLING and REGRESSED print immediately rather than at exit, because most runs of this
   title are killed by `timeout` and an exit-time report is a report nobody receives. The
   four survivors are `cl.txt`, `capcom.txt`, `serial.bin` (dev files at populated paths)
   and `fx_cicadas.big`. **304 -> 4, and zero regressions: the file layer is clean.**

   **THE TWO GRADING TEXTURES, and they are not ours.** `data/misc/textures/cc_03.bct` and
   `sun.bct` are both colour-grading assets, which is why they were worth chasing:

   * `cc_%02d.bct` is built at RUNTIME (`va 8208A1D0`) — numbered **colour-correction
     LUTs**, and `cc_03` is index 3. The package ships **13 `.bct` files and not one
     `cc_*`**.
   * `sun.bct` (`va 82088A5F`) sits inside the post-FX parameter table, between
     `contrast_midpoint`/`contrast`/`saturation` and `vignette_alpha/tint/radius/power`.

   So the title asks for numbered grading LUTs and a sun texture, as LOOSE files, and does
   not get them — **on Xenia exactly as here**. Relevant to item 6 (colour grading) as a
   fact about the title rather than a defect in the port. **Whether they also live inside
   a `.big` is UNKNOWN and the obvious grep cannot answer it**: searching the archives for
   `cc_0` returns zero, and so does searching them for `meat`, `zombie` and `chuck` —
   names that are certainly in there. The archives do not store plain text, so that search
   could not have matched and its negative result means nothing (gotcha 25). Answering it
   needs `docs/big-archive-format.md` and a real TOC reader.

00i. **PART 45: THE MENU HALF IS SOLVED, AND IT WAS NEVER A TEXTURE-LEVEL OR
   MIP QUESTION — our own synth tool dropped interpolants after PARTIAL
   register writes, so 217 of 333 pixel shaders sampled their diffuse (and
   often two more textures) at ONE TEXEL.** The fourth-addendum menu-lab plan
   ran to completion: the GAS ball draw was named by draw-ID (not inference),
   all four texture slots came back byte-identical to B1's memory records,
   the UVs identical on every sampled vertex, every recoverable constant
   equal, the dummies refuted by poison — and the generated HLSL then showed
   `r0 = 0.0` where the microcode's `tfetch2D r0.__xy` had only written .zw
   and the diffuse is fetched at r0.xy. Fixed in
   `tools/synth_shader_container.py` (per-component liveness, commit
   fdda6f3); the menu ball is RED on the new cache and the E3 correlation
   gate flips +0.687→+0.710 (fail→pass). Full record: `phase5-notes.md`
   §6by; gotcha 316. The old cache is `assets/shader_spv_pre45`
   (CZ_SHADER_SPV selects it — the same-binary control arm).
   **AND THE OUTDOOR HALF WENT THE SAME WAY — the operator's own A/B closes
   it, same evening**: two launches of one binary differing only by
   `CZ_SHADER_SPV`, their own route. Fixed cache: *"the game looks way better
   now, the building doesn't seem to have issues… almost like OG game."*
   Pre-45 cache: *"way worse — gas station looks bad and building look FLAT
   DEPENDING ON DISTANCE."* That is this item's original complaint, switched
   on and off by the shader cache. **The flat-at-range class was
   substantially the liveness defect** (§6by addendum 2).
   **What remains open under this number is only the MIP OVERSHOOT
   SIGNATURE**, which still reproduces on the clean bank but no longer has a
   symptom the operator can see. Before any further work on it, re-run part
   44's decisive arm — `CZ_VK_NO_MIPS=1` at a matched view — on the FIXED
   cache: it was measured through the broken shaders, and if it no longer
   restores anything then the overshoot has no picture-level support at all.
   Historical head below, kept for the elimination it records.

   (superseded by the part-45 head) 00i. **REOPENED THE SAME DAY — the operator's matched capture refutes the
   closure's attribution: the flat Big Buck buildings bind FULL-SIZE textures
   (8 tiny-on-big draws of 1,760 in their capture 4), so the flat look at
   range is NOT the thumbnail class at all, on either platform.** The B2 rate
   argument below could never place hardware's tiny binds on building-scale
   surfaces (rate is not prominence — the same error class as gotcha 248's
   semantic detector), and the visible flatness survives with normal-sized
   textures bound. New mechanism under test (operator A/B in flight):
   MIP SAMPLING AT RANGE — distant surfaces sample the deepest levels, which
   since part 41 come from the packed-mip-tail decode ("302 mostly-empty tail
   levels" upload per run); a wrong/empty tail level flattens a building to
   one color exactly at distance while it looks fine up close — the
   operator's original wording of this item. Arms: CZ_VK_NO_MIP_TAIL=1, then
   CZ_VK_NO_MIPS=1, F9 at the matched view. The CLOSED-AS-FAITHFUL record
   below stands only for what it measured (bind-size distributions; the menu
   retraction is untouched and correct).

   (superseded same-day) 00i. **CLOSED IN PART 44 — FAITHFUL. Fresh hardware binds the same thumbnail
   class at the same rate; the "hardware is full everywhere" oracle was a
   WARM LOADED-SAVE session.** Two censuses settled it (`docs/phase5-notes.md`
   §6bx): (1) the MENU: our F9 census equals B1's title era bind for bind —
   including the 31 draws of an 8×8 on big meshes, which is
   `flat_color_gray_cm.bct`, 346 bytes on disc, flat by design; part 43's
   final "the defect is the set-apply" reframe is RETRACTED (its control was
   never run). (2) THE TOWN: B2 — a FRESH hardware session walking into Still
   Creek, on disk since day one — runs a steady **2–3% tiny-on-big through
   the whole town era, 1,300–3,900 world-shader draws per bucket on an 8×8**,
   the exact class the operator reported on ours (our frames: 0.2–4.9%), and
   the tiny textures PERSIST to the end of the session — no promotion wave
   exists on a fresh session. The R4 traces' zero-tiny street is the loaded
   Case 0-2 save's carried state (mechanism unproven, save-side; the
   only remaining follow-up would be loading the operator's tanker save on our
   runtime with `CZ_SET_APPLY_PROBE=1`, purely to name the carrier). The
   flat-at-range look on a fresh session is the engine's own design: the far
   zones' `COMMON_TEXTURE_LOD.tex` thumbnail sets, applied through a texture
   level machine that part 44 named end to end and verified healthy on our
   runtime (39 walk-scheduled payload ops → their archive reads; shared
   atlases promoted live when a full zone references them). Comparisons for
   the operator must be like-for-like: fresh session vs fresh session, or
   same save vs same save.

   The part-43 record, kept because its reverse-engineering stands:

   (superseded head) 00i. **LOD POPS IN FAR TOO LATE — PART 43: OUR SIDE IS EXONERATED UP TO ITS
   INPUTS; the fork now waits on capture R5 (one fresh-spawn F4).** The decision
   is fully named (`docs/phase5-notes.md` §6bw): `sub_82270870` picks
   `COMMON_TEXTURE_LOD.tex` iff the zone is LOD-capable (flag at rec+0x90C,
   written at setup from the two files' sizes) AND every volume in the zone's
   list (rec+0x910, 0xD0-stride spheres + thresholds) is farther than its
   threshold from the camera `[g+0x40]`, thresholds boosted by per-level tables
   (level 14 = no boost). `CZ_ZONE_TEX_PROBE=1` prints every input at decision
   time and predicted part 42's narration line for line; `tools/zone_lod_live.py`
   re-evaluates it on a live process. Measured: the decision runs ONCE per zone
   load, at ~46 s, with the camera ALREADY at the spawn; zones 1/2/3/7 are
   all-far by 31-107 m; the LOD file IS the thumbnail set by design (27 KB vs
   1.3 MB); no promotion trigger found statically, and no roam has yet crossed
   a LOD threshold to test it live (the EXPLORER pocket is ~60 m). The R4
   traces are a WARM-session sweep at Big Buck and cannot adjudicate a fresh
   state. **Do not build any fix before R5 lands** — forcing full sets, faking
   skip bits or widening thresholds would all be faking the decision (gotcha 5).

   The part-42 record, kept because its measurements stand:

   (superseded head) 00i. **LOD POPS IN FAR TOO LATE — operator report, OPEN, and the first pass found no
   defect but built the instrument that can.** *"I have to be really close to an object
   to show their near LOD."* Part 28, ~1 hour.

   **PART 42: THE MECHANISM IS CORNERED — A PROMOTION-DENIAL DECISION, NOT A RATE.**
   Four measurements (`docs/phase5-notes.md` §6bv):
   * The complaint verified PRE-POST-CHAIN: the 003053 building's walls are
     patternless in the scene surface itself, so this is not the DoF composite.
   * The flat class is a handful of SHARED WORLD ATLASES stuck at thumbnail
     quality: 53 big draws (up to 3,575 verts) in one frame from just TWO
     addresses (8×8 `0ED9A000`, `11DB9000`), shader `ps_34524bb64374d20e`.
   * Two-sided at the shader level: hardware binds a ≤16×16 s0 on a ≥200-vert
     draw of that shader **0 times across all eight R4 traces**; our side does
     it in 44 of 82 walk frames (912 draws) and 14 of 20 part-41 frames (516).
   * **Stand-still, fixed camera: the same three tiny-bound textures never
     promote across 13 censuses / 2.3 minutes.** A rate defect (IO latency, the
     `KeSetBasePriorityThread` no-op) would fill in while stationary —
     **REFUTED as this item's mechanism, do not build it for this item.** The
     defect is a distance/screen-size THRESHOLD or a POOL BUDGET whose input
     differs on our runtime. Next: the engine's own narration
     (`CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` at the stand-still spot — first run's
     log at `/tmp/part42_diag.log`, part 42), then `gdis` on the
     `cLODController` / `wait_for_tex_lod` / `ForceLODTexForStreamingWorld`
     sites to name the wanted-level function and its inputs.

   **PART 39 DID THE CONTENT PAIRING AND IT EXONERATED THE LEVEL-0 INPUT.** The Big Buck
   shopfront's 153-vertex sign draw pairs across platforms on shader and on all four
   texture extents, and the bytes the two samplers read are **md5-identical**
   (`b06f8bdd…`, hardware `15689000` vs our `0E04D000` — different boot, different
   address, same content). Two of the kickoff's three candidates died with it: the white
   dummy is bound **once** in most of the eight F9 frames (not a building-scale path),
   and `mip_min_level` is **0 on all 328,164 hardware fetches**, so the streaming system
   is not raising an LOD clamp we ignore.

   **WHAT THE SAME CENSUS FOUND IS A WHOLE DECLARED INPUT WE DISCARD: THE MIP CHAIN.**
   `mip_max_level` runs to **9**, and **88,689 of 328,164 fetches across all eight R4
   traces (27.0%) carry a separate mip-chain address** (dword5); our own outdoor frame
   declares a chain on **69.6%** of fetches. `DecodeTextureFetch` had parsed the fields since phase 5 and
   nothing read them; `CreateImage` hardcoded `mipLevels = 1`. Part 39 implements the
   chain — layout verified level by level against hardware's own bytes, packed tail
   declined and counted — with **`CZ_VK_NO_MIPS=1` as the same-binary control arm**.
   1,815 textures take a chain on the outdoor route.

   **THE A/B IS DONE — TWICE — AND IT DOES NOT CLOSE THIS ITEM.** The first block
   (three runs an arm) read mean luma −1.35% against a 0.65% floor and looked RESOLVED,
   moving toward hardware's darker frames. It was measuring a BUG: the divergence guard
   shipped with the feature found **254 of 1,818 chained textures holding a level that is
   not that texture**, and re-running arm A with those rejected took mean luma to
   **+0.36% (unresolved)** — sign flipped, magnitude gone. Distinct colours −5.45%
   against a 7.91% within-arm spread, also unresolved. **So the mip chain, as
   implemented, produces no era-statistic change resolvable at three runs an arm**, it is
   justified on correctness alone (the guest declared data we discarded), and it does not
   demonstrate anything about this item. Gotchas 298 (the registered prediction had the
   wrong sign — filtering REDUCES distinct colours) and 301 (a bug that moves the metric
   toward the oracle is the most dangerous shape a measurement takes).
   `docs/phase5-notes.md` §6bq.

   **STILL OWED HERE:** the packed-mip tail (levels below one tile share a tile at
   sub-tile offsets), which is where the *deepest* minification lives and therefore
   where a far building most needs its level. If the A/B moves at all, finishing the
   tail is the obvious follow-on and the oracle method is already worked out — decode
   hardware's chain out of the trace and check the mean holds while distinct colours
   fall.

   **PART 41 PAID THAT DEBT: the tail is DECODED (409777d, §6bt).** Square DXT tail
   levels down to 4x4 texels upload now at offsets brute-forced from 7,466/7,515
   hardware votes (`tools/packed_mip_derive.py` over all eight R4 traces — a level of
   width W blocks sits at block (W,0) in the shared tile). `mip: packed tail level
   TAKEN` = 1,877 on the boot route; `CZ_VK_NO_MIP_TAIL=1` is the tail-only arm. In
   the same part, per-fetch samplers landed (d5b8fdc): the world's albedo now filters
   at the 4:1/8:1 aniso the fetch constants ask for, which is the OTHER half of
   "distance is mush". Whether the pair closes this item's flat-panel look is the
   running era-median A/B plus an operator look — not yet a claim.

   **PART 38: THE OWED ONE-LOOK IS ANSWERED AND THE DEFECT IS OURS — this is now the
   TOP PICTURE ITEM.** The operator captured the Big Buck approach on our side (9 F9s,
   flat-color building panels at range snapping to full texture only up close, and
   "almost everything in the game behaves like this"), then delivered
   `Xenia logs/R4_world/` the same night: EIGHT frame-locked single-frame traces of
   the same approach on hardware. **Hardware shows fully textured buildings at every
   distance** — the HARDWARE sign is legible from far down the street. So the flat
   look at range is not the game's streaming policy; it is our runtime, with eight
   paired oracles (each .xtr carries the textures hardware sampled for the far
   buildings). Note the part-28 analysis below predates the part-37/38 fixes (the
   lightmap-UV transposition and the stale texture cache BOTH corrupted this exact
   evidence class), so its "no streaming failure" reading stands but its visual
   comparisons should not be re-quoted. The next move: pair one far building's draw
   between our F9 census (`part38-operator/arm1b_revalidate/`, frames 9965-10986) and
   the R4 trace for the same viewpoint, by texture CONTENT (gotcha 291), and see what
   hardware binds where we draw flat color — a missing texture level, an unset fetch
   slot, or a constant.

   **What the engine's own vocabulary says LOD IS here.** Every `lod` string in the image
   (54 of them, the complete set) is about STREAMING, not about a distance curve: a
   `cLODController` array parsed per zone beside the occluders and model instances,
   `ForceLODTexForStreamingWorld`, `WAITING: cLevel - wait_for_tex_lod`, a `UseLOD` bool in
   the PROP schema (next to `Unmoveable`, `Durability`, `MergedFilename`), and a per-zone
   choice between `COMMON_TEXTURE.tex` and `COMMON_TEXTURE_LOD.tex`. There is no
   LOD-distance scalar named anywhere in the executable. **So this is a question about the
   streaming system's promotion rate, and it should not be chased as a distance
   comparison.** 989 `*_LOD.tex` entries ship across the archives.

   **Measured, with `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` on the DebugJump outdoor route:**
   * The per-zone LOD decision RUNS and is mixed, which is what working looks like — of 7
     zones loaded, **3 took `COMMON_TEXTURE.tex` and 4 took `COMMON_TEXTURE_LOD.tex`**.
   * **No streaming failure of any kind fired.** No `Queue is full in MoveLoadRequest()`,
     no `Out of memory in the load & decomp heap!`, and neither `cZone::UpdatePriorities()`
     assert (`mForceLowLOD`, `mNumVolumes`). The run's errors were unrelated
     (`[cNavMesh::LoadFrom] Failed to load nm_prologue_menu.txt`).
   * Heap headroom is healthy and shrinks smoothly: the zombie vertex-buffer heap runs
     5,742,568 -> 2,221,880 largest-free in ~330 KB steps with no floor hit.
   * The title asks for **447 MB up front and gets it** (`[heap] big
     MmAllocatePhysicalMemoryEx: 447 MB -> A0000000`), so this is not a starved allocator.
   * The per-level streaming vertex-buffer heap is a fixed 24-entry table at `0x82042EB8`,
     **25-65 MB by level id** — guest data, identical on hardware.

   **So nothing here is yet shown to be OURS, and the honest state is that the mechanism is
   unlocated.** The one candidate that WOULD be ours and has not been tested:
   `KeSetBasePriorityThread` is a no-op and `KeQueryBasePriorityThread` returns
   THREAD_PRIORITY_NORMAL for every thread (`runtime/kernel/imports.cpp:1463`). The title
   creates ~47 threads including a `DecompressThreadLoop`; on the 360 those carry explicit
   priorities and hardware-thread affinity. A decompression thread scheduled level with the
   render thread promotes late, which is *exactly* "it loads, but only when I am close".
   Cheap test: honour the priority as a host nice level or a thread-pool weighting, and
   read the same `Largest free ... delta` and `[LOAD] took` lines either side.

   **THE ORACLE WAS ASKED, AND THE ANSWER IS THAT THE QUESTION IS NOT YET ANSWERABLE —
   THIS ITEM IS PARKED, NOT REFUTED.** The operator checked the same distance in Xenia and
   reported that it does not obviously change there, **but that hardware's LOD transitions
   are far less VISIBLE because hardware's textures are not broken.** That is the correct
   reading of their own observation and it is not a null result: an LOD swap is seen as a
   change in surface detail, so on our arm — where item 00f's flat `rgb(180,180,180)`
   plateaus already destroy surface detail on world geometry — a swap that hardware makes
   invisibly is loud, and one that hardware makes visibly is indistinguishable from the
   plateau. **The two arms are not comparable on this axis** (the A/B admissibility rule in
   CLAUDE.md, arrived at from the picture side instead of the draw-set side). Recorded
   2026-08-10 so nobody re-runs the comparison expecting it to decide anything.

   **UNBLOCK CONDITION, stated so this is a dependency and not a shrug: 00f/00g first.**
   Re-ask this only once world surfaces render their real materials. Then it is one
   screenshot pair, and the two live answers are still (a) faithful — DR2-family titles are
   aggressively streamed on real hardware — or (b) our streaming promotes late, whose only
   named candidate is the `KeSetBasePriorityThread` no-op above. **Do not build the thread
   priority work before then**: it would be a fix aimed at a defect nobody has yet shown
   exists, measured against a picture that cannot report whether it worked.

   **PART 35: THE UNBLOCK CONDITION IS MET AND THE PICTURE IS CAPTURED.** On the
   part-34 renderer the pop is cleanly visible and photographed: the same shop as flat
   colour panels at street-across distance and full siding closer
   (`part35-item1-operator/reload_test`, captures 30631/30807). The far state is FLAT
   COLOUR (single-repeated-block uploads in the texture census — possibly the game's
   own placeholder), not broken texture. **What remains is exactly one Xenia look**:
   walk the same street and say whether hardware still shows flat panels at that
   distance. Same distance -> faithful, close 00i; textured earlier -> our streaming
   promotes late and the `KeSetBasePriorityThread` no-op is the first candidate.

00b. **THE TEXTURE CACHE IS NOT THE WRONG-TEXTURE MECHANISM — MEASURED AND RETIRED.**
   Part 23's opening hypothesis was that the cache, keyed on the fetch constant's six
   dwords (a DESCRIPTOR) and never invalidated, serves a previous occupant's image when
   this title's texture streaming reuses a heap address. It was built into an instrument
   (`CZ_VK_TEX_GUARD`, a content guard over the guest bytes, with `CZ_VK_TEX_REVALIDATE`
   as the repair and `CZ_VK_TEX_GUARD_POISON` as the control) and **refuted**:

   | run | cache hits checked | served a stale image |
   |---|---|---|
   | poisoned control | 14,554,550 | 14,554,550 — **100.00%** |
   | boot, no poison | 14,584,635 | 0 — **0.00%** |
   | headless outdoor, 620 s | 144,560,672 | 118,757 — **0.08%**, 10 addresses |
   | **OPERATOR SESSION, wrong textures on screen throughout** | **139,775,032** | **1,968 — 0.00%** |

   The operator's own log is the decisive row: the wrong floor atlas, the iridescent
   cabinet and the red dumpster were all on screen while the counter sat at zero, and
   `CZ_VK_TEX_REVALIDATE=1` repaired all 1,968 stale hits without changing any of them.
   (A "blank white wall" was listed here too and is REMOVED: the operator says that wall
   is a normal texture rendering correctly. It was my reading of their screenshot, not
   their report — see item 00's note on inferring defects from someone else's picture.) **Address recycling is real and utterly marginal.**
   The instrument is kept: it is two-sided (100% poisoned, 0.00% on a boot), it is the
   only thing that can see a stale texture at all, and it retired a plausible theory in
   one run instead of a session. But do not reach for it again for wrong textures.
   NB it is blind by construction to a fetch pointing at an address that was never that
   texture's home — it only compares bytes at an address we already uploaded from.

00c. ~~**THE UI / AMMO-COUNTER DEFECT**~~ **CLOSED (part 24). The cross-frame stream
   store's guard was sampling; the exact bound is now 16 KB and the HUD is correct in a
   crowd at zero frame-rate cost.**

   **The closing measurement, operator, gas-station crowd:** at **6,778 draws/frame** the
   frame is still pinned at **32.2 ms / 31.0 fps** — the title's two-vblank floor — with
   `guard read` at 14.15 MB/frame and `record` at 19.3%. `outside` is 54.2%, so there is
   still headroom. The store is simultaneously avoiding **50-61 MB/frame of copying**, so
   the guard spends 13-14 MB of hashing to save 50-61 MB of memcpy. HUD confirmed correct
   throughout, with the ammo counter tracking.

   The cost is real in CPU terms and invisible in frame rate, which is gotcha 237 working
   in our favour for once: a CPU saving converts to frame rate only above the vblank floor,
   and so does a CPU COST. Do not re-quote 19.3% of `record` as a regression without
   showing a frame above the floor.

   **Residual exposure, deliberately left and counted:** 604-624 streams/frame still exceed
   16 KB and are only sampled, so a small edit inside one of those is still invisible. The
   profile line reports that count on every window. If a similar defect ever reappears,
   raise `CZ_VK_STREAM_GUARD_BYTES` first — it needs no rebuild — and only then go looking.

   The full history is below, kept because three sessions looked at this and the reasons it
   survived them are more useful than the fix.

00c-history. **CAUSE AND MECHANISM. The cross-frame
   stream store's GUARD was sampling, and a sampled guard cannot see a small edit inside a
   large UI buffer. `CZ_VK_STREAM_GUARD_EXACT=1` fixes it outright; what is left is making
   that affordable.**

   **The confirming run (operator, part 24):** store ON, `CZ_VK_STREAM_GUARD_EXACT=1`, over
   several minutes with both a pistol and an assault rifle. HUD intact throughout — LV/PP,
   LIFE pips, `$2,000`, `ZOMBREX 0`, `52 KILLED`, `Case 0-2 - Find Zombrex` — and the ammo
   counter tracked the real value (21) instead of flickering 26<->27. 31.0 fps, the title's
   own pacing floor.

   So the chain is settled end to end: the store serves a cached buffer when the guard says
   the guest bytes are unchanged; `StreamGuard` is exact only to 512 bytes and hashes 8
   blocks of 64 above that; a HUD is batched into one multi-KB vertex buffer in which only
   the digit quads change; those quads fall outside the sampled windows; the guard reports
   "unchanged" and the draw gets the previous frame's numbers. It is independent of
   `CZ_VK_FRAMES_IN_FLIGHT` (the ping-pong is off at 1) and invisible to the census
   (`GUARD MISSED: 0 of 0` — a zero DENOMINATOR, blind rather than negative), which is
   exactly why it survived three sessions of looking at it.

   **The 512-byte bound was the whole defect, and note what justified it**: the census
   found every rewritten stream was exactly 80 bytes, so the bound looked generous. That
   census could only see streams rewritten between two consecutive frames in a recipe that
   never changed a HUD number — it measured the population it could reach and the bound was
   fitted to it. Gotcha 235 is the same shape and this is a second instance.

   **WHAT IS LEFT — cost.** Exact hashing is unbounded in stream size, and the guard runs on
   EVERY stream every frame because it is how a hit is decided. A crowd frame moves 61-77 MB
   of stream bytes, so exact could mean hashing all of it rather than ~1 MB. The operator's
   confirming run was ordinary gameplay, not a crowd, so it does not bound this. Measure
   `CZ_VK_STREAM_GUARD_EXACT=1` against the default on the outdoor-crowd recipe with
   `CZ_VK_PROFILE` before choosing between: (a) ship exact unconditionally, (b) raise the
   exact bound to cover the UI population with margin and keep sampling above it, or
   (c) build the real invalidation (the `mprotect` design in phase5-notes §6av, written up
   and never built). Whatever is chosen, the guard cost is charged to `record`, not
   `streams` (gotcha 238) — re-baseline before attributing anything.

   ~~THE UI / AMMO-COUNTER DEFECT — the store is confirmed as the cause; what remains is the
   mechanism.~~ The operator reports the HUD
   intermittently collapsing (text overlapping at the top-left, the ammo count absent) and
   the pistol ammo flickering between **26 and 27 every frame regardless of the real
   ammo**, triggered by firing a shot.
   * It happens at `CZ_VK_FRAMES_IN_FLIGHT=1`, so part 23's ping-pong is **not** the
     cause — that path is disabled at 1.
   * One run with `CZ_VK_NO_PERSIST_STREAMS=1` had an intact HUD and a plausible ammo
     count, which POINTS at part 22's cross-frame stream store. **This is one sample per
     arm against a defect the operator describes as intermittent, so it is a lead and not
     a result** — do not write it up as one, which part 23 briefly did.
   * **Every headless instrument came back blind, not negative.** `CZ_VK_STREAM_CENSUS=2`
     over 620 s at both settings reports `GUARD MISSED: 0 of 0` and
     `cross-frame CONTENT UNCHANGED: 1,483,804 of 1,483,804 repeated keys (100.0%)`, with
     the only rewritten streams being **30 distinct keys, all exactly 80 bytes** — below
     the guard's 512-byte exact bound, so the guard is exact for everything the recipe can
     see. The recipe walks and looks; it never shoots and never changes a HUD number.
   * **A sampled-guard miss was argued to be unlikely** on the grounds that a two-digit
     counter is a few hundred bytes, under the 512-byte exact bound. That argument assumes
     the digits live in a stream of their OWN. If instead they are quads inside one larger
     UI vertex buffer of a few KB — which is how a HUD is usually batched — the guard
     samples 8x64 bytes of it and can miss them entirely. **Both readings are live and
     `CZ_VK_STREAM_GUARD_EXACT=1` separates them in one run**, so do not spend argument on
     it: if the exact guard is clean the sampling was the mechanism, and if it still breaks
     the store is guilty by some other route and the 80-byte census reading was right.
   * The candidate that survives: the guest DOUBLE-BUFFERS its UI vertex data across two
     addresses, one entry goes stale, and the display alternates between the true value
     and a stale one every frame. Untested.
   * **SETTLED AS FAR AS THE STORE: the operator fired in the store-off arm and the run
     stayed clean end to end.** That was the one hole in the earlier A/B — the table was
     retracted because arm 1 might simply never have fired. It did. So both arms had
     firing, one variable separated them, and the cross-frame stream store (part 22, our
     own change) is the cause. Still one sample per arm, but it is now an A/B rather than
     a coincidence.
   * **THE MECHANISM IS ALMOST CERTAINLY THE GUARD'S SAMPLING, AND THAT IS THE NEXT AND
     CHEAPEST TEST.** `StreamGuard` is exact only to 512 bytes; above that it hashes 8
     blocks of 64. A UI vertex buffer of a few KB in which only the digit quads change can
     therefore hash IDENTICAL, so the store calls it unchanged and serves the previous
     frame's buffer — which is exactly "26 and 27 regardless of the real ammo". It is
     independent of `CZ_VK_FRAMES_IN_FLIGHT` because the ping-pong is disabled at 1, which
     is what killed the earlier ping-pong explanation. And the census is blind to it by
     construction, which is why `GUARD MISSED` reads `0 of 0` rather than `0 of N`.
     **`CZ_VK_STREAM_GUARD_EXACT=1` (part 24) is the discriminator**: store ON, guard
     hashes every byte. Operator run, fire several rounds, play a few minutes.
       - clean -> the SAMPLING is the bug, not the store. The fix is a better guard for
         UI-sized streams and part 22's 4.7 ms of a crowd frame stays bought.
       - still breaks -> the guard is not the mechanism; look at eviction
         (`staleEvicted`), key collision, or the `alt` twin instead.
   * **A HEADLESS METRIC THAT DOES NOT WORK, recorded so it is not rebuilt.** Part 24 tried
     to make this self-servable by counting frames where the LIFE pips / PP bar / LV circle
     are absent. It reproduces beautifully (69.0% and 69.4% across two runs of the same
     config) and it is measuring the WRONG THING: `phase5-notes.md:2152` already records
     that partial HUD is context-dependent — the safehouse has not raised it yet — so the
     metric tracks where Chuck is standing. Its three-arm result (store off scoring WORSE
     than the control) is therefore inadmissible and is retracted. The machinery is fine
     and reusable; the region was the mistake. A valid headless metric must watch a HUD
     NUMBER CHANGE, not a widget's presence.
   * **The synthetic-input arm cannot fire a weapon, and that is why no headless recipe
     ever has.** `CZ_FAKE_PRESS_SEQ`'s vocabulary is A/B/X/Y/START/BACK/D-pad/NONE plus the
     four sticks — there is no trigger, and attack in this title is RT
     (`COMMAND_PLAYER_QUICK_ATTACK` / `HEAVY_ATTACK`). **Adding the button was considered
     and deliberately dropped**: it only solves half the problem, because a recipe would
     still have to ACQUIRE a gun and ammo along a long scripted path through the world.
     The operator does weapon tests directly. Anyone re-proposing this should solve the
     acquisition first — the button is the easy half and it buys nothing on its own.
     Never flickers there but reliably does with the store on -> the store is guilty.
     Flickers in both -> the store is innocent and "two fixed values unrelated to the real
     ammo" points upstream of the renderer entirely, at the guest's own HUD state.
   * A headless recipe that FIRES A WEAPON and watches a HUD number would make this
     self-servable; every existing recipe is blind to it by construction (gotcha 190).

0a. ~~**A CROSS-FRAME STREAM CACHE**~~ **BUILT IN PART 22 (§6av).** 97-99% of first-touch
   streams are now served across the frame boundary, copied bytes fall from 61-66 MB/frame
   to **0.23**, and `streams` goes from 11.1% of a crowd frame to **0.0%** for a guard cost
   of ~0.8 MB/frame. The draw path at ~6,000 draws is 9.2 ms against the store-off arm's
   13.9 — **4.7 ms, a third of it.**
   **In frame rate that is 44 ms -> 32 ms at ~3,700 draws and approximately nothing at
   ~6,500**, because a CPU saving on this title converts to frame rate only where the frame
   is above one vblank floor and within reach of the next (gotcha 237). Quote the share of
   frames pinned to a 16 ms multiple — 10% -> 97% — not the mean, which moved 1.7%.
   Four things worth carrying out of it rather than re-deriving:
   * **`mprotect` was not needed.** The census was extended to name WHICH streams get
     rewritten in place, and all 30 are exactly 80 bytes and all are declared vertex
     bindings — so a guard that hashes anything up to 512 bytes exactly, and larger
     streams at eight spread windows, covers the whole observed population. The
     `SIGSEGV`-handler design (and its `fread`-returns-EFAULT hazard against
     `kernel/vfs.cpp`) is written up in §6av and was never built.
   * **The staleness risk was much larger than the census had said** — ~20 streams a
     frame rather than 30 a run — because a frame-to-frame census cannot see an address
     the guest recycles after a gap, which is exactly what a persistent cache is exposed
     to. Gotcha 235. The guard catches all of them and `stale` counts them.
   * **Eviction is a whole drop, counted, and deliberately not an LRU** — compaction
     would have to move live streams, which is the copying this removed. `flushes` on the
     profile line is the evidence that would justify building the harder thing; it is 0
     in a crowd after one growth to 256 MB.
   * **The guard's cost is charged to `record`, not `streams`** (gotcha 238), because
     `ProfScope(streams)` wraps only the copy and `record`'s scope encloses `UploadStream`.
     Re-baseline `record` before attributing anything to it — it read 5.2% with the store
     off, 9.7% with the store and a per-byte guard, and 6.5% with the widened one.
   * **The stale path overwrites a store slot IN PLACE, and that is safe only because the
     submit is synchronous.** Item 3 below (CPU/GPU overlap) must make it allocate a fresh
     slot instead. The comment is at the line that breaks; the failure mode is a wrong
     mesh, silently.

0a-i. ~~**Vectorise `CopySwapped`**~~ **RETRACTED — MEASURED AT ESSENTIALLY ZERO, and the
   way it died is more useful than the item was.** `CopySwapped` really does compile to a
   10-instruction SSE2 sequence where one `pshufb` would do (`-msse4.1 -mavx` is applied
   to the `ppc_image` target only, so `runtime/gpu/vk_renderer.cpp` is baseline x86-64).
   It is worth nothing, because after 0a there is almost nothing left for it to swap:
   stream copying is 0.23 MB a frame, and **texture upload is 2,387 untilings in a whole
   ten-minute run.**
   I asserted "~1 ms" for this twice without measuring it, on the reasoning that
   `textures` is ~3.1 ms of a crowd frame and texture upload is inside it. `CZ_VK_TEX_CENSUS`
   says that reasoning was wrong at the first step:

   | `UploadTexture` calls over one outdoor run | 166,715,853 | |
   |---|---|---|
   | cache hit | 123,735,182 | 74.2% |
   | served from a DEPTH resolve snapshot | 38,200,406 | 22.9% |
   | served from a resolve snapshot | 2,519,093 | 1.5% |
   | fetch constant is not a texture | 2,258,785 | 1.4% |
   | **actually UPLOADED (untiled + swapped)** | **2,387** | **0.0014%** |

   **`ProfScope(textures)` wraps the whole of `UploadTexture`, not the upload** — the key
   hash, the fetch-constant decode and the cache lookup are all inside it, and the copy
   only happens on a miss. This is gotcha 233's second half ("read where the timer starts")
   arriving a third time, and the third time it was me reading a column name and inferring
   its contents. See 0a-ii, which is what the 3.1 ms actually is.

0a-ii. **`textures` IS 3.1 ms OF PURE LOOKUP — ~13,900 `UploadTexture` calls a frame at
   ~223 ns each, and 0.0014% of them do any work.** This is the item 0a-i was mistaken for,
   and it is well specified because the census above already names every path. Each call
   does a six-dword FNV hash over the fetch constant and then an `unordered_map` find;
   223 ns for that is slow enough to suggest the map is missing cache on a large working
   set, which is a measurement to make rather than a conclusion.
   **It is the same shape as the stream store and should be cheaper:** consecutive draws
   overwhelmingly re-fetch the same constants, so a within-frame memo keyed on the fetch
   slot — invalidated the way the register file already tracks dirty constants — skips the
   hash and the find entirely. Two cautions carried from 0a: measure it against the
   `textures` column and not the frame (gotcha 237), and check where the replacement work
   gets charged before believing the column (gotcha 238).
   Note 22.9% of calls take the DEPTH-snapshot path, which has its own lookup, so a memo
   has to cover both or it will move the cost rather than remove it.

0b. **A FIRST-VISIT STUTTER, found only because an operator played (§6as).** `other` —
   `DoDraw`'s untimed work — sits at ~6% of a crowd frame and spikes to 20-26% (16.7 ms
   a frame) on first arrival at new material, taking the frame from 20.1 to 15.5 fps.
   **It does not recur**: revisiting the same spot 11 minutes later reads 6.1-6.3% across
   six consecutive windows, flat. At MATCHED draw counts one area cost 3.1 ms and another
   16.7, so it is not a draw-count effect.
   **Not pipeline compilation** — inferred three times, failed a pre-registered
   prediction, refuted on magnitude by `GetPipeline`'s new counter (0.08-0.15 ms per
   pipeline, not the ~3 ms assumed; gotcha 232). What remains in `other`: two
   `std::map<uint64_t>` shader lookups, a `std::map<PipelineKey>` lookup with a 40-byte
   memcmp comparator, the fetch-constant decode loop and the vertex-attribute loop — all
   per-draw. **Next cheap step: a per-draw census of sampler slots and vertex attributes**,
   to see whether first-visit draws simply carry more of both.
   This class is invisible to every instrument in this project, because a repeat run has
   already paid for it and the headless recipe visits each area once at a drifting
   offset. Reproducing it needs an operator or a recipe that enters virgin material.

0. ~~**GET A HEADLESS RECIPE THAT SKIPS A CINEMATIC.**~~ **DONE — the recipe is in
   the Commands section above and it reaches live gameplay with no operator.** START
   skips a cinematic; the Zombrex tutorial's second page needs D-pad LEFT then B. Every
   gameplay item below is now self-servable.

1. ~~**CINEMATICS NEVER END**~~ **CLOSED IN PART 29 — no cinematic is known to fail.**
   The one confirmed remaining failure was the PROLOGUE's, and 00j fixed it. On the fixed
   build, one operator session played **four** through to completion with sound: the
   prologue, one more, the walk out of the safehouse, and the **combo-weapon award, which
   awards the weapon**. State it as "none known to fail" rather than "all work" — the
   session did not visit every cinematic in the game. The original retraction trail is
   kept below because it is a worked example of a claim generalised from two failures.

   **RETRACTED THE SAME NIGHT IT WAS WRITTEN.** Later in
   the same operator session, cinematics played through and returned control cleanly —
   the Katey Zombrex grab, and the bike-frame delivery to the safehouse. The claim was
   generalised from two failures and is false as stated. What is actually true:
   **SOME cinematics fail and most do not** — and the list shrank twice more while it
   was being written. The COMBO-WEAPON one (camera parked, HUD gone, and skipping it did
   not award the weapon) **now plays properly and awards the weapon**. So the only
   confirmed remaining failure is the PROLOGUE's, which is black with the camera frozen
   at the first frame; Katey Zombrex, the bike-frame delivery and the combo weapon all
   complete.
   **What fixed the combo weapon is NOT known.** The plausible candidates are the same
   session's shader-cache completion (337 -> 353) and the bindless-heap raise, and
   nothing isolates them. Do not record either as the cause; the honest statement is
   that it was broken on the old binary and works on the new one. If it matters, the
   arm is `CZ_VK_MAX_TEXTURES=4096` — which reproduces the old heap — against a cache
   trimmed back.
   And a large share of the "cinematic is broken" evidence was never cinematics at all:
   the operator established that a black screen after a cinematic was the VIEW-DEPENDENT
   BLACK (item 1c) seen through a camera the save prompt and tutorial had locked. Two
   symptoms, one of them borrowed. Re-derive this item from scratch before working it —
   the surviving question is why those specific three fail, not why "the trigger is
   missing everywhere".
1c. ~~**A VIEW-DEPENDENT WHOLE-FRAME BLACK**~~ **SOLVED IN PART 19: it is the renderer's
   per-frame bump ARENA overflowing, and the auto-exposure hypothesis below is refuted.**
   `ArenaAlloc` skips every draw it cannot satisfy, this title's post-process chain is at
   the END of the frame, and the arena was a fixed 128 MB against a true peak of 161 —
   so a frame with enough geometry in it lost its whole post chain and presented black
   with a correctly rendered scene sitting in EDRAM behind it. "View-dependent" is just
   "which way the camera points decides how much geometry is in the frame".
   Measured both ways on one binary: **128 MB gives 160 black frames of 8,216 gameplay
   frames and 512 MB gives zero**, and within the control arm **every one of the 160 is
   the frame immediately after an `arena EXHAUSTED` line — 160 of 160**. The resolve-chain
   dump of a black frame is what pointed at it: the scene colour `0684B000` was 98.4%
   lit at mean 35.7 and every downstream surface — the downsamples, the luminance ladder,
   the three colour LUTs, the tone-map output — was identically zero.
   The arena grows now rather than being a bigger number (`CZ_VK_NO_ARENA_GROWTH=1` is
   the control). **Part 21 moved the growth OUT of `DoDraw`** — `BeginFrame()` is called
   from inside it, so a growth charged its device-wait and its allocation to the draw
   path's `other`, which §6as measured at 29.8% of a frame. It runs at the end of
   `DoSwapImpl` now, after the fence wait. A measurement fix, not a black-frame fix: the
   count is unchanged at one lost frame per growth in both arms, and it is the frame that
   overran, which is lost either way (§6au). What is NOT closed is the consumption:
   ~27 KB a draw, 8 KB of which is
   the per-draw constant block that `perf-cpu-plan.md` §1a-D already wants deduplicated.
   `docs/phase5-notes.md` §6ap. The original report is kept below, because the
   hypothesis it argues for was wrong in an instructive way — every piece of supporting
   evidence in it was real and none of it was the cause.
1c-original. **A VIEW-DEPENDENT WHOLE-FRAME BLACK, and it is now the top rendering defect.**
   Looking at the gas station (and at least one spot in the Quarantine Area) turns the
   ENTIRE frame black; turning away restores it instantly. It absorbed three separate
   "black screen" reports before the operator noticed the camera dependence. **Missing
   shaders are ruled out** — a run with `no translated shader` = 0 still does it.
   Leading hypothesis: the AUTO-EXPOSURE chain. A whole-frame, instantly reversible,
   view-dependent black is what a degenerate scene-luminance measurement produces — a
   bright emissive surface driving the 64x64 luminance reduction to an inf/NaN and
   collapsing the tone map's exposure. Supporting evidence from the other end: when the
   bindless heap was exhausted and the scene filled with WHITE dummies, the frame washed
   out, so exposure demonstrably tracks scene content. `CZ_VK_SNAP_DUMP` dumps that
   luminance chain and is the direct check.
1d. ~~**The prologue-vs-later cinematic split may be a CLOCK problem**~~ **CLOSED IN
   PHASE A/V: IT WAS AUDIO, AND THE ENTRY BELOW THAT SAYS "not audio" IS THE ONE THAT WAS
   WRONG.** Wiring a real XMA decoder takes the prologue's longest frozen camera run from
   **10,513 frames to 159** and presented coverage from **15.00% to 99.94%**, with
   `CZ_NO_XMA_DECODE=1` as the same-binary control and a second control run confirming the
   pair is not `CZ_FAKE_PRESS_SEQ` drift (the two controls agree to 0.1%).
   `docs/phase-av-notes.md` §4.

   **Read the retraction, not just the result.** Part 16's "not audio" was not a guess: it
   was `CZ_XMA_NULL_DECODER` run in three configurations of one binary — voices always
   playing, never playing, and starting-then-finishing with 19 start / 18 stop edges — all
   freezing identically. The arm moved the predicate the title POLLS and could reach
   nothing downstream of PCM existing, so it refuted a smaller hypothesis than the one it
   was pointed at. Gotcha 268; `docs/phase5-notes.md` §6ah (i) is corrected in place.

   The clock hypothesis was never tested and is now moot for the prologue.
   `CZ_DETERMINISTIC_CLOCK=1` still exists if a LATER cinematic misbehaves.
   Everything else part 16 retired — no deadlock, not our synthetic input, no missing
   import, not the renderer — still stands. The original text follows.

1d-original. **The prologue-vs-later cinematic split may be a CLOCK problem, untested.** The two
   failure modes are opposites — frozen at the first frame, or (apparently) jumping past
   the end — which is what a timeline driven by an unclamped wall-clock delta does either
   side of a long load. `CZ_DETERMINISTIC_CLOCK=1` advances the guest clock a fixed
   quantum per presented frame and is the arm that tests it in one run. NB the "jumped
   past the end" half is itself uncertain: the operator first read auto-skips and then
   retracted them (see 1's retraction). Retired with arms and not to be re-bought: not audio, not a deadlock, not
   our synthetic input, not a missing import, not the renderer. Start from the SKIP
   path — find what it calls to end a cinematic, then ask who else should call it and
   what condition they are waiting on. `cCinematic`, `cCinematicsItem`, `cCineMovieEvent`,
   `cCineBackendMovieEvent`, `cMissionCinematic` are all named in the image with their
   source paths, and `CZ_GUEST_LOG` is already wired for the day the debug gates go up.
   **And skipping is not a workaround for PLAYING**: the operator reports that skipping
   the combo-weapon cutscene does not award the combo weapon, which is the same defect
   from the other end — the completion is what grants the reward. That puts a floor
   under how much of the game is reachable until this is fixed.

1b. **CLOSED POSITIVE — DEBUGJUMP IS A USABLE TEST UI (F2 AT THE TITLE MENU).** Part 24 found that the retail executable still
   carries Blue Castle's entire debug build, gated on 393 name-resolved booleans, and
   `runtime/cpu/debug_tunables.cpp` now flips them (`CZ_DEBUG_MENU=1`, gotcha 239).
   The flags provably flip — 0 -> 1 on a headless boot with a same-binary control — but
   The normal main menu remains unchanged, because
   `enable_debug_jump_menu`'s single reader at `0x824D6170` gates a text-formatting path
   rather than the menu list. The working bridge captures the real frontend manager
   from native startup transitions and F2 requests the shipped `DebugJump` screen via
   `sub_827F6D40`. The operator confirmed it visible; `/tmp/dbgrun5.log` recorded
   manager `A33F4CC0` and hash `ACC86853` on both requests.

   The trigger, read from the code and NOT yet observed: `sub_82483378` is the
   pause-button check (`COMMAND_PAUSEMENU` 0x00 / `COMMAND_FRONTEND_PAUSEMENU` 0x10,
   with the 0x01/0x11 variants when `debug_on_controller_2_only` is set), and
   `0x824A2244` reads `enable_one_button_debug_menu`, calls it, and on a hit dispatches
   through a vtable at `0x824A22D0`. That reads as **START opens the debug menu once
   you are in gameplay**, instead of pausing. The first operator test exposed two
   preset bugs rather than confirming that prediction: `enable_quickie_debug_menu`
   routed the loop to controller 2, and clearing that byte live still produced the
   normal pause menu because the preset had omitted the 26-reader
   `enable_dev_only_debug_tiwwchnt` master gate. The corrected runtime exposes the
   keyboard as controller 2; its preset enables the master and quickie controller-2
   routing. Input tracing then proved Enter and F7+F8 reached pad 2, but neither raw
   XInput chord had a retail binding to the dev-only commands. A first command-query
   bridge also proved the controller manager never creates slot 2: the raw pad packets
   arrived but queries 0x122/0x123 were never made for it. A visibility-byte bridge
   then proved `0x82A5AA4C` toggled 0 -> 1 without showing a menu; its only reader is
   an unrelated text overlay. The actual renderer was identified from its literal
   `"Quickie Menu v0.21"`: `sub_821E55A0` dispatches its four pages and requires the
   previously omitted `enable_button_through_timed_dialogs` gate plus command bytes
   in the active player record. The corrected preset enables that gate, and the host
   fed **keyboard F7+F8 in gameplay** to the active player's menu-held field and the
   real dispatcher ran every frame, but nothing appeared. The final arm bypassed every
   dispatcher predicate and directly invoked page 0 (`sub_82195AB0`), which draws the
   literal `"Quickie Menu v0.21"`. The log proved the call completed without a crash;
   the operator still saw nothing. This closes the input, flag, trigger, and dispatcher
   hypotheses. The experimental bridges were removed.

   **This is an operator test, not a headless one.** Two headless attempts to land an
   in-game START derailed — one to the main menu, one to the save-slot screen — because
   `CZ_FAKE_PRESS_SEQ` is a fixed-interval arm against a boot whose depth in wall time
   is a distribution (gotcha 75), and the same STARTs that drive the recipe are the
   press being tested. Run `CZ_DEBUG_MENU=1`, reach gameplay, press START.

   Why it is worth the five minutes: it would replace both remaining ways of reaching a
   place in this game (an operator playing to it, or `CZ_FAKE_PRESS_SEQ` manufacturing
   its way there over minutes) with a chosen jump — which is gotcha 190's whole
   complaint, and it would make item 2 below and every crowd-performance A/B cheaper.
   Note the ceiling: only Case Zero's scenes ship, so jumps outside
   `prologue`/`prologue_menu`/`prologue_menu2`/`prologue_safehouse`/`safehouse` have no
   data behind them. Nothing here is a substitute for the save round trip, which already
   works — a backed-up `CZ_SAVE_DIR` is the checkpoint mechanism available today.

2. ~~**THE PROLOGUE**~~ **CLOSED IN PHASE A/V — it was the missing XMA decoder.** See 1d
   above and `docs/phase-av-notes.md` §4. Of the three lines this item proposed, the
   cheapest one — raise the engine's own debug gates — became one env var in part 28
   (`CZ_GUEST_DIAG=1`) and is still the right first move for any "what does the title
   think it is doing" question; it just was not needed here. The original follows.

2-original. **THE PROLOGUE — the search space is now much smaller** (see part 16 above). It is
   not audio, not a deadlock, not our synthetic input, not a missing import and not the
   renderer, all with arms to show for it. Three lines, cheapest first: **raise the
   engine's debug-log gates** so the title says what state it is in (`CZ_GUEST_LOG` is
   already wired; the tutorial gate is the global at `0x829EC974`, the cinematic ones
   are object-relative); **instrument the cinematic system directly** (`cCinematic`,
   `cCineMovieEvent`, `cMissionCinematic` are all named in the image, with their source
   paths); or **diff what the guest does per frame either side of frame ~974**, since
   the loading screen and title screen both animate and only the world does not. The
   no-fade shader arm (`CZ_SHADER_SPV` + one line in `ps_114c4965eaabd54c`, §6af) is how
   you watch the scene while it is faded out. **Operator intel: after a new game the
   real game plays two cinematics with loadings between them and then PAUSES to show a
   tutorial** — so a frozen world is a state the game legitimately enters later, and
   there is a known-good sequence to compare against.
1b. ~~**THE SAVE FAILS ON ONE UNHANDLED XAM MESSAGE**~~ **RETRACTED IN PART 19 — THE
   SAVE WORKS.** An operator played to a save point on the part-19 binary and the title
   reported "Game saved successfully"; the log is A3's exact sequence and the file is
   303,104 bytes with bytes 4..31 IDENTICAL to the hardware save A3 shipped. The message
   this item is about, `app FB message 000B0008`, does not appear in that run at all —
   it was fixed by part 16's `XUserWriteAchievements` work and the item outlived the
   defect. **A finding with a complete causal chain can still be about a path the title
   no longer takes**; the chain below was real when it was measured. What actually
   stopped the save was two defects in the file layer (`docs/phase3-notes.md` finding
   52): `NtCreateFile` ignored `createDisposition` and opened every handle read-only,
   and `NtWriteFile` was a stub. The original report follows.
1b-original. **THE SAVE FAILS ON ONE UNHANDLED XAM MESSAGE — measured end to end.**
   `[xam] no handler for app FB message 000B0008 (8-byte buffer) — returning E_FAIL`,
   that E_FAIL completes an overlapped with `0x80004005`, and the save's poll reads it
   (`[save] XGetOverlappedResult(ovl=A3EDD414) block{result=80004005 ...} -> 2147500037`)
   at `825D6094`, where the guest accepts ONLY 0 or 996 and tears down on anything else.
   The content overlapped is innocent — it reads 0. Its sender is
   `sub_825D7CA8(dwordA, dwordB, overlapped)`, which posts the 8-byte pair as XGI
   `0x000B0008`, returns 1627 on a negative result and 997 when given an overlapped;
   callers `sub_825C4400` <- `sub_825C4190` / `sub_825C86A0`. `docs/phase1-notes.md`
   already put `000B0008` among the LOCAL XGI messages (everything past it is Live), so
   this is implementable rather than a gap — but **derive its two dwords from those
   callers before writing a handler** (gotchas 5/59/201: this is a message whose result
   the guest tests, so a wrong "success" is worse than the honest E_FAIL).
   **RETRACTED on the way**: this failure was first read off `CZ_KCALL_WHO`'s teardown
   backtrace as "the CONTENT overlapped poll returns a bad value". A live read of that
   block showed all zeros, and the probe then named a DIFFERENT overlapped. A backtrace
   names the branch, not the datum it branched on.

2. **XAM ordinal `0x271` is resolved on the save-LOAD path and we answer NOT_FOUND** —
   **and part 19 removed this item's biggest confound.** The only save this port had ever
   been able to test against was A3's, made under the fork's profile GUID, and a 360 save
   is signed per profile — so `Damaged Content` may have been the correct answer to that
   file and the ordinal a red herring. There is now a save on disk that THIS RUNTIME
   wrote, from this machine's profile, structurally identical to hardware's. One relaunch
   and a Load Game separates the two explanations for the first time. Everything below
   still applies to the ordinal itself.
   (`docs/phase3-notes.md` finding 51). With A3's real save installed, our content layer
   enumerates it correctly and the title reaches the save-slot panel — then labels SLOT 1
   `Damaged Content` and puts up `Load failed! Please check your storage device and try
   again`, having never opened the file. `imports.cpp`'s `kResolvable` is the SEVEN
   ordinals A1 resolves, and A1 was captured with no save; A3 resolves an eighth. Do NOT
   mint a stub for it blind (gotchas 59/201) — name it from the guest's call site first,
   and note the profile-signature question is separate. This also CLOSES part 12's black
   panels: they are the save's thumbnail, and black is correct for a slot with no valid
   content.
3. ~~**THE SHADOW CASCADE**~~ **CLOSED IN PART 34, BOTH WAYS — both known defects fixed,
   shipped as the default, and operator-confirmed against both control arms in one
   session.** The atlas is 4096x1024 and holds four 1024x1024 cascades; part 31 fixed
   the ADDRESS FOLD that made them four disjoint snapshots, and **part 34 shipped the
   4x MSAA Y factor as the default** (part 32 found it, part 34 reconciled the
   scene-tile objection and flipped it — `phase5-notes.md` §6bf and §6bh).
   Atlas on this binary: **0.0038% zero, 1024/1024 covered rows**, against 46.8750% /
   512 with `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` (the control arm, the part-33 renderer).
   Outdoor era medians: distinct colours **+8.30% at 5.2x the null**, the direction a
   graded shadow term predicts, on top of part 32's independent +14.17% at 40x.
   **The operator's three-way verdict landed the same day** — one Case 0-2 crowd spot,
   same framing, F9 per arm (`~/DR2CZ-troubleshooting/part34-operator/`): the default
   is *"perfect"* in their words with atlas 0.0006% zero at the capture; both control
   arms show hard-edged black false-occlusion blotches on the same truck (46.875% and
   75% atlas zero respectively). §6bh's verdict table. NB at close range the control
   defect presents as BLOTCHES on nearby geometry, not the camera-locked boundary line
   — same defect, distance-dependent presentation.
   **What survives of this item is not a defect but a form**: the EDRAM stand-in at
   SAMPLE resolution in both axes with downsampling resolves is the exact version of
   the shipped per-axis factor. Build it when a defect is traced to the over-clear
   class, not before.

   The part-32 statement, kept for its measurements:

   **Part 32 found that half of every cascade band was ZERO, and a zero depth sample
   reads as OCCLUDED** — 46.8750% of every band, rows 512..1023 minus a 64-column
   sliver, identical in four bands rendered from four different light frusta. The
   geometry is submitted for all of it (`CZ_VK_DEPTH_ALWAYS`: 46.875% -> 1.86% zero);
   the bottom half is rejected by a depth test against the zero the EDRAM image was
   created with, because nothing ever clears it.
   **The cause is that a 4x MSAA surface is twice as TALL in samples as well as twice as
   wide, and only X ever had the factor.** The title's two clear rects for the cascade
   — `(0,0)-(480,512)` on a 520-pitch 4x surface and `(960,0)-(1024,1024)` — tile the map
   EXACTLY when both axes are scaled and cover 53.125% of it when only X is.
   It was held behind an arm because the SCENE tile's 4x clear appeared to want X
   scaled and Y not — reconciled in §6bh: a clear rect is in the CLEAR declaration's own
   pixel space, and doubling Y over-clears past a shorter surface exactly as the X
   factor always has, the same approximation applied consistently.
   **AND THE HARDWARE YARDSTICK BELOW IS RETRACTED.** "Hardware's copy of the same
   surface, 3.5% zero with all four X bands populated" is 16 MB of the PREVIOUS FRAME'S
   COMPOSITED SCENE — the HUD is legible in it. A `.xtr` cannot supply any surface the GPU
   produces inside the traced frame (gotcha 280); `xtr_draw_bindings.py --dump-texture`
   now exits 2 rather than writing those bytes. The target is 100%, not 96.5%.

   The part-31 statement, kept for its measurements:

   ~~**THE SHADOW CASCADE IS STILL HALF EMPTY**~~ **— MECHANISM FOUND AND FIXED IN
   PART 31, and none of the three readings below was right.** The atlas is 4096x1024 and
   holds FOUR 1024x1024 cascades side by side; the title tells them apart by
   pre-offsetting `RB_COPY_DEST_BASE` by 0x20000 each, which in Xenos tiled address space
   is exactly +1024 texels in X (a 32bpp macro tile is 4096 bytes, a 4096-wide surface is
   128 tiles per row, +32 tiles = 0x20000). `DoResolve` un-offset the SCISSOR form of that
   idiom and not the ADDRESS form, so the four became four disjoint snapshots and a fetch
   of the base address read zero past column 1023. Ours was **86.7% zero**; hardware's
   copy of the same surface, dumped out of `w1_spawn` with
   `xtr_draw_bindings.py --dump-texture 1812F000`, is **3.5% zero with all four X bands
   populated**. Fixed, with `CZ_VK_NO_ADDR_TILE_FOLD=1` as the control arm: 53.125%
   non-zero across all 4,096 columns against 13.281% across 1,024, one atlas against four,
   17,355 folds against none, and `13.281% x 4 = 53.125%` exactly.
   `docs/phase5-notes.md` §6bc.
   **What is still owed is the PICTURE.** The atlas is right now; whether the shadow
   LOOKUP is has not been tested, and part 15's instruction — *do not judge the lookup
   until the map is right* — has only just become satisfiable. Ask the operator whether
   shadows appear, and note it will NOT have fixed the white surfaces: filling the empty
   region leaves the plateau exactly where it was (2,074 px at 180 before and after), and
   the sign says why — a zero depth sample reads as OCCLUDED, so an empty region darkens.

   The original statement, kept for its measurements:

   **THE SHADOW CASCADE IS STILL HALF EMPTY, and part 15 halved the question.** The
   operator's top report on the running build is "no shadows anywhere". Part 15 counted
   the empty region's exact boundaries — rows 0..511 fully populated, rows 512..719 in a
   64-wide strip at x=960..1023, nothing below 720 — fixed the one boundary that was
   ours (the window-coordinate clip at 720, worth exactly that strip), and named the
   title's own clear rects with `CZ_VK_DRAW_PROBE`: **`(0,0)-(480,512)` and
   `(960,0)-(1024,1024)`**, z=1.0, compare func ALWAYS. Those do not cover a 1024x1024
   map and nothing this renderer does causes it. Three readings, all testable: 480x512
   is a PIXEL extent wanting a x2 somewhere (the pass reports `msaa=0`, so our 4x
   scaling does not apply — check what the guest thinks that surface's sample extent
   is); the "cascade" is really several smaller maps packed into one surface (four
   cascades are resolved per frame and it is 4096 wide); or the uncleared region is
   never sampled and the shadows fail elsewhere. Test the third first, because it is
   free: the consumer's fetch coordinates say which part of the map it reads
   (`CZ_VK_DRAW_PROBE` on the pass that fetches `1439B000(depth)`, 629,023 fetches a
   boot). Do NOT judge the shadow lookup until the map is right.
3b. **THE BINDLESS HEAP — MITIGATED (4096 -> 65536), NOT YET FIXED.** Confirmed
   working: a Still Creek session that reached **4,522 slots** reports `bindless heap
   full` ZERO times and the white buildings are gone. That is past the old cap, so the
   same session would have been serving dummies before. Slot recycling is still the
   real fix; a cap is only ever a bigger number. NB the operator's white BLOOD SPLATTER
   is NOT this — it keeps its splatter shape and only the colour is wrong, and the heap
   is healthy — so it is a separate texture/shading defect.
3z. **THE BINDLESS HEAP WAS EXHAUSTED IN STILL CREEK — MEASURED.** White buildings,
   white NPCs, white blood, white button glyphs, and `R->nextTextureSlot` read
   **4096** — exactly `kMaxDescriptors` — out of the operator's LIVE process with
   `gdb -p ... print`. Texture slots are handed out monotonically and NEVER RECYCLED
   (`entry.slot = R->nextTextureSlot++`); on overflow `UploadTexture` returns slot 0,
   the 1x1 white dummy, and counts `texture: bindless heap full` silently.
   **The fix is slot recycling** (an LRU over the texture cache, with deferred
   destruction so an in-flight frame cannot lose its image).
   Two things the operator's pictures add that a counter cannot. The rule is not "late
   in TIME goes white" but **"anything needing a NEW SLOT after the heap filled goes
   white"** — this title streams textures BY DISTANCE, so approaching a building
   requests a higher-resolution texture, which is a new fetch constant, a new cache
   entry and a new slot. That is why every building whitens on approach while its
   distant version was fine. And the washed-out frame and greyed HUD are a PREDICTED
   second-order effect: the dummy is white, so a scene full of dummies is a scene full
   of maximum-luminance surfaces driving auto-exposure and bloom too bright — if the
   wash survives the fix, it is a separate defect. Pictures 13-16 in
   `~/DR2CZ-troubleshooting/INDEX.md`.
   **The cheap confirming arm before the real fix**: raise `kMaxDescriptors`. If the
   buildings render, the whole causal chain is proven end to end; it is not the fix,
   because a cap is only ever a bigger number.
3e. ~~**PP / LEVELLING IS BROKEN**~~ **RETRACTED WITHIN THE HOUR — Case Zero needs
   20,000 PP PER LEVEL, and the operator was at 9,700.** Staying at LV. 1 was CORRECT
   and there is no levelling defect. Progression is tracked properly throughout: the
   STATUS screen reads `OVERALL TOTAL PP 9,700`, `CURRENT GAME TOTAL PP 9,700`,
   `ZOMBIE KILL COUNT 41`, money $2,000 -> $17,000.
   What survives is small and cosmetic: **the HUD's PP bar looks empty at 9,700 of
   20,000**, where it should read roughly half full. Worth one look, not an
   investigation — and note the bar is thin and dark, so "looks empty" is an
   impression from a screenshot rather than a measurement.
   The case timer bar not running is UNCHANGED and still open. NB the guest clock IS
   advancing (save screens read `Day 1 - 07:08 AM` then `07:58 AM`), so a stopped case
   timer is not a stopped clock.
   The lesson is the cheap one: the threshold was a NUMBER, it was knowable, and the
   defect was written up before anyone looked it up. Check the game's own rules before
   filing a game-logic bug.
   NB the guest clock IS advancing (save screens read `Day 1 - 07:08 AM` then
   `07:58 AM`), so a stopped case timer is not simply a stopped clock.
3d. ~~**NPC PART MESHES GO MISSING**~~ **CLOSED IN PART 35 — the re-test the item asked
   for was run and the parts are back.** Dick renders whole (head, hands, legs, body)
   in every capture on two binaries
   (`~/DR2CZ-troubleshooting/part35-item1-operator/`). The missing parts were the
   shader-cache gap, exactly as the "re-test on a fresh launch BEFORE investigating"
   note predicted — every one of the 16 shaders that session lacked is translated now.
   What Dick shows INSTEAD is the striped-material class (item 0s below): his far-LOD
   material renders as banded garbage while his near material is clean, or vice versa
   depending on the boot.

   The original statement, kept because its method note (look for what missing parts
   share, not what is wrong with one character) is right the next time parts go
   missing: Dick rendered as a head and one hand; Fausto had no legs; Gemini no hair.
   Assembled from separate part meshes (`childface`, `childhand`, `childupperbdy`,
   `childfullbody`).
3c. **The pause menu is sheared and broken in STILL CREEK and perfect in the
   SAFEHOUSE.** Same menu, same shaders, different world state — so it arrives with its
   own control, which is rare. The paper becomes a trapezoid with stray white polygons
   and thin black lines, i.e. garbage GEOMETRY rather than a texture fault. Part 13
   established that this title sub-allocates its whole UI out of ONE dynamic vertex
   buffer via `VGT_INDX_OFFSET`, so a busier scene sharing that buffer is the obvious
   place to look: an offset that drifts, or a buffer that wraps.
3f. ~~**PERFORMANCE IS NOW A REAL ITEM: 8-12 fps in gameplay.**~~ **ORDINARY GAMEPLAY IS
   CLOSED (11.8 -> 31 fps, the title's own cap). CROWDS ARE THE OPEN ITEM, AND THEY ARE
   CPU-BOUND IN OUR RUNTIME — the plan is `docs/perf-cpu-plan.md`.** A Still Creek zombie
   crowd issues **4,800-6,800 draws a frame** against ordinary gameplay's 1,930, which is
   3.5x the workload every conclusion in this port was based on, and it reorders the
   whole frame budget. At 6,592 draws / 43.4 ms with the GPU at 1950 MHz: **renderer draw
   path 21.4 ms (49%), PM4 walk 11.0 ms (25%)**, GPU 6.6 ms (15%), pump sleep 3.9 ms.
   75% of the frame is our own CPU in two roughly equal halves, both linear in the draw
   count, neither ever optimised. **Item 0 of that plan blocks the rest: the headless
   recipe reaches gameplay but only ~1,930 draws, i.e. it never enters the workload —
   so every A/B would run against the vblank cap where CPU savings are invisible by
   construction, which is exactly how the state cache first measured as a dead heat.** The first operator play-through
   ran at 8-12 fps and stopped partly because of it; the frame rate was a blocker on
   EVIDENCE rather than a polish item. Two changes, neither of which deletes any work,
   took a gameplay frame from 84.4 ms to ~34 ms — **the ring is walked promptly
   (`CZ_PM4_TICK_MS`, 1.21x) and the vblank arrives on time (`CZ_VBLANK_TICKCOUNT`,
   2.0x)** — both because a frame's dominant term was the graphics pump asleep in its
   own loop, which no instrument here could see. `docs/phase5-notes.md` §§6aj-6am.
   **Every "known contributor" listed here before was wrong or unmeasured**: the
   synchronous submit is 0.1% on the CPU side, the per-draw constant upload is 0.5%,
   the whole renderer is under a quarter of the frame, and the readback was fixed in
   part 17. **What is left is one open question and it is not ours**: the GPU has been
   running at **210 MHz of 2100** for every measurement this port has ever taken
   (gotcha 219). ~~**ANSWERED by the operator session: `sudo nvidia-smi -pm 1` then
   `-lgc 2100,2100` takes it to 1950 MHz...**~~ **RE-ANSWERED IN PART 20, AND THE
   PREMISE WAS AN ARTIFACT.** The 210 MHz came from an overnight session with the
   MONITOR ASLEEP. Measured with the display awake over a full crowd run: **P5 in 182 of
   200 samples, mean 524 MHz, 32% utilisation, 28.6 W**, and `vkcube` settles in the
   same place on the same machine — the control that was never run. The governor was
   never mistreating us; **do not pin the clock**, sample it with
   `tools/gpu_clock_sample.py` and quote it.
   What the pair of numbers actually says (gotcha 231): a low clock at LOW utilisation is
   correct power management, and our 32% means the GPU is idle 68% of every frame because
   `SubmitAndWait` blocks immediately after submitting. **The defect is the idleness.**
   Overlapping the CPU and the GPU would take a 44.6 ms crowd frame to about
   max(27.7, 16.5) = ~28 ms — **22 -> 36 fps at the SAME 28.6 W**, which is more than
   everything part 20 did and costs no power. It needs the per-frame READBACK off the
   critical path (a real swapchain present) and a second frame-in-flight arena. This
   revives the overnight plan's §2a, which `docs/phase5-notes.md` §6al dismissed as
   "aimed at a number that is mostly an artifact of the machine's power state" — the
   artifact was the MEASUREMENT's, not the frame's (gotcha 172).
   **RE-SCOPED BY THE OPERATOR SESSION (§6as).** Seven crowds, 26,241 frames: the areas
   are interchangeable at matched draw counts and the item is one sentence — **a
   ~7,500-draw crowd is ~20 fps**, everything below ~4,000 draws is already at the
   title's cap. Real crowds are 7,000-9,000 draws where the CPU alone exceeds the 32 ms
   cap, so overlap is worth ~1.45x (to ~27-30 fps) rather than reaching the cap as my
   headless ceiling claimed. `streams` is the biggest draw-path term in real crowds
   (12.3-14.3%, twice the headless figure), so plan §1b outranks §1a.
   **ITEM 0 OF THAT PLAN IS CLOSED (part 19): the headless recipe reaches the outdoor
   world and 6,400-8,100 draws a frame.** It is in `CLAUDE.md`'s Commands section beside
   the safehouse one; the change is one extra `B` at the door and alternating `LSUP` with
   `RSRIGHT`/`RSLEFT`. §1 and §2 of the plan are now runnable.
   **PART 20 RE-MEASURED THE DRAW PATH AND THE PLAN'S RANKING OF §1 WAS WRONG**, because
   `ProfScope` counted nested phases twice (gotcha 228). `record` is 6.7 ms, not 11.07,
   and `other` — `DoDraw`'s own untimed work, filed as "the cheapest item in this
   document" — is 5.6 ms and second in the draw path. The PM4 walk is unaffected and
   re-measures at 11.8 ms. Corrected table in `docs/perf-cpu-plan.md` and
   `docs/phase5-notes.md` §6aq.
   Two things were done on it. The per-draw path carried five `std::map<std::string>`
   counters, four `getenv` calls and an ungated `snprintf`; removing them takes `record`
   −47% and `other` −49% at matched draw counts. And §1a hypothesis A was measured
   rather than assumed: in the crowd era **34% of vertex binds and 22% of index binds
   repeat the previous offset**, worth ~1.4 ms — real, a third of what the hypothesis
   expected, and permanently below this workload's noise floor, so it can only be
   claimed from the counter.
   **The noise floor is the thing to carry forward: two runs of ONE binary disagree by
   10-13% in the crowd bins** (gotcha 229), so a frame-time claim here needs three runs
   an arm, alternated. `tools/frame_perf_bins.py` bins frames by draw count and pools
   runs; `docs/measurement.md` has the recipe.
4. **No mipmaps have ever been uploaded** — `ci.mipLevels = 1` in `CreateImage`, every
   texture, every phase. This is the operator's "all textures seem weird grainy", and it
   is real work rather than a one-liner: the Xenos mip chain has its own address layout.
4b. **A KNOWN-WRONG TEXTURE PATH FIRES 1,337,658 TIMES IN ONE OUTDOOR RUN, and it has been
   counting quietly the whole time.** The counter names its own defect:
   `texture: snapshot served at the surface PITCH, not the fetch's declared size — texture
   coordinates would be scaled wrong`. Found incidentally while running
   `CZ_VK_TEX_CENSUS` for 0a-i; nobody had looked at it because the census needs
   `CZ_VK_STATS=N` as well and so had effectively never been read.
   Same run: `texture: colour fetch served by a DEPTH resolve snapshot` 10,956, and
   `texture: uploaded entirely BLACK (the guest has not written it)` 250.
   None of these is diagnosed. All three are cheap to start on because the counter already
   says which fetch and the census's per-address table prints the extent and format —
   and a wrongly scaled texture coordinate is exactly the kind of defect that reads as
   "the art looks a bit off" rather than as a bug. **Read this before working 3, 4 or 6**:
   it may be the mechanism behind one of them rather than a fourth separate thing.
5. **The Still Creek sign's dark smear and the GAS roundel.** Neither has an identity.
   Both are `CZ_VK_SKIP_TEX` to name the address, then `CZ_VK_TEX_DUMP` to separate "our
   decode scrambled this" from "the texture is fine and the draw shades it wrong". The
   smear is NOT the untiler (0 skips in 925 textures) and NOT a shadow
   (`CZ_VK_NO_DEPTH_FETCH=1` leaves it).
6. **The last picture difference: colour is flat and green-shifted** (§6ad item 2).

   **PART 27: THE GRADING LUT IS REAL, IT IS APPLIED, AND THE INDEX RESPONDS TO THE
   CLOCK.** The whole chain was walked end to end for the first time.

   * The 15 grades ship as `cc_01..cc_15.bct` inside `data/streamedassets.big`, LZX
     compressed. `tools/big_list.py --extract` plus `tools/big_decompress` recovers them;
     each is **131,120 bytes = a 48-byte header + a 32-cubed RGBA LUT unrolled into a
     1024x32 strip**, and TILED (see `docs/big-archive-format.md`).
   * **They are not 15 distinct grades.** By content: `cc_01 == cc_04`, and
     `cc_07..cc_13` are one grade — roughly five distinct looks across the fifteen slots.
   * **The runtime binds one**, `1024x32` at slot 4, to `ps_114c4965eaabd54c`, exactly one
     draw a frame. The title renders its live LUT into three surfaces (`14338000`,
     `14359000`, `1437A000`) which hold identical bytes while no transition is running.
   * **The content served is an EXACT byte match to a disc LUT — mean |delta| 0.00** —
     against a field where the runner-up is 16 away and the furthest is 110, so the match
     is a measurement and not a coincidence.
   * **And the index tracks the clock**: with `DISABLE TIME OF DAY` the bound LUT is
     **`cc_03`**; with the clock running it is **`cc_01`**, stable over five samples across
     two minutes. Freezing the clock changes the chosen grade, which is what says the
     selection is driven by time-of-day state rather than being a constant.

   **WHAT IS STILL NOT ESTABLISHED, and the capture cannot settle it.** Whether the index
   we pick is the index HARDWARE picks at the same in-game time. `w1_spawn` (7:53 am)
   binds its LUT at `180CC000`, which is a RESOLVE DESTINATION there as well — so the
   trace's `MemoryRead` carries whatever the allocator left, not the resolved pixels
   (gotcha 113). Read tiled it is 16 of 31 monotone on the neutral diagonal; read linear,
   15-18 of 31. **A LUT is monotone in every channel under the right layout, and this is
   monotone under none, so those bytes are not a LUT** and the best disc match is a
   meaningless 55.6. The capture is not omniscient — the same lesson as gotcha 263, in a
   different subsystem.

   **One thread worth stating without over-reading it:** our served LUT matches a disc LUT
   EXACTLY, i.e. we snap to one grade with no blending, and the title keeps THREE LUT
   surfaces, which is the shape of a cross-fade. If hardware blends between adjacent
   grades and we snap, colour would be right at the endpoints of each time band and wrong
   in between — which is the sort of thing "colour is flat" describes. **There is no
   evidence either way yet**; establishing it needs a capture taken during a transition,
   or the guest code that writes those three surfaces.

   Much improved by part 14 and not closed. The tone map's LUT is what §6s proved this
   frame depends on completely.
7. **The conservative screen extent is still a placeholder** (part 11). Both tiles
   execute ~975,000 draws where hardware executes ~573,000 each. **Do not do this
   speculatively** — the cost has still not been shown to matter.
8. **A1 is exhausted as an oracle.** Its position 93 is NOT the next piece of work —
   `KeQueryBasePriorityThread` has been implemented since phase 1, and reaching it
   means reproducing an audio-subsystem FAILURE that hardware had once, late, on a
   path we do not drive (finding 49, gotcha 107). Going further needs a gameplay
   comparison built from A2 — and the run that reaches the prologue is the first this
   port has had that would exercise one.
9. **Prove the still-unexercised imports** (gotcha 67 — implemented is a prediction,
   not a result). Four of finding 34's eight have now RUN — `XamTaskSchedule`,
   `XamGetOverlappedResult`, `XMsgInProcessCall`, `XMsgCompleteIORequest`, all on the
   save-data path. **Part 19 cleared the save layer's own
   `XamContentCreateEx`/`XamContentClose` off this list and added `NtWriteFile` and
   `XamContentCreateInternal` to the proven set**, all four on a save/load round trip an
   operator drove end to end. Still unrun: the rest of finding 34, both of finding 36's
   teardown paths (`XAudioUnregisterRenderDriverClient`, `XMAReleaseContext` — the boot
   never shuts audio down), and part 13's `XeCryptSha` one-shot.
10. Audio output and XMA decoding (phase 6). **DEMOTED by part 16** — it is no longer
   a candidate for the prologue blocker, so it is back to being "the game is silent".
   The kick bitmap at `0x7FEA1A80` lands in ordinary flat memory and is inert; a real
   decoder needs that aperture trapped as MMIO or the kick is written and never
   noticed. `CZ_XMA_NULL_DECODER` is the half-implementation to build on: it already
   models input consumption at a rate.
11. **A VFS gap, recorded rather than fixed** (§6ah(vi)). `VfsTranslate` returns empty
   for any guest path with no `:`, so a path with no device prefix can never resolve.
   A boot makes 29 such opens; none of those files exist under any prefix either, so
   nothing is currently lost. On console a relative path resolves against the title's
   own directory, and CLAUDE.md already warns that at least one path here is built at
   runtime (`anm_%s.big`).
12. **The keyboard is XInput user 2, so a machine with no gamepad cannot drive player 1**
   (part 24). Deliberate: the title's own controller-2 debug route consumes the keyboard
   without stealing the gameplay pad, and `runtime/kernel/imports.cpp` states that
   policy where the pad count is set. The cost is that keyboard-only play, which worked
   before, no longer does — `Host_PadState(0, ...)` reads the SDL controller alone.
   Headless is unaffected, because `CZ_FAKE_PRESS_SEQ` is checked ahead of the device
   for every user index. **Low priority and not blocking anything**: the operator has a
   pad, and the debug menu depends on the split. The fix, when it is worth doing, is a
   fallback rather than a revert — route the keyboard to user 0 as well when SDL reports
   no controller attached, so the split only exists when there is something to split.
   Say so in the log line either way, because a keyboard that silently stopped driving
   the game looks exactly like input that broke (gotcha 214).


---

## 00o. THE PART-56 OPERATOR SESSION: four defects, two hypotheses refuted, one measured gap

All of this is from one evening of operator captures (2026-08-19,
`~/DR2CZ-troubleshooting/play/play_0819_*`). Recorded in full because two of the
hypotheses were WRONG and the refutations cost real time to buy.

### What the operator reported, in their words

* **decals** — *"pretty much normal but it appears and disappear like flicker"*, and
  decisively: *"if you do not move the camera the decals either stay or is not there"*.
* **the GAS sign** — broken lettering at distance, correct up close: the letters go BLACK
  with a magenta/yellow/cyan fringe on the G.
* **the bunting** between sign and gas-station roof — absent at distance, present close.
  The string survives as a REGULARLY SPACED DASHED LINE whose spacing matches the pennants,
  i.e. the surviving top edge of each triangle. **Not LOD culling** — that removes an
  object, it does not leave evenly spaced remnants.
* **the canopy fascia** — black at distance, red up close, changing in DISCRETE STEPS as
  you approach.
* **cutting a zombie in half** — *"it get separated in two full zombie and the blood is a
  square"*. Confirmed in `capture_029266`: two COMPLETE bodies, each sitting on a red
  rhombus of gore texture.

### REFUTED 1: z-fighting / the missing polygon offset

The camera-motion dependence is the classic signature of z-fighting, and the renderer
genuinely never set `depthBiasEnable` — `PA_SU_POLY_OFFSET_*` was read nowhere, in this
port OR in Fable 2's. The registers were censused rather than inherited (Fable 2 reads the
enable bits from 0x2205, which is RB_BLENDCONTROL1 in our verified map), and the census
found the offset REAL and pointed at the right draws:

    3715 draws  po=0/0/0
     178 draws  po=0/-4.8/-3e-05   } all `verts=6 prim=4 blend=07060706`
      48 draws  po=0/-1.6/-1e-05   } — one alpha-blended quad each, i.e. DECALS

So the decode was right and the change reached the decals. **It fixed nothing.** The
positive control is what settled it: `CZ_VK_POLY_OFFSET_SCALE=10000` visibly lifts decals
in front of walls and zombies, so the path reaches the picture — and the operator's verdict
at that setting was *"when they are not floating they flicker"*. **A ten-thousand-fold
depth bias does not stop the flicker, so the flicker is not a depth fight.**

The polygon offset is KEPT (it is guest state we were ignoring, and the gates pass at
+0.8823) but it fixes nothing reported. `CZ_VK_NO_POLY_OFFSET=1` is the control arm.

### REFUTED 2: the mip chain

The unifying story was that lower mip levels decode wrong — colour for the sign, alpha for
the bunting — and that mip SELECTION oscillating under camera motion explained the decal
flicker while a still camera pinned it to one level. It fits every symptom and it is wrong.

`CZ_VK_NO_MIPS=1` **engaged** (the mip-level-rejection lines go 8 -> 0) and the operator's
verdict was *"gas is still black and everything still is like before"*. The plumbing was
checked afterwards and is correct — `CreateImage` takes the level count and the view's
`subresourceRange` carries it — and the counters say the chain is real (7,088 chains
uploaded, 18,397 packed-tail levels taken). **The sign is black with mips and without
them, so mip DATA is not what blackens it.**

One loose end left deliberately open: the operator also saw NO added aliasing with mips
off, which a live chain should produce. That is a soft observation against a hard one
(the sign), so it is recorded rather than acted on — but "turning the mip chain off changes
nothing visible" is itself worth a measurement someday.

### IMPLEMENTED IN PART 56 (see below for what it fixed): the stencil test, and it was
### MEASURED rather than inferred

**`stencilTestEnable` appears ZERO times in this renderer**, while RB_DEPTHCONTROL bit 0 is
`stencil_enable` — our own comment says so, and the code reads bits 1, 2 and 4-6 and skips
it. Censused on the operator's own slicing frames:

    capture_f29266:  350 of 1980 draws have the STENCIL TEST enabled
    capture_f27715:  326 of 1823 draws

Five distinct stencil-enabled RB_DEPTHCONTROL values, all decoding sensibly on the other
fields (z_enable, z_write, zfunc), which is a check on the bit layout.

**And the operator's "two full zombies" is what identifies the mechanism.** The game is not
producing half-meshes: it draws the WHOLE body twice and masks each copy to one side of the
cut. Nothing masks them, so both copies render whole and the cross-section cap renders as a
full quad — two symptoms, one cause. The depth format is already `D24_UNORM_S8_UINT`, so the
buffer exists.

**Before implementing:** the stencil func/op field layout of RB_DEPTHCONTROL and the
ref/mask register must be located BY CENSUS, the method that worked twice in one evening
here and that a register document would not have settled (the 0x2205 confusion above).

### The decal flicker is now UNEXPLAINED, and its constraints are

Not depth (refuted at 10,000x). Not mip data (refuted). Camera-motion dependent, stable
under a still camera in BOTH states — present or absent. `CZ_VK_DRAW_ID` on a burst, or a
census taken at two camera positions a frame apart, is the obvious next instrument; the F8
burst as built carries no per-draw census, which is the gap that stopped this evening's
analysis and is the first thing to fix if the defect is picked up again.

### 00o (cont). THE DISTANCE DEFECTS: the operator's own pointer, and what it found

Their steer when the stencil work started: *"For the gas sign, rooftop and all this type of
thing that are at a distance I am pretty sure it is similar to our last issue that was like
that."* That is **item 00i** — the "flat panels until you walk up to them" class, which part
45 traced to OUR shader translation dropping pixel-shader interpolants, leaving 217 shaders
sampling diffuse at ONE TEXEL. A single texel is a flat colour, and a dark one is BLACK,
which is precisely the sign's symptom. It is a better-shaped hypothesis than either of the
two this evening refuted, because it explains a colour REPLACEMENT rather than a colour
error.

**What the captures say, and it supports the steer.** Diffing the pixel shaders used in the
far/broken sign frame against the near/fine one:

    17 pixel shaders appear ONLY in the far frame
     9 appear ONLY in the near frame

So **this title swaps pixel shaders by distance** — the far view is not the near view's
shader with a smaller texture, it is different code. That alone makes "the defect lives in a
distance-only shader" a live and cheap-to-test proposition, and it is consistent with mips
having been ruled out (a shader swap is not a mip).

**What the metadata does NOT show**: the 17 far-only shaders carry 2-8 interpolators
(median ~4) against a whole-cache median of 5, so none of them is obviously stripped the way
part 45's were. If this is the 00i class it is a different manifestation of it, not the same
bug recurring.

**The next measurement, when this is picked up**: identify WHICH of the 17 draws the sign,
then read our translated SPIR-V for it against the capture's own disassembly — the step
part 27 named for the white-surface class and which has still never been taken. The far-only
list is in `~/DR2CZ-troubleshooting/play/play_0819_1626/capture_f12174.census`.


### 00o (cont). WHAT THE STENCIL FIXED, AND WHAT IT DID NOT

Implemented at the close of part 56 (`CZ_VK_NO_STENCIL=1` is the control arm). Operator
verdict: **"Isn't as bad but far from perfect"**. Their captures in
`~/DR2CZ-troubleshooting/play/play_0819_1815/` show real improvement — `capture_013122` has
a correctly shaped LOWER HALF with a proper cross-section, and `capture_010173` a correct
decapitation — where the previous build gave whole bodies sitting on rhombi.

**WHAT IS STILL WRONG IS ASKED AND UNANSWERED.** Three captures were examined and the
remaining defect could not be identified in them: both bodies in the close-up render
correctly, one headed and one decapitated with a proper cap. **Do not guess it from the
images.** Useful shapes for the question: is the cap the wrong shape, colour or place? do
halves still sometimes come out whole? does it depend on the weapon or the cut direction?

If it proves intermittent, three untested suspects in order: the guest writing the mask in
one pass and testing it in a later one while our EDRAM handling clears or reuses the
depth/stencil image in between; the two-sided reading (`ds.back = ds.front` when
RB_DEPTHCONTROL bit 7 is clear is an INTERPRETATION, not a measurement); and the stencil not
surviving a resolve.


## 00p. PART 57: where the three part-56 defects stand now (the full record is
## phase5-notes §6cm; every measurement below is from OPERATOR sessions)

* **ZOMBIE SLICING — the doubling is FIXED, two residuals remain, mechanism scoped.**
  User clip planes are implemented (the VS computes ClipDistance[6] from planes the
  runtime publishes at SharedConstants+2080; `XE_USER_CLIP_PLANES` second-cache arm,
  poison-proven in both directions). The halves separate now. Residuals: the cut is
  see-through (the gore plug that seals it is clipped away) and a thin slab sometimes
  shows on both halves. CZ_VK_CLIP_BIAS measured the scale: +0.01 of a plane's magnitude
  un-clips the WHOLE body, so the plug lives on a sub-0.01 margin around dot=0. **Next:
  derive the correct dot SPACE from the ten captures' plane values + poses — do not fit
  an epsilon.** The 00o two-sided-stencil suspicion is retired: the gore-plug draws use
  two-sided stencil and our pipeline reads those fields correctly.
* **THE GAS SIGN (operator's #1) — attributed, shader exonerated, suspect narrowed to
  far-LOD texture CONTENT, hardware trace R6 filed and blocking.** Far sign =
  `ps=57d441f53fc93ad7` + letters `ps=86ac6569ea0d700d` (draw-ID, no inference). Our
  translation of the first is instruction-for-instruction faithful to the capture's
  disassembly. A live dump seconds after an operator F9 shows the letters' 32x16 DXT1
  mostly BLACK with garbage in guest memory, its 64x64 companion incoherent at every
  extent — same tool decodes the neighbouring 256x256 perfectly. R6 (one single-frame
  trace at the far viewpoint) says whether hardware's guest memory holds the same bytes
  (-> our sampling) or different ones (-> our streaming/level machine, and the right
  bytes to aim for). `docs/xenia-capture-requests.md`.
* **THE DECAL FLICKER — "never issued" is RETRACTED; the title triple-buffers.** The
  burst census + the va= field established a 3-address rotating ring for decal geometry
  with per-address content constant; folded by class, decal draws are issued EVERY
  frame. A blinking decal is issued-and-lost, prime suspect the cross-frame stream store
  + frame-ahead guard against a 3-frame ring (`CZ_VK_NO_PARALLEL_GUARD=1` is the ready
  A/B). The defect DID NOT FIRE in either part-57 session — recorded as an observation,
  not a fix; the arm waits for the next sighting.

## 00q. PART 58: the slicing residuals' space hypothesis is REFUTED offline; the margin
## probe is rebuilt correctly and waits on one operator ladder

* **THE DOT SPACE IS SETTLED — IT WAS ALREADY RIGHT.** 00p's "derive the correct dot
  SPACE from the ten captures" is done (`tools/clip_plane_space.py`) and the answer is
  that no space error exists: all 88 distinct captured planes are UNIT VIEW-SPACE
  planes under the scene projection (|n| = 1.000 ± 0.0003 RMS after one two-parameter
  fit — fov 42.98°, 16:9 exact; the pose's first-draw 45° matrix is a different
  camera). The register planes are exactly clip-space planes, our raw-oPos dot is
  hardware's dot, and the view matrix the poses failed to capture (all ten bvc blocks
  are the SHADOW pass's ortho — the "biggest draw" heuristic caught the shadow ground
  draw every time) was never needed: Projᵀ alone maps a plane to view space, where
  meters are true.
* **00p's margin claim is RETRACTED** ("the plug lives on a sub-0.01 margin"): the
  CZ_VK_CLIP_BIAS arm it was read from adds eps·|P| to w, and because the captured
  planes have c ≈ −d, that increment lands entirely in the view plane's z-COEFFICIENT
  — a ROTATION worth 0.8–8 m of boundary at eps=0.01, which is what "un-clips the whole
  body" actually measured. The plug's clearance has never been measured.
* **The probe that measures it is built**: `CZ_VK_CLIP_SHIFT=<meters>` translates every
  enabled plane's boundary by true view-space meters (Δplane = Proj⁻ᵀ·(0,0,0,δ), z-row
  constants derived from the captures). `tools/part58_operator_session.sh` is the
  ladder: 0 / +0.05 (positive control — a ~10 cm doubled band must appear) / +0.02 /
  −0.02. Heals at +0.02, worsens at −0.02 → the gore sits centimeters from the boundary
  and our clip errs at precision scale. No change at ±0.02 while ±0.05 moves → **the
  clip branch CLOSES** and the suspect becomes the four-pass interlock, which part 58's
  census read established: per piece and per tile, the order is two-sided stencil
  write (dc=047087B7, refs 0xB0+i, BODY plane) → visible gore paint (dc=04708797, z
  LESS, refs 0xAC+i, a SECOND plane ~40° off the body's) → body depth prepass
  (00700736) → body color at EQUAL (00700722, alpha test on). The gore pass uses the
  SAME ps and textures as the body — the gore look must come from mesh interior
  geometry — and both stencil funcs are ALWAYS (nothing tests stencil among the four).
* Sign (R6) and decal-flicker items: unchanged from 00p, both still waiting on their
  external events.

**00q RESOLUTION, same day: THE SEE-THROUGH CUT IS FIXED — it was TRIANGLE FACING, not
clip.** The ladder ran and changed nothing at any rung (all counters engaged), closing
the clip branch; the censuses then found the sealing pass part 57 never saw — the gore
cap is a 6-vert QUAD stencil-tested EQUAL against the per-piece ref the two-sided
passes write front-REPLACE/back-ZERO (tester refs match writer refs in-frame). Facing
has no other consumer in this renderer (culling permanently NONE, su=00080008 on every
draw), so the hardcoded CCW front sat unverifiable until part 56 wired two-sided
stencil — inverted, the stencil mask complements and the quad fails exactly at the cap,
view-dependently, which was the operator's report verbatim. One arm flipped it;
operator: **"Yes it is perfect now."** FRONT=CW is the default;
`CZ_VK_STENCIL_CCW_FRONT=1` is the control arm. `phase5-notes.md` §6cn §6. Remaining on
watch: the part-57 doubled-slab sighting was not re-observed in any part-58 session —
an observation with a shelf life, re-open if it shows.

## 00r. PART 59: THE DISTANCE CLASS (gas sign) IS FIXED — small packed textures

The R6 trace closed item 00's oldest distance defect in one session. The far-LOD
"black letters / garbage" class was neither texture content (byte-identical to
hardware's — the part-57 content suspicion is retracted in §6co) nor the shader
(exonerated in part 57): **any texture with a dimension <= 16 texels packs its whole
chain, LEVEL 0 INCLUDED, into one tile at fixed offsets with mipAddr=0** — our
renderer read level 0 at the tile origin (the scrap region) and skipped the chain.
79 distinct textures in one street frame. Fixed in commit cf62229
(`PackedLevelOffset`, verified 69/70 chains + the 378/378 square table before
shipping); first session read 1,662 base-offset reads / 2,404 packed levels taken;
operator: **"Work really well now."** Control arm `CZ_VK_NO_PACKED_SMALL=1`.
Full record: `phase5-notes.md` §6co. R6 is FULFILLED — no outstanding capture request.

Still open from the part-56/57/58 chain: the decal flicker (waiting on a sighting;
F8 burst + CZ_VK_NO_PARALLEL_GUARD=1 A/B ready) and the doubled-slab watch (00q).
