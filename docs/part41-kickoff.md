# Part 41 kickoff — THE DISTANCE PLAN: everything that makes the far field ugly, ranked, with the evidence and the method for each

Written at the end of part 40 (2026-08-13), at the operator's request: *"focus on
fixing everything issue with distance making them ugly"*. This is the working plan for
a fresh conversation. Read `docs/phase5-notes.md` §6bs and gotchas 306-308 first if the
part-40 alpha-test work is not already familiar — three of its fixes are stacked under
everything below.

---

## 0. Where part 40 left the picture

Three commits transformed the near and mid field, all mechanism-derived:

1. **`kRbColorControl` = 0x2202** (was 0x2205 = RB_BLENDCONTROL1). The alpha test —
   12.2% of all hardware draws — fires at all now. Foliage, fences, hair, backdrop
   sheets. (ed46db7)
2. **Strict GREATER**: threshold published as `ref + 1/512`, because the foliage cuts
   out OPAQUE cards with GREATER at ref = 0.0 and the emitted clip is `>=`-shaped.
   (aa33e8e)
3. **EQUAL@1.0 emulated exactly** (`ref - 1/512`): the shadow-caster flavor of the
   cutout — 174 caster draws — which was stamping solid cards into the shadow atlas
   and blacking out canopy interiors. (same commit)

The operator's verdict after fix 2 (fix 3 landed after their session): near trees
massively improved, small orange trees improved, **the far field is now the complaint**.
A last tree capture (`verify/capture_002863`) is unadjudicated — the operator said "the
tree might be ok"; look at it before spending anything more on trees.

**The evidence bank for this part is already collected:**

* **The 81-capture operator walk** — `~/DR2CZ-troubleshooting/part40-operator/verify/`:
  F9 every ~1 m down the whole main street on the strict-GREATER build. Every capture
  has the picture, the pose (player world position), and the per-draw census — which
  since part 40 carries **`cc=` (RB_COLORCONTROL) and `ar=` (RB_ALPHA_REF) on every
  draw line**. This is the largest self-consistent picture dataset the port has.
* **The eight R4 hardware traces + frame-locked PNGs** (`Xenia logs/R4_world/`) at the
  Big Buck store — the walk's west end. Regenerate any CSV with the FIXED
  `tools/xtr_draw_bindings.py` (it read the wrong register until part 40; part-39 CSVs
  are void for colorControl).
* The headless treecam viewpoint (trees + long street view, no operator):
  `F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,RSLEFT,NONE,RSLEFT,NONE,F9,NONE,NONE`
  with `CZ_DEBUG_MENU=1 CZ_CAPTURE_KEY=<dir>`.

---

## 1. ANISOTROPIC FILTERING IS OFF — the cheapest fix and probably the biggest

`vk_renderer.cpp` creates exactly two samplers (linear, point); `anisotropyEnable` is
never set. A road surface viewed at a grazing angle under trilinear-only filtering
turns to mush well before the horizon — which is precisely a "distance looks ugly on
everything" symptom, and it costs the whole ground plane, not one material. Xenos
applies up to 16:1 aniso and **the fetch constant carries the aniso field per texture**
(dword3: mag:2 min:2 mip:2 aniso:3 — the layout comment sits at the decode site,
`vk_renderer.cpp:2578`).

Plan:
* Step 1a: turn aniso on globally (`si.maxAnisotropy = 8 or 16`, check
  `samplerAnisotropy` device feature is enabled at device creation — if not, enable
  it). One-line experiment, era-median A/B on the walk route plus one operator look.
  `CZ_VK_NO_ANISO=1` as the same-binary arm.
* Step 1b (correctness): honor the per-fetch filter fields. The code marks this as a
  stated simplification ("Two samplers, and one global choice per draw"). Build a small
  sampler cache keyed on (mag, min, mip, aniso, address modes) decoded from the fetch
  constant. The census should print the decoded filter fields so a wrong decode is
  visible against hardware's (`xtr_draw_bindings` can carry the same fields —
  two-sided check like the dim sidecars, gotcha 244's method).
* Gotcha to respect: the fetch constant's field layout must be READ, not assumed —
  partition on an independent answer where possible (the dim census precedent).

## 2. THE PACKED MIP TAIL — the deepest mips, still declined since part 39

Part 39 uploads each texture's mip chain but **stops at the packed tail** ("mip: PACKED
TAIL REACHED" counter). The tail is where the smallest levels live — i.e. exactly what a
DISTANT surface samples. With the tail missing, the sampler clamps at the last uploaded
level; the far field samples a level far too detailed for its footprint → shimmer and
noise at range (with aniso off, additionally mush — do item 1 first so the two don't
confound).

Also in this bucket: **254 chains are guard-REJECTED entirely** (part 39's divergence
guard, gotcha 301) and sample level 0 only. The rejection rule's limit is known; with
the tail layout implemented the offset rule may generalize and reclaim some of the 254.

Plan:
* Derive the packed-tail layout EMPIRICALLY, the way part 39 verified the ordinary
  chain: hardware's R4 traces carry `packedMips=1` on most fetches together with the
  full guest bytes; decode candidate sub-level offsets inside the tail tile and accept
  the layout that produces coherent images level after level (mean-luma continuity;
  remember gotcha 298 — "distinct colours" REWARDS aliasing, do not score with it).
  The classic 360 layout packs all levels ≤ one tile into a single tile at fixed
  block offsets; do not trust a remembered table — verify per format (DXT1 8-byte vs
  DXT5 16-byte blocks at least).
* Wire it into `UploadTexture`'s existing chain walk (the termination condition
  becomes "packed tail decoded" instead of "packed tail declined"), keep the
  per-level divergence guard, keep `CZ_VK_NO_MIPS=1` as the whole-feature arm.
* Gate: the part-39 registered-prediction discipline — era medians against the R4
  frames' statistics, expectations stated BEFORE the run.

## 3. ITEM 00i — the flat-panel streaming pop, now fully pairable

The one distance defect that is definitely NOT filtering: a building face renders as
flat colour panels at range and pops to full siding on approach. Level-0 bytes were
md5-exonerated (part 39), `mip_min_level` is 0 on all 328k hardware fetches, so the
suspect is WHICH quality level the streaming system has resident, or which texture the
draw binds at range.

The 81-capture walk makes this measurable for the first time:
* Find a capture pair where the same building face is flat in one frame and detailed
  in a later frame (the operator walked at ~1 m spacing — the pop is bracketed to a
  meter). The pose files give the distances.
* In the flat frame's census, identify the face's draw (use `CZ_VK_DRAW_ID=1` at the
  reproduced spot if pointing is needed — F9 works headlessly, and DebugJump + camera
  holds reach the street) and read its s0 extent/address; do the same in the detailed
  frame. If the flat frame binds a genuinely tiny/flat texture, the question becomes
  "when does hardware swap" — find the same building in the R4 frames (they are all
  near the west end) and read what extent hardware binds at ITS distance.
* Depending on the answer this is (a) our file-IO latency starving the streamer,
  (b) a priority/budget input we feed wrongly (part 28 found `KeSetBasePriorityThread`
  is a no-op — the one candidate fix, deliberately not built then), or (c) the title's
  own behaviour, identical on hardware — in which case CLOSE the item with the
  evidence.

## 4. ALPHA-TO-MASK AT DISTANCE — the foliage's far-field softness

Hardware sets A2M (bit 4) alongside the alpha test on the foliage (473 draws across
R4). On 4x MSAA hardware that FEATHERS the cutout — partial alphas dither across
samples — which matters most at DISTANCE, where leaf texels are minified into partial
coverage. Our single-sampled emulation is a hard 1/512 threshold: distant foliage will
look harsher/sparser than hardware's, and minified leaf alpha that averages below the
threshold DISAPPEARS — thin canopies at range.

Plan (only after items 1-2, which change the same pixels):
* Cheap approximation with a principled basis: for draws with A2M set, emulate
  alpha-to-coverage's expected value with an ordered-dither threshold
  (screen-position-hashed threshold in [0,1) instead of the fixed 1/512) — one small
  change in how the runtime publishes `g_AlphaThreshold`... no: the threshold is
  uniform per draw. A per-pixel dither needs a shader-side term — that is a
  XenosRecomp change (a `SPEC_CONSTANT_ALPHA_TO_MASK` variant of the clip using a
  hash of `iPos.xy`). Fable 2 inherits it for free; UnleashedRecomp has the same
  concept (`SPEC_CONSTANT_ALPHA_TO_COVERAGE`) as structural reference only (GPLv3).
* Measure: era medians on foliage-heavy frames plus the operator's look at a distant
  tree line. Prediction to register: distant canopies get FULLER, not thinner.

## 5. SMALLER NAMED SUSPECTS, in checking order

* **Cyan/teal edge fringes** on some mid-distance building edges (visible in walk
  captures 008854/008962 left edge). Likely DXT1 punch-through decoding or a
  border/clamp artifact under REPEAT addressing on atlas edges — per-fetch address
  modes are currently ignored (item 1b covers the fix; verify it covers this).
* **EQUAL at refs below 1.0**: none observed in R4; counter exists. If a future
  census shows them at distance-relevant draws, they need the two-clip form.
* **The exposure trace beside an operator F9** (§6ba's "closed pending a matched
  location") — the far field's washed look could be partly exposure; the instrument
  (`CZ_VK_EXPOSURE_TRACE`) and the method are already built. One operator F9 at
  `w1_spawn` with the trace on completes it.
* **Fog/haze constants**: not yet compared against hardware at any draw. The foliage
  shader's fog block (pc(18)/pc(19)) was read in part 40; a distance complaint that
  survives items 1-4 should compare the fog constants at one far draw against the R4
  trace (`tools/xtr_draw_constants.py`).

## 6. Discipline reminders for this part

* **One change per experiment**; era medians with three runs per arm for anything
  quantitative (`tools/frame_era_medians.py`; the noise floor is 10-13% at one run a
  side). The walk route's matched-frame A/B is unsatisfiable (gotcha 254).
* Every new emulation gets a same-binary OFF arm and a counter, and the counter gets
  READ (gotcha 308's lesson: part 38's alpha test counter said zero for two parts).
* Point `CZ_SHADER_DUMP` at `~/DR2CZ-troubleshooting/ucode-dumps` on any run that
  reaches new ground; never under /tmp.
* An operator report outranks a headless metric; launch the game for them with
  instruments wired (their preference, recorded in memory).
* The R4 CSVs must come from the POST-part-40 `xtr_draw_bindings.py`.
