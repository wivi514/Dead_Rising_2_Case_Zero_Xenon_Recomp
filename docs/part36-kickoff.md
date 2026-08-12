# Part 36 hand-off (for part 37). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part35-kickoff.md` for "where
the port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays cinematics; shadows fixed
(part 34), white surfaces fixed (part 33). Part 36 ran the R3-oracle comparison that
part 35's kickoff ordered as step one, and **item 0s's framing did not survive it**:
the "junk impostor sheets" are correct TO THE BYTE (md5-identical to what hardware's
GPU sampled at the same site), and decoded properly they are coherent billboard
alpha-cutouts the junk-scorer was guaranteed to misflag. The writer hunt is closed
before it started; the item is reframed as a WRONG-BINDING question (a real asset at
the wrong streamed quality level). `docs/phase5-notes.md` §6bj; gotchas 287-288.

## WHAT PART 36 DID — do not re-derive any of this

* **The kickoff's "3 DXT5, 0 DXN in tanker.xtr" concern is settled**: the full census
  is 3,514 DXT5 / 3,040 DXN fetches (112 / 53 distinct), including every odd-extent
  sheet (512x240, 400x240, 360x160, 1024x64, 88x88) on 4..8-vert quads. Hardware
  draws the sheet class; the "LOD tier we linger in" branch is dead.
* **Our sheets == hardware's sheets, byte for byte**: 400x240 (ours 036DA000 = hw
  0746E000) and 1024x64 (036FB000 = 0748F000), md5-identical despite different
  streaming addresses. "Guest memory holds garbage" is RETRACTED for these; there is
  no writer defect for them. `Xenia logs/R3_world/` bytes vs
  `~/DR2CZ-troubleshooting/part35-item1-operator/tanker_blotch_f43675/`.
* **`tools/tex_decode.py` is new** — DXT1/DXT5, tiled/linear, swap16, alpha plane.
  The 400x240 sheet decodes to a foliage cutout (content in ALPHA, colour white);
  part 35's "weird" 110AD000 decodes to structured white slats/boards — real content,
  not noise, but absent from hardware's frame (728 textures searched by prefix).
* **Content-match census**: 226 of 459 textures our blotch frame bound are
  byte-identical to hardware's frame. The 233 others are UNADJUDICATED, not suspects
  (different camera/streaming state; render targets can never match) — gotcha 288.
* **Guest narration is exhausted for this item**: outdoor route with
  `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` gives 1,209 [guest] lines, zero mentioning
  impostor/billboard/composite.

## THE OPERATOR SESSION LANDED THE PAIR — read §6bk before step 1

`~/DR2CZ-troubleshooting/part36-operator/`: 13 F9 captures, 9 with live texture
dumps, one boot. **`capture_002048` is the tanker CLOSE and blotched (dark olive,
hard-edged patches); `capture_005614` is the SAME tanker at street distance rendering
the correct cream skin** — both states of one asset, with censuses and dumps. Also
established there: the close-up's textures are md5-identical to hardware's for ~45 of
~50 pairable draws (including the body/cab albedo), the shadow atlas is 0.0003% zero
in the blotched frame, and only 22 hardware textures above 32 KB are absent from our
frame (none a tanker skin). **Do not pair draws by vertex count across the two frames**
— it yields 115 false differences because crowd actors share meshes (§6bk).

## WHERE TO START

1. **Item 0s, reframed — name the blotched draw, with `CZ_VK_ONLY_TEX`.** The arm
   already exists: `CZ_VK_ONLY_TEX=<addr>` renders only draws binding that texture,
   `CZ_VK_SKIP_TEX` the complement — pick candidates from `capture_f2048.census` and
   let the picture identify the blotched surface. Both are read once per process, so
   the arm needs a fresh launch; **first check whether two runs of the headless
   DebugJump recipe give the same census addresses**, because if streaming addresses
   move between boots the filter must become runtime-readable (a file or a key) to be
   usable in the boot that shows the defect. Once the draw is named, decode its s0
   with `tex_decode.py` and compare against hardware's tanker-body draw in tanker.xtr
   (draw 4184, verts=18193, s0=11995000 512x512 DXT1). Two outcomes: its content
   differs from hardware's for the same surface → find the `.big` read that filled it
   (CZ_FILE_TRACE + address); or it samples a DIFFERENT texture → the material system
   picked a wrong quality slot, which is where "one level per asset per boot" points.
2. **The resolve write-back lead**: hardware's tanker frame issues 16 small colour
   resolves (64x64 x9, 128x64 x4, 128x128, 512x256) that are NOT in our 61-entry
   resolve census. Check whether our PM4 stream carries them on the outdoor route
   (CZ_RING_TRACE / resolve census instrument). If the title issues them and we
   never write resolve pixels back to guest memory, that arrow still has victims —
   possibly the 231 colour-fetches-from-DEPTH-snapshot sub-defect.
3. **The Xenia one-look for 00i** (promotion distance of the flat-panel shop,
   reload_test 30631 spot) — still owed, one deliberate look.
4. The rest of `docs/open-items.md` and `docs/perf-cpu-plan.md`'s CPU/GPU overlap.

## Gates, on this binary

* No runtime/renderer change in part 36 (analysis + docs + one new tool only), so
  part 34's gates stand as last run; A5 diff still owed a re-run before any claim
  resting on it.
* Shader cache: 417; the part-36 outdoor run bound nothing new
  (`no translated shader` = 0).
