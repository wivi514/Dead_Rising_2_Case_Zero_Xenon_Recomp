# Part 73 kickoff — performance, with an autonomous route and an item table rebuilt from it

> **THIS IS THE LIVE HAND-OFF**, superseding `part72-kickoff.md`.
>
> **The live subject is PERFORMANCE.** There are now THREE performance documents and this
> section is the only place that says how they relate — read them in this order:
>
> | document | what it is |
> |---|---|
> | **`docs/perf-plan-autonomous.md`** | **THE LIVE PLAN.** Work that needs no operator, on the route they authorised. Its §4 has already run. |
> | `docs/perf-plan-part72.md` | the ITEM TABLE (items 1-5 / A-E), carrying four part-72 retractions in place |
> | `docs/part72-fix-plan.md` | what part 72's operator sittings established, and what is owed |
> | `docs/perf-state-parked.md` | the reference the item designs come from — **not superseded** |
>
> Records: `phase5-notes.md` **§6de** (desk), **§6df** (the operator session), **§6dg**
> (item C + item A's blockers), **§6dh** (the session read). Lessons: gotchas **423-434**.

---

## 0. THE STANDING AUTHORISATION, and its four limits

**The operator authorised running the game myself on ONE route** (2026-08-23): *"just debug
jump to case 0-2 and once you are in game move the camera to right or left for 30 second to
try to reproduce stutter."* `tools/autoroute.sh` is that route and the only place it is
written down. It supersedes `operator-runs-all-verification` **for these runs only** — a
claim about their save, their load or their felt experience still needs them.

**Every number from it inherits four limits** (`perf-plan-autonomous.md` §0):

1. **It is a LIGHTER load** — 4,890 draws/frame measured, against their 9,750. No result
   here is a claim about their frame.
2. **It runs WINDOWED and must.** `readback` reads 0.0% on every headless run this project
   has ever taken and 8-23% windowed.
3. **It cannot see what they see.** Every picture verdict in part 72 came from their eye.
4. **One machine, one afternoon** — which is how gotcha 433 happened three times.

---

## 1. THE ITEM TABLE, current status

| item | status | who |
|---|---|---|
| **1. wide-culling over-widen** | **UNPRICED.** Census refuted itself, is fixed, and is **verified offline against hardware** (84.6% on-screen placed vs 12.2% unplaced, 12,560 draws, 20 traces). Never re-run in-engine. | **me** — 2 arms + null on the route |
| **2. pipeline cache** | **CLOSED.** Both claims retracted; position dominates and it is unmeasurable on a warmed machine. Prewarming is the only real answer. | — |
| **3. part-56 dynamic state** | **CLOSED.** 306 `vkCmdSet*` a frame = 0.006-0.031 ms. | — |
| **C. constant gather** | **SHIPPED and verified** — 0 disagreements of 17,948,265; **≈−0.8 ms** at their load; 297 GB not copied. | flicker verdict owed |
| **NEW: 41 near-empty passes/frame** | **LEAD.** 80% of passes hold 0.6% of draws. Cost unknown. | **me** — one clock first |
| **0w. menu/load stutter** | **RE-SCOPED.** Gameplay is smooth (`>2x med` 0.0%, 51 windows); every hitch is <2,000 draws. Slow-frame table built, **not yet run**. | **me** |
| **E. geometry in VRAM** | **RE-ASKABLE** — it lost because we re-uploaded constants every draw, and C cut that 62%. Arm exists, no new code. | **me** — 3 arms |
| **B. parallel texture untile** | unbuilt | — |
| **A. parallel command recording** | **RE-PRICED AND DEMOTED** — see §2. Blocked on a thread-budget decision. | **operator decision** |
| **D. the PM4 walk** | −2.2 ms ceiling, high risk, take last | — |

## 2. ITEM A: THE HISTOGRAM KILLED ITS DESIGN BEFORE ANYONE WROTE A RECORDER

`perf-plan-autonomous.md` §4 ran. 28,632 frames, 1.47M passes, 140M draws:

```
1.35 passes/frame carry 2048-4095 draws and hold 63.6% of ALL draws
the mean big pass is 2,306 draws = 47% of a whole frame
41 passes/frame are EMPTY or ONE draw and hold 0.6% of draws
```

**Whole-pass scheduling caps at ~2.12x regardless of worker count**, because one worker
owns half the frame. So item A must split WITHIN a pass — legal, but it pays the design's
own stated price (full state re-establishment per range) three times inside the big pass
instead of once. **Its 1.5 ms kill threshold is no longer obviously clearable.** It stays
last, and it is still additionally blocked: `ThreadBudget_Take` is first-come-first-served,
the budget is 3, the guard pool takes all 3, so a `record` pool is granted **zero**.

## 3. WHAT IS OWED BY THE OPERATOR — three things, none of them long

1. **The sky-flicker verdict.** They ran all three arms of `tools/part72_flicker_session.sh`
   and the verdict was never given. `nogather` / `fixed` / `prefix` — did each flicker?
   **`prefix` MUST flicker** or the fix is unconfirmed rather than confirmed. This is the
   only open question that could mean item C shipped a picture defect.
2. **The thread-budget decision** for item A — theirs, about their machine.
3. **The A5 kernel gate**, carried since part 67. No kernel path has changed in 67-72.

## 4. DO NOT RE-BUY

* **the pipeline cache A/B** — unmeasurable here, and it has now produced three opposite
  conclusions from three orderings (gotcha 433);
* **route (a) for the wide culling** — the engine has no aspect scalar; 2,056 property
  registrations, exactly one `Aspect`, on `cZombieSpawnRegion` (gotcha 424);
* **whole-pass parallel recording** — §2;
* **a range copy for the constants** — dead since Night Run 1; the GATHER is what shipped;
* **geometry in VRAM without item C** — that is why it lost (gotcha 363).

## 5. GATES AT PART 72's CLOSE — all clean

```
--smoke OK                          vcull predicate   18/18 (fails on 3 deliberate breaks)
order gate test  10/10              itemc selftest    11/11
vcull selftest   12/12              alu_const_gate    clean on all 16 caches
shader_dim_census clean             rt_world_xform    104 of 104, exit 0
play cache NAME diff empty          `no translated shader` = 0 on the autonomous run
```

**A5 is owed**, carried since part 67.

## 6. THE ONE THING TO CARRY FORWARD

Part 72 produced four retractions, **two of them of its own claims made the same day**. The
shape was identical every time: **the instrument worked exactly as built and was not
measuring what the decision turned on.** A cumulative mean where the steady state was 1.0
(428). A skip RATE where the population was 6% of draws (434). An arm ORDER where the
warming was monotone (433). A verifier whose scope was narrower than its feature's blast
radius — which only the operator's eye caught. **Before trusting a number, say out loud
what the decision turns on and check that the number is that.**
