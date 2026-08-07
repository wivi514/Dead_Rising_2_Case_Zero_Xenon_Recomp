# D3D phase C, part 11 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §6w** first — it is the
record of the session this hands off from. `docs/d3d-translation-plan.md`'s
**"Phase C part 10"** is the same story in one page, plus the two items part 10 closed
and the one it deliberately did not.
`docs/d3d-phase-c10-kickoff.md` is the previous hand-off: its items 1 and 2 are done,
item 3 is explicitly declined (see below), item 4 is unchanged.

## What part 10 changed, in one paragraph

Part 9 handed over "the right tile is bin predication — a title does not emit 74,773
unreachable draws a boot, so either the bins are not the left/right split we assume or
the comparison is wrong", with the instruction to replay the rule over capture B1 first
because it needs no emulator. Replayed: **hardware discards 0.3% of this title's draw
packets and we discard 33%**, and in the capture both tiles are offered exactly 575,744
draws and each keeps 99.5%. The rule is right. What is wrong is the mask VALUE standing
at the right tile's draws — hardware `8000000F`, ours `80000000` — and `80000000` is not
a computed answer, it is the **placeholder** the draw emitters write as a literal, which
a fix-up pass in the D3D worker is supposed to overwrite with the real rect-vs-tile
intersection. On our runtime that pass runs once per boot and patches zero records,
behind a gate word whose bit 31 is clear. Separately, and for free: the phase C **draw
arm**, which part 9 could not re-gate, turns out to have inherited parts 8 and 9 and is
now in the healthy chain shape part 7 defined and the port had never reached.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`, 170 s headless boot:

* `--smoke` OK.
* A1: exact 84-prefix (this run hit the long-known position-71 permutation, 4 of 10
  historically).
* A5: **exit 0, 0 real windows**.
* `truncated=0`; deepest file **#83 `game:\data\skeleton\cinezombie.big`**.
* `draws=4,409,812 (predicated out=1,461,744)`.

Phase C draw arm, `CZ_D3D_DRAW=1` (NB it is mutually exclusive with `CZ_VKDRAW` and says
so loudly on stderr — a command line carrying both silently runs the PM4 arm):

| | part 8 | **now** |
|---|---|---|
| deepest file | #60 `models\zombies.big` | **#83 `skeleton\cinezombie.big`** |
| `ints/arms` | 12 : 856 | **1.000** |
| `walks == kicks == drains` | — | yes |
| `distinct` token buffers | 2 | **523-831** |
| engine counter `dev+0x2B04` | -552 | **0** |
| A1 | 82-prefix | **exact 84-prefix** |
| A5 | exit 0, 0 real | exit 0, 0 real |
| `truncated` | 0 | 0 |

## Where part 11 starts, in order

1. **FINISH THE MASK STORY — the placeholder explains 1,040,207 draws and not the other
   336,178.** Full record in `docs/phase5-notes.md` §6w. Established: `0x80000000` is a
   literal written by the three UP-draw emitters (82842A18, 82842DE0, 8284322C); the only
   rect-to-bin-mask computation in the image is `sub_8284A7F8`, reachable only through
   the D3D worker's token dispatcher `sub_8284B228`, gated on bit 31 of `[obj+0x164]`;
   and on our runtime that dispatcher runs **once** with the gate word at `00000000` and
   the pass patches **zero** records. NOT established: where the 97,115 draws carrying
   `8000000F` and the 239,063 carrying `80000003` come from, given that nothing patched
   them. Find that first — it either completes the story or refutes it, and both are
   cheap. `CZ_BINMASK_PROBE=1` is the instrument; `CZ_PM4_BIN_TRACE_ARMMASK=80000003`
   points the write trace at the era those values appear in.

   Then the real question: **who sets bit 31 of `[obj+0x164]`, and why does hardware's
   guest set it?** One token handler in the same dispatcher writes `0x7FFFFFFF` there
   (8284B3C4) — bit 31 clear — so something else must set it. This has the exact shape of
   part 5's display-controller gate (gotcha 154): one status bit, a whole subsystem
   behind it, and complete silence when it is clear.

   **A caution that is not optional here.** The gate being closed is entangled with
   part 7's finding that the D3D worker is never used at all for the healthy era on the
   PM4 arm. On the DRAW arm the worker now genuinely runs (`walks == kicks == drains`,
   `distinct=831`), so **re-run the bin census and the probe on the draw arm before
   concluding anything about the gate** — the two arms may not have the same answer, and
   part 10 measured the probe only on the control arm.

2. **Item 3 from part 10's kickoff is DECLINED, not forgotten.** Deleting the walker's
   `case 0x54:` INTERRUPT block and `MirrorIsPoisoned()` was recommended on the strength
   of zero counts measured on a draw arm that stalled at `#60`. That arm now runs to
   `#83` with a completely different chain shape, so those zeros describe a machine that
   no longer exists, and both are guards against a crash that was real (`ctr=0BADF00D`).
   Re-measure on the current arm — `CZ_VK_STATS=N` prints the walker's counters — and
   delete only if they are still zero there.

3. **The kernel gates are exhausted as a forward oracle.** A1's position 93 is not the
   next piece of work (finding 49, gotcha 107). Going further needs a gameplay comparison
   built from A2. Unchanged from part 9 and part 10.

4. **The picture.** §6w does not claim the right tile is fixed — it explains why it is
   empty. Nothing about the rendered frame changed this session, so part 9's state
   stands: the title screen's LEFT half is a complete, bright Still Creek and the right
   half is nearly empty. Judge any change to that with `tools/frame_compare.py` over the
   era, never by eye (gotchas 127, 133).

## Traps this session paid for — do not re-buy them

* **`CZ_D3D_DRAW=1 CZ_VKDRAW=1` is not the draw arm.** They are mutually exclusive; the
  runtime prints `CZ_D3D_DRAW DISABLED for this run` on stderr and proceeds as the PM4
  arm. A 170 s gate run was spent before that line was read. The tell in the log is
  `kicks=0 walks=0 drains=0` on a run that is supposed to be exercising the worker.
* **A stream trace armed at the start proves nothing here.** The first ~300,000 packets
  of our stream and B1's are packet-identical — same `MASK_LO` values, same order, same
  SKIP outcomes. The divergence is in the mature tiled era, a quarter of a million
  packets in. `CZ_PM4_BIN_TRACE_ARMMASK` exists for exactly that.
* **`--find-uses` on a 32-bit constant finds `lis`+`ori` pairs and nothing else.**
  `0x8000000F` is built by a loop, not a literal, so the scan reported zero sites and
  the value looked like it came from nowhere. Zero from a scanner is a detection
  failure, not a fact (gotcha 3), and the way out was to scan for the *idiom*
  (`oris rX, rY, 0x8000`) over an address range instead.
* **A backgrounded `cd X && cmd` from inside X silently runs nothing.** The `cd` fails,
  `&&` short-circuits, and the trailing `; echo done` still reports success. Two gate
  runs produced no log this way.

## New instruments and arms

```
tools/xtr_bin_predication.py    replay the ME bin-predication rule over a .xtr and
                                print the (mask, select) -> offered/skipped table
CZ_PM4_BIN_CENSUS=1             the same table for OUR run, on the ring trace
CZ_PM4_BIN_TRACE=N              now logs the mask WRITES in stream order too, not just
                                the draws, with (SKIPPED) marked
CZ_PM4_BIN_TRACE_ARM=hex        hold the budget until the bin SELECT first equals this
CZ_PM4_BIN_TRACE_ARMMASK=hex    hold the budget until the bin MASK first equals this
CZ_BINMASK_PROBE=1              the guest side: the mask setter's caller census, the
                                patch pass's output histogram (read back out of the
                                records, not recomputed), and the gate word above it
```

## Standing gate results to compare against

Both arms, this binary, default flags:

* `--smoke` OK.
* A1: **exact 84-prefix** on both (position 71 permutes on some runs).
* A5: **exit 0, 0 real windows** on both.
* `truncated=0`; deepest file `#83 game:\data\skeleton\cinezombie.big` on both.
* Both PM4 capture oracles (`pm4_packet_lengths.py`, `pm4_indirect_walks.py`) clean.
