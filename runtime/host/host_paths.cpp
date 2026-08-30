// The three platform spellings of "where am I", and the root walk built on top.
// See host_paths.h for why this exists and what the resolution order is.
//
// All three spellings are here from the start even though only one of them can be
// compiled today. They are eight lines each, they are the kind of thing that is
// obvious now and a half-day of guessing when Windows is being ported under time
// pressure, and having them written means milestone B's file list is genuinely the
// five files docs/release-plan.md §1.1 censused rather than five plus whatever turns
// up.
#include "host_paths.h"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath (SavedGames); shell32 is already linked
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#endif

namespace
{
// How far up from the executable to look for `assets`. Two is what the dev tree needs
// (runtime/build -> repo root); four leaves room for a packaging layout that nests one
// deeper (a macOS .app puts the binary in Contents/MacOS/) without letting the walk
// wander off into the user's home directory and find some unrelated `assets`.
constexpr int kMaxWalk = 4;

std::filesystem::path QueryExePath()
{
#if defined(_WIN32)
    // GetModuleFileNameW rather than the A variant: a player's install path may hold
    // characters that do not survive the ANSI code page, and "it works on my machine"
    // is exactly what that defect looks like.
    std::wstring buf(1024, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size())
        {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2); // truncated; ERROR_INSUFFICIENT_BUFFER
    }
#elif defined(__APPLE__)
    // _NSGetExecutablePath returns the path as INVOKED, which may contain symlinks or
    // `..`; weakly_canonical resolves it the same way readlink already does on Linux.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // returns -1 and sets the size we need
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(std::filesystem::path(buf.data()), ec);
    return ec ? std::filesystem::path(buf.data()) : p;
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    return std::filesystem::path(buf);
#endif
}

const char* g_rootSource = "unresolved";

std::filesystem::path ResolveRoot()
{
    std::error_code ec;

    if (const char* env = std::getenv("CZ_ROOT"); env && *env)
    {
        const std::filesystem::path p(env);
        if (std::filesystem::is_directory(p, ec))
        {
            g_rootSource = "CZ_ROOT";
            return p;
        }
        // Loud, and then we continue. See the header: silently ignoring an override
        // means the operator debugs the wrong tree.
        std::fprintf(stderr,
                     "[paths] CZ_ROOT=%s is not a directory — ignoring it and falling "
                     "back to the executable walk.\n",
                     env);
    }

    const std::filesystem::path exeDir = HostPaths::ExeDir();
    std::filesystem::path at = exeDir;
    for (int i = 0; i <= kMaxWalk; ++i)
    {
        if (std::filesystem::is_directory(at / "assets", ec))
        {
            g_rootSource = "assets-walk";
            return at;
        }
        const std::filesystem::path up = at.parent_path();
        if (up.empty() || up == at) // hit the filesystem root
            break;
        at = up;
    }

    g_rootSource = "exe-dir";
    return exeDir;
}
} // namespace

namespace HostPaths
{
const std::filesystem::path& ExeDir()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path exe = QueryExePath();
        if (exe.empty())
        {
            std::fprintf(stderr, "[paths] could not locate the executable — every asset "
                                 "path will be resolved relative to \".\".\n");
            return std::filesystem::path(".");
        }
        return exe.parent_path();
    }();
    return dir;
}

const std::filesystem::path& Root()
{
    static const std::filesystem::path root = ResolveRoot();
    return root;
}

const char* RootSource()
{
    Root(); // force the resolution, so the source string is never "unresolved"
    return g_rootSource;
}

std::filesystem::path Assets()      { return Root() / "assets"; }
std::filesystem::path Package()     { return Assets() / "package"; }
std::filesystem::path Game()        { return Assets() / "game"; }
std::filesystem::path GameXex()     { return Game() / "default.xex"; }
std::filesystem::path SaveDir()     { return Assets() / "save"; }
std::filesystem::path ShaderCache() { return Assets() / "shader_spv"; }
std::filesystem::path Config()      { return Root() / "config"; }
std::filesystem::path Tools()       { return Root() / "tools"; }

std::filesystem::path SavedGames()
{
    // See the header for the doctrine. The game-folder NAME is the human one on
    // purpose — this directory is meant to be found by a player.
    static const char* kGameFolder = "Dead Rising 2 Case Zero";
#ifdef _WIN32
    // The real Saved Games known folder, not Documents: FOLDERID_SavedGames has been
    // the OS's answer to exactly this question since Vista, and it follows the user's
    // language and folder redirection where a hardcoded path would not.
    PWSTR w = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &w)) && w)
    {
        base = w;
        CoTaskMemFree(w);
    }
    else if (const char* up = std::getenv("USERPROFILE"); up && *up)
        base = std::filesystem::path(up) / "Saved Games";
    else
        base = ExeDir(); // no user profile at all: stay beside the exe, loudly odd
    return base / kGameFolder;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / "Library" /
           "Application Support" / kGameFolder;
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / kGameFolder;
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".local" / "share" / kGameFolder;
#endif
}

void Report()
{
    std::fprintf(stderr, "[paths] root %s (%s), exe %s\n", Root().string().c_str(),
                 RootSource(), ExeDir().string().c_str());
}
} // namespace HostPaths
