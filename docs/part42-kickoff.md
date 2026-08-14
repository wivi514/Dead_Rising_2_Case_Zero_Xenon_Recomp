# Part 42 kickoff — THE FLAT-TEXTURE CLASS FIRST (the operator's actual complaint), then the DoF composite

**RE-RANKED at the end of part 41 session 2, on the operator's own correction**: *"I
did not say the ground looked bad I said everything except the ground looks bad"* —
and what they mean is FLAT TEXTURES on buildings/objects at range (item 00i), not
softness. The DoF work below is real and stays, but it is SECOND. Do not conflate
the two again: a blurred brick wall still shows brick; the complaint class shows NO
PATTERN AT ALL (capture_003053's two-story building, capture_003368's slab, in
`~/DR2CZ-troubleshooting/part41-operator/default/`).

## ITEM 00i FIRST — the flat-texture pairing, now with the pointing instrument

What part 41 session 2 established for this item:
* The 20-capture censuses carry tiny textures (8x8, 16x16) bound to world draws
  at range (e.g. 0ED9A000:8x8, 0F9BA000:16x16) — candidates for the flat look.
* Census-only draw correspondence across the walk FAILED (619 keys alias across
  distinct objects; the multiset tracker cannot isolate one building face).
* Therefore: use `CZ_VK_DRAW_ID=1` + `CZ_CAPTURE_KEY` + F9 headlessly at a
  reproduced flat-building view (DebugJump spawn IS on the walked street; camera
  holds reach the 003053 building). The draw-ID map names the flat face's draw;
  its census row names the bound texture and extent.
* Then the two-sided question: what does HARDWARE bind for the same face at the
  same distance (R4 CSVs, `--min-verts 0`)? Bigger extent -> OUR streaming
  promotion is late (candidates: file-IO latency; the KeSetBasePriorityThread
  no-op part 28 deliberately left unbuilt) -> fix and A/B. Same extent -> the
  title's design, and the real divergence is elsewhere (mesh LOD selection?).
* Warning for the comparison: the DoF blur (below) sits ON TOP of these pixels
  in the presented frame. Judge flatness on the SCENE snapshot (1439B000), which
  is pre-post-chain — or the comparison measures the blur, not the binding.

## SECOND: the DoF composite (the far-field softness)

Written at the end of part 41 session 2 (2026-08-14). **This is the LIVE hand-off**,
superseding `part41-kickoff.md` on "what to do next" (that plan's items 3-5 are
still open but PARKED behind this: item 0u changes the same pixels as all of them).

Read `docs/phase5-notes.md` §6bt (samplers + packed tail, both default and
confirmed) and §6bu (this defect's anatomy and the retraction) first.

## What is established, with the evidence

1. **The scene surface is SHARP at every distance** — the operator's F9 snapshots
   prove it (`1439B000` in any capture of
   `~/DR2CZ-troubleshooting/part41-operator/default/`). The far-field complaint is
   manufactured entirely by the DoF composite: `oC0 = lerp(scene, blur640, blur.a)`
   (draw 6813, `ps_8375f611aba84bc4`).
2. **Our prepass CoC saturates at distance**: `CoC = saturate(viewZ/50)`,
   `viewZ = 1/(10-9.999*z)`, our z = real scene depth 0.83..1.0 → CoC 1.0 by ~50 m.
   Hardware, same constants (stable across R4 01/04/08), same D24S8 unorm domain,
   identity viewport — yet its 40-60 m storefront is legible. **A compensating term
   on hardware is UNLOCATED. Do not ship a fix before naming it** — the three
   candidates, in checking order, are at the end of §6bu.
3. **One of our two depth servings is wrong for sure**: the gather
   (`ps_166bbb9722e9c3ca`) reads the depth surface DECLARED AS 8_8_8_8 (byte-split
   trick, 8 taps, `.z` channel) and we serve the float-depth image — the part-36
   "231 colour fetches served by a DEPTH resolve snapshot" class. Whatever the
   contradiction's answer, this serving must eventually return the packed BYTES.

## The plan

* Step 0 (one tool run each): hardware's gather/composite constants —
  `xtr_draw_constants.py --ps 166bbb9722e9c3ca --regs 48,82,96,97,98,99,100,101,252,253,254,255`
  and `--ps 8375f611aba84bc4`. If pc82.x or pc48.w is 0 on hardware, the blur is
  gated off there and the question becomes what OUR values are.
* Step 1: read OUR constants at the same draws. Cheapest instrument: extend the
  census (CZ_CAPTURE_KEY already prints pc255 per draw) to print a named register
  list per draw — a dozen lines in vk_renderer.cpp.
* Step 2: whichever term differs, trace it back (the guest computes these from
  its own state; `import_call_sites.py` / `gdis.py` if it smells like an HLE input).
* Step 3: implement the fmt6-over-depth byte view (a small conversion at snapshot
  time: D32F -> RGBA8 packed 24_8 bytes, refreshed per resolve like the cube
  faces), with its own arm + counter. This is required regardless of step 0-2's
  answer.
* Step 4: era A/B + operator look. The prediction to register: mid-range (20-80 m)
  sharpness on the walk route rises to match the scene snapshot's own detail;
  hardware-matching far field.

## Standing state

* Part 41 defaults: per-fetch samplers (d5b8fdc) + packed mip tail (409777d),
  arms `CZ_VK_NO_FETCH_SAMPLERS` / `CZ_VK_NO_MIP_TAIL` / `CZ_VK_ANISO=N`.
  Confirmed: sharpness +2.6% no-overlap; tree cutouts hold; tail TAKEN ~4,468/run.
* `xenos.h` kRbDepthInfo comment is WRONG (bit16 is D24S8 vs D24FS8, not 16-bit
  vs 24_8) — harmless today (we treat all depth as D32F), fix when touching it.
* Part-41 items still open: 00i pairing (the 81-capture walk + 20 new captures),
  A2M dither (item 4 — the orange tree's shards at range), clamp modes / cyan
  fringes (item 5 + 1b's clamp half). All parked behind item 0u.
* The gotcha ledger gained 309-311 this part (global-resource semantics; a metric
  mislabeling a defect; and §6bu's second gotcha-280 disguise is folded into 311).
