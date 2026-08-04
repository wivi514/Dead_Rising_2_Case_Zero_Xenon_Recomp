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
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <ucontext.h>
#include <unistd.h>

#include "../kernel/guestcall.h"
#include "../kernel/memory.h"
#include "guest_thread.h"

namespace {

std::atomic<int> g_reported{ 0 };

constexpr uint32_t kCodeLo = uint32_t(PPC_CODE_BASE);
constexpr uint32_t kCodeHi = uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE);

void Emit(const char* buf, size_t n)
{
    ssize_t unused = write(STDERR_FILENO, buf, n);
    (void)unused;
}

// The back-chain walk, shared by the fault handler and the on-demand dump. Writes
// into `b` and returns the length used.
int FormatGuestBacktrace(char* b, int cap, PPCContext* ctx, const char* prefix)
{
    uint8_t* base = g_memory.base;
    int n = snprintf(b, cap, "%s  #0 %08X  (lr)\n", prefix, uint32_t(ctx->lr));
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
        n += snprintf(b + n, cap - n, "%s  #%-2d %08X  sp=%08X%s\n", prefix, i, savedLr, prev,
                      inText ? "" : "  <- NOT .text (walk off)");
        if (!inText)
            break;
        sp = prev;
    }
    return n;
}

void Handler(int sig, siginfo_t* info, void* ucontext)
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
        unsigned long long npc = 0;
#if defined(__x86_64__)
        if (ucontext)
            npc = (unsigned long long)((const ucontext_t*)ucontext)->uc_mcontext.gregs[REG_RIP];
#endif
        const int nn = snprintf(nb, sizeof nb,
                                "\n!!! the crash reporter itself faulted: signal %d at %p, "
                                "host pc %016llX\n!!! the report above is TRUNCATED\n",
                                sig, info ? info->si_addr : nullptr, npc);
        Emit(nb, nn);
        _exit(139);
    }

    char b[4096];
    const uint8_t* addr = info ? (const uint8_t*)info->si_addr : nullptr;
    int n = snprintf(b, sizeof b, "\n=== guest fault: signal %d at host address %p ===\n", sig,
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
        n += snprintf(b + n, sizeof b - n, "faulting GUEST address %08X  (%s)\n", guest, where);
    }
    else
    {
        n += snprintf(b + n, sizeof b - n,
                      "the faulting address is OUTSIDE the 4 GB guest space — this is a "
                      "host-side bug, not a guest one\n");
    }

    // Which thread. With the pump thread now running guest ISR code and the command
    // processor concurrently with the main thread, "who faulted" is the first thing
    // worth knowing about an intermittent crash.
    n += snprintf(b + n, sizeof b - n, "guest thread id %08X\n",
                  GuestThread::GetCurrentThreadId());

    // The register dump below comes from the thread's PPCContext, which the compiler
    // keeps in host registers inside a recompiled function and does not flush at
    // every instruction — so for the innermost frame it can be stale. The host
    // instruction pointer is not: run
    //     addr2line -f -C -e runtime/build/cz_runtime <host pc>
    // to get the exact ppc_recomp line, which is the authoritative answer to "which
    // guest instruction did this".
#if defined(__x86_64__)
    if (ucontext)
    {
        const auto* uc = (const ucontext_t*)ucontext;
        const unsigned long long pc = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
        n += snprintf(b + n, sizeof b - n, "host pc %016llX (addr2line this)\n", pc);
        // dladdr is not async-signal-safe, but we are exiting anyway and the register
        // dump is flushed below before anything riskier runs.
        Dl_info di{};
        if (dladdr((void*)pc, &di) && di.dli_fname)
            n += snprintf(b + n, sizeof b - n, "  = %s + 0x%llX (log-only; addr2line the RAW "
                                               "pc)%s%s\n",
                          di.dli_fname, pc - (unsigned long long)(uintptr_t)di.dli_fbase,
                          di.dli_sname ? " in " : "", di.dli_sname ? di.dli_sname : "");
    }
#endif

    // Flush the host side on its own: everything below reads through pointers that
    // are, by construction, suspect, and a fault inside a signal handler is immediate
    // death with no output.
    Emit(b, n);
    n = 0;

    PPCContext* ctx = g_ppcContext;
    if (!ctx)
    {
        n += snprintf(b + n, sizeof b - n,
                      "no guest context on this thread (the fault is in host code)\n"
                      "=== end guest fault ===\n");
        Emit(b, n);
        _exit(139);
    }

    const uint32_t ctr = ctx->ctr.u32;
    n += snprintf(b + n, sizeof b - n, "lr=%08X ctr=%08X r1(sp)=%08X r13(pcr)=%08X\n",
                  uint32_t(ctx->lr), ctr, ctx->r1.u32, ctx->r13.u32);

    // A null si_addr with a plausible guest ctr means the `bctrl` target had no entry
    // in the indirect-dispatch table — the jump went to 0, not to the guest.
    if (info && info->si_addr == nullptr && ctr >= uint32_t(PPC_IMAGE_BASE) &&
        ctr < uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE))
    {
        const bool present = g_memory.FindFunction(ctr) != nullptr;
        n += snprintf(b + n, sizeof b - n, "LIKELY null indirect call: bctrl target %08X %s\n",
                      ctr,
                      present ? "IS in the dispatch table (so this is not it)"
                              : "is NOT in the dispatch table — unrecompiled, or a bad "
                                "vtable slot");
    }

    // A fault ON 0xC0000002 or just past it is the signature of an unimplemented
    // import being asked for a pointer rather than an NTSTATUS — gotcha 42, written
    // down precisely so it is recognisable, and worth saying out loud here rather
    // than leaving the reader to remember it.
    if (addr && g_memory.base)
    {
        const uint64_t guest = uint64_t(addr - g_memory.base);
        if (guest >= 0xC0000002ull && guest < 0xC0001002ull)
            n += snprintf(b + n, sizeof b - n,
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
        n += snprintf(b + n, sizeof b - n, "r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i,
                      gpr[i], i + 1, gpr[i + 1], i + 2, gpr[i + 2], i + 3, gpr[i + 3]);

    // Emit what we have before touching guest memory: losing the register dump to a
    // bad `this` pointer is exactly the case worth surviving.
    Emit(b, n);
    n = 0;

    n += snprintf(b + n, sizeof b - n, "guest backtrace (lr first):\n");
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
        n += snprintf(b + n, sizeof b - n, "%s %08X:", what, ea);
        for (int i = 0; i < 8; i++)
            n += snprintf(b + n, sizeof b - n, " %08X", PPC_LOAD_U32(ea + 4 * i));
        n += snprintf(b + n, sizeof b - n, "\n");
    };
    dumpObject("[r3] ", ctx->r3.u32);
    dumpObject("[r4] ", ctx->r4.u32);
    dumpObject("[r11]", ctx->r11.u32);

    n += snprintf(b + n, sizeof b - n, "=== end guest fault ===\n");
    Emit(b, n);
    _exit(139);
}

} // namespace

extern "C" void CzInstallCrashReporter()
{
    struct sigaction sa{};
    sa.sa_sigaction = Handler;
    // SA_NODEFER is load-bearing, not decoration. Without it the kernel blocks SIGSEGV
    // for the duration of the handler, so a fault raised *inside* the report cannot be
    // delivered to this handler: the default action fires instead and the process is
    // killed on the spot with no output at all. The report simply stops mid-line and
    // looks like it finished. With it, the nested fault re-enters, the depth counter
    // above catches it, and it prints the address it died at.
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
    int n = snprintf(b, sizeof b, "[stall] %s: tid=%08X lr=%08X ctr=%08X r1=%08X r3=%08X\n",
                     label, GuestThread::GetCurrentThreadId(), uint32_t(ctx->lr), ctx->ctr.u32,
                     ctx->r1.u32, ctx->r3.u32);
    n += FormatGuestBacktrace(b + n, int(sizeof b) - n, ctx, "[stall]");
    Emit(b, n);
}
