// The stderr tee. See log_file.h for why it is a descriptor-level pipe and not a
// stdio hook, and for the one cost (bytes in flight) that Flush() exists to cover.
#include "log_file.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace
{
std::filesystem::path g_path;
std::thread g_thread;
std::atomic<bool> g_live{ false };
// Bytes taken out of the pipe by the tee thread, and bytes it has finished writing
// to the file. Flush() is "the pipe is empty AND these two agree": the first alone
// leaves a window between the read and the file write, which is exactly the window
// a crash report's last line would fall into.
std::atomic<unsigned long long> g_read{ 0 }, g_written{ 0 };
int g_pipeRead = -1;   // the tee thread's end
int g_origErr = -1;    // a dup of the stderr we started with (-1: there was none)
int g_file = -1;
#if defined(_WIN32)
HANDLE g_origStdHandle = INVALID_HANDLE_VALUE;
#endif

#if defined(_WIN32)
int SysRead(int fd, void* b, unsigned n) { return _read(fd, b, n); }
int SysWrite(int fd, const void* b, unsigned n) { return _write(fd, b, n); }
void SysClose(int fd) { _close(fd); }
#else
int SysRead(int fd, void* b, unsigned n) { return int(::read(fd, b, n)); }
int SysWrite(int fd, const void* b, unsigned n) { return int(::write(fd, b, n)); }
void SysClose(int fd) { ::close(fd); }
#endif

// Write ALL of a chunk, retrying short writes and EINTR. A terminal that is slow (or
// a pipe to `less`) short-writes; dropping the remainder would lose log lines in the
// console copy, which is the one a developer is reading.
void WriteAll(int fd, const char* p, int n)
{
    while (n > 0)
    {
        const int w = SysWrite(fd, p, unsigned(n));
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return; // a closed console; give up on this copy, keep the other
        }
        p += w;
        n -= w;
    }
}

void TeeLoop()
{
    char buf[65536];
    for (;;)
    {
        const int n = SysRead(g_pipeRead, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break; // the write end is closed: End() ran, or the process is going
        g_read += unsigned(n);
        if (g_origErr >= 0)
            WriteAll(g_origErr, buf, n);
        if (g_file >= 0)
            WriteAll(g_file, buf, n);
        g_written += unsigned(n);
    }
}

unsigned PipeBytesPending()
{
#if defined(_WIN32)
    DWORD avail = 0;
    HANDLE h = HANDLE(_get_osfhandle(g_pipeRead));
    if (h == INVALID_HANDLE_VALUE || !PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
        return 0;
    return unsigned(avail);
#else
    int avail = 0;
    if (ioctl(g_pipeRead, FIONREAD, &avail) != 0)
        return 0;
    return avail > 0 ? unsigned(avail) : 0u;
#endif
}

int OpenLog(const std::filesystem::path& p)
{
#if defined(_WIN32)
    // TEXT mode on purpose: the pipe carries bare LF (it is binary, and the CRT's own
    // stderr writes are untranslated into it), and the console/redirect copy goes out
    // through the ORIGINAL fd 2, which the CRT opened in text mode and so writes CRLF.
    // A binary log here read 64 bytes short of a 64-line console copy on the first
    // czwin run — one LF per line. Text mode makes the two copies byte-identical and
    // gives a Windows player a log Notepad wraps correctly.
    return _wopen(p.wstring().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_TEXT | _O_NOINHERIT,
                  _S_IREAD | _S_IWRITE);
#else
    return ::open(p.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
}

// <name> -> <name>.1, replacing an older .1. One generation is the deliberate choice:
// a player who launches three times and reports the third has the second to compare
// against, and nobody has to explain a directory of numbered logs.
void Rotate(const std::filesystem::path& p)
{
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return;
    std::filesystem::path old = p;
    old += ".1";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(p, old, ec);
}
} // namespace

namespace LogFile
{
bool Begin(const std::filesystem::path& dir, const char* name)
{
    if (g_live)
        return true;
    if (const char* off = std::getenv("CZ_NO_LOG_FILE"); off && *off && std::strcmp(off, "0") != 0)
    {
        std::fprintf(stderr, "[log] CZ_NO_LOG_FILE is set — no log file this run "
                             "(stderr only)\n");
        return false;
    }

    // Where. The override first; else the caller's directory; else the temp dir, so
    // a read-only install (a /opt tree, Program Files) still produces a file
    // SOMEWHERE and the line below says where.
    std::filesystem::path chosen;
    int fd = -1;
    if (const char* env = std::getenv("CZ_LOG_FILE"); env && *env)
    {
        chosen = env;
        Rotate(chosen);
        fd = OpenLog(chosen);
        if (fd < 0)
            std::fprintf(stderr, "[log] CZ_LOG_FILE=%s cannot be opened (%s) — trying the "
                                 "default location\n", env, std::strerror(errno));
    }
    if (fd < 0)
    {
        chosen = dir / name;
        Rotate(chosen);
        fd = OpenLog(chosen);
    }
    if (fd < 0)
    {
        std::error_code ec;
        const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
        if (!ec)
        {
            const int firstErr = errno;
            chosen = tmp / name;
            Rotate(chosen);
            fd = OpenLog(chosen);
            if (fd >= 0)
                std::fprintf(stderr, "[log] %s is not writable (%s) — the log goes to the "
                                     "temp directory instead\n",
                             (dir / name).string().c_str(), std::strerror(firstErr));
        }
    }
    if (fd < 0)
    {
        std::fprintf(stderr, "[log] could not open a log file anywhere (%s) — stderr "
                             "only\n", std::strerror(errno));
        return false;
    }

    int fds[2] = { -1, -1 };
#if defined(_WIN32)
    if (_pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0)
#else
    if (pipe(fds) != 0)
#endif
    {
        std::fprintf(stderr, "[log] pipe() failed (%s) — stderr only\n", std::strerror(errno));
        SysClose(fd);
        return false;
    }
#if !defined(_WIN32)
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    g_file = fd;
    g_pipeRead = fds[0];
#if defined(_WIN32)
    g_origErr = _dup(2); // -1 when the process has no console at all; fine
    g_origStdHandle = GetStdHandle(STD_ERROR_HANDLE);
    _dup2(fds[1], 2);
    _close(fds[1]);
    // Win32 API writers (SDL's message hooks, a library's OutputDebugString fallback,
    // a child process inheriting the standard handles) go through the std handle,
    // not the CRT descriptor; both now point at the pipe.
    SetStdHandle(STD_ERROR_HANDLE, HANDLE(_get_osfhandle(2)));
#else
    g_origErr = dup(2);
    if (g_origErr >= 0)
        fcntl(g_origErr, F_SETFD, FD_CLOEXEC);
    dup2(fds[1], 2);
    close(fds[1]);
#endif
    g_path = chosen;
    g_live = true;
    g_thread = std::thread(TeeLoop);
    // EVERY exit path must end the tee, not just the two that remember to. `g_thread`
    // is a namespace-scope std::thread, so a `return` out of main that skips End()
    // destroys it while it is still joinable and the C++ runtime calls std::terminate
    // — the process ABORTS with a core dump instead of exiting. That is not
    // theoretical: `return 0` on the launcher-quit path (main.cpp, "the player closed
    // the launcher") did exactly this, so closing the launcher without playing
    // aborted, and four more `return 1` failure paths (first-run gate, timebase, image
    // load, entry point) did too — every one of them a case where the log file is the
    // evidence the player was asked to send. atexit runs BEFORE the destructor of a
    // namespace-scope object constructed at static-init time, so the join happens
    // first and the destructor then sees a non-joinable thread. End() is idempotent,
    // so the explicit calls that already exist stay correct.
    std::atexit([] { End(); });
    // The first line through the tee: in the console AND at the top of the file, so a
    // player reading either knows where the other copy is.
    std::fprintf(stderr, "[log] writing a copy of this output to %s (attach it to a bug "
                         "report; the previous run is %s.1)\n",
                 g_path.string().c_str(), g_path.filename().string().c_str());
    return true;
}

void Flush(unsigned timeoutMs)
{
    if (!g_live)
        return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        if (PipeBytesPending() == 0 && g_read.load() == g_written.load())
            return;
        if (std::chrono::steady_clock::now() >= deadline)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void End()
{
    if (!g_live)
        return;
    g_live = false;
    std::fflush(stderr);
    // Put the original stderr back FIRST, so nothing written after this point goes
    // into a pipe nobody reads; that dup2 also closes the pipe's last write end (fd 2
    // was it), which is what ends the tee thread's read loop. The ORIGINAL descriptor
    // is closed only after the join: the thread is still writing the pipe's last
    // bytes to it, and closing it under the thread lost the console copy's tail on
    // the very first --diag run (the file had it, the terminal did not).
#if defined(_WIN32)
    if (g_origErr >= 0)
        _dup2(g_origErr, 2);
    else
        _close(2);
    if (g_origStdHandle != INVALID_HANDLE_VALUE)
        SetStdHandle(STD_ERROR_HANDLE, g_origStdHandle);
#else
    if (g_origErr >= 0)
        dup2(g_origErr, 2);
    else
        close(2);
#endif
    if (g_thread.joinable())
        g_thread.join();
    if (g_origErr >= 0)
        SysClose(g_origErr);
    g_origErr = -1;
    SysClose(g_pipeRead);
    g_pipeRead = -1;
    SysClose(g_file);
    g_file = -1;
}

const std::filesystem::path& Path() { return g_path; }
} // namespace LogFile
