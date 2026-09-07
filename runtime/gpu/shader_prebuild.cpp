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
//   * the `.po`/`.vo` shader object — tools/vo_extract_microcode.py, D.1's container
//     rule. Every bound is checked before use: these objects come off a disc image the
//     player supplies, and a first-run pass that segfaults on a malformed one is
//     worse than one that skips it BY NAME.
//
// TWO PASSES SINCE PART 102, one per shader stage, on one worker pool:
//
//   PIXEL: the `.po` objects hold the runtime microcode verbatim (343 of 345 in the
//   cache reproduced byte-for-byte, D.1), so every distinct object translates as is.
//
//   VERTEX: the `.vo` objects are TEMPLATES — the title patches each one's vertex-fetch
//   instructions at bind time from the vertex declaration, so 0 of 104 runtime vertex
//   shaders appear on disc verbatim. But 102 of them are a same-length template with
//   2-32 dwords rewritten, and `vs_recipes.bin` (tools/vs_recipes.py, shipped beside
//   the exe like prewarm.keys) says which template and which dwords. Applying a recipe
//   to the player's own template reproduces the runtime microcode, and the gate is the
//   renderer's own cache key: the result must FNV-1a to the hash the recipe claims or
//   it is REFUSED by name and never translated. Before this pass the vertex half only
//   existed after the draw that first bound each shader, and every draw wanting a
//   pipeline on it was skipped until translation finished — the session-one pop-in
//   (phase5-notes §6er: 234,849 draws on the outdoor route). With it, the pre-warm
//   seed can build every pipeline at boot on session one. CZ_NO_VS_RECIPES=1 is the
//   control arm (the pixel-only pass, exactly as parts 84-101 shipped it).
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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

static uint64_t Le64(const std::vector<uint8_t>& b, size_t off)
{
    return uint64_t(Le32(b, off)) | (uint64_t(Le32(b, off + 4)) << 32);
}

static uint32_t Be32(const uint8_t* b, size_t off)
{
    return (uint32_t(b[off]) << 24) | (uint32_t(b[off + 1]) << 16) |
           (uint32_t(b[off + 2]) << 8) | uint32_t(b[off + 3]);
}

struct DiscShader
{
    std::string object; // the .po/.vo entry name (or the recipe), for failure messages
    const char* prefix = "ps_";
    uint64_t hash = 0;
    const uint8_t* ucode = nullptr; // into the slurped bank, when `owned` is empty
    uint32_t size = 0;
    std::vector<uint8_t> owned;     // a patched vertex shader owns its bytes
    const uint8_t* bytes() const { return owned.empty() ? ucode : owned.data(); }
    std::string name() const
    {
        char n[32];
        snprintf(n, sizeof n, "%s%016llx", prefix, (unsigned long long)hash);
        return n;
    }
};

struct BankKind
{
    const char* ext;    // ".po" / ".vo"
    uint32_t magic;     // 0x102A1100 pixel / 0x102A1101 vertex
    const char* prefix; // "ps_" / "vs_"
    const char* what;   // for messages
};
constexpr BankKind kPixel{ ".po", 0x102A1100u, "ps_", "pixel-shader" };
constexpr BankKind kVertex{ ".vo", 0x102A1101u, "vs_", "vertex-shader" };

// Parse the bank and decode every object's microcode range. Returns false only if
// the ARCHIVE is unreadable; individual objects that refuse to decode are named and
// counted, because "a format we have not seen" and "a decode bug" both deserve a line
// and neither deserves a crash.
static bool CollectDiscShaders(const std::filesystem::path& bankPath, const BankKind& kind,
                               std::vector<uint8_t>& bank, std::vector<DiscShader>& out,
                               uint32_t& refused)
{
    std::ifstream f(bankPath, std::ios::binary);
    if (!f)
    {
        fprintf(stderr, "[prebuild] cannot open %s\n", bankPath.string().c_str());
        return false;
    }
    bank.assign(std::istreambuf_iterator<char>(f), {});
    if (bank.size() < 0x18 || bank[0] != 0x06 || bank[1] != 0x05 || bank[2] != 0x04 ||
        bank[3] != 0x03)
    {
        fprintf(stderr, "[prebuild] %s is not a .big archive (bad magic)\n",
                bankPath.string().c_str());
        return false;
    }
    const uint32_t entryCount = Le32(bank, 0x0C);
    const uint32_t namesOffset = Le32(bank, 0x14);
    if (0x18 + size_t(entryCount) * 28 != namesOffset || namesOffset >= bank.size())
    {
        fprintf(stderr, "[prebuild] %s: index self-check failed (%u entries, names at "
                        "0x%X)\n",
                bankPath.string().c_str(), entryCount, namesOffset);
        return false;
    }

    const size_t extLen = strlen(kind.ext);
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
        if (name.size() < extLen || name.compare(name.size() - extLen, extLen, kind.ext) != 0)
            continue; // the banks also carry non-object entries; not a refusal

        // D.1's container rule, bounds checked at every step.
        const uint8_t* obj = bank.data() + dataOff;
        if (size < 0x20 || Be32(obj, 0) != kind.magic)
        {
            ++refused;
            fprintf(stderr, "[prebuild] %s: not a %s object\n", name.c_str(), kind.what);
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
        s.prefix = kind.prefix;
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

constexpr uint32_t kRecipeMagic = 0x5256435Au; // 'ZCVR'
constexpr uint32_t kRecipeVersion = 1;

// Apply `vs_recipes.bin` to the vertex templates: each recipe names a template by hash,
// the runtime hash it must produce, and the dwords to overwrite. A recipe whose template
// this disc lacks, whose bounds do not fit, or whose result does not hash as claimed is
// REFUSED by name — a shader that fails its own hash is a shader we must not translate,
// because the renderer would then bind a wrong vertex format to the real draw.
// Returns the number of runtime vertex shaders produced; -1 if the file is unusable.
static int ApplyRecipes(const std::filesystem::path& recipePath,
                        const std::vector<DiscShader>& templates,
                        std::vector<DiscShader>& out, uint32_t& refused,
                        uint32_t& recipeCount)
{
    std::ifstream f(recipePath, std::ios::binary);
    if (!f)
        return -1;
    std::vector<uint8_t> r((std::istreambuf_iterator<char>(f)), {});
    if (r.size() < 12 || Le32(r, 0) != kRecipeMagic || Le32(r, 4) != kRecipeVersion)
    {
        fprintf(stderr, "[prebuild] %s is not a v%u recipe file — ignoring\n",
                recipePath.string().c_str(), kRecipeVersion);
        return -1;
    }
    recipeCount = Le32(r, 8);
    std::unordered_map<uint64_t, const DiscShader*> byHash;
    for (const DiscShader& t : templates)
        byHash.emplace(t.hash, &t);

    int produced = 0;
    size_t off = 12;
    for (uint32_t i = 0; i < recipeCount; i++)
    {
        if (off + 24 > r.size())
        {
            fprintf(stderr, "[prebuild] %s: truncated at recipe %u\n",
                    recipePath.string().c_str(), i);
            ++refused;
            return produced;
        }
        const uint64_t templateHash = Le64(r, off);
        const uint64_t runtimeHash = Le64(r, off + 8);
        const uint32_t dwords = Le32(r, off + 16);
        const uint32_t patches = Le32(r, off + 20);
        off += 24;
        if (off + size_t(patches) * 8 > r.size())
        {
            fprintf(stderr, "[prebuild] %s: truncated inside recipe %u\n",
                    recipePath.string().c_str(), i);
            ++refused;
            return produced;
        }
        const size_t patchOff = off;
        off += size_t(patches) * 8;

        char name[32];
        snprintf(name, sizeof name, "vs_%016llx", (unsigned long long)runtimeHash);
        auto t = byHash.find(templateHash);
        if (t == byHash.end())
        {
            ++refused;
            fprintf(stderr, "[prebuild] %s: template vs_%016llx is not on this disc\n",
                    name, (unsigned long long)templateHash);
            continue;
        }
        if (t->second->size != dwords * 4)
        {
            ++refused;
            fprintf(stderr, "[prebuild] %s: template is %u dwords, recipe expects %u\n",
                    name, t->second->size / 4, dwords);
            continue;
        }
        DiscShader s;
        s.object = std::string("recipe ") + name;
        s.prefix = "vs_";
        s.owned.assign(t->second->ucode, t->second->ucode + t->second->size);
        bool bad = false;
        for (uint32_t p = 0; p < patches && !bad; p++)
        {
            const uint32_t idx = Le32(r, patchOff + size_t(p) * 8);
            if (idx >= dwords)
            {
                ++refused;
                fprintf(stderr, "[prebuild] %s: patch index %u past %u dwords\n", name,
                        idx, dwords);
                bad = true;
                break;
            }
            // The value is stored as the 4 wire bytes; no swap on either side.
            memcpy(s.owned.data() + size_t(idx) * 4, r.data() + patchOff + size_t(p) * 8 + 4,
                   4);
        }
        if (bad)
            continue;
        s.size = uint32_t(s.owned.size());
        s.hash = Fnv1a(s.owned.data(), s.owned.size());
        if (s.hash != runtimeHash)
        {
            ++refused;
            fprintf(stderr, "[prebuild] %s: REFUSED — the patched template hashes to "
                            "%016llx, not the runtime shader the recipe claims (this disc's "
                            "template differs from the one the recipe was built on)\n",
                    name, (unsigned long long)s.hash);
            continue;
        }
        out.push_back(std::move(s));
        ++produced;
    }
    return produced;
}

static bool Exists(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}
} // namespace

bool WantedAtBoot(const std::filesystem::path& cacheDir, const std::filesystem::path& recipes)
{
    std::error_code ec;
    if (Exists(cacheDir / "disc_prebuild.done"))
    {
        // The pixel pass is finished. The vertex pass (part 102) is a later addition
        // with its own marker, so an install that predates it — or one whose recipe
        // file was updated — runs it once. A recipe file that does not exist owes
        // nothing; the pixel-only cache is complete for what it can be.
        if (!recipes.empty() && Exists(recipes))
        {
            std::ifstream m(cacheDir / "vs_recipes.done");
            unsigned long long stamped = ~0ull;
            if (m)
                m >> stamped; // the recipe file's size when the pass last finished
            if (stamped != (unsigned long long)std::filesystem::file_size(recipes, ec))
                return true;
        }
        return false; // a finished pass; nothing owed
    }
    if (Exists(cacheDir / "disc_prebuild.started"))
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
                  const std::function<void(unsigned, size_t)>& progress,
                  const std::filesystem::path& vsBank,
                  const std::filesystem::path& recipes)
{
    std::vector<uint8_t> bank;
    std::vector<DiscShader> shaders;
    uint32_t refused = 0;
    if (!CollectDiscShaders(psBank, kPixel, bank, shaders, refused))
        return 1;
    const size_t pixelCount = shaders.size();

    // THE VERTEX PASS. Templates come from the vs bank on the player's disc, the
    // patch list from the shipped recipe file; both are optional inputs (a CLI
    // caller that names neither gets the pixel-only pass), and the arm is the env.
    std::vector<uint8_t> vsBankBytes;
    std::vector<DiscShader> templates;
    uint32_t vsRefused = 0, recipeRefused = 0, recipeCount = 0;
    int produced = -1;
    bool vertexPass = false;
    if (const char* off = getenv("CZ_NO_VS_RECIPES"); off && *off && strcmp(off, "0") != 0)
        fprintf(stderr, "[prebuild] CZ_NO_VS_RECIPES=1 — pixel-only pass; the vertex half "
                        "comes from first-sight translation (the pre-part-102 behaviour, "
                        "and the control arm)\n");
    else if (!vsBank.empty() && !recipes.empty())
    {
        if (!Exists(recipes))
            fprintf(stderr, "[prebuild] no %s — vertex shaders will translate at first "
                            "sight instead\n", recipes.string().c_str());
        else if (!Exists(vsBank))
            fprintf(stderr, "[prebuild] no %s — vertex shaders will translate at first "
                            "sight instead\n", vsBank.string().c_str());
        else if (CollectDiscShaders(vsBank, kVertex, vsBankBytes, templates, vsRefused))
        {
            produced = ApplyRecipes(recipes, templates, shaders, recipeRefused, recipeCount);
            vertexPass = produced >= 0;
            if (vertexPass)
                fprintf(stderr, "[prebuild] %s: %u recipes over %zu disc templates (%u "
                                "objects refused) -> %d runtime vertex shaders reproduced, "
                                "%u recipe(s) refused\n",
                        recipes.filename().string().c_str(), recipeCount, templates.size(),
                        vsRefused, produced, recipeRefused);
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    { std::ofstream m(cacheDir / "disc_prebuild.started"); m << psBank.string() << "\n"; }

    // Resume = skip what a previous pass already wrote. The pair is the unit: a .spv
    // without its sidecar would be silently dropped at load, so only a complete pair
    // counts as done.
    std::vector<DiscShader> todo;
    uint32_t already = 0, alreadyVs = 0;
    size_t todoVs = 0;
    for (auto& s : shaders)
    {
        const std::string name = s.name();
        const bool vs = s.prefix[0] == 'v';
        if (Exists(cacheDir / (name + ".spv")) && Exists(cacheDir / (name + ".meta.json")))
            ++(vs ? alreadyVs : already);
        else
        {
            todoVs += vs ? 1 : 0;
            todo.push_back(std::move(s));
        }
    }
    fprintf(stderr, "[prebuild] %s: %zu distinct pixel shaders (%u objects refused), "
                    "%u already in the cache, %zu to translate\n",
            psBank.filename().string().c_str(), pixelCount, refused, already,
            todo.size() - todoVs);
    if (vertexPass)
        fprintf(stderr, "[prebuild] %s: %d runtime vertex shaders, %u already in the "
                        "cache, %zu to translate\n",
                vsBank.filename().string().c_str(), produced, alreadyVs, todoVs);

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
            const std::string name = s.name();
            ShaderTranslator::Result r;
            std::string err;
            if (!ShaderTranslator::Translate(name, s.bytes(), s.size, r, err))
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
            done.load(), already + alreadyVs, fails.size());
    if (fails.empty())
    {
        std::ofstream m(cacheDir / "disc_prebuild.done");
        m << pixelCount << " shaders from " << psBank.string() << "\n";
        if (vertexPass)
        {
            // Refused recipes are NOT failures of this pass — they are a disc whose
            // template differs, named above, and re-running would only refuse them
            // again. The marker records the recipe file's SIZE so a changed file re-runs.
            std::ofstream v(cacheDir / "vs_recipes.done");
            v << (unsigned long long)std::filesystem::file_size(recipes, ec) << " bytes: "
              << produced << " of " << recipeCount << " recipes from " << recipes.string()
              << "\n";
        }
        return 0;
    }
    // The started marker stays, so the next boot tries the failures again rather than
    // declaring the pass finished with holes in it.
    return 1;
}
} // namespace ShaderPrebuild
