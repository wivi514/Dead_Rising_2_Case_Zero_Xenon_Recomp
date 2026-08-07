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

**The one number this session could not settle, recorded with all four runs** (gotcha
159 says print them). The screen extent makes ~2x more draws execute, so it might cost
frame rate on the draw arm. Two 170 s runs per arm, serial, alternated,
`CZ_PM4_NO_SCREEN_EXTENT=1` as the control:

| run | frames | deepest | `truncated` | `ints/arms` | `walks==kicks==drains` | engine ctr |
|---|---|---|---|---|---|---|
| control 1 | 3,659 | #83 | 0 | 1.000 | yes | 0 |
| **extent 1** | **3,570** | #83 | 0 | 1.000 | yes | 0 |
| control 2 | 3,053 | #83 | 0 | 1.000 | yes | 0 |
| **extent 2** | **410** | #83 | 0 | 1.000 | yes | 0 |

Read it carefully, because the obvious reading is wrong twice over. **There is no
systematic cost visible**: 3,570 against 3,659 is 2.4%, and the CONTROL arm's own two
runs differ by 20%. And the 410 run is **not part 7's stall** — that had `arms` frozen
while `ints` climbed and `distinct` collapsed to 2-6. This run's every ratio is healthy
and every counter is scaled down together (`arms` 1,036, `distinct` 169, 476 kicks), it
ran the full 171 s, it did not crash, and it reached #83 with `truncated=0`. It is a
slow run, not a broken one.

What n=2 per arm CANNOT do is separate "the extent made the slow mode more likely" from
"this arm has been bimodal since part 6 and I sampled it twice". **If that matters,
n>=5 per arm, serial (about an hour).** Every structural gate is clean on all four
runs, so it is not a blocker.

Phase C draw arm, `CZ_D3D_DRAW=1`:

* `--smoke` OK. A1: exact 84-prefix. A5: exit 0, 0 real windows.
* `truncated=0`; deepest file **#83** — the same as the control arm.
* the healthy chain shape part 7 defined, unchanged: `arms=1115 ints=1114 isr=1114`
  (0.999), `walks == kicks == drains`, `distinct=191`, engine counter `dev+0x2B04` = 0,
  max wait hold streak 1.

## Where part 12 starts, in order

1. **THE MENU PANELS — the one open defect with a live operator report, and it is now
   MEASURABLE.** On the new-game screen three panels render as solid black rectangles
   and some label text is malformed. Full record in `docs/phase5-notes.md` §6z. It was
   not touched in part 11 because that screen is two menu levels past the title and
   nothing headless could reach it; **`CZ_FAKE_PRESS_SEQ=START,A,A` now walks title ->
   logo -> menu -> loading screen with no operator**, so dump and measure it first.

   Two facts already narrow it: the loading screen's tip text renders crisply in the
   same run, so the glyph pipeline is NOT broken and this is not §6r's swizzle defect
   returning; and the black areas are large filled rectangles, so it is not §6y's
   single-column coverage class either. The shape to check first is §6s's — a pass
   sampling a surface our renderer never wrote is served whatever the guest's allocator
   left there, and `CZ_VK_RESOLVE_TRACE` + `CZ_VK_SNAP_DUMP` is the per-pass dependency
   graph that turns "this pass is black" into "this pass's input was never produced"
   (gotcha 140).

2. **THEN THE PICTURE AS A WHOLE.** Every defect between the scene and the screen that
   this port knows about is fixed; the scene surface is 99.5% non-black with both tiles
   rendering. What nobody has done is compare the result against capture E with
   `tools/frame_signature.py` and say what is still wrong — colour, gamma, missing
   passes, the UI layer. Bind it by era, not by one frame: this title screen is TWO
   screens (gotcha 176), so measure every dumped frame and separate the eras with one
   `awk`. Note the current state: every dumped title-screen frame's best orientation is
   `identity` at +0.42..+0.55, none reaching the tool's +0.70 floor, which is what an
   animated camera looks like rather than a defect.

3. **The conservative extent is a placeholder for a real one, and it should be measured
   before it is improved.** `WriteScreenExtent` writes "this draw may have touched
   anything", which makes bin predication a no-op. That is correct output and it costs
   work: both tiles now execute ~975,000 draws where hardware executes ~573,000 each. If
   frame rate becomes the question, the honest improvement is to write the draw's real
   screen-space bound — which the renderer could compute from the transformed geometry,
   or approximate from the viewport — and the arm to judge it against is
   `CZ_PM4_NO_SCREEN_EXTENT=1` plus the census. **Do not do this speculatively.** The
   current cost has not been shown to matter.

4. **Item 2 from part 10/11's kickoff — the walker's dead `case 0x54:` INTERRUPT block
   and `MirrorIsPoisoned()` — is READY TO DELETE, and part 11 deliberately did not.**
   Re-measured on the current draw arm at `#83`: the walker's in-position INTERRUPT
   delivery prints zero lines and the poisoned-skip counter is zero. So part 10's
   objection (the zeros came from an arm that stalled at `#60`) no longer applies. The
   reason it is still here is gotcha 182 from the other side: part 11 changed what
   executes inside BOTH streams — 818,507 previously-dead packets a boot now write
   guest memory — and "this has always been zero" is not an argument you make in the
   session that changed the regime. **A session that changes nothing else should
   confirm the two zeros and delete both.** That is the cheapest item on this list.

5. **The kernel gates are exhausted as a forward oracle.** A1's position 93 is not the
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
* **A `cmake --build runtime/build` from INSIDE runtime/build fails, and `| tail -3`
  swallows the failure** — the pipeline's exit status is tail's, so a following `&&`
  proceeds and the A/B runs the OLD binary in both arms. That happened here and
  produced two identical arms; the tell is an arm whose "on" run matches its "off" run
  exactly (gotcha 151's signature).
* **`CZ_VK_FRAME_DUMP` does not create its directory**, and says nothing when it
  cannot write. `mkdir -p` first.

## New instruments and arms

```
CZ_PM4_NO_SCREEN_EXTENT=1        do not answer the GPU's screen-extent query — the
                                 pre-part-11 command processor, and the same-binary
                                 control arm for the right tile. Applies to pm4.cpp and
                                 d3d_draw.cpp together
CZ_VK_HALF_PIXEL=1               restore the -0.5 px vertex shift — the pre-part-11
                                 renderer, whose scene tile left column 639 BLACK and
                                 whose blur turned that into a visible line down the
                                 middle of the picture
CZ_FAKE_PRESS_SEQ=START,A,A      which buttons the synthetic-input arm sends, one per
                                 interval, holding the last. This is what makes the
                                 menu screens measurable at all
CZ_BINMASK_PROBE=1               now reports on a 15-second clock and covers all four
                                 inputs: the mask setter's callers, the fix-up pass's
                                 output histogram, its TWO INPUTS (tile rects + a census
                                 of the per-record screen extents), and the bin SELECT
                                 producer sub_8284A6D0
tools/xtr_bin_predication.py     --trace-window N --trace-arm-mask HEX prints the
                                 capture's own stream-order window, the twin of
                                 CZ_PM4_BIN_TRACE + CZ_PM4_BIN_TRACE_ARMMASK
```
