# Open items, in order

**Split out of `CLAUDE.md` on 2026-08-08.** The actionable backlog. Items struck through
are closed or retracted and are kept because a retraction is worth as much as a finding
— several of these were "fixed" by something else entirely, and the record of what was
NOT the cause is what stops the next session re-buying it.

Next, in order:

00. ~~**CUBE MAPS ARE NEVER BOUND.**~~ **BUILT IN PART 25, AND ROUGHLY HALF-DONE BY
   VOLUME.** Cube maps are uploaded as six faces and bound into descriptor set 2;
   `CZ_VK_NO_CUBE=1` is the same-binary control arm. Three things are owed and they are in
   priority order:

   1. **THE OPERATOR'S VERDICT, and the reason is sharper than "ask a human".** In one
      serial block, four configs, same recipe and binary, p90 of the per-frame mean |RGB|:
      null (default vs default) **2.972**, real cubes vs white dummy **3.101**, second
      pairing **2.393**, and the magenta positive control **37.877 — 12.7x the null, no
      overlap.** So the instrument is emphatically NOT blind, and **binding real cube maps
      changes nothing measurable in the safehouse and prologue.** Two live explanations,
      both of which predict the effect lives OUTDOORS: this era's cube maps are themselves
      near-white (small DXT1 environment maps, interior scene), or the surfaces sampling
      them are not on screen indoors. The outdoor era is exactly what item 3 below cannot
      reach admissibly. Same spot, twice, `CZ_VK_NO_CUBE=1` versus default.
   2. **THE CUBE SNAPSHOT PATH** — six resolves into six layers for `06805000`.
      **55% of all cube sampling in the game's opening hour is that one map**, which the
      title renders itself and which is white in BOTH arms until this exists. By volume it
      is the larger half of this item.
   3. ~~**A HARNESS THAT CAN REACH AN ADMISSIBLE OUTDOOR FRAME.**~~ **THE ROUTE IS BUILT**
      (operator-supplied, implemented at the end of part 25): the title's own DebugJump
      screen, driven headlessly by `CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,...`
      with `CZ_DEBUG_MENU=1`. Lands Chuck by the military camp in a full crowd at **7,431
      draws**, against the ~1,800 ceiling that made every outdoor frame inadmissible. The
      recipe is in `CLAUDE.md`'s Commands section.
      **What is NOT yet done is the thing it was built for**: re-run this item's A/B on
      that route and QUOTE how many frames survive the
      `drawFingerprint`/`cameraFingerprint` filter. **Do NOT assume standing still keeps the
      camera matched — it does not.** Measured on the DebugJump route: 300 of 300 frames in
      a tail sample had DISTINCT camera fingerprints with no input at all (idle sway, crowd
      motion). Whether two arms match therefore depends on the engine being deterministic
      from the WAITJUMP anchor, which is untested. Measure that first — two runs of one
      config, count frames sharing a fingerprint — because if it is near zero, no amount of
      recipe work fixes it and the filter itself needs rethinking. A route existing is not a comparison
      being admissible, and until that count is quoted nothing outdoors has been compared.

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

1. ~~**CINEMATICS NEVER END**~~ **RETRACTED THE SAME NIGHT IT WAS WRITTEN.** Later in
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
1d. **The prologue-vs-later cinematic split may be a CLOCK problem, untested.** The two
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

2. **THE PROLOGUE — the search space is now much smaller** (see part 16 above). It is
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
3. **THE SHADOW CASCADE IS STILL HALF EMPTY, and part 15 halved the question.** The
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
3d. **NPC PART MESHES GO MISSING, DIFFERENT PARTS ON DIFFERENT CHARACTERS.** Dick
   renders as a head and one hand; Fausto has no legs; Gemini has no hair (her dark
   arms are GLOVES and correct). These characters are assembled from separate part
   meshes — the boot loads `childface`, `childhand`, `childupperbdy`, `childfullbody`
   as distinct files — so the thing to look for is what a missing part has in common
   with the other missing parts, not what is wrong with a given character. Hair in
   particular is normally its own alpha-tested material, which is a natural candidate
   for a shader or blend-state gap.
   **Distance matters**: Dick was INVISIBLE at range and became head-and-hand on
   approach, which is the same "approaching asks for a new resource and whatever we
   lack goes missing silently" signature as the white buildings (3b) and the black
   areas (item 0's shader misses).
   **UNRESOLVED WHETHER THIS IS A CACHE MISS.** The session that found it ran 351
   shaders while 370 were on disk, and every one of the 16 shaders it reported missing
   is now translated — so some of these parts may already be fixed. Re-test on a fresh
   launch BEFORE investigating: if the parts come back, it was the cache; if they do
   not, it is a real material/geometry defect and Gemini's correctly-rendered body is
   the control sitting next to it.
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
