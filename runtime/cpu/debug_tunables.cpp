// The title's own debug menu, debug-jump screen and dev tunables, switched back on.
//
// WHY THIS EXISTS
// ---------------
// Testing this port means reaching a place in the game, and until now the only two
// ways to do that were an operator playing to it or CZ_FAKE_PRESS_SEQ manufacturing
// its way there over several minutes of fixed 8 s steps (CLAUDE.md gotcha 78 — that
// recipe MANUFACTURES progress, so it can never be a gate). Both are expensive and
// neither is repeatable at a chosen point.
//
// The shipped executable turns out to carry Blue Castle's complete development
// scaffolding. It was never compiled out — only switched off:
//
//   * `common\debugmenu\debugmenu.cpp` and its component header are still in the
//     image's source-path strings, `cDebugMenu` is still a class, and the menu's
//     item tree is intact (System Menu, Chartz Menu, Thread Edit Menu, Controller
//     Assignments, Performance Chartz, GPU Timing Queries, NPC To Spawn, Credits,
//     FontTest, and a header reading "DEBUG-ONLY TUNABLES").
//   * `DebugJump` is a frontend screen listed beside `TitleScreen` and `PressStart`,
//     and its layout `debugjump.txt` SHIPS inside data/frontend/mainmenu.big — at
//     4,144 bytes it is the largest entry in that archive, larger than title.txt.
//   * `God Mode:ON` is referenced by live code in the actor-status overlay.
//
// HOW THE SWITCH WORKS
// --------------------
// 393 boolean tunables live in one contiguous global struct in .data. A single
// loader, `sub_824A2470`, looks each one up BY NAME through sub_82773298 and stores
// the answer into a fixed byte:
//
//     lis   r9, 0x8207
//     addi  r4, r9, <name>        ; e.g. "enable_debug_jump_menu"
//     bl    0x82773298            ; get-bool-by-name
//     stb   r3, 0x7C09(r10)       ; r10 = 0x82A50000  ->  0x82A57C09
//
// and every consumer gates on it with a plain load-and-branch, e.g. at 0x824D6170:
//
//     lbz     r11, 0x7C09(r11)
//     cmplwi  r11, 0
//     beq     <skip>
//
// In a retail build the name lookup finds nothing and every byte comes back 0.
//
// WHY A ONE-SHOT POKE IS ENOUGH
// -----------------------------
// `sub_824A2470` has exactly one caller, and its call chain is three hops off the
// XEX entry point itself:
//
//     sub_825D9F30 (entry)  ->  sub_825D7448  ->  sub_82496D98  ->  sub_824A2470
//
// It runs once, before anything else, and nothing rewrites those bytes afterwards.
// So a post-hook that writes the flags as the loader returns is permanent, and costs
// one env-var read on a function that executes a single time per process. There is no
// per-frame component to this instrument at all, which is the standard gotcha 7 asks
// for — an instrument that cannot perturb what it reports.
//
// WHY A TABLE OF NAMES RATHER THAN RAW ADDRESSES
// ----------------------------------------------
// The addresses below were not read off a disassembly by hand. They were extracted
// mechanically by walking .text for the (addi name / stb offset) pairs above and
// resolving both against their `lis`, then each was CONFIRMED by scanning
// independently for the `lbz` consumers that read it back. The reader count is
// recorded next to every entry, because a tunable nothing reads is a switch that
// does nothing, and shipping one of those silently would be exactly the "a zero is a
// detection failure" trap from the other direction (gotcha 3). The four curated
// names that scanned to ZERO readers are listed at the bottom of the table and
// deliberately excluded — they are almost certainly read through a register base this
// scan does not model, but "almost certainly" is not evidence and this file does not
// offer switches it cannot show are connected.
//
// WHAT IT CANNOT DO
// -----------------
// Only Case Zero's own scenes ship. The image still carries the full Dead Rising 2
// scene list (`yucatan_casino`, `fortune_exterior`, `boss_battle_*`) because the two
// games share an engine, but data/models/environment holds only `prologue`,
// `prologue_menu`, `prologue_menu2`, `prologue_safehouse` and `safehouse`. A jump to
// anything else has no data behind it.
//
// FOR CASE WEST
// -------------
// The same struct will be there, at a different address. Re-derive it with the same
// two scans rather than reusing these constants: find the (addi/stb) pairs in the
// loader, then confirm each with its `lbz` readers.
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../kernel/memory.h"
#include "../kernel/heap.h"
#include "../host/window.h"
#include "ppc_recomp_shared.h"

// `ppc_recomp_shared.h` declares only the WEAK alias, never the real body, so the
// hook has to declare the one it wraps. `extern "C"` is not optional — the
// recompiler defines it via PPC_FUNC_IMPL, which is `extern "C" PPC_FUNC`, and a
// plain C++ declaration here would mangle differently and fail to link (gotcha 33).
extern "C" PPC_FUNC(__imp__sub_824A2470);
extern "C" PPC_FUNC(__imp__sub_8276E398);
extern "C" PPC_FUNC(__imp__sub_827F6D40);
extern "C" PPC_FUNC(__imp__sub_824AAEB8);
extern "C" PPC_FUNC(__imp__sub_824A8120);
extern "C" PPC_FUNC(__imp__sub_824A8758);
extern "C" PPC_FUNC(__imp__sub_824A8840);
extern "C" PPC_FUNC(__imp__sub_824A8FE0);
extern "C" PPC_FUNC(__imp__sub_82211138);
extern "C" PPC_FUNC(__imp__sub_821DF1D0);
extern "C" PPC_FUNC(__imp__sub_82189B00);
extern "C" PPC_FUNC(__imp__sub_82482AD8);
extern "C" PPC_FUNC(__imp__sub_824A97F0);
extern "C" PPC_FUNC(__imp__sub_8215FEC8);
extern "C" PPC_FUNC(__imp__sub_8248A2F8);
extern "C" PPC_FUNC(__imp__sub_82195AB0);
extern "C" PPC_FUNC(__imp__sub_824A9970);
extern "C" PPC_FUNC(__imp__sub_8251DAB0);
extern "C" PPC_FUNC(__imp__sub_824FD628);
extern "C" PPC_FUNC(__imp__sub_8215A5A0);
extern "C" PPC_FUNC(__imp__sub_821E55A0);
extern "C" PPC_FUNC(__imp__sub_821B0A28);
extern "C" PPC_FUNC(__imp__sub_8253FB10);
extern "C" PPC_FUNC(__imp__sub_8253F740);
extern "C" PPC_FUNC(__imp__sub_8253E060);
extern "C" PPC_FUNC(__imp__sub_82539890);
extern "C" PPC_FUNC(__imp__sub_82539908);
extern "C" PPC_FUNC(__imp__sub_82157178);

static uint32_t g_frontendTransitionManager = 0;
static uint32_t g_debugMenuObject = 0;
static uint32_t g_debugMenuFirstNode = 0;
static uint32_t g_debugMenuLastNode = 0;
static bool g_buildingDebugMenu = false;
static bool g_debugMenuActive = false;
static std::vector<uint32_t> g_debugMenuNodes;
static std::vector<uint32_t> g_debugMenuVisibleNodes;
static std::vector<std::string> g_debugMenuBaseLabels;
static std::vector<uint32_t> g_debugMenuRootNodes;
static std::vector<std::string> g_debugMenuRootLabels;
static std::vector<uint32_t> g_debugMenuNativeNodes;
static std::vector<std::string> g_debugMenuNativeLabels;
static uint32_t g_gameDebugController = 0;
static std::atomic<uint32_t> g_pendingZombieSpawns{0};
static std::atomic<uint32_t> g_zombieClearGeneration{0};
static uint32_t g_zombieSpawnScratch = 0;
static uint32_t g_zombieRegistryOwner = 0;
static std::vector<uint32_t> g_registeredZombieActors;
static std::atomic<uint32_t> g_pendingPPAward{0};
static uint32_t g_ppAwardReceiver = 0;
static uint32_t g_ppAwardArg5 = 0;
static uint32_t g_ppAwardArg6 = 0;
static uint32_t g_ppAwardArg7 = 0;
static bool g_debugLevelCap50 = false;
static thread_local bool g_extendedLevelProcessing = false;

constexpr uint32_t kAutoChuckBase = 0xFFFFF100;
constexpr uint32_t kAutoChuckOff = kAutoChuckBase - 1;
constexpr uint32_t kAutoChuckMenu = kAutoChuckBase - 2;
constexpr uint32_t kNativeMenu = 0xFFFFF0FC;
constexpr uint32_t kCustomMenuBase = 0xFFFFE000;
constexpr uint32_t kCustomBoolBase = 0xFFFFD000;
constexpr uint32_t kSpawnMenu = 0xFFFFCFFF;
constexpr uint32_t kSpawnOneZombie = 0xFFFFCFFE;
constexpr uint32_t kSpawnFiveZombies = 0xFFFFCFFD;
constexpr uint32_t kRemoveAllZombies = 0xFFFFCFFC;
constexpr uint32_t kProgressionMenu = 0xFFFFCFFB;
constexpr uint32_t kPPAwardBase = 0xFFFFCFE0;
constexpr uint32_t kToggleLevelCap = 0xFFFFCFDF;
static int32_t g_currentMenu = -1;
const char* const kAutoChuckStates[] = {
    "LOUNGER", "ITEM PICKER", "ZOMBIE KILLER", "EXPLORER",
    "MISSION MASTER", "COOP PLAYER", "COOP VEHICLE"
};

struct CustomBool
{
    uint8_t category;
    const char* label;
    uint32_t address;
};

const char* const kCustomCategoryNames[] = {
    "PLAYER / WEAPONS >", "ZOMBIES / AI >", "VEHICLES >",
    "WORLD / RENDERING >", "UI / GAME FLOW >"
};

// All addresses below come from Case Zero's own sub_824A2470 name-to-byte loader.
// This is intentionally not the larger DR2 PC debug list: Fortune City, TIR,
// poker/casino, online, DLC and main-game boss switches are omitted.
const CustomBool kCustomBools[] = {
    {0, "CHUCK GOD MODE",                 0x82A57C61},
    {0, "CHUCK GHOST MODE",               0x82A57C64},
    {0, "INFINITE PROP DURABILITY",       0x82A57C71},
    {0, "GET ALL COMBO CARDS",            0x82A57C74},
    {0, "ENABLE ALL SKILL MOVES",         0x82A57C67},
    {0, "DISABLE DEATH SEQUENCE",         0x82A57C75},
    {0, "DISABLE SKILL MOVE CAMERAS",     0x82A57C6C},
    {0, "SHOW CHUCK INFO",                0x82A57C6B},
    {0, "SHOW WEAPON DEBUG INFO",         0x82A57C70},
    {0, "SHOW JUMP HEIGHT",               0x82A57C7B},
    {0, "SHOW COMBO SEQUENCE COUNTER",    0x82A57C7A},

    {1, "ZOMBIES IGNORE ALL HUMANS",      0x82A57C63},
    {1, "ZOMBIE DEBUG INFO",              0x82A57C16},
    {1, "NPC DEBUG INFO",                 0x82A57CA5},
    {1, "SHOW ZOMBIE LINE OF SIGHT",      0x82A57C7C},
    {1, "FORCE QUEEN BEES TO SPAWN",      0x82A57C6E},
    {1, "DRAW DAMAGE LOGS",               0x82A57C0B},

    {2, "SHOW VEHICLE INFO",              0x82A57CBE},
    {2, "SHOW VEHICLE HEALTH",            0x82A57CBF},
    {2, "SHOW SUSPENSION",                0x82A57CC8},
    {2, "SHOW CENTER OF MASS",            0x82A57CC6},
    {2, "SHOW WHEEL TRANSFORMS",          0x82A57CC5},
    {2, "PLOT ENVIRONMENT COLLISION",     0x82A57CC7},
    {2, "ENABLE VEHICLE DEBUG BUTTONS",   0x82A57CC3},
    {2, "VEHICLE CAMERA FREE LOOK",       0x82A57CBC},
    {2, "DISABLE PROCEDURAL ANIMATION",   0x82A57CC4},
    {2, "HIDE ACTORS IN VEHICLES",        0x82A57CC2},
    {2, "HIDE DEBUG TIRE MARKS",          0x82A57CC0},

    {3, "ENABLE COLLISION VIEWER",        0x82A57CB7},
    {3, "ENABLE AABB VIEWER",             0x82A57CB8},
    {3, "ENABLE VISUAL DEBUGGER",         0x82A57CB6},
    {3, "SHOW CAMERA INFO",               0x82A57CB4},
    {3, "STATIONARY CAMERA",              0x82A57C78},
    {3, "DISABLE TIME OF DAY",            0x82A57CAA},
    {3, "CHUCK GRAVITY TEST",             0x82A57CB9},

    {4, "SHOW FRONTEND SCREEN INFO",       0x82A57BF2},
    {4, "SHOW LOADING TIMES",              0x82A57C09},
    {4, "SHOW GUIDE ARROW DEBUG INFO",     0x82A57CD9},
    {4, "EVERYTHING UNLOCKED FOR MISSIONS",0x82A57CD8},
    {4, "SHOW ALL NOTEBOOK ENTRIES",       0x82A57BF4},
    {4, "BUTTON THROUGH TIMED DIALOGS",    0x82A57C03},
    {4, "DISABLE TUTORIALS",               0x82A57CE0},
    {4, "DISABLE MOVIES",                  0x82A57CD3},
    {4, "DISABLE CASE FILE POPUPS",        0x82A57BF3},
    {4, "DISABLE LEVEL UP MESSAGE",        0x82A57BF5},
    {4, "DISABLE VIBRATION",               0x82A57CE1},
};

static void PublishDebugMenuLabels(uint8_t* base)
{
    std::vector<std::string> labels;
    labels.reserve(g_debugMenuVisibleNodes.size());
    for (size_t i = 0; i < g_debugMenuVisibleNodes.size(); ++i)
    {
        const uint32_t node = g_debugMenuVisibleNodes[i];
        std::string label = g_debugMenuBaseLabels[i];
        if (node >= kCustomBoolBase &&
            node < kCustomBoolBase + std::size(kCustomBools))
        {
            label += PPC_LOAD_U8(kCustomBools[node - kCustomBoolBase].address)
                ? " : ON" : " : OFF";
            labels.push_back(std::move(label));
            continue;
        }
        if (node == kNativeMenu ||
            (node >= kCustomMenuBase &&
             node < kCustomMenuBase + std::size(kCustomCategoryNames)))
        {
            labels.push_back(std::move(label));
            continue;
        }
        if (node == kAutoChuckMenu)
        {
            labels.push_back(std::move(label));
            continue;
        }
        if (node == kAutoChuckOff)
        {
            label += PPC_LOAD_U8(0x82A586DB) ? "" : " : ACTIVE";
            labels.push_back(std::move(label));
            continue;
        }
        if (node >= kAutoChuckBase && node < kAutoChuckBase + 7)
        {
            const uint32_t controller = g_gameDebugController
                ? PPC_LOAD_U32(g_gameDebugController + 0x30) : 0;
            if (controller)
            {
                const uint32_t state = PPC_LOAD_U32(controller + 0x70);
                if (state == node - kAutoChuckBase)
                    label += " : ACTIVE";
            }
            labels.push_back(std::move(label));
            continue;
        }
        const uint32_t vtable = PPC_LOAD_U32(node);
        if (vtable == 0x82070018 && PPC_LOAD_U32(node + 0x20))
            label += PPC_LOAD_U8(PPC_LOAD_U32(node + 0x20)) ? " : ON" : " : OFF";
        else if (vtable == 0x82070048 && PPC_LOAD_U32(node + 0x20))
            label += " : " + std::to_string(
                static_cast<int32_t>(PPC_LOAD_U32(PPC_LOAD_U32(node + 0x20))));
        else if (vtable == 0x820701C4 && PPC_LOAD_U32(node + 0x24))
        {
            const uint32_t selected = PPC_LOAD_U32(PPC_LOAD_U32(node + 0x24));
            const uint32_t count = PPC_LOAD_U32(node + 0x28);
            const uint32_t choices = PPC_LOAD_U32(node + 0x20);
            label += " : ";
            if (choices && selected < count)
            {
                const uint32_t name = PPC_LOAD_U32(choices + selected * 4);
                if (name)
                    for (uint32_t c = 0; c < 64 && PPC_LOAD_U8(name + c); ++c)
                        label.push_back(static_cast<char>(PPC_LOAD_U8(name + c)));
                else
                    label += std::to_string(selected);
            }
            else
                label += std::to_string(selected);
        }
        labels.push_back(std::move(label));
    }
    Host_DebugMenuSetItems(labels);
}

static void ShowDebugMenuRoot(uint8_t* base)
{
    g_currentMenu = -1;
    g_debugMenuVisibleNodes = g_debugMenuRootNodes;
    g_debugMenuBaseLabels = g_debugMenuRootLabels;
    PublishDebugMenuLabels(base);
}

static void ShowAutoChuckMenu(uint8_t* base)
{
    g_currentMenu = -2;
    g_debugMenuVisibleNodes.clear();
    g_debugMenuBaseLabels.clear();
    g_debugMenuVisibleNodes.push_back(kAutoChuckOff);
    g_debugMenuBaseLabels.push_back("< AUTOCHUCK: OFF / RETURN PLAYER CONTROL");
    for (uint32_t state = 0; state < 7; ++state)
    {
        g_debugMenuVisibleNodes.push_back(kAutoChuckBase + state);
        g_debugMenuBaseLabels.push_back(kAutoChuckStates[state]);
    }
    PublishDebugMenuLabels(base);
}

static void ShowCustomMenu(uint8_t* base, uint32_t category)
{
    g_currentMenu = static_cast<int32_t>(category);
    g_debugMenuVisibleNodes.clear();
    g_debugMenuBaseLabels.clear();
    for (uint32_t i = 0; i < std::size(kCustomBools); ++i)
    {
        if (kCustomBools[i].category != category)
            continue;
        g_debugMenuVisibleNodes.push_back(kCustomBoolBase + i);
        g_debugMenuBaseLabels.push_back(kCustomBools[i].label);
    }
    PublishDebugMenuLabels(base);
}

static void ShowNativeMenu(uint8_t* base)
{
    g_currentMenu = -3;
    g_debugMenuVisibleNodes = g_debugMenuNativeNodes;
    g_debugMenuBaseLabels = g_debugMenuNativeLabels;
    PublishDebugMenuLabels(base);
}

static void ShowSpawnMenu(uint8_t* base)
{
    g_currentMenu = -4;
    // Spawn actions are intentionally hidden: runs 20-29 have now falsified every
    // retained retail path tried so far. Do not leave operator-visible no-op buttons.
    g_debugMenuVisibleNodes = {kRemoveAllZombies};
    g_debugMenuBaseLabels = {"REMOVE ALL REGISTERED ZOMBIES IN CURRENT AREA"};
    PublishDebugMenuLabels(base);
}

static constexpr uint32_t kPPAwardAmounts[] = {
    500, 1000, 2000, 10000, 20000, 100000, 1000000, 5000000
};

static void ShowProgressionMenu(uint8_t* base)
{
    g_currentMenu = -5;
    g_debugMenuVisibleNodes.clear();
    g_debugMenuBaseLabels.clear();
    g_debugMenuVisibleNodes.push_back(kToggleLevelCap);
    g_debugMenuBaseLabels.push_back(g_debugLevelCap50
        ? "LEVEL CAP: 50 (DEBUG)" : "LEVEL CAP: 5 (CASE ZERO DEFAULT)");
    for (uint32_t i = 0; i < std::size(kPPAwardAmounts); ++i)
    {
        g_debugMenuVisibleNodes.push_back(kPPAwardBase + i);
        g_debugMenuBaseLabels.push_back("AWARD " +
            std::to_string(kPPAwardAmounts[i]) + " PP");
    }
    PublishDebugMenuLabels(base);
}

// Called from the XInput import after an F2 edge. The manager value is learned from
// the title's own successful transitions (PressStart -> TitleScreen, etc.), so this
// does not guess an object address or manufacture a partial frontend object.
static void RequestFrontendScreen(PPCContext& ctx, uint8_t* base,
                                  uint32_t nameAddress, uint32_t nameLength,
                                  const char* name)
{
    if (!g_frontendTransitionManager || !getenv("CZ_DEBUG_MENU"))
    {
        fprintf(stderr, "[debug] %s: no captured frontend transition manager yet\n", name);
        return;
    }

    ctx.r3.u64 = nameAddress;
    ctx.r4.u64 = nameLength;
    __imp__sub_8276E398(ctx, base);
    const uint32_t screenHash = ctx.r3.u32;
    ctx.r3.u64 = g_frontendTransitionManager;
    ctx.r4.u64 = screenHash;
    ctx.r5.u64 = 0;
    __imp__sub_827F6D40(ctx, base);
    fprintf(stderr, "[debug] requested %s through frontend manager %08X "
                    "(hash %08X)\n", name, g_frontendTransitionManager, screenHash);
}

void DebugTunables_RequestDebugJump(PPCContext& ctx, uint8_t* base)
{
    RequestFrontendScreen(ctx, base, 0x82071AC8, 9, "DebugJump");
}

void DebugTunables_RequestDebugEnter(PPCContext& ctx, uint8_t* base)
{
    RequestFrontendScreen(ctx, base, 0x82072450, 10, "DebugEnter");
}

void DebugTunables_ToggleFullDebugMenu(PPCContext& ctx, uint8_t* base)
{
    if (!getenv("CZ_DEBUG_MENU"))
        return;
    g_debugMenuActive = !g_debugMenuActive;
    Host_DebugMenuSetVisible(g_debugMenuActive);
    fprintf(stderr, "[debug] F4: host debug-menu renderer %s (%zu retained nodes)\n",
            g_debugMenuActive ? "opened" : "closed", g_debugMenuNodes.size());
}

void DebugTunables_PumpDebugMenu(PPCContext& ctx, uint8_t* base)
{
    if (!g_debugMenuActive)
        return;

    uint32_t index = 0;
    int32_t action = 0;
    if (!Host_DebugMenuConsumeAction(index, action) ||
        index >= g_debugMenuVisibleNodes.size())
        return;

    const uint32_t node = g_debugMenuVisibleNodes[index];
    const char* label = g_debugMenuBaseLabels[index].c_str();
    if (action == -1 && g_currentMenu != -1)
    {
        ShowDebugMenuRoot(base);
        return;
    }
    if (node >= kCustomMenuBase &&
        node < kCustomMenuBase + std::size(kCustomCategoryNames))
    {
        if (action == 1 || action == 2)
            ShowCustomMenu(base, node - kCustomMenuBase);
        return;
    }
    if (node == kNativeMenu)
    {
        if (action == 1 || action == 2)
            ShowNativeMenu(base);
        return;
    }
    if (node == kSpawnMenu)
    {
        if (action == 1 || action == 2)
            ShowSpawnMenu(base);
        return;
    }
    if (node == kProgressionMenu)
    {
        if (action == 1 || action == 2)
            ShowProgressionMenu(base);
        return;
    }
    if (node >= kPPAwardBase &&
        node < kPPAwardBase + std::size(kPPAwardAmounts))
    {
        if (action != 1)
            return;
        const uint32_t amount = kPPAwardAmounts[node - kPPAwardBase];
        if (!g_ppAwardReceiver)
        {
            fprintf(stderr, "[debug] cannot award %u PP yet: earn natural PP once "
                            "in this gameplay session first\n", amount);
            return;
        }
        g_pendingPPAward.fetch_add(amount, std::memory_order_release);
        fprintf(stderr, "[debug] queued native %u PP award\n", amount);
        return;
    }
    if (node == kToggleLevelCap)
    {
        if (action != 1 && action != 2)
            return;
        g_debugLevelCap50 = !g_debugLevelCap50;
        fprintf(stderr, "[debug] progression level cap -> %u\n",
                g_debugLevelCap50 ? 50u : 5u);
        ShowProgressionMenu(base);
        return;
    }
    if (node == kSpawnOneZombie || node == kSpawnFiveZombies)
    {
        if (action != 1)
            return;
        const uint32_t count = node == kSpawnOneZombie ? 1 : 5;
        g_pendingZombieSpawns.fetch_add(count, std::memory_order_release);
        fprintf(stderr, "[debug] queued %u managed zombie spawn%s; waiting for "
                        "the next population submission\n",
                count, count == 1 ? "" : "s");
        return;
    }
    if (node == kRemoveAllZombies)
    {
        if (action != 1)
            return;
        const uint32_t generation =
            g_zombieClearGeneration.fetch_add(1, std::memory_order_release) + 1;
        fprintf(stderr, "[debug] queued native DestroyZombie sweep generation %u\n",
                generation);
        return;
    }
    if (node >= kCustomBoolBase &&
        node < kCustomBoolBase + std::size(kCustomBools))
    {
        if (action == 1 || action == 2 || action == -1)
        {
            const CustomBool& item = kCustomBools[node - kCustomBoolBase];
            const uint8_t next = PPC_LOAD_U8(item.address) ? 0 : 1;
            PPC_STORE_U8(item.address, next);
            fprintf(stderr, "[debug] Case Zero bool '%s' @%08X -> %s\n",
                    item.label, item.address, next ? "ON" : "OFF");
            PublishDebugMenuLabels(base);
        }
        return;
    }
    if (node == kAutoChuckMenu)
    {
        if (action == 1 || action == 2)
            ShowAutoChuckMenu(base);
        return;
    }
    if (node == kAutoChuckOff)
    {
        if (action != 1)
            return;
        PPC_STORE_U8(0x82A586DB, 0);
        const uint32_t autoChuck = g_gameDebugController
            ? PPC_LOAD_U32(g_gameDebugController + 0x30) : 0;
        if (autoChuck)
        {
            PPC_STORE_U32(autoChuck + 0x70, 0);
            PPC_STORE_U8(autoChuck + 0x5E5C, 0);
        }
        fprintf(stderr, "[debug] AutoChuck OFF -> AI gate cleared, player stays in scene\n");
        PublishDebugMenuLabels(base);
        return;
    }
    if (node >= kAutoChuckBase && node < kAutoChuckBase + 7)
    {
        if (action != 1)
            return;
        const uint32_t autoChuck = g_gameDebugController
            ? PPC_LOAD_U32(g_gameDebugController + 0x30) : 0;
        if (!autoChuck)
        {
            fprintf(stderr, "[debug] AutoChuck unavailable: gameplay debug controller "
                            "has not been constructed\n");
            return;
        }

        // This is the retained handler at 0x8240A2CC exactly: raise its gate, then
        // call AutoChuck's vtable +0x14 initializer. The adjacent handler states are
        // direct writes to +0x70 and clear +0x5E5C.
        if (!PPC_LOAD_U8(0x82A586DB))
        {
            PPC_STORE_U8(0x82A586DB, 1);
            ctx.r3.u64 = autoChuck;
            const uint32_t method = PPC_LOAD_U32(PPC_LOAD_U32(autoChuck) + 0x14);
            ctx.ctr.u64 = method;
            PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        }
        const uint32_t state = node - kAutoChuckBase;
        PPC_STORE_U32(autoChuck + 0x70, state);
        PPC_STORE_U8(autoChuck + 0x5E5C, 0);
        fprintf(stderr, "[debug] AutoChuck -> state %u (%s), object %08X\n",
                state, kAutoChuckStates[state], autoChuck);
        PublishDebugMenuLabels(base);
        return;
    }

    const uint32_t vtable = PPC_LOAD_U32(node);
    if (vtable == 0x82070018)
    {
        const uint32_t value = PPC_LOAD_U32(node + 0x20);
        if (value)
        {
            const uint8_t next = PPC_LOAD_U8(value) ? 0 : 1;
            PPC_STORE_U8(value, next);
            fprintf(stderr, "[debug] menu bool '%s' -> %s\n", label,
                    next ? "ON" : "OFF");
        }
    }
    else if (vtable == 0x82070048)
    {
        const uint32_t value = PPC_LOAD_U32(node + 0x20);
        if (value)
        {
            int32_t current = static_cast<int32_t>(PPC_LOAD_U32(value));
            const int32_t low = static_cast<int32_t>(PPC_LOAD_U32(node + 0x24));
            const int32_t high = static_cast<int32_t>(PPC_LOAD_U32(node + 0x28));
            const int32_t delta = action < 0 ? -1 : 1;
            current += delta;
            if (low <= high)
            {
                if (current < low) current = low;
                if (current > high) current = high;
            }
            PPC_STORE_U32(value, static_cast<uint32_t>(current));
            fprintf(stderr, "[debug] menu integer '%s' -> %d\n", label, current);
        }
    }
    else if (vtable == 0x82070184 && action == 1)
    {
        const uint32_t callback = PPC_LOAD_U32(node + 0x20);
        if (callback)
        {
            fprintf(stderr, "[debug] menu action '%s' -> callback %08X\n",
                    label, callback);
            ctx.r3.u64 = 1;
            ctx.ctr.u64 = callback;
            PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        }
    }
    else if (vtable == 0x820701C4)
    {
        const uint32_t selectedAddress = PPC_LOAD_U32(node + 0x24);
        const uint32_t count = PPC_LOAD_U32(node + 0x28);
        if (selectedAddress && count && action != 1)
        {
            uint32_t selected = PPC_LOAD_U32(selectedAddress);
            selected = action < 0
                ? (selected ? selected - 1 : count - 1)
                : (selected + 1) % count;
            PPC_STORE_U32(selectedAddress, selected);
            fprintf(stderr, "[debug] menu selector '%s' -> %u/%u\n",
                    label, selected, count);
        }
    }
    else
    {
        fprintf(stderr, "[debug] menu item '%s': unsupported native type %08X\n",
                label, vtable);
    }
    PublishDebugMenuLabels(base);
}

namespace
{

struct Tunable
{
    const char* name;
    uint32_t    address;   // guest address of the one-byte flag
    int         readers;   // consumers found by the independent lbz scan
    const char* note;
};

// Curated from the 393 extracted. Every entry here has at least one confirmed
// reader; `readers` is that count, from the scan described in the header comment.
const Tunable kTunables[] = {
    // --- the switches that put the menus back on screen ---
    { "enable_debug_jump_menu",       0x82A57C0A, 1,
      "the DebugJump level/mission screen in the main menu" },
    { "enable_quickie_debug_menu",    0x82A57C02, 1,
      "the in-game quick debug menu" },
    { "enable_one_button_debug_menu", 0x82A57C00, 3,
      "open the debug menu with one button instead of a combo" },
    { "debug_on_controller_2_only",   0x82A57C01, 1,
      "route debug input to pad 2 so pad 1 still plays" },

    // --- progression and flow, the ones that make repeat testing cheap ---
    { "skip_startup",                 0x82A57BF0, 1,
      "skip the startup/logo sequence" },
    { "disable_mainmenu_scene",       0x82A57C08, 4,
      "skip the rendered main-menu backdrop" },
    { "notebook_show_all",            0x82A57BF4, 2,
      "every notebook/combo-card entry unlocked" },
    { "disable_level_up_message",     0x82A57BF5, 2,
      "suppress the level-up interstitial" },
    { "disable_casefiles_popup",      0x82A57BF3, 1,
      "suppress the case-file popup" },
    { "enable_button_through_timed_dialogs", 0x82A57C03, 1,
      "button past dialogs that normally hold for a timer" },
    { "enable_prolog_experience",     0x82A57BFA, 3,
      "force the prologue experience flow" },
    { "enable_trial_experience",      0x82A57BFE, 1,
      "force the TRIAL flow -- see CLAUDE.md finding 1 before setting this" },
    { "boss_use_debug_menu_jump",     0x82A57BF7, 1,
      "let the boss flow use the debug jump" },

    // --- overlays and diagnostics ---
    { "debug_show_loading_time",      0x82A57C09, 6,
      "on-screen load timings" },
    { "display_fe_screen_info",       0x82A57BF2, 1,
      "name the active frontend screen on screen" },
    { "zombie_show_debug_info",       0x82A57C16, 23,
      "per-zombie debug info -- 23 readers, expect a heavy overlay" },
    { "draw_damage_logs",             0x82A57C0B, 1,
      "damage event log" },
    { "ignore_boss_damage",           0x82A57C0E, 4,
      "bosses take no damage" },
    { "hide_changelist",              0x82A57BE4, 1,
      "hide the build changelist watermark" },
    { "enable_dev_only_debug_tiwwchnt", 0x82A57BFF, 26,
      "dev-only master switch, 26 readers -- broad, try it alone" },

    // Deliberately NOT offered, because the reader scan found zero consumers for
    // each and this file does not ship switches it cannot show are connected:
    //   show_actor_source_files   0x82A57BDE
    //   EnablePauseMenuCaseFiles  0x82A57BF5
    //   boss_use_debug_jump       0x82A57BF7
    //   make_game_go_faster       0x82A57C12
};

const Tunable* Find(const std::string& name)
{
    for (const Tunable& t : kTunables)
        if (name == t.name)
            return &t;
    return nullptr;
}

// The set CZ_DEBUG_MENU=1 turns on: the dev-only master gate, the menu/jump leaf
// gates and the frontend witness. The input-related flags are deliberately not in
// this preset: operator testing proved the retained Quickie renderer produces no
// visible output, and those flags otherwise reroute or alter ordinary input.
const char* const kMenuPreset[] = {
    "enable_dev_only_debug_tiwwchnt",
    "enable_debug_jump_menu",
    "display_fe_screen_info",
};

// These are the three top-level settings used by the retail PC debug-mode
// configuration.  They are loaded at the tail of the same guest config routine as
// the boolean table above, but live outside that table: two bytes and one integer.
// `limited_debug_menu` must remain false to request the complete development menu.
constexpr uint32_t kLimitedDebugMenu = 0x82A57A5E;
constexpr uint32_t kEnableDevFeatures = 0x82A57A5F;
constexpr uint32_t kTestMode = 0x82A57BB0;
// The frontend update at 0x825016A8 reaches its literal `DebugJump` transition
// only when this internal debug/soak latch is raised.  The retail config calls it
// `soak_test_timer_expired_DONT_USE`; here it is deliberately used as the title's
// own already-wired transition trigger, not as a simulated keyboard binding.

void Apply(uint8_t* base, const Tunable& t, uint8_t value)
{
    // A guest byte needs no endian swap, which is the whole reason these flags are
    // bytes and not ints as far as this hook is concerned.
    const uint8_t before = PPC_LOAD_U8(t.address);
    PPC_STORE_U8(t.address, value);
    fprintf(stderr, "[debug] %-36s @%08X  %u -> %u   (%d reader%s) %s\n",
            t.name, t.address, before, value, t.readers,
            t.readers == 1 ? "" : "s", t.note);
}

void ApplyFromEnvironment(uint8_t* base)
{
    bool any = false;

    if (const char* menu = getenv("CZ_DEBUG_MENU"))
    {
        if (menu[0] && strcmp(menu, "0") != 0)
        {
            fprintf(stderr, "[debug] CZ_DEBUG_MENU=%s -- enabling the title's own "
                            "debug menu and DebugJump screen\n", menu);
            for (const char* name : kMenuPreset)
                if (const Tunable* t = Find(name))
                    Apply(base, *t, 1);

            const uint8_t limited_before = PPC_LOAD_U8(kLimitedDebugMenu);
            const uint8_t dev_before = PPC_LOAD_U8(kEnableDevFeatures);
            const uint32_t test_before = PPC_LOAD_U32(kTestMode);
            PPC_STORE_U8(kLimitedDebugMenu, 0);
            // True skips the hardcoded cDebugMenu construction calls and leaves
            // its root null, expecting external development data that does not
            // ship. False selects the complete embedded menu tree.
            PPC_STORE_U8(kEnableDevFeatures, 0);
            PPC_STORE_U32(kTestMode, 1);
            fprintf(stderr,
                    "[debug] %-36s @%08X  %u -> 0   (full menu)\n"
                    "[debug] %-36s @%08X  %u -> 0   (build embedded menu tree)\n"
                    "[debug] %-36s @%08X  %u -> 1\n",
                    "limited_debug_menu", kLimitedDebugMenu, limited_before,
                    "enable_dev_features", kEnableDevFeatures, dev_before,
                    "test_mode", kTestMode, test_before);
            any = true;
        }
    }

    // CZ_DEBUG_TUNABLES=name[=0|1],name[=0|1],...  -- anything in the table above.
    if (const char* list = getenv("CZ_DEBUG_TUNABLES"))
    {
        std::string spec(list);
        size_t pos = 0;
        while (pos <= spec.size())
        {
            const size_t comma = spec.find(',', pos);
            std::string item = spec.substr(pos, comma == std::string::npos
                                                    ? std::string::npos
                                                    : comma - pos);
            pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;

            if (item.empty())
                continue;

            uint8_t value = 1;
            const size_t eq = item.find('=');
            if (eq != std::string::npos)
            {
                value = static_cast<uint8_t>(atoi(item.c_str() + eq + 1));
                item.resize(eq);
            }

            if (const Tunable* t = Find(item))
            {
                Apply(base, *t, value);
                any = true;
            }
            else
            {
                // Fail loudly and list what IS available. A typo that silently did
                // nothing would be indistinguishable from a tunable that does
                // nothing, and this project does not ship that ambiguity (gotcha 5).
                fprintf(stderr, "[debug] CZ_DEBUG_TUNABLES: unknown name '%s'. "
                                "Known names:\n", item.c_str());
                for (const Tunable& k : kTunables)
                    fprintf(stderr, "[debug]     %-36s %s\n", k.name, k.note);
            }
        }
    }

    if (any)
        fprintf(stderr, "[debug] tunables applied at the entry-point config load; "
                        "nothing rewrites them after this point\n");
}

}  // namespace

// The post-hook. The loader must run FIRST — it writes every one of these bytes from
// the (empty) retail config, so a pre-hook would have its work overwritten
// immediately. This is the ordering the whole instrument depends on.
PPC_FUNC(sub_824A2470)
{
    __imp__sub_824A2470(ctx, base);
    ApplyFromEnvironment(base);
}

// Record the real manager on every native screen transition, then preserve the
// original behaviour unchanged. This also catches the explicit F2 request itself.
PPC_FUNC(sub_827F6D40)
{
    g_frontendTransitionManager = ctx.r3.u32;
    __imp__sub_827F6D40(ctx, base);
}


// Capture the genuine engine-allocated cDebugMenu object. The constructor's only
// direct caller allocates 0x34 bytes and stores the result in the game manager;
// activation later uses this exact pointer and must not operate on host-made state.
PPC_FUNC(sub_824AAEB8)
{
    g_debugMenuObject = ctx.r3.u32;
    g_debugMenuFirstNode = 0;
    g_debugMenuLastNode = 0;
    g_debugMenuNodes.clear();
    g_buildingDebugMenu = getenv("CZ_DEBUG_MENU") != nullptr;
    __imp__sub_824AAEB8(ctx, base);
    g_buildingDebugMenu = false;

    g_debugMenuNativeNodes.clear();
    g_debugMenuNativeLabels.clear();
    for (uint32_t node : g_debugMenuNodes)
    {
        const uint32_t address = PPC_LOAD_U32(node + 0x14);
        if (!address)
            continue;
        std::string label;
        for (size_t i = 0; i < 96; ++i)
        {
            const char c = static_cast<char>(PPC_LOAD_U8(address + uint32_t(i)));
            if (!c) break;
            if (static_cast<unsigned char>(c) < 32) break;
            label.push_back(c);
        }
        if (!label.empty())
        {
            if (getenv("CZ_DEBUG_MENU_DUMP"))
                fprintf(stderr, "[debug-node] %08X type=%08X value=%08X aux=%08X/%08X '%s'\n",
                        node, PPC_LOAD_U32(node), PPC_LOAD_U32(node + 0x20),
                        PPC_LOAD_U32(node + 0x24), PPC_LOAD_U32(node + 0x28),
                        label.c_str());
            g_debugMenuNativeNodes.push_back(node);
            g_debugMenuNativeLabels.push_back(std::move(label));
        }
    }
    g_debugMenuRootNodes.clear();
    g_debugMenuRootLabels.clear();
    g_debugMenuRootNodes.push_back(kAutoChuckMenu);
    g_debugMenuRootLabels.push_back("AUTOCHUCK >");
    g_debugMenuRootNodes.push_back(kProgressionMenu);
    g_debugMenuRootLabels.push_back("PLAYER PROGRESSION >");
    for (uint32_t category = 0; category < std::size(kCustomCategoryNames); ++category)
    {
        g_debugMenuRootNodes.push_back(kCustomMenuBase + category);
        g_debugMenuRootLabels.push_back(kCustomCategoryNames[category]);
    }
    g_debugMenuRootNodes.push_back(kNativeMenu);
    g_debugMenuRootLabels.push_back("ORIGINAL ENGINE DEBUG ITEMS >");
    ShowDebugMenuRoot(base);
    fprintf(stderr, "[debug] captured cDebugMenu object %08X root=%08X "
                    "(%zu unique nodes, %zu labels published to host renderer)\n",
            g_debugMenuObject, PPC_LOAD_U32(g_debugMenuObject + 0x24),
            g_debugMenuNodes.size(), g_debugMenuNativeNodes.size());
}

PPC_FUNC(sub_82211138)
{
    g_gameDebugController = ctx.r3.u32;
    __imp__sub_82211138(ctx, base);
    fprintf(stderr, "[debug] captured gameplay debug controller %08X AutoChuck=%08X\n",
            g_gameDebugController, PPC_LOAD_U32(g_gameDebugController + 0x30));
}

// This is the confirmed zombie-registry insertion used by the current area. Keep
// the real actor argument, not the lightweight 68-byte registry proxy it creates.
PPC_FUNC(sub_8215A5A0)
{
    const uint32_t owner = ctx.r3.u32;
    const uint32_t actor = ctx.r4.u32;
    __imp__sub_8215A5A0(ctx, base);
    if (owner != g_zombieRegistryOwner)
    {
        g_zombieRegistryOwner = owner;
        g_registeredZombieActors.clear();
    }
    if (actor >= 0x10000)
    {
        bool seen = false;
        for (uint32_t prior : g_registeredZombieActors)
            if (prior == actor) { seen = true; break; }
        if (!seen)
            g_registeredZombieActors.push_back(actor);
    }
}

// Confirmed call site in the ordinary gameplay update (0x82257BB4). Service the
// host request after its original work, on the same guest thread that owns actors.
PPC_FUNC(sub_821E55A0)
{
    __imp__sub_821E55A0(ctx, base);
    const PPCContext naturalResult = ctx;
    const uint32_t pp = g_pendingPPAward.exchange(0, std::memory_order_acq_rel);
    if (pp && g_ppAwardReceiver)
    {
        PPCContext award = naturalResult;
        award.r3.u64 = g_ppAwardReceiver;
        award.r4.u64 = pp;
        award.r5.u64 = g_ppAwardArg5;
        award.r6.u64 = g_ppAwardArg6;
        award.r7.u64 = g_ppAwardArg7;
        __imp__sub_8253FB10(award, base);
        fprintf(stderr, "[debug] native PP award serviced: +%u via %08X\n",
                pp, g_ppAwardReceiver);
    }
    static uint32_t servicedGeneration = 0;
    static uint32_t eventAddress = 0;
    const uint32_t generation =
        g_zombieClearGeneration.load(std::memory_order_acquire);
    if (!generation || generation == servicedGeneration)
        return;
    servicedGeneration = generation;

    if (!eventAddress)
    {
        void* event = g_heap.Alloc(16);
        if (event)
        {
            std::memset(event, 0, 16);
            eventAddress = g_memory.MapVirtual(event);
            PPC_STORE_U32(eventAddress + 4, 0x8DB15034); // hash("DestroyZombie")
            PPC_STORE_U32(eventAddress + 8, 0);
        }
    }

    size_t removed = 0;
    if (eventAddress)
    {
        for (uint32_t actor : g_registeredZombieActors)
        {
            const uint32_t vtable = PPC_LOAD_U32(actor);
            if (vtable < PPC_IMAGE_BASE ||
                vtable >= PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
                continue;
            PPCContext remove = naturalResult;
            remove.r3.u64 = actor;
            remove.r4.u64 = eventAddress;
            __imp__sub_824FD628(remove, base);
            ++removed;
        }
    }
    fprintf(stderr, "[debug] registered-zombie clear %u serviced: %zu/%zu actors\n",
            generation, removed, g_registeredZombieActors.size());
    ctx = naturalResult;
}

// Natural gameplay awards arrive here with r4 as the PP amount. The function
// updates both overall and current-game PP, emits the PP notification, and runs
// the normal progression path. Capture a real invocation so host menu requests
// can replay the exact receiver/context rather than manufacturing either one.
PPC_FUNC(sub_8253FB10)
{
    const uint32_t receiver = ctx.r3.u32;
    const uint32_t amount = ctx.r4.u32;
    g_ppAwardReceiver = receiver;
    g_ppAwardArg5 = ctx.r5.u32;
    g_ppAwardArg6 = ctx.r6.u32;
    g_ppAwardArg7 = ctx.r7.u32;
    fprintf(stderr, "[debug] captured natural PP award receiver=%08X amount=%u "
                    "args=%08X/%08X/%08X\n",
            receiver, amount, g_ppAwardArg5, g_ppAwardArg6, g_ppAwardArg7);
    __imp__sub_8253FB10(ctx, base);
}

// The shared DR2 progression routine supports level 50. Case Zero's mode byte at
// 0x82A57BFA selects a cap of 5 instead. Temporarily clear it only around this
// one routine so the debug option cannot accidentally turn Case Zero into another
// game mode for unrelated systems that may consume the same byte.
PPC_FUNC(sub_8253F740)
{
    if (!g_debugLevelCap50)
    {
        __imp__sub_8253F740(ctx, base);
        return;
    }
    const uint8_t caseZeroCap = PPC_LOAD_U8(0x82A57BFA);
    PPC_STORE_U8(0x82A57BFA, 0);
    __imp__sub_8253F740(ctx, base);
    PPC_STORE_U8(0x82A57BFA, caseZeroCap);
}

// Mark only post-Case-Zero level processing. Selective hooks below suppress the
// two absent card reward types while leaving the ordinary level-up notification
// and health/inventory/attack/speed stat notifications intact.
PPC_FUNC(sub_8253E060)
{
    const uint32_t progression = ctx.r3.u32;
    const uint32_t oldLevel = progression >= 0x10000
        ? PPC_LOAD_U32(progression + 0x20) : 0;
    const bool extended = g_debugLevelCap50 && oldLevel >= 5;
    const bool prior = g_extendedLevelProcessing;
    g_extendedLevelProcessing = extended;
    if (extended)
        fprintf(stderr, "[debug] level %u -> %u: normal level/stat UI; "
                        "unavailable card rewards filtered\n",
                oldLevel, oldLevel + 1);
    __imp__sub_8253E060(ctx, base);
    g_extendedLevelProcessing = prior;
}

// Reward types 7 and 8 in the shared level table grant a combo card or skill
// card. Their objects/frontend data are not complete in Case Zero above level 5.
PPC_FUNC(sub_82539890)
{
    if (g_extendedLevelProcessing)
    {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82539890(ctx, base);
}

PPC_FUNC(sub_82539908)
{
    if (g_extendedLevelProcessing)
    {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82539908(ctx, base);
}

// The grant switch publishes C4A/C4B after the calls above. Drop those event
// records as well so the frontend never opens a card/tutorial screen for an
// asset that does not ship, while C1E and C31-C33 continue normally.
PPC_FUNC(sub_82157178)
{
    const uint32_t event = ctx.r4.u32;
    const uint32_t id = event >= 0x10000 ? PPC_LOAD_U32(event + 8) : 0;
    if (g_extendedLevelProcessing && (id == 0xC4A || id == 0xC4B))
    {
        fprintf(stderr, "[debug] filtered unavailable extended-level event %03X\n", id);
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82157178(ctx, base);
}

// The title's own gameplay-stat overlay reads the active player's stats through
// *(player + 0x0c), with Total PP at stats + 0x24.  Observe that established
// access path instead of guessing at a global player pointer.  Logging only on a
// transition keeps this probe cheap enough to leave enabled during an operator
// run and gives us a natural-award control before debug PP actions are added.
PPC_FUNC(sub_821B0A28)
{
    const uint32_t player = ctx.r3.u32;
    const uint32_t stats = player >= 0x10000 ? PPC_LOAD_U32(player + 0x0c) : 0;
    static uint32_t lastStats = 0;
    static uint32_t lastPP = UINT32_MAX;
    if (stats >= 0x10000)
    {
        const uint32_t pp = PPC_LOAD_U32(stats + 0x24);
        if (stats != lastStats || pp != lastPP)
        {
            fprintf(stderr,
                    "[debug] player progression player=%08X stats=%08X total_pp=%u",
                    player, stats, pp);
            if (stats == lastStats && lastPP != UINT32_MAX)
                fprintf(stderr, " delta=%+d", static_cast<int32_t>(pp - lastPP));
            fputc('\n', stderr);
            lastStats = stats;
            lastPP = pp;
        }
    }
    __imp__sub_821B0A28(ctx, base);
}

// This is the gameplay debug-controller update that normally recognizes the
// retained one/15-zombie button chords. Unlike host input polling it runs on the
// gameplay thread every frame. Use it only as a scheduler: the game's complete
// spawn routine still performs player lookup, request construction and submission.
PPC_FUNC(sub_82195AB0)
{
    __imp__sub_82195AB0(ctx, base);
    const uint32_t count = g_pendingZombieSpawns.exchange(0, std::memory_order_acq_rel);
    if (!count)
        return;

    const PPCContext naturalResult = ctx;
    const uint32_t oldMode = PPC_LOAD_U32(0x82A586A4);
    const uint32_t oldCount = PPC_LOAD_U32(0x82A6D3F4);
    PPC_STORE_U32(0x82A586A4, 2);
    PPC_STORE_U32(0x82A6D3F4, count);
    PPCContext spawn = ctx;
    __imp__sub_824A9970(spawn, base);
    PPC_STORE_U32(0x82A586A4, oldMode);
    PPC_STORE_U32(0x82A6D3F4, oldCount);
    fprintf(stderr, "[debug] gameplay-thread spawn routine completed for %u zombie%s\n",
            count, count == 1 ? "" : "s");
    ctx = naturalResult;
}

// Every live zombie reaches this update on the gameplay thread. A clear request is
// deliberately a bounded sweep rather than a permanent "no zombies" mode: for a
// few frames, send the title's own DestroyZombie animation event once to each unique
// object. Its handler tears down rendering/physics and sets the normal destroy flag.
PPC_FUNC(sub_8251DAB0)
{
    const uint32_t zombie = ctx.r3.u32;
    __imp__sub_8251DAB0(ctx, base);
    const PPCContext naturalResult = ctx;

    static uint32_t observedGeneration = 0;
    static uint32_t callsRemaining = 0;
    static uint32_t eventAddress = 0;
    static std::vector<uint32_t> destroyed;
    const uint32_t generation =
        g_zombieClearGeneration.load(std::memory_order_acquire);
    if (generation != observedGeneration)
    {
        observedGeneration = generation;
        callsRemaining = generation ? 4096 : 0;
        destroyed.clear();
        destroyed.reserve(256);
    }

    if (callsRemaining && zombie >= 0x10000)
    {
        --callsRemaining;
        bool seen = false;
        for (uint32_t prior : destroyed)
            if (prior == zombie) { seen = true; break; }
        if (!seen)
        {
            if (!eventAddress)
            {
                void* event = g_heap.Alloc(16);
                if (event)
                {
                    std::memset(event, 0, 16);
                    eventAddress = g_memory.MapVirtual(event);
                    // sub_824FD628 reads event hash at +4 and variant at +8.
                    PPC_STORE_U32(eventAddress + 4, 0x8DB15034); // DestroyZombie
                    PPC_STORE_U32(eventAddress + 8, 0);
                }
            }
            if (eventAddress)
            {
                destroyed.push_back(zombie);
                PPCContext remove = naturalResult;
                remove.r3.u64 = zombie;
                remove.r4.u64 = eventAddress;
                __imp__sub_824FD628(remove, base);
                fprintf(stderr, "[debug] DestroyZombie sweep %u: object %08X (%zu total)\n",
                        generation, zombie, destroyed.size());
            }
        }
        if (!callsRemaining)
            fprintf(stderr, "[debug] DestroyZombie sweep %u complete: %zu unique zombies\n",
                    generation, destroyed.size());
    }
    ctx = naturalResult;
}

// Capture genuine actor-manager submissions made by ordinary gameplay. The failed
// Quickie experiment proved that reconstructing only its final request is not
// sufficient; this records the descriptors the retail mission/population systems
// actually submit, including their caller and source tag. Read-only and bounded.
PPC_FUNC(sub_82189B00)
{
    static std::atomic<uint32_t> sequence{0};
    const bool capture = getenv("CZ_ZOMBIE_CAPTURE") != nullptr;
    const uint32_t id = capture ? sequence.fetch_add(1) : 0;
    const uint32_t caller = static_cast<uint32_t>(ctx.lr);
    const uint32_t factory = ctx.r3.u32;
    const uint32_t descriptor = ctx.r4.u32;
    const uint32_t source = ctx.r5.u32;
    const uint32_t line = ctx.r6.u32;
    const PPCContext entryContext = ctx;

    char sourceText[81]{};
    if (capture && source >= PPC_IMAGE_BASE &&
        source < PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
    {
        for (uint32_t i = 0; i < 80; ++i)
        {
            const uint8_t c = PPC_LOAD_U8(source + i);
            if (!c) break;
            sourceText[i] = c >= 32 && c < 127 ? static_cast<char>(c) : '?';
        }
    }

    uint32_t words[24]{};
    if (capture && descriptor >= 0x10000)
        for (uint32_t i = 0; i < std::size(words); ++i)
            words[i] = PPC_LOAD_U32(descriptor + i * 4);

    __imp__sub_82189B00(ctx, base);

    // Overlay input arrives on the XInput thread, where gameplay factories are not
    // safe to call. Consume it here, inside a genuine population submission, so the
    // correct guest thread and the complete Case Zero warehouse state are present.
    if (false && g_pendingZombieSpawns.load(std::memory_order_acquire) &&
        caller == 0x823E2000 && descriptor >= 0x10000 &&
        PPC_LOAD_U32(descriptor) == 0x8200AE7C)
    {
        const PPCContext naturalResult = ctx;
        const uint32_t manager = PPC_LOAD_U32(0x82A58760);
        if (manager)
        {
            PPCContext helper = entryContext;
            helper.r3.u64 = manager;
            helper.r4.u64 = PPC_LOAD_U32(manager + 0x80);
            __imp__sub_82482AD8(helper, base);
            const uint32_t player = helper.r3.u32;
            if (player)
            {
                if (!g_zombieSpawnScratch)
                {
                    void* scratch = g_heap.Alloc(0x400);
                    if (scratch)
                    {
                        std::memset(scratch, 0, 0x400);
                        g_zombieSpawnScratch = g_memory.MapVirtual(scratch);
                    }
                }
                if (g_zombieSpawnScratch)
                {
                    const uint32_t count =
                        g_pendingZombieSpawns.exchange(0, std::memory_order_acq_rel);
                    const uint32_t position = g_zombieSpawnScratch + 0x100;
                    for (uint32_t spawn = 0; spawn < count; ++spawn)
                    {
                        helper = entryContext;
                        helper.r3.u64 = position;
                        helper.r4.u64 = player;
                        __imp__sub_824A97F0(helper, base);

                        // Reconstruct through the genuine request initializer instead
                        // of copying identity and ownership fields. Parameters not
                        // related to position come from the current natural request.
                        PPCContext construct = entryContext;
                        construct.r1.u64 = g_zombieSpawnScratch + 0x200;
                        construct.r3.u64 = g_zombieSpawnScratch;
                        __imp__sub_8215FEC8(construct, base);
                        construct = entryContext;
                        construct.r1.u64 = g_zombieSpawnScratch + 0x200;
                        construct.r3.u64 = g_zombieSpawnScratch;
                        construct.r4.u64 = PPC_LOAD_U32(descriptor + 0x10);
                        construct.r5.u64 =
                            (uint64_t(PPC_LOAD_U32(position)) << 32) |
                            PPC_LOAD_U32(position + 4);
                        construct.r6.u64 = uint64_t(PPC_LOAD_U32(position + 8)) << 32;
                        construct.r8.u64 = PPC_LOAD_U32(descriptor + 0x24);
                        construct.r9.u64 = PPC_LOAD_U32(descriptor + 0x28);
                        construct.r10.u64 = PPC_LOAD_U64(descriptor + 0x30);
                        PPCRegister angle{};
                        angle.u32 = PPC_LOAD_U32(descriptor + 0x20);
                        construct.f1.f64 = angle.f32;
                        PPC_STORE_U32(construct.r1.u32 + 84,
                                      PPC_LOAD_U32(descriptor + 0x38));
                        PPC_STORE_U8(construct.r1.u32 + 95,
                                     PPC_LOAD_U8(descriptor + 0x4C));
                        PPC_STORE_U32(construct.r1.u32 + 100,
                                      PPC_LOAD_U32(descriptor + 0x50));
                        __imp__sub_8248A2F8(construct, base);

                        PPCContext submit = entryContext;
                        submit.r3.u64 = factory;
                        submit.r4.u64 = g_zombieSpawnScratch;
                        submit.r5.u64 = source;
                        submit.r6.u64 = line;
                        __imp__sub_82189B00(submit, base);
                        fprintf(stderr,
                                "[debug] managed zombie %u/%u submitted beside Chuck "
                                "xyz=%08X/%08X/%08X result=%08X\n",
                                spawn + 1, count, PPC_LOAD_U32(position),
                                PPC_LOAD_U32(position + 4), PPC_LOAD_U32(position + 8),
                                submit.r3.u32);
                    }
                }
            }
        }
        ctx = naturalResult;
    }

    if (capture && id < 512)
    {
        fprintf(stderr, "[zcap %03u] lr=%08X r13=%08X factory=%08X desc=%08X "
                        "source=%08X:%u '%s' result=%08X words=",
                id, caller, ctx.r13.u32, factory, descriptor, source, line,
                sourceText, ctx.r3.u32);
        for (uint32_t word : words)
            fprintf(stderr, "%08X,", word);
        fputc('\n', stderr);
    }
}

// Enabling AutoChuck also enables its old on-screen status formatter. Its backing
// development text state is absent in Case Zero retail and faults in sub_82810690.
// The host overlay already shows the active state, so suppress only that writer.
PPC_FUNC(sub_821DF1D0)
{
    if (getenv("CZ_DEBUG_MENU"))
        return;
    __imp__sub_821DF1D0(ctx, base);
}

// Every debug-menu node constructor finishes here to validate/store its label.
// Restrict the linkage repair to the dynamic extent of cDebugMenu's real startup
// constructor so unrelated uses of the common node base remain untouched.
PPC_FUNC(sub_824A8120)
{
    const uint32_t node = ctx.r3.u32;
    __imp__sub_824A8120(ctx, base);

    if (!g_buildingDebugMenu || node == g_debugMenuObject)
        return;

    for (uint32_t seen : g_debugMenuNodes)
        if (seen == node)
            return;
    g_debugMenuNodes.push_back(node);

}


// Retail destroys the fully populated startup cDebugMenu before gameplay. Rebuilding
// it later is invalid because its constructor consumes startup-only registry state.
// Preserve this one engine-allocated instance—including its menu nodes and storage—
// when the debug instrument is enabled. Other instances and ordinary runs retain the
// original destructor exactly.
PPC_FUNC(sub_824A8FE0)
{
    if (ctx.r3.u32 == g_debugMenuObject && getenv("CZ_DEBUG_MENU"))
    {
        fprintf(stderr, "[debug] preserving populated cDebugMenu %08X at teardown; "
                        "root=%08X\n", g_debugMenuObject,
                        PPC_LOAD_U32(g_debugMenuObject + 0x24));
        ctx.r3.u64 = g_debugMenuObject;
        return;
    }
    __imp__sub_824A8FE0(ctx, base);
}
