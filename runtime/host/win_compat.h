#pragma once
// Windows portability: the legacy macros that collide with this runtime's own names,
// and the POSIX functions it uses that the MSVC CRT spells differently.
//
// WHY THIS FILE EXISTS. `windows.h` is not a header so much as a namespace with no
// walls. It defines, as bare preprocessor macros, a large set of short words that any
// C++ program might reasonably use — and this runtime uses several of them for GUEST
// kernel values, because the Xbox 360 kernel inherited the same Win32 error names. The
// results are diagnostics that point at our line and explain nothing:
//
//   constexpr uint32_t ERROR_NO_MORE_FILES = 18;   ->  expected unqualified-id
//   constexpr uint32_t E_FAIL = 0x80004005;        ->  reference to 'HRESULT' is
//                                                      ambiguous   (E_FAIL expands
//                                                      through _HRESULT_TYPEDEF_)
//   const bool far = ...;                          ->  expected unqualified-id
//                                                      (`far` is still #defined, from
//                                                      the 16-bit memory model)
//
// None of those messages contains the word "macro", and none names the header that did
// it. That is what made this class expensive to diagnose: the compiler blames the
// victim. `-E` and `note: previous definition is here` are what actually answer it.
//
// THE ORDER MATTERS AND IT IS WHY THIS INCLUDES windows.h ITSELF. Undefining a macro
// before the header that defines it has no effect. Pulling windows.h in here, first,
// means its include guard makes every later inclusion a no-op — so these #undefs are
// final and cannot be quietly re-defined by whichever transitive include would
// otherwise have got there first.
//
// What is NOT undefined here: the MEM_* allocation flags. kernel/memory.cpp uses the
// real Win32 ones, so where the guest needed its own the constant was renamed
// (kGuestMemReserve, kGuestMemRelease…) rather than the host's spelling taken away.
// Which of the two to do is decided by whether anything on this side wants the Win32
// meaning; taking away a name someone legitimately uses is how a fix becomes a defect.

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <ctime>

// Guest kernel error codes. These are OUR names for values the title returns upward;
// the Win32 macros of the same name hold the same numbers, and we want the typed
// constant rather than the macro.
#undef ERROR_NO_MORE_FILES
#undef ERROR_INVALID_PARAMETER
#undef ERROR_FILE_NOT_FOUND
#undef ERROR_ALREADY_EXISTS
#undef ERROR_NO_SUCH_USER
#undef ERROR_INSUFFICIENT_BUFFER
#undef ERROR_IO_PENDING
#undef ERROR_DEVICE_NOT_CONNECTED
#undef ERROR_EMPTY
#undef ERROR_FUNCTION_FAILED
#undef E_FAIL

// 16-bit memory model residue. Nothing has needed these since 1995 and they turn any
// variable so named into a syntax error.
#undef far
#undef near

// POSIX spellings the MSVC CRT provides under different names. Not emulation — these
// are the same functions.
#define fseeko _fseeki64
#define ftello _ftelli64
using off_t = long long;

// clock_gettime. The CRT has no such function; QueryPerformanceCounter is the
// monotonic clock on Windows and GetThreadTimes is the per-thread CPU one. Both are
// wrapped rather than substituted at the call sites, so the callers stay identical on
// both platforms and there is one place to be wrong.
#define CLOCK_MONOTONIC 1
#define CLOCK_THREAD_CPUTIME_ID 3

inline int clock_gettime(int clk, struct timespec* ts)
{
    if (clk == CLOCK_THREAD_CPUTIME_ID)
    {
        FILETIME c, e, kern, user;
        if (!GetThreadTimes(GetCurrentThread(), &c, &e, &kern, &user))
            return -1;
        // Kernel + user, in 100 ns units, which is what the POSIX call reports for
        // this clock. Summing both matters: our profiler reads this to attribute
        // thread CPU time, and user-only would under-report every syscall-heavy phase.
        const unsigned long long t =
            ((unsigned long long(kern.dwHighDateTime) << 32) | kern.dwLowDateTime) +
            ((unsigned long long(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        ts->tv_sec = time_t(t / 10000000ULL);
        ts->tv_nsec = long((t % 10000000ULL) * 100);
        return 0;
    }
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    ts->tv_sec = time_t(n.QuadPart / f.QuadPart);
    ts->tv_nsec = long((double(n.QuadPart % f.QuadPart) / double(f.QuadPart)) * 1e9);
    return 0;
}

#endif // _WIN32
