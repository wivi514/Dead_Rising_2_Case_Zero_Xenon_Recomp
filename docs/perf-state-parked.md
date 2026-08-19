# Performance: the parked state, and how to resume it

**Written at the close of part 55, on the operator's instruction:** *"Save all of what is
needed for performance later on and all your finding. We'll switch to fixing the last few
visual bugs for the next few sessions and we'll come back to performance later."*

This document exists so that resuming costs a read and not a re-derivation. It supersedes
`perf-plan-part55.md` as the live performance plan; that document is kept because its §0
(what is possible) and §0b (the thread budget) are still correct and were executed.

---

## 0. WHERE THE FRAME IS, at the load the operator actually plays

All figures are **~7,000 draws in the heaviest place they know**, measured as chained
two-arm soaks in one session on their machine, each step against its own same-session
control. This is the only load worth quoting (gotcha 355).

| | frame | fps | delta |
|---|---|---|---|
| part 55 open | ~12.8 ms | ~80 | — |
| five flat containers | ~11.0 | ~90 | **−13%** |
| + the ALU constant memo | **~10.5** | **~95** | −2 to −3% |

**About −18% across part 55**, and **none of it from parallelism.** The plan's §0 predicted
"roughly a third off the frame" from three parallel items with a ceiling of 5-6 busy
threads; half of that arrived from DELETING work on the one thread instead. Gotcha 362 is
the general form: strategy (a), make the work smaller, outranks strategy (b), move the work
elsewhere, whenever both are available — because (b) has a bill and (a) does not.

The process uses **~3.3 of 8 PHYSICAL cores**; the pump thread is the critical path.

---

## 1. THE PUMP THREAD AS IT NOW STANDS

`tools/part52_recon.sh` + `tools/part53_symbols.py`, outdoors in a crowd, instruments off,
taken AFTER the container work (`~/DR2CZ-troubleshooting/part55/p55_flat3.*`):

| symbol | share of the pump | what it is |
|---|---|---|
| the NVIDIA driver, unsymbolised | **20.57%** | our `vkCmd*` calls |
| `DoDraw` | **19.88%** | the draw path's own work |
| `UploadTexture` | 11.87% | ...of which ~72% was container, now flat |
| `UploadStream` | 11.68% | ...of which 89% was container, now flat |
| `ExecutePacket` | 6.06% | serial by construction |
| `WriteRegisterRun` | 5.78% | serial by construction |
| `SynthRectStream` | 5.31% | |
| `ExecuteLinear` | 2.70% | serial |

**Read these as shares of a thread doing less total work than part 54's table.** A symbol
that did not change absolutely reads HIGHER here — the driver going 16.00% -> 20.57% is
everything around it getting cheaper, not the driver getting slower.

---

## 2. THE ITEMS THAT REMAIN, in the order they should be taken

### Item A — parallel command recording (`DoDraw` + the driver, ~40% of the pump)

**Still the largest single thing left, still the riskiest, and now the ONLY large item.**
Vulkan secondary command buffers, N workers each recording a contiguous RANGE of the
frame's draws, the pump executing them in order into the primary buffer at submit.

* draw order is semantic -> workers own *contiguous ranges*, concatenated in order. Never
  interleaved, never reordered.
* renderer state is threaded through `DoDraw` implicitly -> each range must begin by
  re-establishing full state rather than inheriting it. That is the item's price.
* **there is no gate that catches getting draw order wrong**, so the item needs one built
  FIRST: a per-frame ordered hash of (draw index, pipeline, vertex range) recorded by both
  the serial and the parallel path and compared. Same shape as part 53's slot-mix-up check
  and part 55's flat-cache verifier, both of which read 0 over their whole part and are why
  those items were trustworthy.
* it must ask `ThreadBudget_Take("record", N, nullptr)` for its workers — **not**
  `hardware_concurrency()` (gotchas 358, 359). On the operator's 8-core machine the whole
  budget is 3 and the guard pool already holds them, so this item either shares them or the
  budget policy needs revisiting with a measurement.
* **Expected: −3 to −5 ms at soak load. Risk: high.**

### Item B — parallel texture untile (`UploadTexture`, 11.87% of which the untile is part)

Carried forward, still unbuilt. Untiling is a pure address swizzle with no Vulkan calls.
**Price it against the MEMORY SYSTEM, not only the CPU it frees** — part 53 measured the
guard pool's 2.2-3.0 GB/s doubling the cost of an unrelated 3.5 MB copy, and part 55 showed
this renderer is already moving ~85 MB/frame of CPU writes.

### Item C — the constant copy, the half the memo does NOT reach

The memo serves the PIXEL window on ~61% of draws and the VERTEX window on **2.9%**, because
the guest rewrites a world matrix per object. So ~4 KB per draw is still copied
unconditionally, ~28 MB/frame at soak load.

**The unexplored idea, and it needs a census before it is built:** copy only the constants
the shader actually READS. The `.meta.json` sidecars carry `tfetchConsts`, `tfetchDims` and
`interpolators` but **no ALU constant usage**, so the census does not exist yet. It would
have to come out of the microcode or the SPIR-V at cache-build time, and it must handle
`a0`-relative (dynamic) indexing, which forces the full copy for those shaders. **Measure
the distribution of max-constant-used across the 439 modules before writing any runtime
code** — if the median shader reads 200 of 256, the item is worthless.

### Item D — the PM4 walk (~14.5% of the pump), serial but not necessarily this slow

Cannot be parallelised — a command stream's meaning is positional. Can be made faster; the
old plan's "inline the walk" has a **−2.2 ms ceiling** and is high risk. Take it last, and
only with a poison arm.

### Item E — geometry in VRAM, **REFUTED, do not re-buy without reading gotcha 363**

`CZ_VK_VRAM_STREAMS=1` measured **~14% SLOWER** at the operator's soak. The reason is
structural to recompilers and is written up in gotcha 363 and `reusability.md`. **The one
thing that would flip it is removing the per-draw constant upload** — i.e. item C — so
re-ask this only after item C lands, and not before.

---

## 3. HOW TO MEASURE, and the three ways this part got it wrong

**`tools/part55_chained_ab.sh` is the harness.** Both arms in one sitting, the operator
soaking one spot, `CZ_FPS_LOG` only. Arms are one environment variable each against the
shipped default: `flat`/`maps`, `vram`/`ram`, `memo`/`nomemo`. Adding an arm is two lines.

* **Take the A/B at the operator's soak load** (gotcha 355). The AutoChuck roaming campaign
  puts its best-populated bin at 2,500-3,000 draws while they play at 6,700-7,300. Their own
  framing, which is why the roaming campaign was abandoned mid-run: *"I'll do your campaign
  with soak at the spot that hit the cpu the most so we just get 2x 3minutes soak instead of
  hours of testing in unstable environment with autochuck."*
* **THE TWO SOAKS ARE NEVER AT THE SAME DRAW COUNT.** Every session in part 55 landed its
  arms 4-7% apart, and that alone moved a headline in both directions. Project one arm onto
  the other's draw count using each arm's OWN within-arm slope, quote both projections, and
  say which windows were used.
* **Compare an arm against its own control arm and nothing else** (gotcha 364). Part 55
  quoted −5.5% by comparing arm 1 against the PREVIOUS session's control; arm 2 said −2 to
  −3%. The pull is strongest when the older number is recent, from the same harness and at a
  similar draw count.
* **Pre-register the claim AND its kill threshold.** The constant memo's first design was
  killed by "below 30% this is not worth its risk", written before the run and measured at
  3.6-7.1%.
* **A hypothesis that splits a cost needs a per-part counter** (gotcha 365), or the
  explanation is a story that happens to sit next to a number.
* **A hot path can be too hot to instrument with a `ProfScope`** — 33,000 calls a frame
  against two 20 ns clock reads is 1.3 ms. Use `tools/part55_srcline.py`, which folds a
  `perf` profile by SOURCE LINE inside one symbol using the DWARF line table, at zero cost
  to the subject (gotcha 360).
* **Every parallel item needs a verify arm, a poison arm to prove the verifier can fire, a
  counter that says it engaged, and its BILL measured on the workers** (gotchas 30, 151,
  342, 344, 346).

---

## 4. THE ARMS THAT EXIST, all off by default unless noted

| variable | what it does |
|---|---|
| `CZ_WORKERS=N` | **the one thread knob.** Overrides the whole budget; `0` forces the serial path everywhere. Default `clamp(physical − 2 reserved − 3 committed, 0, 6)` |
| `CZ_VK_NO_FLAT_CACHE=1` | restores `std::unordered_map`/`std::map` for all five tables — the control arm for part 55's largest item |
| `CZ_VK_VERIFY_FLAT_CACHE=1` | both structures, every lookup compared (0 of 48.5 M disagreed) |
| `CZ_VK_VERIFY_FLAT_CACHE_POISON=1` | forces the check to fire (81.7%) |
| `CZ_VK_NO_CONST_MEMO=1` | the constant copy runs every draw — the memo's control arm |
| `CZ_VK_VERIFY_CONST_MEMO=1` | copy anyway into scratch, compare every dword (0 of 117,521) |
| `CZ_VK_VERIFY_CONST_MEMO_POISON=1` | forces it to fire (100.0000%) |
| `CZ_VK_VRAM_STREAMS=1` | geometry buffers in video memory. **Refuted: ~14% slower** |
| `CZ_VK_NO_PARALLEL_GUARD=1` | the part-53 guard pool off |
| `CZ_VK_GUARD_WORKERS=4` | restores part 53's measured worker count (the budget now grants 3) |
| `CZ_VK_NO_SWAPCHAIN=1` | the pre-part-54 readback present path |

Every run prints `[threads] machine: N physical cores ... -> budget N workers`, the memory
placement of each big buffer, `[vk] const memo: ...%` and `[vk] flat cache grows: ...` —
**quote those lines with any performance number**, because a measurement has a machine and a
configuration as well as a workload.

---

## 5. WHAT IS OWED, if someone picks this up cold

1. **A second soak pair for the constant memo.** −2 to −3% is near the resolution limit of
   one pair (the memo arm drifted ~1.5% within itself). The mechanism counters are solid;
   the frame-time figure is the soft one.
2. **A `CZ_VK_GUARD_WORKERS=4` vs budget-3 pair.** The thread budget silently changed part
   53's measured configuration and the cost has never been measured.
3. **Item A's ORDER GATE, before item A.** It does not exist and nothing else can catch that
   class of defect.
4. **The ALU constant usage census** over the 439 shader modules, before item C.
