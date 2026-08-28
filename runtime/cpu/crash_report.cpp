// A SIGSEGV/SIGBUS handler that prints the GUEST state, not the host state.
//
// WHY THIS EXISTS
// ---------------
// A fault in recompiled code lands in some `ppc_recomp.NNN.cpp` with a host
// backtrace that names guest functions and tells you nothing about *why* — which
// address was touched, which object, which vtable slot. The guest context is sitting
// right there in thread-local storage; without this file nobody reads it.
//
// It arrives here to answer a specific question. Since the vblank pump started
// running guest ISR code and the command processor, roughly 2 runs in 10 segfault
// within 20 seconds (`docs/phase1-notes.md` §5). An intermittent fault is the case
// where a debugger is least useful — you cannot attach to a run that has not crashed
// yet, and gdb on this 109 MB binary takes minutes to load symbols — so the report
// has to come out of the crashing run itself, every time, for free.
//
// The common shape is a null indirect call: XenonRecomp compiles `bctrl` to
// `(PPC_LOOKUP_FUNC(base, ctr))(ctx, base)`, and an address with no recompiled
// function has a null slot — so the process jumps to 0 with `ctr` still holding the
// target. That case is detected explicitly, because it is otherwise
// indistinguishable from a wild jump.
//
// THE GUEST BACKTRACE IS A BACK-CHAIN WALK, NOT A SCAN
// -----------------------------------------------------
// Every prologue in this image saves LR with `stw r12,-8(r1)` before its `stwu`, so
// for a frame with stack pointer SP the return address lives at `[[SP] - 8]` — in
// the CALLER's frame, not at a positive offset from it. That gives an exact chain,
// where the scan this runtime used before (kernel/imports.cpp's stall trace) gives
// plausible-looking addresses that may be stale spill slots.
//
// Frames outside `.text` are FLAGGED rather than silently printed. A walk that has
// gone off the rails must be distinguishable from a genuinely short stack, or an
// unmarked bad walk reads as merely uninformative and quietly wastes the capture.
#include "crash_report.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
// win_compat.h has already pulled windows.h in; DbgHelp is what replaces dladdr.
#include <dbghelp.h>
#include <process.h> // _exit
#else
#include <dlfcn.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "../kernel/guestcall.h"
#include "../kernel/memory.h"
#include "guest_thread.h"

namespace {

std::atomic<int> g_reported{ 0 };

constexpr uint32_t kCodeLo = uint32_t(PPC_CODE_BASE);
constexpr uint32_t kCodeHi = uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE);

// APPEND, CLAMPED — and the reason it exists is a bug that destroyed the process it
// was reporting on.
//
// This file used to build its report with, twenty times over:
//
//     n = Append(b, int(sizeof b), n, ...);
//
// snprintf returns the length it WOULD have written, not what it did. So a report
// longer than the buffer pushes `n` past `sizeof b`, and `sizeof b - n` is then size_t
// arithmetic that underflows to ~2^64 — after which the next call writes off the end of
// a 4 KB stack buffer. On Windows /GS detects the smashed cookie and __fastfail()s: the
// process dies instantly at 0xC0000409, WER buckets it BEX64, and NOTHING is printed —
// no report, and no handler can run because __fastfail bypasses SEH entirely. On Linux
// it is the identical bug with a luckier layout; the Windows report is longer only
// because the module path is.
//
// A crash reporter that can crash while reporting is worse than none: it replaces a
// diagnosable fault with an undiagnosable one. Clamping is the whole fix.
int Append(char* b, int cap, int n, const char* fmt, ...)
{
    if (n < 0 || n >= cap - 1)
        return n;
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(b + n, size_t(cap - n), fmt, ap);
    va_end(ap);
    if (r < 0)
        return n;
    return (r >= cap - n) ? cap - 1 : n + r;   // truncated: park at the end
}

void Emit(const char* buf, size_t n)
{
    // The raw descriptor, not stdio: a fault inside a handler must not depend on a
    // FILE* lock the faulting thread may already hold.
#if defined(_WIN32)
    (void)_write(STDERR_FILENO, buf, unsigned(n));
#else
    ssize_t unused = write(STDERR_FILENO, buf, n);
    (void)unused;
#endif
}

// The back-chain walk, shared by the fault handler and the on-demand dump. Writes
// into `b` and returns the length used.
int FormatGuestBacktrace(char* b, int cap, PPCContext* ctx, const char* prefix)
{
    uint8_t* base = g_memory.base;
    int n = Append(b, cap, 0, "%s  #0 %08X  (lr)\n", prefix, uint32_t(ctx->lr));
    uint32_t sp = ctx->r1.u32;
    for (int i = 1; i < 24 && sp >= 0x10000 && sp < PPC_MEMORY_SIZE - 8 && n < cap - 128; i++)
    {
        const uint32_t prev = PPC_LOAD_U32(sp);
        // A frame pointer must move UP the stack and by a sane amount. Either test
        // failing means the chain is not a chain any more.
        if (prev <= sp || prev - sp > 0x100000 || prev >= PPC_MEMORY_SIZE - 8)
            break;
        const uint32_t savedLr = PPC_LOAD_U32(prev - 8);
        const bool inText = savedLr >= kCodeLo && savedLr < kCodeHi;
        n = Append(b, cap, n, "%s  #%-2d %08X  sp=%08X%s\n", prefix, i, savedLr, prev,
                      inText ? "" : "  <- NOT .text (walk off)");
        if (!inText)
            break;
        sp = prev;
    }
    return n;
}

// THE PORTABLE CORE. It used to take (int, siginfo_t*, void*) and reach into
// ucontext_t for RIP, which made the whole report Linux-shaped for the sake of two
// values. It takes those two values directly now, and each platform's entry point
// below extracts them the way that platform spells it. The ~200 lines of guest-state
// reporting in between are identical on both — which is the point: the report is the
// thing worth having and it was never OS-specific.
//
//   sig        a POSIX signal number, synthesised on Windows from the exception code
//              so the printed report reads the same on both
//   faultAddr  the address the guest touched (nullptr for a deliberate trap)
//   hostPc     the host instruction pointer — the one field that is never stale
void Report(int sig, const void* faultAddr, unsigned long long hostPc)
{
    // One report only: a fault inside the handler must not loop, and other guest
    // threads will usually fault too once the first one has.
    //
    // But say WHERE before going. A silent `_exit` here is indistinguishable from the
    // report simply ending, and that ambiguity is expensive — the output stops
    // mid-line and there is no way to tell a truncated report from a finished one.
    const int depth = g_reported.fetch_add(1);
    if (depth > 1)
        _exit(139); // a fault while reporting the fault in the report — just go
    if (depth != 0)
    {
        char nb[256];
        const int nn = snprintf(nb, sizeof nb,
                                "\n!!! the crash reporter itself faulted: signal %d at %p, "
                                "host pc %016llX\n!!! the report above is TRUNCATED\n",
                                sig, faultAddr, hostPc);
        Emit(nb, nn);
        _exit(139);
    }

    char b[4096];
    const uint8_t* addr = (const uint8_t*)faultAddr;
    int n = Append(b, int(sizeof b), 0, "\n=== guest fault: signal %d at host address %p ===\n", sig,
                     (void*)addr);

    // THE HEADLINE. A host address means nothing to anyone; the guest address is
    // what can be looked up in a capture, in the arena map, or in the image.
    if (g_memory.base && addr >= g_memory.base && addr < g_memory.base + PPC_MEMORY_SIZE)
    {
        const uint32_t guest = uint32_t(addr - g_memory.base);
        const char* where = guest < 0x1000            ? "the NULL page"
                            : guest < 0x40000000u     ? "the 4 KB-page virtual arena"
                            : guest < 0x7FE00000u     ? "the 64 KB-page virtual arena"
                            : guest < 0x82000000u     ? "unmapped space below the image"
                            : guest < 0x82B40000u     ? "the XEX image"
                            : guest < 0x83C26AC8u     ? "the indirect-dispatch table"
                            : guest < 0x88000000u     ? "unmapped space below the user heap"
                            : guest < 0x9FF00000u     ? "the o1heap user arena"
                            : guest < 0xA0000000u     ? "unmapped space below the physical arena"
                            : guest < 0xBFFF0000u     ? "the physical arena (cached view)"
                                                      : "the write-combined/uncached views";
        n = Append(b, int(sizeof b), n, "faulting GUEST address %08X  (%s)\n", guest, where);
    }
    else if (g_ppcContext && g_ppcContext->ctr.u32 == 0)
    {
        // Do NOT say "host-side bug" here. When ctr is zero the process jumps to host
        // address 0 and the faulting address is whatever the dispatch lookup computed
        // from a null base — an arbitrary number outside the guest space, every time.
        // The old unconditional wording named our runtime for a fault that is a guest
        // null function pointer, which is the most expensive kind of wrong a first
        // line can be: it sends the reader into the wrong codebase. See the ctr test
        // below for what it actually is.
        n = Append(b, int(sizeof b), n,
                      "the faulting address is outside the guest space, but ctr is 0 — "
                      "read it as a null indirect call, not as a host bug\n");
    }
    else
    {
        n = Append(b, int(sizeof b), n,
                      "the faulting address is OUTSIDE the 4 GB guest space — this is a "
                      "host-side bug, not a guest one\n");
    }

    // Which thread. With the pump thread now running guest ISR code and the command
    // processor concurrently with the main thread, "who faulted" is the first thing
    // worth knowing about an intermittent crash.
    n = Append(b, int(sizeof b), n, "guest thread id %08X\n",
                  GuestThread::GetCurrentThreadId());

    // The register dump below comes from the thread's PPCContext, which the compiler
    // keeps in host registers inside a recompiled function and does not flush at
    // every instruction — so for the innermost frame it can be stale. The host
    // instruction pointer is not: run
    //     addr2line -f -C -e runtime/build/cz_runtime <host pc>
    // to get the exact ppc_recomp line, which is the authoritative answer to "which
    // guest instruction did this".
    // ALWAYS print the pc; symbolise only when there is something to symbolise. The
    // first version of this guarded both on `hostPc` and so printed nothing at all when
    // RIP was 0 — which is not a missing value but the most informative one available:
    // it says execution jumped to zero, which is exactly the null-indirect-call case
    // this reporter exists to diagnose. A refactor that turns the interesting case into
    // the omitted case is the worst kind.
    n = Append(b, int(sizeof b), n, "host pc %016llX (addr2line this)\n", hostPc);
    if (hostPc)
    {
#if defined(_WIN32)
        // DbgHelp is dladdr's counterpart, called for the same reason and with the same
        // caveat: neither is async-signal-safe, and we are exiting anyway. This line is
        // what turns a bare address into something a bug report can be written about.
        // SymInitialize lazily, so a process that never faults never pays for it.
        static bool symReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* si = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = MAX_SYM_NAME;
        DWORD64 disp = 0;
        char modPath[MAX_PATH] = {};
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)hostPc, &mod);
        if (mod)
            GetModuleFileNameA(mod, modPath, sizeof modPath);
        const bool named =
            symReady && SymFromAddr(GetCurrentProcess(), hostPc, &disp, si) != FALSE;
        n = Append(b, int(sizeof b), n,
                      "  = %s + 0x%llX (log-only; addr2line the RAW pc)%s%s\n",
                      modPath[0] ? modPath : "?",
                      hostPc - (unsigned long long)(uintptr_t)mod,
                      named ? " in " : "", named ? si->Name : "");
#else
        // dladdr is not async-signal-safe, but we are exiting anyway and the register
        // dump is flushed below before anything riskier runs.
        Dl_info di{};
        if (dladdr((void*)hostPc, &di) && di.dli_fname)
            n = Append(b, int(sizeof b), n, "  = %s + 0x%llX (log-only; addr2line the RAW "
                                               "pc)%s%s\n",
                          di.dli_fname, hostPc - (unsigned long long)(uintptr_t)di.dli_fbase,
                          di.dli_sname ? " in " : "", di.dli_sname ? di.dli_sname : "");
#endif
    }

    // Flush the host side on its own: everything below reads through pointers that
    // are, by construction, suspect, and a fault inside a signal handler is immediate
    // death with no output.
    Emit(b, n);
    n = 0;

    PPCContext* ctx = g_ppcContext;
    if (!ctx)
    {
        n = Append(b, int(sizeof b), n,
                      "no guest context on this thread (the fault is in host code)\n"
                      "=== end guest fault ===\n");
        Emit(b, n);
        _exit(139);
    }

    const uint32_t ctr = ctx->ctr.u32;
    n = Append(b, int(sizeof b), n, "lr=%08X ctr=%08X r1(sp)=%08X r13(pcr)=%08X\n",
                  uint32_t(ctx->lr), ctr, ctx->r1.u32, ctx->r13.u32);

    // An indirect call that could not go anywhere. Two distinct shapes, and the
    // original version of this test only recognised the second:
    //
    //  * ctr == 0 — the guest loaded a function pointer that was never written. The
    //    dispatch-table lookup is not even reached; the process jumps to host 0 and
    //    si_addr is whatever the lookup happened to compute, NOT null. Missing this
    //    case cost a session: the report read as an ordinary segfault at a strange
    //    address and said nothing about the `bctrl` two instructions above it.
    //  * ctr inside the image but absent from the dispatch table — the pointer is a
    //    plausible guest address that was never recompiled, or a corrupt vtable slot.
    //    Here PPC_LOOKUP_FUNC yields a null slot and si_addr IS null.
    //
    // Anything else in ctr (a small non-zero value, a heap address) is worth printing
    // too: it means the pointer was overwritten rather than left unset.
    if (ctr == 0)
    {
        n = Append(b, int(sizeof b), n,
                      "LIKELY null indirect call: ctr is ZERO — the guest called "
                      "through a function pointer that was never written (an object "
                      "whose vtable/callback slot is still 0). The `bctrl` is at the "
                      "guest address just before lr=%08X.\n",
                      uint32_t(ctx->lr));
    }
    else if (faultAddr == nullptr && ctr >= uint32_t(PPC_IMAGE_BASE) &&
             ctr < uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE))
    {
        const bool present = g_memory.FindFunction(ctr) != nullptr;
        n = Append(b, int(sizeof b), n, "LIKELY null indirect call: bctrl target %08X %s\n",
                      ctr,
                      present ? "IS in the dispatch table (so this is not it)"
                              : "is NOT in the dispatch table — unrecompiled, or a bad "
                                "vtable slot");
    }
    else if (faultAddr == nullptr)
    {
        n = Append(b, int(sizeof b), n,
                      "LIKELY indirect call through a CORRUPT pointer: ctr=%08X is "
                      "outside the image (%08X..%08X), so it is not an unrecompiled "
                      "function — it is not a code address at all.\n",
                      ctr, uint32_t(PPC_IMAGE_BASE),
                      uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE));
    }

    // A fault ON 0xC0000002 or just past it is the signature of an unimplemented
    // import being asked for a pointer rather than an NTSTATUS — gotcha 42, written
    // down precisely so it is recognisable, and worth saying out loud here rather
    // than leaving the reader to remember it.
    if (addr && g_memory.base)
    {
        const uint64_t guest = uint64_t(addr - g_memory.base);
        if (guest >= 0xC0000002ull && guest < 0xC0001002ull)
            n = Append(b, int(sizeof b), n,
                          "NOTE: this address is at/just past 0xC0000002 = "
                          "STATUS_NOT_IMPLEMENTED. That is what an unimplemented import "
                          "returning a status where the guest wanted a POINTER looks like "
                          "(gotcha 42). Check the last [kcall] in the log.\n");
    }

    // PPCContext declares the registers as named members (r3 first, for the host
    // ABI), not an array, so list them.
    const uint32_t gpr[32] = {
        ctx->r0.u32,  ctx->r1.u32,  ctx->r2.u32,  ctx->r3.u32,  ctx->r4.u32,  ctx->r5.u32,
        ctx->r6.u32,  ctx->r7.u32,  ctx->r8.u32,  ctx->r9.u32,  ctx->r10.u32, ctx->r11.u32,
        ctx->r12.u32, ctx->r13.u32, ctx->r14.u32, ctx->r15.u32, ctx->r16.u32, ctx->r17.u32,
        ctx->r18.u32, ctx->r19.u32, ctx->r20.u32, ctx->r21.u32, ctx->r22.u32, ctx->r23.u32,
        ctx->r24.u32, ctx->r25.u32, ctx->r26.u32, ctx->r27.u32, ctx->r28.u32, ctx->r29.u32,
        ctx->r30.u32, ctx->r31.u32,
    };
    for (int i = 0; i < 32; i += 4)
        n = Append(b, int(sizeof b), n, "r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i,
                      gpr[i], i + 1, gpr[i + 1], i + 2, gpr[i + 2], i + 3, gpr[i + 3]);

    // Emit what we have before touching guest memory: losing the register dump to a
    // bad `this` pointer is exactly the case worth surviving.
    Emit(b, n);
    n = 0;

    n = Append(b, int(sizeof b), n, "guest backtrace (lr first):\n");
    n += FormatGuestBacktrace(b + n, int(sizeof b) - n, ctx, "");
    Emit(b, n);
    n = 0;

    // r3/r4 usually hold `this` and the argument object; the first word of an object
    // is its vtable pointer, and a garbage one is the usual reason a vtable dispatch
    // goes nowhere.
    auto dumpObject = [&](const char* what, uint32_t ea) {
        if (ea < 0x1000 || ea >= PPC_MEMORY_SIZE - 32)
            return;
        uint8_t* base = g_memory.base;
        n = Append(b, int(sizeof b), n, "%s %08X:", what, ea);
        for (int i = 0; i < 8; i++)
            n = Append(b, int(sizeof b), n, " %08X", PPC_LOAD_U32(ea + 4 * i));
        n = Append(b, int(sizeof b), n, "\n");
    };
    dumpObject("[r3] ", ctx->r3.u32);
    dumpObject("[r4] ", ctx->r4.u32);
    dumpObject("[r11]", ctx->r11.u32);

    n = Append(b, int(sizeof b), n, "=== end guest fault ===\n");
    Emit(b, n);
    _exit(139);
}

#if defined(_WIN32)
// THE WINDOWS ENTRY POINT — an UNHANDLED-exception filter, not a vectored handler.
//
// The first version used AddVectoredExceptionHandler(1, …) on the reasoning that the
// vectored chain runs before any frame-based __try/__except, so the guest state would
// be seen before anything unwound. That reasoning is right and the choice was still
// wrong, because vectored handlers see FIRST-CHANCE exceptions: every exception in the
// process, including the ones Windows raises routinely and expects somebody downstream
// to handle. The runtime reached the title screen with working sound, then a benign
// first-chance access violation on some thread reached this handler, which faithfully
// reported it as a fatal guest fault and killed a healthy process.
//
// SetUnhandledExceptionFilter fires only when nothing in the chain handled it, which is
// the definition of the case worth reporting. It costs nothing that matters: the filter
// runs BEFORE unwinding, with an EXCEPTION_POINTERS whose ContextRecord is the state at
// the fault — and the guest registers live in a thread-local PPCContext that SEH never
// touches anyway. The premise that made vectored look necessary was simply false.
//
// CZ_WIN_FIRSTCHANCE=1 adds a vectored handler that only LOGS, so the traffic this used
// to act on can be inspected without acting on it again.
LONG CALLBACK FirstChanceLogger(EXCEPTION_POINTERS* ep)
{
    char b[160];
    const int n = snprintf(b, sizeof b, "[seh] first-chance %08lX at %p (pc %016llX)\n",
                           (unsigned long)ep->ExceptionRecord->ExceptionCode,
                           ep->ExceptionRecord->ExceptionAddress,
                           (unsigned long long)ep->ContextRecord->Rip);
    Emit(b, n);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK UnhandledFilter(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    int sig;
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:      sig = SIGSEGV; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: sig = SIGBUS;  break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:   sig = SIGILL;  break;  // __builtin_trap
    case EXCEPTION_BREAKPOINT:            sig = SIGTRAP; break;  // __builtin_debugtrap
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    sig = SIGFPE;  break;
    default:
        // Report it anyway rather than dying silently — by the time an unhandled filter
        // runs, the process is going down regardless, and a report naming an exception
        // code we do not have a signal for beats no report at all. The code is printed
        // below so an unfamiliar one is identifiable.
        sig = 0;
        break;
    }
    {
        char b[128];
        const int n = snprintf(b, sizeof b, "=== windows exception %08lX ===\n",
                               (unsigned long)code);
        Emit(b, n);
    }
    // ExceptionInformation[1] is the address touched, and only for an access violation;
    // for a trap there is no faulting address and nullptr is the honest answer — which
    // is also what the report's "deliberate trap" branch keys on.
    const void* addr = (code == EXCEPTION_ACCESS_VIOLATION &&
                        ep->ExceptionRecord->NumberParameters >= 2)
                           ? (const void*)ep->ExceptionRecord->ExceptionInformation[1]
                           : nullptr;
    Report(sig, addr, (unsigned long long)ep->ContextRecord->Rip);
    return EXCEPTION_EXECUTE_HANDLER; // unreachable: Report() exits
}
#else
void Handler(int sig, siginfo_t* info, void* ucontext)
{
    unsigned long long pc = 0;
#if defined(__x86_64__)
    if (ucontext)
        pc = (unsigned long long)((const ucontext_t*)ucontext)->uc_mcontext.gregs[REG_RIP];
#endif
    Report(sig, info ? info->si_addr : nullptr, pc);
}
#endif

} // namespace

extern "C" void CzInstallCrashReporter()
{
#if defined(_WIN32)
    SetUnhandledExceptionFilter(UnhandledFilter);
    if (const char* fc = getenv("CZ_WIN_FIRSTCHANCE"); fc && *fc != '0')
    {
        fprintf(stderr, "[seh] CZ_WIN_FIRSTCHANCE=%s — logging every first-chance "
                        "exception. This is a DIAGNOSTIC: the reporter does not act on "
                        "them, because acting on them is what killed a healthy process "
                        "the first time.\n", fc);
        AddVectoredExceptionHandler(1, FirstChanceLogger);
    }
#else
    struct sigaction sa{};
    sa.sa_sigaction = Handler;
    // SA_NODEFER is load-bearing, not decoration. Without it the kernel blocks SIGSEGV
    // for the duration of the handler, so a fault raised *inside* the report cannot be
    // delivered to this handler: the default action fires instead and the process is
    // killed on the spot with no output at all. The report simply stops mid-line and
    // looks like it finished. With it, the nested fault re-enters, the depth counter
    // above catches it, and it prints the address it died at.
    //
    // Windows needs no equivalent: a vectored handler is re-entered by construction,
    // and the same depth counter catches it.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    // SIGILL and SIGTRAP are deliberate traps, not accidents: `__builtin_trap`
    // (ud2 -> SIGILL) is how a recompiled switch fails honestly, and
    // `__builtin_debugtrap` (int3 -> SIGTRAP) is what the recompiler emits for an
    // instruction it cannot translate. Both would otherwise kill the process with no
    // report at all, which is the one thing a deliberate trap must not do — the whole
    // point of trapping is to be told where.
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
#endif
}

// The same guest stack walk, on demand and without dying.
//
// Needed because a boot that stops crashing starts *spinning*, and "which guest
// function is this thread sleeping in" is not a question a host backtrace answers:
// recompiled frames get inlined and tail-called under -O2, so gdb attributes them to
// whatever function the code was folded into. The guest's own LR chain is exact.
extern "C" void CzDumpGuestBacktrace(const char* label)
{
    PPCContext* ctx = g_ppcContext;
    if (!ctx)
    {
        fprintf(stderr, "[stall] %s: no guest context on this thread\n", label);
        return;
    }

    char b[2048];
    int n = Append(b, int(sizeof b), 0, "[stall] %s: tid=%08X lr=%08X ctr=%08X r1=%08X r3=%08X\n",
                     label, GuestThread::GetCurrentThreadId(), uint32_t(ctx->lr), ctx->ctr.u32,
                     ctx->r1.u32, ctx->r3.u32);
    n += FormatGuestBacktrace(b + n, int(sizeof b) - n, ctx, "[stall]");
    Emit(b, n);
}
