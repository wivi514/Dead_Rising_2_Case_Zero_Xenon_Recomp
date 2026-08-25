# Part 76 kickoff — the crowd is flat now, and the priority order changed on the operator's evidence

> **THIS IS THE LIVE HAND-OFF**, superseding `part75-kickoff.md`.
>
> **Read `phase5-notes.md` §6dp first.** It is part 75 end to end: the re-baseline that
> disagreed with part 75's own kickoff, the two phase splits, the write-combining
> mechanism, the mapping audit, and the A/B method that replaced binning.
>
> | document | what it is |
> |---|---|
> | **`phase5-notes.md` §6dp** | **part 75 — the biggest cost in the frame was a READ from write-combined memory** |
> | `phase5-notes.md` §6do | the operator's marked stutters, and why there are TWO problems |
> | `phase5-notes.md` §6dn | the per-frame CPU/GPU profiler, and the texture decode |
> | `docs/perf-state-parked.md` | the reference the older item designs came from — still not superseded |
>
> | **`phase5-notes.md` §6dp §10** | **the operator's own crowd session — it set §1's order** |
>
> Lessons: gotchas **446-450**. There is still no live PLAN; **§1 is the board, in order.**

---

## 0. WHAT PART 75 DID, IN ONE PARAGRAPH

The operator picked **problem B — the crowd** out of part 75's two. `constants` turned out
to be 43-44% of a crowd frame; splitting it twice put essentially all of that in the fov /
21:9 **projection patch**, which read sixteen floats back out of the per-frame arena twice
per draw. The arena is `HOST_VISIBLE | HOST_COHERENT` with **no `HOST_CACHED`** — i.e.
write-combined — so each of those reads was an uncached round trip to DRAM. Patching a copy
taken from the cached register file instead is **−35% to −36% of the frame at 5,500-8,500
draws** (52 -> 81 fps at ~5,800, 37 -> 59 at ~8,200), verified byte-identical over
47,352,900 draws with a poison arm that fires on 100%. A pinned 16:9 control puts the whole
of it at **ONE 64-byte read per draw**: 0.030 us/draw where nothing reads the mapping
against 1.21 where one `SceneXformForm` does.

## 1. THE PRIORITY ORDER, AND IT WAS SET BY THE OPERATOR'S OWN SESSION

Part 75 closed with a play session at 3440x1440, the operator driving into the Main Street
crowd and pressing **F7** at every felt stutter (`tools/part75_operator_session.sh`; record
`phase5-notes.md` §6dp §10). 5,802 frames, 6 marks, all gates clean. **That session, not the
plan, is what ordered this list** — and it demoted the item this file previously put first.

### ITEM 1 — THE F8/F9 READBACK. Take this first: ~3.5 ms, near-zero risk, a few hours.

**`readback` measured 3.49 ms of a 23.31 ms crowd frame — 15% — and it is not the game.**
The swapchain (part 54) was supposed to delete this path. It survives exactly when a
PICTURE INSTRUMENT is armed, the predicate is a `static` read once at startup, and
**`tools/play_session.sh` sets `CZ_CAPTURE_KEY` and `CZ_BURST_DUMP` unconditionally so F8
and F9 work.** So every play session since part 54 has paid a **19.8 MB `memcpy` per frame**
(`Host_PresentPixels`, under a mutex) into a buffer the swapchain never displays, plus the
`vkCmdCopyImageToBuffer` that fills it — to make a key work that is pressed five times an
hour. Gotcha 450.

**The fix:** split `wantCachedPixels` into the instruments that need EVERY frame
(`CZ_VK_FRAME_STATS`, `CZ_VK_FRAME_DUMP`, `CZ_VK_SNAP_ON_BLACK/DARK`, `CZ_VK_SKY_ASYM`) and
the ones that are EDGE-TRIGGERED (`CZ_CAPTURE_KEY`/F9, `CZ_BURST_DUMP`/F8). The second group
becomes a per-frame dynamic flag: the key press arms the readback for the NEXT frame. One
frame of lag on a still screenshot is nothing, and an F8 burst is a second long.

**Watch the trap the existing comment names:** gating on "is an instrument armed" ships a
default path no picture gate exercises (gotcha: *a fast path no gate runs*). So keep a
control arm (`CZ_VK_PRESENT_ALWAYS=1`), and make one gate run WITHOUT a picture instrument.
**Pre-registered kill: below 2 ms off the crowd median at 3440x1440, do not ship it.**

### ITEM 2 — THE TEXTURE PATH. The only thing that still produces a felt stutter.

**All six F7 marks were this.** Splitting 607 crowd frames on whether a REAL upload
happened:

| population | n | median | p99 | worst | median `texPh` |
|---|---|---|---|---|---|
| no real upload | 413 | 22.3 ms | 31.9 | **34.3** | 1.39 |
| >= 1 real upload | 194 | 29.0 ms | **174.1** | **290.1** | 7.86 |

Of the 32 frames above 1.4x the crowd median, **29 (91%) carry a texture upload**. The
no-upload population **has no tail at all** — its worst frame in the whole session is
34.3 ms. Fully specified in `part75-kickoff.md` §1 and unchanged: **469 ms/run of decode
against 244 ms of staging+submit; take the DECODE half first** (pure CPU on the pump, and
the port already has a worker pool from part 53). Pre-registered kill: below 40 ms off the
worst frame of the route, do not ship that half.

### ITEM 3 — PARALLEL COMMAND RECORDING. **DEMOTED BY MEASUREMENT, not by preference.**

`perf-state-parked.md` item A led this board for four parts and this file put it first an
hour ago. **The operator session prices it out: the GPU is now 12.57 ms of a 23.31 ms
frame.** Every remaining CPU saving in the renderer, all of it, is worth at most **10.7 ms**
before the frame hits a GPU floor — and item A targets `DoDraw` plus the driver, a fraction
of that, at "high risk" by its own design note. Before part 75 the same arithmetic gave
~22 ms of headroom. **It is not dead; it is no longer obviously the best-priced thing, and
whoever picks it up must state the GPU floor in the same sentence as the expected saving.**

### ITEM 4 — THE GPU, which has never been looked at here.

12.57 ms of the frame and **54% of it**, against ~37% before part 75. This project has
never done a single piece of GPU-side work; `fence` is still 0.00 so it is not the limiter
yet, but it is now the ceiling every CPU item is measured against. `CZ_VK_RES` is the one
lever already built (part 51: resolution scaling is nearly free in crowds and expensive in
light zones).

## 2. WHAT THE CROWD FRAME LOOKS LIKE NOW

Median over 607 frames at >= 7,000 draws, operator session, 3440x1440, profiler on:

```
readback 3.49*  recState 2.31  oFetch 1.72  drawOther 1.66  texPh 1.50
recVert  1.35   recIdx   1.28  record 0.93  cShared 0.53   constants 0.63
cVsCopy  0.46   cVsPatch 0.43  oTail  0.39  oPipe   0.37   oBegin  0.36
                                     wall 23.31   GPU 12.57   fence 0.00
        * item 1 — this is the session's own instrument, not the game
```

**Part 75's whole constant path is 2.22 ms of that**, where the projection patch ALONE was
~10 ms at this draw count before. **There is no dominant term left**, which is why item 1
(a 3.5 ms instrument bill) outranks item 3 (a high-risk parallel rewrite).

**Re-measure before pricing anything.** Part 75 inherited a four-column sketch from one
marked frame and none of the four survived a banded re-baseline. The trace now carries all
twenty-one phase columns; band it, and quote the RESOLUTION with every number (§5).

## 3. THE AUDIT THAT IS NOW DONE, AND MUST NOT BE RE-DERIVED

Every mapped pointer in the process was enumerated and every access classified — the table
is `§6dp` §5. **The arena's projection patch was the only ungated read of write-combined
memory; everything else writes.** The readback buffers have carried `HOST_CACHED` since
part 17. Do not re-open this; do re-run it if a new mapping is added.

## 4. WHAT IS RULED OUT — do not start these

* **the GPU.** 7.93 ms mean, 0.37 ms fence wait, and all 10 of the operator's marks had
  fence 0.0. Part 75 did not change that.
* **the guest side / outside the renderer.** Residual is 0.0 ms on every hitch frame (§6dm).
* **reverting the RT era for performance** — 0.5-0.7 ms (§6dj).
* everything on part 73's list: the pipeline-cache A/B, route (a) for the wide culling,
  whole-pass parallel recording, a range copy for the constants, geometry in VRAM, the 41
  near-empty passes, a cheaper wait primitive for uploads.
* **the constant GATHER and the constant MEMO.** Both measured in part 75 at 0.36 ms and
  0.13 ms respectively, and the operator session puts the WHOLE constant path at 2.22 ms of
  a 23.31 ms frame. They are not the item and were never the item.
* **the crowd's steady-state frame as a source of STUTTER.** It has no tail: 413 crowd
  frames with no texture upload have a p99 of 31.9 ms against a 22.3 median and a worst
  frame of 34.3 in the entire session. It is throughput, it is now flat, and the felt
  stutter is item 2.

## 5. THE MEASUREMENT METHOD — READ THIS BEFORE RUNNING ANY A/B ON THIS ROUTE

**`tools/part75_ab_report.py` is the one to use.** Its docstring is the list of ways part 75
got this wrong, in order, and each entry is a trap the next version had to close:

* a 2,000-draw bin is too coarse (5,000-7,000 is a 40% range);
* **a line fit is WORSE, not better** — one control run never left a 90-draw range, so its
  slope was noise and its intercept came out negative. `part75_bandfit.py` is kept only
  because that failure is instructive; do not trust it without checking the x actually
  varied;
* **read the route gate, on a FINISHED log.** `autoroute.sh` exits 3 and says "DID NOT REACH
  THE OUTDOOR WORLD"; a run mid-write looks identical to one that failed. Never send an A/B
  loop's output to `/dev/null` — the gate fires there and nowhere else;
* **PIN `CZ_VK_RES` IN BOTH ARMS.** The desktop changed mode mid-campaign (3440x1440 ->
  2560x1440) and the two halves of one A/B then disagreed 33% vs 0%. Both were internally
  matched; they measured 21:9 and 16:9. `WideMode()` is `9W > 16H` on the internal
  resolution, so a whole renderer path exists at one and not the other. `[host] display 0
  is WxH` is in every log and says which;
* **report the per-arm GATE FAILURE RATE.** The slower arm polls fewer times inside the
  150 ms press window, so it misses the route more often and the surviving control runs are
  that arm's luckiest — part 75 saw 2 of 5 against 0 of 4. `PRESSMS=5000` fixes it;
* **never `pgrep -f "autoroute.sh"`** to wait on a run — it matches the waiting shell's own
  command line and three waiters deadlocked at once. `pgrep -x cz_runtime_auto`.

**The within-run profiled attribution is immune to all of it** — a phase column is measured
inside single frames and needs no cross-run matching. Prefer it, and use the A/B to confirm
rather than to discover.

## 6. HOW TO RUN AN OPERATOR SESSION

`tools/part75_operator_session.sh` is the harness part 75 built for exactly this and it is
worth reusing verbatim: profiler + all twenty-one trace columns + F7 marks + shader dump,
god mode and no death sequence (but **NOT** "zombies ignore all humans" — the crowd's
behaviour is the load being measured), and it PINS the resolution and shouts if the desktop
is not 21:9. Read the marks with the worst frame in the ~60 frames BEFORE each one; human
reaction is 200-500 ms and a mark names a neighbourhood.

**And it carries its own bill:** `CZ_VK_PROFILE` is 2-4 ms and `CZ_CAPTURE_KEY` costs the
readback in item 1. Quote SHAPE from such a session, never absolute fps — and if the
operator wants an honest frame rate, `tools/play_session.sh` is the run for it.

## 7. THE ONE THING TO CARRY FORWARD

Part 75's cost had been sitting in the largest column of the profiler for four parts, under
a name — "the per-draw ALU constant copy into mapped memory" — that described the *other*
thing in the same scope. Part 74 then shipped an optimisation against that name and the
column did not shrink in proportion, which is a free tell nobody read. **After you optimise
a phase, split it again in the same part.** And when you learn a property of one mapped
pointer, check every other mapping the same day — this project knew the write-combining fact
since part 17 and paid for it for 58 parts anyway.
