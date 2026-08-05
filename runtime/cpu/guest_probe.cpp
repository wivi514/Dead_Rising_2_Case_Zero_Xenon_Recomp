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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../kernel/memory.h"
#include "guest_thread.h"
#include "ppc_recomp_shared.h"

// `ppc_recomp_shared.h` declares only the WEAK alias `sub_X`, never the real body
// `__imp__sub_X`, so every hook has to declare the one it wraps. The `extern "C"` is
// not optional: the recompiler defines the body with PPC_FUNC_IMPL, which is
// `extern "C" PPC_FUNC`, while a plain PPC_FUNC declaration here would be
// C++-mangled and fail to link (gotcha 33, from the other direction).
extern "C" PPC_FUNC(__imp__sub_82959360);
extern "C" PPC_FUNC(__imp__sub_829565B8);
extern "C" PPC_FUNC(__imp__sub_82955780);
extern "C" PPC_FUNC(__imp__sub_82689A70);

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
    static std::atomic<int> calls{ 0 };
    static std::atomic<int> nulls{ 0 };
    const int n = calls.fetch_add(1);
    const bool anomaly = cb == 0;
    if (n >= 4 && !(anomaly && nulls.fetch_add(1) < 16))
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
