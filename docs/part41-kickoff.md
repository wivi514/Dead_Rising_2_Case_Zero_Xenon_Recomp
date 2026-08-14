# Part 41 kickoff — after the register fix: verify, then the distance items

Part 40 (2026-08-13) found and fixed the largest picture defect since the NaN plateau:
**`kRbColorControl` was 0x2205 (which is RB_BLENDCONTROL1); RB_COLORCONTROL is 0x2202.**
Read `docs/phase5-notes.md` §6bs first — it is the full chain — then `docs/gotchas.md`
306-308 and the part-40 blocks in `docs/open-items.md` 0t. The wrong index had silenced
part 38's alpha test entirely (its counter read zero in every log, unread) and had
part 39 "refute" the correct explanation by censusing the wrong register.

## What already exists — do not rewrite

* **The fix is committed and on by default** (ed46db7). `CZ_VK_NO_ALPHA_TEST=1` is the
  same-binary control arm (the parts-38..39 renderer).
* **The headless tree viewpoint**: DebugJump Case 0-2 spawn + two `RSLEFT` holds +
  synthetic `F9` — `F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,RSLEFT,NONE,RSLEFT,NONE,F9,NONE,NONE`.
  F9 works inside `CZ_FAKE_PRESS_SEQ` (host key 9); the census is armed for the frame
  AFTER the press, so keep entries after the F9.
* **`CZ_VK_TEX_DUMP` handles DXT now**, and `CZ_VK_TEX_DUMP_PS=<ps hash>` dumps a
  material's textures keyed by shader — the handle that survives a reboot where a guest
  address does not (gotcha 306; the address form returned BARBED WIRE for part 39's
  foliage addresses).
* **Hardware alpha-test census** (all eight R4 traces, correct register): 4,975 of
  40,703 draws (12.2%) enabled; funcs GREATER 3,043 / EQUAL 1,400 / GEQUAL 532; A2M 473,
  always alongside the test. Top shaders under it: the caster `34524bb64374d20e`
  (1,787), foliage `69a5c3be9359b87c`/`790283523afcaf20`, sheets `8d88657855b704b3`.

## The backlog, in order

1. **Operator verdict on the trees and the distance look** (the tour is item 0t's
   close). Places: the main-street trees from the part-39 walk, the safehouse junkyard,
   the Big Buck approach (R4's PNGs are the oracle for all three). Expect fences, wires,
   hair and the horizon sheets to have changed too — all under the test on hardware.
2. **Item 00i — the flat-panel streaming pop — is NOT closed by part 40.** The R4
   pairing plan in `open-items.md` still stands. Note the alpha-test fix DID change the
   backdrop sheets at street ends; re-look before assuming the old reports still
   reproduce as described.
3. **Func EQUAL** (1,400 hardware draws; ref=1.0, blended — the two-pass cutout's core
   redraw). Un-emulated, counted. Needs a XenosRecomp change if ever built (the emitted
   clip is GEQUAL-shaped); measure the visual cost first at a two-pass site.
4. **The packed mip tail** (part 39's leftover): still declined and counted. Distant
   minification lives there; 254 guard-rejected chains also still sample level 0.
5. **The guard-cost frame-time A/B** owed since part 38, and part 39's stale
   `drawid_read.py` docstring ("the frame after the captured one" — it is the same
   frame now).

## Warnings for this part

* The bias32/census runs from part 40's middle (`part40-bias/`) were taken while the
  binary was being relinked mid-run — treat their outputs as void.
* When comparing arms at the treecam viewpoint, the F9 lands on slightly different
  cameras per run (the AI crowd shifts timing). Fine for "are the plates gone", not for
  pixel diffs — era medians or matched still-scenery crops only.
* Part 39's R4 CSVs in scratchpads carried colorControl=0x2205 values; regenerate with
  the fixed tool before quoting anything from them.
