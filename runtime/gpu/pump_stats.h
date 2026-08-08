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

struct PumpStats
{
    uint64_t ticks;   // pump loop iterations
    uint64_t sleepNs; // time asleep at the top of the loop
    uint64_t walkNs;  // time inside Pm4_Execute (command processor + renderer + submit)
    uint64_t isrNs;   // time inside the guest's vblank ISR
};

// A snapshot. Like ChainStats_Read these are independent relaxed counters, so the fields
// are not consistent with each other to the nanosecond — which is right for a rate over
// a window and wrong for arithmetic between two of them within one tick.
PumpStats PumpStats_Read();
