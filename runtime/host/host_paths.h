#pragma once
// WHERE THINGS ARE, anchored to the EXECUTABLE rather than to the working directory.
//
// Why this file exists (docs/release-plan.md A.1). Until it did, every path in this
// runtime was resolved relative to the process's CWD:
//
//     main.cpp        "../../assets/game/default.xex"
//     vk_renderer.cpp three "../.."-shaped candidates for the shader cache, tried in
//                     order, plus a fourth derived from /proc/self/exe
//     vk_renderer.cpp four more for config/rt_world_xform.json
//
// which is why every documented command in CLAUDE.md begins with `cd runtime/build`.
// That is survivable for a developer who reads the recipe. It is not survivable for a
// player: a shipped build launched from a desktop shortcut has a CWD of the user's home
// directory, finds no shader cache, and renders a black screen with one log line —
// the exact failure this project has spent parts of its life diagnosing from the other
// end.
//
// The rule: ONE root, decided once, printed once, and every asset path derived from it.
//
// Resolution order, in full, because a path that resolves differently on two machines
// is the packaging defect that presents as a game defect:
//
//   1. $CZ_ROOT, if set. Used verbatim. If it does not exist we say so and CARRY ON
//      with the fallback rather than silently ignoring it — an override that is
//      quietly dropped is worse than one that fails.
//   2. Walk up from the executable's own directory, at most kMaxWalk levels, and take
//      the first directory that contains an `assets` subdirectory. This covers BOTH
//      layouts with one rule:
//         dev      <repo>/runtime/build/cz_runtime   -> <repo>          (2 levels up)
//         release  CaseZeroRecomp/cz_runtime         -> CaseZeroRecomp  (0 levels up)
//      The release tree ships assets/package/PUT_YOUR_GAME_HERE.txt precisely so that
//      `assets` exists before the player has done anything (release-plan §2.2).
//   3. Failing that, the executable's own directory. A first-run build whose `assets`
//      was deleted still has somewhere sane to create it.
//
// Nothing here ever falls back to the CWD. That is deliberate: a CWD-relative fallback
// would make the dev tree keep working while the shipped one silently did not, which
// is the one failure mode that survives every test done on the build machine.
#include <filesystem>
#include <string>

namespace HostPaths
{
// The directory containing the running executable. Never empty: if the platform query
// fails, this is "." and Report() says so.
const std::filesystem::path& ExeDir();

// The installation root, resolved as above. Cached; the first call decides.
const std::filesystem::path& Root();

// How Root() was decided — "CZ_ROOT", "assets-walk" or "exe-dir". For the log line and
// for anything that wants to refuse when the root was merely guessed.
const char* RootSource();

// Derived locations. These are pure joins — they do not test for existence, because a
// caller that wants to know whether the game is unpacked should ask FirstRun, whose
// job is to say what is missing and where it comes from.
std::filesystem::path Assets();       // <root>/assets
std::filesystem::path Package();      // <root>/assets/package   the STFS container
std::filesystem::path Game();         // <root>/assets/game      the unpacked package
std::filesystem::path GameXex();      // <root>/assets/game/default.xex
std::filesystem::path SaveDir();      // <root>/assets/save
std::filesystem::path ShaderCache();  // <root>/assets/shader_spv

// THE PER-USER SAVED-GAMES FOLDER (part 86, operator decision after a repackage wiped
// the play copy: player data must live where no tool that touches the install tree
// can reach it). This is the GAME folder; the per-profile subdirectory under it is
// content.cpp's business ("default" until the cross-recomp profile project exists).
//   Windows  <FOLDERID_SavedGames>\Dead Rising 2 Case Zero   (the real Saved Games
//            known folder — localized, redirection-aware; falls back to
//            %USERPROFILE%\Saved Games if the shell cannot answer)
//   Linux    $XDG_DATA_HOME (default ~/.local/share)/Dead Rising 2 Case Zero
//   macOS    ~/Library/Application Support/Dead Rising 2 Case Zero
// CZ_SAVE_DIR (read in content.cpp) still overrides the whole save root for tests.
std::filesystem::path SavedGames();
std::filesystem::path Config();       // <root>/config
std::filesystem::path Tools();        // <root>/tools

// One line on stderr naming the root and how it was chosen. Called once from main()
// before anything opens a file, so that a wrong root is visible at the top of the log
// rather than inferred from a missing-file message forty lines down.
void Report();
} // namespace HostPaths
