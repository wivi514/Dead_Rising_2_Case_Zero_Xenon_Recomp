#include "vfs.h"

#include "../cpu/boot_skip.h"
#include "../cpu/native_kbm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>

#include "klog.h"

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
std::map<std::string, std::string> g_mounts;   // "game" -> "/.../assets/game"
std::unordered_map<std::string, std::string> g_resolved; // guest path -> host path

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Walk a guest path component by component, matching each against the real
// directory case-insensitively. Only used when the exact path does not exist, so
// the common case costs one stat.
std::string CaseInsensitiveResolve(const fs::path& root, const std::string& relative)
{
    fs::path current = root;
    size_t start = 0;
    while (start <= relative.size())
    {
        const size_t slash = relative.find('/', start);
        const std::string part =
            relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? relative.size() + 1 : slash + 1;
        if (part.empty())
            continue;

        std::error_code ec;
        if (fs::exists(current / part, ec))
        {
            current /= part;
            continue;
        }
        const std::string want = Lower(part);
        bool found = false;
        for (const auto& entry : fs::directory_iterator(current, ec))
        {
            if (Lower(entry.path().filename().string()) == want)
            {
                current = entry.path();
                found = true;
                break;
            }
        }
        if (!found)
            return {};
    }
    return current.string();
}

} // namespace

void VfsSetGameRoot(const std::string& hostPath)
{
    // Xenia registers both of these before the title runs — A1:
    //   Registered symbolic link: GAME: => \Device\Package_0
    //   Registered symbolic link: D:    => \Device\Package_0
    // so neither is something the guest has to ask for, and mounting them up front
    // is what the console's own state looks like at entry.
    VfsMountDevice("game", hostPath);
    VfsMountDevice("d", hostPath);
    KLOG("VFS: game: and d: -> %s\n", hostPath.c_str());
}

void VfsMountDevice(const std::string& device, const std::string& hostPath)
{
    std::lock_guard lock(g_mutex);
    g_mounts[Lower(device)] = hostPath;
    g_resolved.clear();
}

void VfsUnmountDevice(const std::string& device)
{
    std::lock_guard lock(g_mutex);
    g_mounts.erase(Lower(device));
    g_resolved.clear();
}

// Drop one path's cached answer.
//
// The resolver caches NEGATIVE results on purpose (see VfsResolveExisting), which is
// right for a boot that probes for optional files and wrong the moment anything in
// this runtime CREATES one: the create itself is what asked "does it exist?" and got
// the "no" that is now cached, so the file it just wrote is invisible to every later
// open. The file layer's own self-test caught exactly that — it wrote 303,104 bytes
// and then could not re-open them — and it is the save path end to end, because the
// title probes for `save:\DR2P000.DSF` before it writes one.
//
// Mount and unmount clear the whole map instead, since a device pointing somewhere new
// invalidates every path under it and there is no cheap way to enumerate those.
void VfsForget(const std::string& guestPath)
{
    std::lock_guard lock(g_mutex);
    g_resolved.erase(guestPath);
}

std::string VfsTranslate(const std::string& guestPath)
{
    const size_t colon = guestPath.find(':');
    if (colon == std::string::npos)
        return {};

    std::string device = Lower(guestPath.substr(0, colon));
    // `\??\GAME:` and `\Device\...` spellings both reach here; strip the prefix.
    if (const size_t last = device.find_last_of("\\/"); last != std::string::npos)
        device.erase(0, last + 1);

    std::string relative = guestPath.substr(colon + 1);
    std::replace(relative.begin(), relative.end(), '\\', '/');
    while (!relative.empty() && relative.front() == '/')
        relative.erase(0, 1);

    std::lock_guard lock(g_mutex);
    auto it = g_mounts.find(device);
    if (it == g_mounts.end())
        return {};
    return relative.empty() ? it->second : it->second + "/" + relative;
}

std::string VfsResolveExisting(const std::string& guestPath)
{
    {
        std::lock_guard lock(g_mutex);
        auto cached = g_resolved.find(guestPath);
        if (cached != g_resolved.end())
            return cached->second;
    }

    const std::string direct = VfsTranslate(guestPath);
    if (direct.empty())
        return {};

    std::error_code ec;

    static const bool overlayOff = getenv("CZ_NO_PATCHED_ASSETS") != nullptr;

    // THE MODS OVERLAY (2026-10-08). assets/game_mods/ holds whole replacement files
    // built by the DR2 CZ/CW modding toolkit (`dr2 mod deploy`, in the sibling
    // DR2_CZ_CW_Modding repo): a re-packed archive whose texture a player recoloured,
    // an edited script bank, and so on. The toolkit does all format work (LZX, tiling,
    // nested archives) and builds each file ON TOP OF game_patched's copy when there is
    // one, so this layer only ever swaps whole files and can never drop the recomp's
    // own patches. It is checked FIRST, before the boot-skip / keyboard-prompt /
    // patched layers: a player who installed a mod expects to see it. A file one of
    // those toggle layers also carries is shadowed while the mod is installed, and the
    // toolkit warns about that at deploy time.
    //
    // Lookup is exact first, then case-insensitive (the toolkit writes the shipped
    // files' case; the guest may spell a path differently). Every file served from here
    // is logged, so "is my mod loaded?" is one grep. CZ_NO_MODS=1 is the off switch:
    // the layer is never consulted and the run is byte-for-byte the unmodded one.
    // Independent of CZ_NO_PATCHED_ASSETS, so each can be A/B'd alone.
    static const bool modsOff = getenv("CZ_NO_MODS") != nullptr;
    if (!modsOff)
    {
        std::string gameRoot;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_mounts.find("game");
            if (it != g_mounts.end())
                gameRoot = it->second;
        }
        const std::string modsRoot = gameRoot + "_mods";
        if (!gameRoot.empty() && direct.rfind(gameRoot + "/", 0) == 0 &&
            fs::is_directory(modsRoot, ec))
        {
            const std::string relative = direct.substr(gameRoot.size() + 1);
            std::string modded = modsRoot + "/" + relative;
            if (!fs::is_regular_file(modded, ec))
                modded = CaseInsensitiveResolve(modsRoot, relative);
            if (!modded.empty() && fs::is_regular_file(modded, ec))
            {
                KLOG("VFS: '%s' served from the MODS overlay -> %s "
                     "(CZ_NO_MODS=1 turns mods off)\n",
                     guestPath.c_str(), modded.c_str());
                std::lock_guard lock(g_mutex);
                g_resolved.emplace(guestPath, modded);
                return modded;
            }
        }
    }

    // THE BOOT-SKIP OVERLAY (part 99). assets/game_bootskip/ holds one file —
    // fecmn.big with intro.txt's logo-timeline keyframes collapsed (see
    // cpu/boot_skip.cpp for why this is a data patch and not a hook). Its own
    // layer, not a game_patched edit, because the launcher toggle must pick at
    // boot between the stock timeline and the collapsed one without
    // regenerating anything. Checked FIRST: the file it carries is generated
    // FROM the patched layer, so it supersedes it. Toggle off = the layer is
    // never consulted, so the stock boot stays byte-for-byte identical.
    if (!overlayOff && BootSkip_Enabled())
    {
        std::string gameRoot;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_mounts.find("game");
            if (it != g_mounts.end())
                gameRoot = it->second;
        }
        if (!gameRoot.empty() && direct.rfind(gameRoot + "/", 0) == 0)
        {
            const std::string patched =
                gameRoot + "_bootskip/" + direct.substr(gameRoot.size() + 1);
            if (fs::exists(patched, ec))
            {
                KLOG("VFS: '%s' served from the BOOT-SKIP overlay -> %s "
                     "(skip_intro_logos; CZ_SKIP_INTRO=0 restores the logos)\n",
                     guestPath.c_str(), patched.c_str());
                std::lock_guard lock(g_mutex);
                g_resolved.emplace(guestPath, patched);
                return patched;
            }
        }
    }

    // THE KEYBOARD-PROMPT OVERLAY (part 92). assets/game_kbm/ holds the banks
    // tools/gen_kbm_icons.py generates — fecmn.tex with the pad-button glyphs
    // replaced by our own key-cap chip art — and is a SEPARATE layer from
    // game_patched because it should exist only while the native keyboard is
    // the input path: a pad player (CZ_NO_NATIVE_KBM=1) keeps pad prompts, and
    // CZ_NO_KB_PROMPTS=1 keeps them with the keyboard live too. Checked BEFORE
    // game_patched: this layer overrides files the part-60 overlay also carries
    // (str_en.bcs — the PRESS ENTER title line rides on the patched bank).
    static const bool kbPromptsOn = NativeKbm_Enabled() &&
                                    getenv("CZ_NO_KB_PROMPTS") == nullptr;
    if (!overlayOff && kbPromptsOn)
    {
        std::string gameRoot;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_mounts.find("game");
            if (it != g_mounts.end())
                gameRoot = it->second;
        }
        if (!gameRoot.empty() && direct.rfind(gameRoot + "/", 0) == 0)
        {
            const std::string patched =
                gameRoot + "_kbm/" + direct.substr(gameRoot.size() + 1);
            if (fs::exists(patched, ec))
            {
                KLOG("VFS: '%s' served from the KB-PROMPT overlay -> %s "
                     "(CZ_NO_KB_PROMPTS=1 restores the pad glyphs)\n",
                     guestPath.c_str(), patched.c_str());
                // The string device-follow must read the bank that LOADED, not
                // str_en's (player issue #6: a French player's bank shares
                // en's id table but not its offsets, so no string ever swapped).
                {
                    const std::string fn = fs::path(patched).filename().string();
                    if (fn.rfind("str_", 0) == 0 && fn.size() > 8 &&
                        fn.compare(fn.size() - 4, 4, ".bcs") == 0)
                        NativeKbm_NoteStringBank(patched);
                }
                std::lock_guard lock(g_mutex);
                g_resolved.emplace(guestPath, patched);
                return patched;
            }
        }
    }

    // THE PATCHED-ASSET OVERLAY (part 60). assets/game_patched/ mirrors the package
    // tree and holds the files tools/gen_pc_options.py generates — the repacked
    // fecmn.big whose options_pc.txt is rewritten for the resurrected PC options
    // screen, and the string banks with our added value strings. A file that exists
    // there wins over the shipped one; everything else falls through untouched.
    //
    // The overlay keys on the TRANSLATED path so it composes with both the `game:`
    // and `d:` spellings, and it is checked BEFORE the shipped file so that "the
    // patched archive failed to parse" can never silently degrade into "the shipped
    // menu came up" — a wrong patched file should be seen, not masked (gotcha 5).
    // CZ_NO_PATCHED_ASSETS=1 is the control arm: the shipped data, byte for byte.
    if (!overlayOff)
    {
        std::string gameRoot;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_mounts.find("game");
            if (it != g_mounts.end())
                gameRoot = it->second;
        }
        if (!gameRoot.empty() && direct.rfind(gameRoot + "/", 0) == 0)
        {
            const std::string patched =
                gameRoot + "_patched/" + direct.substr(gameRoot.size() + 1);
            if (fs::exists(patched, ec))
            {
                KLOG("VFS: '%s' served from the PATCHED overlay -> %s "
                     "(CZ_NO_PATCHED_ASSETS=1 restores the shipped file)\n",
                     guestPath.c_str(), patched.c_str());
                std::lock_guard lock(g_mutex);
                g_resolved.emplace(guestPath, patched);
                return patched;
            }
        }
    }

    std::string answer;
    if (fs::exists(direct, ec))
    {
        answer = direct;
    }
    else
    {
        // Case-insensitive fallback. Recover the mount root and the relative part so
        // the walk starts somewhere real.
        const size_t colon = guestPath.find(':');
        std::string device = Lower(guestPath.substr(0, colon));
        if (const size_t last = device.find_last_of("\\/"); last != std::string::npos)
            device.erase(0, last + 1);
        std::string relative = guestPath.substr(colon + 1);
        std::replace(relative.begin(), relative.end(), '\\', '/');
        while (!relative.empty() && relative.front() == '/')
            relative.erase(0, 1);

        std::string root;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_mounts.find(device);
            if (it == g_mounts.end())
                return {};
            root = it->second;
        }
        answer = CaseInsensitiveResolve(root, relative);
        if (!answer.empty())
            KLOG("VFS: '%s' matched case-insensitively -> %s\n", guestPath.c_str(),
                 answer.c_str());
    }

    std::lock_guard lock(g_mutex);
    // Negative results are cached too: a title that probes for optional files (this
    // one probes game:\data\capcom.txt at boot) would otherwise re-scan a directory
    // on every miss.
    g_resolved.emplace(guestPath, answer);
    return answer;
}
