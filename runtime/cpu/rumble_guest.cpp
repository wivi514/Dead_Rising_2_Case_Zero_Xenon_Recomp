// THE TITLE'S RUMBLE, ON THE CONSOLE'S CLOCK (part 108) — and the probe that found it.
//
// WHY IT EXISTS. Vibration reached the pad in part 108 and the operator's first report
// was "it vibrates but it's always the same, grabbed or shooting". The host trace
// showed every episode the title sent was FULL for exactly three controller updates
// (then sometimes 40% for three to five more), and the probe below traced every level
// to ONE function, sub_82805A58, the title's rumble manager tick, which the frame loop
// calls once per FRAME (sub_82805F90 <- sub_8248F728 <- sub_824A1EC8). Read in full:
//
//   * a 100-entry effect table at 0x82AD66B8, 16 bytes each: {ticks remaining, loop
//     flag, owner, effect definition}; sub_828034B0 registers one with ticks = the
//     definition's max duration (+0x18), sub_82803580 unregisters by owner+type;
//   * the definition (base ctor sub_828052B8): per motor a LEVEL float (+0x08/+0x0C)
//     and a DURATION in TICKS, an integer (+0x10/+0x14); two classes, constant
//     (GetLevel sub_82804138: level while elapsed <= duration, else 0) and ramp
//     (sub_82804170, the same with a fade);
//   * the tick: for each pad, the max level over its effects (lower priority number
//     wins outright, equal priorities max), SetMotor (vt+0x54, sub_828067F8) per
//     motor, Send (vt+0x58, sub_82806828 -> XamInputSetState), THEN every entry's
//     ticks-remaining is decremented by ONE, a looping entry reloading at zero.
//
// So an effect's length is a count of frames, and the title's own present interval is
// two vblanks: the durations were authored in 30 fps frames. A "3" is 100 ms on the
// console; at the 110 fps the operator plays at it is 27 ms, which a motor barely
// spins up for — every effect collapsed to the same click. The fix is to let the tick
// run at 30 Hz of real time: this hook accumulates the wall clock between calls and
// runs the original once per 1/30 s, never more than once per call (a long frame is
// one tick on the console too), never banking more than one tick. At 30 fps it is
// the console; at 60 (a shipped configuration, CZ_FPS_CAP=60) it gives the 30-fps
// authoring rather than the console's own halved effects at 60. `CZ_RUMBLE_TICK_HZ=N`
// sets the rate; `=0` is the control arm (the tick per frame, the runtime as it was
// on the day the operator reported it).
//
// The probe (CZ_RUMBLE_TRACE): sub_828067F8 is SetMotor(index r4, level f1 in 0..1);
// sub_828068D8 is StopAll. Armed by the same switch as the host side, so one env var
// gives both halves of a rumble's life.
//
// BOUNDED, AND THE REASON IS A 248 GB LOG. The first probe printed on CHANGE of
// (value, caller) per motor and one operator session wrote 248 GB in a few minutes
// (the probe's own lines numbered 356; what filled the file was never identified,
// because the file had to go before the disk did). So: a histogram of (caller, motor,
// value-class) printed every 10 s whatever the call rate, plus change lines capped at
// 20 a second with the overflow counted — and operator sessions now run under
// ~/DR2CZ-troubleshooting/part108/watched_play.sh, which kills the game and keeps the
// tail if a log passes 2 GB.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <map>

#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_828067F8);
extern "C" PPC_FUNC(__imp__sub_828068D8);
extern "C" PPC_FUNC(__imp__sub_82805A58);

namespace
{
bool Armed()
{
    static const bool armed = getenv("CZ_RUMBLE_TRACE") != nullptr;
    return armed;
}
double Now()
{
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::mutex g_mu;
struct Key { uint32_t lr; uint32_t idx; int vclass; bool operator<(const Key& o) const
    { return lr != o.lr ? lr < o.lr : idx != o.idx ? idx < o.idx : vclass < o.vclass; } };
std::map<Key, uint64_t> g_hist;          // value classes: 0 = zero, 1 = (0,1], 2 = other/NaN
std::map<uint32_t, uint64_t> g_stopHist; // StopAll by caller
double g_nextReport = 10.0;
double g_secStart = 0.0; int g_linesThisSec = 0; uint64_t g_suppressed = 0;

int VClass(float v) { return v == 0.f ? 0 : (v > 0.f && v <= 1.f) ? 1 : 2; }

void MaybeReport(double t)
{
    if (t < g_nextReport) return;
    g_nextReport = t + 10.0;
    fprintf(stderr, "[rumble-guest] %.0f s: setter callers (caller motor class=count)", t);
    for (const auto& [k, n] : g_hist)
        fprintf(stderr, "  %08X m%u %s=%llu", k.lr, k.idx,
                k.vclass == 0 ? "zero" : k.vclass == 1 ? "lvl" : "odd", (unsigned long long)n);
    for (const auto& [lr, n] : g_stopHist)
        fprintf(stderr, "  StopAll<-%08X=%llu", lr, (unsigned long long)n);
    fprintf(stderr, "  | change lines suppressed so far: %llu\n", (unsigned long long)g_suppressed);
    g_hist.clear(); g_stopHist.clear();
}

bool LineBudget(double t)
{
    if (t - g_secStart >= 1.0) { g_secStart = t; g_linesThisSec = 0; }
    if (g_linesThisSec < 20) { ++g_linesThisSec; return true; }
    ++g_suppressed; return false;
}
}

PPC_FUNC(sub_828067F8)
{
    if (Armed())
    {
        const uint32_t idx = ctx.r4.u32;
        const float v = float(ctx.f1.f64);
        const uint32_t lr = uint32_t(ctx.lr);
        const double t = Now();
        std::lock_guard<std::mutex> lock(g_mu);
        ++g_hist[Key{lr, idx < 2 ? idx : 2u, VClass(v)}];
        // Change detection is PER PAD OBJECT (r3): the tick sets every connected pad's
        // motors in one pass, and the first version keyed on the motor index alone, so
        // pad 0's 1.0 followed by pad 1's 0.0 read as a change every tick — a burst
        // that ate the line budget and a timeline that measured every episode as 0 ms.
        static std::map<uint32_t, float> last[2];
        if (idx < 2)
        {
            auto& slot = last[idx][ctx.r3.u32];
            if (slot != v)
            {
                slot = v;
                if (LineBudget(t))
                    fprintf(stderr, "[rumble-guest] t=%.3f pad@%08X SetMotor(%u, %.3f) from %08X\n",
                            t, ctx.r3.u32, idx, v, lr);
            }
        }
        MaybeReport(t);
    }
    __imp__sub_828067F8(ctx, base);
}

PPC_FUNC(sub_828068D8)
{
    if (Armed())
    {
        const double t = Now();
        std::lock_guard<std::mutex> lock(g_mu);
        ++g_stopHist[uint32_t(ctx.lr)];
        if (LineBudget(t))
            fprintf(stderr, "[rumble-guest] t=%.3f StopAll from %08X\n", t, uint32_t(ctx.lr));
        MaybeReport(t);
    }
    __imp__sub_828068D8(ctx, base);
}

// The tick, at the console's rate.
namespace
{
double TickHz()
{
    static const double hz = [] {
        const char* e = getenv("CZ_RUMBLE_TICK_HZ");
        const double v = e ? atof(e) : 30.0;
        if (e)
            fprintf(stderr, "[rumble] CZ_RUMBLE_TICK_HZ=%s: the title's rumble tick runs %s\n", e,
                    v > 0 ? "at that rate of real time" : "ONCE PER FRAME (the control arm)");
        return v > 0 ? v : 0.0;
    }();
    return hz;
}
}

PPC_FUNC(sub_82805A58)
{
    const double hz = TickHz();
    if (hz <= 0.0)
    {
        __imp__sub_82805A58(ctx, base);
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    static double acc = 0.0;
    static bool announced = false;
    const auto now = std::chrono::steady_clock::now();
    acc += std::chrono::duration<double>(now - last).count();
    last = now;
    const double period = 1.0 / hz;
    if (acc < period)
        return;                          // not yet a console frame's worth of time
    acc -= period;
    if (acc > period)
        acc = period;                    // bank at most one tick: a stall is one tick
    if (!announced)
    {
        announced = true;
        fprintf(stderr, "[rumble] the title's rumble tick (sub_82805A58) runs at %.0f Hz of "
                        "real time; effect durations are counts of 30 fps frames\n", hz);
    }
    __imp__sub_82805A58(ctx, base);
}
