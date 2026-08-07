# D3D phase C, part 13 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §6aa** first — it is the
record of the session this hands off from, and §6z now carries a banner pointing at it.
`docs/d3d-translation-plan.md`'s **"Phase C part 12"** is the same story in one page.
`docs/d3d-phase-c12-kickoff.md` is the previous hand-off: its **items 1 and 4 are
closed**, items 2, 3 and 5 are unchanged and repeated below.

## What part 12 changed, in one paragraph

Part 12's job was the new-game screen's three black panels and its malformed label text,
newly reachable headless via `CZ_FAKE_PRESS_SEQ=START,A,A`. Both are now localised to a
named object by arms rather than by reading, and **the shape part 11 predicted is
refuted**: nothing on that screen samples a surface the renderer failed to produce. The
black rectangles are ONE texture drawn on top of three thumbnails that render correctly;
the malformed text is one of two glyph atlases, and the atlas itself is neither stale nor
mis-untiled. Part 11's item 4 (the walker's dead ISR replication and
`MirrorIsPoisoned()`) is confirmed zero on a correctly configured draw arm and deleted.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`, one 120 s boot:

* `--smoke` OK. A1: exact 84-prefix. A5: exit 0, 2 windows, **0 real**.
* `truncated=0`; deepest file **#83 `game:\data\skeleton\cinezombie.big`**.

Phase C draw arm, **`CZ_D3D_DRAW=1` ALONE** (see the trap below), one 170 s boot:

* `--smoke` OK. A1: exact 84-prefix. A5: exit 0, 2 windows, **0 real**.
* `truncated=0`; deepest file **#83**; max wait-hold streak 1.
* `arms=12702 ints=12700 isr=12700` (0.9998), `kicks == walks == drains = 6648`,
  `distinct=985`, engine counter 0 — the healthy shape part 7 defined.

## Where part 13 starts, in order

1. **THE BLACK PANELS: find who writes `0364B000`.** The three rectangles are a 16x16
   DXT1 at that address whose every texel reads zero in our guest memory.
   `CZ_VK_SKIP_TEX=0364B000` removes exactly those three rectangles and reveals three
   correct thumbnails, so everything else about the screen is right. The draws blend
   `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` and our pipeline honours that, and an all-zero DXT1
   is opaque black under BC1 — so hardware's bytes there differ from ours.

   The instrument is **gotcha 143's**, not `CZ_PM4_MEM_WATCH` (which only sees GPU
   stores): attach `gdb -p` to a run parked on that screen and
   `watch *(unsigned int*)((char*)g_memory.base + <va>)`, then two continues and two
   backtraces name the guest function that should have filled it. Getting the run parked
   is now easy — `CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A` puts the panel on
   screen for about ten frames around frame 570, and `CZ_THREAD_TRACE=1` joins gdb's
   host threads to guest ones.

2. **THE MALFORMED TEXT: probe the texture coordinates.** `007C6000` (376x376 glyph
   atlas) garbles and `007BB000` (184x184) is perfect, through the SAME `(vs, ps)` pair,
   and the atlas has been cleared by two arms — `CZ_VK_TEX_REFRESH` (2,250 in-place
   re-uploads, picture identical) and `CZ_VK_TEX_DUMP` (a clean page of glyphs). So it is
   the draw's UVs. Isolate one with `CZ_VK_ONLY_TEX=007C6000` and read its vertex data.
   The suggestive number: **376/184 = 2.04**, and the garbled glyphs read as magnified
   fragments — so "the UVs were computed for the other page's dimensions" is the first
   thing to test, and `CZ_VK_DRAW_PROBE` plus `CZ_VK_DRAW_PROBE_MINVERTS` is the tool.

3. **THEN THE PICTURE AS A WHOLE** (part 12's item 2, untouched). Every defect between
   the scene and the screen that this port knows about is fixed and the scene surface is
   99.5% non-black with both tiles rendering. Nobody has compared the result against
   capture E with `tools/frame_signature.py` and said what is still wrong — colour,
   gamma, missing passes, the UI layer. Bind it by ERA, not by one frame: this title
   screen is TWO screens (gotcha 176). Current state: every dumped title-screen frame's
   best orientation is `identity` at +0.42..+0.55, none reaching the tool's +0.70 floor,
   which is what an animated camera looks like rather than a defect.

4. **The conservative screen extent is a placeholder** (part 12's item 3, untouched).
   `WriteScreenExtent` writes "this draw may have touched anything", which makes bin
   predication a no-op and costs work: both tiles execute ~975,000 draws where hardware
   executes ~573,000 each. The honest improvement is the draw's real screen-space bound,
   judged against `CZ_PM4_NO_SCREEN_EXTENT=1` plus the census. **Do not do this
   speculatively** — the cost has still not been shown to matter.

5. **The kernel gates are exhausted as a forward oracle** (unchanged since part 9). A1's
   position 93 is not the next piece of work (finding 49, gotcha 107). Going further
   needs a gameplay comparison built from A2.

6. **A CRASH NOBODY HAS LOOKED AT, and it is past every screen this port has measured.**
   With `CZ_FAKE_PRESS_SEQ=START,A,A` held, the arm keeps pressing A, the title loads
   the actual game, and the boot reaches **file #137 `game:\data\audio\Prologue.txt`**
   before a SIGSEGV in guest code — 53 files deeper than any gate in this project. The
   crash report is in the session log; `lr=827885DC`, `r4` points at the string
   `deadrising/assets.php`, and the guest backtrace is 15 frames deep through
   `825DA0C0 -> 825D7564 -> 825D2648 -> ...`. It is the first evidence this port has of
   what happens after the menu, and it is a genuinely new frontier rather than a
   regression. Reproduce with the recipe in §6aa before theorising.

## Traps this session paid for — do not re-buy them

* **`CZ_D3D_DRAW=1 CZ_VKDRAW=1` IS NOT THE DRAW ARM.** They are mutually exclusive and
  the runtime disables the draw arm, saying so once in a line that scrolls past:
  `[d3ddraw] CZ_D3D_DRAW and CZ_VKDRAW are mutually exclusive; CZ_D3D_DRAW DISABLED for
  this run`. Three runs here were PM4 runs wearing the draw arm's name, and they agreed
  with each other and with a control perfectly. The tell was in the chain: `arms=74,
  kicks=0, walks=0` against the real arm's `arms=12627, kicks=6752`.
* **A range that is not the range.** "39 textures uploaded black and their guest memory
  is non-zero now" was measured over the 8 KB tiled FOOTPRINT of textures whose own
  texels are 128 bytes, so it was mostly asking about the neighbours. Asked of the bytes
  the untile actually reads it is zero of 58 — and a repair built on the bad number fired
  3,258 times and turned the panel white.
* **A documented instrument can not exist.** `CZ_VK_PASS_DRAWS=N` has been in
  `CLAUDE.md`'s list since part 9; the count was a hardcoded 4 and the variable was read
  nowhere. Grep the emitter before quoting a knob, the same way gotcha 25 says to grep it
  before believing a zero.
* **Run timed arms serially** (gotcha 183) — unchanged.

## New instruments and arms

```
CZ_VK_SKIP_TEX / CZ_VK_ONLY_TEX=<hex[,hex]>  render all but, or only, the draws whose
                                 first bound texture is at that guest address. One level
                                 below CZ_VK_ONLY_VS, because a UI compose is a hundred
                                 quads sharing two shaders. This is what gives a
                                 rectangle on screen a name
CZ_VK_PASS_DRAWS=N               how many of a pass's draws the resolve trace lists —
                                 NOW IMPLEMENTED (it was a hardcoded 4). Each entry
                                 carries the draw's texture address and flags (DUMMY)
CZ_VK_TEX_CENSUS=1               per texture ADDRESS: uploads, all-black uploads,
                                 fetches served from a resolve snapshot, and fetches
                                 that fell back because the snapshot was too old
CZ_VK_TEX_REFRESH=<hex[,hex]>    re-read those textures' pixels on EVERY fetch, into the
                                 same image and slot. The arm for "we cached a texture
                                 the guest is still writing"
CZ_VK_TEX_DUMP=<dir>             the UNTILED bytes of a texture as a greyscale PGM;
CZ_VK_TEX_DUMP_ADDR=<hex[,hex]>  restrict it to named addresses. Separates "our untiling
                                 scrambled this" from "the draw samples it wrong"
CZ_VK_SNAP_FRAME=N               which frame CZ_VK_SNAP_DUMP dumps (was a hardcoded 600)
CZ_VK_FRAME_DUMP_EVERY=N         the frame-dump interval (was a hardcoded 64; the panel
                                 appears in ONE frame of a 180 s boot at that cadence)
```
