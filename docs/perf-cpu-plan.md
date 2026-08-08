# The CPU plan: a crowd frame is 75% our own code, in two equal halves

Successor to `docs/perf-plan-overnight.md`, which is finished. That plan's subject was a
frame whose largest term turned out to be a `sleep_for`; this one's subject is what is
left after the sleep, the vblank and the GPU clock were fixed (`docs/phase5-notes.md`
§§6aj-6an).

**Read §6an first.** The one-line summary: ordinary gameplay is pinned at 31 fps by the
title's own two-vblank pacing and needs nothing; **a Still Creek zombie crowd runs at
22-25 fps and is CPU-bound in our runtime**, at 6,592 draws and 43.4 ms:

| term | ms | µs/draw | share |
|---|---|---|---|
| **renderer draw path** | **21.40** | **3.25** | **49.3%** |
| **PM4 command-processor walk** | **10.98** | **1.67** | **25.3%** |
| GPU fence wait (at 1950 MHz) | 6.64 | 1.01 | 15.3% |
| pump sleep | 3.86 | — | 8.9% |
| readback | 0.56 | — | 1.3% |

Both big terms scale linearly with the draw count and neither has ever been optimised,
because until the operator session of 2026-08-08 nothing in this port had profiled a
scene with more than ~1,930 draws in it.

---

## 0. FIRST, AND IT BLOCKS EVERYTHING ELSE: a headless recipe that reaches a crowd

Every number above came from an operator playing. The existing recipe in `CLAUDE.md`
reaches live gameplay headlessly but renders ~1,930 draws — **it never enters the
workload this plan is about**, so with it every A/B here would be run on a frame sitting
against the vblank cap, where CPU savings are invisible by construction. That is exactly
how the state cache first measured as a dead heat.

`CZ_FAKE_PRESS_SEQ` already has stick entries (`LSUP/LSDOWN/LSLEFT/LSRIGHT` walk Chuck,
`RSUP/...` aim the camera, and a stick entry HOLDS for its interval). The job is to
extend the sequence until a headless run reports **>4,000 draws/frame** in
`CZ_VK_FRAME_STATS`, then pin that recipe in `CLAUDE.md` beside the existing one.

Gotcha 190's rule, one more time: a measurement that needs a human is a measurement
nobody will repeat. **Do not start §1 or §2 before this exists** — and note the acceptance
test is a number in the frame stats, not a screenshot, so it is self-checking.

Watch for: the sequence is fixed 8-second intervals against a boot whose depth in fixed
wall time is a distribution (gotcha 75), and the game now runs **2.5x faster** than when
the current recipe was written, so its press timings are already suspect. Re-derive, do
not extend blindly.

---

## 1. The renderer draw path — 21.4 ms, 3.25 µs per draw

Ranked by (expected size) x (confidence), each with the measurement that settles it
BEFORE any code changes. The whole overnight session is the argument for that ordering:
its prime suspect was 0.5% of the frame and the real cause was a sleep nobody had timed.

### 1a. `record` is 11.07 ms — 1.68 µs for ~5 remaining vkCmd calls

That is **~340 ns per call**, where a `vkCmd*` on this driver should be 50-150 ns. So
either the calls are not what is costing, or something around them is. What remains per
draw after the state cache: `vkCmdBindVertexBuffers` (once per binding),
`vkCmdBindIndexBuffer`, `vkCmdPushConstants` (24 bytes), `vkCmdDrawIndexed`.

* **Hypothesis A — the vertex/index binds repeat too.** A crowd is many copies of a few
  zombie meshes, and `UploadStream` already caches per frame by (address, size, endian),
  so consecutive draws may be binding the *same buffer at the same offset*. The state
  cache deliberately stopped short of these; extending `Renderer::BoundState` to them is
  a dozen lines. **Measure first:** add skip counters exactly like the existing ones and
  run WITHOUT acting on them — if the repeat rate is low this is dead, and the counters
  cost nothing to leave in.
* **Hypothesis B — `Count()` is on the hot path.** `perf` at 1,930 draws already showed
  `std::map<std::string>::operator[]` at 0.44% and `__strncmp_avx2` at 0.75%; at 6,600
  draws that is ~3x, and `DoDraw` calls it several times per draw. Every call constructs
  a `std::string` and does a red-black tree walk. **This is the one item here that needs
  no measurement to justify** — it is instrumentation overhead in the thing being
  instrumented, the same defect that made the state cache measure as a dead heat. Convert
  to an enum-indexed `uint64_t[]` with the names in a static table; `g_stats` keeps its
  printing interface.
* **Hypothesis C — `getenv` on the hot path.** `perf` showed `getenv` at 0.42%. Every
  `Env("...")` that is not behind a function-local `static const` is a `strncmp` walk of
  the environment per draw. **Measure:** `grep -n 'Env(\|getenv' runtime/gpu/vk_renderer.cpp`
  and check each is cached; `pm4.cpp` has at least one per-tick `getenv` too.
* **Hypothesis D — push constants per draw are unnecessary.** The 24 bytes are three
  arena addresses, and `constants` allocates a fresh arena block per draw whether or not
  the values changed. If consecutive draws share identical constant blocks, both the
  allocation and the push could be skipped. **Measure:** hash the constant block per draw
  and count repeats before writing anything.

### 1b. `streams` is 4.77 ms — 0.72 µs per draw

The per-frame dword-swap copy, cached by (address, size, endian) in an
`unordered_map<uint64_t, VkDeviceSize>`. In a crowd the same meshes recur constantly, so
this should be nearly all cache hits — in which case 0.72 µs/draw is the *lookup*, not
the copy, and the fix is a cheaper key or a small direct-mapped cache in front of it.

**Measure first, and this one is genuinely ambiguous:** count hits and misses, and total
bytes actually copied per frame. A high miss rate and a high hit rate need opposite
fixes, and no reading of the code can tell you which you have.

### 1c. `textures` is 3.43 ms — and it is 7.9-10.9% in crowds against 2.5% in ordinary
gameplay

That rise is the tell: crowds stream textures. The cost is untile + upload, plus a cache
lookup per fetch. **Measure:** uploads per frame versus fetches per frame
(`CZ_VK_TEX_CENSUS=1` already has the columns). If uploads are near zero and this is
still 3.4 ms, it is the lookup path and belongs with 1b; if uploads are frequent, it is
real streaming work and the question becomes whether the untiler is efficient.

### 1d. `other` is 0.91 ms and rising — 0.0% at the title screen, 2.1-4.2% in crowds

`DoDraw`'s untimed work: register decode, pipeline key build, hash lookup. A term that
only appears at scale deserves its own `ProfScope` before anyone reasons about it —
splitting it costs three lines and it is the cheapest item in this document.

---

## 2. The PM4 walk — 10.98 ms, 1.67 µs per draw packet

Completely uninstrumented inside. `ExecutePacket` does header decode, an opcode switch,
register writes and the draw-sink dispatch, and there is no way at present to say which.

**Do exactly what `submit` got (§6al): split it before theorising.** Four `ProfScope`s
inside the walk — packet dispatch, register writes, the draw sink, everything else —
and one run answers it. Candidates worth having in mind, but NOT worth acting on first:

* `WriteRegister` is called per dword of every `SET_CONSTANT` / `LOAD_ALU_CONSTANT`
  packet, and a crowd frame carries a great many. It contains several env-gated side
  paths (const watch, bin census); each is a predictable branch, but there are a lot of
  dwords.
* Every dword goes through `GuestLoad32` + a byte swap individually.
* `Pm4Draw` is constructed and passed to the sink per draw — check whether that is a
  copy of something large.

---

## 3. What NOT to do

* **Do not optimise anything measured at ~1,930 draws.** That frame is against the
  two-vblank cap; savings there are structurally invisible, and a change that measures
  as zero will be discarded when it was actually worth 5% in the workload that matters.
* **Do not touch the pump tick, the vblank deadline, the ring brake, the per-CPU ISR
  acknowledge or the vblank gate.** They are the title's frame pacing and cost 40 runs
  to establish (parts 5-6, 18).
* **Do not put a `Count()`, an `Env()` or a `std::string` on a path you are timing.** It
  has now caused two false results in this project in one day.
* **Do not quote a frame rate measured with the GPU at stock clocks.** Every GPU number
  before 2026-08-08 was taken at P8/210 MHz of a 2100 MHz maximum (gotcha 219). The
  measurement configuration is `sudo nvidia-smi -pm 1` and `-lgc 2100,2100`, and it must
  be stated with any number, because it is worth 2.9x on the GPU term by itself.
* **Do not expect ordinary gameplay to move.** 31.2 fps is the title pacing itself at its
  console target. Only crowds can improve, and only the two CPU halves above.

---

## 4. What success looks like

A crowd frame is 43.4 ms. The GPU is 6.6 ms of it and the pacing is 3.9 ms, so **the
floor without touching either is about 11 ms — 90 fps** and utterly unreachable. A
realistic target is halving the two CPU halves: 21.4 + 11.0 = 32.4 ms becomes ~16 ms,
frame ~27 ms, **crowds at ~35 fps** — i.e. above the title's own cap, which would put
every scene in the game on the 31 fps pacing rather than below it.

That is the honest goal: **not "faster", but "never below the title's own frame rate".**
If §1 and §2 together cannot get the CPU halves under ~20 ms combined, say so and stop —
the remaining structural option is a second thread for command recording, which is a much
larger piece of work and should not be started on an assumption.
