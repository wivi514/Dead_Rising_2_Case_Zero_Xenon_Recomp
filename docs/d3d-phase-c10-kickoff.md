# D3D phase C, part 10 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §§6s-6v** first — they are
the record of the session this hands off from, and they are about the RENDERER rather
than about phase C's command-stream work. `docs/d3d-translation-plan.md`'s
**"Phase C part 9"** section is the same story in one page.
`docs/d3d-phase-c9-kickoff.md` is the previous hand-off: its item 1 is done, items 3-5
are unchanged and restated below.

## What part 9 changed, in one paragraph

The title screen has always been TWO screens, and every claim about it for three phases
was a single sample of whichever one the frame dump happened to catch: a 49-frame logo
pulse (already correct — a near match for capture E2) and a much longer era that is
supposed to show the animated Still Creek 3D background of capture **E3** and showed
black. Four defects sat between that scene and the screen, each hiding the next: a stale
texture-cache entry that fed the tone map a dead colour-grading LUT (so the whole compose
came out black); DRAW_INDX's index swizzle read from the address dword instead of the
size dword (so the geometry exploded from screen centre, and 40% of index buffers were
also read one index early); a rectangle list's fourth corner never being drawn (so half
of every per-pass CLEAR was missing and stale depth rejected the scene behind it); and
window coordinates being mapped one-to-one on a 4x-MSAA surface (so the scene tile's
clear covered 320 of its 640 columns). All four are fixed, each with a same-binary
control arm.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`, 170 s headless boots:

| | before part 9 | **after** |
|---|---|---|
| tone map output `1439B000` | 0.00% non-black, 1 colour | **95.3%, 56,658 colours** |
| presented frame, Still Creek era | 2.31% non-black, ~880 colours | **99.4%, 30-78k colours** |
| scene surface `0684B000`, columns 0..640 | content in 0..320 only, inside one triangle | **1.00 covered, all 640** |
| what it looks like | PRESS START on black | **sky, power lines, the gas station, zombies, the road** |
| frames presented | 1,189 | 1,188 |

Gates on that binary: `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 0 real
windows**; `truncated=0`; deepest file **#83 `cinezombie.big`**. The draw arm was not
re-gated this session — see item 2.

## Where part 10 starts, in order

1. **THE RIGHT TILE IS BIN PREDICATION, and the numbers are already on the table.**
   Full record in `docs/phase5-notes.md` §6v. The scene is rendered as two 640-wide
   tiles (gotcha 118); the left tile's pass executes **931 draws / 496,130 vertices**
   and renders correctly, the right tile's executes **23 / 9,219**. The reason is that
   `gpu/pm4.cpp` implements ME predication — `(header & 1) && (binMask & binSelect) == 0`
   — and **a third of this title's draw packets are discarded by it** (1,039,423 of
   3,113,236 over 1,579 frames). `CZ_PM4_BIN_TRACE=N` prints the pair hardware compares;
   over 300,000 draw packets:

   | draw's `SET_BIN_MASK` | tile's `SET_BIN_SELECT` | count | outcome |
   |---|---|---|---|
   | `00000000FFFFFFFF` | `0000000080000003` | 108,328 | run |
   | `00000000FFFFFFFF` | `00000000FFFFFFFF` | 81,079 | run |
   | `0000000080000003` | `000000000000000C` | **74,773** | SKIPPED |
   | `0000000080000000` | `000000000000000C` | **25,770** | SKIPPED |
   | `000000008000000F` | `000000000000000C` | 8,385 | run |

   So the two tiles select bins `{0,1,31}` and `{2,3}`, and the `{2,3}` tile keeps ~8%
   of what it is offered. A title does not emit 74,773 unreachable draws a boot, so
   either the bins are not the left/right split we assume, or the comparison is wrong in
   one of three specific places: the 64-bit assembly from the LO/HI packets, the meaning
   of bit 31 (set in nearly every mask and in one select but not the other), or the
   ORDER — a mask read one draw late produces exactly this shape, and `SET_BIN_MASK_LO`
   outnumbers draw packets ~1.6:1 in B1 so the pairing is not obvious.

   **Run this first, and it needs no emulator:** B1 carries the same
   `SET_BIN_MASK`/`SET_BIN_SELECT`/`DRAW_INDX` stream, so replaying the predication rule
   over it with `tools/xtr.py` and counting survivors per bin select says whether 8% is
   what hardware does. `CZ_PM4_NO_PREDICATION=1` is the arm and is NOT a candidate fix:
   it is destructive (257,395 draws in 2,905 frames, black screen), because running a
   packet hardware skipped puts one tile's geometry in another tile's pass.

   A number that had to be WITHDRAWN on the way, because it would have sent the next
   session hunting the wrong thing: "hardware issues ~2,540 draws a frame and we issue
   ~1,620" compares draw PACKETS in a capture against draws the RENDERER ACCEPTED. At
   the same instant of one run the command processor parses 1,971 packets a frame and
   hands the renderer 1,313. The predication that eats the difference had been in the
   code since phase 1 and had never been counted — `ring: ... draws=N (predicated
   out=M)` is now always on.

2. **Re-gate the phase C DRAW arm.** Everything in part 9 is in shared renderer code
   (`gpu/vk_renderer.cpp`) plus one matching change in `gpu/d3d_draw.cpp`'s walker, so
   `CZ_D3D_DRAW=1` should inherit all of it — but that is a prediction, not a result
   (gotcha 67). Run the arms alternated and check `ints/arms`, `distinct`, `dev+0x2B04`,
   the deepest file and the A1/A5 gates against part 8's table.

3. **The walker's `case 0x54:` INTERRUPT block is DEAD on every arm**, and
   `MirrorIsPoisoned()` still records zero skips. Unchanged from part 9's kickoff:
   deleting them is a clean, reversible decision for a session that is not also changing
   the routing.

4. **The kernel gates are exhausted as a forward oracle.** A1's position 93 is not the
   next piece of work (finding 49, gotcha 107). Going further needs a gameplay comparison
   built from A2.

## Traps this session paid for — do not re-buy them

* **CHECK THE WHOLE DIFF AGAINST THE LAST KNOWN GOOD COMMIT BEFORE COMMITTING.** Five
  files in the working tree were rolled back to their pre-part-8 content by something
  outside this session's edits, and `git add -A` committed the rollback — taking part
  8's entire fix with it (`D3dDraw_ServiceReserve`'s `dev+0x3460` routing, the two
  `ChainStats` resolve/reseed counters and their hook-table call sites). The draw arm
  duly regressed to `ints/arms` in the tens and `#60 zombies.big`. What found it was
  `git diff <last-known-good> HEAD --stat -- runtime/`, which showed deletions in files
  this session never opened; what proved it was not part 9's work was reverting all four
  of part 9's fixes with their arms and seeing the draw arm STILL fail. Run
  `git diff` over the whole runtime, and run the OTHER arm, before believing a session
  changed nothing it did not mean to.

* **A counter behind an early return counts the times the early return did not happen.**
  `texture: resolve snapshot too old, falling back to guest memory` read **7** on the
  broken binary and **70,681** on the fixed one, because the cache hit short-circuited
  before the snapshot was ever consulted. Its silence was unfalsifiable from inside, and
  every other instrument reported a healthy chain while the picture was black.
* **A retirement is only as good as the oracle it was measured on.** Part 9 retracts
  `phase5-notes` §6c's retirement of index endianness. That A/B was honestly run and
  scored against a frame that was black for an unrelated upstream reason. Before quoting
  any earlier "measured and retired", ask what metric it was scored on and whether that
  metric was working then.
* **A picture is a sample of an animated scene, and this phase has now nearly lost four
  claims to that.** `CZ_VK_FORCE_COLORMASK=1` produced a visibly better-textured scene
  and is not a fix. What settled it was a counter — splitting "empty colour mask" by
  `RB_MODECONTROL` — not another look.
* **Two capped prints can make a dependency graph unreadable.** The resolve trace's
  60-line budget guarded only the pass HEADER, so the frame's LAST pass (the front
  buffer, where every "why is it black" question ends) fell off the end whenever the
  resolve order shifted; and the per-pass draw list was capped at 4. Both are now
  configurable (`CZ_VK_RESOLVE_TRACE_PASSES`, `CZ_VK_PASS_DRAWS`). The instrument that
  finally localised the black frame is the per-pass dependency graph gotcha 140 was
  written for, and two print caps were all that stood between it and the answer.
* **Bound a state probe by FRAME.** `CZ_VK_DRAW_PROBE_MINFRAME` and the new
  `CZ_VK_DRAW_PROBE_COUNT` matter for a shader issued many times per pass with different
  data — a rectangle-list CLEAR is issued once per surface, so the first three entries
  describe three other passes entirely.

## New instruments and arms

```
CZ_VK_TEX_CACHE_FIRST=1        consult the texture cache before the resolve snapshot
                               (the pre-part-9 lookup order; reproduces the black frame)
CZ_PM4_INDEX_ADDR_SWIZZLE=1    read DRAW_INDX's index swizzle off the ADDRESS dword's
                               low bits again (the pre-part-9 exploded geometry)
CZ_PM4_DRAW_TRACE=1            the raw DRAW_INDX body for the first 24 indexed draws
CZ_VK_RECT_HALF=1              expand a rectangle list to the same triangle twice again
CZ_VK_NO_MSAA_WINDOW_SCALE=1   map window coordinates one-to-one on a 4x MSAA surface
CZ_VK_NO_DEPTH_TEST=1          an ARM, never a fix: draw everything regardless of depth.
                               Separates "never submitted" from "submitted and rejected
                               by depth left over from another pass"
CZ_VK_PASS_DRAWS=N             how many of a pass's draws the resolve trace lists
CZ_VK_RESOLVE_TRACE_PASSES=N   the resolve trace's budget, in PASSES rather than lines
CZ_VK_DRAW_PROBE_COUNT=N       how many draws the draw probe prints
CZ_PM4_BIN_TRACE=N             the predication inputs for the first N draw packets:
                               the draw's bin MASK, the tile's bin SELECT, their overlap
                               and whether the packet ran
CZ_PM4_NO_PREDICATION=1        execute predicated packets anyway. An arm, and a
                               DESTRUCTIVE one — see item 1
```

The `[vkvp]` viewport line now carries the SCISSOR, the window offset, the MSAA mode and
the raw `RB_SURFACE_INFO`; `[psbind]` carries the draw's colour mask and blend control.

## Standing gate results to compare against

PM4 control arm, this binary, default flags:

* `--smoke` OK.
* A1: **exact 84-prefix** (position 71 permutes on some runs — the long-known
  scheduling-sensitive window, 4 of 10 historically).
* A5: **exit 0, 0 real windows**.
* `truncated=0`; deepest file `#83 game:\data\skeleton\cinezombie.big`.
* Both PM4 capture oracles (`pm4_packet_lengths.py`, `pm4_indirect_walks.py`) clean.
