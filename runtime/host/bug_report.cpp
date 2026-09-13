// The bug-report capture. See bug_report.h for what it is and the contract it writes.
//
// HOW A CAPTURE HAPPENS. The window thread sees F9 (or F8) and calls Request; that
// stamps the moment T0, writes a marker into the log (so the log itself says where the
// key was), and leaves a frame want on the renderer. The renderer, which already arms
// its present readback for F8/F9's dev instruments, arms it for this too and hands
// every presented frame's pixels to OfferPixels; the capture keeps the first (and, for
// F8, two more about half a second apart), copied out of the readback into its own
// buffer — the readback slot is reused the next frame. A worker thread then sleeps
// until T0 + 15 s, asks the log tee for the ring's text between T0 − 60 s and now,
// gathers the machine, encodes the PNGs, writes <name>.partial/, renames it, prunes
// the folder, and says so in the log and (when the overlay is up) as a toast.
//
// THE SIZES ARE THE CONTRACT'S: at most 6 files, 4 MiB each, 8 MiB together. A frame
// wider than 1920 is box-filtered down by halves first (a 4K frame is a 2160-line PNG
// nobody needs to read a bug), and a PNG that still exceeds its share is halved again;
// the log is cut to its last 3.5 MiB with a line saying so at the top, because the end
// (the key, and what followed) is the part that matters.
//
// PNG through miniz (public domain, thirdparty/miniz): real deflate, so a 1080p game
// frame lands at 1-2 MiB instead of the 6 MiB a stored-block writer would produce.
#include "bug_report.h"

#include "log_file.h"
#include "settings.h"
#include "../kernel/xlive_glue.h"
#include "../kernel/xlive_overlay_glue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#else
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <cpuid.h>
#include <fstream>
#endif

#include "../thirdparty/miniz/miniz.h"

#ifndef CZ_GAME_VERSION
#define CZ_GAME_VERSION "dev"
#endif

// The Vulkan version fields, without pulling vulkan.h into this file.
#define VK_VERSION_MAJOR_(v) ((v) >> 22)
#define VK_VERSION_MINOR_(v) (((v) >> 12) & 0x3FFu)
#define VK_VERSION_PATCH_(v) ((v) & 0xFFFu)

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr size_t kFileMax = 4u * 1024 * 1024;       // the contract's per-file cap
constexpr size_t kTotalMax = 8u * 1024 * 1024;      // and the per-report cap
constexpr size_t kLogKeep = 3500u * 1024;           // the log's share, from the end
constexpr unsigned kMaxCaptures = 40;
constexpr auto kBefore = std::chrono::seconds(60);
constexpr auto kAfter = std::chrono::seconds(15);
constexpr uint64_t kBurstSpacingFrames = 30;        // F8: three frames, ~half a second apart

struct Frame
{
    std::vector<uint8_t> rgb;
    uint32_t w = 0, h = 0;
    uint64_t frame = 0;
};

struct Pending
{
    std::string trigger;
    Clock::time_point t0;
    std::chrono::system_clock::time_point wall;
    unsigned wantFrames = 1;
    uint64_t nextFrame = 0;         // take the next presented frame at or after this
    Clock::time_point pixelDeadline; // stop waiting for pixels after this
    std::vector<Frame> frames;
};

bool g_enabled = false;
fs::path g_dir;
std::mutex g_mutex;                 // guards the pending capture and the frame want
std::condition_variable g_cv;
std::unique_ptr<Pending> g_pending;  // the capture in flight, if any
std::atomic<bool> g_wantPixels{ false };
std::thread g_worker;
std::atomic<bool> g_quit{ false };
std::string g_gpu = "unknown", g_driver = "unknown";
uint32_t g_apiVersion = 0;

// ---- the machine ---------------------------------------------------------------

std::string CpuBrand()
{
    char brand[49] = { 0 };
#if defined(_WIN32)
    int regs[4];
    for (unsigned i = 0; i < 3; i++)
    {
        __cpuid(regs, int(0x80000002u + i));
        memcpy(brand + i * 16, regs, 16);
    }
#else
    unsigned a, b, c, d;
    for (unsigned i = 0; i < 3; i++)
    {
        if (!__get_cpuid(0x80000002u + i, &a, &b, &c, &d))
            return "unknown";
        unsigned regs[4] = { a, b, c, d };
        memcpy(brand + i * 16, regs, 16);
    }
#endif
    std::string s(brand);
    // The brand string pads with spaces; collapse them.
    std::string out;
    for (char ch : s)
        if (!(ch == ' ' && !out.empty() && out.back() == ' '))
            out += ch;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    return out.empty() ? "unknown" : out;
}

std::string OsName()
{
#if defined(_WIN32)
    // RtlGetVersion tells the truth where GetVersionEx lies to un-manifested exes.
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")))
            fn(&v);
    char product[128] = "Windows", display[64] = "";
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_READ, &key) == ERROR_SUCCESS)
    {
        DWORD n = sizeof product;
        RegQueryValueExA(key, "ProductName", nullptr, nullptr, (LPBYTE)product, &n);
        n = sizeof display;
        RegQueryValueExA(key, "DisplayVersion", nullptr, nullptr, (LPBYTE)display, &n);
        RegCloseKey(key);
    }
    char out[256];
    // Windows 11 still reports ProductName "Windows 10 ..."; the build number says which.
    const bool eleven = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000;
    snprintf(out, sizeof out, "%s %s (build %lu.%lu.%lu)", eleven ? "Windows 11" : product,
             display, (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
             (unsigned long)v.dwBuildNumber);
    return out;
#else
    std::string pretty;
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("PRETTY_NAME=", 0) == 0)
        {
            pretty = line.substr(12);
            if (pretty.size() >= 2 && pretty.front() == '"')
                pretty = pretty.substr(1, pretty.size() - 2);
        }
    utsname u{};
    uname(&u);
    if (pretty.empty())
        pretty = u.sysname;
    return pretty + " (" + u.release + ")";
#endif
}

uint64_t RamMb()
{
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    GlobalMemoryStatusEx(&m);
    return m.ullTotalPhys / (1024 * 1024);
#else
    struct sysinfo si{};
    if (sysinfo(&si) != 0)
        return 0;
    return uint64_t(si.totalram) * si.mem_unit / (1024 * 1024);
#endif
}

std::string JsonEscape(const std::string& s)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += char(c);
    }
    return out;
}

// ---- the picture ---------------------------------------------------------------

// Halve a frame with a 2x2 box filter. Odd edges drop their last column/row.
void Halve(Frame& f)
{
    const uint32_t w = f.w / 2, h = f.h / 2;
    std::vector<uint8_t> out(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            for (unsigned c = 0; c < 3; c++)
            {
                const uint8_t* p0 = &f.rgb[(size_t(2 * y) * f.w + 2 * x) * 3 + c];
                const uint8_t* p1 = p0 + size_t(f.w) * 3;
                out[(size_t(y) * w + x) * 3 + c] =
                    uint8_t((unsigned(p0[0]) + p0[3] + p1[0] + p1[3] + 2) / 4);
            }
    f.rgb.swap(out);
    f.w = w;
    f.h = h;
}

// The frame as a PNG that fits `budget`, halving until it does (or until it is
// smaller than 640 wide, at which point whatever it is goes).
std::vector<uint8_t> EncodePng(Frame f, size_t budget)
{
    while (f.w > 1920)
        Halve(f);
    for (;;)
    {
        size_t len = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(f.rgb.data(), int(f.w), int(f.h),
                                                              3, &len, 6, MZ_FALSE);
        if (!png)
            return {};
        std::vector<uint8_t> out(static_cast<uint8_t*>(png), static_cast<uint8_t*>(png) + len);
        mz_free(png);
        if (out.size() <= budget || f.w <= 640)
            return out;
        Halve(f);
    }
}

// ---- the folder ----------------------------------------------------------------

uint64_t MaxBytes()
{
    if (const char* m = getenv("CZ_BUG_REPORT_MAX_MB"))
        return strtoull(m, nullptr, 10) * 1024ull * 1024ull;
    return 256ull * 1024 * 1024;
}

uintmax_t DirBytes(const fs::path& d)
{
    uintmax_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(d, ec); !ec && it != fs::end(it); it.increment(ec))
        if (it->is_regular_file(ec))
            n += it->file_size(ec);
    return n;
}

// Oldest first by name (the names start with the UTC time), until the folder is
// under its caps. A .partial older than ten minutes is a capture whose process died.
void Prune()
{
    std::error_code ec;
    struct Entry { fs::path path; uintmax_t bytes; bool partial; };
    std::vector<Entry> entries;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& e : fs::directory_iterator(g_dir, ec))
    {
        if (!e.is_directory(ec))
            continue;
        const std::string name = e.path().filename().string();
        const bool partial = name.size() > 8 && name.compare(name.size() - 8, 8, ".partial") == 0;
        if (partial)
        {
            const auto age = now - fs::last_write_time(e.path(), ec);
            if (!ec && age > std::chrono::minutes(10))
            {
                fs::remove_all(e.path(), ec);
                fprintf(stderr, "[bugreport] removed a stale %s\n", name.c_str());
            }
            continue;
        }
        entries.push_back({ e.path(), DirBytes(e.path()), false });
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.path.filename() < b.path.filename(); });
    uintmax_t total = 0;
    for (const auto& en : entries)
        total += en.bytes;
    const uint64_t cap = MaxBytes();
    size_t i = 0;
    while (i < entries.size() && (total > cap || entries.size() - i > kMaxCaptures))
    {
        fs::remove_all(entries[i].path, ec);
        fprintf(stderr, "[bugreport] the captures folder is over its cap (%llu MB, %u captures): "
                        "deleted the oldest, %s\n", (unsigned long long)(cap >> 20), kMaxCaptures,
                entries[i].path.filename().string().c_str());
        total -= entries[i].bytes;
        i++;
    }
}

// ---- the report ----------------------------------------------------------------

std::string Rfc3339(std::chrono::system_clock::time_point t)
{
    const time_t tt = std::chrono::system_clock::to_time_t(t);
    tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

std::string CaptureName(std::chrono::system_clock::time_point t)
{
    const time_t tt = std::chrono::system_clock::to_time_t(t);
    tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[48];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &g);
    std::random_device rd;
    char tail[8];
    snprintf(tail, sizeof tail, "-%04x", unsigned(rd() & 0xFFFF));
    return std::string(b) + tail;
}

bool WriteFile(const fs::path& p, const void* data, size_t n)
{
    FILE* f = fopen(p.string().c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

const char* DisplayModeName(CzDisplayMode m)
{
    switch (m)
    {
    case CzDisplayMode::Windowed: return "windowed";
    case CzDisplayMode::Borderless: return "borderless fullscreen";
    case CzDisplayMode::Fullscreen: return "fullscreen";
    }
    return "?";
}

void WriteReport(std::unique_ptr<Pending> p)
{
    const auto t1 = Clock::now();
    const std::string name = CaptureName(p->wall);
    const fs::path partial = g_dir / (name + ".partial");
    const fs::path final = g_dir / name;
    std::error_code ec;
    fs::create_directories(partial, ec);
    if (ec)
    {
        fprintf(stderr, "[bugreport] cannot create %s: %s — the capture is LOST\n",
                partial.string().c_str(), ec.message().c_str());
        return;
    }

    struct Out { std::string name, type, what; size_t bytes; };
    std::vector<Out> files;
    size_t total = 0;
    bool ok = true;

    // The pictures. The first is "screenshot.png" (the launcher shows that one); the
    // burst's others are burst_2/3. Each gets the per-file cap; together they may not
    // crowd out the log and the machine, so the later ones are dropped if they would.
    for (size_t i = 0; i < p->frames.size(); i++)
    {
        const size_t reserve = kLogKeep + 64 * 1024;   // the log's and system.txt's share
        const size_t room = kTotalMax > total + reserve ? kTotalMax - total - reserve : 0;
        if (room < 64 * 1024)
            break;
        std::vector<uint8_t> png = EncodePng(p->frames[i], std::min(kFileMax, room));
        if (png.empty() || png.size() > room)
            break;
        char fname[32], what[96];
        if (i == 0)
        {
            snprintf(fname, sizeof fname, "screenshot.png");
            snprintf(what, sizeof what, "the frame when %s was pressed", p->trigger.c_str());
        }
        else
        {
            snprintf(fname, sizeof fname, "burst_%zu.png", i + 1);
            snprintf(what, sizeof what, "%.1f s after the key", double(i) * 0.5);
        }
        ok = ok && WriteFile(partial / fname, png.data(), png.size());
        files.push_back({ fname, "image/png", what, png.size() });
        total += png.size();
    }
    if (p->frames.empty())
        fprintf(stderr, "[bugreport] no frame reached the capture (headless, or the renderer "
                        "presented no frame in the window): the report has no picture\n");

    // The log: the ring's text for [T0 - 60 s, now]. Cut from the FRONT to its share,
    // with a line saying so — the end is the part that matters.
    {
        std::string log = LogFile::Recent(p->t0 - kBefore, t1);
        std::string head;
        char line[256];
        snprintf(line, sizeof line, "[bugreport] %s pressed; this is the log from 60 s before to "
                                    "15 s after (%zu bytes)\n", p->trigger.c_str(), log.size());
        head = line;
        if (log.size() > kLogKeep)
        {
            snprintf(line, sizeof line, "[bugreport] (cut: the first %zu bytes of the window are "
                                        "not in this file)\n", log.size() - kLogKeep);
            head += line;
            log.erase(0, log.size() - kLogKeep);
            // Start on a line boundary.
            const size_t nl = log.find('\n');
            if (nl != std::string::npos)
                log.erase(0, nl + 1);
        }
        log = head + log;
        ok = ok && WriteFile(partial / "log.txt", log.data(), log.size());
        files.push_back({ "log.txt", "text/plain", "the log: 60 s before the key and 15 s after",
                          log.size() });
        total += log.size();
    }

    // The machine, as text for the report and as the JSON object the launcher shows.
    const std::string os = OsName(), cpu = CpuBrand();
    const uint64_t ram = RamMb();
    uint32_t rw = 0, rh = 0;
    Settings_InternalRes(rw, rh);
    char vk[32];
    snprintf(vk, sizeof vk, "%u.%u.%u", VK_VERSION_MAJOR_(g_apiVersion), VK_VERSION_MINOR_(g_apiVersion),
             VK_VERSION_PATCH_(g_apiVersion));
    std::string sys;
    {
        char b[1024];
        snprintf(b, sizeof b,
                 "game: Dead Rising 2: Case Zero (recomp) %s\n"
                 "os: %s\n"
                 "cpu: %s (%u threads)\n"
                 "gpu: %s\n"
                 "driver: %s\n"
                 "vulkan: %s\n"
                 "ram_mb: %llu\n"
                 "resolution: %ux%u (%s), msaa %d, vsync %s, fps cap %d, shadows %d, fov %d\n"
                 "signed_in: %s\n"
                 "trigger: %s at %s\n",
                 CZ_GAME_VERSION, os.c_str(), cpu.c_str(), std::thread::hardware_concurrency(),
                 g_gpu.c_str(), g_driver.c_str(), vk, (unsigned long long)ram, rw, rh,
                 DisplayModeName(Settings_DisplayMode()), Settings_Msaa(),
                 Settings_VSync() ? "on" : "off", Settings_FpsCap(), Settings_ShadowTier(),
                 Settings_Fov(), CzXlive_SignedIn() ? "yes" : "no", p->trigger.c_str(),
                 Rfc3339(p->wall).c_str());
        sys = b;
    }
    ok = ok && WriteFile(partial / "system.txt", sys.data(), sys.size());
    files.push_back({ "system.txt", "text/plain", "OS, CPU, GPU, driver, RAM, settings", sys.size() });
    total += sys.size();

    // capture.json, last.
    std::string j = "{\n  \"version\": 1,\n";
    {
        char b[512];
        snprintf(b, sizeof b, "  \"title_id\": \"58410a8d\",\n  \"game\": \"Dead Rising 2: Case Zero\",\n"
                              "  \"game_version\": \"%s\",\n  \"captured_at\": \"%s\",\n"
                              "  \"trigger\": \"%s\",\n  \"files\": [\n",
                 JsonEscape(CZ_GAME_VERSION).c_str(), Rfc3339(p->wall).c_str(),
                 JsonEscape(p->trigger).c_str());
        j += b;
    }
    for (size_t i = 0; i < files.size(); i++)
    {
        j += "    { \"name\": \"" + files[i].name + "\", \"type\": \"" + files[i].type +
             "\", \"what\": \"" + JsonEscape(files[i].what) + "\" }" +
             (i + 1 < files.size() ? ",\n" : "\n");
    }
    {
        char b[1024];
        snprintf(b, sizeof b,
                 "  ],\n  \"system\": { \"os\": \"%s\", \"cpu\": \"%s\", \"gpu\": \"%s\", "
                 "\"driver\": \"%s\", \"ram_mb\": %llu, \"vulkan\": \"%s\", "
                 "\"resolution\": \"%ux%u\", \"display_mode\": \"%s\", \"game_version\": \"%s\" }\n}\n",
                 JsonEscape(os).c_str(), JsonEscape(cpu).c_str(), JsonEscape(g_gpu).c_str(),
                 JsonEscape(g_driver).c_str(), (unsigned long long)ram, vk, rw, rh,
                 DisplayModeName(Settings_DisplayMode()), JsonEscape(CZ_GAME_VERSION).c_str());
        j += b;
    }
    ok = ok && WriteFile(partial / "capture.json", j.data(), j.size());

    if (!ok)
    {
        fprintf(stderr, "[bugreport] could not write every file of %s — left as .partial "
                        "(the launcher will not list it; it is pruned after ten minutes)\n",
                name.c_str());
        return;
    }
    fs::rename(partial, final, ec);
    if (ec)
    {
        fprintf(stderr, "[bugreport] rename to %s failed: %s\n", final.string().c_str(),
                ec.message().c_str());
        return;
    }
    fprintf(stderr, "[bugreport] %s: wrote %s (%zu files, %zu KiB) — send it from the "
                    "XenonLive launcher's Issues tab, or delete it there\n",
            p->trigger.c_str(), final.string().c_str(), files.size(), total / 1024);
    CwOverlay_Notify("Bug report captured — send it from the XenonLive launcher's Issues tab",
                     6.0, "bugreport");
    Prune();
}

void WorkerLoop()
{
    for (;;)
    {
        std::unique_ptr<Pending> job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return g_quit.load() || g_pending != nullptr; });
            if (!g_pending)
                return;                       // quitting with nothing in flight
            const auto due = g_pending->t0 + kAfter;
            // Wait out the 15 s (or the quit), then take the job.
            g_cv.wait_until(lock, due, [] { return g_quit.load(); });
            job = std::move(g_pending);
            g_wantPixels.store(false, std::memory_order_release);
        }
        WriteReport(std::move(job));
        if (g_quit.load())
            return;
    }
}
} // namespace

void BugReport_Init()
{
    if (const char* e = getenv("CZ_BUG_REPORTS"); e && *e == '0')
    {
        fprintf(stderr, "[bugreport] off (CZ_BUG_REPORTS=0): F8/F9 write no capture for the launcher\n");
        return;
    }
    std::string dir = getenv("CZ_BUG_REPORT_DIR") ? getenv("CZ_BUG_REPORT_DIR") : CzXlive_CapturesDir();
    g_dir = dir;
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    if (ec)
    {
        fprintf(stderr, "[bugreport] cannot create %s (%s): F8/F9 write no capture\n",
                g_dir.string().c_str(), ec.message().c_str());
        return;
    }
    g_enabled = true;
    fprintf(stderr, "[bugreport] F9 (one frame) / F8 (three frames) capture a bug report for the "
                    "XenonLive launcher's Issues tab into %s — the picture, the log 60 s before "
                    "and 15 s after, the machine. CZ_BUG_REPORTS=0 turns it off.\n",
            g_dir.string().c_str());
    Prune();
}

void BugReport_SetGpu(const char* device, const char* driver, uint32_t apiVersion)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gpu = device ? device : "unknown";
    g_driver = driver ? driver : "unknown";
    g_apiVersion = apiVersion;
}

void BugReport_Request(const char* trigger, unsigned frames)
{
    if (!g_enabled)
        return;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending)
    {
        fprintf(stderr, "[bugreport] %s pressed while a capture is in flight — folded into it "
                        "(its window ends %.0f s from now)\n", trigger,
                std::chrono::duration<double>(g_pending->t0 + kAfter - now).count());
        return;
    }
    auto p = std::make_unique<Pending>();
    p->trigger = trigger;
    p->t0 = now;
    p->wall = std::chrono::system_clock::now();
    p->wantFrames = std::max(1u, std::min(3u, frames));
    p->nextFrame = 0;
    p->pixelDeadline = now + std::chrono::seconds(4);
    fprintf(stderr, "[bugreport] %s pressed: capturing %u frame%s, the log 60 s before and 15 s "
                    "after, and the machine — written in 15 s\n",
            trigger, p->wantFrames, p->wantFrames == 1 ? "" : "s");
    g_pending = std::move(p);
    g_wantPixels.store(true, std::memory_order_release);
    if (!g_worker.joinable())
        g_worker = std::thread(WorkerLoop);
    g_cv.notify_all();
}

bool BugReport_WantsPixels()
{
    return g_wantPixels.load(std::memory_order_acquire);
}

void BugReport_OfferPixels(const uint8_t* rgba, uint32_t width, uint32_t height, uint64_t frame)
{
    if (!g_wantPixels.load(std::memory_order_acquire) || !rgba || !width || !height)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Pending* p = g_pending.get();
    if (!p || p->frames.size() >= p->wantFrames)
    {
        g_wantPixels.store(false, std::memory_order_release);
        return;
    }
    if (Clock::now() > p->pixelDeadline)
    {
        g_wantPixels.store(false, std::memory_order_release);
        return;
    }
    if (frame < p->nextFrame)
        return;
    Frame f;
    f.w = width;
    f.h = height;
    f.frame = frame;
    f.rgb.resize(size_t(width) * height * 3);
    const size_t n = size_t(width) * height;
    for (size_t i = 0; i < n; i++)
    {
        f.rgb[i * 3 + 0] = rgba[i * 4 + 0];
        f.rgb[i * 3 + 1] = rgba[i * 4 + 1];
        f.rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    p->frames.push_back(std::move(f));
    p->nextFrame = frame + kBurstSpacingFrames;
    if (p->frames.size() >= p->wantFrames)
        g_wantPixels.store(false, std::memory_order_release);
}

void BugReport_Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quit.store(true);
    }
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
}
