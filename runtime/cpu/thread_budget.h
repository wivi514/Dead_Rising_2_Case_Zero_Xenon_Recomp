#pragma once
// ===================================================================================
// THE THREAD BUDGET — one number for the whole runtime, sized from the USER'S machine
// ===================================================================================
//
// WHY THIS EXISTS. The operator's instruction opening part 55 was two sentences and the
// second is a design constraint, not a preference: *"even if we really needed the 16 core
// we should still leave core empty for user background item and all. So we should do it
// smart and depend on amount of core the user has instead of aiming for my machine."*
//
// Without a central budget, the trap is arithmetic rather than philosophical. Part 55's
// plan proposes THREE parallel items (content guards, texture untile, command recording).
// If each sizes itself the way the guard pool did — `hardware_concurrency() >= 6 ? 4 : …`
// — then a six-core machine ends up with TWELVE workers plus the graphics pump plus the
// guest's own busy threads (A1 names JobThread0..5, cAsyncFileSystem and a BigFile
// decompress thread; part 54's profile has two of them at 80.7% and 70.9% of a core). The
// machine is then oversubscribed by a factor of two and every one of those threads is
// slower than it would have been alone.
//
// AND THE DENOMINATOR WAS WRONG. `std::thread::hardware_concurrency()` returns 16 on the
// operator's box, which is a **Ryzen 7 5700: 8 physical cores, 2 threads per core**. Every
// "N of 16 cores" figure written in this project since part 50 counted logical threads, so
// the process at 3.75 cores was using 47% of that machine and not 23%. Two SMT siblings
// share one core's execution resources: a second thread on a busy core buys perhaps 20-30%
// on a mixed workload and close to nothing on one already saturating the same units, which
// a memory-latency-bound hash loop very nearly is. So the budget is counted in PHYSICAL
// cores — logical threads are the right denominator for "how many runnable threads may
// exist", physical cores for "how much machine is left", and it is the second question
// this budget is spending. Gotchas 358 and 359.
//
// THE POLICY, stated so it can be argued with (docs/perf-plan-part55.md §0b):
//
//     physical  = counted from the machine, never divided by an assumed SMT factor
//     reserved  = 2      # one for the OS/compositor, one for the user's own software
//     committed = 3      # the graphics pump + the two busy guest threads, measured
//     budget    = clamp(physical - reserved - committed, 0, 6)
//
//   4-core laptop -> 0 workers, the fully serial path, which is the CORRECT answer and
//                    not a degraded one; it is the control arm and it is gated.
//   6-core        -> 1     8-core (the operator's) -> 3     12-core and up -> 6, capped.
//
// THE CAP OF 6 IS NOT TIMIDITY, it is the ceiling in §0 of the plan: the PM4 walk is
// serial because a command stream's meaning is positional, and draw submission is ordered
// because this title depends on overdraw order. Past five or six busy threads there is
// nothing left to give them, and part 53 measured extra workers doing real harm — 13.1
// points of core left the pump while 33.2 appeared on the workers, plus ~0.4 ms/frame of
// cache pollution charged to two phases that had nothing to do with the work moved
// (gotcha 344).
//
// ONE KNOB. `CZ_WORKERS=N` overrides the whole budget and `CZ_WORKERS=0` forces the serial
// path everywhere. One variable rather than one per pool, because otherwise the arms
// multiply and no one can say afterwards what a run was configured as. Per-pool arms that
// already exist (`CZ_VK_GUARD_WORKERS`, `CZ_VK_NO_PARALLEL_GUARD`) still win where they
// are set, and the start-up print says so.
//
// AND IT PRINTS. A performance number taken at an unknown thread count is not comparable
// with anything — that is gotcha 353's shape a third time over (a parallel measurement has
// a MACHINE as well as a workload, and naming only one is naming none). So the budget, the
// machine it was derived from and every share handed out are printed once at start-up.

#include <cstdint>

// The machine, counted rather than assumed. Both are cached after the first call.
unsigned ThreadBudget_PhysicalCores();
unsigned ThreadBudget_LogicalCpus();

// The policy's result: how many worker threads this whole runtime may run, across all
// pools. Zero is a legitimate answer and means "take the serial path".
unsigned ThreadBudget_Total();

// Claim up to `desired` workers for a named pool. Returns what was actually granted,
// which may be zero. First come, first served — and deliberately so rather than
// proportionally divided, because with one pool built (the content guards) a division
// rule would be fitted to a population of one. What makes that safe is that it is LOUD:
// every claim is printed with what was left, so a pool that gets nothing says so in the
// log instead of silently running serial. Revisit when the second pool lands.
//
// `overrideEnv` names a per-pool environment variable that wins if it is set (e.g.
// "CZ_VK_GUARD_WORKERS"); pass nullptr for none. Idempotent per pool name: asking twice
// returns the same grant rather than claiming twice, so a lazily-initialised pool can
// call this from a `static` initialiser without the count depending on call order.
unsigned ThreadBudget_Take(const char* pool, unsigned desired, const char* overrideEnv);

// Print the machine, the policy and every share handed out. Safe to call more than once;
// prints only when something has changed since the last call, so the line that matters —
// the final allocation — is the last one in the log.
void ThreadBudget_Report();
