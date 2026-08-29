#pragma once
// THE IN-PROCESS STFS EXTRACT — release-plan §2.3 step 2, built in part 85.
//
// Until this existed, a player whose package was present but unextracted was REFUSED
// with the python3 command to run (host/first_run.h explains why refusing beats
// proceeding). This file is that command, in-process: the same container walk as
// tools/extract_stfs.py, which remains the reference implementation and the dev tool.
// The two are deliberate duplicates in the same sense as the shader translator's
// (part 84): when either changes, the gate is a byte diff of the two output trees
// over the real package — `cz_runtime --extract-package <pkg> <dir>` vs the Python.
//
// Scope, stated rather than implied:
//   * STFS only. Case Zero ships as STFS (volume type 0), and every XBLA title does.
//     An SVOD package (volume type 1) is refused BY NAME with the Python tool that
//     does handle it — a rare path with no test coverage here would be a guess
//     shipped to a stranger (gotcha 5's shape).
//   * Read-only packages only, same as the Python: guessing the read-write hash
//     table offset wrong reads hash tables as data, silently.
//   * Every file offset is bounds-checked against the package size, and every entry
//     name is checked against path traversal, because the package is PLAYER-SUPPLIED
//     input: a malformed one must name what was wrong, never write outside outDir.
//
// Interruption safety: default.xex is written LAST. The first-run check's "is the
// game unpacked" question is exactly "does default.xex exist" — so an extraction
// killed halfway must not leave that file behind with the data/ tree missing, which
// would boot into hundreds of file-not-found lines that no longer name the cause.
// Re-running the extraction is idempotent (it overwrites).
#include <filesystem>
#include <functional>
#include <string>

namespace StfsExtract
{
// Unpack `package` into `outDir`, creating directories as needed. Prints the package
// identity (title id, display name, content type) and progress to stderr — the
// identity print is the cheapest check on a file whose NAME carries no information.
// Returns true on success; on failure `err` says what was wrong with which entry.
// `progress(bytesDone, bytesTotal)` fires on the CALLING thread after every file —
// main.cpp feeds it to the first-run progress window; the CLI passes nothing.
bool Extract(const std::filesystem::path& package, const std::filesystem::path& outDir,
             std::string& err,
             const std::function<void(uint64_t, uint64_t)>& progress = {});
} // namespace StfsExtract
