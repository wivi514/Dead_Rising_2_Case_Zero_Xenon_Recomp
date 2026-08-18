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

## 2b. THE WINDOW-SIZE WORRY — raised, filed as open, and REFUTED the same evening

§2's campaign left the window at its default (a **1088x612** drawable) and the operator
plays **maximised at 2560x1417**. The two arms genuinely do not scale the same way with it:
the readback path's CPU cost is a function of the INTERNAL resolution and is independent of
the window, while the swapchain's blit DESTINATION *is* the window. So the headline could
have been a small-window artifact.

**It was not.** `MAXIMIZED=1 tools/part54_present_cost.sh`, three rounds an arm, both arms
at a stable 2560x1417:

| draws | base | swapchain | delta | null |
|---|---|---|---|---|
| 500-999 | 7.10 ms | 3.90 ms | −45.1% | +3.5% |
| **2,500-2,999** (n=7/9) | **10.70** | **7.60** | **−29.0%** | **+0.0%** |
| 3,500-3,999 | 11.60 | 8.90 | −23.3% | −1.7% |
| 4,500-4,999 | 12.60 | 9.90 | −21.4% | — |

The 2x2 that falls out of the two campaigns says why: **the window costs the swapchain arm
+2.6…+8.6% and the readback arm +2.7…+4.9%.** The mechanism was right and the magnitude was
small. Filing it as OPEN rather than as "probably fine" was still correct — a sound argument
cannot separate a small effect from a large one.

**Both present arms now log their drawable size** at start-up and on every change, because
the readback arm reported it nowhere and that is what made the mistake unavoidable rather
than careless. `CZ_WINDOW_MAXIMIZED=1` / `CZ_WINDOW_SIZE=WxH` set it at CREATION — resizing
afterwards is not a control, the window bounced through six drawables in one run.

### RESOLVED BY THE OPERATOR'S SOAK — and it retracts §2's headline

`tools/part54_chained_ab.sh` — both arms in one session, their machine, a four-minute SOAK
an arm, uninstrumented but for `CZ_FPS_LOG`, matched on the draw count that line now
carries:

| draws | readback | swapchain | Δ fps | Δ ms | delta |
|---|---|---|---|---|---|
| 2,250-2,499 | 90.4 fps | 114.7 | +24.2 | −2.33 | −21.1% |
| **6,750-6,999** (n=4/6) | **67.8** | **70.2** | **+2.4** | **−0.51** | **−3.5%** |

Their report, before seeing any of it: *"3 to 6 fps higher"*. Measured **+1.5 to +2.4**.

**EVERY CAMPAIGN IN PART 54 SAMPLED THE LIGHT END.** Six of them — two internal
resolutions, two window sizes, three rounds an arm — and all put their best-populated band
at 2,500-2,999 draws, while the operator plays at **6,700-7,300**. Their light band agrees
with the campaigns (−21.1% at ~2,400), so nothing was mis-measured; it was over-generalised.
And the SHAPE was wrong too: §2 said the millisecond saving holds while the percentage
falls, and the milliseconds collapse as well, **2.33 → 0.51 ms**. Likely mechanism, as a
hypothesis: at high load the GPU is busy, so CPU time taken off the pump is absorbed by a
longer fence wait rather than converted to frames — testable with `CZ_VK_PROFILE` on a
soak, which nobody has run. **Gotcha 355, and it is the single most important thing in this
hand-off: take the A/B at the load the player is at.**

**The stutter is real and is a separate win.** Frame-time mean against median in transit:
**+3.3% for MAILBOX against +5.9% for the compositor-paced SDL present**; at the settled
soak both are smooth. That is the present MODE, a choice the SDL path could never make, and
it survives the retraction intact.

---

## 3. CLOSED: THE SWAPCHAIN IS THE DEFAULT, AND THE OVERLAY CAME WITH IT

The operator's decision at the close of part 54, after judging both arms. What follows is
kept because it records what the decision cost and what it did not.

**`CZ_VK_NO_SWAPCHAIN=1` is the control arm**, and it restores the readback path exactly.
It is what every measurement before part 54 was taken on, so it is what future present
claims get compared against — not a deprecated path.

**The one blocker was the F4 overlay, and it was ported rather than accepted.** A window
carrying `SDL_WINDOW_VULKAN` has no `SDL_Renderer`. The layout is now emitted once
(`EmitDebugOverlay` in `host/window.cpp`) and consumed by two backends: SDL rects, and a
software rasteriser the renderer uploads and blits over the presented image. Writing it
twice is how two drawings of one menu drift apart until somebody reports that it "looks
different in the other mode". The only deliberate difference is an opaque panel where
SDL's is 88%, because a blit cannot blend.

**Gating it found a bug that looking never would.** The overlay was first placed after the
`CZ_VK_SWAPCHAIN_DUMP` copy, so the dump could not show it (0 panel pixels against the
overlay's own counter of 5,595) and — silently — the dump had already moved the image to
`TRANSFER_SRC`, so the overlay blitted into an image whose layout said otherwise.
Undefined behaviour reachable only with the dump armed, i.e. only in the gate. **Anything
drawn into the presented image must be drawn before the dump reads it.**

### The superseded three-way decision, kept for the reasoning

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

* **The operator's verdict on the swapchain arm is IN, on both halves.** Picture: *"Looks
  a lot nicer now"* (after the resize fix). Frame rate: *"3 to 6 fps higher, less
  stutter"*, and the measurement agrees. **Whether it becomes the default is still open**
  and is now a judgement about −3.5% plus smoothness plus removing three copies of
  architecture, against losing the F4 overlay — see §3.
* **A PROFILED SOAK.** The retraction's mechanism — CPU time absorbed by the fence wait at
  high load — is a hypothesis, and `CZ_VK_PROFILE` on a soak at 6,800 draws would settle
  it. It costs 2-4 ms a frame so it is a different frame, but `submit gpu` is the column
  that would show it.
* **RE-PRICE EVERY OPEN PLAN ITEM AT SOAK LOAD.** Item 1.2 and item 1.4 have both been
  sized off campaigns of exactly the kind gotcha 355 is about.
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
  (+`_EVERY=N`), `CZ_FAKE_PRESS_EDGE_MISS=1`, `CZ_WINDOW_RESIZE_AT=SECS:WxH`. All in
  `docs/instruments.md`.
* **The swapchain follows the window**, as of the operator's blurry-picture report: the
  event loop publishes the drawable size into two atomics and the blit rebuilds when it
  differs. `CZ_WINDOW_RESIZE_AT` is the positive control, because no headless gate can
  resize a window (gotcha 354).
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
