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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../gpu/d3d_draw.h"
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
