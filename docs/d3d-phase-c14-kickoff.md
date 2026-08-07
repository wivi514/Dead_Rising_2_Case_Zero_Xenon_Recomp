# D3D phase C, part 14 kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. Read **`docs/phase5-notes.md` §§6ab-6ad** and
**`docs/phase3-notes.md` finding 50** first — they are the record of the session this
hands off from. `docs/d3d-translation-plan.md`'s **"Phase C part 13"** is the same story
in one page. `docs/d3d-phase-c13-kickoff.md` is the previous hand-off: its **items 1, 2,
3 and 6 are closed or answered**, items 4 and 5 are unchanged and repeated below.

## What part 13 changed, in one paragraph

Part 13's list was two menu defects then the picture; the item listed LAST as a frontier
turned out to be the biggest. The malformed menu text was `VGT_INDX_OFFSET` — a register
this renderer printed and never applied — so every UI draw in the title rendered the
first run's vertices and the whole text layer was one run repeated; part 12's attribution
to the glyph atlases is retracted. The black panels are now a measured negative rather
than an inference: nothing in the process ever writes those bytes. And the SIGSEGV at
file #137 was the title's own `dbAssert("Bad file digest")`, three links deep
(XexGetModuleSection answering nothing, `.idata` skipped by name, and XeCrypt SHA-1 left
as stubs) — fixed, and the boot now runs past it to **#154 with zero faults**.

## The state you inherit, measured

PM4 control arm, `CZ_VKDRAW=1`, no input, one 120 s boot:

* `--smoke` OK. A1: exact 84-prefix. A5: exit 0, 2 windows, **0 real**.
* `truncated=0`; deepest file **#83 `game:\data\skeleton\cinezombie.big`**.

Same binary with `CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A`, 300 s:

* deepest file **#154 `game:\data\skeleton\childfullbody.big`**, **zero faults**,
  `truncated=0`. The screen is black there and the file count stops climbing.

Phase C draw arm, **`CZ_D3D_DRAW=1` ALONE** (the mutual-exclusion trap is still live),
one 170 s boot — re-gated at the end of the session, because part 13's fixes are all in
shared code and gotcha 181 says that is where they hide:

* announced itself (`[d3ddraw] Phase C draw service UP`), which is the check that
  distinguishes this arm from a PM4 run wearing its name.
* `arms=12741 ints=12740 isr=12740` (0.9999), `kicks == walks == drains = 6776`,
  `distinct=813` — the healthy shape part 7 defined. `truncated=0`; deepest file **#83**.
* A5: exit 0, 3 windows, **0 real**. A1: the long-known position-71 scheduling window
  (gotcha 86; it permutes 4-of-10 against 1-of-10 on both binaries), not a regression.
* `draw: VGT_INDX_OFFSET applied` reads **2,258** here, so the fix reaches this arm too —
  `d3d_draw.cpp`'s `SetReg` is generic, so the register lands in its private file by
  construction rather than by anyone having remembered it.

## Where part 14 starts, in order

1. **#154 IS THE FRONTIER: stalled, or just the next thing to implement?** The boot is
   71 files deeper than any gate this project had, nothing is crashing, and it stops in
   a run of `skeleton\child*.big`. Nobody has yet asked which. The instruments already
   exist and the order is the cheap one first: `CZ_WAIT_TRACE=1` and `CZ_CS_TRACE=1`
   name any wait that outlasts 5 s and any critical section a thread cannot get;
   `CZ_RING_TRACE=1`'s chain counters say whether the GPU hand-off is still healthy or
   has gone to `distinct=2`; `CZ_FILE_TRACE=1` says whether an open is FAILING rather
   than not happening. And gotcha 82's rule applies: a thread stuck in guest code is
   invisible from inside the runtime, so `gdb -p` plus `CZ_THREAD_TRACE=1` is the
   escalation — this session's worked example of that is in finding 50, and the runtime
   prints `runtime: guest memory at 0x...` unconditionally so no symbol lookup is needed
   to turn a guest address into a host one.

2. **THE BLUR.** The single largest visible difference from capture E3, and the only one
   of §6ad's four that changes the whole frame: our title screen is uniformly out of
   focus at every depth — the near sign and the foreground character included — where
   E3 is sharp except in the far distance. That is what a depth-of-field or bloom blend
   fed a wrong depth looks like: everything reads as "far". The pass is live and
   load-bearing (it is what turned gotcha 188's one black column into a 19 px band), so
   this is a wrong INPUT, not a missing pass. `CZ_VK_RESOLVE_TRACE` plus
   `CZ_VK_SHADER_CENSUS` names the pass and its shader; the capture ships that shader's
   disassembly beside its blob, which is gotcha 124's pair. Judge any change with
   `tools/frame_compare.py` over an era, never one frame (gotchas 127, 133).

3. **The other three picture differences** (§6ad): flat, green-shifted colour; the
   missing `(C) CAPCOM CO., LTD. 2010` line and community-watch sign lettering; the blank
   `GAS` balloon and absent street bunting. Note the two text items are worth re-checking
   FIRST and cheaply — part 13's `VGT_INDX_OFFSET` fix landed after those frames were
   dumped in only one of the two arms' worth of runs, and missing text is exactly the
   shape that register produces.

4. **The black panels' one remaining test: a run with a REAL save present** (§6ac).
   Everything else about that screen is measured — nothing writes `0364B000`, no resolve
   targets it, it is bound only on the save-slot screen, and the three slots are all
   empty. If a save makes the title write it, black-for-an-empty-slot is correct
   behaviour and the item closes. Remember gotcha 106: a save in the root changes the A1
   gate, so gate with an EMPTY `CZ_SAVE_DIR` and test with a populated one.

5. **The conservative screen extent is still a placeholder** (unchanged since part 11).
   `WriteScreenExtent` answers "this draw may have touched anything", which makes bin
   predication a no-op and costs work: both tiles execute ~975,000 draws where hardware
   executes ~573,000 each. The honest improvement is the draw's real screen-space bound,
   judged against `CZ_PM4_NO_SCREEN_EXTENT=1` plus the census. **Do not do this
   speculatively** — the cost has still not been shown to matter.

6. **The kernel gates are exhausted as a forward oracle** (unchanged since part 9). A1's
   position 93 is not the next piece of work (finding 49, gotcha 107). Going further
   needs a gameplay comparison built from A2 — and #154 is the first time this port has
   had a run that would exercise one.

## Traps this session paid for — do not re-buy them

* **A probe that prints one component of a vector has not printed the vector.** The draw
  probe showed a single dword per vertex for non-position attributes, which for a
  `32_32_FLOAT` texture coordinate is `u` alone. Two draws sampling different atlases
  produced transcripts that agreed on every printed value and disagreed on the one that
  mattered. It now decodes every component as a float and takes `CZ_VK_DRAW_PROBE_VERTS`,
  because one quad is one glyph and one glyph cannot show whether a run advances.
* **Gotcha 57 applies to BREAKPOINTS, not just to crash dumps.** `ctx.rN` read under
  `gdb` in the middle of a recompiled function is stale — two attempts to read a computed
  digest off the guest stack returned twenty zero bytes and a completely different code
  path's registers. At a function's ENTRY the values are fresh, which is what the
  `PPC_FUNC` alias seam gives for free. Hook the function; do not breakpoint its middle.
* **A hardware watchpoint on one alias cannot see a write through another.** This
  runtime maps the physical arena at `0xA0000000`, `0xC0000000` and `0xE0000000` from one
  memfd, deliberately, so the guest can convert pointers by arithmetic. A watchpoint has
  to be set on all three or its silence means nothing.
* **A skip list keyed on NAMES cannot state the condition it stands in for.** `.idata`
  was skipped for four sessions alongside `.reloc` under a comment describing a bounds
  condition that only `.reloc` meets, and that is what made the XEX's resources
  twenty-eight zero bytes. If you write a name list, write the check instead.
* **A guest ASSERT presents as a null-pointer crash.** XenonRecomp lowers `twi` to
  nothing, so the deliberate store that follows an engine's `dbAssert` is what faults and
  the crash reporter truthfully blames guest address 0. The tell is a `twi` immediately
  above the faulting store — and the assertion's own strings are two `lis`/`addi` pairs
  away (`tools/gdis.py`, gotcha 144).
* **Run timed arms serially** (gotcha 183) — unchanged.

## New instruments and arms

```
CZ_VK_NO_INDX_OFFSET=1        do not apply VGT_INDX_OFFSET — the pre-part-13 renderer,
                              in which every UI draw renders the first run's vertices and
                              the save-slot panel shows one overlapped garbled text run
CZ_VK_DRAW_PROBE_VERTS=N      how many vertices the draw probe prints per attribute
                              (default 4 = one quad = one glyph). The probe now also
                              decodes every COMPONENT as a float for float formats, and
                              carries the bound texture, its dimensions, and
                              VGT_INDX_OFFSET/min/max on its header line
CZ_DIGEST_PROBE=1             the file-digest check link by link: the verified name, its
                              buffer and length, the resource name the container asks
                              for, the engine's string hash RECOMPUTED IN HOST CODE (so
                              the probe is an oracle, not a description), and the 20
                              bytes SHA1_Final actually wrote
```
