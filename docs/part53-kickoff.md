# Part 53 kickoff — four items shipped, and the headless route has run out of headroom

Written at the close of part 52 (2026-08-17). **This is the LIVE hand-off**, superseding
`part52-kickoff.md`.

## START HERE

**The plan is still `docs/perf-plan-part52.md`** — four of its items are done, one of its
designs was refuted by its own verify arm, and its remaining items are unchanged and
still correctly ordered. Read it, then `phase5-notes.md` §6ci for what part 52 measured,
which corrects two of the plan's own numbers.

**Read §6ci §5c before planning any frame-time measurement**, because the thing that
changed most in part 52 is not an item, it is what can be measured at all: *the headless
outdoor route now sits on the frame cap for most of its length.* Both arms of an A/B land
on the rung and the comparison reads zero whatever the change was worth. That is an
UNMEASURABLE result, not a null one. `tools/part52_item_campaign.sh` raises
`CZ_FPS_CAP=120` in every arm to get around it, and what it then reports is the CPU
saving rather than a frame rate anybody sees.

**The operator's instruction is still current**: *"prepare a whole plan to fix CPU
performance issue and we'll start it in a fresh conversation."* Their two deferred picture
items (00m decals, 00n a sign and items at distance) remain deferred. **They have now
judged part 52 — "performance is better" — and their crowd frame reaches the 60 fps cap;
read "the operator has judged it" below BEFORE picking an item, because it changes the
question part 53 should ask.**

## WHAT PART 52 SHIPPED — do not re-derive any of this

| plan item | result |
|---|---|
| **1.0 `BindShader` memoization** | **`BindShader` 14.16% of the pump thread -> 0.00%.** ~2.4 ms at 6,000-7,000 draws, a lower bound |
| **2.1 `Count` -> `COUNT`** | ten sites, chosen by the counter DUMP; the plan's "28 sites in `DoDraw`" was counted by reading |
| **4.1 re-split `outside`** | one thread-CPU clock read; the pump is BLOCKED only **0.09-0.12 ms of a 16 ms frame** |
| **3.2 pipeline lookup** | **110-112 -> 38-43 ns/draw**, ~0.43 ms/frame, with every other `other` column unchanged to the digit |

### The finding that matters most is a method, not a millisecond

**The plan's memo key was refuted by the verify arm the plan itself insisted on.** It
specified `(va, size)` plus the first and last dword of the microcode, argued the probe
"catches the overwhelming majority of a re-upload", and argued the failure mode was
benign because a wrong hash would read as a cache MISS. On its first full outdoor run:

```
[pm4] SHADER MEMO MISMATCH #2: VS va=00000000 size=102 — memo said f2ef2d2f8de976d0,
      the microcode hashes to 8ed00911a7bc1eb1 (first=F1555004 last=A9A9C68D)
[pm4] SHADER MEMO MISMATCH #3: VS va=00000000 size=102 — memo said 8ed00911a7bc1eb1,
      the microcode hashes to f2ef2d2f8de976d0 (first=F1555004 last=A9A9C68D)
```

Two different shaders, same size, same both probe dwords, alternating. And the wrong
answer is **another real shader's hash** — which IS in the cache, so the renderer would
have bound a real, wrong, translated shader and drawn with it, past every gate this
project owns. **Gotcha 342: when a cache key is a probe rather than the content, ask what
the wrong answer IS, not just how likely it is.** The shipped memo compares the whole
microcode with `memcmp` — exact, and still ~30x cheaper than the hash, because the hash's
cost is a serial multiply chain and not a memory read.

Corollary worth as much: **write the verify arm even when the argument for the fix sounds
complete.** Nothing else would have found this.

### Two of the plan's own numbers are corrected

* **Item 2.1 is one site, not 28.** Of ~62.5 M plain-`Count` calls, 52,901,332 (84.6%)
  are the single site in `VkRenderer_Draw`. `VkRenderer_DumpStats` already prints every
  counter's call count — the exact statistic that ranks these sites — and reading the
  source instead ranks them by how alarming they look.
* **`BindShader` is ALU-latency bound, not memory bound.** `perf annotate` puts ~71% of
  its samples on four `imulq`s. That is the opposite of `GuardFold` (plan §0 fact 3,
  memory-bound at ~10 GB/s), and the two need opposite fixes. Check which you have before
  reaching for SIMD or for a memo.

## THE ORDER TO TAKE PART 53

`perf-plan-part52.md` §10 minus what is done. The symbol table **was re-taken after the
items landed** (§6ci §5b), so this ranking is current rather than inherited:

| # | item | expected | risk | note |
|---|---|---|---|---|
| 1 | **1.1 parallel content guards** | **−2..3 ms** | medium | `GuardFold` is now **29.85% of the pump**, up from 24.30% — its share ROSE because the thread got smaller, so convert to ms before comparing (gotcha 320) |
| 2 | 1.3 readback off the pump | −0.5..1 ms | low | price it from `readback` with frame stats OFF, never from `DoSwapImpl`'s symbol share |
| 3 | 2.3 audit the always-on censuses | −0.1..0.3 ms | none | |
| 4 | 3.3 `_int_malloc` on the frame path | −0.2..0.3 ms | low | part 52 removed ~1,500 mallocs/frame from the shader path as a by-product; it still reads 1.08-1.41% of the pump, so there is another caller |
| 5 | 2.2 frame-stats sampling | correction | none | |
| 6 | 1.2 parallel textures | −1..2 ms | med-high | only after 1.1 proves the pool |
| 7 | 3.4 `memcmp` at 3.4-4.2% | ? | — | **measure before touching**; part 52 added the memo's own `memcmp` to this symbol, so it is no longer only the state cache |
| 8 | 4.2 inline the PM4 walk | −2.2 ms ceiling | **high** | last, and only with a poison arm |

**Item 1.1 is still the headline and its ceiling is unchanged.** But note what part 52's
`outside` split says about the strategy behind it: the pump is blocked 0.09-0.12 ms of a
16 ms frame, i.e. it is *working*, not waiting. Moving work off it is therefore still the
right idea — there is nothing to overlap with, only work to relocate.

## THE OPERATOR HAS JUDGED IT — and their frame reaches the cap

Done at the close of part 52 (`phase5-notes.md` §6ci §10, `tools/part52_operator_session.sh`).
**"Performance is better."** A whole-map lap, `CZ_VK_PROFILE` on and `CZ_VK_FRAME_STATS`
deliberately off. The phase split against part 51's session on the same machine:

| phase | part 51 | part 52 | delta | predicted |
|---|---|---|---|---|
| `outside` | 5.37 | 3.02 | **−2.35** | ~2.4 (the memo — `BindShader` runs in the PM4 WALK, so its saving lands here, not in `draw`) |
| `other` | 3.92 | 3.48 | **−0.44** | 0.43 (the pipeline lookup) |
| everything else | | | within ±0.30 | untouched |

**Their crowd frame is ~14-16 ms uninstrumented — at or above the 60 fps cap.** Part 50
quoted them at 35.7 fps in that band, part 51 at 41.7. This is the first session where the
heaviest thing they walk through is not CPU-bound.

**It is not an A/B**, and the same-binary control was not run:
`ARM=ab tools/part52_operator_session.sh` chains an arm with `CZ_PM4_NO_SHADER_MEMO=1`.
Run it before quoting any of this as a measured speedup rather than as confirmation of
direction and mechanism.

## WHAT IS OWED

* **THE THING THAT SHOULD SHAPE PART 53: both routes now reach the cap.** The headless one
  does, and so does the operator's. Every remaining item in the plan is worth 0.2-3 ms of a
  frame that is already pacing-limited on both machines this project can measure on, which
  means **none of them can be shown to help a player** without either raising the cap (a
  measurement device, not a shipping configuration) or finding a heavier place than
  anybody has yet looked for. **Answer that before picking item 1.1.** The honest options
  are: find the worst thing in the game and measure there; accept that the remaining items
  buy headroom rather than frames and say so when shipping them; or stop optimising CPU
  and take the picture items.
* **A decision about the headless route.** It has run out of headroom at the shipped cap.
  Either every future CPU A/B runs at a raised cap and says so, or the route needs a
  heavier destination. The first is what part 52 did and it works; the second has never
  been looked for.
* **Item 1.3 is repriced by the operator's own session**: `readback` is **0.62 ms** with
  frame stats off, so it is a 0.5-0.6 ms item and not the 1.2 the old plan guessed.

## MEASUREMENT RULES THAT CHANGED IN PART 52

* **A capped frame cannot report a CPU saving.** Raise `CZ_FPS_CAP` in EVERY arm, and say
  that the number is a saving rather than a frame rate.
* **`frame_perf_bins.py`'s `pinned%` column is defined for a 16 ms ladder.** At any other
  cap it reads "on the second rung", not "on the ladder". Quote it with the ladder named.
* **A verify arm can wreck the statistic next to it.** Part 52's re-inserted into the memo
  on every load and reported 46x more evictions than misses until one comparison stopped
  it. Gotcha 7, one level in.
* **The shader-cache NAME diff must run in any part where `CZ_SHADER_DUMP` was set** —
  which for a performance part is every part, because the recon scripts set it. It found
  `ps_bd5d8eb053e36a84` this time: in the dumps, not in the cache, never bound by any run,
  so the miss counter read 0 and the COUNT matched at 435 = 435 with different members.

## STANDING STATE

* Runtime defaults unchanged from part 51: 60 fps cap, host vsync off, 100 us ring tick.
* **New arms**: `CZ_PM4_NO_SHADER_MEMO=1`, `CZ_PM4_VERIFY_SHADER_HASH=1`,
  `CZ_PM4_VERIFY_SHADER_POISON=1`, `CZ_VK_NO_PIPELINE_CACHE1=1`. All in
  `docs/instruments.md`.
* **New instrument lines**: `[vkprof] shader memo:`, `[vkprof] pipeline lookup:`,
  `[vkprof]   pump thread:`.
* **New tooling**: `tools/part52_item_campaign.sh`; `tools/part52_recon.sh` gains `ENVX=`
  and `NO_DWARF=1`, which makes it the A/B harness for an item rather than only a survey.
* **The shader cache is 436** (was 435).
* Artifacts: `~/DR2CZ-troubleshooting/part52/` — `p52i10_{memo,ctrl}.*` (the symbol A/B)
  and `frame/` (the frame-time campaign, 9 runs).
* **Gates at close: ALL CLEAN.** `--smoke`; both PM4 oracles on B1; the switch gate (0
  defects); the dimension census (0 disagreements); `no translated shader` = 0;
  `truncated=0`; deepest file #83 `cinezombie.big`; **A5 exit 0 with 4 permutation
  windows and 0 real** (part 51 also had 4); **E3 best of five +0.8771, 4 of 5 agreeing
  on layout** (part 51 +0.8043 of fourteen, part 50 +0.8820 of five — an animated
  backdrop, so read the spread and not the point, gotcha 133).
