#include "vfs.h"

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
