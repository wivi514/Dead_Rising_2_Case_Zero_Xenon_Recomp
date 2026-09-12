# Part 117 kick-off — the pump on two cores, and the bound has moved to the guest

**READ `docs/perf-plan-part117.md` §0 and §4 FIRST.** §0 is the PMU table that says what
the pump is (instruction-bound, not bandwidth-bound — gotcha 551's mechanism retracted);
§4 is the execution record with every number's table. This file says where the port is
after the operator's second unattended order of 2026-09-12 (*"try to see if you can
separate the pm4 pump on multiple core"*), what shipped, and what is owed.

## §0. Where the frame is now (the operator's crowd, 1920x1080, Ryzen 7 5700)

| term | part 116 close | part 117 close | how measured |
|---|---|---|---|
| the shipped wall | 10.2-10.6 ms (~96 fps) | **9.0-9.1 ms (~110 fps)** | median, matched 250-draw bands, 3 runs a side (§4.2) |
| the walk (`cz-pump`) | part of the 10.1 | **3.3-3.7 ms**, off the critical path | `walk cpu` on the [fps] line |
| the renderer thread (`cz-draw`) | — | **8.6-9.1 ms/frame, ~0.9 of it idle** | `pump cpu` (the thread that calls DoDraw) + `[split] didle` |
| the guest's Main Thread | 7.7-7.9 ms CPU + 2.2 in the wait-any poll | **8.1-8.4 CPU + ~1.2 waits** — **THE LONGEST TERM NOW** | `guest main` + `[guestwait]` |
| the guest's Draw Thread | 5.7-5.9 CPU + 4.4 on our fence | 6.2-6.5 CPU + 2.4 on our fence | `guest draw` + `[guestwait]` |
| GPU | 3.2 ms | unchanged | part 116 §4.0 |

**The −1.19 decomposes as ~−0.3 from the two cores and ~−0.9 from the wait-any wake the
two cores made worth having** (campaign 2, plan §4.3: split alone 10.0-10.1, split + wake
9.06-9.13 at 8,000-8,500 draws, monotone). The part-109 bundle takes −0.56 off the
renderer thread and −0.13 off the frame under the split — the thread is not the bound —
so it stays the operator's call, OFF. The huge-page advice is not established either way.

`wall ~ max(pump, guest floor, GPU)` is now `wall ~ guest Main Thread (+ the ping-pong
with our renderer thread)`. **The next millisecond is the guest's, not ours**: its Main
Thread is 8.1-8.4 ms of the title's own simulation and render submission (part 116 §4.1
profiled it: flat, codegen-insensitive), and it grew +0.36 ms under the split from
sharing the machine with a fifth busy core. The renderer thread's remaining ~8 ms of work
still matters (it sets the ping-pong's other half and the p99), but no renderer-side
item alone can take the frame under ~8.5 ms at this crowd any more.

## §1. What shipped

1. **The two-core pump** (`gpu/pump_split.{h,cpp}`, pm4.cpp's walk hooks, vd.cpp's
   start and nap): the PM4 walk, the register file, the WAIT_REG_MEMs, the read-pointer
   publication and both ISRs stay on `cz-pump`; draws, resolves, the swap, every store
   into guest memory, the INTERRUPT packets and first-sight shader binds execute in
   stream order on `cz-draw` from ONE ordered stream, against a replica register file
   replayed from the same stream. **ON by default from six physical cores or eight
   logical CPUs; OFF below (`CZ_PUMP_SPLIT=1`/`=0` force either).** Three rules the design turns on are the
   header comment of `pump_split.h`; the crash that taught the fourth (one stream, not
   two) is gotcha 566.
2. **The wait-any wake follows the pump**: ON under the split (its parked trigger —
   "the pump under ~8 ms" — is met there), the poll on the one-thread pump.
   `CZ_WAITANY_WAKE=1`/`=0` force either.
3. `MADV_HUGEPAGE` on the guest map at mapping time, with the kernel's THP policy printed
   (`[mem] MADV_HUGEPAGE ...`); the physical views need root's
   `shmem_enabled=advise` to take it. `CZ_NO_HUGEPAGES=1` is the control. Measured
   (plan §4.3): +0.40 ms with it OFF, NOT monotone — not established; kept because it
   costs nothing (43 MB of the private range promoted).
4. `ProfScope`'s destructor tests the profiler flag inline (0.5% of the renderer thread
   with the profiler OFF).
5. Instruments: `walk cpu` on the [fps] line; the `[split]` health lines (idle by cause,
   the wait census by polled word); `tools/part117_memprobe.sh` (PMU counters + IBS on
   the pump); `tools/part117_gates.sh` (every standing gate plus synchronization
   validation and its poison, then a 10-minute soak, one process at a time).

## §2. What is owed

* **An operator session on the shipped default.** The split changes WHEN every guest
  store, interrupt and present happens relative to the walk; 30+ crowd runs, the A5 boot,
  the E3 picture, sync validation and the explorer soak are clean, but "does it feel the
  same" is theirs — and the p99 (12.1 vs 14.0 ms) says it should feel better.
* **Windows.** `pump_split.cpp` is portable (`pthread_getcpuclockid` guarded; `walk cpu`
  reads -1 there) but czwin was not reached this part either; the first Windows build is
  a gate.
* **The 4c/4t shape.** The default is ON from six physical cores or eight logical CPUs —
  the 4c/8t stand-in measured −1.3 ms with it (plan §4.4, one run each way) — and OFF on
  four cores without SMT, which nobody has measured.
* **The guest's +0.36/+0.53 ms under the split** — cache or SMT contention from the fifth
  busy core. `taskset -c 0-7` made it worse (the scheduler beats the pin). A smaller
  stream ring (the 32 MB one cycles 3.5 MB of fresh lines through L3 every frame) is the
  one untried lever; it needs the vblank ISR to survive a blocked walk.
* **D's ~0.9 ms idle a frame** is the guest/renderer ping-pong at the driver's fence
  blocks plus the swap rendezvous (the vblank walker clears `mirror+4` on the next tick).
  Both are the guest's timing; neither is a renderer item.
* **Item 3 (the instruction diet)** is a list, not a result: `DoDraw` is 36 KB of code at
  ~2,000 instructions a draw with no instruction above 1.6% — a flat profile that only a
  structural removal moves. The remaining small ones are in the plan's §3.

## §3. Gates on the shipped binary (plan §4.4) — `tools/part117_gates.sh`, 08:54-09:24

`--smoke` OK · unlowered switches 0 · shader dims clean · both PM4 oracles clean · E3
**+0.8411** (4 of 5 agreeing on layout, pinned 1280x720; part 116 read +0.8472) · A5
**exit 0** (5 permutation windows, 0 real — identical to part 116) · `no translated
shader` 0 · **synchronization validation 0 hazards** at 6,173 draws on the outdoor route,
**the poison producing 30** · `truncated=0` across every crowd log of the part · a
10-minute `CZ_AUTOCHUCK=EXPLORER` soak with `CZ_WAIT_TRACE=1` (the map closed twice,
no fault, no corrupt stream, 143 fps median at 2,400-3,200 draws).

## §4. Rules this part paid for

Gotchas 565-569: read the counters before naming a bound; two queues are two orders; a
pending store is the stream-order truth for a later wait; a pipelined pair is measured by
its bubbles; re-check every parked item's trigger when the bound moves.
