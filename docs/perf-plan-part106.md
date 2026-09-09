# Performance plan, part 106 — 60 fps locked at 1080p on a GTX 1060

**The operator's instruction, 2026-09-08, closing the Steam Deck work:** *"search for way to
improve performance on linux that could also work on all platform this game should be
atleast playable 60fps locked at 1080p on gtx 1060."*

This plan supersedes the part-91 parking (`part91-kickoff.md` §0c/§0d) as the live
performance document. It does NOT supersede the ledger those parts left: every number
they measured still stands, and §0 below is built from them. What changes is the
QUESTION. Parts 71-91 asked "how much faster can the crowd go on the dev box", where
the crowd is CPU-bound by ~1.4 ms and every GPU saving converted to nothing. A locked
60 on a GTX 1060 asks "what does the frame cost on a GPU three to four times slower",
and on that box the GPU side is the binding constraint at EVERY load.

## §0. The arithmetic that governs everything

### 0.1 What the frame costs at 1080p, measured (this box, RTX 3070, 2x MSAA, headless)

`tools/part80_crowdroute.sh` pinned to `RES=1920x1080`, three runs, `CZ_VK_FRAME_TRACE`
+ `CZ_VK_GPU_PASSES` (bill nil), no profiler, no frame stats. Medians per 1,000-draw
band, `tools/part80_trace_band.py`; the three runs agree within 2% in every band.

| draws | wall ms | GPU ms | fence ms | regime |
|---|---|---|---|---|
| 0-1,000 (menus) | 2.00 | 0.86 | 0.00 | cap |
| 2,000-3,000 | 5.06 | 5.00 | 1.06-1.15 | **GPU-bound** |
| 3,000-4,000 | 6.19 | 4.92 | 0.00 | CPU-bound |
| 5,000-6,000 | 8.20 | 6.20 | 0.00 | CPU-bound |
| 7,000-8,000 | 10.03 | 7.71 | 0.00 | CPU-bound |
| 8,000-9,000 (the crowd) | **10.61** | **8.85** | 0.00 | CPU-bound by ~1.8 |

Every headless GPU number carries the 0.63 ms present readback the windowed build
never pays (gotcha 517), so the crowd's real device frame is **~8.2 ms** — the same
number part 103 read windowed (§6et §1). The crowd's device split (base1, windows 10-14,
`tools/gpu_split_window.py`):

| class | ms/frame | share |
|---|---|---|
| pass: >=256 draws (the title's own scene passes) | 6.40 | 72% |
| resolve copy (the MSAA resolve, 22 Mpix) | 0.72 | 8% |
| present readback (headless only) | 0.63 | 7% |
| pass: 2-255 draws | 0.52 | 6% |
| pass: 1 draw (the post chain) | 0.39 | 4% |
| everything else (barriers, clears, snapshots, cube) | 0.15 | 2% |

And at LIGHT load (2,500 draws, windows 2-4): 5.0 ms, of which the big passes are
2.0-2.3 ms — i.e. **2.5 ms of every frame does not scale with the crowd at all**.

### 0.2 What a GTX 1060 is, relative to what we have measured

Nobody on the project owns one, so every 1060 number here is a SCALING of two boxes
that have been measured with the same instrument at the same resolution:

- RTX 3070 (this box): crowd device frame 8.2 ms at 1080p 2x (§6et §1, windowed).
- RX 6600 (czamd): **20.1 ms** at the same load and settings, 2.4-2.5x the 3070 across
  the whole load range (§6et §1) — which matched the two cards' published throughput
  ratio, and was the basis for the ledger's verdict that the frame is "the title's own
  shading at the hardware's price".

A GTX 1060 6 GB sits below the RX 6600 by roughly 1.6-1.8x in published 1080p game
averages, and below the RTX 3070 by roughly 2.5-3x. **Taking the czamd measurement as the
anchor and the 1060 at 1.7x below it:**

| load | 3070 measured | RX 6600 measured | GTX 1060 estimate |
|---|---|---|---|
| light (2,500 draws) | 5.1 | 12.2 | **~20** |
| crowd (8,500 draws) | 8.2 | 20.1 | **~34** |

**Neither row is inside a 16.7 ms budget.** The light-load row is the decisive one: the
crowd could be excused as a worst case, but a 1060 missing 60 fps in an EMPTY street means
the frame's fixed cost has to fall by half before crowds are even the question. The CPU
side is the second constraint — this box's Ryzen 7 5700 is CPU-bound at the crowd at
10.6 ms, and a 1060 owner's CPU is typically 1.3-1.6x slower per thread — but it only
matters once the GPU fits.

### 0.3 The claim the ledger never tested

The part-103 verdict compared two GPUs against EACH OTHER and found the ratio matched
their hardware. That is a consistency check, not a cost check. The cost check is against
what the title actually does: an Xbox 360 rendered this scene at 1280x720, 30 fps, on a
GPU of ~240 GFLOPS with 10 MB of EDRAM. A GTX 1060 has ~18x that arithmetic and ~3x the
bandwidth of the console. Rendering 2.25x the pixels at 2x the frame rate is 4.5x the
console's work, which leaves a factor of ~4 on the table if our translation of the
frame were at hardware's price — and the 1060 estimate above says we are spending it
and more. The RX 6600 (37x the console's FLOPS) taking 20 ms for what the console did in
33 ms at 0.44x the pixels is the same fact from the other side. **The title's own passes
are 72-82% of the device frame and nobody has ever said what they are MADE OF** — vertex
work, pixel work, overdraw, the fixed cost of 9,000 draws, or the shape of the translated
shaders (constants through raw 64-bit loads, bindless heaps, an Int64 capability in 450 of
450). A share you have not decomposed is a share you cannot act on; that decomposition is
this part's first deliverable.

## §1. The instruments built for it (part 106, commit ff286b9)

All same-binary, all off by default, all printing an engagement line:

- **`CZ_VK_GPU_STATS=1`** — a `VK_QUERY_TYPE_PIPELINE_STATISTICS` query around every
  render scope, accumulated per pass class next to the timing split: primitives
  assembled, vertex-shader invocations, clipped primitives, **fragment-shader
  invocations and FS invocations per visible pixel (the overdraw factor)**. Needs
  `CZ_VK_GPU_PASSES=1` (it hangs off the timing segments) and FORCES the serial recorder,
  because a query cannot span the parallel recorder's worker chunks; the GPU work is
  identical in both (the order gate is the proof), so the census is honest about the
  frame even though its wall time is the part-88 recorder's. `pipelineStatisticsQuery`
  joins the feature table as optional.
- **`CZ_VK_NULL_PS=1`** — every translated pixel shader replaced by a do-nothing fragment
  stage (`tools/null_ps.hlsl`, embedded like the draw-ID shader). Vertex shader, vertex
  input, depth test/write, MSAA and raster all as the draw would have them; only the pixel
  shading goes. "Everything but pixel shading."
- **`CZ_VK_SCISSOR_1PX=1`** — every draw's scissor collapsed to 1x1 at its own origin.
  Vertex shading, assembly, clipping and the per-draw fixed cost stay; the fragments go.
  "Everything but the pixels."
- **`pass: shadow cascade`** — a new class in the GPU split: any scope whose draws bound
  the 1040-pitch shadow surface, whatever its draw count, so the cascade's cost stops
  hiding inside ">=256" next to the scene.

The pair of arms brackets the decomposition: base − NULL_PS = pixel shading;
base − SCISSOR_1PX = everything fragment-side (shading + ROP + depth/MSAA traffic);
SCISSOR_1PX itself = vertex + fixed. Both destroy the picture by design and can never be
modes; they are measurement arms in the sense of `docs/measurement.md`.

**Found while building them: the pass EXTENT CENSUS had been blind since part 89.** Under
parallel record the per-draw extent read `R->bound.scissor`, which the capture path never
writes, so it read 0x0 on every draw and every run since printed no census rows at all —
an instrument that looked like an empty table (gotcha 3's shape). It now reads the draw's
own scissor, which both recorders bind.

## §2. The decomposition (2026-09-08, this box, 1920x1080 pinned, 2x MSAA, headless)

Crowd route, one to two runs an arm against four pooled baselines, medians per
1,000-draw band, GPU column = device timestamps (includes the 0.63 ms headless readback
in every arm alike). Every arm's route reached the crowd (peak windowed draws 8,450-9,250,
≥6 windows at ≥8,000). Replicated arms agree to 0.07 ms.

| arm | light (2,000-3,000 draws) GPU | crowd (8,000-9,000) GPU | what went |
|---|---|---|---|
| base (4 runs) | **5.00** | **8.86** | — |
| `CZ_VK_SCISSOR_1PX` (2 runs) | 3.88 / 3.87 | 7.69 / 7.76 | every fragment: **−1.1 ms at BOTH loads** |
| `CZ_VK_NULL_PS` (2 runs) | 3.04 / 3.04 | 5.62 / 5.61 | pixel shading AND the vertex outputs the link strips with it: **−3.25 crowd, −1.96 light** |
| `CZ_VK_MSAA=0` | 4.58 | 8.55 | 2x MSAA: −0.3 to −0.4 |
| `CZ_VK_SHADOW_TIER=0` | 4.76 | 8.74 | the cascade at a quarter: −0.1 to −0.25 |
| `CZ_VK_ANISO=0` | 4.85 | 8.94 | anisotropy: a null |
| `CZ_VK_NO_PAR_RECORD` (the stats run's control) | 5.01 | 9.01 | GPU unchanged; wall +2.2 ms, the part-89 gain |

And the census (`CZ_VK_GPU_STATS`, crowd window of 1,500 frames at ~9,000 draws):

| class | ms | passes/frame | IA prims | VS inv | FS inv | FS/pixel |
|---|---|---|---|---|---|---|
| pass: >=256 draws (scene) | 6.19 | 2.13 | 3,585,367 | 2,366,780 | 8,700,024 | 4.20 |
| pass: shadow cascade | 0.86 | 7.37 | 560,138 | 386,378 | 1,322,675 | 0.64 |
| pass: 2-255 draws | 0.17 | 4.96 | 28,599 | 18,717 | 5,887,276 | 2.84 |
| pass: 1 draw (post) | 0.40 | 29.8 | 57 | 128 | 3,858,525 | 1.86 |
| **all passes** | | | **4.17 M** | **2.77 M** | **19.8 M** | **9.5** |

The scene renders in ~2 passes a frame at ~3.1 ms each: 1.21 passes at the full
1920x1536 EDRAM extent and 0.77 at the 960x1080 tile (the census now has rows again —
see §1). The whole frame shades 19.8 M fragments = 9.5x the screen, of which the scene
passes are 4.2x — an ordinary overdraw for a crowd with alpha-tested foliage, and the
1-pixel arm says it costs 1.1 ms. **Overdraw is not the story.**

### 2.1 What it says

1. **The fragment side of the whole frame is 1.1 ms of 8.2.** Pixel shaders, ROP, depth
   traffic, MSAA — all of it. At light load the same 1.1 ms. The "resolution and MSAA
   hardly matter in crowds" memory from part 51 is this fact seen from the outside.
2. **With NO fragments the scene passes still cost ~5.3 ms for 2.37 M vertex
   invocations and ~9,000 draws** — 2.2 ns a vertex, or 0.59 µs a draw. An RTX 3070
   shades simple vertices at 3-4 G/s; 2.4 M of them is under a millisecond. So the ~5 ms
   is not vertex arithmetic. It is one of two things, and the next two arms split them:
   the per-draw front-end cost of ~9,000 draws with their binds and push constants
   (`CZ_VK_TRI1`), or the vertex FETCH — and here the ledger already has the fact that
   makes that the prime suspect: **both the per-frame arena and the 1 GB cross-frame
   stream store live in system RAM** (`per-frame arena: 256 MB in system RAM`,
   `cross-frame stream store: 1024 MB in system RAM`, every run's log). Every vertex,
   every index and every constant of every draw is fetched across PCIe, and a 16-bit
   strip with ~1.5 index reads and a 32-64 byte vertex per invocation at 2.8 M
   invocations is 100-200 MB a frame — 6-12 GB/s at 60 fps, which is PCIe 3.0 x16's
   whole practical bandwidth, and a GTX 1060 sits on PCIe 3.0.
3. **The null-PS arm removes 2.1 ms MORE than the 1-pixel arm.** With no consumer the
   pipeline link strips every vertex output and the arithmetic and the attribute fetches
   behind it. That 2.1 ms is therefore inside the vertex stage — fetch or math — and is
   the same suspect from the other side.
4. **Part 73's "geometry in VRAM is 14% slower" was a WALL-time verdict in a CPU-bound
   regime** (gotcha 363): the arm moved the per-draw constant arena too, and the CPU's
   write-combined stores of ~8 KB per draw cost more than the GPU saved. Its GPU column
   was never read. The SPLIT — store in VRAM, arena in RAM — is the shape that keeps
   the CPU's cheap writes and gives the GPU its fetch, because the store is written
   0.2 MB a frame and read ~100 MB a frame (`CZ_VK_VRAM_STORE=1`, built this part).

## §3. The finding: the GPU was fetching its geometry over PCIe

Campaign 2, same route, same instruments, one to two runs an arm:

| arm | light GPU | crowd GPU | crowd wall | big passes (crowd) |
|---|---|---|---|---|
| base (5 runs) | 5.00 | 8.84 | 10.60 | 6.4 |
| `CZ_VK_TRI1` (first primitive only) | 2.26 | 3.72 | 10.15 | 1.1 |
| `CZ_VK_TRI1` + `SCISSOR_1PX` | 1.90 | 3.68 | 10.28 | — |
| **`CZ_VK_VRAM_STREAMS=1`** (2 runs) | **2.96 / 2.97** | **4.34 / 4.16** | 11.52 / 11.21 | **1.75** |

So the crowd's 8.84 ms decomposes as: **~4.5 ms vertex/index fetch across PCIe**
(base − VRAM), **1.1 ms fragments** (base − 1PX), **~0.6 ms vertex shading** (VRAM −
TRI1), **~1.1 ms per-draw front end** for ~9,000 draws (TRI1's big passes; 0.12 µs a
draw), and the rest is the resolves, the post chain and the headless readback. At light
load the fetch is 2.0 of 5.0 ms. **Geometry in video memory halves the device frame at
every load** — 8.84 → 4.2-4.3 at the crowd, 5.0 → 3.0 in an empty street — which on the
§0.2 scaling puts a GTX 1060 near 16-17 ms at the crowd and ~12 at light load. Not a lock
yet, but the first change in this port's history that moves a 1060 into the 60 fps
conversation, and it moves an RX 6600 from 20 ms to ~10.

**Campaign 3 — the SPLIT, `CZ_VK_VRAM_STORE=1` (store in the host-visible VRAM heap, arena
in RAM), two runs against seven baselines:**

| band | base GPU | vstore GPU | base wall | vstore wall |
|---|---|---|---|---|
| 2,000-3,000 | 5.00 | **3.26 / 3.25** | 5.06 | **4.11 / 4.20 (−18%)** |
| 5,000-6,000 | 6.20 | 3.49 / 3.48 | 8.18 | 8.07 / 7.96 |
| 8,000-9,000 | 8.84 | **4.03 / 4.11** | 10.66 | 10.53 / 10.69 (a null) |
| 9,000-10,000 | 9.10 | 4.33 / 4.34 | 11.37 | 11.23 / 11.27 |

The whole GPU saving is the STORE's, and with the arena left in RAM the wall-time cost
part 73 measured is gone: the crowd reads a null on wall and the GPU-bound bands convert
the saving 1:1 (an empty street −18% wall). This is the design confirmed by its own
control: the arena half of gotcha 363 was right, the store half was not.

**Why the ledger had refuted it.** Part 73 measured `CZ_VK_VRAM_STREAMS` at the
operator's soak as ~14% SLOWER — on WALL time, on this box, where the crowd is
CPU-bound: the arm moves the per-frame arena too, and the arena is where the pump writes
~8 KB of shader constants per draw; write-combined stores of those across PCIe cost the
CPU more than the GPU saved (gotcha 363). The GPU column was never read — it was not
the question then. Every word of that gotcha about the ARENA stands; its title ("geometry
belongs in VRAM is wrong for a recompiler") does not survive the store's numbers, and it
is retracted in place in `docs/gotchas.md`. The wall column above shows the same +0.6-0.9
ms at the crowd here, for the same reason; the GPU column is the target's metric.

**Why the arm itself cannot ship.** It needs a `DEVICE_LOCAL | HOST_VISIBLE` heap large
enough for a 1 GB store, i.e. Resizable BAR, which a GTX 1060 (and every pre-Ampere
NVIDIA card, and any AMD card without SAM) does not expose beyond a 256 MB window; and
its write-combined CPU writes are the wall cost above. The shippable form keeps the
CPU's cached writes and gives the GPU its fetch:

### 3.1 The store MIRROR (built this part; `CZ_VK_NO_STORE_MIRROR=1` is the control)

- A DEVICE-LOCAL twin of the cross-frame store — same size (or a quarter of the
  device-local heap, halving until it fits; slots past its end stay on the host path),
  same offsets, no mapping.
- The CPU writes the HOST store exactly as before. Every slot it writes (a fill or a
  stale re-copy into the ping-pong twin) is queued as a `VkBufferCopy` and stamped with
  the frame's mirror generation; the queue is recorded as one `vkCmdCopyBuffer` (batches
  of 4,096 regions) at the top of the NEXT frame's command buffer, before any rendering
  instance, with a transfer → vertex-input/vertex-shader/fragment-shader barrier.
- A persist HIT binds the mirror iff the slot's stamp is older than the current
  generation (its copy is in this or an earlier command buffer, ahead of this draw);
  otherwise the host store — the slot's first frame only. Dependent-fetch streams get the
  mirror's device address the same way; the few CPU readers of a stream
  (`StreamLoc::bytes()`: the rect synthesis, two diagnostics) read the host twin through
  `Buffer::shadowMapped`.
- Race-freedom rides on the existing ping-pong argument: a slot rewritten in frame W is
  read from the mirror only by frames > W, and it cannot be rewritten again until the
  frame that last read it has retired.
- The per-frame arena — constants, first-touch geometry, expanded index lists — stays in
  RAM: it is the half of gotcha 363 that was right.
- ONE path everywhere, on purpose: on a Resizable-BAR box the host-visible arm
  (`CZ_VK_VRAM_STORE=1`) would do the same job with no copies, but it is a second code
  path with a write-combined-read hazard on the rect-synthesis reader, and a GTX 1060
  cannot take it at all. The mirror is the default on every device with a device-local
  heap; the ReBAR arm stays a measurement arm.
- Growth (`PersistMaintenance`) re-creates the twin at the new size with the device idle;
  the mirror's absence (no heap, allocation failure, the arm) is printed and is the
  part-105 renderer.

Gates: `CZ_VK_VALIDATION=1` clean, `CZ_VK_SYNC_VALIDATION=1` 0 hazards, the crowd route
3v2 against the control arm (§4 below), then the operator's eye at the crowd — the
failure mode of a wrong generation stamp is a one-frame stale or torn mesh, which no
headless number can see (gotcha 254's shape), so an operator session is owed before it
ships in a release.

## §4. The mirror measured (campaign 4, 2026-09-08 evening)

Default ON, three runs, against the seven pooled baselines (`CZ_VK_NO_STORE_MIRROR=1`
control runs read 8.87 GPU / 10.53 wall at the crowd — the baseline, as they must):

| band | base GPU | mirror GPU (3 runs) | base wall | mirror wall |
|---|---|---|---|---|
| 2,000-3,000 | 5.00 | **3.25 / 3.26 / 3.27** | 5.06 | **3.94 / 3.96 / 3.85 (−22 to −24%)** |
| 5,000-6,000 | 6.20 | 3.50 / 3.53 / 3.53 | 8.18 | 8.02 / 7.93 / 7.97 |
| 7,000-8,000 | 7.71 | 3.77 / 3.78 / 3.80 | 9.95 | 9.66 / 9.62 / 9.57 |
| 8,000-9,000 | 8.84 | **4.00 / 4.11 / 4.04** | 10.66 | 10.29 / 10.61 / 10.39 |

A fourth run on the build with the `TRANSFER_SRC` fix (campaign 5) reads the same:
**3.98 crowd / 3.24 light, wall −4.1% / −24.7%**; its third control run 8.78 / 4.98. The
mirror's own bookkeeping over that run: **0.32-0.34 MB/frame copied host → VRAM in ~30
copies/frame, and persist hits bound the mirror 100.0% of the time** (the host-store
binding is a slot's first frame only). The mirror reads the same GPU column as the ReBAR
arm (4.03-4.11) to within the run floor — the copies' own cost is the split's residual, 0.05-0.07 ms a frame — and the
wall column is a small GAIN even at the CPU-bound crowd (−0.5 to −3.4%), where the ReBAR
arm was a null: the CPU never touches write-combined memory in this form. Validation:
the first boot found the host store lacking `TRANSFER_SRC` (fixed, TRANSFER_SRC is in
`PersistUsage()`); synchronization validation 0 hazards on a 100 s boot. The residual
gate (`present blit` 1.00 regions/frame, overflow 0) holds in every run.

**Windowed, the shipped build's crowd on this box goes from ~8.2 to ~3.4 ms of GPU.** On
the §0.2 scaling that is the RX 6600 from 20 to ~8.5 ms and a GTX 1060 from ~34 to
~14-15 ms at the crowd — inside the budget for the first time, with the light street at
~11.

## §5. What remains after the mirror, priced from the same runs

With the fetch gone the crowd's device frame on this box is ~4.2 ms (headless; ~3.6
windowed). On the §0.2 scaling a GTX 1060 sits near **16-17 ms at the crowd and ~12 ms in
an empty street** — a locked 60 in ordinary play, on the edge in the heaviest crowds.
What is left, from the arms above, in order of size at the crowd:

| item | ms (3070, crowd) | 1060 estimate | what would move it |
|---|---|---|---|
| per-draw front end (TRI1's big passes) | ~1.1 | ~4 | fewer draws — the title's, not ours; or fewer state changes per draw (pipeline switches 30%, push constants 1.0/draw) |
| fragments (base − 1PX) | 1.1 | ~4 | resolution and MSAA are the player's levers; nothing pathological here (4.2x overdraw is the crowd) |
| MSAA resolve copies | 0.7 | ~2.5 | `CZ_VK_MSAA=0` is −0.3 here; the resolve is the picture decision's price |
| shadow cascade | 0.86 | ~3 | the tier row exists; Low is −0.1 to −0.25 here |
| vertex shading | ~0.6 | ~2 | the translated VS is small; the clip-plane cache adds six dot products a vertex (operator's play cache) |
| post chain (1-draw passes) | 0.4 | ~1.5 | real shading, refuted as overhead in part 79 |
| first-touch geometry still fetched from the arena | unmeasured | — | the arena's streams are the store's misses; a warm store serves 94% of lookups |

None of these is a factor of two again; the next factor is the CPU. **The CPU side is
the second half of the target and it has not moved: the crowd is 10.6 ms of pump time on
a Ryzen 7 5700, and a 1060 owner's CPU is typically 1.3-1.6x slower per thread**, which
puts their crowd at 14-17 ms of CPU against a 16.7 ms budget. The parked ledger's CPU
items (part91-kickoff §0d: the PM4 walk ~3.1 ms serial, UploadStream's resolve half
162 ns/draw, the remaining record cost 431 ns/draw) are the board for that half, and
every one of them converts 1:1 on a CPU-bound box. A light street is CPU-light too
(5.1 ms wall at 2,500 draws), so "playable 60 at 1080p on a 1060" is, after the mirror,
a claim about ordinary play with the heaviest crowds as the known exception — until the
CPU half is worked.

**What is owed:** (1) the mirror's own gates (§3.1) and an operator session at the
crowd — a wrong generation stamp is a one-frame stale mesh no headless number can see;
(2) a GPU-bound box's confirmation — czamd (RX 6600) is the nearest thing to a 1060 the
project can reach, and the mirror's GPU column there is the number to quote next to the
3070's; (3) the CPU half, re-baselined on this box at 1080p (the 10.6 ms above) and
worked from the parked board; (4) a real GTX 1060 measurement, by the operator or a
player, before "locked" is claimed anywhere.

### 4.1 The downclocked stand-in (operator, 2026-09-09 00:50)

The operator locked the RTX 3070 at **705 MHz core** (`nvidia-smi -lgc 700,700`; the
memory lock did not hold — 5,001 MHz under load against the stock 7,001, pstate P3) and
the crowd route ran twice an arm at 1080p:

| band | OFF GPU | ON GPU | OFF wall | ON wall |
|---|---|---|---|---|
| 2,000-3,000 | 7.14 | **5.72** | 7.19 | 5.81 (−19%) |
| 5,000-6,000 | 8.14 | 6.02 | 8.49 | 8.43 |
| 8,000-9,000 | **10.95** | **6.89** | 11.51 | 10.87 (−5.5%) |

Two readings. (1) At a third of the core clock the mirror's saving is **−4.1 ms at the
crowd (−37%)**, and the crowd went from GPU-bound (wall ≈ GPU, 11.5) back to CPU-bound
(fence 0.00, GPU 6.9 under a 10.9 wall) — a GPU at a third of this one's clock keeps up
with this CPU once the fetch is gone. (2) **The stand-in under-states a 1060, and says
why:** the fetch-bound half of the OFF frame barely moved with the clock (8.84 → 10.95
for a 3x clock cut, because PCIe bandwidth did not change) while the mirror's frame
scaled with the core (4.0 → 6.9). A GTX 1060 sits on PCIe 3.0 x16 — half this box's
PCIe 4.0 link — so its fetch cost would be ~9 ms where this box paid 4.5, and the
mirror's saving there is larger than any number this box can show. A downclock models
the ALU/raster share of a slower card; it cannot model its bus, and this frame's problem
was the bus. Every 1060 number in this plan remains a scaling, not a measurement.

### 4.2 The operator's play at 210 MHz (the GTX 1050 Ti compute stand-in, 2026-09-09)

Core locked at **210 MHz** (~2.5 TFLOPS, a 1050 Ti's arithmetic); the memory lock did NOT
hold (7,001 MHz under load — stock bandwidth), so this is the compute side of a 1050 Ti
with a 3070's memory and bus. 1920x1080, 2x MSAA, mirror ON, the operator playing
through crowds: **58-62 fps the whole session** — `[fps]` windows: 62.7 at 2,000-3,000
draws, 59.9 at 7,000-8,000, 58.6 (worst 58.1) at 8,000-9,000. GPU split over the run,
11.9 ms/frame: scene passes 3.30, **MSAA resolve copies 3.13**, post chain 2.05, shadow
cascade 1.33. At a tenth of the clock the resolve and the post chain are a third of the
device's frame — the next GPU items for a card of that class are the picture decisions
(2x MSAA, the post chain's full-resolution passes), not the geometry any more.

**What this does and does not say about the Steam Deck.** The Deck's GPU (~1.6 TFLOPS
RDNA2 at 1280x800, 88 GB/s shared) is in this stand-in's class and renders 40% fewer
pixels, so the GPU side is plausible there. Its CPU is not this box's: four Zen 2 cores
at 2.4-3.5 GHz against eight Zen 3, and the thread budget hands a 4-core box ZERO
workers (`clamp(cores − 2 − 3, 0, 6)`), which turns parallel record and the guard pool
off — the serial recorder's crowd is 12.9 ms here, and would be well past 16.7 on the
Deck. The Deck verdict is a CPU question (part 107 §1 item 1) and a measurement on the
device (part 106's kickoff, the RADV live-USB test), not a scaling.
