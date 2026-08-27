// See first_run.h for why this exists. This file is the messages; the checks are
// three filesystem questions.
#include "first_run.h"

#include "host_paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace
{
namespace fs = std::filesystem;

// A shipped STFS container is 825 MB. The threshold is 1 MB, not 800: the point is to
// tell a real package from the PUT_YOUR_GAME_HERE.txt marker and from a partially
// copied file, and a tighter bound would refuse a legitimately different SKU. The
// magic word is checked too, so a player who dropped in the wrong file gets told that
// rather than "no package".
constexpr std::uintmax_t kMinPackageBytes = 1u << 20;

// A candidate is a big enough regular file; whether it is a PACKAGE is a separate
// question, answered by its first four bytes. Keeping the two apart is the whole
// reason this function reports the magic rather than testing it: the first version
// tested size only and reported the magic as a footnote, so a 2 MB .wav dropped into
// assets/package/ was accepted as the game and the player was told to unpack it. The
// footnote was unreachable — printing it required a decision that could never be
// reached, and only running the branch on purpose showed that (gotcha 30).
bool BigFileMagic(const fs::path& p, std::string* magicOut)
{
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) < kMinPackageBytes)
        return false;
    char magic[5] = {};
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f)
        return false;
    const size_t n = std::fread(magic, 1, 4, f);
    std::fclose(f);
    if (n != 4)
        return false;
    if (magicOut)
        *magicOut = magic;
    return true;
}

// XContent containers begin with one of three four-byte words. LIVE is what this
// title ships as; CON is a console-signed package; PIRS is a system one.
bool IsXContentMagic(const std::string& m)
{
    return m == "LIVE" || m == "CON " || m == "PIRS";
}

// The package lives at assets/package/<titleid>/<contenttype>/<hash>, so this walks
// rather than globs one level.
//
// `requireMagic` is the difference between the two questions this file asks. The CHECK
// asks "is there a package" and must say no to a .wav. The MESSAGE asks "is there
// anything big in there at all", so that a player who dropped in the wrong file is
// told about that file by name instead of being told the directory is empty.
bool FindPackage(fs::path* out, std::string* magicOut, bool requireMagic)
{
    std::error_code ec;
    const fs::path root = HostPaths::Package();
    if (!fs::is_directory(root, ec))
        return false;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::string magic;
        if (!BigFileMagic(it->path(), &magic))
            continue;
        if (requireMagic && !IsXContentMagic(magic))
            continue;
        if (out)
            *out = it->path();
        if (magicOut)
            *magicOut = magic;
        return true;
    }
    return false;
}

bool ShaderCachePopulated()
{
    std::error_code ec;
    // CZ_SHADER_SPV overrides the location, and a run that sets it has said out loud
    // where its cache is; do not second-guess it here. The renderer still refuses
    // loudly if that directory turns out to be empty.
    const char* env = std::getenv("CZ_SHADER_SPV");
    const fs::path dir = (env && *env) ? fs::path(env) : HostPaths::ShaderCache();
    if (!fs::is_directory(dir, ec))
        return false;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".spv")
            return true; // one is enough to say "a cache exists"; the renderer counts
    return false;
}
} // namespace

namespace FirstRun
{
Status Check(const std::string& xexPath)
{
    std::error_code ec;
    const bool haveGame = std::filesystem::is_regular_file(xexPath, ec);
    if (!haveGame)
        return FindPackage(nullptr, nullptr, /*requireMagic=*/true) ? Status::NoGame
                                                                 : Status::NoPackage;
    if (!ShaderCachePopulated())
        return Status::NoShaderCache;
    return Status::Ready;
}

std::string Explain(Status s, const std::string& xexPath)
{
    switch (s)
    {
    case Status::Ready:
        return {};

    case Status::NoPackage:
    {
        // Both the thing that is missing AND where it comes from. "Put the game here"
        // is useless to someone who does not know what shape the game is in on disk.
        std::string m =
            "No Dead Rising 2: Case Zero package found.\n"
            "\n"
            "  Put your own copy of the XBLA package into:\n"
            "      " + HostPaths::Package().string() + "/\n"
            "\n"
            "  It is the file your Xbox 360 downloaded, normally at\n"
            "      Content/0000000000000000/58410A8D/000D0000/<long hash, no extension>\n"
            "  and it is about 825 MB. Copy the whole 58410A8D folder in if that is\n"
            "  easier — this looks recursively.\n"
            "\n"
            "  This build ships no game data and cannot supply it.\n";
        // Reachable, and t5 of the A.2 gate is what makes it so: a big file that is
        // not an XContent container gets named, with the word that disqualified it.
        std::string magic;
        std::filesystem::path found;
        if (FindPackage(&found, &magic, /*requireMagic=*/false))
            m += "\n  There IS a large file there, but it is not an Xbox 360 content\n"
                 "  package — it begins \"" + magic + "\" where a package begins LIVE,\n"
                 "  CON or PIRS:\n      " + found.string() + "\n";
        return m;
    }

    case Status::NoGame:
    {
        std::filesystem::path pkg;
        std::string magic;
        FindPackage(&pkg, &magic, /*requireMagic=*/true);
        return "The package is there but has not been unpacked yet.\n"
               "\n"
               "  Missing: " + xexPath + "\n"
               "  Package: " + pkg.string() + "\n"
               "\n"
               "  Unpack it (825 MB in, 832 MB out, about 30 seconds):\n"
               "      python3 " + (HostPaths::Tools() / "extract_stfs.py").string() +
               " \\\n          \"" + pkg.string() + "\" -o " +
               HostPaths::Game().string() + "\n";
    }

    case Status::NoShaderCache:
        return "No translated shader cache.\n"
               "\n"
               "  Missing: " + HostPaths::ShaderCache().string() + "\n"
               "\n"
               "  Without it the renderer starts, declines every draw whose shader it\n"
               "  cannot find, and presents a black screen — which is why this refuses\n"
               "  to start rather than letting you discover it that way.\n"
               "\n"
               "  Build it:\n"
               "      " + (HostPaths::Tools() / "build_shader_spv.sh").string() +
               " <ucode dir> " + HostPaths::ShaderCache().string() + "\n"
               "\n"
               "  Or run without the renderer (CZ_VKDRAW unset), which needs no cache.\n";
    }
    return {};
}

bool Gate(bool renderer, const std::string& xexPath)
{
    if (const char* e = std::getenv("CZ_NO_FIRST_RUN_CHECK"); e && *e && *e != '0')
    {
        std::fprintf(stderr, "[firstrun] CZ_NO_FIRST_RUN_CHECK=%s — checks skipped.\n", e);
        return true;
    }

    const Status s = Check(xexPath);
    // A missing shader cache is only fatal if something is going to ask for shaders.
    // Every log-diff gate in this project runs with the renderer off, and refusing
    // those on a fresh clone would take the gates offline to protect the player from
    // a screen they are not going to see.
    if (s == Status::NoShaderCache && !renderer)
    {
        std::fprintf(stderr, "[firstrun] no shader cache, but the renderer is off "
                             "(CZ_VKDRAW unset) — proceeding.\n");
        return true;
    }
    if (s == Status::Ready)
        return true;

    std::fprintf(stderr, "\n=== Dead Rising 2: Case Zero — cannot start ===\n\n%s\n",
                 Explain(s, xexPath).c_str());
    return false;
}
} // namespace FirstRun
