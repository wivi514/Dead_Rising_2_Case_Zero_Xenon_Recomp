# Part 55 plan — make the port genuinely multi-threaded, and spread the load

The operator's instruction, closing part 54: *"For part 55 I want us to focus on making it
so the game properly use multithread and dispose of the load properly unless you tell me
it's not possible."*

This document is the honest answer to the "unless" first, and the plan second.

---

## 0. THE HONEST ANSWER: yes, but the ceiling is about 5-6 busy threads, not 16

**Possible, and it is the right target.** Three of the five largest costs on the critical
thread are pure functions of guest bytes with no renderer state, which is what makes them
movable. Part 53 already proved the machinery on this port: both content guards fold on
four workers a frame ahead, and the pool never once blocked the pump.

**Not possible: "use all 16 cores".** Two hard limits, and neither is an implementation
detail we can engineer away:

* **The PM4 walk is inherently SERIAL, because the stream's meaning is POSITIONAL.** A
  packet's effect depends on every register write before it; you cannot start in the middle
  of a command stream and know what state you are in. That is `WriteRegisterRun` +
  `ExecutePacket` + `ExecuteLinear` = **17.2% of the pump** which can be made *faster* but
  not *parallel*.
* **Draw ORDER is semantic.** Recording draws on several threads is exactly what Vulkan
  secondary command buffers exist for, and the frame must still be *submitted* in the
  order the guest issued it, because this title depends on overdraw order (the post chain,
  the two-tile split, alpha). Parallel recording is possible; parallel *submission* is not.

So the realistic shape is what CLAUDE.md already says: **3-6 busy threads, not 16.** The
process is at **3.75 of 16 cores** today and the pump is the critical path at ~94% of one.
A good outcome for part 55 is the pump falling to ~60% of a core with 5-6 threads busy —
i.e. the frame getting shorter, not the machine getting busier.

**And one warning that has to be read before any of it** (gotcha 344): moving work onto
idle cores RAISES total CPU. Part 53 measured 13.1 points leaving the pump and **33.2
appearing on the workers**, plus ~0.4 ms/frame coming straight back as cache pollution in
two phases that had nothing to do with the work moved. Budget three things per item: the
work that moves, the dispatch bookkeeping, and the cache the moved work stops warming.

---

## 1. WHERE THE TIME IS, re-measured in part 54

Pump thread, instruments off, outdoors in a crowd, **93.7% of a core**:

| symbol | share | movable? |
|---|---|---|
| `DoDraw` | **24.43%** | **yes** — parallel command recording (item A) |
| the NVIDIA driver, unsymbolised | **15.13%** | **yes, with the above** — it IS our `vkCmd*` calls |
| `UploadStream` | **12.84%** | probably — unexamined, see item C |
| `WriteRegisterRun` | 9.10% | **no** — serial by construction |
| `UploadTexture` | 8.69% | **yes** — parallel untile (item B) |
| `ExecutePacket` | 5.77% | **no** — serial |
| `SynthRectStream` / `ExecuteLinear` | 2.76 / 2.36% | partly / no |
| `_int_malloc` | 2.00% | reduce, not move |

**`DoDraw` + the driver is 39.6% of the pump and they are ONE item**: the driver's time is
the `vkCmd*` calls the recording makes, so moving the recording moves both.

---

## 2. THE ITEMS, in the order they should be taken

### Item A — parallel command recording (`DoDraw` + driver, ~39.6% of the pump)

**The largest thing left in this port by a wide margin, and the riskiest.** Vulkan
secondary command buffers, N workers each recording a contiguous RANGE of the frame's
draws, the pump executing them in order into the primary buffer at submit.

**Why it is risky and what makes it safe enough to try:**
* draw order is semantic → workers own *contiguous ranges*, concatenated in order. Never
  interleaved, never reordered.
* renderer state (bound pipeline, descriptors, scissors) is currently threaded through
  `DoDraw` implicitly → each range must begin by re-establishing full state rather than
  inheriting it, which costs a state re-bind per range and is the price of the item.
* **there is no gate that catches getting draw order wrong** — so the item needs one built
  FIRST: a per-frame ordered hash of (draw index, pipeline, vertex range) recorded by both
  the serial and parallel paths, compared, and any disagreement printed loudly. Same shape
  as part 53's slot-mix-up check, which read 0 over the whole part and is why that item was
  trustworthy.

**Expected: −3 to −5 ms at soak load. Risk: high.**

### Item B — parallel texture untile (`UploadTexture`, 8.69%)

Carried over from the part-52 plan, still unbuilt, and now unblocked twice over. Untiling
is a pure address swizzle with no Vulkan calls; the pump needs the *result* rather than a
decision, which is what makes it harder than the guard. Textures whose guard says
"unchanged" need no work at all, and that guard is itself pre-computed a frame ahead now.

**Price it against the MEMORY SYSTEM, not only the CPU it frees** — the guard pool already
moves 2.2-3.0 GB/s and part 53 measured that pressure doubling an unrelated 3.5 MB copy.

**Expected: −1 to −2 ms. Risk: medium-high.**

### Item C — split `UploadStream` (12.84%) before assuming anything about it

**Not in any previous plan, and the cheapest unexamined thing on the list.** Part 22 closed
the stream cache on the strength of `ProfScope(streams)` reading 0.0%, and the symbol says
that scope is not where the cost is — gotcha 343's shape, a scope is a region of code and
not a subsystem. Split it before deciding whether it moves, is removed, or is already
optimal.

**Expected: unknown, which is the point. Risk: none (it is an instrument).**

### Item D — the PM4 walk (17.2%), serial but not necessarily this slow

Cannot be parallelised. Can be made faster: it is a register-write loop over ~90,000
packets a frame, and part 20 measured 815,020 register dwords a frame at 15.3 ns each.
Item 4.2 of the old plan (inline the walk) has a **−2.2 ms ceiling** and is high risk. Take
it last, and only with a poison arm.

---

## 3. HOW IT MUST BE MEASURED — this part's own hardest lesson

**Take every A/B at the operator's soak load.** Part 54 priced an item across six
campaigns, all of which put their best-populated band at 2,500-2,999 draws while the
operator plays at 6,700-7,300 — and the headline was −29% where the truth at their load was
−3.5% (gotcha 355). A roam visits the heavy places briefly, so binning by draw count does
not save you: it produces bands with n=1 exactly where the answer lives.

* `tools/part54_chained_ab.sh` is the harness — a soak an arm, god mode and
  ZOMBIES IGNORE ALL HUMANS held in both so the camera cannot be moved by a grab.
* `tools/part54_fps_bins.py` matches windows on `draws med (min..max)` and discards any
  window that straddled two places.
* **Re-price items A, B and D at soak load before building any of them.** All three were
  sized off campaigns of exactly the kind gotcha 355 is about.
* Every parallel item needs its own **verify arm** and a **counter that says it engaged**
  (gotchas 151, 342, 346), and its BILL measured on the workers as well as the saving
  measured on the pump (gotcha 344).

---

## 4. WHAT "DISPOSE OF THE LOAD PROPERLY" LOOKS LIKE IF THIS GOES WELL

| | today | plausible |
|---|---|---|
| pump thread | ~94% of a core | ~60% |
| busy threads | 3.75 cores of 16 | 5-6 |
| frame at ~6,800 draws | 14.2 ms (70 fps) | 10-11 ms (~90-100 fps) |

That is the honest ceiling with the serial PM4 walk left in place. It is not 16 cores and
it was never going to be; it is roughly **a third off the frame at the load the operator
actually plays**, which is a far larger prize than anything part 54 delivered.
