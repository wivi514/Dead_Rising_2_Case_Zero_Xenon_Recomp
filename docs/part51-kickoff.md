# Part 51 kickoff — CPU performance, with the budget corrected and two items retired

Written at the close of part 50 (2026-08-16). **This is the LIVE hand-off**, superseding
`part50-kickoff.md`.

## START HERE

**The plan is still `docs/perf-plan-part50.md`** — part 50 executed tiers 1 and 3 and
instrumented tier 2, and the plan's structure survives. But **read `docs/phase5-notes.md`
§6cg BEFORE the plan**, because it retires two of the plan's items, refutes the
highest-confidence candidate inside a third, and corrects every number in the plan's
budget. Reading the plan first will send you at things that are not there.

**The operator's instruction is still current**: *"prepare a whole plan to fix CPU
performance issue and we'll start it in a fresh conversation."* Performance is the
subject. Their two deferred picture items (00m decals, 00n a sign and items at distance)
remain deferred and were re-confirmed as pre-existing in part 48.

## THE BUDGET IS SMALLER THAN THE PLAN SAYS — read this before quoting any number

**`CZ_VK_PROFILE` costs 2-4 ms a frame, 8-18%** (§6cg §6, three runs an arm, two of four
draw bands outside their own noise floor and all four pointing the same way). Every figure
in `perf-plan-part50.md` §1 — including the operator's whole-map lap — was read out of a
profiled run, because that is the only way to get a phase split.

| | the plan says | actually, in play |
|---|---|---|
| 5,000-7,000 draws | 28.3 ms, 35.7 fps | **~25-26 ms, ~39-40 fps** |

Rankings are unchanged, because every phase is inflated rather than one of them. What
changes is the distance: the plan's creditable intermediate of **20 ms is ~3 ms closer
than it believed**, and 16 ms is 9 ms away rather than 12.

**Never quote a frame time from a profiled run without saying so.** This project did it
from part 30 to part 49 and could not have noticed: a 32 ms pacing floor absorbs an 8%
inflation without moving. Part 49 removed the floor.

### AND WHAT PART 50 ACTUALLY DELIVERED IS ~0.4 ms — do not inherit the rest as a win

This matters more than it looks, because both of part 50's headline numbers move the
reported frame time in the same direction and it would be easy to bank them together:

| | ms | is it a speedup? |
|---|---|---|
| item 1a, shipped | **−0.4** | **yes** — but below this route's frame-time noise; its own A/B read **+0.0%** in every draw band, and the 0.4 comes from ns/packet |
| the profiler correction | −2.8 | **NO.** The player never paid it; nobody plays with `CZ_VK_PROFILE` set. It changed what we may CLAIM, not what the game does |

So part 51 starts from **~25.5 ms at 7,000 draws, of which part 50 earned 0.4**. Writing
"part 50: −3 ms" would be false in the way that matters and would compound — a win already
claimed cannot be won again. Gotcha 337.

**One consequence for how part 51 must measure**: the null floor here is **9.4% on
ns/packet and 8-18% on frame time by draw band**, both measured in part 50 rather than
assumed. An item worth under ~1 ms is invisible in frame time on this route and has to be
settled on a per-unit statistic (ns/packet, ns/draw, MB/frame). Budget for that before
picking an item, not after it reads as zero.

## What part 50 settled — do not re-derive any of this

* **Item 1a (filler runs) is BUILT, GATED AND SHIPPED, and it is worth ~0.3-0.5 ms, not
  the plan's 1.5-2 ms.** The plan's prediction of 20-30 ns/packet is refuted: measured
  4.0-6.5 ns against a **9.4% null floor** (base against itself). The sign holds because
  all three rounds order the same way and 12,267 calls a frame are provably removed.
* **Filler is 2.24 dwords per run, bimodal, and 0% of it is at ring level.** It is the
  title's own indirect buffers, not driver ring padding. `CZ_PM4_NO_FILLER_RUNS=1` is the
  arm; `CZ_PM4_VERIFY_FILLER_POISON=1` is the positive control and it produces 9
  truncations against a gate that requires 0.
* **Item 3 (`other`'s residual) IS RETIRED. It was this profiler.** A `ProfScope`'s
  constructor clock read lands in the PARENT's residual and nothing subtracts it, and
  `other` is DoDraw's outermost scope. Confirmed by a control that could have refuted it:
  `CZ_VK_PROFILE_EXTRA_SCOPES=8` moved the residual **205 -> 397 ns**, 24.0 ns per scope
  against a 21.6 ns calibrated read, and DoDraw's ~8 direct children account for 94% of
  it. **There is no frame time there** — it is absent from every unprofiled run. The plan
  called this "the highest-yield-per-hour item in the document".
* **Item 1c's top candidate is refuted for free.** "Hoist the wrap modulo in
  `Source::operator()`" is worth nothing: `INDIRECT_BUFFER` is **43-46 packets a frame**,
  so ~45 buffers carry all ~75,000 packets and every one is fetched with `wrapDwords == 0`.
* **Item 1c IS still real and is now priced by measurement rather than estimate.** Item
  1a's own difference prices one `ExecutePacket` call at **24-40 ns**, and because a filler
  packet returns early that is a LOWER bound. At 74,767 packets a frame that is **~2.2 ms
  of pure dispatch overhead**, inside the plan's guessed 2-3 ms. But it has **no single
  lever** — call, fetch, thread-local census read, two counter updates, switch, return, none
  dominant. See below.

## Item 2a — the largest item, now understood, and NOT what the plan or I thought

The guard reads 35.6 MB a frame. The split by promotion door (new in part 50):

| door | streams/frame | MB/frame |
|---|---|---|
| **proven** — `needsExact`, **unbudgeted and permanent** | **388-483** | **21.8-29.4** |
| speculative — dynamic, still accruing proof (budgeted) | 29-37 | 0.0 |
| probe — newly met (budgeted) | 10-12 | 0.0 |

**This refutes part 46's expectation of its own mechanism.** It wrote that `needsExact`
would be "the UI text buffer's small edits inside a large buffer and almost nothing else".
It is **15,643 of 126,536 store entries — 12.4%, rising monotonically window over window,
and the latch never unlatches.** A streaming world keeps meeting new large buffers.

**And it refutes the obvious fix, which is the part to internalise.** The argument that
writes itself is: a stream proven to change is one we are about to copy anyway, so hashing
it is an extra whole read to learn what the copy will tell us free — and always-copying is
cheaper AND safer, since a stream always copied can never be served stale. One counter
killed it: **only 11-13% of proven observations find a change.** The guard saves the copy
on ~88% of them. Gotcha 336.

**So the item is the narrow question the counters now pose: can a large buffer's change be
detected more cheaply than by reading all of it?** The sampled guard reads 16 KB of a
128 KB buffer and genuinely misses localized edits — the latch fires *correctly*. The
serious candidate is to stop asking the bytes and ask the kernel: **soft-dirty page
tracking** (`/proc/self/clear_refs` + `/proc/self/pagemap`) turns a 128 KB read into a
256-byte one and is EXACT rather than probabilistic. Costs to establish first, in this
order, because any one of them can kill it:

1. **What does `clear_refs` cost on a 4 GB flat guest map, once per frame?** It walks page
   tables. If that is milliseconds the idea is dead and the measurement is one run.
2. Does soft-dirty survive this kernel's THP/migration behaviour without false negatives?
   **A false negative is a stale mesh** — the exact defect class part 46 spent a session
   on — so it needs a positive control against the existing exact hash before it is
   trusted, not after.
3. Only then, the win: 26 MB/frame of hashing becomes ~50 KB of pagemap reads.

**Do not start this as a tightening.** It is architectural, it touches correctness, and it
deserves its own part with the exact hash retained as the oracle it would be replacing.

## WE ARE USING 2.5 OF 16 CORES, AND THE PLAN NEVER ASKED — read §6cg §7

Asked at the end of part 50 and never asked before: **is this a single-core problem?**
One 25 s sample of `/proc/PID/task/*/stat` in an outdoor crowd (`tools/part50_thread_cpu.py`):

```
process total   246.2% of one core = 2.46 cores of 16 (15.4% of the machine)
37 threads alive; TWENTY of them below 0.5%
```

| thread | % of one core | what it is, from its stack |
|---|---|---|
| — | **93.2%** | a GUEST thread — the title simulating. **Nearly saturated, and we cannot optimise it directly** |
| — | **79.0%** | **OURS** — `GraphicsInterruptPump` -> `Pm4_Execute` -> `ExecutePacket` -> `DoDraw` |
| — | 29.1% | a second guest thread |

Three consequences, none of which are in the plan:

1. **Our pump does the PM4 walk AND the Vulkan recording on ONE thread** — the stack shows
   `ExecutePacket -> DoDraw` directly. So `outside`'s walk (~8 ms) and the whole draw path
   (~15 ms) are serialised on one core **by construction**, and every item in the plan is
   an attempt to make that one core's work smaller. Legitimate, but not the only strategy,
   and nobody chose it deliberately.
2. **The busiest thread is the GAME'S, at 93.2%.** If the simulation thread is the real
   limiter then milliseconds taken off our pump buy **nothing**, and no item in the plan
   would ever find that out. **Resolve this first.**
3. **`outside` is not all work.** At 79% busy our pump is blocked ~4.7 ms of a 22.3 ms
   frame, and `outside` reported 10.7 ms. The plan reads the non-walk part of `outside` as
   "the guest's own simulation, ~3 ms" — but the guest simulates on a *different thread*
   and cannot be inside our pump's frame time except as blocking. That ~3 ms is our pump
   WAITING, so the walk item is smaller than `outside` makes it look.

**Caveat that must not be dropped: a thread's CPU% does not distinguish WORKING from
SPINNING.** A guest thread spinning on a lock looks identical here. The 93.2% is a lead,
not a conclusion (gotcha 338).

## The order to take part 51, given all of the above

| # | item | expected | note |
|---|---|---|---|
| **0** | **IS THE GUEST SIM THREAD THE LIMITER, AND IS IT WORKING OR SPINNING?** | one run + `perf` | **new, and it comes first because it can invalidate items 2-4.** `perf record -t <tid>` or repeated `eu-stack` samples of the 93% thread: a spin shows as one tight address range. If it IS the limiter, the plan's whole "make the pump smaller" strategy is mis-aimed |
| 1 | **item 2a step 1** — price `clear_refs` on the guest map | one run | can kill the biggest item in ten minutes |
| 2 | **parallelism** — get the walk and the recording off one thread | **re-cost it** | the plan's §5 defers multithreaded recording "for the same reasons" as the swapchain; §6cg §7 is the measurement that says what it might be worth. ~13 idle cores |
| 3 | **item 5** — present without the readback | −1.2 ms | the GPU is idle and can do the blit |
| 4 | **item 1c** — inline the walk so there is no call per packet | up to −2.2 ms | a REFACTOR with desync risk; both PM4 oracles are blind inside `ExecutePacket`, so the incumbent is the oracle and it needs a poison arm, as 1a's did |
| 5 | item 3's leftovers — `pipeline` std::map -> flat, shader-pair cache | −1.0 ms | these were always real; only the *residual* was the phantom |
| 6 | item 1d — does the guest sim run per PRESENT or per its own timer? | ? | `CZ_FPS_CAP` makes it a one-run experiment: compare `outside` per SECOND at 30, 45, 60. **Item 0 may answer this as a side effect** |

## Standing state

* **Runtime defaults are unchanged from part 49**: 60 fps (`CZ_FPS_CAP=30` is the control
  arm and reproduces the shipped pacing), host vsync OFF (`CZ_HOST_VSYNC=1` restores it).
  A player-facing option to choose the cap is still unbuilt, still later work.
* **New arms in part 50**: `CZ_PM4_NO_FILLER_RUNS=1`, `CZ_PM4_VERIFY_FILLER_POISON=1`,
  `CZ_VK_PROFILE_EXTRA_SCOPES=N`.
* **New instrument lines**, all in the `[vkprof]` window: `pm4 filler` (dwords, runs, mean,
  ring share, run-length histogram), `guard promotion by reason` (the three doors, the
  latched-entry ratchet and the proven change rate), and `instrument` (scopes/draw, the
  calibrated clock read, and what share of `other`'s residual is the profiler).
* **New tooling**: `tools/part50_campaign.sh` (one pinned binary, base as its own null
  control) and `tools/part50_profiler_cost.sh` (the profile-off arm).
* **Artifacts**: `~/DR2CZ-troubleshooting/part50/` — `campaign/` (base, nofiller and
  noprof, 3 runs each), `null.log`/`extra8.log` (the profiler control pair), `instr.log`.
* **Gates at close**: ALL CLEAN. `--smoke`, both PM4 oracles, the switch gate, the shader
  dimension census, `truncated=0`, and E3 **best of five +0.8820** with 4 of 5 samples
  agreeing on layout.

## Two process notes that each cost real time in part 50

* **`pgrep -f cz_runtime` matches the agent's own shell**, because the tool's command line
  contains the string. The campaign guard refused to start the campaign, having found
  itself. Match on `/proc/PID/comm` instead — it is the executable's name and nothing
  else — while still matching on a PREFIX, because part 49's trap (Linux truncates `comm`
  to 15 characters) is still there. `part50_campaign.sh` carries both notes.
* **A growing file read mid-run is a complete file that ends early.** An arm's stats
  ending at 96.7 s of a 330 s run, with a third of the frames, is the signature of a stall
  — and it was a file being written at that moment. Nothing in the file distinguishes the
  two. Check the driver's own completion marker before reading any artifact of a long run.
  Gotcha 339.
