#pragma once
// THE FIRST-RUN CHECK, and the refusal messages that are the whole point of it.
//
// docs/release-plan.md §2.3 and A.2. A shipped build is launched by someone who has
// never read this repository, and the three things it needs are supplied by them, in
// order:
//
//     assets/package/<hash>   the STFS container, which they own and we never ship
//     assets/game/            what tools/extract_stfs.py unpacks out of it
//     assets/shader_spv/      the translated shader cache
//
// Any one of them missing today produces a failure that says nothing useful: no
// package is `cannot open ../../assets/game/default.xex`; no shader cache is a black
// screen and one log line. Both are failures this project has spent parts of its life
// diagnosing from the wrong end — with the whole ledger in hand. A stranger has no
// chance.
//
// So the rule this file exists to enforce: WHEN SOMETHING IS MISSING, SAY WHAT IT IS,
// WHERE IT GOES, AND WHICH COMMAND PRODUCES IT — then exit non-zero. Never proceed
// into a subsystem that will fail later for a reason that no longer names the cause.
//
// What this file does NOT do: any of the work. Running the STFS extract in-process is
// milestone D's job and the shader build is D.3. Until those land, the check names the
// command the player must run. That ordering is deliberate — the diagnosis is worth
// shipping before the automation is, and it is what makes D's progress bar an
// improvement on a working thing rather than the first thing that works.
#include <filesystem>
#include <string>

namespace FirstRun
{
enum class Status
{
    Ready,           // everything present; proceed
    NoPackage,       // assets/package holds no STFS container
    NoGame,          // package present, assets/game/default.xex absent
    NoShaderCache,   // game present, assets/shader_spv absent or empty
};

// The package the check would name, if one is present — exposed so the in-process
// STFS extract (host/stfs_extract.h, release §2.3 step 2) runs on the same file the
// refusal message would have pointed at, rather than re-deriving the search.
bool FoundPackage(std::filesystem::path* out);

// Runs the checks in order and returns the FIRST thing missing. Does not print.
// `xexPath` is the image the run will actually load — an explicit argv[1] or CZ_XEX
// points somewhere other than the default, and refusing such a run because the
// DEFAULT location is empty would be a check reporting on a tree nobody is using.
Status Check(const std::string& xexPath);

// One human sentence for a status, ending in the command that fixes it. Empty for
// Ready. `xexPath` is named in the message so that a run pointed elsewhere by CZ_XEX
// says which file it actually looked for.
std::string Explain(Status s, const std::string& xexPath);

// Check(), print, and return true if the run may proceed. `renderer` says whether the
// renderer is going to be asked for — a headless gate run with CZ_VKDRAW unset does
// not need a shader cache and must not be refused for the lack of one, because that
// would take every log-diff gate in this project offline on a fresh clone.
//
// CZ_NO_FIRST_RUN_CHECK=1 skips the whole thing. It exists so that a session can
// deliberately run against a hand-assembled tree, and because a check that cannot be
// turned off eventually gets deleted rather than fixed.
bool Gate(bool renderer, const std::string& xexPath);
} // namespace FirstRun
