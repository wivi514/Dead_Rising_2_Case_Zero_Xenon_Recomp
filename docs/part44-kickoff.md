# Part 44 kickoff — item 00i is a SET-APPLY defect: the full texture set loads and never reaches the descriptors

**FINAL REFRAME (end of part 43, after three operator corrections):** the flat
class exists in the MAIN MENU (56 tiny-on-big draws, twelve of them
`ps_34524bb64374d20e` reading an 8x8 at `0D875000`), where the zone texture-set
decision provably chose FULL and both set files provably loaded. **The decision
is exonerated as the cause; the defect is downstream: the decompressed set
payload never rewrites the material texture descriptors, which stay at their
8x8/16x16 creation state.** Everything below about the decision chain remains
valid reverse-engineering and the position analysis of R4/R3 remains the proof
that hardware holds full-size atlases — but part 44's hunt is the SET-APPLY
pipeline: NtReadFile extents for the set file -> the decompress pump
`sub_82177358` (probe calls + completion) -> the asset-type-3 completion
(`CallbackLoadRequest`, `sub_82269388`) -> whatever walks the set and rewrites
fetch descriptors, and where that silently dies on our runtime. §6bw fourth
addendum has the census; the B1 stream (menu frames, hardware) is the oracle
for the same draws if needed. Also open: the operator's sledgehammer-pickup
FREEZE (signal-15, not a fault — next session carries CZ_WAIT_TRACE=1).


Written at the end of part 43 (2026-08-14). **This is the LIVE hand-off**,
superseding `part43-kickoff.md`. Read `docs/phase5-notes.md` §6bw first — part
43 executed the part-43 plan's steps 1-2 and the answer INVERTED the item's
framing.

## What part 43 established (do not re-derive)

* **The zone texture-set decision is fully named and our execution of it is
  correct.** `sub_82270870` (sole call site `sub_82271550` ← `sub_82272128`)
  picks `COMMON_TEXTURE_LOD.tex` iff flag `rec+0x90C` (setup: full set
  `0 < size < 0x280000` AND a LOD file exists) AND every volume in the zone's
  list (`rec+0x910`: count +0x120, elems [+0x124], 0xD0 stride, sphere at
  +0x80, skip-bit u32 +0x90 bit0, threshold +0xA8) is farther than its
  threshold from the camera `[0x82A46294]+0x40`, thresholds boosted per level
  (`[g+0x34F5C]`, tables `0x82042C18`/`0x82042D68`; level 14 = no boost;
  force byte `0x82A57BD7` = LOD everywhere). Decision runs ONCE per zone load.
* **Verified live three ways**: `CZ_ZONE_TEX_PROBE=1` predicted part 42's
  narration line for line; `tools/zone_lod_live.py` reproduces the verdict on
  a live process; margins for zones 1/2/3/7 are +31..+107 m at the spawn
  (0/5/8 have near volumes → full). The LOD file IS the thumbnail set by
  design (zone 1: 27,734 B vs 1,297,584 B full). Zones 4/6/9 have 0-byte
  sets → flag 0 → the narration silence part 42 saw.
* **The ordering hypothesis is dead**: from the menu camera or the origin,
  MORE zones go LOD, not fewer. No load-time camera position outside town
  produces hardware's all-full street.
* **The R4 traces cannot adjudicate item 00i**: they are a standing sweep AT
  Big Buck after a walk — a warm session. Fresh jump vs warm walk are
  unmatched arms.
* **No promotion trigger found**: statically, nothing re-writes state 0 into
  the zone table at `this+0x841C` (entry+0 state, entry+4 has-set flag);
  dynamically, a 9.5-min EXPLORER roam re-decided nothing — but the roam
  never crossed a LOD threshold (~60 m pocket; zone 7's nearest volumes are
  west of spawn at x≈−180..−206, 31-47 m past threshold), so "no promotion"
  is UNPROVEN (gotcha 151). A no-AutoChuck DebugJump run does not move at all.
* **Capture R5 is filed** (`docs/xenia-capture-requests.md`): one fresh
  DebugJump stand-still F4 + PNG at the spawn, optionally a second mid-walk.
  It decides "engine design (matched states show the same flat street)" vs
  "real input divergence (skip bits → volume data → unfound reload path, in
  that order)". **Build no fix before it lands** — every candidate is
  branch-faking (gotcha 5).

## The plan

0. **SAME-DAY ADDENDUM — read §6bw's addendum first: R4/R3 already answered
   most of R5.** Hardware's camera positions were recovered from the traces;
   at all ten, zones 1/2/3/7 are all-far (+21..+136 m) yet render full —
   hardware's state is NOT a fresh batch decision. A DOWN,DOWN jump (spawn
   -271,-64) reshuffled our verdicts exactly with position (zone 1 FULL,
   5/8 LOD): the mechanism is proven end to end on our side. **The blocking
   unknown is now one operator sentence: how did the R4 session enter the
   level (DebugJump-then-walk, or normal play)?** DebugJump → the zone
   RELOAD-on-approach trigger exists on hardware and is dead/untriggered on
   ours (the hunt target); normal play → both faithful, flat-at-range is the
   batch-load state. Also note: synthetic LSUP does not move Chuck at the
   jump spawn (only a real pad does) — the directed-approach test needs an
   operator or a fixed input path. New operator report, filed not chased:
   AO only visible close to objects (§6bw addendum; plausibly the far-LOD
   mesh switch, may ride along with 00i's fix).

1. **If R5 has landed: read it first.** Flat spawn street → close 00i as
   state-comparison artifact, note the design, and tell the operator how to
   compare like states. Full spawn street → chase the ordered suspect list
   with the same probe pair (skip bits are one live read per volume; the
   volume data can be diffed against the trace's own memory records).
2. **The 0u residues (parked since part 42's kickoff step 5)**:
   * serve the DoF gather's fmt6 depth-as-8888-bytes fetch a packed byte view
     (D32F → RGBA8 24_8 at snapshot time, own arm + counter). Edge-weight
     correctness; predicted visible effect small.
   * the gather's pc255.x (taps' depth threshold): ours is 0, hardware
     unrecoverable (loads from CPU-written `032B6000`). If the byte view
     lands, re-derive what the shader expects and whether 0 is even wrong.
3. **Optional, if an operator session happens anyway**: have them walk from
   the fresh spawn INTO zone 7 (west, ~50 m) on OUR renderer with
   `CZ_ZONE_TEX_PROBE=1` and `tools/zone_lod_watch.py` sampling — the
   directed-approach test the headless AI cannot do. A re-decision appearing
   → promotion exists and works; none → the once-only reading is confirmed
   live on ours too.
4. Parked from part 41: A2M dither at distance (item 4), clamp modes / cyan
   fringes (item 5).

## Standing state

* Cache 435, dim gate clean. Part-43 artifacts: `/tmp/part43_probe2.log`
  (canonical stand-still + probe), `/tmp/part43_walk.log` (EXPLORER roam),
  `/tmp/part43_watch.txt`/`watch2.txt` (live samplers) — all tmpfs, copy out
  if they matter. The part-42 sets under `~/DR2CZ-troubleshooting/part42/`.
* New instruments: `CZ_ZONE_TEX_PROBE=1` (prints every decision input +
  prediction at each `sub_82270870` entry), `tools/zone_lod_live.py` (live
  re-evaluation), `tools/zone_lod_watch.py` (periodic sampler: position, zone
  states, would-be verdicts).
* Probe caveat paid for in part 43: instrument logs can contain NULs
  (a `%s` on a non-string); `grep` then reports the whole file binary-absent.
  `grep -a` / `tr -d '\0'` first (gotcha 25's self-made form).
