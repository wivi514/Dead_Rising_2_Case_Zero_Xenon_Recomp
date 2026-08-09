# Open items, in order

**Split out of `CLAUDE.md` on 2026-08-08.** The actionable backlog. Items struck through
are closed or retracted and are kept because a retraction is worth as much as a finding
— several of these were "fixed" by something else entirely, and the record of what was
NOT the cause is what stops the next session re-buying it.

Next, in order:

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

0a-i. **`CopySwapped` is vectorised, but as a 10-instruction SSE2 sequence rather than one
   `pshufb`** — `-msse4.1 -mavx` is applied to the `ppc_image` target only, so
   `runtime/gpu/vk_renderer.cpp` compiles at baseline x86-64. Noticed while measuring 0a
   and **most of its value was then taken away by 0a itself**: stream copying is now
   0.23 MB a frame, so the remaining beneficiary is TEXTURE upload (`CopySwapped` at
   `vk_renderer.cpp:2232`/`2255`). That is not nothing — with `streams` at zero,
   **`textures` is now the second-largest draw-path term at 5.8-7.0% of a crowd frame
   (2.8-3.4 ms), behind only `record`** — so a five-line change
   (`__attribute__((target("ssse3")))` on that one function, keeping the rest of the
   binary at baseline) is plausibly worth ~1 ms. Measure it against `textures`, not the
   frame, and remember gotcha 237 before expecting the frame to move.
   The *reason* its value moved is the part worth carrying: sizing a micro-optimisation
   before the structural change lands prices it against the wrong baseline — this one was
   worth multiple milliseconds an hour earlier, then almost nothing, and is now worth
   about a millisecond again for a completely different reason.

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

