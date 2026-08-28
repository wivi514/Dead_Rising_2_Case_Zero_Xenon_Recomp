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

#include <cstddef> // ptrdiff_t, for the ssize_t alias
#include <io.h>    // _write, which is what `write` becomes below
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

// SDL2 carries a workaround for a Clang 11-era conflict between its own
// `_m_prefetchw` and winnt.h's, and defines `_m_prefetch` itself unless
// __PRFCHWINTRIN_H says the real intrinsic header has been seen. Clang has provided
// `_m_prefetch` as a builtin for years, so the workaround now IS the conflict:
// "definition of builtin function '_m_prefetch'". Claiming the guard is how SDL's own
// header asks to be told, and is preferable to patching a dependency's source.
#ifndef __PRFCHWINTRIN_H
#define __PRFCHWINTRIN_H
#endif

// Signal numbers Windows does not define. The VALUES are Linux's on purpose: the crash
// report prints the number, and a Windows report and a Linux report should be readable
// by the same eyes without a translation table. Nothing dispatches on them — the
// Windows handler switches on the EXCEPTION_* code and only labels the result.
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif

// POSIX's raw stderr descriptor. The crash reporter writes through the fd rather than
// stdio because a fault inside a handler must not depend on a FILE* lock that the
// faulting thread may already hold — the same reason it does on POSIX.
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

// NOT `#define write _write`. That is precisely the mistake this header exists to
// clean up after: `write` is a short, common identifier, and defining it as a macro
// rewrote `std::ostream::write(...)` into `std::ostream::_write(...)` in an unrelated
// file, producing a link error naming a std method that does not exist. The one call
// site that needs it forks explicitly instead — three lines there, no macro here.

// gettid(). Linux-only; the Windows spelling reports the same thing — an OS-level
// thread id, used only for log lines that correlate our threads with a profiler's.
inline unsigned int gettid() { return GetCurrentThreadId(); }

// 16-bit memory model residue. Nothing has needed these since 1995 and they turn any
// variable so named into a syntax error.
#undef far
#undef near

// POSIX spellings the MSVC CRT provides under different names. Not emulation — these
// are the same functions.
//
// `off_t` is deliberately NOT aliased here. The UCRT already defines it, as `long`,
// i.e. 32 BITS — so an alias collides ("type alias redefinition with different types")
// and accepting theirs would silently truncate any offset past 2 GB. The call sites
// use int64_t instead, which is what _fseeki64 takes and what the guest means.
#define fseeko _fseeki64
#define ftello _ftelli64

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
        // static_cast, not `unsigned long long(x)`: a functional cast cannot take a
        // multi-word type name, so that spelling is a syntax error. It was invisible
        // on Linux because this whole file is behind #if defined(_WIN32) and is never
        // compiled there — a platform-guarded block gets ZERO validation from the
        // other platform's build, however green it is.
        const unsigned long long t =
            ((static_cast<unsigned long long>(kern.dwHighDateTime) << 32) |
             kern.dwLowDateTime) +
            ((static_cast<unsigned long long>(user.dwHighDateTime) << 32) |
             user.dwLowDateTime);
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
