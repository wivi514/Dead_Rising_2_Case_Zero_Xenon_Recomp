# D3D phase C, part 24 hand-off (for part 25). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c23-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session three times now, including once in part 24 itself.

## The one-paragraph state of the port

The game boots, renders and plays. A headless run reaches the title, the menus, the
prologue and live gameplay with no operator, and the outdoor recipe reaches a crowd at
6,000-8,700 draws a frame. Ordinary gameplay is ~30-31 fps and that is the title's own
two-vblank pacing floor, not our ceiling. The CPU and GPU overlap (part 23). The HUD /
ammo defect is FIXED (part 24, below). **The top remaining picture item is open-items
item 00: cube maps have never been bound** — 91 of 395 shaders sample `TextureCube[]` and
every one of them reads descriptor index 0, the 1x1 white dummy, on every draw, since
phase 5. It is fully specified and needs no operator for part 1.

## READ THIS BEFORE MEASURING ANYTHING

* **The frame time is PINNED at two vblanks (~32 ms) for everything this port can
  currently reach**, including a 6,778-draw crowd. So a CPU saving does not show up as
  frame rate, and neither does a CPU COST (gotchas 237 and **243**). Part 24 shipped a
  change that took `record` from 4.8% to 19.3% of a crowd frame at zero fps cost. Quote
  `outside` (the headroom) alongside the phase percentages, and never call a phase
  percentage a regression without showing a frame above the floor.
* **Every phase in `CZ_VK_PROFILE` is EXCLUSIVE of the ones nested inside it** as of part
  20; numbers from before that overstate `record` by the whole of `streams` (gotcha 228).
* **The guard's cost is charged to `record`, not `streams`** (gotcha 238). Re-baseline
  before attributing anything to `record`.
* **Do not pin the GPU clock** — sample it with `tools/gpu_clock_sample.py`. Gotcha 219 is
  retracted in part; the P8/210 MHz that motivated pinning was a session with the monitor
  asleep.
* **Three runs an arm on any crowd frame-time claim**; the noise floor is 10-13% at one
  run a side (gotcha 229). Run the null comparison first.

## Where part 25 starts, in order

1. **open-items item 00 — CUBE MAPS ARE NEVER BOUND.** The top picture item, fully
   specified, three parts, and part 1 is the real work: carry the per-slot texture
   DIMENSION into the sidecar metadata (a shader may sample both a 2D and a cube, so it
   must be per fetch slot, not per module). Then create cube textures as six array layers
   with a `VK_IMAGE_VIEW_TYPE_CUBE` view registered in **set 2**, and write
   `kSharedTexCube[constIdx]`. Every reflective surface currently multiplies its specular
   by pure white. Three competing theories died in the same census and are recorded in
   item 00 so they are not re-bought.
2. **The binned frame-time A/B still owed for `CZ_VK_FRAMES_IN_FLIGHT=2`** (part 23). Use
   `tools/frame_perf_bins.py` and read the MEDIAN and the vblank-pinned share, not the
   mean (gotcha 237).
3. The rest of `docs/open-items.md` in order: shadow cascade (3), mipmaps (4), colour
   (6), and item 12 (keyboard-as-user-2 fallback, low priority).

**Considered and deliberately NOT planned: giving `CZ_FAKE_PRESS_SEQ` a trigger.** Its
vocabulary has no RT/LT and attack here is RT, which is why no headless recipe has ever
fired a weapon — but adding the button only solves half of it. A recipe still has to
ACQUIRE a gun and ammo, which is a long scripted path through the world, so the trigger
alone would not make any weapon test self-servable. The operator does weapon tests
directly when one is needed. Do not re-propose this as a way to close a weapon-related
item; propose the acquisition path first, and only then the button.

## What part 24 delivered

* **The title's own debug build, switched back on.** `CZ_DEBUG_MENU=1`. The retail XEX
  still contains `debugmenu.cpp`, `cDebugMenu`, the whole item tree, and a `DebugJump`
  screen **whose layout ships** (largest entry in `mainmenu.big`). 393 boolean tunables
  gate it; one loader resolves them by name three hops off the entry point and nothing
  rewrites the bytes, so a post-hook is permanent and free.
* **The operator built the usable UI**: F2 opens the shipped DebugJump screen, F4 opens a
  host-rendered menu over the preserved `cDebugMenu`. **AutoChuck gives Chuck an AI that
  completes objectives** — the single most useful thing here for testing, because it
  removes the human from the driving. Plus a PP award and a level-50 cap.
* **Open item 00c CLOSED.** The HUD collapse and the 26<->27 ammo flicker were our own
  cross-frame stream store — specifically its GUARD, which was exact only to 512 bytes and
  sampled 8x64 above. Exact bound is now 16 KB; HUD correct in a crowd; zero frame-rate
  cost. `CZ_VK_STREAM_GUARD_BYTES=N` retunes without a rebuild.
* **`XamInputGetState` no longer fakes success** for users 2 and 3.

## Gates, on the part-24 binary

`--smoke` OK. `CZ_VK_STREAM_GUARD_BYTES` announces itself on every run. The profile line
now reports the residual sampled-stream count. **Not re-run this part and owed before any
claim that rests on them**: the A5 kernel-call diff, `truncated=0`, the PM4 capture
oracles, and the capture-E picture correlation.

## The method notes worth carrying

* **Gotcha 241 — bind name-to-slot tables by DATAFLOW, never by adjacency.** My table of
  387 tunables was off by one because the loader stores a lookup's result after the NEXT
  name is loaded. It survived a consumer scan (readers exist at both candidate addresses)
  and a live-memory read (a tautology — I read back bytes I wrote). Ask what would REFUTE
  a binding.
* **Gotcha 242 — a threshold fitted to a census is fitted to what the INSTRUMENT could
  reach.** The 512-byte bound came from a census whose recipe never fired a weapon. Ship a
  counter for whatever falls outside a threshold.
* **Gotcha 243 — a pinned frame time hides a CPU cost as thoroughly as a CPU saving.**
* **A metric that reproduces tightly can still measure the wrong thing.** Part 24's
  LIFE-pips detector gave 69.0% and 69.4% across two runs and was measuring where Chuck
  was standing, because partial HUD is location-dependent (`phase5-notes.md` §2152, which
  already said so). Tight reproduction is evidence of a stable measurement, not a relevant
  one.
* **The operator's report outranks my headless number when the two disagree** and the
  headless recipe cannot reach the condition. In part 24 the operator's "I fired in arm 1
  and it was clean" reversed a conclusion I had drawn from three headless arms.
