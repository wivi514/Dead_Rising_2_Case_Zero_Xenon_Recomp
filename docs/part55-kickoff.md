# Part 55 kickoff — the present stopped copying the frame, the headless route is honest
# again, and the biggest thing left on the pump is the draw path

Written at the close of part 54 (2026-08-18). **This is the LIVE hand-off**, superseding
`part54-kickoff.md`. The record is `docs/phase5-notes.md` **§6ck**; the live plan is still
`docs/perf-plan-part52.md`.

---

## START HERE — three things changed, and the second changes how you measure

1. **`CZ_VK_SWAPCHAIN=1` presents the frame where it already is** (plan §7). `readback`
   goes to 0.0%. It is an ARM, not the default, and the reason is in §3.
2. **THE HEADLESS ROUTE IS OFF THE PACING RUNG AND THE PUMP IS SATURATED THERE.** 93.7% of
   a core, where part 53 closed at 50.3%. The frame-cap default moving 60 → 500 did that,
   and it **retires §6ci §5c's warning** that a headless A/B here reads zero whatever the
   change was worth. Headless measurement is a valid method again.
3. **The route to the outdoor world was a coin flip and is fixed.** A synthetic host debug
   edge (`F2`) was delivered only if the guest polled input inside a fixed 150 ms window.
   On a miss the whole DebugJump recipe silently degraded. Gotcha 349.

---

## 1. THE SYMBOL BUDGET, RE-TAKEN — read this before choosing an item

`tools/part52_recon.sh` + `tools/part53_symbols.py`, outdoors in a crowd, instruments off.
The pump thread is **26.5% of the process's cycles and 93.7% of a core**:

| symbol | share of the pump | which plan item |
|---|---|---|
| `DoDraw` | **24.43%** | **1.4** parallel command recording |
| the NVIDIA driver, unsymbolised | **15.13%** | 1.4 as well — it is our `vkCmd*` calls |
| `UploadStream` | **12.84%** | not in the plan; see below |
| `WriteRegisterRun` | 9.10% | 4.2 inline the PM4 walk |
| `UploadTexture` | 8.69% | **1.2** parallel texture untile |
| `ExecutePacket` | 5.77% | 4.2 |
| `SynthRectStream` / `ExecuteLinear` | 2.76 / 2.36% | — |
| `_int_malloc` | 2.00% | 3.3 |

`GuardFold`, a quarter of this thread at the start of part 53, does not appear at all.

**Two readings that are not in the table.** `DoDraw` plus the driver is **39.6% of the
pump** and they are the same item: recording draws in parallel moves both, because the
driver time IS the `vkCmd*` calls the recording makes. That is item 1.4, still the largest
single thing left and still the riskiest — draw ORDER is semantic and no gate here would
catch getting it wrong. And **`UploadStream` at 12.84% is not in the plan at all**: part 22
closed the stream cache on the strength of `ProfScope(streams)` reading 0.0%, and the
symbol says that scope is not where the cost is (gotcha 343's shape — a scope is a region
of code, not a subsystem). Splitting it is the cheapest unexplored thing on the list.

---

## 2. WHAT THE SWAPCHAIN IS WORTH, AND AT WHICH RESOLUTION

`readback` was **never measured windowed before part 54**, because `Host_PresentPixels`
returns immediately with no window and every performance run this project has taken is
headless (gotcha 352). Windowed, at ~2,900-3,700 draws:

| | 1280x720 | 2560x1440 |
|---|---|---|
| `readback` | 8.1-8.7% of the frame (~0.65 ms) | **16.4-22.6% (~1.7-2.2 ms)** |
| `submit gpu` | 0.0-2.8% | **2.1-14.7%** — the GPU is a limiter at 2x |
| the window thread | 8.8% of a core | **15.0%** |

**What it is worth**, three rounds an arm, alternated, one frozen binary, read as medians
per draw band with `tools/part54_swap_bins.py`. The best-populated band (2,500-2,999
draws, n=7-9 an arm), with the campaign's own within-arm null beside it:

| | base | swapchain | delta | null |
|---|---|---|---|---|
| **1280x720** | 7.20 ms | 6.60 ms | **−8.3%** | −0.7% |
| **2560x1440** | 10.20 ms | 7.00 ms | **−31.4%** | +0.0% |

**It is a SLOPE the other way round from every previous item.** Parts 52 and 53 shipped
savings that grew with the draw count, because their work ran per draw or per packet. This
is a FIXED cost per frame, so its share is largest where the frame is otherwise lightest:
at 2x it is **−50.0% at 500-999 draws and −17.8% at 5,000-5,499**. Quote the draw count,
and expect the trend to run the other way.

It also removes **more than the phase it zeroes**: at 2x, zeroing a 24.5% `readback` out of
10.20 ms predicts 7.70 and the measurement is 7.00. The extra 0.7 ms is the two copies no
instrument here charges to anything — the GPU's image-to-buffer copy, and the pump waiting
on `g_frameMutex` while the window thread runs `SDL_UpdateTexture` under it.

The picture is right at both scales, against Xenia's own screenshot: **+0.8831 at 1x**
(21 of 38 agreeing on layout) and **+0.8741 at 2x** (16 of 32), against the E3 gate's own
standing +0.8396…+0.8808. Vulkan validation is clean.

---

## 3. THE OPEN DECISION: DOES THE SWAPCHAIN BECOME THE DEFAULT?

It is an arm because of one thing and one thing only: **the host-rendered F4 debug overlay
is drawn by SDL's renderer, and a Vulkan window has no SDL renderer.** The title's own F2
DebugJump screen is drawn by the GAME and is unaffected, as is every other instrument.

Three ways forward, in the order they should be considered:

1. **Ask the operator to judge it** — `CZ_VK_SWAPCHAIN=1 tools/play_session.sh`, ideally
   at `RES=2560x1440` where the saving is largest and where they already play. Their
   verdict on look and feel is the input this arm does not have, exactly as it was for the
   A2M foliage mode (part 46) and the resolution knob (part 53). **This is the cheapest
   next step in the whole part and it should happen before anything is built on top.**
2. **Port the overlay** — rasterise the glyph table into a small RGBA image and blit it
   over the swapchain image before presenting. ~120 lines, no new dependency, and it is
   the only thing standing between this arm and being the default.
3. **Ship it as the default with the overlay caveat in the log.** Cheapest, and it makes
   the F4 menu a `CZ_VK_SWAPCHAIN=0` errand.

---

## 4. MEASUREMENT — what changed

* **Headless is honest again** (see START HERE 2). The rule "raise `CZ_FPS_CAP` in every
  arm" is doubly retired: the default is 500 and the route no longer sits on it.
* **A cost that only exists with a window needs a windowed harness.**
  `tools/part54_present_cost.sh` is it — the recon's route and event gate,
  `CZ_VK_PROFILE`, per-thread CPU, `ENVX=` for the arm. `tools/part54_swap_bins.py` bins
  the profiler's own windows by draw count, because the two arms never see the same draw
  list (one round put them at 1,806 and 4,039 draws in the same window).
* **A gate must read the bytes the change produced** (gotcha 350). Every picture gate here
  walks the present readback; none of them can see a change that removes it.
* **A compositor grab needs an awake screen** (gotcha 351). Use
  `tools/part54_swapchain_picture.sh` by day and `CZ_VK_SWAPCHAIN_DUMP` at night.
* **An intermittent failure cannot be attributed by a single-run A/B** (gotcha 349), and
  the most recent change is the most tempting wrong answer.

---

## 5. WHAT IS OWED

* **The operator's verdict on the swapchain arm** — see §3.
* **A `CZ_VK_PRESENT_STAGING`-only pair at the operator's load**, still owed from part 53
  and now mostly moot if the swapchain becomes the default.
* Their two deferred picture items, **00m decals** and **00n a sign and items at
  distance**.
* **The operator's standing instruction** from part 52: *"prepare a whole plan to fix CPU
  performance issue and we'll start it in a fresh conversation."* `perf-plan-part52.md` is
  that plan, still live.

---

## 6. STANDING STATE

* **Runtime defaults, unchanged by this part**: 500 fps cap (1 ms vblank period), host
  vsync off, 100 us ring tick, 4 guard workers, no present staging copy, internal
  resolution 1280x720, **the readback present path**.
* **New arms**: `CZ_VK_SWAPCHAIN=1`, `CZ_VK_SWAPCHAIN_FIFO=1`, `CZ_VK_SWAPCHAIN_DUMP=dir`
  (+`_EVERY=N`), `CZ_FAKE_PRESS_EDGE_MISS=1`. All in `docs/instruments.md`.
* **New instrument lines**: `[vkprof] swapchain` (presents, DROPPED frames, rebuilds,
  suboptimal), `[vk] swapchain WxH … present mode`, and the `LATE` annotation on a
  synthetic host debug edge that had to be recovered.
* **New tooling**: `tools/part54_present_cost.sh` (the windowed A/B harness),
  `tools/part54_swap_bins.py` (draw-binned profiler windows),
  `tools/part54_swapchain_picture.sh` (the compositor picture gate).
* * **Gates at close: ALL CLEAN.** `--smoke`; switch gate 0 defects; dimension census 0
  disagreements; both PM4 oracles on B1 (24.5 M packets, 28,726 indirect buffers); E3 best
  of five **+0.8399**, 4 of 5 agreeing on layout; `no translated shader` 0; `truncated=0`;
  0 `PARALLEL GUARD SLOT MIX-UP`; deepest file **#83 `cinezombie.big`**; **A5 exit 0, 4
  permutation windows, 0 real**; shader-cache NAME diff shows only `ps_926c15dd20571cf1`,
  the known lost-microcode entry. **The cache is 438**, unchanged.
  **Plus two rows this part had to invent** — the swapchain image against E3 at **1x
  (+0.8831)** and at **2x (+0.8741)** — because the E3 row above them is produced by the
  READBACK path and says nothing about this arm at all (gotcha 350).
* **Artifacts**: `~/DR2CZ-troubleshooting/part54/` — `p54_base.*` (the re-taken symbol
  budget), `p54_win{1,2}x.*` (the first windowed pricing), `p54c_*` (the 12-run campaign),
  `swapdump/` and `swapdump2x/` (the picture gate), `picture/` (the compositor gate that
  read black).
