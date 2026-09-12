// The pump on two cores (perf plan part 117, item 1): the PM4 WALK on the pump thread,
// the RENDERER on a second thread, joined by an ordered queue.
//
// WHY THIS EXISTS. The pump — one thread doing the ring walk AND every per-draw call of
// the renderer — is 10.0-10.5 ms of a 10.2 ms crowd frame and it is the frame's bound
// (part 116 §0: `wall ~ max(pump, guest floor, GPU)`, guest 8.1, GPU 3.2). Part 111
// closed item B ("use the idle cores") after relocating the shared-block zero recovered
// nothing, and named the reason "bound by bytes, machine-wide" (gotcha 551). Part 117's
// step 0 read the PMU instead of inferring: the pump retires 1.6 instructions a cycle,
// pulls 0.7 GB/s from DRAM and serves 0.47% of its loads from RAM — it is an
// INSTRUCTION-bound serial thread, not a bandwidth-bound one, so two serial streams on
// two cores overlap where the same bytes on two cores did not. What relocating the zero
// measured was the pump re-touching every line the worker had zeroed.
//
// THE SPLIT. The pump thread (W) keeps everything it does today except the renderer:
// it walks the ring and the indirect buffers, keeps the register file, evaluates
// WAIT_REG_MEMs, publishes the read pointer and delivers the vblank ISR — ~2.3 ms of the
// 10.3. Every effect the renderer or the guest can observe goes into an ordered
// single-producer / single-consumer queue that a second thread (D, `cz-draw`) executes in
// stream order: draws, resolves, the swap, every store into guest memory, the interrupt
// packets and the first-sight shader binds. D keeps a REPLICA of the register file,
// replayed from a run log W appends to at every register write, so `DoDraw` still takes
// the `const uint32_t* regs` it always took — the renderer's code is untouched.
//
// THE ORDER IS THE CONTRACT, and three of its consequences are the design:
//
//   1. STORES ARE DEFERRED, not just draws. A fence (EVENT_WRITE) written by W at the
//      packet would tell the guest the GPU has consumed the draws before it while D has
//      not yet COPIED their vertex streams out of guest memory — and this title's UP
//      draws recycle exactly that memory behind exactly that fence. So every GuestStore
//      the walk would make is a queue op behind the draws it follows.
//   2. INTERRUPTS ARE DELIVERED BY W, AT D's POSITION. The source-1 ISR runs guest code
//      on the pump thread's guest context, reads the scratch mirror the stream stored
//      just before the INTERRUPT packet and is poisoned just after it (pm4.h). D reaches
//      the interrupt op, asks W, and WAITS until W has delivered — so the mirror it reads
//      is D's store and the poison is still behind it. W services these requests at the
//      top of every tick, inside the walk between packets, and while it waits for queue
//      space, so the two threads cannot wait on each other at once.
//   3. WAIT_REG_MEM stays on W. A wait on a word the GUEST writes behaves as today; a
//      wait on a word a DEFERRED store writes holds (the brake returns, the tick retries)
//      until D executes that store — the pipeline drains at the hand-off blocks, which is
//      where hardware's CP stalls too.
//
// OFF unless CZ_PUMP_SPLIT=1 (this part's arm; the shipped default is the one-thread
// pump every release has carried). The [fps] line's `pump cpu` names the thread that
// calls DoDraw — D under the split — and gains `walk cpu` for W, so the two halves of the
// bill are printed beside each other.
#pragma once

#include <cstdint>

struct Pm4Draw;
struct Pm4ShaderBinding;
struct Pm4VsPaletteWrites;

namespace split {

// Read on the hot paths of the walk (one load per register run / store / draw); set once
// by Start() before the ring is walked and never cleared.
extern bool g_on;

// The state a draw carries from the walk that DoDraw used to read off pm4's globals at
// the packet: the memo stamps and the bone-palette write extents. Captured by W at the
// draw packet, consumed by D at the draw.
struct DrawCtx
{
    uint64_t aluVersion[2];
    uint64_t fetchVersion;
    uint32_t palCoverExtent, palPartialExtent, palCoverBursts, palPartialBursts;
    uint32_t palHighWater;
};

// W side ------------------------------------------------------------------------------
// Spawn D. `deliverInterrupt` is the walk's own source-1 delivery (pm4.cpp's sink call),
// run on W when D asks. Returns false (and leaves g_on false) when the arm is not set.
bool Start(uint8_t* base, void (*deliverInterrupt)());
void LogRun(uint32_t index, uint32_t count, const uint32_t* src);
void EnqueueDraw(const Pm4Draw& d, const Pm4ShaderBinding& vs, const Pm4ShaderBinding& ps,
                 const DrawCtx& ctx);
void EnqueueStore(uint32_t va, uint32_t value);   // rides the log, not the op ring
// W's nap between ticks: a plain sleep on the one-thread pump; under the split, a wait
// an interrupt request from D cuts short.
void Nap(int us);
void EnqueueInterrupt();
void EnqueueSwap(uint32_t frontBuffer, uint32_t width, uint32_t height);
void EnqueueShaderBind(uint32_t type, uint64_t hash, const uint8_t* code, uint32_t sizeDwords);
// Deliver the interrupts D is waiting on. Called at the top of every pump tick, between
// packets inside the walk, and inside every wait for queue space.
void ServiceInterrupts();
// W: a WAIT_REG_MEM on guest memory just evaluated false; `va` is the polled word.
void NoteWaitUnmet(uint32_t va);
// W: the value the LAST deferred store to `va` will write, if D has not landed it yet.
// This is what lets the walk run AHEAD of D at the driver's pipeline-drain blocks
// (EVENT_WRITE then WAIT_REG_MEM on the same word, ~5 a frame): on hardware the CP
// waits there for the shader pipeline; here D executes the stream in order anyway, so
// nothing W does after the wait can observe the difference — every store, interrupt
// and draw after it is behind the pending store in D's queue. Without this W drained
// the pipeline at every block (46 of 52 unmet evaluations a frame were on our own
// pending store) and D sat idle 1.5 ms a frame.
bool PendingStoreValue(uint32_t va, uint32_t* value);
void NoteWaitSatisfiedByPending();

// D side / the renderer ---------------------------------------------------------------
// The context of the draw D is executing, or nullptr on the one-thread pump (the
// renderer then reads pm4's globals as before).
const DrawCtx* CurrentDraw();
// The bone-palette take under the split: the extents accumulated over every draw op
// since the renderer last took them — the same semantic Pm4_TakeVsPaletteWrites has on
// the one-thread pump, where the take runs only on dynamic-VS draws.
Pm4VsPaletteWrites TakePalette();
uint32_t PaletteHighWater();

// Instruments: W's CPU clock (seconds; -1 when not split) for the [fps] line, and the
// queue's own counters for the stats block.
double WalkCpuSeconds();
struct Stats
{
    uint64_t ops, draws, stores, storesQueued, interrupts, swaps, logDwords, runs, runsMerged;
    uint64_t wSpaceWaits;    // W stalled for queue space (must be ~0 at any load)
    uint64_t dEmptyWaits;    // D found the queue empty and parked
    uint64_t dIrqWaitNs;     // D's time waiting for W to deliver an interrupt
    uint64_t irqDelivered;
    uint64_t dIdleNs;        // D with nothing to do (spinning or parked)
    uint64_t waitsUnmet;     // W: WAIT_REG_MEM evaluations that failed...
    uint64_t waitsOnOurStore;// ...of which the word is one D has a store pending to
    uint64_t waitsByPending; // ...and of THOSE, the ones the pending value satisfies (run-ahead)
    uint64_t dIdleByKindNs[5];  // idle after: other, draw, store, irq, swap
    uint32_t waitVa[4];         // the first four polled words seen unmet, and how often
    uint64_t waitVaCount[4];
};
Stats GetStats();

} // namespace split
