// Argument probes on named guest functions, for hunting a bad pointer to its source.
//
// WHY THIS EXISTS
// ---------------
// The crash reporter (cpu/crash_report.cpp) answers "which guest instruction faulted
// and on what address". It cannot answer the next question, which is always "where
// did that pointer come from" — by the time the fault happens the producing frame has
// usually returned, and the register dump for the innermost frame is partly stale
// because the compiler keeps PPCContext fields in host registers between calls.
//
// So the follow-up instrument is a probe at the *producing* site, printing its
// arguments and the objects they point at, on entry, before anything can go wrong.
// This is the alias/weak-link seam CLAUDE.md gotcha 6 describes: XenonRecomp emits
// every guest function as `__imp__sub_X` plus a WEAK alias `sub_X`, so a strong
// `PPC_FUNC(sub_X)` here silently takes over every call site in the image.
//
// WHAT IS IN HERE IS A SOLVED CASE, KEPT AS THE WORKED EXAMPLE
// ------------------------------------------------------------
// The hooks below found docs/phase1-notes.md finding 27 and are left in place because
// the *shape* of that hunt is the reusable part, and Case West will want it:
//
//   1. the crash reporter gives the faulting instruction, via addr2line on the raw
//      host pc — the only field in that report that cannot be stale;
//   2. read the generated C++ to see which register the instruction dereferences and
//      which function produced it;
//   3. probe the producer on entry — is the value already bad, or does it go bad
//      later? Here it was GOOD, which eliminated the entire "one of our imports
//      handed the guest a null" family of hypotheses in a single run;
//   4. if it goes bad later, watch the value AND the register across every call in
//      between. Whichever call changes one of them is the answer.
//
// Step 4 named it. `sub_82955780` went in with r31 = the descriptor and came out with
// r31 = its own local value, while the descriptor's memory was untouched — so this was
// never a bad pointer at all. It was a callee returning without its epilogue, because
// a `bctr` had not been lowered to a switch. See docs/xenia-capture-analysis.md
// section 15 and tools/find_unlowered_switches.py.
//
// HOW TO ADD ONE
// --------------
// Copy a block below, change the address, and change what it dumps. The address must
// be a real function start — a mid-function address has no `sub_` symbol and the link
// fails, which is the good failure.
//
// Everything is behind CZ_ARG_PROBE, read once. When it is off each probe costs one
// predictable branch, which is the standard this project holds its instruments to
// (gotcha 7: a probe expensive enough to change the timing manufactures the
// behaviour it reports, and this code runs inside the per-frame render path).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../gpu/d3d_draw.h"
#include "../kernel/audio.h"
#include "../kernel/memory.h"
#include "chain_stats.h"
#include "guest_thread.h"
#include "ppc_recomp_shared.h"

// `ppc_recomp_shared.h` declares only the WEAK alias `sub_X`, never the real body
// `__imp__sub_X`, so every hook has to declare the one it wraps. The `extern "C"` is
// not optional: the recompiler defines the body with PPC_FUNC_IMPL, which is
// `extern "C" PPC_FUNC`, while a plain PPC_FUNC declaration here would be
// C++-mangled and fail to link (gotcha 33, from the other direction).
extern "C" PPC_FUNC(__imp__sub_82959360);
extern "C" PPC_FUNC(__imp__sub_82788478);
extern "C" PPC_FUNC(__imp__sub_82822430);
extern "C" PPC_FUNC(__imp__sub_829565B8);
extern "C" PPC_FUNC(__imp__sub_82955780);
extern "C" PPC_FUNC(__imp__sub_82689A70);

// ---------------------------------------------------------------------------------
// The always-on chain counters (cpu/chain_stats.h says why they are separate from the
// probe above, and what each link means). Relaxed atomics on hooks that already exist,
// so they cost nothing and are present on EVERY run rather than only on a probe run.
//
// They live at the top of the file because their first user — the worker's token
// interpreter hook — is several hundred lines above the fence probe they belong with.
namespace {

std::atomic<uint64_t> g_chainArms{ 0 };
std::atomic<uint64_t> g_chainIsr{ 0 };
std::atomic<uint64_t> g_chainKicks{ 0 };
std::atomic<uint64_t> g_chainKickRepeat{ 0 };
std::atomic<uint64_t> g_chainWalks{ 0 };
std::atomic<uint64_t> g_chainDrains{ 0 };
std::atomic<uint64_t> g_chainRingsub{ 0 };
std::atomic<uint64_t> g_chainRingsubEnts{ 0 };
std::atomic<uint64_t> g_chainSegSubmit{ 0 };
std::atomic<uint64_t> g_chainSegQueued{ 0 };
std::atomic<uint64_t> g_chainResolves{ 0 };
std::atomic<uint64_t> g_chainAsyncSubmit{ 0 };

// Distinct token-buffer pointers ever kicked.
//
// A bounded open-addressed set rather than a std::set, because this runs on the ISR
// path and the runaway arm reaches it eleven million times in a boot: an allocation
// there would change the thing being measured (gotcha 7). 8,192 slots is far more than
// the interesting range — the healthy arm advances its pointer once per frame, i.e. a
// few thousand in a two-minute boot, and the sick arm is expected to sit on a handful.
//
// The cap is reported rather than hidden: once the table is full the count SATURATES,
// and a saturated count is a floor, not a number (gotcha 109). `ChainStats_Read` says
// so by returning the saturated value, and the printer flags it with a '+'.
constexpr size_t kKickSetSlots = 8192;
std::atomic<uint32_t> g_kickSet[kKickSetSlots];
std::atomic<uint64_t> g_kickDistinct{ 0 };

void NoteKickBuffer(uint32_t buf)
{
    if (!buf || g_kickDistinct.load(std::memory_order_relaxed) >= kKickSetSlots / 2)
        return; // saturated: stop probing, the count is already a floor
    // Fibonacci hash, then linear probe. Races can insert the same pointer twice and
    // over-count by a handful; that is acceptable for a distinctness ORDER OF MAGNITUDE
    // and a lock here would sit on the interrupt path.
    size_t h = (size_t(buf) * 2654435761u) % kKickSetSlots;
    for (size_t i = 0; i < 64; i++)
    {
        std::atomic<uint32_t>& slot = g_kickSet[(h + i) % kKickSetSlots];
        uint32_t cur = slot.load(std::memory_order_relaxed);
        if (cur == buf)
            return;
        if (cur == 0 && slot.compare_exchange_strong(cur, buf, std::memory_order_relaxed))
        {
            g_kickDistinct.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

} // namespace

ChainStats ChainStats_Read()
{
    ChainStats s{};
    s.arms = g_chainArms.load(std::memory_order_relaxed);
    s.isr = g_chainIsr.load(std::memory_order_relaxed);
    s.kicks = g_chainKicks.load(std::memory_order_relaxed);
    s.kickDistinct = g_kickDistinct.load(std::memory_order_relaxed);
    s.kickRepeat = g_chainKickRepeat.load(std::memory_order_relaxed);
    s.walks = g_chainWalks.load(std::memory_order_relaxed);
    s.drains = g_chainDrains.load(std::memory_order_relaxed);
    s.ringsub = g_chainRingsub.load(std::memory_order_relaxed);
    s.ringsubEnts = g_chainRingsubEnts.load(std::memory_order_relaxed);
    s.segSubmit = g_chainSegSubmit.load(std::memory_order_relaxed);
    s.segQueued = g_chainSegQueued.load(std::memory_order_relaxed);
    s.resolves = g_chainResolves.load(std::memory_order_relaxed);
    s.asyncSubmit = g_chainAsyncSubmit.load(std::memory_order_relaxed);
    return s;
}

void ChainStats_CountIsr() { g_chainIsr.fetch_add(1, std::memory_order_relaxed); }
void ChainStats_CountResolve() { g_chainResolves.fetch_add(1, std::memory_order_relaxed); }
void ChainStats_CountAsyncSubmit()
{
    g_chainAsyncSubmit.fetch_add(1, std::memory_order_relaxed);
}

namespace {

bool ProbeEnabled()
{
    static const bool on = getenv("CZ_ARG_PROBE") != nullptr;
    return on;
}

// Print a 32-bit guest pointer and the first few words it points at. A pointer that
// is null or outside the guest space is said so rather than dereferenced — this runs
// on data we already believe is corrupt.
void Dump(const char* label, uint32_t p, int words = 4)
{
    uint8_t* base = g_memory.base;
    if (p == 0)
    {
        fprintf(stderr, "    %-6s = NULL\n", label);
        return;
    }
    if (uint64_t(p) + words * 4 > PPC_MEMORY_SIZE)
    {
        fprintf(stderr, "    %-6s = %08X  <outside the guest space>\n", label, p);
        return;
    }
    fprintf(stderr, "    %-6s = %08X ->", label, p);
    for (int i = 0; i < words; i++)
        fprintf(stderr, " %08X", PPC_LOAD_U32(p + 4 * i));
    fprintf(stderr, "\n");
}

// The descriptor address sub_829565B8 is currently working on, published so the
// probes on ITS callees can report the one word that matters. Deliberately a plain
// non-atomic uint32_t: only the main guest thread runs this path, and making it
// atomic would imply a synchronisation this instrument does not have.
uint32_t g_watchDesc = 0;

// Print r31 and the watched descriptor word around a callee. Which of the two moves
// is the whole question:
//   the WORD changes  -> something wrote through a stale/aliasing pointer
//   r31 changes       -> the callee returned without restoring non-volatiles, which
//                        is the truncated-function / bad-bounds signature (gotcha 3)
void Watch(const char* who, const char* when, PPCContext& ctx, uint8_t* base)
{
    // Say so when there is no descriptor yet rather than printing a zero: a zero here
    // reads exactly like the corruption being hunted, and the first version of this
    // line duly reported "*** ZEROED ***" for every call that happened before the
    // first sub_829565B8 entry.
    if (!g_watchDesc)
    {
        fprintf(stderr, "[watch] %-14s %-5s r31=%08X  [desc+4]=(no descriptor yet)\n", who,
                when, ctx.r31.u32);
        return;
    }
    const uint32_t word = PPC_LOAD_U32(g_watchDesc + 4);
    fprintf(stderr, "[watch] %-14s %-5s r31=%08X  [desc+4]=%08X%s\n", who, when,
            ctx.r31.u32, word, word == 0 ? "   *** ZEROED ***" : "");
}

} // namespace

// sub_82959360 — builds, in its own stack frame at r1+112, the five-word descriptor
// that sub_829565B8 consumes:
//
//     desc+0  = arg0 + 32
//     desc+4  = [[arg0+0] + 8]      <- the matrix pointer the crash dereferenced
//     desc+8  = [[arg0+0] + 0]
//     desc+12 = [[arg0+4] + 0]
//     desc+16 = float [[arg0+12] + 8]
//
// sub_829565B8 loads three vectors from desc+4 (`lvx128 v8,r0,r8` and +16/+32, i.e. a
// 48-byte matrix) with no null check, so a null there faults on guest address 0.
// This probe existed to answer whether desc+4 was ALREADY null when written. It was
// not — it held a valid A434E9F0 on the one call before the crash, and that is what
// ruled out every "our kernel returned null" hypothesis and forced the search
// downstream into sub_829565B8's own callees.
PPC_FUNC(sub_82959360)
{
    if (ProbeEnabled())
    {
        static std::atomic<uint64_t> calls{ 0 };
        static std::atomic<int> shown{ 0 };

        uint8_t* base = g_memory.base;
        const uint32_t n = uint32_t(calls.fetch_add(1, std::memory_order_relaxed));
        const uint32_t arg0 = ctx.r3.u32;
        const uint32_t owner = arg0 ? PPC_LOAD_U32(arg0 + 0) : 0;
        const uint32_t matrix = owner ? PPC_LOAD_U32(owner + 8) : 0;

        // The first few calls are the control: without them a report that only fires
        // on the bad call cannot say whether the slot is EVER non-null (gotcha 30 —
        // a test that has never passed has not been shown capable of passing).
        const bool bad = matrix == 0;
        if ((n < 6 || bad) && shown.fetch_add(1, std::memory_order_relaxed) < 40)
        {
            fprintf(stderr, "[probe] sub_82959360 call #%u%s\n", n,
                    bad ? "   *** desc+4 (the matrix) is NULL ***" : "");
            Dump("arg0", arg0, 6);
            Dump("owner", owner, 6);   // = [arg0+0]
            Dump("o+0", owner ? PPC_LOAD_U32(owner + 0) : 0, 4);
            Dump("o+8", matrix, 4);    // the matrix that must be 48 bytes of floats
            Dump("arg1", ctx.r4.u32, 4);
            Dump("arg2", ctx.r5.u32, 4);
        }
    }
    __imp__sub_82959360(ctx, base);
}

// sub_829565B8 — the consumer, and the function that faults.
//
// It takes the descriptor sub_82959360 just built as arg0, keeps it in the
// non-volatile r31, then makes TWO indirect calls through vtable slot +60 and one
// direct call to sub_82955780 before finally doing `lwz r8,4(r31)` and loading a
// matrix from r8. The probe above proved the descriptor's +4 slot is a valid
// pointer when it is written, so between the write and the load either the memory
// or r31 itself is destroyed.
//
// That narrows the suspects to the three calls, and the interesting one is a callee
// that returns WITHOUT restoring non-volatiles — the classic signature of a function
// whose bounds are wrong (CLAUDE.md gotcha 3: a mis-detected jump table makes the
// recompiler emit a bare `return;` for a case body, with no epilogue, so the caller
// resumes with the callee's registers). So this dumps the two indirect targets and
// says whether each is a known function start, which is the cheapest way to see a
// bounds problem from the outside.
PPC_FUNC(sub_829565B8)
{
    if (!ProbeEnabled())
    {
        __imp__sub_829565B8(ctx, base);
        return;
    }

    static std::atomic<int> shown{ 0 };
    const uint32_t desc = ctx.r3.u32;
    g_watchDesc = desc;
    const int n = shown.fetch_add(1, std::memory_order_relaxed);

    // An indirect target is `[[obj] + 60]`. Resolving it here, before the call, is
    // the only chance to see it: by the time anything goes wrong ctr has moved on.
    auto slot60 = [&](uint32_t obj) -> uint32_t {
        if (!obj || uint64_t(obj) + 4 > PPC_MEMORY_SIZE) return 0;
        const uint32_t vt = PPC_LOAD_U32(obj + 0);
        if (!vt || uint64_t(vt) + 64 > PPC_MEMORY_SIZE) return 0;
        return PPC_LOAD_U32(vt + 60);
    };

    if (n < 4)
    {
        const uint32_t o8 = PPC_LOAD_U32(desc + 8);
        const uint32_t o12 = PPC_LOAD_U32(desc + 12);
        const uint32_t t1 = slot60(o8), t2 = slot60(o12);
        fprintf(stderr, "[probe] sub_829565B8 entry #%d\n", n);
        Dump("desc", desc, 5);
        fprintf(stderr, "    bctrl#1 target %08X %s\n", t1,
                g_memory.FindFunction(t1) ? "(known function start)" : "(NOT a function start)");
        fprintf(stderr, "    bctrl#2 target %08X %s\n", t2,
                g_memory.FindFunction(t2) ? "(known function start)" : "(NOT a function start)");
    }

    __imp__sub_829565B8(ctx, base);

    // After the call the descriptor should be untouched — it belongs to the caller.
    // Printing it on the way out turns "something corrupts it" into a yes/no, and
    // this line only ever runs when the function did NOT fault.
    if (n < 4)
    {
        fprintf(stderr, "[probe] sub_829565B8 exit  #%d (returned without faulting)\n", n);
        Dump("desc", desc, 5);
    }
}

// The calls sub_829565B8 makes between writing the descriptor and reading it. Each
// prints r31 and the descriptor word on the way in and on the way out, so the
// corrupting call names itself rather than being deduced. This is the pair of lines
// that closed finding 27:
//
//     [watch] sub_82955780   in    r31=88040DE0  [desc+4]=A434E9F0
//     [watch] sub_82955780   out   r31=88040BC0  [desc+4]=A434E9F0
//
// Memory intact, r31 destroyed — so not a bad pointer, a missing epilogue.
#define CZ_WATCH_CALLEE(name)                                                        \
    PPC_FUNC(name)                                                                   \
    {                                                                                \
        static std::atomic<int> seen{ 0 };                                           \
        const bool show = ProbeEnabled() && seen.fetch_add(1, std::memory_order_relaxed) < 3; \
        if (show) Watch(#name, "in", ctx, base);                                     \
        __imp__##name(ctx, base);                                                    \
        if (show) Watch(#name, "out", ctx, base);                                    \
    }

CZ_WATCH_CALLEE(sub_82955780)
CZ_WATCH_CALLEE(sub_82689A70)

// ---------------------------------------------------------------------------
// The DVD-cache trail (phase1-notes finding 33) — the worked example of a
// correct instrument pointing at an innocent subsystem
// ---------------------------------------------------------------------------
//
// These three probes no longer print anything, and that is the result rather than a
// bug: the code they watch is not reached any more. They are kept because the shape
// of what they did is the reusable part, and it is not the shape anyone expects.
//
// The question was "why does our boot enter the title's DVD-cache subsystem when
// hardware does not". Reading the tree statically kept flipping — sub_82829098 has
// four separate routes to a zero return and two of them are *failure* paths — so the
// return values got printed instead:
//
//     [ret] sub_82823A58   -> 1    '\Device\Image' is absent (correct: no disc)
//     [ret] sub_82831528   -> 2    ERROR_FILE_NOT_FOUND on \Device\Harddisk0\partition0
//     [ret] sub_82829098   -> 0    ... which makes the caller run the cache block
//
// Every one of those readings was accurate, and the chain they described was real.
// It was also entirely beside the point. The branch that actually differed was one
// level further up and in a different subsystem: a stubbed XamContentGetLicenseMask,
// whose failure status sent the title looking for a disc in the first place. The
// device-not-found results were downstream symptoms faithfully reported.
//
// TWO LESSONS, and the second is the expensive one:
//   1. When a predicate's polarity is not obvious on one reading, print it. That
//      costs less than reading it three times and, unlike the reading, it cannot be
//      wrong.
//   2. A probe answers the question you point it at. Pointing it at the deepest
//      frame you can see confirms the symptom in detail and says nothing about the
//      cause — the useful move was walking OUTWARD to the first caller whose
//      behaviour differs from the capture, which here was five frames up.
extern "C" PPC_FUNC(__imp__sub_82829098);
extern "C" PPC_FUNC(__imp__sub_82823A58);
extern "C" PPC_FUNC(__imp__sub_82831528);

namespace {

// Print a guest function's return value the first few times it is called. `note`
// says what a zero MEANS at that site, because "returns 0" is not a finding on its
// own — the branch that consumes it is.
void ShowRet(const char* who, const char* note, PPCContext& ctx, std::atomic<int>& seen)
{
    if (seen.fetch_add(1, std::memory_order_relaxed) < 4)
        fprintf(stderr, "[ret] %-14s -> %u (0x%X)   %s\n", who, ctx.r3.u32, ctx.r3.u32, note);
}

} // namespace

#define CZ_SHOW_RET(name, note)                                                      \
    PPC_FUNC(name)                                                                   \
    {                                                                                \
        static std::atomic<int> seen{ 0 };                                           \
        __imp__##name(ctx, base);                                                    \
        if (ProbeEnabled()) ShowRet(#name, note, ctx, seen);                          \
    }

CZ_SHOW_RET(sub_82829098, "0 here makes sub_82788F48 run the cache block")
CZ_SHOW_RET(sub_82823A58, "1 = '\\Device\\Image' is absent (not a disc)")
CZ_SHOW_RET(sub_82831528, "nonzero makes sub_82829098 return 0")

// ---------------------------------------------------------------------------------
// sub_8284B568 — the graphics driver's command-stream interpreter, and the null
// indirect call of task #11.
//
// WHAT THIS FUNCTION IS. It walks a token stream out of one of four buffers the job
// object carries at +0x5C..+0x68, keeping its state in a SHARED object at [job+0],
// which the guest reaches through r31:
//
//     [obj+0x00]  a lwarx/stwcx spin lock
//     [obj+0x10]  the callback              <- set by a 0x8C000000 token
//     [obj+0x14]  its user data                (from the same token)
//     [obj+0x18]  iteration index           }  set by a "run" token, whose low 16
//     [obj+0x1C]  iteration limit           }  bits are a repeat count and whose
//     [obj+0x20]  base pointer              }  high 15 are an offset to the next
//     [obj+0x24]  stream cursor             }  token
//
// The dispatch is `lwz r11,0x10(r31); mtctr r11; bctrl` at guest 8284B704, and it is
// reached from the run-token path WITHOUT re-reading the stream — so the callback it
// calls is whatever a PREVIOUS token left in the object. A null there means the
// interpreter executed a run token before any 0x8C000000 token had set one.
//
// WHY A PROBE RATHER THAN MORE READING. The crash reporter names the instruction and
// nothing else useful: by the time it fires, ctx's registers are stale (gotcha 57)
// and the interpreter's own state lives in guest memory the report does not know to
// dump. What decides between "the object was never initialised", "the stream we walked
// is not a stream", and "something clobbered +0x10" is the object's state on entry to
// the call that dies — and the last line this prints before a crash IS that call.
//
// CZ_JOBQ_PROBE=1, separate from CZ_ARG_PROBE because this path runs per frame and
// the two hunts have no reason to share a switch.
extern "C" PPC_FUNC(__imp__sub_8284B568);

namespace {

bool JobProbeEnabled()
{
    static const bool on = getenv("CZ_JOBQ_PROBE") != nullptr;
    return on;
}

void DumpInterpreter(PPCContext& ctx, uint8_t* base)
{
    const uint32_t job = ctx.r3.u32;
    if (!job || uint64_t(job) + 0x6C > PPC_MEMORY_SIZE)
    {
        fprintf(stderr, "[jobq] job pointer %08X is unusable\n", job);
        return;
    }
    const uint32_t obj = PPC_LOAD_U32(job);
    if (!obj || uint64_t(obj) + 0x40 > PPC_MEMORY_SIZE)
    {
        fprintf(stderr, "[jobq] job=%08X -> obj=%08X is unusable\n", job, obj);
        return;
    }

    const uint32_t cb = PPC_LOAD_U32(obj + 0x10);

    // The first few calls establish what healthy looks like; after that only the
    // anomaly is worth a line, because this runs per frame. A cap on the anomaly too:
    // if it starts firing every call, sixteen lines say so as well as a million.
    //
    // BUT THE CAP IS NOT A COUNT (gotcha 109), and reading it as one cost a session:
    // phase C part 2's hand-off recorded "every one of its 4 entries had a null
    // callback" from this probe's four PRINTED lines and reasoned about the worker on
    // that basis. Four was the cap. So the counters are reported separately, on their
    // own cadence, and they are the numbers to quote.
    static std::atomic<int> calls{ 0 };
    static std::atomic<int> nulls{ 0 };
    static std::atomic<int> empties{ 0 };
    const int n = calls.fetch_add(1);
    const bool anomaly = cb == 0;
    if (anomaly)
        nulls.fetch_add(1);
    if (PPC_LOAD_U32(job + 0x54) == PPC_LOAD_U32(job + 0x58))
        empties.fetch_add(1);
    if (((n + 1) % 2048) == 0)
        fprintf(stderr, "[jobq] TOTALS entries=%d nullCallback=%d queueEmptyAtEntry=%d\n",
                n + 1, nulls.load(), empties.load());
    if (n >= 4 && !(anomaly && nulls.load() < 16))
        return;

    fprintf(stderr,
            "[jobq] call #%d job=%08X obj=%08X  cb=%08X%s user=%08X idx=%u lim=%u "
            "base=%08X cursor=%08X depth=%u\n",
            n, job, obj, cb, anomaly ? " *** NULL — this call will fault ***" : "",
            PPC_LOAD_U32(obj + 0x14), PPC_LOAD_U32(obj + 0x18), PPC_LOAD_U32(obj + 0x1C),
            PPC_LOAD_U32(obj + 0x20), PPC_LOAD_U32(obj + 0x24), PPC_LOAD_U32(obj + 0x3C));

    // The stream the interpreter is about to walk: the buffer ring is four entries at
    // job+0x5C selected by [job+0x58] & 3, and the guest starts reading at buffer+4.
    const uint32_t tail = PPC_LOAD_U32(job + 0x58);
    const uint32_t slot = job + 0x5C + (tail & 3) * 4;
    const uint32_t buf = PPC_LOAD_U32(slot);
    fprintf(stderr, "[jobq]   head=%u tail=%u buffer[%u]=%08X:", PPC_LOAD_U32(job + 0x54),
            tail, tail & 3, buf);
    if (buf && uint64_t(buf) + 64 <= PPC_MEMORY_SIZE)
        for (int i = 0; i < 12; i++)
            fprintf(stderr, " %08X", PPC_LOAD_U32(buf + 4 * i));
    else
        fprintf(stderr, " <unusable>");
    fprintf(stderr, "\n");
}

} // namespace

extern "C" void CzMaybeCrashTest(PPCContext& ctx, uint8_t* base);

PPC_FUNC(sub_8284B568)
{
    g_chainWalks.fetch_add(1, std::memory_order_relaxed);
    if (JobProbeEnabled())
        DumpInterpreter(ctx, base);
    CzMaybeCrashTest(ctx, base);
    __imp__sub_8284B568(ctx, base);
}

// A deliberate fault, to prove the crash reporter's null-indirect-call branch.
//
// Task #11's crash was a `bctrl` with ctr == 0, and the reporter's "LIKELY null
// indirect call" test did NOT fire on it: the test required si_addr == nullptr AND
// ctr inside the image, and neither holds when ctr is zero. Widening it to catch
// ctr == 0 is a one-line change and would be worth nothing unproven — gotcha 30, a
// check that has never failed has not been shown capable of failing, and a
// diagnostic that stays silent on the case it was written for is worse than none.
//
// CZ_CRASH_TEST=nullcall makes the next call to the interpreter hook do exactly what
// the guest did: set ctr to zero and call through it. Expect the report to name
// "ctr is ZERO". Off by default, and it announces itself before faulting so a run
// that has it on can never be mistaken for a real crash.
extern "C" void CzMaybeCrashTest(PPCContext& ctx, uint8_t* base)
{
    // A static bool, not a getenv/strcmp per call: this hook is on the per-frame
    // interpreter path, and gotcha 7's rule is that an instrument must be free when
    // it is off, not merely cheap.
    static const bool enabled = [] {
        const char* m = getenv("CZ_CRASH_TEST");
        return m && strcmp(m, "nullcall") == 0;
    }();
    if (!enabled)
        return;
    fprintf(stderr, "[crashtest] CZ_CRASH_TEST=nullcall — deliberately calling through "
                    "a null ctr to prove the crash reporter names it. This crash is "
                    "ON PURPOSE.\n");
    fflush(stderr);
    ctx.ctr.u64 = 0;
    PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// ---------------------------------------------------------------------------
// The audio work queue — A1 gate position 93
// ---------------------------------------------------------------------------
//
// Position 93 is `KeQueryBasePriorityThread`, and it has exactly one call site in the
// image: `sub_825DBA20`, which is the XAPI `GetThreadPriority`. Its only game-side
// caller is `sub_828576D8`, a work-queue drain:
//
//     r28 = this + 0x3EA0                     ; the queue
//     if (!pop(r28, &item)) return            ; NOTHING TO DO — the common case
//     old = GetThreadPriority(-2)             ; <-- position 93 lives here
//     SetThreadPriority(-2, 15)               ; boost while draining
//     ... dispatch the item through two vtables ...
//     while (pop(r28, &item)) ...
//     SetThreadPriority(-2, old)
//
// So position 93 is not a missing import — `KeQueryBasePriorityThread` has been
// implemented since phase 1. It is reached only when this queue is NON-EMPTY, and A1
// reaches it exactly ONCE in a whole boot, on the audio thread (Xenia's F800010C, the
// one that does the XMA `MmMapIoSpace`), at log line 122,563 — long after the title
// screen.
//
// This probe answers the only question worth asking before writing any code: does our
// run enter the drain at all, and does its queue ever hold anything? "Never entered"
// and "entered thousands of times with an empty queue" are completely different
// problems, and no amount of reading tells them apart.
//
// It reports the FIRST entry and the first non-empty drain, then goes quiet, because
// this sits on a worker loop and gotcha 7 applies.
extern "C" PPC_FUNC(__imp__sub_828576D8);

static bool QueueProbeEnabled()
{
    static const bool on = getenv("CZ_QUEUE_PROBE") != nullptr;
    return on;
}

PPC_FUNC(sub_828576D8)
{
    if (!QueueProbeEnabled())
    {
        __imp__sub_828576D8(ctx, base);
        return;
    }

    static std::atomic<uint64_t> entries{ 0 };
    const uint64_t n = entries.fetch_add(1);
    const uint32_t self = ctx.r3.u32;
    if (n == 0)
        fprintf(stderr,
                "[queue] sub_828576D8 first entry: this=%08X queue=%08X (guest thread "
                "%08X)\n",
                self, self + 0x3EA0, GuestThread::GetCurrentThreadId());

    __imp__sub_828576D8(ctx, base);

    // Whether it did any work is not visible in the return value, so infer it the
    // same way the function does: the priority-boost path is the work path, and
    // KeQueryBasePriorityThread is on it. Counting entries against that import's own
    // call count in the log is what separates "never entered" from "always empty".
    if ((n & 0xFFFFu) == 0)
        fprintf(stderr, "[queue] sub_828576D8 entered %llu times\n",
                (unsigned long long)(n + 1));
}

// The seven callers of the drain, so that "the drain never runs" can be turned into
// "and neither does anything that would have called it" — or into the much more
// interesting "a caller runs and the queue is always empty". Those are different
// problems and no amount of reading separates them.
extern "C" PPC_FUNC(__imp__sub_828587B0);
extern "C" PPC_FUNC(__imp__sub_828589D0);
extern "C" PPC_FUNC(__imp__sub_828595F8);
extern "C" PPC_FUNC(__imp__sub_82859888);
extern "C" PPC_FUNC(__imp__sub_82874BD0);
extern "C" PPC_FUNC(__imp__sub_82875588);
extern "C" PPC_FUNC(__imp__sub_82876080);

PPC_FUNC(sub_828587B0)
{
    if (QueueProbeEnabled())
    {
        static std::atomic<bool> seen{ false };
        if (!seen.exchange(true))
            fprintf(stderr, "[queue] caller sub_828587B0 entered (guest thread %08X)\n",
                    GuestThread::GetCurrentThreadId());
    }
    __imp__sub_828587B0(ctx, base);
}

PPC_FUNC(sub_828589D0)
{
    if (QueueProbeEnabled())
    {
        static std::atomic<bool> seen{ false };
        if (!seen.exchange(true))
            fprintf(stderr, "[queue] caller sub_828589D0 entered (guest thread %08X)\n",
                    GuestThread::GetCurrentThreadId());
    }
    __imp__sub_828589D0(ctx, base);
}

PPC_FUNC(sub_828595F8)
{
    if (QueueProbeEnabled())
    {
        static std::atomic<bool> seen{ false };
        if (!seen.exchange(true))
            fprintf(stderr, "[queue] caller sub_828595F8 entered (guest thread %08X)\n",
                    GuestThread::GetCurrentThreadId());
    }
    __imp__sub_828595F8(ctx, base);
}

PPC_FUNC(sub_82859888)
{
    if (QueueProbeEnabled())
    {
        static std::atomic<bool> seen{ false };
        if (!seen.exchange(true))
            fprintf(stderr, "[queue] caller sub_82859888 entered (guest thread %08X)\n",
                    GuestThread::GetCurrentThreadId());
    }
    __imp__sub_82859888(ctx, base);
}

PPC_FUNC(sub_82874BD0)
{
    if (QueueProbeEnabled())
    {
        static std::atomic<bool> seen{ false };
        if (!seen.exchange(true))
            fprintf(stderr, "[queue] caller sub_82874BD0 entered (guest thread %08X)\n",
                    GuestThread::GetCurrentThreadId());
    }
    __imp__sub_82874BD0(ctx, base);
}

PPC_FUNC(sub_82875588)
{
    if (QueueProbeEnabled())
    {
        static std::atomic<bool> seen{ false };
        if (!seen.exchange(true))
            fprintf(stderr, "[queue] caller sub_82875588 entered (guest thread %08X)\n",
                    GuestThread::GetCurrentThreadId());
    }
    __imp__sub_82875588(ctx, base);
}

PPC_FUNC(sub_82876080)
{
    if (QueueProbeEnabled())
    {
        static std::atomic<bool> seen{ false };
        if (!seen.exchange(true))
            fprintf(stderr, "[queue] caller sub_82876080 entered (guest thread %08X)\n",
                    GuestThread::GetCurrentThreadId());
    }
    __imp__sub_82876080(ctx, base);
}

// ---------------------------------------------------------------------------------
// CZ_FENCE_PROBE=1 — the D3D fence/segment plumbing, end to end.
//
// WHY: phase C's boot parks in the engine's per-frame GPU sync with
// `emitted - completed` pinned at 16, and every static reading of the path was
// consistent with several different causes. The three functions below are the whole
// producer side of that number and there is nowhere else it can come from:
//
//   sub_828459D0  the fence-block emitter: writes an INVALIDATE_STATE + two
//                 EVENT_WRITE_SHD packets at a caller-supplied cursor, advances
//                 dev+0x2A9C by 2, and has a CPU FAST PATH that writes the
//                 completion word itself when dev+0x54EC == 0 and dev+0x2ABD bit 1
//                 is set. Whether that path is taken decides who retires the fence.
//   sub_82845AC0  the segment submit, and it is a FORK: dev+0x2B04 != 0 queues the
//                 segment as a token for the D3D worker, == 0 submits it to the ring
//                 directly. A queued segment nobody drains is an unretired fence.
//   sub_82845DE0  the close/kick, which decides between those on flags this probe
//                 prints (dev+0x2ABC, dev+0x2ABD, dev+0x3460) and can bail out
//                 entirely when the segment measures zero dwords.
//
// It prints one line per call, capped, with the guest thread id — the cap exists
// because a stalled boot emits thousands and only the tail is interesting.
extern "C" PPC_FUNC(__imp__sub_828459D0);
extern "C" PPC_FUNC(__imp__sub_82845AC0);
extern "C" PPC_FUNC(__imp__sub_82845DE0);

namespace {

bool FenceProbeEnabled()
{
    static const bool on = getenv("CZ_FENCE_PROBE") != nullptr;
    return on;
}

// Shared across the three hooks so their lines interleave in call order and the cap
// is a budget for the whole picture, not per function.
std::atomic<uint64_t> g_fenceLines{ 0 };
// CZ_FENCE_PROBE=1 keeps the 40,000-line default; CZ_FENCE_PROBE=<N> sets the budget.
// It is adjustable because a saturated budget is a FLOOR, not a count (gotcha 109),
// and a stall this probe exists to explain is at the END of a boot, not the start.
uint64_t FenceBudget()
{
    static const uint64_t n = [] {
        const char* v = getenv("CZ_FENCE_PROBE");
        const long l = v ? strtol(v, nullptr, 10) : 0;
        return l > 1 ? uint64_t(l) : uint64_t(40000);
    }();
    return n;
}
bool FenceLine()
{
    return g_fenceLines.fetch_add(1, std::memory_order_relaxed) < FenceBudget();
}

// " SCRATCH" when a command-buffer cursor points into phase C's private redirect
// buffer, "" when it points at the real ring-fed command buffer. On the PM4 control
// arm it is always "" — which is what makes the two arms' logs directly diffable.
const char* WhereCursor(uint32_t cursor)
{
    uint32_t va = 0, bytes = 0;
    D3dDraw_ScratchRange(va, bytes);
    return (va && cursor >= va && cursor < va + bytes) ? " SCRATCH" : "";
}

} // namespace

// The budget, shared with gpu/d3d_hooks.cpp's sub_8284B9C0 hook (cpu/fence_probe.h).
bool FenceProbe_Line() { return FenceProbeEnabled() && FenceLine(); }

PPC_FUNC(sub_828459D0)
{
    if (FenceProbeEnabled() && FenceLine())
    {
        const uint32_t dev = ctx.r3.u32;
        const uint32_t wb = PPC_LOAD_U32(dev + 0x2A90);
        fprintf(stderr, "[fence] emit  t=%08X cursor=%08X fence=%u wb=%08X completed=%u "
                        "54EC=%08X 2ABD=%02X cpuPath=%d\n",
                GuestThread::GetCurrentThreadId(), ctx.r4.u32,
                PPC_LOAD_U32(dev + 0x2A9C), wb, wb ? PPC_LOAD_U32(wb) : 0,
                PPC_LOAD_U32(dev + 0x54EC), PPC_LOAD_U8(dev + 0x2ABD),
                (PPC_LOAD_U32(dev + 0x54EC) == 0 && (PPC_LOAD_U8(dev + 0x2ABD) & 2)) ? 1 : 0);
    }
    __imp__sub_828459D0(ctx, base);
}

PPC_FUNC(sub_82845AC0)
{
    g_chainSegSubmit.fetch_add(1, std::memory_order_relaxed);
    if (PPC_LOAD_U32(ctx.r3.u32 + 0x2B04))
        g_chainSegQueued.fetch_add(1, std::memory_order_relaxed);
    // r7 (`incr`) and r8 (`queue`) are the two arguments that decide whether this
    // submission can become a replay, and neither was printed before phase C part 4.
    //
    // sub_8284B9C0 submits its arm-carrying segment with incr=1 and queue=dev+0x3518,
    // and its fence segment with incr=0 and queue=dev+0x3500 — two DIFFERENT token
    // streams — while the callback it arms (sub_8284AAD0) is handed the head of the
    // 0x3500 stream. So on a healthy frame the arm block is either submitted straight
    // to the ring (counter == 0) or parked in a stream the wake-up does not name. It
    // is only when the counter is ALREADY nonzero at the incr=1 submit that the arm
    // lands in a token stream at all, and from there the loop closes: the ISR pushes
    // that stream, the worker resubmits the arm, the CP raises the interrupt again.
    // Printing the fork's inputs is what makes "which of those happened" readable
    // instead of inferred (gotcha 145 — a claim about a value needs the value).
    if (FenceProbeEnabled() && FenceLine())
        fprintf(stderr, "[fence] submit t=%08X addr=%08X dwords=%u incr=%u queue=%08X "
                        "tok=%08X%s 2B04=%d -> %s\n",
                GuestThread::GetCurrentThreadId(), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                ctx.r8.u32, ctx.r4.u32, WhereCursor(ctx.r4.u32),
                int32_t(PPC_LOAD_U32(ctx.r3.u32 + 0x2B04)),
                PPC_LOAD_U32(ctx.r3.u32 + 0x2B04) ? "WORKER TOKEN QUEUE" : "ring direct");
    __imp__sub_82845AC0(ctx, base);
}

// sub_8284AAD0 — the worker KICK, i.e. the only function in the image that pushes a
// token-buffer pointer onto the per-CPU D3D worker's job ring and signals its event.
//
// It is the ISR's callback, so on the producer side it appears only as a constant
// passed to sub_82845BA0. Hooking it directly gives the count the mem-watch could only
// infer, and — more usefully — the VALUE pushed: sub_8284B568 pops that pointer and,
// when the interpreter's nesting depth is 1 and [obj+0x48] is clear, walks it from
// `pointer + 4`. Two consecutive kicks carrying the SAME pointer are therefore the
// same token stream walked twice, which is the replay stated in one line.
extern "C" PPC_FUNC(__imp__sub_8284AAD0);

PPC_FUNC(sub_8284AAD0)
{
    // Always-on: this is the link whose ratio to `arms` is the ~300x part 7 exists to
    // explain, and it must not be a by-product of a line-budgeted print (gotcha 109).
    static std::atomic<uint32_t> lastKick{ 0 };
    const uint64_t k = g_chainKicks.fetch_add(1, std::memory_order_relaxed);
    const uint32_t arg = ctx.r3.u32;
    const uint32_t prev = lastKick.exchange(arg, std::memory_order_relaxed);
    if (k && arg == prev)
        g_chainKickRepeat.fetch_add(1, std::memory_order_relaxed);
    NoteKickBuffer(arg);

    if (FenceProbeEnabled())
    {
        // Every kick while the count is small (the era the seed lives in), then only
        // the periodic total — a runaway prints millions and the budget is shared.
        if ((k < 64 || (k % 10000) == 0) && FenceLine())
            fprintf(stderr, "[fence] kick  t=%08X #%llu buf=%08X%s%s\n",
                    GuestThread::GetCurrentThreadId(), (unsigned long long)k, arg,
                    WhereCursor(arg), (k && arg == prev) ? " SAME-AS-PREVIOUS" : "");
    }
    __imp__sub_8284AAD0(ctx, base);
}

PPC_FUNC(sub_82845DE0)
{
    if (FenceProbeEnabled() && FenceLine())
    {
        const uint32_t dev = ctx.r3.u32;
        const uint32_t cursor = PPC_LOAD_U32(dev + 0x30);
        const uint32_t segStart = PPC_LOAD_U32(dev + 0x3B20);
        // The cursor label is the important field. sub_82845DE0 measures the segment
        // it is about to hand to the ring as [dev+0x3B20, [dev+0x30]+4) — so if it
        // runs on ANY thread while a redirect has the private scratch installed in
        // dev+0x30, the segment it publishes spans from the real command buffer to
        // our scratch, and the command processor then executes our already-walked
        // content as if it were a command stream.
        fprintf(stderr, "[fence] close t=%08X dev=%08X cursor=%08X%s seg=%08X%s dwords=%d "
                        "2ABC=%02X 2ABD=%02X 3460=%08X 2B04=%08X\n",
                GuestThread::GetCurrentThreadId(), dev, cursor, WhereCursor(cursor),
                segStart, WhereCursor(segStart),
                int(int32_t(cursor + 4 - segStart) >> 2), PPC_LOAD_U8(dev + 0x2ABC),
                PPC_LOAD_U8(dev + 0x2ABD), PPC_LOAD_U32(dev + 0x3460),
                PPC_LOAD_U32(dev + 0x2B04));
    }
    __imp__sub_82845DE0(ctx, base);
}

// The graphics ISR itself, under the same CZ_FENCE_PROBE budget. sub_82844D38's
// source-1 path reads its callback and argument out of GUEST MEMORY at
// [[user+0x2A94]] + 0x10/+0x14 — the scratch-register mirror the command stream arms
// just before each INTERRUPT packet — so this line says exactly which kick the title
// asked for and whether it was armed at the moment the interrupt arrived. It is the
// one place the PM4 control arm and the phase C draw arm can be compared directly:
// phase C's walker replicates this path itself for content-stream interrupts, so a
// callback that appears here on the control arm and nowhere on the draw arm is a kick
// the redirect ate.
extern "C" PPC_FUNC(__imp__sub_82844D38);

PPC_FUNC(sub_82844D38)
{
    // Source 0 is the vblank tick: it carries no callback, it arrives ~60 times a
    // second on every arm, and printing it made 14,340 of one run's 16,245 ISR lines
    // — a budget spent on the one interrupt that cannot be the answer, and enough
    // fprintf to move where the boot got to in a fixed wall time. Counted, not
    // printed; the count still says the pump is alive.
    static std::atomic<uint64_t> vblanks{ 0 };
    if (ctx.r3.u32 == 0)
    {
        const uint64_t n = vblanks.fetch_add(1) + 1;
        if (FenceProbeEnabled() && (n % 4096) == 0 && FenceLine())
            fprintf(stderr, "[fence] isr   (source 0 / vblank) x%llu so far\n",
                    (unsigned long long)n);
        __imp__sub_82844D38(ctx, base);
        return;
    }
    if (FenceProbeEnabled() && FenceLine())
    {
        const uint32_t mirror = ctx.r3.u32 == 1 ? PPC_LOAD_U32(ctx.r4.u32 + 0x2A94) : 0;
        fprintf(stderr, "[fence] isr   t=%08X source=%u mirror=%08X cb=%08X arg=%08X\n",
                GuestThread::GetCurrentThreadId(), ctx.r3.u32, mirror,
                mirror >= 0x1000 ? PPC_LOAD_U32(mirror + 0x10) : 0,
                mirror >= 0x1000 ? PPC_LOAD_U32(mirror + 0x14) : 0);
    }
    __imp__sub_82844D38(ctx, base);
}

// sub_82845BA0 is the callback-arming emitter, and it is the producer side of the ISR
// line above: it writes a type-0 packet setting scratch registers 0x057C/0x057D to
// (callback, argument), three WAIT_REG_MEMs that hold the GPU until that mirror has
// landed in memory, and then the INTERRUPT packet. Printing its CURSOR says which
// stream each arming went into — under phase C's redirect, a cursor inside the private
// scratch means our walker owns that kick and a cursor in the real command buffer
// means the CP does.
extern "C" PPC_FUNC(__imp__sub_82845BA0);

PPC_FUNC(sub_82845BA0)
{
    g_chainArms.fetch_add(1, std::memory_order_relaxed);
    if (FenceProbeEnabled() && FenceLine())
        fprintf(stderr, "[fence] arm   t=%08X cursor=%08X%s flags=%08X cb=%08X arg=%08X\n",
                GuestThread::GetCurrentThreadId(), ctx.r4.u32, WhereCursor(ctx.r4.u32),
                ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    __imp__sub_82845BA0(ctx, base);
}

// ---------------------------------------------------------------------------------
// The CONSUMER half of the same protocol, under the same flag — because the producer
// half alone cannot answer phase C part 3's question.
//
// dev+0x2B04 is the count of outstanding async command segments, and sub_82846210
// SPINS on it reaching zero. Session 14 left it pinned nonzero on the draw arm with
// the engine thread at 99% CPU, and the producer probe could only show that segments
// were being submitted. What decides the count is on the other side:
//
//   the object       obj = dev + 0x2AC4. Proven, not assumed: sub_82845AC0 locks
//                    dev+0x2B08 and sub_8284A960 locks obj+0x44, so obj+0x44 IS
//                    dev+0x2B08. Hence obj+0x3C = dev+0x2B00 (the interpreter's
//                    nesting depth) and obj+0x40 = dev+0x2B04 (THE COUNTER).
//   sub_8284B568     the D3D worker's token interpreter. Pops one buffer off the
//                    per-CPU ring (job+0x5C + (job+0x58 & 3)*4) and does
//                    ++[obj+0x3C] before walking it.
//   sub_8284A960     the 0xC0000000 end-of-stream sentinel handler. Does
//                    --[obj+0x3C], and ONLY when that reaches zero (and [obj+0x48]
//                    is clear) does it do --[obj+0x40]. So the counter drains once
//                    per fully-walked stream, not once per segment.
//   sub_8284B9C0     the frame-end async submit, and the ONLY site in the image that
//                    arms sub_8284AAD0 (the worker kick — the 0x8284AAD0 constant is
//                    built at 8284BAC4/8284BACC and passed to sub_82845BA0). It
//                    allocates three command-buffer blocks through sub_82845078 and
//                    then hands their addresses to sub_82845AC0 as segments, the
//                    middle one with r7=1 — the only +1 the counter ever gets.
//   sub_82846210     the spin itself.
//
// The cursor labels are the point: sub_8284B9C0 is reached from Resolve, which phase C
// REDIRECTS, so every block it allocates can come out of the private scratch — and a
// segment address inside the scratch is one the redirect has already consumed and the
// worker will walk anyway. Printing SCRATCH vs ring next to each cursor is what makes
// that visible instead of inferrable.
// NB sub_8284B9C0 is NOT hooked here. gpu/d3d_hooks.cpp services it (it has to run on
// the real ring under a redirect), and it prints this probe's `fsubmit` line from
// there, through FenceProbe_Line() in cpu/fence_probe.h.
extern "C" PPC_FUNC(__imp__sub_8284A960);
extern "C" PPC_FUNC(__imp__sub_82846210);

PPC_FUNC(sub_8284A960)
{
    g_chainDrains.fetch_add(1, std::memory_order_relaxed);
    if (FenceProbeEnabled() && FenceLine())
    {
        const uint32_t obj = ctx.r3.u32;
        fprintf(stderr, "[fence] drain t=%08X obj=%08X depth=%u counter=%u 48=%08X "
                        "token=%08X\n",
                GuestThread::GetCurrentThreadId(), obj, PPC_LOAD_U32(obj + 0x3C),
                PPC_LOAD_U32(obj + 0x40), PPC_LOAD_U32(obj + 0x48),
                ctx.r5.u32 >= 0x1000 ? PPC_LOAD_U32(ctx.r5.u32) : 0);
    }
    __imp__sub_8284A960(ctx, base);
}

// sub_828455C0 — the RING submitter, and the only thing that puts INDIRECT_BUFFER
// packets in front of our command processor on the phase C arm.
//
// It takes an array of `count` 8-byte {tokenWord, sizeDwords} entries and writes one
// 3-dword indirect-buffer packet per entry into the ring, calling sub_82844AB0 first
// to wait for ring space. Its two callers are the reserve/close path and, on the D3D
// worker, the token sub-dispatcher sub_8284B228.
//
// It is instrumented because of what a ring trace showed on the draw arm: through the
// boot movie the ring carries ~390 packets and ~48 draws a frame, and then from around
// frame 384 it goes to 1.25 MILLION packets and 135,000 draws per second with the
// XE_SWAP count frozen — i.e. the command processor is faithfully executing a stream
// that never ends, while every guest thread is parked. `truncated=0` and the IB verify
// stays clean throughout, so this is not a parser fault (gotcha 88 again): the BYTES
// are wrong, and this is the function that chooses them.
extern "C" PPC_FUNC(__imp__sub_828455C0);

PPC_FUNC(sub_828455C0)
{
    const uint64_t k = g_chainRingsub.fetch_add(1, std::memory_order_relaxed);
    const uint64_t e =
        g_chainRingsubEnts.fetch_add(ctx.r5.u32, std::memory_order_relaxed) + ctx.r5.u32;
    if (FenceProbeEnabled())
    {
        // Every entry of the first CZ_FENCE_RINGSUB calls (default 4000), then a
        // periodic total — because the interesting claim is a RATE (entries per
        // second), and a capped list of lines cannot carry one (gotcha 109).
        //
        // The per-entry form exists for one question phase C part 4 could not answer
        // any other way: WHICH segment addresses does the ring see more than once?
        // The replay is 106 M submissions of the same three segments, so the entry
        // address is the identity of the thing being replayed, and a submission list
        // is the only place it is stated. Entries are {0x8100_0000|dwords, gpuAddr}
        // pairs — the same 8-byte record sub_82845DE0 appends to dev+0x350C and
        // sub_82845AC0 builds on its stack.
        static const uint64_t verbose = [] {
            const char* v = getenv("CZ_FENCE_RINGSUB");
            const long l = v ? strtol(v, nullptr, 10) : 0;
            return l > 0 ? uint64_t(l) : uint64_t(4000);
        }();
        if (k < verbose || (k % 20000) == 0)
        {
            const uint32_t arr = ctx.r4.u32;
            const uint32_t n = ctx.r5.u32 > 8 ? 8 : ctx.r5.u32;
            char ents[512];
            int off = 0;
            for (uint32_t i = 0; i < n && arr >= 0x1000; i++)
                off += snprintf(ents + off, sizeof ents - off, " %08X/%u",
                                PPC_LOAD_U32(arr + i * 8 + 4), PPC_LOAD_U32(arr + i * 8) & 0xFFFFFF);
            ents[off] = 0;
            fprintf(stderr, "[fence] ringsub t=%08X #%llu count=%u totalEntries=%llu "
                            "arr=%08X%s ents:%s\n",
                    GuestThread::GetCurrentThreadId(), (unsigned long long)k, ctx.r5.u32,
                    (unsigned long long)e, arr, WhereCursor(arr), ents);
        }
    }
    __imp__sub_828455C0(ctx, base);
}

PPC_FUNC(sub_82846210)
{
    const bool probe = FenceProbeEnabled();
    const uint32_t dev = ctx.r3.u32;
    if (probe && FenceLine())
        fprintf(stderr, "[fence] spin- t=%08X dev=%08X counter=%u fence=%u\n",
                GuestThread::GetCurrentThreadId(), dev, PPC_LOAD_U32(dev + 0x2B04),
                PPC_LOAD_U32(dev + 0x2A9C));
    __imp__sub_82846210(ctx, base);
    if (probe && FenceLine())
        fprintf(stderr, "[fence] spin+ t=%08X dev=%08X RETURNED counter=%u\n",
                GuestThread::GetCurrentThreadId(), dev, PPC_LOAD_U32(dev + 0x2B04));
}

// ---------------------------------------------------------------------------------
// THE BIN-MASK PATCH PASS — CZ_BINMASK_PROBE=1
//
// Phase C part 10. Capture B1 says hardware discards 0.3% of this title's draw
// packets to bin predication and we discard 33%, and the pair census
// (CZ_PM4_BIN_CENSUS) localised that to the mask VALUE standing at the right tile's
// draws: hardware 8000000F, ours 80000000.
//
// 80000000 is not a computed value. It is the PLACEHOLDER: the draw emitters write
// `SET_BIN_MASK_LO 0x80000000` into the command buffer as a literal (82842A18,
// 82842DE0, 8284322C are the three sites), and the real mask is patched in later, in
// place, by the D3D worker:
//
//   sub_8284B568 (token interpreter) -> sub_8284B228 -> sub_8284A900 -> sub_8284A7F8
//
// sub_8284A7F8 walks a list of 16-byte {record*, x0, x1-1, y0, y1-1} entries — the
// rect is in units of 8 pixels, hence the <<3 — intersects each against the tile
// rects at tileInfo+8, accumulates `(acc << 2) | 3` per overlapping tile, ORs in bit
// 31, and stores the result at record+8, which is the dword the emitter left as
// 0x80000000.
//
// So a draw carrying 80000000 is a draw whose mask was NEVER PATCHED — the command
// processor reached it before (or without) the worker's fix-up pass. That is an
// ordering/liveness question about our worker, not a bin-mask question, and the two
// numbers that separate them are: how often this pass runs, and what it computes.
// If it computes 8000000F and the stream still shows 80000000, we execute too early;
// if it barely runs, the worker is not getting there.
extern "C" PPC_FUNC(__imp__sub_8284A7F8);

namespace {

bool BinMaskProbeEnabled()
{
    static const bool on = getenv("CZ_BINMASK_PROBE") != nullptr;
    return on;
}

// Report on a CLOCK, not on a call count.
//
// Part 10 read "the dispatcher ran 1 time" and "the other mask setter ran 1 time, with
// mask 0" off probes that printed at call #1 and then every 20,000/200,000. A subsystem
// that runs 900 times a boot therefore prints exactly one line — its FIRST — and that
// line's counts are all 1 by construction. Both numbers were quoted as totals. This is
// gotcha 109 (a capped emitter's output is not a count) arriving in our own probes for
// the third time, so the schedule is now wall-clock: every REPORT_SECONDS, whatever the
// call rate, plus one final-ish line often enough that a 170 s boot always produces
// several. A time-based report also costs nothing on the hot path — one relaxed load
// and a compare — because the clock is only read when the deadline has passed.
constexpr double kBinReportSeconds = 15.0;

bool BinReportDue(std::atomic<uint64_t>& nextNs)
{
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    uint64_t due = nextNs.load(std::memory_order_relaxed);
    if (now < due)
        return false;
    // Whoever wins the exchange prints; the losers skip. No lock, and no risk of two
    // threads interleaving a multi-line report is claimed — the reports below are one
    // fprintf each for exactly that reason.
    return nextNs.compare_exchange_strong(
        due, now + uint64_t(kBinReportSeconds * 1e9), std::memory_order_relaxed);
}

std::atomic<uint64_t> g_binPatchCalls{ 0 };
std::atomic<uint64_t> g_binPatchRecords{ 0 };
// Histogram of the masks this pass actually writes, keyed on the low nibble plus the
// all-ones case. Small and fixed: the interesting values are 0 (no tile), 3 (tile 0),
// C (tile 1) and F (both), and anything else is worth seeing as "other".
std::atomic<uint64_t> g_binPatchValue[17];

// The (rect -> mask) census. Bounded and lock-guarded; the pass runs ~1,750 times a
// boot over ~390,000 records, which is far too rare to matter on any hot path.
struct RectRow { uint16_t x0, x1, y0, y1; uint32_t mask; uint64_t count; bool used; };
constexpr size_t kRectRows = 24;
RectRow g_rectRows[kRectRows];
std::mutex g_rectMutex;
std::atomic<uint64_t> g_rectOverflow{ 0 };

void RectCensusNote(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1, uint32_t mask)
{
    std::lock_guard<std::mutex> lock(g_rectMutex);
    for (size_t i = 0; i < kRectRows; i++)
    {
        if (!g_rectRows[i].used) { g_rectRows[i] = { x0, x1, y0, y1, mask, 1, true }; return; }
        if (g_rectRows[i].x0 == x0 && g_rectRows[i].x1 == x1 && g_rectRows[i].y0 == y0 &&
            g_rectRows[i].y1 == y1 && g_rectRows[i].mask == mask)
        { g_rectRows[i].count++; return; }
    }
    g_rectOverflow.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

PPC_FUNC(sub_8284A7F8)
{
    if (!BinMaskProbeEnabled())
    {
        __imp__sub_8284A7F8(ctx, base);
        return;
    }

    const uint32_t tileInfo = ctx.r3.u32;
    const uint32_t list = ctx.r4.u32;
    __imp__sub_8284A7F8(ctx, base);

    // Read the masks BACK out of the records the pass just patched, rather than
    // reimplementing its arithmetic here: a probe that recomputes what it is
    // measuring can only ever agree with itself.
    if (list >= 0x1000)
    {
        const uint32_t end = PPC_LOAD_U32(list);
        for (uint32_t p = list + 4; p + 16 <= end && p < list + 0x10000; p += 16)
        {
            const uint32_t rec = PPC_LOAD_U32(p);
            if (rec < 0x1000)
                continue;
            const uint32_t mask = PPC_LOAD_U32(rec + 8);
            g_binPatchRecords.fetch_add(1, std::memory_order_relaxed);
            const uint32_t lo = mask & 0xF;
            g_binPatchValue[(mask & 0x80000000u) ? lo : 16].fetch_add(
                1, std::memory_order_relaxed);
            // The RECT that produced that mask, as a census. 76% of our records come
            // back "touches no tile" against hardware's ~100% "touches both", so the
            // next question is which of the two inputs is wrong — the draw's rect or
            // the tile rects — and neither is guessable. The rect is in units of 8
            // pixels (the pass's own `<<3`) and stored x0, x1-1, y0, y1-1 as u16, so
            // a whole-screen 1280x720 draw is 0,159,0,89. Keyed on the rect so a
            // handful of distinct values collapses to a handful of rows.
            RectCensusNote(PPC_LOAD_U16(p + 4), PPC_LOAD_U16(p + 6),
                           PPC_LOAD_U16(p + 8), PPC_LOAD_U16(p + 10), mask);
        }
    }

    static std::atomic<uint64_t> nextReport{ 0 };
    const uint64_t n = g_binPatchCalls.fetch_add(1) + 1;
    if (n == 1 || BinReportDue(nextReport))
    {
        fprintf(stderr, "[binmask] patch pass ran %llu times, patched %llu records:",
                (unsigned long long)n,
                (unsigned long long)g_binPatchRecords.load());
        for (uint32_t i = 0; i < 17; i++)
        {
            const uint64_t c = g_binPatchValue[i].load();
            if (!c)
                continue;
            if (i == 16)
                fprintf(stderr, "  bit31-clear=%llu", (unsigned long long)c);
            else
                fprintf(stderr, "  8000000%X=%llu", i, (unsigned long long)c);
        }
        fprintf(stderr, "\n");

        // The TILE rects, the pass's other input. sub_8284A7F8 reads a count at
        // tileInfo+4 and `count` 16-byte rects from tileInfo+8, in PIXELS, laid out
        // {x0, y0, x1, y1} — that ordering is fixed by which of the four dwords each
        // of its four comparisons loads (offsets -4/+0/+4/+8 around tileInfo+8+16i+4).
        // Printed because "the intersection returns empty" has exactly two possible
        // causes and this is the cheap one to rule out.
        if (tileInfo >= 0x1000)
        {
            const uint32_t tiles = PPC_LOAD_U32(tileInfo + 4);
            // `list` is printed because the extent the patch pass reads is written by
            // the GPU, through a PHYSICAL address the guest builds from the record —
            // so a record outside our physical arena means the store is dropped and
            // the pass reads uninitialised memory anyway (StoreGpuRaw says so, but
            // only for the first eight).
            fprintf(stderr, "[binmask] list=%08X tileInfo=%08X [+0]=%08X tiles=%u",
                    list, tileInfo, PPC_LOAD_U32(tileInfo), tiles);
            for (uint32_t t = 0; t < tiles && t < 8; t++)
            {
                const uint32_t r = tileInfo + 8 + 16 * t;
                fprintf(stderr, "  tile%u=%u,%u..%u,%u", t, PPC_LOAD_U32(r),
                        PPC_LOAD_U32(r + 4), PPC_LOAD_U32(r + 8), PPC_LOAD_U32(r + 12));
            }
            fprintf(stderr, "\n");
        }

        std::lock_guard<std::mutex> lock(g_rectMutex);
        fprintf(stderr, "[binmask] rect -> mask (rect in units of 8 px, overflow=%llu):\n",
                (unsigned long long)g_rectOverflow.load());
        for (size_t i = 0; i < kRectRows && g_rectRows[i].used; i++)
            fprintf(stderr, "[binmask]   %u,%u..%u,%u  (px %u,%u..%u,%u)  -> %08X  x%llu\n",
                    g_rectRows[i].x0, g_rectRows[i].y0, g_rectRows[i].x1, g_rectRows[i].y1,
                    g_rectRows[i].x0 * 8u, g_rectRows[i].y0 * 8u,
                    (g_rectRows[i].x1 + 1u) * 8u, (g_rectRows[i].y1 + 1u) * 8u,
                    g_rectRows[i].mask, (unsigned long long)g_rectRows[i].count);
    }
}

// The gate above the patch pass. sub_8284B228 is the worker's token dispatcher for
// this stream; its bin-mask-patch case is
//
//     lwz     r11, 0x164(r30)
//     rlwinm. r11, r11, 0, 0, 0     ; test BIT 31
//     beq     skip                  ; clear -> the patch never runs
//     bl      sub_8284A900          ; set   -> patch every record's mask
//
// and the token is consumed either way, so a clear bit is silent. One token handler
// in the same dispatcher writes 0x7FFFFFFF there (8284B3C4) — bit 31 clear — so the
// question "is the gate ever open" is a question about a VALUE, and gotcha 65 says
// print it rather than read it a third time. r30 is this function's second argument.
//
// PART 11 CORRECTION: `[obj+0x164]` is not a flags word with a gate bit in it. It is
// the CURRENT BIN SELECT — sub_8284A6D0 computes the select for the tile being
// recorded, caches it there, and emits SET_BIN_SELECT_LO with it (see the probe on
// that function below). So the test above reads "is bit 31 of the current select set",
// and sub_8284A668 sets bit 31 exactly when the tile index is 0. The patch pass
// therefore runs once per multi-tile recording, at the FIRST tile — which is why an
// object whose select has never been computed (0x00000000, what we measured) is silent.
extern "C" PPC_FUNC(__imp__sub_8284B228);

PPC_FUNC(sub_8284B228)
{
    if (!BinMaskProbeEnabled())
    {
        __imp__sub_8284B228(ctx, base);
        return;
    }
    static std::atomic<uint64_t> calls{ 0 };
    static std::atomic<uint64_t> gateOpen{ 0 };
    static std::atomic<uint64_t> nextReport{ 0 };
    const uint32_t obj = ctx.r4.u32;
    const uint32_t gate = obj >= 0x1000 ? PPC_LOAD_U32(obj + 0x164) : 0;
    const uint64_t n = calls.fetch_add(1) + 1;
    if (gate & 0x80000000u)
        gateOpen.fetch_add(1, std::memory_order_relaxed);
    if (n == 1 || BinReportDue(nextReport))
        fprintf(stderr, "[binmask] dispatcher #%llu obj=%08X [obj+0x164]=%08X "
                        "bit31=%u  (open on %llu of %llu entries)\n",
                (unsigned long long)n, obj, gate, (gate >> 31) & 1,
                (unsigned long long)gateOpen.load(), (unsigned long long)n);
    __imp__sub_8284B228(ctx, base);
}

// The BIN SELECT producer — the other half of the predication pair, and the thing the
// "gate" above actually reads.
//
//   sub_8284A668(obj):                        ; compute the select
//       r11 = 0x2AAAAAAA if [obj+0x30] bit31   ; three "which half of a bin pair" modes
//           = 0x15555555 if [obj+0x30] bit30
//           = 0xFFFFFFFF otherwise             ; not tiling: select everything
//       if [obj+0x30] bit29:                   ; tiling is ON
//           r11 &= 3 << (2 * [obj+0x34])       ; [obj+0x34] is the TILE INDEX
//           if tile == 0 and not the bit31 mode: r11 |= 0x80000000
//       return r11
//
//   sub_8284A6D0(obj):                        ; publish it, if it changed
//       sel = sub_8284A668(obj)
//       if sel != [obj+0x164]:
//           [obj+0x164] = sel
//           emit SET_BIN_SELECT_LO sel         ; header C0006200, via sub_82844B60
//
// That is where our two observed selects come from and what they mean: `80000003` is
// tile 0 (bins 0-1, plus the bit-31 "first tile" flag) and `0000000C` is tile 1 (bins
// 2-3). It also explains why the LEFT tile keeps every draw on hardware AND on our
// runtime regardless of the mask: every patched mask carries bit 31 too (sub_8284A7F8
// ORs it in unconditionally), so `mask & select` is nonzero for tile 0 by construction.
// Only the right tile's pass ever consults the real bin bits, which is exactly the pass
// that renders nothing here.
//
// The census is keyed on (select, [obj+0x30] mode bits, tile index) because the
// question is not only "what select was published" but "did the tiling flags ever say
// we are tiling at all" — a select of FFFFFFFF means bit 29 was clear, i.e. the guest
// believes it is rendering untiled, which would be a completely different fault from a
// select that is right and a mask that is wrong.
extern "C" PPC_FUNC(__imp__sub_8284A6D0);

PPC_FUNC(sub_8284A6D0)
{
    if (!BinMaskProbeEnabled())
    {
        __imp__sub_8284A6D0(ctx, base);
        return;
    }
    struct Site { uint32_t sel, flags, tile; uint64_t count; bool used; };
    constexpr size_t kSites = 32;
    static Site sites[kSites];
    static std::mutex mutex;
    static std::atomic<uint64_t> calls{ 0 };
    static std::atomic<uint64_t> changed{ 0 };
    static std::atomic<uint64_t> nextReport{ 0 };

    const uint32_t obj = ctx.r3.u32;
    const uint32_t flags = obj >= 0x1000 ? PPC_LOAD_U32(obj + 0x30) : 0;
    const uint32_t tile = obj >= 0x1000 ? PPC_LOAD_U32(obj + 0x34) : 0;
    const uint32_t before = obj >= 0x1000 ? PPC_LOAD_U32(obj + 0x164) : 0;

    __imp__sub_8284A6D0(ctx, base);

    // Read the published select BACK out of the object rather than recomputing
    // sub_8284A668 here: a probe that reimplements what it measures can only agree
    // with itself (the same rule the patch-pass probe above follows).
    const uint32_t after = obj >= 0x1000 ? PPC_LOAD_U32(obj + 0x164) : 0;
    if (after != before)
        changed.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t i = 0;
        for (; i < kSites; i++)
        {
            if (!sites[i].used) { sites[i] = { after, flags, tile, 1, true }; break; }
            if (sites[i].sel == after && sites[i].flags == flags && sites[i].tile == tile)
            { sites[i].count++; break; }
        }
    }
    const uint64_t n = calls.fetch_add(1) + 1;
    if (n == 1 || BinReportDue(nextReport))
    {
        std::lock_guard<std::mutex> lock(mutex);
        char line[1024];
        int off = snprintf(line, sizeof line,
                           "[binmask] select producer: %llu calls, %llu published:",
                           (unsigned long long)n, (unsigned long long)changed.load());
        for (size_t i = 0; i < kSites && sites[i].used; i++)
            off += snprintf(line + off, sizeof line - size_t(off),
                            "  sel=%08X(flags=%08X tile=%u)x%llu", sites[i].sel,
                            sites[i].flags, sites[i].tile,
                            (unsigned long long)sites[i].count);
        fprintf(stderr, "%s\n", line);
    }
}

// ---------------------------------------------------------------------------------
// CZ_DIGEST_PROBE=1 — the file-digest check, link by link.
//
// `sub_82788478(container, name, buffer, length, flags)` is the digest manager's
// verify. It calls XexGetModuleSection for the XEX resource whose name sits at
// `container+4` ("Digest"), hashes `name` with the engine's own `h = h*0x21 ^ (signed
// char)c` string hash, looks that hash up in the table, SHA-1s `buffer || length`, and
// compares. If ANY of those steps fails it runs
// `dbAssert(0 && "Bad file digest.  Please re-link the executable and try again.")`
// from digestmanager.cpp, whose tail is `twi 31,r0,22` followed by `stw r26,0(0)` —
// so the observable is a null-store SIGSEGV in guest code with no hint that an
// assertion is what produced it.
//
// The reason this needs a hook rather than a debugger is gotcha 57: the compiler keeps
// PPCContext fields in host registers across calls, so `ctx.rN` read at a breakpoint
// in the MIDDLE of a recompiled function is stale — two attempts to read the computed
// digest off the guest stack under `gdb` returned the assert-reporting path's
// registers and twenty zero bytes. At a function's ENTRY the values are fresh, which
// is exactly what the alias seam gives for free.
static bool DigestProbeEnabled()
{
    static const bool on = getenv("CZ_DIGEST_PROBE") != nullptr;
    return on;
}

static void DumpBytes(const char* label, uint32_t va, uint32_t n)
{
    uint8_t* base = g_memory.base;
    fprintf(stderr, "[digest]   %s %08X:", label, va);
    for (uint32_t i = 0; i < n; i++)
        fprintf(stderr, "%s%02X", (i % 4) ? "" : " ", PPC_LOAD_U8(va + i));
    fprintf(stderr, "\n");
}

// SHA1_Final(context, out) — the last link. Printing its OUTPUT is the one thing that
// separates "the table lookup missed" from "the hash of the file came out wrong",
// which are different subsystems and look identical from the assert.
PPC_FUNC(sub_82822430)
{
    const uint32_t out = ctx.r4.u32;
    __imp__sub_82822430(ctx, base);
    if (DigestProbeEnabled())
    {
        static std::atomic<int> shown{ 0 };
        if (shown.fetch_add(1, std::memory_order_relaxed) < 8)
            DumpBytes("SHA1_Final ->", out, 20);
    }
}

PPC_FUNC(sub_82788478)
{
    if (!DigestProbeEnabled())
    {
        __imp__sub_82788478(ctx, base);
        return;
    }
    const uint32_t container = ctx.r3.u32;
    const uint32_t nameVa = ctx.r4.u32;
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32;

    // The engine's string hash, recomputed HERE in host code. That makes this an
    // oracle rather than a description: if the guest's own hash of the same bytes
    // disagrees with this one, the defect is in the recompiled hash function and not
    // in the table, and no amount of reading the table can say so.
    char name[256] = {};
    for (uint32_t i = 0; i < sizeof name - 1; i++)
    {
        name[i] = char(PPC_LOAD_U8(nameVa + i));
        if (!name[i])
            break;
    }
    uint32_t want = 0;
    for (const char* p = name; *p; p++)
        want = uint32_t(want * 0x21) ^ uint32_t(int32_t(*p));

    fprintf(stderr,
            "[digest] verify '%s' buffer=%08X length=%u  section='%s'  host hash=%08X\n",
            name, buffer, length, reinterpret_cast<const char*>(base + container + 4),
            want);

    __imp__sub_82788478(ctx, base);
    fprintf(stderr, "[digest] verify '%s' -> %u\n", name, ctx.r3.u32);
}

// ---------------------------------------------------------------------------------
// CZ_SAVE_PROBE=1 — the guest's OWN XGetOverlappedResult, printing the block it read.
//
// WHY THIS EXISTS
// ---------------
// The save reaches XamContentCreateEx, gets the ERROR_IO_PENDING its call site
// demands, and is then torn down without a byte being written. The teardown's call
// stack (CZ_KCALL_WHO=XamContentClose) puts the decision at 825D6094:
//
//     bl   sub_825D83A8          ; the title's hand-rolled XGetOverlappedResult
//     cmplwi r3,0    / beq       ; 0     -> the save proceeds
//     cmplwi r3,0x3e4/ beq       ; 996   -> ERROR_IO_INCOMPLETE, poll again
//     ...                        ; ANYTHING ELSE -> free, close, "Save failed"
//
// and sub_825D83A8 simply returns `ovl->result` once that word is no longer 997. We
// write 0 into it. So either the word we wrote is not the word it reads, or something
// overwrites it between the two — and those are different bugs with different fixes,
// which no amount of further disassembly can separate. Print the block from inside
// the guest's own reader and the question is settled in one run.
//
// Budgeted rather than capped-and-silent: this poll also serves the profile and
// enumerate paths, so it runs long before any save does.
extern "C" PPC_FUNC(__imp__sub_825D83A8);

PPC_FUNC(sub_825D83A8)
{
    static const bool on = getenv("CZ_SAVE_PROBE") != nullptr;
    if (!on)
    {
        __imp__sub_825D83A8(ctx, base);
        return;
    }
    const uint32_t ovl = ctx.r3.u32;
    const uint32_t result = ovl ? PPC_LOAD_U32(ovl + 0) : 0;
    const uint32_t length = ovl ? PPC_LOAD_U32(ovl + 4) : 0;
    const uint32_t event = ovl ? PPC_LOAD_U32(ovl + 12) : 0;
    const uint32_t extended = ovl ? PPC_LOAD_U32(ovl + 24) : 0;
    __imp__sub_825D83A8(ctx, base);

    // Everything that is NOT the ordinary "still pending" answer, plus a thin sample
    // of the pending ones so the line rate says whether polling is even happening.
    static std::atomic<uint64_t> n{ 0 };
    const uint64_t i = n.fetch_add(1, std::memory_order_relaxed);
    const bool interesting = result != 997;
    if (interesting || (i % 2048) == 0)
        fprintf(stderr,
                "[save] XGetOverlappedResult(ovl=%08X) block{result=%08X length=%08X "
                "event=%08X extended=%08X} -> %u%s\n",
                ovl, result, length, event, extended, ctx.r3.u32,
                result == 997 ? "  (pending, sampled)" : "");
}

// ---------------------------------------------------------------------------------
// CZ_GUEST_LOG=1 — the ENGINE'S OWN debug output, which it has been writing all
// along and nobody was reading.
//
// WHY THIS EXISTS
// ---------------
// Every hunt in this port so far has instrumented the runtime and inferred the
// title's state from the outside. But this image is a release build of a PC-hosted
// engine and it kept its debug logging: `sub_827877C8` is a vsnprintf into an 0x800
// stack buffer with **640 distinct callers**, and it hands the formatted result to
// `sub_828223A0`, which twelve sites in the image share. Hooking that one function
// turns the game into its own narrator — `[FE] Showing tutorial %d`,
// `cinematics are playing`, `For cinematic props... check the prop names match`,
// and 637 more, in the engine's own vocabulary and at the engine's own moments.
//
// The catch, stated because it is what makes the output partial rather than
// complete: most of the interesting call sites are gated on a debug byte
// (`lbz r11,<flag>; cmplwi r11,0; bne <skip>`) that a shipped build leaves at zero.
// So this prints what the title logs UNCONDITIONALLY — errors, warnings and
// asserts — and the gated categories stay silent until their flag is raised.
// Silence from a category is therefore not evidence about that category (gotcha
// 25); it is evidence about the flag.
extern "C" PPC_FUNC(__imp__sub_828223A0);

PPC_FUNC(sub_828223A0)
{
    static const bool on = getenv("CZ_GUEST_LOG") != nullptr;
    if (on)
    {
        char text[512];
        uint32_t i = 0;
        for (; i < sizeof text - 1; i++)
        {
            const char c = char(PPC_LOAD_U8(ctx.r3.u32 + i));
            if (!c)
                break;
            text[i] = c;
        }
        text[i] = '\0';
        // The engine's messages carry their own newlines inconsistently, so trim
        // and re-add one: a log whose lines do not line up cannot be grepped.
        while (i && (text[i - 1] == '\n' || text[i - 1] == '\r'))
            text[--i] = '\0';
        if (i)
            fprintf(stderr, "[guest] %s\n", text);
    }
    __imp__sub_828223A0(ctx, base);
}

// ---------------------------------------------------------------------------------
// CZ_XMA_PROBE=1 — the guest's own "is this voice still playing?" predicate, beside
// the hardware bits it computes the answer from.
//
// WHY THIS EXISTS
// ---------------
// Part 15 left the prologue stuck in a faded-out state with a frozen camera while
// the draw stream kept moving, and named audio as the leading hypothesis on the
// strength of a peak amplitude of exactly 0.0000. That is a statement about our
// output, not about anything the guest can observe, so it could never have been more
// than a suspicion. The image states the mechanism outright:
//
//   sub_8285EFE0(pool, i)   reads the i'th XMA context's dword 0 and returns
//                           ((d0 >> 20) & 3) == 0 — bits 20 and 21 are the two
//                           input-buffer-VALID flags. The guest sets them when it
//                           hands the decoder packets; the DECODER clears them as it
//                           consumes them. So this is "has this context run dry".
//   sub_82862A90(voice)     loops over the voice's contexts and returns 1 as soon as
//                           one of them has NOT run dry, i.e. IsPlaying().
//   sub_82864808(voice)     the per-update edge detector: it remembers the previous
//                           answer at voice+0x120 and branches three ways on
//                           (old, new) — the playing -> stopped transition is the
//                           one that runs the voice's shutdown path at 828648A4.
//
// The edge is counted by reading voice+0x120 either side of the call rather than by
// hooking the handler, because the first version of this probe hooked sub_828638D0
// on the strength of the 82864854 call site and called it "the finished handler".
// It has TWO call sites in that one function — the other is on the still-playing
// path — so it is the per-update streaming refill, and the counter read 284,354
// where the truth was 0. Attribute a count to the branch, not to the callee.
//
// We have no XMA decoder, so nothing on this machine ever clears an input-valid bit.
// The prediction that follows is exact and falsifiable: IsPlaying() returns 1 for
// every call for the life of the process, the playing -> stopped edge never fires,
// and anything cued off a voice completing waits forever. This probe is what turns
// that from a reading of the disassembly into a measurement — it counts both answers
// and prints the raw context words the guest read them out of, so a run that
// DISAGREES with the reading says so rather than being silently assumed.
//
// The clock is deliberate (gotcha 186): a call-count schedule on a predicate polled
// tens of thousands of times a boot prints once and reads as "this ran once".
extern "C" PPC_FUNC(__imp__sub_8285EFE0);
extern "C" PPC_FUNC(__imp__sub_82862A90);
extern "C" PPC_FUNC(__imp__sub_82864808);

static bool XmaProbeEnabled()
{
    static const bool on = getenv("CZ_XMA_PROBE") != nullptr;
    return on;
}

namespace
{
std::atomic<uint64_t> g_xmaIsPlayingCalls{ 0 };   // sub_82862A90 entries
std::atomic<uint64_t> g_xmaIsPlayingTrue{ 0 };    //   ... that answered "still playing"
std::atomic<uint64_t> g_xmaDryCalls{ 0 };         // sub_8285EFE0 entries
std::atomic<uint64_t> g_xmaDryTrue{ 0 };          //   ... that answered "run dry"
std::atomic<uint64_t> g_xmaUpdates{ 0 };          // sub_82864808 entries
std::atomic<uint64_t> g_xmaStartEdges{ 0 };       // voice+0x120 0 -> 1 transitions
std::atomic<uint64_t> g_xmaStopEdges{ 0 };        // voice+0x120 1 -> 0 transitions

// Print on a 5 s clock. Returns true at most once per interval, from whichever
// thread gets there first.
bool XmaProbeTick()
{
    using clock = std::chrono::steady_clock;
    static std::mutex m;
    static clock::time_point next{};
    std::lock_guard<std::mutex> lock(m);
    const auto now = clock::now();
    if (now < next)
        return false;
    next = now + std::chrono::seconds(5);
    return true;
}

void XmaProbeReport()
{
    uint8_t* const base = g_memory.base;
    const uint32_t arrayVa = Audio_XmaContextArray();
    fprintf(stderr,
            "[xma] isPlaying calls=%llu playing=%llu | dry-test calls=%llu dry=%llu | "
            "updates=%llu startEdges=%llu stopEdges=%llu\n",
            (unsigned long long)g_xmaIsPlayingCalls.load(std::memory_order_relaxed),
            (unsigned long long)g_xmaIsPlayingTrue.load(std::memory_order_relaxed),
            (unsigned long long)g_xmaDryCalls.load(std::memory_order_relaxed),
            (unsigned long long)g_xmaDryTrue.load(std::memory_order_relaxed),
            (unsigned long long)g_xmaUpdates.load(std::memory_order_relaxed),
            (unsigned long long)g_xmaStartEdges.load(std::memory_order_relaxed),
            (unsigned long long)g_xmaStopEdges.load(std::memory_order_relaxed));
    if (!arrayVa)
        return;

    // The hardware kick bitmap, one bit per context, index >> 5 — the register the
    // guest writes to tell the decoder a context is armed. It lands in ordinary flat
    // memory here and nothing consumes it; printing it says whether the guest is
    // still asking for work, which "the pump produced silence" cannot.
    const uint32_t kickBitmap = 0x7FEA1A80;
    fprintf(stderr, "[xma]   kick bitmap %08X: %08X %08X\n", kickBitmap,
            *reinterpret_cast<uint32_t*>(g_memory.Translate(kickBitmap)),
            *reinterpret_cast<uint32_t*>(g_memory.Translate(kickBitmap + 4)));

    for (unsigned i = 0; i < Audio_XmaContextCount(); i++)
    {
        if (!Audio_XmaContextInUse(i))
            continue;
        const uint32_t va = arrayVa + i * Audio_XmaContextSize();
        uint32_t d[16];
        for (int w = 0; w < 16; w++)
            d[w] = PPC_LOAD_U32(va + w * 4);
        // ALL SIXTEEN, because the four that used to print here were the four whose
        // meaning we already knew, and a decoder needs the pointer dwords. Which
        // dword holds input_buffer_0_ptr is a HARDWARE fact this project has never
        // measured — it was going to be taken from the Fable 2 port's copy of
        // Xenia's struct, which is a recollection, not evidence (gotcha 3 in its
        // other direction: believing a layout nobody checked here). A guest address
        // is instantly recognisable in a hex dump; a bitfield is not. So dump the
        // context and let the addresses name their own dwords.
        fprintf(stderr,
                "[xma]   ctx%u @%08X inputValid=%u%u loopCount=%u pkts0=%u\n"
                "[xma]     %08X %08X %08X %08X %08X %08X %08X %08X\n"
                "[xma]     %08X %08X %08X %08X %08X %08X %08X %08X\n",
                i, va, (d[0] >> 20) & 1, (d[0] >> 21) & 1, (d[0] >> 12) & 0xFF,
                d[0] & 0xFFF, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8],
                d[9], d[10], d[11], d[12], d[13], d[14], d[15]);

        // WHAT IS ACTUALLY IN THE BUFFERS.
        //
        // Phase A/V wired a real XMA2 decoder and it produced nothing, and the
        // reason was one line of hex: the first packet of the title's first voice
        // is `00 00 00 00 ...`. The guest had declared 64 valid packets at an
        // address holding no XMA data. That is a second silence, one level below
        // the one open item 00e measured, and it is invisible to every counter
        // that talks about contexts rather than about bytes.
        //
        // So scan the declared buffers. This lives in the PROBE rather than in the
        // decoder because the decoder consumes and RETIRES an input buffer within
        // a few milliseconds of seeing it, which destroys the evidence it is being
        // asked about — with `CZ_NO_XMA_DECODE=1` this is a purely passive reading
        // of what the guest wrote.
        // A context's buffer pointers are PHYSICAL — the APU is a DMA device — and in
        // our flat map physical `P` is the cached-view alias `0xA0000000 | P`. The
        // first version of this scan read the untranslated address and reported
        // "0 non-zero" on a buffer the title had just filled, which is the finding
        // this comment exists to stop being re-made (kernel/audio.cpp says it in
        // full). Both addresses print, because the pair IS the evidence.
        auto phys2virt = [](uint32_t p) { return p >= 0xA0000000u ? p : (0xA0000000u | (p & 0x1FFFFFFFu)); };
        auto scan = [&](const char* name, uint32_t physPtr, uint32_t packets) {
            if (!physPtr || !packets)
                return;
            const uint32_t ptr = phys2virt(physPtr);
            const uint64_t bytes = uint64_t(packets) * 2048;
            if (uint64_t(ptr) + bytes > uint64_t(PPC_MEMORY_SIZE))
            {
                fprintf(stderr, "[xma]     %s=%08X x%u pkts: OUT OF RANGE\n", name, ptr,
                        packets);
                return;
            }
            const uint8_t* p = g_memory.base + ptr;
            uint64_t nonZero = 0;
            int64_t first = -1;
            for (uint64_t b = 0; b < bytes; b++)
                if (p[b])
                {
                    if (first < 0)
                        first = int64_t(b);
                    nonZero++;
                }
            fprintf(stderr,
                    "[xma]     %s=%08X (phys %08X) %u pkts (%llu bytes): %llu non-zero "
                    "(%.2f%%), first at %lld\n",
                    name, ptr, physPtr, packets, (unsigned long long)bytes,
                    (unsigned long long)nonZero, 100.0 * double(nonZero) / double(bytes),
                    (long long)first);
        };
        scan("in0", d[5], d[0] & 0xFFF);
        scan("in1", d[6], d[1] & 0xFFF);

        // And the OUTPUT ring, which is the thing the title's own mixer reads. A
        // non-zero input with a zero output is our decoder failing; both zero is
        // the guest never producing audio in the first place. The two have the same
        // symptom and completely different owners.
        const uint32_t outPtr = phys2virt(d[7]);
        const uint32_t outBytes = ((d[0] >> 22) & 0x1F) * 256;
        if (outPtr && outBytes && uint64_t(outPtr) + outBytes <= uint64_t(PPC_MEMORY_SIZE))
        {
            const uint8_t* p = g_memory.base + outPtr;
            uint32_t nonZero = 0;
            for (uint32_t b = 0; b < outBytes; b++)
                if (p[b])
                    nonZero++;
            fprintf(stderr, "[xma]     out=%08X %u bytes: %u non-zero (%.2f%%)\n", outPtr,
                    outBytes, nonZero, 100.0 * double(nonZero) / double(outBytes));
        }
    }
}
} // namespace

// sub_8285EFE0(pool, index) -> 1 when the context's two input buffers are both
// invalid, i.e. the decoder has consumed everything it was given.
PPC_FUNC(sub_8285EFE0)
{
    if (!XmaProbeEnabled())
    {
        __imp__sub_8285EFE0(ctx, base);
        return;
    }
    __imp__sub_8285EFE0(ctx, base);
    g_xmaDryCalls.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32)
        g_xmaDryTrue.fetch_add(1, std::memory_order_relaxed);
}

// sub_82862A90(voice) -> IsPlaying: 1 if ANY of the voice's contexts still has input.
PPC_FUNC(sub_82862A90)
{
    if (!XmaProbeEnabled())
    {
        __imp__sub_82862A90(ctx, base);
        return;
    }
    __imp__sub_82862A90(ctx, base);
    g_xmaIsPlayingCalls.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32)
        g_xmaIsPlayingTrue.fetch_add(1, std::memory_order_relaxed);
}

// sub_82864808(voice) — the per-update edge detector. Counted so that "the predicate
// is stuck" and "nothing is polling it" stay separable, which they are not from the
// predicate's own counter alone. voice+0x120 is the cached previous answer, so
// reading it either side of the call gives the transition the guest itself saw.
PPC_FUNC(sub_82864808)
{
    if (!XmaProbeEnabled())
    {
        __imp__sub_82864808(ctx, base);
        return;
    }
    const uint32_t voice = ctx.r3.u32;
    const uint32_t before = PPC_LOAD_U32(voice + 0x120);
    g_xmaUpdates.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82864808(ctx, base);
    const uint32_t after = PPC_LOAD_U32(voice + 0x120);
    if (!before && after)
        g_xmaStartEdges.fetch_add(1, std::memory_order_relaxed);
    else if (before && !after)
        g_xmaStopEdges.fetch_add(1, std::memory_order_relaxed);
    if (XmaProbeTick())
        XmaProbeReport();
}

// ---------------------------------------------------------------------------
// CZ_CINE_PROBE=1 — is the cinematic CONTINUOUSLY waiting for its end sync point,
// or does it pass that branch once?
// ---------------------------------------------------------------------------
//
// WHY THIS EXISTS. `CZ_GUEST_DIAG` made the prologue narrate its own stall:
//
//     [guest] WAITING: end sync point not received yet!
//
// printed at 0x824A10D4, on the FAILING side of a branch whose success side
// (0x824A10E0) is a virtual call taking a float in f1 — the cinematic's
// Update(delta). So a missing sync point means the cinematic is not advanced.
//
// That reading has a hole, and the hole is why this file gained a probe instead of a
// conclusion: over 7,778 frames of continuous ping-ponging the line printed **once**.
// A branch taken every frame would print thousands of times. Either the print site
// latches and the wait is continuous, or the path is taken once and something else
// sustains the loop — which would make the sync point an event at the START of the
// loop rather than the condition holding it. Different defect, different fix.
//
// A print cannot answer that (it is the thing under suspicion). A counter can.
//
// WHAT IS HOOKED, AND WHY IT IS NOT 0x824A10D4. That address is mid-function, and the
// hook seam in this port replaces whole functions — there is nowhere to attach. Two
// real function boundaries bracket it instead:
//
//   sub_8249EEA8  the predicate itself. The caller does
//                     bl sub_8249EEA8 / clrlwi. r11,r3,0x18 / beq <WAITING path>
//                 so a return of ZERO *is* the branch condition. Counting its calls
//                 and its zero-returns measures the wait directly, at its source.
//   sub_824A0FC0  the function containing both the call and the print (verified: no
//                 blr between 0x824A0FC0 and 0x824A10D4). Its entry count is the
//                 denominator — how often the containing tick ran at all.
//
// THE POSITIVE CONTROL IS BUILT IN, and it has to be (gotcha 30). Reporting only
// "not received" would leave a stuck counter and a stuck predicate identical. Both
// outcomes are counted, so the RECEIVED count during the healthy part of the
// cinematic is the proof the instrument can report the other answer. If received
// stays 0 for the whole run, treat the instrument as unproven, not the predicate as
// stuck.
//
// Cost when off: one `getenv`-backed bool test on two functions. Neither is on the
// per-draw path.
extern "C" PPC_FUNC(__imp__sub_8249EEA8);
extern "C" PPC_FUNC(__imp__sub_824A0FC0);

namespace {

std::atomic<uint64_t> g_cineTicks{ 0 };        // sub_824A0FC0 entries
std::atomic<uint64_t> g_cineSyncCalls{ 0 };    // sub_8249EEA8 calls
std::atomic<uint64_t> g_cineSyncMissing{ 0 };  // ... returning 0 = "not received yet"

bool CineProbeEnabled()
{
    static const bool on = getenv("CZ_CINE_PROBE") != nullptr;
    return on;
}

// Its own 5 s clock, deliberately separate from the XMA probe's: this instrument has
// to be usable on a run with no audio arms set at all.
bool CineProbeTick()
{
    using clock = std::chrono::steady_clock;
    static std::mutex m;
    static clock::time_point next{};
    std::lock_guard<std::mutex> lock(m);
    const auto now = clock::now();
    if (now < next)
        return false;
    next = now + std::chrono::seconds(5);
    return true;
}

void CineProbeReport()
{
    const uint64_t ticks = g_cineTicks.load(std::memory_order_relaxed);
    const uint64_t calls = g_cineSyncCalls.load(std::memory_order_relaxed);
    const uint64_t missing = g_cineSyncMissing.load(std::memory_order_relaxed);
    fprintf(stderr,
            "[cine] sub_824A0FC0 ticks=%llu | end-sync-point asked=%llu "
            "NOT-received=%llu received=%llu\n",
            (unsigned long long)ticks, (unsigned long long)calls,
            (unsigned long long)missing, (unsigned long long)(calls - missing));
}

} // namespace

// sub_8249EEA8 -> 0 when the end sync point has NOT arrived. See the block above.
PPC_FUNC(sub_8249EEA8)
{
    if (!CineProbeEnabled())
    {
        __imp__sub_8249EEA8(ctx, base);
        return;
    }
    __imp__sub_8249EEA8(ctx, base);
    g_cineSyncCalls.fetch_add(1, std::memory_order_relaxed);
    // The caller tests only the low byte (`clrlwi. r11,r3,0x18`), so this must too —
    // reading the full word would count a high-byte-only value as "received" where
    // the guest reads it as zero.
    if ((ctx.r3.u32 & 0xFF) == 0)
        g_cineSyncMissing.fetch_add(1, std::memory_order_relaxed);
}

// sub_824A0FC0 — the tick containing both the predicate call and the WAITING print.
PPC_FUNC(sub_824A0FC0)
{
    if (!CineProbeEnabled())
    {
        __imp__sub_824A0FC0(ctx, base);
        return;
    }
    g_cineTicks.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_824A0FC0(ctx, base);
    if (CineProbeTick())
        CineProbeReport();
}

// ---------------------------------------------------------------------------
// CZ_CINE_TIME=<file> — the cinematic's own CLOCK, one line per frame it is asked for
// ---------------------------------------------------------------------------
//
// WHY THIS EXISTS. `open-items.md` 00j is a prologue cinematic that ping-pongs: the
// camera walks forward ~1 s, back ~1 s, forever. Three consumer-side explanations were
// bought and refuted (our output ring, the audio stopping, the animation end sync
// point). The remaining instruction in the hand-off was the right one and is not a
// guess: **a palindrome means some clock DECREMENTS, so find what writes the
// cinematic's time.** This probe is that value, read at its source.
//
// WHAT WAS FOUND STATICALLY, because reading it is what makes the columns mean
// something. `sub_82475718` IS the cinematic clock — it returns, in f1, the time the
// scene is to be played at, and `sub_82478FC8` calls it twice per cinematic update.
// Its body is a three-way switch on a MODE word in the config block it is handed:
//
//     mode 0 (or anything else) -> return cfg[0]          the raw scene time; no audio
//     mode 1                    -> return audioPos        slave straight to the stream
//     mode 2                    -> return PID(audioPos)   <- sub_824741D8
//
// and `sub_824741D8` is a textbook PID, which the image names itself: its tail plots
// four values through the engine's debug-graph API under the strings
// `Cine.Audio P-gain / I-gain / D-gain / MV (ms)` (0x82062FF0/FD8/FC0/FAC). Decoded:
//
//     err  = (cfg[0] - audioPos) - this[0x28]      error, outside a deadband cfg[0x18]
//     MV   = P*err + I*integral + D*(err - prevErr)
//     this[0x28] += MV                             <- the accumulated CORRECTION
//     return cfg[0] - this[0x28]                   <- the time handed back
//
// **That last line is the whole reason this probe exists.** The returned time is a
// setpoint MINUS an accumulator that a control loop drives. Nothing in it is monotonic.
// If the accumulator overshoots, the time the cinematic is played at goes DOWN, and the
// scene runs backwards — which is the reported symptom, exactly, and with the right
// shape (smooth, symmetric, and starting when a synced stream starts).
//
// So the columns are chosen to make that refutable rather than illustrative:
//
//   msec      host steady clock, ms since the probe's first line
//   mode      cfg[4]. **If this is never 2 the PID is not the mechanism and this whole
//             reading is dead** — which is the single most valuable thing the file can
//             say, and it says it in one column
//   playing   sub_82758CC8 — did the stream report itself playing this frame
//   audioPos  sub_82759170 — the stream position the loop is tracking, i.e. what OUR
//             decoder makes true. The PID's input
//   ret       f1 on return: the time the scene is played at. **This is the value whose
//             non-monotonicity is the defect.** Diff it and the palindrome is arithmetic
//   setpoint  cfg[0], the uncorrected scene time. Monotonic if the caller is healthy
//   acc       this[0x28], the accumulated correction
//   prevErr   this[0x2c]
//   integ     this[0x34]
//   pid       1 if sub_824741D8 actually ran this frame
//
// `ret` and `setpoint` together are also the discriminator this probe cannot be talked
// out of: if `setpoint` oscillates too, the correction is innocent and the caller is
// the defect; if `setpoint` climbs while `ret` ping-pongs, it is the correction.
//
// ON GOTCHA 269, WHICH THIS PROBE IS SHAPED BY. The previous cinematic probe reported
// from inside the function it counted, so it went silent exactly when its subject
// stopped — and only luck (frames visibly advancing) made that readable. This one
// cannot avoid being driven by its subject either: the value only exists when the
// guest asks for it. What it does instead is stamp every line with a host clock and
// write to a FILE, so the run's independent clock — `CZ_VK_FRAME_STATS`, written by the
// graphics pump regardless — can be laid beside it. A gap in this file next to
// continuing frames is then a measurement ("not called for 4 s"), not an absence of
// data. Pair the two files; never read this one alone.
//
// Cost when off: one `getenv`-backed bool test on four functions, none on a draw path.
extern "C" PPC_FUNC(__imp__sub_82475718);
extern "C" PPC_FUNC(__imp__sub_824741D8);
extern "C" PPC_FUNC(__imp__sub_82758CC8);
extern "C" PPC_FUNC(__imp__sub_82759170);

namespace {

// The guest is big-endian and these are all `lfs`/`stfs` singles. PPC_LOAD_U32 does the
// byte swap; the bit-cast is the remaining half. Reading them as host floats directly
// would be silently wrong on every value that is not a byte-palindrome.
float GuestF32(uint8_t* base, uint32_t addr)
{
    const uint32_t bits = PPC_LOAD_U32(addr);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

FILE* CineTimeFile()
{
    static FILE* f = [] () -> FILE* {
        const char* path = getenv("CZ_CINE_TIME");
        if (!path || !*path)
            return nullptr;
        FILE* h = (strcmp(path, "-") == 0) ? stderr : fopen(path, "w");
        if (!h)
        {
            fprintf(stderr, "[cinetime] cannot open '%s' — probe is OFF\n", path);
            return nullptr;
        }
        fprintf(h, "# msec mode playing audioPos ret setpoint acc prevErr integ pid\n");
        return h;
    }();
    return f;
}

// CZ_CINE_AUDIO_MODE=0|1|2 — THE SAME-BINARY ARM FOR 00j, and the reason it is worth
// having is that every one of its settings is a path the TITLE ITSELF implements.
// sub_82475718 switches on cfg[4], which both call sites copy out of the read-only
// global 0x829DC320 (one reader each, no writer anywhere in the image; shipped as 2):
//
//     0  return the raw scene time              — no audio sync at all
//     1  return the audio stream position       — slave the scene straight to the stream
//     2  return PID(audio position)             — SHIPPED, and the suspect
//
// So this is not a synthetic behaviour bolted onto the guest; it is the guest's own
// three-way switch, driven from outside. That matters for admissibility: an arm that
// invents a code path can only ever refute itself, where this one either removes the
// correction and the palindrome goes with it, or removes the correction and the
// palindrome stays — and the second answer would kill the PID reading outright.
//
// -1 (unset) means "leave the guest alone", so a run with neither this nor
// CZ_CINE_TIME set takes the original function with one predictable branch in front.
int CineAudioModeOverride()
{
    static const int mode = [] {
        const char* v = getenv("CZ_CINE_AUDIO_MODE");
        if (!v || !*v)
            return -1;
        const int m = atoi(v);
        fprintf(stderr, "[cinetime] forcing Cine.Audio sync mode %d "
                        "(shipped is 2 = PID)\n", m);
        return m;
    }();
    return mode;
}

// Values sub_82475718 reads through calls whose returns it does not keep anywhere we
// can see afterwards. Both callees have exactly ONE caller in the image (checked with
// tools/guest_callers.py), so there is no interleaving to disambiguate and a plain
// global is honest here — this is not a general-purpose stash.
float g_cineAudioPos = 0.0f;
int g_cineAudioPlaying = -1;   // -1 = the getter was not reached this frame
int g_cinePidRan = 0;

} // namespace

// sub_82758CC8 — "is the cinematic's audio stream playing". Called only from the clock.
PPC_FUNC(sub_82758CC8)
{
    __imp__sub_82758CC8(ctx, base);
    if (CineTimeFile())
        g_cineAudioPlaying = (int)(ctx.r3.u32 & 0xFF);
}

// sub_82759170 — the stream position in seconds, in f1. The PID's input.
PPC_FUNC(sub_82759170)
{
    __imp__sub_82759170(ctx, base);
    if (CineTimeFile())
        g_cineAudioPos = (float)ctx.f1.f64;
}

// sub_824741D8 — the PID. Only its "did it run" bit is taken here; every value it
// touches is a member of the object the caller still holds, so they are read there.
PPC_FUNC(sub_824741D8)
{
    __imp__sub_824741D8(ctx, base);
    if (CineTimeFile())
        g_cinePidRan = 1;
}

// sub_82475718 — the cinematic clock itself. One line per call.
PPC_FUNC(sub_82475718)
{
    FILE* out = CineTimeFile();
    const int forced = CineAudioModeOverride();
    if (!out && forced < 0)
    {
        __imp__sub_82475718(ctx, base);
        return;
    }

    // r3/r4 are argument registers and the callee is free to clobber both, so the two
    // object pointers have to be taken BEFORE the call. Reading them afterwards is the
    // kind of mistake that yields plausible numbers off whatever happened to be left.
    const uint32_t self = ctx.r3.u32;
    const uint32_t cfg = ctx.r4.u32;

    // THE ARM. Both call sites build this config block on their own stack and copy the
    // mode out of a read-only global (0x829DC320, shipped as 2), so writing it here —
    // after the block is built and before the switch reads it — reaches every path and
    // cannot be stomped by a later re-initialisation. See CineAudioModeOverride().
    if (forced >= 0)
        PPC_STORE_U32(cfg + 4, (uint32_t)forced);

    g_cineAudioPlaying = -1;
    g_cinePidRan = 0;
    g_cineAudioPos = 0.0f;

    __imp__sub_82475718(ctx, base);

    if (!out)
        return;

    static const auto t0 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();

    fprintf(out, "%.1f %d %d %.6f %.6f %.6f %.6f %.6f %.6f %d\n",
            ms,
            (int)PPC_LOAD_U32(cfg + 4),
            g_cineAudioPlaying,
            g_cineAudioPos,
            (float)ctx.f1.f64,
            GuestF32(base, cfg + 0),
            GuestF32(base, self + 0x28),
            GuestF32(base, self + 0x2C),
            GuestF32(base, self + 0x34),
            g_cinePidRan);
    // Flushed per line on purpose: these runs end on a `timeout` SIGTERM, and a probe
    // whose last buffer never reaches disk loses exactly the tail that matters.
    fflush(out);
}

// ---------------------------------------------------------------------------------
// Part 43, item 00i — the per-zone COMMON_TEXTURE vs COMMON_TEXTURE_LOD decision.
//
// sub_82270870 is cZone-streaming's "load this zone's common texture set". Its LOD
// branch (0x82270C38..C70) picks COMMON_TEXTURE_LOD.tex iff the zone's byte flag at
// rec+0x90C is set AND sub_821C4F28(rec+0x910) == 1, where the latter walks the
// zone's volume list and returns 1 only if EVERY volume is far from the camera:
// sub_82175040 computes dist = |cam - sphere(e+0x80).xyz| - 0.01 - sphere.w and
// treats the volume as far when dist > threshold(e+0xA8), the threshold boosted by
// the per-level tables at 0x82042C18/0x82042D68 when it is under ~25-30.
//
// So the decision SNAPSHOTS the camera position at zone-load time. This probe prints
// every input of that computation on entry — camera, per-volume spheres, thresholds,
// distances — plus its own prediction of the branch, which the CZ_GUEST_DIAG
// narration of the same run can then confirm or refute (gotcha 30: an instrument
// must be able to disagree with something).
//
// Behind its own env var rather than CZ_ARG_PROBE because it is useful alone on an
// otherwise-quiet run; costs one predictable branch when off.
extern "C" PPC_FUNC(__imp__sub_82270870);

PPC_FUNC(sub_82270870)
{
    static const bool zoneProbeOn = getenv("CZ_ZONE_TEX_PROBE") != nullptr;
    if (zoneProbeOn)
    {
        const uint32_t self = ctx.r3.u32;
        const uint32_t zone = ctx.r4.u32;

        static const auto t0 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();

        const uint32_t g = PPC_LOAD_U32(0x82A46294);
        const float camX = GuestF32(base, g + 0x40);
        const float camY = GuestF32(base, g + 0x44);
        const float camZ = GuestF32(base, g + 0x48);
        const uint32_t level = PPC_LOAD_U32(g + 0x34F5C);
        const uint8_t force = PPC_LOAD_U8(0x82A57BD7);

        // zone id -> record: slot table at this+0x834C, records strided 0x3F0.
        const uint32_t slot = PPC_LOAD_U32(self + 0x834C + 4 * zone);
        const uint32_t rec = self + slot * 0x3F0;
        const uint8_t flag = PPC_LOAD_U8(rec + 0x90C);
        const uint32_t volObj = PPC_LOAD_U32(rec + 0x910);

        // Per-level threshold boost, as sub_82373DC0/82373E00 read it (the debug
        // override byte at 0x82A58623 is reported rather than honoured: it ships 0).
        const float boostBelow = (level < 16) ? GuestF32(base, 0x82042C18 + 4 * level) : 0.f;
        const float boostMul = (level < 16) ? GuestF32(base, 0x82042D68 + 4 * level) : 1.f;

        // rec+0x69C is a directory OBJECT, not a char*: the first version printed it
        // as %s and salted every log line with NULs and newlines, which turned grep
        // binary-mode and made the whole probe read as absent (gotcha 25, self-made).
        // Sanitize to printable ASCII; it still shows a name when one is in there.
        char zname[17] = { 0 };
        for (int i = 0; i < 16; i++)
        {
            uint8_t c = PPC_LOAD_U8(rec + 0x69C + i);
            zname[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }

        fprintf(stderr,
                "[zonetex] %.1fms zone=%u slot=%u rec=%08X name=%s flag90C=%u vol=%08X "
                "cam=(%.2f,%.2f,%.2f) level=%u force=%u boost(<%.1f x%.1f)\n",
                ms, zone, slot, rec, zname, flag, volObj, camX, camY, camZ, level,
                force, boostBelow, boostMul);

        if (volObj)
        {
            const uint32_t count = PPC_LOAD_U32(volObj + 0x120);
            const uint32_t elems = PPC_LOAD_U32(volObj + 0x124);
            uint32_t nNear = 0, nFar = 0, nSkip = 0;
            for (uint32_t i = 0; i < count && i < 256; i++)
            {
                const uint32_t e = elems + i * 0xD0;
                const float x = GuestF32(base, e + 0x80);
                const float y = GuestF32(base, e + 0x84);
                const float z = GuestF32(base, e + 0x88);
                const float r = GuestF32(base, e + 0x8C);
                const uint32_t skip = PPC_LOAD_U32(e + 0x90) & 1;
                float thr = GuestF32(base, e + 0xA8);
                if (thr < boostBelow) thr *= boostMul;
                const float dx = camX - x, dy = camY - y, dz = camZ - z;
                const float d = sqrtf(dx * dx + dy * dy + dz * dz) - 0.01f - r;
                // sub_82175040: skip-bit -> "near" (forces full); else far iff
                // thr - d < ~0, i.e. d > thr.
                const bool far = !skip && (thr - d < 1.3e-11f);
                if (skip) nSkip++;
                else if (far) nFar++;
                else nNear++;
                if (i < 24)
                    fprintf(stderr,
                            "[zonetex]   vol[%u] c=(%.1f,%.1f,%.1f) r=%.1f thr=%.1f "
                            "d=%.1f %s%s\n",
                            i, x, y, z, r, thr, d, skip ? "SKIP->full" : "",
                            !skip ? (far ? "far" : "NEAR->full") : "");
            }
            const bool predictLod = force || (flag && nNear == 0 && nSkip == 0 && count > 0);
            fprintf(stderr,
                    "[zonetex]   count=%u near=%u far=%u skip=%u -> predict %s\n",
                    count, nNear, nFar, nSkip,
                    predictLod ? "COMMON_TEXTURE_LOD.tex" : "COMMON_TEXTURE.tex");
        }
        else
        {
            fprintf(stderr, "[zonetex]   vol=NULL -> predict COMMON_TEXTURE.tex\n");
        }
        fflush(stderr);
    }
    __imp__sub_82270870(ctx, base);
}
