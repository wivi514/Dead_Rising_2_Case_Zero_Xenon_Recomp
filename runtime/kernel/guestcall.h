// Bridges kernel imports to host C++ functions with natural signatures.
//
// Xenon calling convention (what the recompiled code expects at a call site):
//   - integer/pointer args in r3..r10, one 64-bit GPR per arg; the 9th onward spill
//     to the caller's parameter area at r1+0x54 (8-byte slots).
//   - float/double args in f1..f13 by *float ordinal*; a float arg still consumes
//     its GPR/stack slot position.
//   - integer/pointer return in r3, float return in f1.
//
// GUEST_FUNCTION_HOOK(__imp__NtClose, NtClose_x) defines the PPC_FUNC for the
// import and marshals: guest pointers arrive as host pointers (base + u32), pointer
// returns are mapped back to guest addresses. Big-endian struct fields are handled
// by declaring the parameter types with be<>/xpointer<> from xbox.h.
//
// This is also where the out-parameter rule becomes enforceable. A generated stub
// cannot zero-fill an out-parameter because it has no signature (see
// tools/gen_import_stubs.py); a hook declared here has the out-parameter as a typed
// argument, which is the whole reason "give it a real signature" is the escalation
// path rather than "make the stub smarter".
//
// Every hook also emits the kernel-call trace (kernel/klog.h) — one line the first
// time each import is called, which is the sequence the phase gates diff against
// Xenia's. Cost is one relaxed atomic increment per kernel call.
#pragma once

#include <array>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include <xbox.h>

#include "klog.h"
#include "memory.h"
#include "ppc_recomp_shared.h"

// Each host thread running guest code keeps its context reachable for imports that
// need registers beyond the marshalled args (e.g. r13 as the per-thread PCR
// address, which is how we identify the calling guest thread in lock code).
inline thread_local PPCContext* g_ppcContext;

namespace guestcall {

inline uint64_t GetGpr(const PPCContext& ctx, uint8_t* base, size_t pos) noexcept
{
    switch (pos)
    {
        case 0: return ctx.r3.u64;
        case 1: return ctx.r4.u64;
        case 2: return ctx.r5.u64;
        case 3: return ctx.r6.u64;
        case 4: return ctx.r7.u64;
        case 5: return ctx.r8.u64;
        case 6: return ctx.r9.u64;
        case 7: return ctx.r10.u64;
        default:
            return *reinterpret_cast<be<uint32_t>*>(base + ctx.r1.u32 + 0x54 + (pos - 8) * 8);
    }
}

inline double GetFpr(const PPCContext& ctx, size_t ord) noexcept
{
    switch (ord)
    {
        case 0: return ctx.f1.f64;
        case 1: return ctx.f2.f64;
        case 2: return ctx.f3.f64;
        case 3: return ctx.f4.f64;
        case 4: return ctx.f5.f64;
        case 5: return ctx.f6.f64;
        case 6: return ctx.f7.f64;
        case 7: return ctx.f8.f64;
        case 8: return ctx.f9.f64;
        case 9: return ctx.f10.f64;
        case 10: return ctx.f11.f64;
        case 11: return ctx.f12.f64;
        default: return ctx.f13.f64;
    }
}

template<typename T>
T FetchArg(PPCContext& ctx, uint8_t* base, size_t pos, size_t fltOrd) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        return static_cast<T>(GetFpr(ctx, fltOrd));
    }
    else if constexpr (std::is_pointer_v<T>)
    {
        uint32_t guest = static_cast<uint32_t>(GetGpr(ctx, base, pos));
        return guest ? reinterpret_cast<T>(base + guest) : nullptr;
    }
    else if constexpr (sizeof(T) == 8)
    {
        return static_cast<T>(GetGpr(ctx, base, pos));
    }
    else
    {
        return static_cast<T>(static_cast<uint32_t>(GetGpr(ctx, base, pos)));
    }
}

// Float ordinal of each argument position (floats are numbered separately from GPR
// slots, but still consume a GPR slot position).
template<typename... Args>
constexpr auto FloatOrdinals()
{
    std::array<size_t, sizeof...(Args) + 1> ords{};
    size_t flt = 0, i = 0;
    ((ords[i++] = std::is_floating_point_v<Args> ? flt++ : 0), ...);
    return ords;
}

template<typename R, typename... Args, size_t... I>
void DispatchImpl(R (*func)(Args...), PPCContext& ctx, uint8_t* base,
                  std::index_sequence<I...>)
{
    constexpr auto fltOrds = FloatOrdinals<Args...>();
    // Fetch before calling: the call may clobber r3 via nested guest calls.
    [[maybe_unused]] std::tuple<Args...> args{ FetchArg<Args>(ctx, base, I, fltOrds[I])... };

    if constexpr (std::is_void_v<R>)
    {
        std::apply(func, args);
    }
    else
    {
        R v = std::apply(func, args);
        if constexpr (std::is_pointer_v<R>)
            ctx.r3.u64 = v ? g_memory.MapVirtual(v) : 0;
        else if constexpr (std::is_floating_point_v<R>)
            ctx.f1.f64 = static_cast<double>(v);
        else
            ctx.r3.u64 = static_cast<uint64_t>(v);
    }
}

template<typename R, typename... Args>
constexpr std::tuple<Args...> guestcall_args(R (*)(Args...))
{
    return {};
}

template<auto Func>
void Dispatch(PPCContext& ctx, uint8_t* base)
{
    g_ppcContext = &ctx;
    DispatchImpl(Func, ctx, base,
                 std::make_index_sequence<std::tuple_size_v<decltype(guestcall_args(Func))>>{});
}

} // namespace guestcall

#define GUEST_FUNCTION_HOOK(guest, host) \
    PPC_FUNC(guest) { KCALL(#guest); guestcall::Dispatch<host>(ctx, base); }

// A deliberate no-op that returns 0. Reserved for imports where doing nothing IS
// the faithful behaviour (cache hints, IRQL bookkeeping) — never for "we have not
// written this yet", which is what the generated honest-failure stubs are for.
// Every use of this macro needs a comment saying why nothing is the right answer.
#define GUEST_FUNCTION_STUB(guest) \
    PPC_FUNC(guest) { KCALL(#guest); ctx.r3.u64 = 0; }
