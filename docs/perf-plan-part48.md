# The performance plan for part 48 — written against the operator's own part-47 frame

Written at the close of part 47 (2026-08-16). **Supersedes `docs/perf-plan-part47.md`
as the live plan**; that document's budget was the part-46 frame and every one of
its tier-1 and tier-2 items is now built, so it should be read as history plus its
STATUS header.

Target, unchanged: **the Xbox 360 shipped this game at 30 fps, i.e. a 33 ms frame.**

---

## STATUS at the CLOSE of part 48 — **THE TARGET IS MET**; read this before the plan below

**The operator's own frame at the spot they name as worst is 33.6 ms and 29.8 fps at
~7,000 draws**, against 42.8 ms and 23.4 fps in part 47. The target this document
opens with — "the Xbox 360 shipped this game at 30 fps, i.e. a 33 ms frame" — is met.
`docs/phase5-notes.md` §6ce addendum is the measurement.

| item | state |
|---|---|
| **0 the operator's confirmation of the fold** | **CONFIRMED, to within 0.1 ms of the prediction: 6.9 ms measured against ~6.8 predicted.** It halves `rec.vertex` (776 → 1,547 ns/draw when undone), 61x the measured floor |
| 1a print the opcode census | **BUILT.** Found two things nobody predicted — see below |
| 1b per-thread census counters | **BUILT and verified** (0 of 135 counters disagree; the poison arm reports 1). **Measured at only ~3 ns/packet against a predicted 20-40** — real, consistent in 3/3 runs, and an order of magnitude smaller than the plan priced it |
| — the PM4 walk's `getenv` | **NOT IN THIS PLAN AT ALL, and it is the part's second-biggest win: 4.5 ms on the operator's frame**, 136 → 95 ns/packet. `ExecutePacket` called `getenv` once per type-3 packet, ~29,000 times a frame. Found by applying gotcha 329 — written the same afternoon, for the renderer — to `pm4.cpp` |
| 2b stream cache without the churn | **BUILT, MEASURED A LOSS, REVERTED** (d8068d7). Its own counter was perfect (97.7% node reuse, 45x fewer allocations) and the phase got 8.5% slower, because the map grew 1,900 → 7,000 entries and 22,000 lookups a frame paid for 1,800 cheaper inserts. Gotcha 330 |
| 2d isolate the bind cache | **NOT ESTABLISHED EITHER WAY, and specifically NOT the loss part 47 feared**: +0.8% on `record` against a ±2% floor, inconsistent across runs. Keep it; the headless route cannot separate it |
| §5 split `other` | **BUILT — and it REFUTED this document's own prediction. §5 below is wrong; see the correction** |

**§1a found two things this plan did not know**, both from counters that already
existed: **`SET_BIN_MASK_LO` is the most frequent packet in the entire stream** — a
third of all type-3 packets, half again as many as there are draws — and **28.7% of
every packet walked is type-2 ring FILLER**, which does no work at all and, before
item 1b, still paid two atomic read-modify-writes. That second number is most of
item 1b's justification and nothing in this plan had it.

**§5's prediction is REFUTED.** It said "expect the pipeline-key build and its
`std::map` lookup to be most of" `other`. Measured on the outdoor route at ~5,000
draws, `other` = 735 ns/draw = **key 40 + pipeline 118 + fetch 246 + residual 329**.
The probe is **16%**; the residual is 45% and the fetch walk is second. The
within-phase ranking in §5 should be read as a list of candidates, not a priority
order (gotcha 328 — a plan can only name suspects that *have* names, so it is
systematically biased toward the named component).

**And the split found a real item within minutes of printing**: a `getenv` and an
`snprintf` on the per-draw path in the alpha-to-mask block. `otherFetch` 246 → 119
ns/draw, `other` 735 → 571; at the operator's ~7,000 draws that is ~1.15 ms.
`other`'s residual is now split three ways again (shader / begin / tail) and that
reading is owed.

---

## 1. THE BUDGET — where the operator's frame goes NOW

Their own session, `~/DR2CZ-troubleshooting/part47-operator/`, matched on draw
count against the pre-47 arm of the same binary. **This is the only budget that
decides anything**: the headless route understates their draw path by about a
factor of two and, as part 47 discovered, does not even submit the same packet
mix (below).

At **~7,010 draws, 42.8 ms (23.4 fps)**:

| phase | ms | what it is |
|---|---|---|
| **`outside`** | **16.61** | the PM4 walk — 81,533 packets/frame at **144 ns each** |
| **`record`** | **15.19** | **2.17 µs per draw**; its `vertex` section is 70% of it |
| `other` | 4.19 | DoDraw's own untimed work — **uninstrumented** |
| `textures` | 4.45 | was 25.19; item 1.1 closed it |
| constants + readback | 2.2 | |
| `submit` / `streams` | ~0 | the GPU is not the wall and the store is closed |

For comparison, part 46's profile of the same machine was **61.7 ms at 7,231
draws** with `textures` at 26.5 ms.

**One change has landed since that measurement and has NOT been measured on their
machine**: the guard fold's four lanes (9.0 → 35.7 GB/s). Headlessly it takes
`record` from 1,636 to 1,198 ns/draw; on their 81.65 MB/frame of guard hashing it
predicts **~6.8 ms**, i.e. `record` ~8.4 ms and the frame ~36 ms.
**Confirming that is action zero.**

---

## 2. THE FINDING THAT SHOULD SHAPE EVERYTHING BELOW

**Their frame and the headless route are not the same workload, in a way that
goes beyond "theirs is bigger".**

| | operator | headless |
|---|---|---|
| ns per PM4 packet | **144** | 110-113 |
| register dwords per packet | **7.8** | 9.4 |
| `record` per draw | **2.17 µs** | ~1.3 µs |

Same binary, same commit. Their packets carry 17% fewer register dwords, so they
submit proportionally **more non-register packets** — which means part 47's bulk
register path (100.0% engaged, 19.0 → 12.1 ns per dword) buys them less than it
bought the headless route, and **what dominates their walk is per-PACKET cost, not
per-dword cost.**

Every item below is therefore ranked on THEIR budget, and every measurement of a
walk change should be quoted as **ns per packet**, which is normalised by the
work, rather than as `outside` in milliseconds — the arms do not submit the same
stream, so a matched draw band does not match a PM4 workload (gotcha 321's shape,
one subsystem over).

---

## 3. TIER 1 — the PM4 walk, 16.6 ms

### 1a. FIRST: PRINT THE OPCODE CENSUS. It is already counted and read by NOTHING.

`Pm4_OpcodeCount(opcode)` and `Pm4_TypeCount(type)` have existed since phase 4,
are incremented on every packet, and are **called from nowhere in the runtime**.
So "which packets cost the 16.6 ms" is unanswerable today while the data sits in
memory — the same gap `record` had until part 47 split it, and the same gap the
walk itself had before it got its packet census.

**Do this before any walk optimisation**, because the operator's packet mix is
the thing that differs and nothing currently describes it. Differenced per
profile window like every other rate on those lines, printed as counts and as a
share, with the type-0/1/2/3 split alongside. It is perhaps twenty lines and it
costs nothing, since the counting already happens.

**It may well retire item 1b or promote something not on this list.** Write it,
run one operator session, then rank.

### 1b. The census counters are FOUR ATOMIC RMWs PER PACKET — EXPECTED 2-3 ms

`ExecutePacket` does `g_packets.fetch_add`, `g_types[type].fetch_add`,
`g_opcodes[opcode].fetch_add` and `g_regWrites.fetch_add`, plus `g_draws` on draw
packets. Every one is a `lock xadd`, ~20 cycles on x86 even uncontended. At the
operator's **81,533 packets a frame that is roughly 326,000 atomic RMWs**, and it
is pure instrumentation sitting on the hottest loop in the runtime — the same
defect class as part 47's items 1.2 and 1.3, one subsystem over (gotcha 230).

**The atomics are not decoration and must not simply be removed**: the walk runs
on the graphics pump and `[vkprof]` differences these from the renderer's thread.
The fix is **per-thread counters aggregated at read time** — a small
`struct { uint64_t packets, types[4], opcodes[128], regWrites, draws; }` per
walking thread, summed in the accessors. Same numbers, no bus locks.

**Prediction**: ns-per-packet falls by 20-40 on their mix. **Control arm**:
`CZ_PM4_ATOMIC_COUNTERS=1` restores the current form. **Verify** the totals are
identical to the atomic version's over a full run — that is the correctness check
and it is free.

### 1c. Whatever 1a names, and the two structural questions behind it

The walk's remaining per-packet cost after 1b is the opcode dispatch itself and
the per-packet bookkeeping (`g_constWatchSource`, the type decode, the bounds
checks). Two cheap things worth measuring once 1a says which opcodes dominate:

* **Hoist the dispatch for runs of identical packet types.** Only worth it if 1a
  shows long runs, which is exactly what 1a is for.
* **Audit the common path for allocation and `std::string` work**, the same
  audit part 47 applied to `UploadTexture`. `g_constWatchSource = "..."` is a
  pointer store and fine; check the rest.

---

## 4. TIER 2 — `record`, ~8.4 ms after the fold fix

### 2a. RE-MEASURE FIRST. The split is in and the fold has changed it.

`record` is now printed as four numbers with ns-per-draw
(`state + vertex + index + residual`). Before optimising anything here, take one
operator window and read it — the fold fix removed most of what `recordVertex`
was doing, so the ranking inside `record` is very likely different now.
Headlessly, post-fold: **1,198 ns/draw = 153 state + 618 vertex + 238 index + 170
residual**.

### 2b. The stream cache is probed ~33,000 times a frame — EXPECTED 1-2 ms

`R->streamCache` is a `std::unordered_map<uint64_t, StreamLoc>` that is
`clear()`ed every frame and refilled — **3,164 first-touch inserts a frame** on
the operator's session against ~33,000 lookups. A node-based map means an
allocation per insert, a free per clear, and a pointer chase per lookup.

**The change**: stop clearing it. Give each entry a frame stamp — exactly the
`guardFrame` pattern part 47's item 1.1 used — so a lookup whose stamp is stale
is a miss and is overwritten in place. No node churn at all, and the map settles
at the largest frame's working set (~3,200 entries, tens of KB).
**Measure the insert and lookup rates first**; both counters exist.

### 2c. `recordVertex`'s per-attribute walk

What is left after 2b is `DecodeVertexFetch` + `PhysToVa` + `GuestRangeOk` +
`BindVertexBufferCached` per attribute, at 3-5 attributes a draw. Cheap
individually; worth a look only if 2a still shows `vertex` dominating.

### 2d. Isolate the vertex/index bind cache — OWED, and it may be a LOSS

**This is the one part-47 change never A/B'd on its own.** `record` came out
~1 ms higher on the part-47 arm in BOTH the headless run (7.15 vs 6.76, inside a
22.8% floor) and the operator's (15.19 vs 14.20), and while neither is
established — a matched draw COUNT is not a matched draw COMPOSITION — the sign
is consistent and this is where a net loss would show. `CZ_VK_NO_BUFFER_BIND_CACHE=1`
exists precisely for it. Three runs an arm, one variable. **If it is a loss,
delete it**; the repeat rates that justified it (51%/39%) predict a saving only if
`vkCmdBind*` is dearer than the comparison that avoids it, and that was assumed
rather than measured.

---

## 5. TIER 3 — `other`, 4.19 ms and uninstrumented

Exactly the state `record` was in before part 47 split it, and the split is what
found the guard. `drawOther` is documented as "register decode, the pipeline-key
build and its lookup, the fetch-constant walk, and the always-on censuses" — four
things wearing one number, one of which (`GetPipeline`) already has its own
counter for creation but not for lookup.

**Split it the same way**, and expect the pipeline-key build and its `std::map`
lookup to be most of it: `std::map<PipelineKey, VkPipeline>` is a red-black tree
probed once per draw, which is the same shape as the sampler `std::map` part 47
turned into a flat table.

---

## 6. TIER 4 — the two items deliberately left alone

* **`CZ_VK_TEX_GUARD_BYTES=N`, the bounded-prefix lever inside item 1.1.** It
  exists, **its default is unchanged at 16384**, and the histogram that prices it
  prints with the stats. It is the only remaining texture-guard option that trades
  DETECTION for cost, `textures` is already down to 4.45 ms, and the operator has
  not reported a stale texture — so there is no reason to spend detection on 2-3 ms
  until everything above is done. If it is ever taken, choose the bound off the
  histogram and the per-address `changed` table (which now carries each texture's
  source size), and confirm with an operator session.
* **Multithreaded command recording** (part 47's plan §3.2). Still last, for the
  reasons that document gives: invasive, changes the ordering the resolve/snapshot
  logic relies on, multiplies every per-draw bug. Its prerequisite — a per-draw
  path free of shared mutable state — is *further* away now, not nearer, because
  part 47 added a per-frame stamp to the texture cache and per-command-buffer bind
  tracking. If tiers 1-3 land, the frame should be near 30 ms and this is not
  needed.

---

## 7. THE ORDER, and what it should add up to

| # | item | expected | cumulative (operator frame) |
|---|---|---|---|
| — | measured baseline | — | **42.8 ms** (23.4 fps) |
| 0 | the guard fold's four lanes (built, unmeasured there) | −6.8 | **36.0** |
| 1a | print the opcode census | 0 — it is an instrument | 36.0 |
| 1b | per-thread census counters | −2.5 | **33.5** |
| 2b | stream cache without the per-frame churn | −1.5 | 32.0 |
| 2d | isolate the bind cache | −1 to +1 | 32.0 |
| 5 | split `other`, then act on what it names | −1.5 | **30.5 ms → 33 fps** |
| 1c | whatever the opcode census names | ? | ? |

**Everything above tier 4 is mechanical and individually verifiable.** The frame
does not need the architectural item to reach 30 fps if these land.

---

## 8. HOW TO MEASURE ANY OF THIS — the rules part 47 paid for

* **Quote MILLISECONDS, not phase shares** (gotcha 320). Taking 13 ms out of
  `textures` made four untouched phases read 34-68% worse.
* **Restrict profile windows to a matched draw band** (gotcha 321). Pooling across
  a route reported a 58% "noise floor" that was a safehouse window averaged with a
  crowd window.
* **For the WALK, quote ns per packet, not `outside` in ms.** The arms do not
  submit the same stream.
* **Read the 16 ms-pinned share** (gotchas 237/238). It went 5-13% → 73-85% and
  that is what "the frame stopped being CPU-bound" looks like.
* **Every item gets a same-binary control arm and a counter proving it engaged**
  (gotcha 151), and each lands in **its own commit**.
* **A gate that would pass whether or not your change is correct has not tested
  it** (gotcha 322). Both PM4 boundary oracles are in that position for anything
  inside `ExecutePacket`; the incumbent implementation is the oracle, as
  `CZ_PM4_VERIFY_BULK_REGS` does — and give it a poison arm first.
* **The operator's session is not a confirmation step, it is the measurement.**
  `tools/part47_operator_session.sh`, two chained arms, instruments wired in.
  Run `tools/part47_gates.sh` before handing them a binary.
