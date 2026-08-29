// Release D.3 — the first-run shader build. See shader_prebuild.h for the contract.
//
// Two decoders are ported here from tools that remain the reference implementations:
//
//   * the `.big` archive index — docs/big-archive-format.md, tools/big_list.py.
//     LITTLE-endian, which is worth stating again because everything else on this
//     console is big-endian: a reader that assumes BE gets a plausible-looking magic
//     and nonsense everywhere after. Names are NUL-terminated in a fixed-width table
//     whose width is derived, never hardcoded (the 40-byte-stride mistake in the
//     format doc's history is exactly why the index stride comes from names_offset).
//   * the `.po` shader object — tools/vo_extract_microcode.py, D.1's container rule.
//     Every bound is checked before use: these objects come off a disc image the
//     player supplies, and a first-run pass that segfaults on a malformed one is
//     worse than one that skips it BY NAME.
//
// The pass dedupes by content hash (1,280 objects -> ~1,265 distinct shaders), skips
// names the cache already holds (which is the whole resume mechanism), and translates
// the rest on a whole-machine pool — this runs before the guest exists, so there is no
// game to leave cores for.
#include "shader_prebuild.h"
#include "shader_translator.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ShaderPrebuild
{
namespace
{
// The same FNV-1a the runtime keys the cache with (pm4.cpp's BindShader and
// tools/vo_extract_microcode.py both spell it identically).
static uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t Le32(const std::vector<uint8_t>& b, size_t off)
{
    return uint32_t(b[off]) | (uint32_t(b[off + 1]) << 8) | (uint32_t(b[off + 2]) << 16) |
           (uint32_t(b[off + 3]) << 24);
}

static uint32_t Be32(const uint8_t* b, size_t off)
{
    return (uint32_t(b[off]) << 24) | (uint32_t(b[off + 1]) << 16) |
           (uint32_t(b[off + 2]) << 8) | uint32_t(b[off + 3]);
}

struct DiscShader
{
    std::string object; // the .po entry name, for failure messages
    uint64_t hash = 0;
    const uint8_t* ucode = nullptr; // into the slurped bank
    uint32_t size = 0;
};

// Parse the bank and decode every .po entry's microcode range. Returns false only if
// the ARCHIVE is unreadable; individual objects that refuse to decode are named and
// counted, because "a format we have not seen" and "a decode bug" both deserve a line
// and neither deserves a crash.
static bool CollectDiscShaders(const std::filesystem::path& psBank,
                               std::vector<uint8_t>& bank,
                               std::vector<DiscShader>& out, uint32_t& refused)
{
    std::ifstream f(psBank, std::ios::binary);
    if (!f)
    {
        fprintf(stderr, "[prebuild] cannot open %s\n", psBank.string().c_str());
        return false;
    }
    bank.assign(std::istreambuf_iterator<char>(f), {});
    if (bank.size() < 0x18 || bank[0] != 0x06 || bank[1] != 0x05 || bank[2] != 0x04 ||
        bank[3] != 0x03)
    {
        fprintf(stderr, "[prebuild] %s is not a .big archive (bad magic)\n",
                psBank.string().c_str());
        return false;
    }
    const uint32_t entryCount = Le32(bank, 0x0C);
    const uint32_t namesOffset = Le32(bank, 0x14);
    if (0x18 + size_t(entryCount) * 28 != namesOffset || namesOffset >= bank.size())
    {
        fprintf(stderr, "[prebuild] %s: index self-check failed (%u entries, names at "
                        "0x%X)\n",
                psBank.string().c_str(), entryCount, namesOffset);
        return false;
    }

    std::vector<uint64_t> seen;
    for (uint32_t i = 0; i < entryCount; i++)
    {
        const size_t e = 0x18 + size_t(i) * 28;
        const uint32_t nameOff = Le32(bank, e + 0x00);
        const uint32_t size = Le32(bank, e + 0x08);
        const uint32_t dataOff = Le32(bank, e + 0x10);
        if (nameOff >= bank.size() || size_t(dataOff) + size > bank.size())
        {
            ++refused;
            fprintf(stderr, "[prebuild] entry %u: bounds outside the archive\n", i);
            continue;
        }
        std::string name(reinterpret_cast<const char*>(bank.data() + nameOff));
        if (name.size() < 3 || name.compare(name.size() - 3, 3, ".po") != 0)
            continue; // the ps bank also carries non-object entries; not a refusal

        // D.1's container rule, bounds checked at every step.
        const uint8_t* obj = bank.data() + dataOff;
        if (size < 0x20 || Be32(obj, 0) != 0x102A1100)
        {
            ++refused;
            fprintf(stderr, "[prebuild] %s: not a pixel-shader object\n", name.c_str());
            continue;
        }
        const uint32_t blob = Be32(obj, 0x04);
        const uint32_t blobLen = Be32(obj, 0x08);
        const uint32_t desc = Be32(obj, 0x18);
        if (blob >= size || blob + blobLen != size || desc + 4 > size)
        {
            ++refused;
            fprintf(stderr, "[prebuild] %s: container bounds refuse to parse\n",
                    name.c_str());
            continue;
        }
        const uint32_t start = blob + Be32(obj, desc);
        if (start >= size || (start % 4))
        {
            ++refused;
            fprintf(stderr, "[prebuild] %s: microcode start %u refuses to parse\n",
                    name.c_str(), start);
            continue;
        }
        DiscShader s;
        s.object = name;
        s.ucode = obj + start;
        s.size = size - start;
        s.hash = Fnv1a(s.ucode, s.size);
        if (std::find(seen.begin(), seen.end(), s.hash) != seen.end())
            continue; // 1,280 objects carry ~1,265 distinct shaders
        seen.push_back(s.hash);
        out.push_back(std::move(s));
    }
    return true;
}
} // namespace

bool WantedAtBoot(const std::filesystem::path& cacheDir)
{
    std::error_code ec;
    if (std::filesystem::exists(cacheDir / "disc_prebuild.done", ec))
        return false; // a finished pass; nothing owed
    if (std::filesystem::exists(cacheDir / "disc_prebuild.started", ec))
        return true; // interrupted first run — finish it
    // Neither marker. An empty or missing directory is a player's first launch; a
    // populated one is a developer cache built from dumps, which the gates count and
    // which this pass must never silently grow.
    if (!std::filesystem::is_directory(cacheDir, ec))
        return true;
    for (const auto& e : std::filesystem::directory_iterator(cacheDir, ec))
        if (e.path().extension() == ".spv")
            return false;
    return true;
}

int BuildFromDisc(const std::filesystem::path& psBank,
                  const std::filesystem::path& cacheDir,
                  const std::function<void(unsigned, size_t)>& progress)
{
    std::vector<uint8_t> bank;
    std::vector<DiscShader> shaders;
    uint32_t refused = 0;
    if (!CollectDiscShaders(psBank, bank, shaders, refused))
        return 1;

    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    { std::ofstream m(cacheDir / "disc_prebuild.started"); m << psBank.string() << "\n"; }

    // Resume = skip what a previous pass already wrote. The pair is the unit: a .spv
    // without its sidecar would be silently dropped at load, so only a complete pair
    // counts as done.
    std::vector<DiscShader> todo;
    uint32_t already = 0;
    for (auto& s : shaders)
    {
        char name[32];
        snprintf(name, sizeof name, "ps_%016llx", (unsigned long long)s.hash);
        if (std::filesystem::exists(cacheDir / (std::string(name) + ".spv"), ec) &&
            std::filesystem::exists(cacheDir / (std::string(name) + ".meta.json"), ec))
            ++already;
        else
            todo.push_back(s);
    }
    fprintf(stderr, "[prebuild] %s: %zu distinct pixel shaders (%u objects refused), "
                    "%u already in the cache, %zu to translate\n",
            psBank.filename().string().c_str(), shaders.size(), refused, already,
            todo.size());

    std::atomic<size_t> next{ 0 };
    std::atomic<uint32_t> done{ 0 };
    std::mutex failMx;
    std::vector<std::string> fails;
    // `isCaller` marks the one worker running on the CALLING thread: only it may
    // fire `progress`, because the consumer is the SDL progress window and SDL
    // draws only from the thread that created it (window.h's standing rule).
    auto worker = [&](bool isCaller) {
        for (size_t i; (i = next.fetch_add(1)) < todo.size();)
        {
            if (isCaller && progress)
                progress(done.load(), todo.size());
            const DiscShader& s = todo[i];
            char name[32];
            snprintf(name, sizeof name, "ps_%016llx", (unsigned long long)s.hash);
            ShaderTranslator::Result r;
            std::string err;
            if (!ShaderTranslator::Translate(name, s.ucode, s.size, r, err))
            {
                std::lock_guard<std::mutex> lk(failMx);
                fails.push_back(s.object + " (" + name + "): " + err);
                continue;
            }
            if (!ShaderTranslator::WritePair(cacheDir, name, r))
            {
                std::lock_guard<std::mutex> lk(failMx);
                fails.push_back(s.object + " (" + name + "): write failed");
                continue;
            }
            const uint32_t n = done.fetch_add(1) + 1;
            // The progress §2.3 asks for, at a cadence a console can carry. A player
            // watching this sees it move; a log keeps the whole trail.
            if ((n % 64) == 0 || n == todo.size())
                fprintf(stderr, "[prebuild] preparing shaders... %u of %zu\n", n,
                        todo.size());
        }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < std::min<size_t>(hw, todo.size() ? todo.size() : 1); t++)
        pool.emplace_back(worker, /*isCaller=*/false);
    worker(/*isCaller=*/true);
    for (auto& t : pool)
        t.join();
    if (progress)
        progress(done.load(), todo.size());

    for (auto& f : fails)
        fprintf(stderr, "[prebuild] FAILED: %s\n", f.c_str());
    fprintf(stderr, "[prebuild] %u translated, %u already present, %zu failed\n",
            done.load(), already, fails.size());
    if (fails.empty())
    {
        std::ofstream m(cacheDir / "disc_prebuild.done");
        m << shaders.size() << " shaders from " << psBank.string() << "\n";
        return 0;
    }
    // The started marker stays, so the next boot tries the failures again rather than
    // declaring the pass finished with holes in it.
    return 1;
}
} // namespace ShaderPrebuild
