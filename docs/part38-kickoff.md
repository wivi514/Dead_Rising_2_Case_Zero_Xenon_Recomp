# Part 38 hand-off (for part 38). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part37-kickoff.md` for "where
the port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays cinematics. Part 37 solved the
top picture item: the striped-material blotches (item 0s — the tanker, Dick's far LOD,
the pawnshop boards) were the runtime's own 16-bit texcoord unswizzle mask correcting
a transposition the shader's own destination swizzle already corrects, so baked-lightmap
UVs arrived (V,U) and the lightmap's black prop shadows painted the surfaces. The mask
now defaults to zero; the fix is committed, gated, and confirmed headlessly at the
reproduced defect site — which turned out to be the Case 0-2 DebugJump spawn itself.

## WHAT PART 37 DID — do not rebuild any of this

Records: `phase5-notes.md` §6bo, gotchas 291-292, `port-history.md` part 37.
Evidence: `~/DR2CZ-troubleshooting/part37-headless/` (indexed in its INDEX.md).

* **The named draw**: tanker = verts 5896, vs_fa161b0fde7aa4d5 / ps_c3ae0ec7855c4a18,
  s0=109FC000 (1024x1024 skin, md5 = hardware's 14790000), s1=109CC000 (the LIGHTMAP,
  md5 = hardware's 14760000). Streaming addresses held identical across FOUR boots now.
* **The fix**: `g_SwappedTexcoords` published as 0 (vk_renderer.cpp; the long comment
  there is the state-chain argument). `CZ_VK_TEXCOORD_SWAP=1` = the old mask, the
  same-binary control arm that repaints the blotch. `CZ_VK_NO_TEXCOORD_SWAP` = no-op.
* **Retraction applied**: §6bk's "body/cab draws" were street clutter (gotcha 291 —
  identify draws by CONTENT, never by vertex count, especially across platforms).
* **Method that worked**: decode the hardware frame's textures and LOOK; md5 content
  into our dumps for the address; the census names the draw. `tools/tex_decode.py` +
  `xtr_draw_bindings.py --dump-texture` + the F9 texdumps.
* **Headless defect loop**: DebugJump recipe + F9 in `CZ_FAKE_PRESS_SEQ` + an extra
  `A` to dismiss the Case File card + RSRIGHT sweep = reproduce, A/B and confirm any
  defect at the spawn area with no operator. Recipe in `port-history.md` / run
  commands in `~/DR2CZ-troubleshooting/part37-headless/INDEX.md`.
* **Gates on the new binary**: --smoke OK; A5 exit 0 (3 permutation, 0 real); E2
  identity +0.9594 (standing +0.9597); tanker clean on two boots of the new default.

## WHERE TO START

1. **The class-closure tour (cheap, high value).** The mechanism predicts Dick's
   far-LOD stripes and the pawnshop window boards are the same lightmap-UV defect.
   One operator session (or headless if reachable): visit both on the new default,
   F9 each. If either still stripes, item 0s has a second mechanism — the remaining
   sub-defects listed below become live again. Ask the operator; they turn these
   around same-night.
2. **Item 0s residue, now unmasked**: (a) hardware's 16 small colour resolves — NOTE:
   part 37 observed our F9 snap dumps at the tanker site DO contain the same extent
   classes (64x64 x27, 128x64 x4, 128x128, 512x256), which appears to contradict
   part 36's "in no resolve set of ours" — re-derive that claim before building
   resolve write-back on its strength; (b) the 231 colour fetches served by a DEPTH
   snapshot; (c) the billboard-sheet quality-level question (§6bj — sheets correct to
   the byte).
3. **The Xenia one-look for item 00i** (flat-panel shop promotion distance) — owed
   since part 35, one deliberate look, operator-served.
4. **The teleport, if still wanted** (it is no longer needed for item 0s — the spawn
   IS the tanker site). Part 37 advanced it: `cMissionTeleportPlayer`'s vtable follows
   the `missionteleportplayer.cpp` string at 0x8204C868 (floats, then 15 ptrs); its
   trigger is **0x823A8CD8**, which builds a stack event record {vtable 0x8200B300,
   +4=0, +8=**0x6A**, +0xC=byte 0, +0x10=4, 0, 0}, fills it from the asset via
   0x8248BE28, and posts it with **0x82188488**(mgr->0x78->0x70, &event, file, 0x61).
   The 0x6A consumer was not found by immediate-scan (it is dispatched virtually);
   next step is following 0x82188488's listener walk, or hooking it live at a real
   DebugJump spawn with CZ_ARG_PROBE to catch the placement call in the act.
5. `docs/perf-cpu-plan.md`'s CPU/GPU overlap — still the largest performance item.

## READ THIS BEFORE MEASURING ANYTHING

* Everything from parts 26-36 stands, plus gotchas 291-292.
* **Whole-frame statistics cannot see a localized material defect** — §6n's null was
  measured honestly and hid this for four parts. The admissible comparison is the
  matched-index F9 crop at a reproduced site.
* **Two runs' era medians are not comparable when their camera paths diverge**
  (gotcha 254 still applies headlessly — crowd RNG steers Chuck differently per boot).
* The F9-behind-a-menu trap: a capture fired while the Case File card is up censuses
  a PAUSED world behind the card; the pictures show the card. Dismiss with A first.
