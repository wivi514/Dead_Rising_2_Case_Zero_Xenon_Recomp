# Part 76 kickoff — the crowd frame after the write-combined read came out

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
> Lessons: gotchas **446-447**. There is still no live PLAN; §1 and §2 below are the board.

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

## 1. WHAT THE CROWD FRAME LOOKS LIKE NOW

**Re-measure before pricing anything** — that is the standing rule and part 75 is exactly
why: it inherited a four-column sketch from one marked frame and none of the four survived
contact with a banded re-baseline. `SECS=90 tools/autoroute.sh <tag> CZ_VK_PROFILE=10
CZ_VK_FRAME_TRACE=<file>` then band it; the trace now carries all twenty-one phase columns.

**There is no dominant term left.** Pre-fix the frame had one column at 36%; the remaining
columns at 5,000-7,000 draws were `recState` 1.48, `oFetch` 1.29, `drawOther` 1.25,
`recIdx` 0.86, `recVert` 0.82, `texPh` 0.79, `record` 0.71 — seven things of 0.7-1.5 ms
each. **That shape is the finding**: further per-draw shaving has no obvious single target,
which puts `perf-state-parked.md` **item A — parallel command recording** back at the top of
the board by default, and its ORDER GATE is built and proven (part 72) where it was the
blocker.

**Quote no phase number without its RESOLUTION.** Everything above is 3440x1440, which is
what the operator plays at and where the 21:9 patch path exists at all. At 2560x1440 the
same route profiles `constants` at 14.9% with `patch` at 1.6%, because `PatchWideProjection`
never runs. Two numbers from this renderer are not comparable unless both name their
internal resolution.

## 2. THE OTHER PROBLEM IS STILL UNTOUCHED

**Problem A — the post-load texture hitch — was not worked in part 75 and is unchanged.**
It remains fully specified in `part75-kickoff.md` §1 and it is the one the operator felt as
a real hitch: 150-305 ms frames, 82-90% texture upload + decode, **469 ms/run of decode
against 244 ms of staging+submit**. Take the decode half first (parallelise or cache; the
port already has a worker pool). Pre-registered kill: below 40 ms off the worst frame of
the route, do not ship that half.

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
  0.13 ms respectively. They are not the item and were never the item.

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

## 6. THE ONE THING TO CARRY FORWARD

Part 75's cost had been sitting in the largest column of the profiler for four parts, under
a name — "the per-draw ALU constant copy into mapped memory" — that described the *other*
thing in the same scope. Part 74 then shipped an optimisation against that name and the
column did not shrink in proportion, which is a free tell nobody read. **After you optimise
a phase, split it again in the same part.** And when you learn a property of one mapped
pointer, check every other mapping the same day — this project knew the write-combining fact
since part 17 and paid for it for 58 parts anyway.
