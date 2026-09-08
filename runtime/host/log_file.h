#pragma once
// THE LOG FILE: everything this process writes to stderr, also written to a file.
//
// Why this exists (docs/steam-deck-plan.md §1 item 3, §3 item 1). Every shipped build
// before part 105 printed to stderr and nowhere else. A developer runs the game from a
// terminal and pipes the output to a file; a PLAYER double-clicks an exe, an AppImage
// or a Steam library entry, and in every one of those the console is either absent or
// gone the moment the process exits. So every Steam Deck report of v1.0.1 read
// "didn't work" — not because the players were vague but because that is ALL the
// shipped build let them say. The one thing that turns a report into a diagnosis is a
// file that survives the exit, written from the first line, in a place the player can
// find.
//
// How. Not by replacing the ~3,000 fprintf(stderr, ...) sites, and not by a FILE*
// hook (glibc has fopencookie, the MSVC CRT has nothing): the tee is at the FILE
// DESCRIPTOR. fd 2 is redirected onto a pipe, and one thread copies the pipe to the
// original stderr and to the file. That catches every writer in the process —
// stdio, the crash reporter's raw write(2), SDL's and the DXC library's own messages —
// including the ones written by code that has no idea this module exists.
//
// The one cost of a pipe is that bytes are in flight between the writer and the
// file, and a process that _exit()s a microsecond after printing the crash report
// would lose its tail. Flush() waits for the pipe to drain; the crash reporter, the
// signal handlers and the three _Exit quit paths call it first.
//
// Where: the caller decides (main.cpp passes the data root — beside the executable
// for a release bundle, beside the .AppImage for that layout, the repo root for a dev
// tree). CZ_LOG_FILE=<path> overrides the location; CZ_NO_LOG_FILE=1 turns the tee
// off entirely (and says so — an instrument that silently stopped is worse than none).
// An existing file is rotated to <name>.1 first, so the previous run's log survives
// one launch of "let me just try it again".
#include <filesystem>

namespace LogFile
{
// Start the tee into <dir>/<name> (or $CZ_LOG_FILE when set). Prints one line naming
// the file once the tee is live, so the line lands in both the console and the file.
// Safe to call once; a second call is ignored. Returns false (and says why) when the
// file could not be opened anywhere — the process carries on with stderr alone.
bool Begin(const std::filesystem::path& dir, const char* name);

// Wait until every byte written to fd 2 so far has reached the file, or the timeout
// passes. For the exit paths that bypass End(): _Exit, _exit, the crash reporter.
// Async-signal-safe enough for a handler: it reads two atomics, one ioctl and sleeps.
void Flush(unsigned timeoutMs);

// Put fd 2 back, drain, close the file and join the thread. The ordinary-return path
// (main returning) is the only caller; every other exit goes through Flush().
void End();

// The file being written, or empty when the tee is off.
const std::filesystem::path& Path();
} // namespace LogFile
