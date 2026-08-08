# D3D phase C, part 16 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §§6af-6ag** first — they
are the record of the session this hands off from. `docs/d3d-phase-c15-kickoff.md` is the
previous hand-off: **its item 1 is CLOSED and reframed, item 3 is half closed**, items 2
and 4-9 are unchanged and repeated below.

## What part 15 changed, in one paragraph

Part 15's item 1 was "the prologue presents 0.00% non-black while issuing ~1,200 draws a
frame; the tone map is the first black link; it is a compose failure". It is three
defects stacked, and **the one that actually holds the screen black is not a defect at
all** — the title's own tone map is set to fade to black at full strength, and the
renderer is drawing that faithfully. Underneath it the prologue's opening highway into
Still Creek renders correctly. The two real defects found on the way (a shader missing
from the cache; the colour-grading LUT's snapshot expiring) are both fixed and neither
was the symptom. So the frontier moved off the renderer entirely: **the prologue is stuck
in a faded-out state with a frozen camera**, which is a CPU-side question.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`, no input, empty `CZ_SAVE_DIR`, one 150 s boot:

* `--smoke` OK. A1: **exact 84-prefix**. A5: **exit 0, 2 windows, 0 real**.
* `truncated=0`, 0 parser stalls; deepest file **#83 `game:\data\skeleton\cinezombie.big`**.
* Presented frame at the title screen: **98.99% non-black, 117,112 colours.**

The shader cache is **337** shaders, not 336 — see the CLAUDE.md recipe, which now runs
the dump deep enough to reach the prologue. `assets/shader_spv/` is gitignored, so a
fresh clone must rebuild it with that recipe or it will silently decline 28,718 draws a
prologue run.

## How the prologue's black screen came apart, because the SHAPE is the lesson

Three layers, and fixing the first two changed nothing visible. Full detail in §6af.

1. **`vs_24e60d91249e6d04` was not in the shader cache.** 351 dwords, loaded by the
   prologue, in NEITHER capture (A1 stops at the title screen, A2 is gameplay) and not in
   our own dump either, because the recipe built it from a plain boot that also stops at
   the title screen. It reports as ONE log line — `[vk] no translated shader for VS
   <hash> — draws skipped`, once per hash — plus a counter. No failure, no fallback, no
   picture alarm. `grep -c "no translated shader" run.log` must be 0 and is now the
   documented gate. **Fixed; frame still 0.00%.**
2. **The colour-grading LUT's resolve snapshot EXPIRED.** The rule was `frameSeen + 1 >=
   frame`. The title screen re-renders all three LUTs every frame so the window never
   bound; the prologue's grade is static, the LUT stops being resolved, and the fetch
   fell through to guest memory, which is zero for a resolve destination because resolved
   pixels are never written back. §6s already proved a black LUT is a black frame. The
   window is gone; `CZ_VK_SNAPSHOT_MAX_AGE=1` is the control arm. **Fixed; frame still
   0.00%,** because (3) was on top of it.
3. **The guest is asking for black.** `CZ_VK_DRAW_PROBE` with
   `CZ_VK_DRAW_PROBE_PC=105,106,110,111,112,254,255` on the tone map draw
   (`tex=0684B000 1280x720`, vs `39942752000f0549`, ps `114c4965eaabd54c`):

   | | title screen (frame 440) | prologue (frame 1000) |
   |---|---|---|
   | `pc(110)` vignette colour + strength | 0,0,0, **0.4** | 0,0,0, **1.0** |
   | `pc(111)` vignette power + norm | **1.0**, 1.414, 0, 0 | **0**, 0, 0, 0 |
   | `pc(254)`, `pc(255)` LUT arithmetic | identical | identical |

   The shader ends `r0.x = pow(r1.w / pc(111).y, pc(111).x) * pc(110).w;
   oC0 = saturate(r0.x * (pc(110).xyz - colour) + colour)`. With the power at zero,
   `pow(·,0) == 1` everywhere, so the weight is 1.0 at every pixel and the whole frame
   is lerped to `pc(110).xyz` = black.

**Proved with an arm, not argued** — and this is the technique to reach for again:

```
# copy the cache, patch ONE line of the tone map's HLSL, point the runtime at it
sed '701s/.*/\tps = 0.0;/' tonemap.hlsl > tonemap_nofade.hlsl     # the fade weight
dxc -T ps_6_0 ... -Fo $DIR/ps_114c4965eaabd54c.spv tonemap_nofade.hlsl
CZ_SHADER_SPV=$DIR ./cz_runtime ...
```

| prologue frames 1024..1664 | non-black | colours |
|---|---|---|
| stock shader | 0.00% | 1 |
| fade weight forced to 0 | **99.99%** | **~89,450** |

and the picture is the road into Still Creek with power lines, rocks and the tree,
tone-mapped and graded. **The renderer draws the prologue.** (Regenerating that HLSL:
`tools/synth_shader_container.py` on the `.ucode`, then XenosRecomp — the whole recipe is
inside `tools/build_shader_spv.sh`.)

## Where part 16 starts, in order

1. **THE PROLOGUE IS STUCK, AND IT IS A CPU-SIDE QUESTION.** Everything below is
   measured on the black era of a 300 s `CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A`
   run:
   * the **camera fingerprint is one constant value, `00d7a3a4aaed62c6`, for 1,700+
     frames**, and the scene surface's mean luminance is pinned at 104.484 with coverage
     100.0000 on every sampled frame (`CZ_VK_FRAME_STATS` +
     `CZ_VK_FRAME_STATS_SURFACE=0684B000`);
   * the draw stream still MOVES — 1,225..1,247 draws and 848,653..883,067 vertices,
     cycling through a handful of fingerprints — so the title is alive and submitting
     work and it is the scene STATE that is frozen;
   * everything underneath is healthy: `ring: chain arms=11489 ints=11483 isr=11483`,
     `kicks == walks == drains = 6659`, `distinct=764`, `truncated=0`, zero parser
     stalls, and every `[wait]` is an idle worker or one of the two threads the title
     blocks by design (finding 41).

   **The leading hypothesis is AUDIO, and it is a hypothesis.** The loading ends with six
   `XMACreateContext` calls, the pump runs — **55,808 driver frames in 300 s** — and
   every sampled frame has peak amplitude **exactly 0.0000**, because there is no XMA
   decoder (item 10). An in-engine cinematic cued off a voice or music stream would look
   exactly like this. Two cheap ways in, neither of them renderer work:
   * **the operator gets past this with a real controller.** That is free evidence and it
     should be checked before anything is built: if a human can advance it, the block is
     not audio. Confirm what the operator actually did (the part-14 report is all Still
     Creek gameplay, so they reached it somehow).
   * **probe what the cinematic polls.** The XMA context block has fields the guest reads
     back (subframes played, loop state) and ours are pinned at 0 forever. `gdis.py
     --find-uses` on the context array's address, or a hardware watchpoint on a context
     field in a run that reaches the prologue, names the reader (gotcha 143).

   Use the no-fade shader arm to watch the scene while it is faded out — it is the only
   way to see whether anything in the world is moving.

2. **XAM ordinal `0x271` is resolved on the save-LOAD path and we answer NOT_FOUND**
   (`docs/phase3-notes.md` finding 51 — unchanged since part 14). With A3's real save
   installed (`Xenia logs/A3_save_content/cz_A3_save_DR2P000.zip`, laid out as
   `<CZ_SAVE_DIR>/DR2P000.DSF/DR2P000.DSF`), our content layer enumerates it correctly —
   `1 item(s)`, `XMsgCompleteIORequest(result=0)` — and the title reaches the save-slot
   panel, labels SLOT 1 **`Damaged Content`** and puts up `Load failed!`, having never
   opened the file. `imports.cpp`'s `kResolvable` is the SEVEN ordinals A1 resolves, and
   A1 was captured with no save; A3 resolves an eighth. Do NOT mint a stub for it blind
   (gotchas 59/201) — name it from the guest's own call site first, and note that
   `tools/gdis.py --find-uses 0x271` finds nothing, so the ordinal is not built by a plain
   `li`. The profile-signature question (a 360 save is signed per profile and A3's was
   made under the fork's GUID) is separate and may make "Damaged Content" the right answer
   to THAT file even once the ordinal exists.

3. **THE SHADOW CASCADE, half closed.** Part 15 counted the empty region instead of
   looking at it, which split the problem cleanly:

   ```
   rows    0.. 511   all 1024 columns populated
   rows  512.. 719   only columns 960..1023 (a 64-wide strip)
   rows  720..1023   nothing
   ```

   * **Ours, fixed:** window-coordinate draws were mapped through the presented frame's
     1280x720 rather than the EDRAM's 1280x1024. The arithmetic is an identity either way
     so nothing looked wrong; the CLIP is not. Cascade non-black **12.82% -> 13.28%**,
     which is the clipped 64x304 strip to the pixel.
     `CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1` is the control arm.
   * **The title's, open:** its clear rects for a 1024x1024 cascade are
     **`(0,0)-(480,512)` and `(960,0)-(1024,1024)`** (`CZ_VK_DRAW_PROBE` on
     `vs=539ea9e08aa83f0c`, prim 8, 3 indices, z=1.0, `depthCtl=76` = test/write with
     compare func **ALWAYS**). Those do not cover the map and nothing this renderer does
     causes it.

   Three readings, and **test the third first because it is free**: (a) 480x512 is a
   PIXEL extent wanting a x2 somewhere — the pass reports `msaa=0` so our 4x scaling does
   not apply, and what the guest thinks that surface's sample extent is has not been
   checked; (b) the "cascade" is several smaller maps packed into one 4096-wide surface —
   four are resolved per frame; (c) the uncleared region is never sampled and the shadows
   fail at the consumer. (c) is answered by probing the fetch coordinates of the pass that
   samples `1439B000(depth)` — 629,023 fetches a boot, the most of any texture in the
   frame. **Do not judge the shadow lookup until the map is right** (unchanged).

4. **No mipmaps have ever been uploaded** — `ci.mipLevels = 1` in `CreateImage`, every
   texture, every phase. The operator's "all textures seem weird grainy". Real work: the
   Xenos mip chain has its own address layout and the untiler reads mip 0 only.

5. **The Still Creek sign's dark smear, and the GAS roundel.** `CZ_VK_SKIP_TEX` to give
   each an address, then `CZ_VK_TEX_DUMP` to separate "our decode scrambled this" from
   "the texture is fine and the draw shades it wrong". The smear is NOT the untiler (0
   skips in 925 textures) and NOT a shadow (`CZ_VK_NO_DEPTH_FETCH=1` leaves it).

6. **Colour is flat and green-shifted** (§6ad item 2). Much improved by part 14 and not
   closed. §6s proved the frame depends completely on the colour-grading LUT — and part
   15 has now shown the LUT snapshot was expiring, so **re-ask this one**: it was last
   judged on a binary where the LUT could go stale (gotcha 172).

7. **The conservative screen extent is still a placeholder** (part 11). **Do not do this
   speculatively** — the cost has still not been shown to matter.

8. **The depth-resolve cost, if it ever matters.** ~6% of the frame rate. An
   optimisation with no measured problem behind it.

9. **The kernel gates are exhausted as a forward oracle.** A1's position 93 is not the
   next piece of work (finding 49, gotcha 107). Going further needs a gameplay comparison
   built from A2.

10. **Audio output and XMA decoding (phase 6).** Promoted in importance by item 1: it is
    no longer only "the game is silent", it may be what the prologue is waiting for. The
    kick bitmap at `0x7FEA1A80` lands in ordinary flat memory and is inert; a real
    decoder needs that aperture trapped as MMIO.

## Traps this session paid for — do not re-buy them

* **Fixing a real defect and seeing no change is not a wrong theory.** Two of the three
  layers under the prologue's black screen were ours and neither moved the picture. If
  the first fix does nothing, keep the fix and look for the next layer.
* **"Did we compute this black, or were we told to?"** is the first question a black
  frame deserves, and only an arm on the shader answers it. Every aggregate in this
  project reported a healthy chain, correctly.
* **A freshness window is a statement about one consumer's access pattern.** Ask what
  makes the value stale, not how old it is.
* **An identity mapping can still clip.** Window coordinates belong to the surface the
  pass renders into, not to the screen.
* **Count an axis-aligned boundary, do not look at it.** 512 / 720 / a 64-wide strip at
  x=960 named one defect immediately (720 is the presented frame's height) and turned the
  rest into the title's own vertex data. Eyes are for transforms and blurs (gotchas 135,
  204), not for edges.
* **My own grep was wrong twice this session** — once checking the cache against a stale
  cwd, once grepping `[audio]` for lines the kernel prints as `[kernel] audio frame`.
  Gotcha 25 applies to the checks you run in the shell, not just to the ones in the code.
* **Run timed arms serially** (gotcha 183) — unchanged.

## New instruments and arms

```
CZ_VK_SNAPSHOT_MAX_AGE=N   how many frames old a resolve snapshot may be and still be
                           served (default 0 = NO limit; 1 is the pre-part-15 renderer)
CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1  map window-coordinate draws through the presented
                           frame's 1280x720 rather than the EDRAM's 1280x1024 — the
                           pre-part-15 renderer, in which every such draw taller than the
                           screen is clipped at row 719
```

`CZ_VK_DRAW_PROBE_PC=<list>` and `CZ_VK_DRAW_PROBE_VERTS=N` both already existed and are
the two that did the work here: the constants named the fade, and the vertices named the
clear rects. When a pass is one full-screen quad, `CZ_VK_DRAW_PROBE_COUNT` has to be big
enough to reach it — a frame has ~54 passes and the probe counts DRAWS, so 250 was needed
to reach the tone map at a chosen frame.
