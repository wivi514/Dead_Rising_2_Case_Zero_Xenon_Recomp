# D3D phase C, part 12 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §6x** first — it is the
record of the session this hands off from, and §6w now carries a retraction banner
pointing at it. `docs/d3d-translation-plan.md`'s **"Phase C part 11"** is the same story
in one page. `docs/d3d-phase-c11-kickoff.md` is the previous hand-off: its item 1 is
**closed**, item 2 is measured below, items 3 and 4 are unchanged.

## What part 11 changed, in one paragraph

Part 10 handed over "the right tile's draws carry `80000000`, which is the placeholder
the emitters write and a fix-up pass is supposed to overwrite — and on our runtime that
pass runs once, patches zero records, behind a closed gate", plus one open item it could
not explain. **All of that was a probe printing only its first call.** The pass runs
1,751 times a boot over 388,451 records with its gate open; what it computes is wrong,
because one of its two inputs is uninitialised memory. That input is the **screen
extent** the GPU is supposed to write — `EVENT_WRITE_EXT` event `0x1A`, 818,507 packets
a boot, a packet our command processor decoded and dropped since phase 1. Answering it
conservatively took the bin rule's discard rate from **32.7% to 0.28%, against
hardware's 0.3%**, and the scene surface from 56% to 99.5% coverage.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`:

* `--smoke` OK.
* A1: exact 84-prefix. A5: exit 0, 0 real windows.
* `truncated=0`; deepest file **#83 `game:\data\skeleton\cinezombie.big`**.
* `draws=2,761,594 (predicated out=7,853)` — **0.28%**, against B1's 0.3%.
* bin census: right tile `8000000F` x974,779 kept 100%, left tile `FFFFFFFF` x975,698
  kept 100%; the only residue is the per-tile clear loop's `3`/`C` masks, symmetric at
  3,928 / 3,925.
* scene surface (`06BE4000`) median coverage **99.48%**, 2,480 draws and 1.28 M
  vertices a frame, over two runs per arm alternated against
  `CZ_PM4_NO_SCREEN_EXTENT=1` (56.1 / 53.8%, 1,630 draws, 820 k vertices).

Phase C draw arm, `CZ_D3D_DRAW=1` — see the gate section at the bottom for the numbers
this session measured; part 10's healthy shape (`arms:ints` ~= 1.0, `walks == kicks ==
drains`, `distinct` in the hundreds, engine counter 0) is the thing to check.

## Where part 12 starts, in order

1. **THE PICTURE IS THE NEXT QUESTION, and it is now worth asking properly.** Every
   defect between the scene and the screen that this port knows about is fixed; the
   scene surface is 99.5% non-black with both tiles rendering. What nobody has done is
   compare the result against capture E with `tools/frame_signature.py` and say what is
   still wrong — colour, gamma, missing passes, the UI layer. Do that FIRST, because it
   is the only thing that can tell you whether the renderer's remaining work is large or
   cosmetic. Bind it by era, not by one frame: this title screen is TWO screens (gotcha
   176), so measure every dumped frame and separate the eras with one `awk`.

2. **The conservative extent is a placeholder for a real one, and it should be measured
   before it is improved.** `WriteScreenExtent` writes "this draw may have touched
   anything", which makes bin predication a no-op. That is correct output and it costs
   work: both tiles now execute ~975,000 draws where hardware executes ~573,000 each. If
   frame rate becomes the question, the honest improvement is to write the draw's real
   screen-space bound — which the renderer could compute from the transformed geometry,
   or approximate from the viewport — and the arm to judge it against is
   `CZ_PM4_NO_SCREEN_EXTENT=1` plus the census. **Do not do this speculatively.** The
   current cost has not been shown to matter.

3. **Item 2 from part 10/11's kickoff — the walker's dead `case 0x54:` INTERRUPT block
   and `MirrorIsPoisoned()`.** Re-measured this session on the current draw arm; the
   result is in the gate section below. Delete only if they are still zero *there*, and
   not in a session that is also changing segment routing (gotcha 182).

4. **The kernel gates are exhausted as a forward oracle.** A1's position 93 is not the
   next piece of work (finding 49, gotcha 107). Going further needs a gameplay
   comparison built from A2. Unchanged from parts 9, 10 and 11.

## Traps this session paid for — do not re-buy them

* **A probe that reports "1" is reporting its schedule.** Two probes printed at call #1
  and then every 20,000/200,000 against subsystems that run a few thousand times a boot,
  so the only line any run emitted read "x1" — and a whole session's conclusions were
  built on it. Both now report on a 15-second clock. Before quoting any probe number,
  read its emitter (gotchas 109, 186).
* **A packet with a name in the opcode table is not a packet you handle.**
  `EVENT_WRITE_EXT` was named, censused, and passed both capture oracles while doing
  nothing 818,507 times a boot, because the handler it shared stores only when a packet
  carries a value dword. Census the capture by `(opcode, body length, event type)` and
  read any row your handler falls through as a hole (gotcha 184).
* **`frame_compare.py` reads a file that may still be being written.** A run judged
  immediately after `timeout` killed the process reported `NO SCENE CONTENT` for an arm
  that had 386 frames of it. Wait for the process, then compare.
* **The frame dump does not create its directory.** `CZ_VK_FRAME_DUMP=/tmp/x` on a path
  that does not exist writes nothing and says nothing. `mkdir -p` first.
* **Run timed arms serially** (gotcha 183) — unchanged, and it still applies to the
  draw arm.

## New instruments and arms

```
CZ_PM4_NO_SCREEN_EXTENT=1        do not answer the GPU's screen-extent query — the
                                 pre-part-11 command processor, and the same-binary
                                 control arm for the right tile. Applies to pm4.cpp and
                                 d3d_draw.cpp together
CZ_BINMASK_PROBE=1               now reports on a 15-second clock and covers all four
                                 inputs: the mask setter's callers, the fix-up pass's
                                 output histogram, its TWO INPUTS (tile rects + a census
                                 of the per-record screen extents), and the bin SELECT
                                 producer sub_8284A6D0
tools/xtr_bin_predication.py     --trace-window N --trace-arm-mask HEX prints the
                                 capture's own stream-order window, the twin of
                                 CZ_PM4_BIN_TRACE + CZ_PM4_BIN_TRACE_ARMMASK
```
