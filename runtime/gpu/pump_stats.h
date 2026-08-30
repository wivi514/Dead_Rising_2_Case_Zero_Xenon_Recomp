// Where the graphics pump's wall time goes — the half of the frame CZ_VK_PROFILE
// reports as `outside` and cannot see into.
//
// WHY THIS EXISTS. The frame profile splits a frame into renderer phases and calls the
// remainder `outside`, and on a gameplay frame `outside` is 58-62% of it — about 51 ms
// of an 85 ms frame. That number was read for a whole session as "the guest's own
// simulation plus the command processor", i.e. as WORK, and every optimisation it
// suggested was aimed at making some piece of that work cheaper.
//
// It is mostly not work. `perf record` over 60 s of gameplay puts 77.6% of the process's
// cycles in ONE thread and 60% of them in two guest functions (`sub_8283C6C8` under
// `sub_82845160`, finding 38's ring-progress spin) — a busy-wait that burns a core and
// is on nobody's critical path — while the pump thread, which is where the command
// processor and the whole renderer run, accounts for 10.8%. A cycles profile cannot see
// the rest of the pump's frame because the pump is not on a CPU for it: it is asleep in
// the `sleep_for(vblankMs)` at the top of its own loop.
//
// So the three quantities that decide the frame rate are the ones below, and none of
// them appears in any instrument this port owns:
//
//   ticks    how many pump iterations a frame takes. The ring walk stops at every
//            unsatisfied WAIT_REG_MEM and resumes on the NEXT tick (phase C part 4), so
//            a frame containing N hand-off waits costs at least N sleep periods.
//   sleepNs  the sleep itself. This is the quantity that is neither work nor GPU wait.
//   walkNs   Pm4_Execute — the command processor, the renderer it drives, and the GPU
//            fence wait inside it. `submit` in the frame profile is a subset of this.
//   isrNs    the guest's own vblank ISR, which runs the swap-queue walker (part 5).
//
// Always on and unconditional: four clock reads per tick at ~62 ticks a second is
// nothing measurable, and an instrument that is off by default is an instrument that is
// not in the campaign logs when a question turns up later (gotcha 95).
#pragma once

#include <cstdint>

// PART 51 ADDS THE ONLY QUESTION THE FOUR FIELDS ABOVE CANNOT ANSWER: was the sleep ON
// THE CRITICAL PATH? `sleepNs` says how long the pump was not running; it cannot say
// whether anybody was waiting for it. Both readings are consistent with 12% of the wall
// clock in that line:
//
//   harmless  the ring is empty, the guest has not kicked yet, and a pump that woke
//             sooner would only spin. Sleeping is then the correct thing to do.
//   costly    packets are already waiting, or a WAIT_REG_MEM the walk stopped at has
//             since been satisfied. Every nanosecond of the sleep is then frame time,
//             and the Draw Thread is burning a core spinning on our read pointer while
//             we sleep (finding 38 — the busiest thread in this process is that spin).
//
// The discriminator is what the walk does NEXT. If the walk immediately after a sleep
// advances the ring cursor, then there was work to do and the sleep delayed it; if it
// returns the same cursor, the sleep cost nothing. So `sleepBeforeProgressNs` is an
// UPPER BOUND on the latency the tick period is adding — upper, because work that
// arrived halfway through a sleep was only delayed by the remaining half, and nothing
// on this side can see when it arrived. Quote it as a bound, never as a saving.
struct PumpStats
{
    uint64_t ticks;   // pump loop iterations
    uint64_t sleepNs; // time asleep at the top of the loop
    uint64_t walkNs;  // time inside Pm4_Execute (command processor + renderer + submit)
    uint64_t isrNs;   // time inside the guest's vblank ISR
    uint64_t progressTicks;        // ticks whose walk advanced the ring cursor
    uint64_t sleepBeforeProgressNs; // ...and the sleep that immediately preceded them
    // The walk IN PROGRESS, as a start timestamp, or 0 if the pump is not inside one.
    // walkNs only accumulates when a walk returns, and the present happens inside a walk,
    // so a reader at present time must add (now - walkStartNs) or it will attribute a
    // hitch to the frame after the one that suffered it.
    uint64_t walkStartNs;
    // Ticks that skipped the sleep entirely because the walk before them made ring
    // progress (the eager tick, 2026-08-29). The share eager/ticks is the engagement
    // gate for CZ_PM4_NO_EAGER_TICK's arm.
    uint64_t eagerTicks;
    // Ticks whose sleep was shortened below the tick period because the ring was HELD
    // at a WAIT_REG_MEM (the held-wait fast retry, 2026-08-29). The engagement gate
    // for CZ_PM4_NO_FAST_HELD's arm.
    uint64_t heldFastTicks;
};

// A snapshot. Like ChainStats_Read these are independent relaxed counters, so the fields
// are not consistent with each other to the nanosecond — which is right for a rate over
// a window and wrong for arithmetic between two of them within one tick.
PumpStats PumpStats_Read();
