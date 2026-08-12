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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <sys/stat.h>
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

// Put Chuck under AI control in one of the seven states, WITHOUT the menu.
//
// Lifted verbatim out of the menu handler so the two paths cannot drift: it is the
// retained handler at 0x8240A2CC exactly — raise the gate at 0x82A586DB, call AutoChuck's
// vtable +0x14 initializer once, then write the state to +0x70 and clear +0x5E5C.
//
// Returns false when the gameplay debug controller has not been constructed yet, which is
// the normal state until a level is actually running. That is why `CZ_AUTOCHUCK` is
// applied by a PUMP rather than at startup: "when the game loads" is not a moment the
// runtime can name in advance, and the same problem produced the held DebugJump request.
static bool SetAutoChuckState(PPCContext& ctx, uint8_t* base, uint32_t state,
                              bool announce = true)
{
    const uint32_t autoChuck =
        g_gameDebugController ? PPC_LOAD_U32(g_gameDebugController + 0x30) : 0;
    if (!autoChuck)
        return false;
    if (!PPC_LOAD_U8(0x82A586DB))
    {
        PPC_STORE_U8(0x82A586DB, 1);
        ctx.r3.u64 = autoChuck;
        const uint32_t method = PPC_LOAD_U32(PPC_LOAD_U32(autoChuck) + 0x14);
        ctx.ctr.u64 = method;
        PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    }
    PPC_STORE_U32(autoChuck + 0x70, state);
    PPC_STORE_U8(autoChuck + 0x5E5C, 0);
    if (announce)
        fprintf(stderr, "[debug] AutoChuck -> state %u (%s), object %08X\n", state,
                kAutoChuckStates[state], autoChuck);
    return true;
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
// A request made before the frontend exists is HELD, not dropped.
//
// The manager is only captured on the first native screen transition (`sub_827F6D40`
// below), so a request has to arrive after the title screen has been left. A human at a
// keyboard just presses F2 again; a SYNTHETIC recipe cannot, because it fires at a fixed
// wall-clock offset against a boot whose depth in fixed time is a distribution
// (gotcha 75) — which is the very fragility the DebugJump route exists to escape. So an
// early request is remembered and serviced the moment the manager appears.
//
// Deliberately at most ONE pending request, cleared when it fires: a queue would let a
// mistimed recipe stack up screen transitions and land somewhere nobody chose. Both the
// deferral and the eventual firing are logged, because a request that happens seconds
// later in another subsystem is otherwise indistinguishable from one that was dropped.
struct PendingScreen
{
    uint32_t nameAddress = 0;
    uint32_t nameLength = 0;
    const char* name = nullptr;
};
static PendingScreen g_pendingScreen;

// How many screen requests have actually reached the frontend. `CZ_FAKE_PRESS_SEQ`'s
// WAITJUMP barrier parks on this, so a recipe can say "press DOWN one interval AFTER the
// DebugJump screen opens" instead of "press DOWN at 136 seconds" — the second is a fit to
// one afternoon's boot (gotcha 75), and it is what made the first two attempts miss: the
// jump landed at 131 s and DOWN had already fired at 128 s.
static std::atomic<uint32_t> g_screenRequestsServiced{ 0 };

// AUTO-CLOSE THE SCREEN AUTOCHUCK OPENS BY ITSELF.
//
// Measured, not guessed: with EXPLORER held and no synthetic input for over two minutes,
// the title requests two screens of its own at the same instant the map appears —
// 06903E1A and 890DF3E5 — and the BACK-delivered counter reads 0 throughout, so nothing in
// our input path did it. Those two hashes are therefore the map, and they are the default
// here; `CZ_AUTOCHUCK_CLOSE_HASHES=hex,hex` overrides the list and an empty value disables
// the whole mechanism.
//
// Closing it means pressing BACK, which is what a player does — not unwinding the frontend
// ourselves, because a screen stack we did not push is not one we should be popping. The
// press is injected through the same XamInputGetState the guest already reads, so it is
// indistinguishable from a real one and needs no new path.
static std::atomic<bool> g_autoChuckHeld{ false };
static std::atomic<long long> g_autoBackFromMs{ -1 };
static std::atomic<long long> g_autoBackUntilMs{ -1 };
static std::atomic<uint64_t> g_autoBackCount{ 0 };
static long long g_autoBackLastMs = -1000000;
// How long to let the screen settle before pressing, and how long to hold. The delay is
// the operator's observation: the map may not accept a close on the frame it opens, and a
// press that lands too early is indistinguishable from one that was never sent.
static long long AutoCloseDelayMs()
{
    static const long long v =
        getenv("CZ_AUTOCHUCK_CLOSE_DELAY_MS")
            ? strtoll(getenv("CZ_AUTOCHUCK_CLOSE_DELAY_MS"), nullptr, 10) : 1200;
    return v;
}

// Seconds since process start, on every line this file prints about a screen request.
// A synthetic recipe has to place its menu presses AFTER the jump lands, and the jump
// lands whenever the frontend gets round to it — so "when did it land" is the one number
// a recipe author needs and it was not in the log. Same units as
// `CZ_FAKE_START_MS`'s own lines, so the two can be read against each other directly.
// The epoch is at NAMESPACE SCOPE on purpose. As a function-local static it would be
// seeded on the FIRST CALL — which is the first screen request — so the first line always
// printed `at 0s` and the number said nothing at all. A clock that reads zero whenever you
// look at it is worse than no clock, because it looks like data (gotcha 151 in its
// quietest form). Static initialisation runs before main, so this is process start to
// within a few milliseconds, which is the resolution a recipe needs.
static const auto g_debugEpoch = std::chrono::steady_clock::now();

static long long DebugElapsedSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - g_debugEpoch)
        .count();
}

// Milliseconds since the same epoch. The auto-close window is "wait a beat, then hold the
// button briefly", and both halves are sub-second — expressed against the SECONDS helper
// they quantise to 0 or 1000 ms and the press either never happens or lasts a whole
// second. A timing window needs a clock finer than the window.
static long long DebugElapsedMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_debugEpoch)
        .count();
}

static void RequestFrontendScreen(PPCContext& ctx, uint8_t* base,
                                  uint32_t nameAddress, uint32_t nameLength,
                                  const char* name)
{
    if (!getenv("CZ_DEBUG_MENU"))
    {
        fprintf(stderr, "[debug] %s: ignored, CZ_DEBUG_MENU is not set\n", name);
        return;
    }
    if (!g_frontendTransitionManager)
    {
        g_pendingScreen = { nameAddress, nameLength, name };
        fprintf(stderr, "[debug] %s: no frontend transition manager yet at %llds — "
                        "request HELD until the first screen transition captures one\n",
                name, DebugElapsedSeconds());
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
    g_screenRequestsServiced.fetch_add(1, std::memory_order_release);
    fprintf(stderr, "[debug] requested %s through frontend manager %08X "
                    "(hash %08X) at %llds\n", name, g_frontendTransitionManager,
            screenHash, DebugElapsedSeconds());
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

// Service a request that arrived before the frontend existed. Called from the same
// XamInputGetState bridge as everything else here, so it runs on a guest thread with a
// usable context — which is why it cannot simply be done inside the transition hook that
// captures the manager, where re-entering the frontend would run a screen change from
// inside a screen change.
// CZ_AUTOCHUCK=<state> — hand Chuck to the AI as soon as a level is running, with no menu
// navigation at all.
//
// The F4 debug menu can do this, but only through the HOST overlay, which is driven by SDL
// keyboard events — so it is unreachable from a headless run and from `CZ_FAKE_PRESS_SEQ`,
// which speaks pad buttons and the F2/F3/F4 edges but not overlay navigation. Rather than
// build a second synthetic-input path into the overlay, this drives the same guest writes
// the menu item does. One env var replaces "F4, arrow to AUTOCHUCK, Right, arrow to the
// state, Enter".
//
// Accepts a state NAME (case- and space-insensitive: `EXPLORER`, `mission master`), an
// INDEX 0..6, or `OFF`. An unrecognised value is refused loudly with the list, because a
// silently ignored setting looks exactly like an AI that did not engage.
//
// A SCHEDULE, not just a state: `CZ_AUTOCHUCK="ITEM PICKER@0,ZOMBIE KILLER@180"` roams
// picking up items for three minutes and then switches to fighting. That shape exists
// because one state is rarely the right test on its own — a roamer covers ground and shows
// many textures but stalls once it runs out of objectives (EXPLORER walks to the ambulance
// and then opens the map), while a fighter stays in a crowd but never leaves it. `@seconds`
// is measured from the moment the AI first engages on the CURRENT level, not from process
// start, so a schedule means the same thing however long the boot took (gotcha 75).
//
// Applied by a pump, not at startup: it needs the gameplay debug controller, which does not
// exist until a level is actually running. And it RE-APPLIES whenever the world under it
// changes — a different AutoChuck object, or another screen request serviced — because the
// first version fired once at 6 s against the controller that already exists at the MENU,
// and the DebugJump level then loaded on top of it. A new screen request also restarts the
// schedule, since a fresh level is a fresh test.
struct AutoChuckStep
{
    int state;          // -1 = OFF, 0..6 = a state
    long long atSeconds;
};

static void PumpAutoChuckFromEnvironment(PPCContext& ctx, uint8_t* base)
{
    static const char* want = getenv("CZ_AUTOCHUCK");
    static bool refused = want == nullptr;
    if (refused)
        return;

    static std::vector<AutoChuckStep> schedule;
    static bool parsed = false;
    if (!parsed)
    {
        parsed = true;
        auto squash = [](const std::string& in) {
            std::string out;
            for (char c : in)
                if (!isspace(static_cast<unsigned char>(c)) && c != '_' && c != '-')
                    out.push_back(char(toupper(static_cast<unsigned char>(c))));
            return out;
        };
        const std::string all(want);
        size_t at = 0;
        while (at <= all.size())
        {
            const size_t comma = std::min(all.find(',', at), all.size());
            std::string item = all.substr(at, comma - at);
            at = comma + 1;
            if (item.empty())
                continue;
            long long when = 0;
            const size_t sep = item.find('@');
            if (sep != std::string::npos)
            {
                when = strtoll(item.c_str() + sep + 1, nullptr, 10);
                item = item.substr(0, sep);
            }
            const std::string w = squash(item);
            int state = -2;
            if (w == "OFF")
                state = -1;
            else if (w.size() == 1 && w[0] >= '0' && w[0] <= '6')
                state = w[0] - '0';
            else
                for (uint32_t i = 0; i < 7; i++)
                    if (squash(kAutoChuckStates[i]) == w)
                    {
                        state = int(i);
                        break;
                    }
            if (state == -2)
            {
                fprintf(stderr, "[debug] CZ_AUTOCHUCK: \"%s\" is not a state. Use OFF, 0..6, "
                                "or one of:", item.c_str());
                for (uint32_t i = 0; i < 7; i++)
                    fprintf(stderr, " %s%s", kAutoChuckStates[i], i == 6 ? "" : ",");
                fprintf(stderr, "   (each entry may carry @SECONDS)\n");
                refused = true;
                return;
            }
            schedule.push_back({ state, when });
        }
        if (schedule.empty())
        {
            refused = true;
            return;
        }
        // Sorted by time so the "which step is current" search below is a simple scan, and
        // so an out-of-order schedule does what it says rather than what it was typed as.
        std::sort(schedule.begin(), schedule.end(),
                  [](const AutoChuckStep& a, const AutoChuckStep& b) {
                      return a.atSeconds < b.atSeconds;
                  });
        fprintf(stderr, "[debug] CZ_AUTOCHUCK schedule:");
        for (const auto& st : schedule)
            fprintf(stderr, " %s@%llds", st.state < 0 ? "OFF" : kAutoChuckStates[st.state],
                    st.atSeconds);
        fprintf(stderr, "  (seconds from the AI first engaging on this level)\n");
    }

    const uint32_t objectNow =
        g_gameDebugController ? PPC_LOAD_U32(g_gameDebugController + 0x30) : 0;
    const uint32_t screensNow = g_screenRequestsServiced.load(std::memory_order_acquire);
    static uint32_t appliedObject = 0;
    static uint32_t appliedScreens = 0xFFFFFFFFu;
    static long long epoch = -1;
    static int appliedStep = -1;

    // A fresh level restarts the schedule, and re-arms the first step.
    if (objectNow != appliedObject || screensNow != appliedScreens)
    {
        epoch = -1;
        appliedStep = -1;
    }

    if (epoch < 0)
    {
        // Not engaged yet. The first step decides when the clock starts, so an @0 step
        // engages as soon as a level exists.
        if (schedule.front().state >= 0 &&
            !SetAutoChuckState(ctx, base, uint32_t(schedule.front().state)))
            return;   // no controller yet — try again on the next poll
        if (schedule.front().state < 0)
        {
            PPC_STORE_U8(0x82A586DB, 0);
            if (objectNow)
            {
                PPC_STORE_U32(objectNow + 0x70, 0);
                PPC_STORE_U8(objectNow + 0x5E5C, 0);
            }
        }
        epoch = DebugElapsedSeconds();
        appliedStep = 0;
        g_autoChuckHeld.store(schedule.front().state >= 0, std::memory_order_release);
        appliedObject = objectNow;
        appliedScreens = screensNow;
        fprintf(stderr, "[debug] CZ_AUTOCHUCK: %s engaged at %llds (object %08X, after %u "
                        "screen request(s)) — Chuck is under AI control; this run's "
                        "movement is SYNTHETIC and its progress is not evidence\n",
                schedule.front().state < 0 ? "OFF"
                                           : kAutoChuckStates[schedule.front().state],
                epoch, objectNow, screensNow);
        return;
    }

    // HOLD THE STATE, because the title's own AI changes it underneath us.
    //
    // Measured, not assumed: we wrote state 1 (ITEM PICKER) at 6 s and again at 28 s, and
    // a live read of the running process an hour later found 4 (MISSION MASTER), stable.
    // We never write 4. So `CZ_AUTOCHUCK` was only ever setting the INITIAL state and the
    // AI promoted itself — which is why every state looked identical from the outside:
    // each one walked to the ambulance objective and waited there, because by then it was
    // MISSION MASTER. Two of my explanations for that (EXPLORER running out of waypoints,
    // a stray synthetic A press opening the map) were wrong for the same reason.
    //
    // Re-asserting on every poll is cheap — the gate is already up, so it is two stores —
    // and it is COUNTED, because "the AI overrode us 4,000 times a minute" and "once" are
    // completely different facts about this title's debug AI and only the number tells
    // them apart. `CZ_AUTOCHUCK_NO_HOLD=1` restores set-once, which is the control arm.
    static const bool noHold = getenv("CZ_AUTOCHUCK_NO_HOLD") != nullptr;
    static uint64_t overrides = 0;
    static long long lastOverrideLog = -1;
    if (!noHold && appliedStep >= 0 && schedule[size_t(appliedStep)].state >= 0 && objectNow)
    {
        const uint32_t desired = uint32_t(schedule[size_t(appliedStep)].state);
        if (PPC_LOAD_U32(objectNow + 0x70) != desired)
        {
            SetAutoChuckState(ctx, base, desired, false);
            ++overrides;
            const long long now = DebugElapsedSeconds();
            if (lastOverrideLog < 0 || now - lastOverrideLog >= 15)
            {
                lastOverrideLog = now;
                fprintf(stderr, "[debug] CZ_AUTOCHUCK: the title's AI has changed the state "
                                "away from %s %llu time(s); re-asserting (CZ_AUTOCHUCK_NO_HOLD=1 "
                                "to let it win)\n",
                        kAutoChuckStates[desired], (unsigned long long)overrides);
            }
        }
    }

    // Which step the schedule is on now.
    const long long since = DebugElapsedSeconds() - epoch;
    int want_step = 0;
    for (size_t i = 0; i < schedule.size(); i++)
        if (since >= schedule[i].atSeconds)
            want_step = int(i);
    if (want_step == appliedStep)
        return;

    const AutoChuckStep& st = schedule[size_t(want_step)];
    if (st.state < 0)
    {
        PPC_STORE_U8(0x82A586DB, 0);
        if (objectNow)
        {
            PPC_STORE_U32(objectNow + 0x70, 0);
            PPC_STORE_U8(objectNow + 0x5E5C, 0);
        }
    }
    else if (!SetAutoChuckState(ctx, base, uint32_t(st.state)))
        return;
    appliedStep = want_step;
    g_autoChuckHeld.store(st.state >= 0, std::memory_order_release);
    fprintf(stderr, "[debug] CZ_AUTOCHUCK: step %d -> %s at %llds (%llds into the level)\n",
            want_step, st.state < 0 ? "OFF" : kAutoChuckStates[st.state],
            DebugElapsedSeconds(), since);
}

// CZ_DEBUG_FLAGS=NAME[,NAME...] — set the title's own gameplay debug bools without the
// menu, by the label the menu shows.
//
// The F4 menu can already toggle every one of these, and for a question you can answer in
// one sitting that is fine. It is not fine for the thing the operator actually needs,
// which is to STAND STILL in front of a defect and photograph it while a crowd tries to
// eat them: `ZOMBIES IGNORE ALL HUMANS` and `CHUCK GOD MODE` are the difference between a
// comparison and a fight, and `DISABLE TIME OF DAY` is the difference between two arms lit
// the same way and two arms lit ten in-game minutes apart. The last one matters most and
// is the least obvious: the sun moves during a session, so an A/B taken twenty minutes
// apart has a lighting confound baked into it that no amount of standing in the same spot
// removes (and part 26's outdoor era medians are a whole-frame luma statistic, which is
// exactly what a moving sun perturbs).
//
// APPLIED BY THE PUMP, not once at startup, and re-asserted every time. Same reason
// `CZ_AUTOCHUCK` is: the bytes live in the guest image but the game writes them itself —
// a level load, a case transition or the title's own debug loader can clear them, and a
// flag that silently stopped being set halfway through a run is indistinguishable from a
// flag that never worked. The count of re-asserts is reported so a flag that is being
// fought over is a NUMBER rather than a mystery (gotcha 151 — an arm with no counter
// cannot be shown to have engaged).
void PumpDebugFlagsFromEnvironment(uint8_t* base)
{
    static const char* spec = getenv("CZ_DEBUG_FLAGS");
    if (!spec)
        return;
    // Resolved once: which table entries the operator named, and whether every name was
    // understood. A typo must fail LOUDLY and list what is available, exactly as
    // CZ_DEBUG_TUNABLES does — a name that silently does nothing is indistinguishable
    // from a flag that does nothing (gotcha 5).
    static std::vector<const CustomBool*> wanted = [] {
        std::vector<const CustomBool*> out;
        std::string s(spec);
        for (size_t pos = 0; pos <= s.size();)
        {
            const size_t comma = s.find(',', pos);
            std::string item =
                s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = (comma == std::string::npos) ? s.size() + 1 : comma + 1;
            if (item.empty())
                continue;
            // Case- and separator-insensitive, because the labels are shouted with spaces
            // and slashes and nobody should have to quote them exactly.
            auto norm = [](std::string v) {
                std::string o;
                for (char c : v)
                    if (isalnum(static_cast<unsigned char>(c)))
                        o += char(toupper(static_cast<unsigned char>(c)));
                return o;
            };
            const std::string want = norm(item);
            const CustomBool* hit = nullptr;
            for (const CustomBool& b : kCustomBools)
                if (norm(b.label) == want)
                    hit = &b;
            if (hit)
            {
                out.push_back(hit);
                fprintf(stderr, "[debug] CZ_DEBUG_FLAGS: '%s' @%08X will be held ON\n",
                        hit->label, hit->address);
            }
            else
            {
                fprintf(stderr, "[debug] CZ_DEBUG_FLAGS: unknown flag '%s'. Known:\n",
                        item.c_str());
                for (const CustomBool& b : kCustomBools)
                    fprintf(stderr, "[debug]     %s\n", b.label);
            }
        }
        return out;
    }();

    static uint64_t reasserts = 0;
    for (const CustomBool* b : wanted)
        if (!PPC_LOAD_U8(b->address))
        {
            PPC_STORE_U8(b->address, 1);
            // The FIRST set of each flag and then every thousandth, because a flag the
            // game rewrites every frame would otherwise drown the log — and the total is
            // printed either way, so "it was fought over" is still visible.
            if (++reasserts <= std::size(kCustomBools) || (reasserts % 1000) == 0)
                fprintf(stderr, "[debug] CZ_DEBUG_FLAGS: '%s' set ON (%llu asserts so "
                                "far — anything above one means the title cleared it)\n",
                        b->label, (unsigned long long)reasserts);
        }
}

// CZ_GUEST_DIAG=1 — the engine's ENTIRE diagnostic layer, switched back on by one byte.
//
// WHY THIS EXISTS
// ---------------
// `CZ_GUEST_LOG` (runtime/cpu/guest_probe.cpp) hooks the engine's log sink and has
// always printed almost nothing. Its own header explains why with a guess — "most of
// the interesting call sites are gated on a debug byte that a shipped build leaves at
// zero" — and gotcha 215 records raising those gates as open work, as though it were a
// hunt across hundreds of independent flags.
//
// It is ONE byte, and the polarity is the other way round. A scan of .text for
// `lis`-resolved byte references finds `0x829EC974` read by **2,013 sites and written by
// none**, every one of them the identical shape:
//
//     lbz     r11, -0x368c(r29)     ; r29 = 0x829F0000, so 0x829EC974
//     cmplwi  r11, 0
//     bne     <skip>                ; NONZERO skips
//     ...                           ; build the message
//     bl      0x827877C8            ; the vsnprintf that feeds sub_828223A0
//
// and the image ships that byte as **0x01**. So this is not a debug flag that a release
// build failed to set; it is a release-build KILL SWITCH that silences the whole layer,
// and clearing it re-enables all 2,013 at once. Nothing in the guest ever writes it, so
// there is no fight to lose — but it is pumped rather than poked once for the same reason
// CZ_DEBUG_FLAGS is, and for the counter (gotcha 151).
//
// WHY THE SECOND BYTE
// -------------------
// Many of those 2,013 sites are the assert formatter, and the assert path continues:
//
//     lbz     r10, 0x3ead(r10)      ; 0x82AC3EAD
//     cmplwi  r10, 0
//     bne     <survive>
//     ...
//     twui    r0, 0x16              ; TRAP
//
// so with the log byte cleared, an assert that was previously a silent no-op becomes a
// fatal trap. `0x82AC3EAD` is the "report it, do not die" byte (592 readers), and the
// guest writes it in exactly two places, which is why this is pumped. Setting it is not
// hiding a failure: the assert still PRINTS, with its file and line, through the sink
// CZ_GUEST_LOG already reads. Silencing the trap is what makes the message reachable.
//
// WHAT IT IS FOR
// --------------
// Asked to explain why world geometry only reaches its near LOD at close range, the
// useful evidence is the engine's own: `Queue is full in MoveLoadRequest() priority=%d!`,
// `Out of memory in the load & decomp heap!`, `WAITING: cLevel - wait_for_tex_lod = %c`,
// and the two `cZone::UpdatePriorities()` asserts (`mForceLowLOD`, `mNumVolumes`). All
// four are behind this byte. Pair it with CZ_GUEST_LOG=1, which is the sink — this arm
// alone prints nothing, because it only decides whether the messages are FORMATTED.
//
// IT IS A DIAGNOSTIC ARM, NEVER A GATE CONFIGURATION. Two thousand formatting sites on
// the frame path cost real time, and gotcha 7 applies: a probe expensive enough to stall
// the game manufactures the behaviour it reports. Quote frame numbers from a run WITHOUT
// this set.
static void PumpGuestDiagnosticsFromEnvironment(uint8_t* base)
{
    static const bool on = getenv("CZ_GUEST_DIAG") != nullptr;
    if (!on)
        return;

    constexpr uint32_t kSuppressLog = 0x829EC974;   // 1 = silence, and it ships as 1
    constexpr uint32_t kAssertsFatal = 0x82AC3EAD;  // 0 = trap on assert

    static uint64_t reasserts = 0;
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        fprintf(stderr,
                "[debug] CZ_GUEST_DIAG: clearing the release log gate at %08X (was %u; "
                "2,013 call sites read it, none write it) and setting %08X so asserts "
                "print instead of trapping. Needs CZ_GUEST_LOG=1 to see anything — this "
                "arm only decides whether the engine FORMATS its messages. Costs frame "
                "time; do not measure performance with it on.\n",
                kSuppressLog, PPC_LOAD_U8(kSuppressLog), kAssertsFatal);
    }

    if (PPC_LOAD_U8(kSuppressLog) != 0)
    {
        PPC_STORE_U8(kSuppressLog, 0);
        ++reasserts;
    }
    if (PPC_LOAD_U8(kAssertsFatal) == 0)
    {
        PPC_STORE_U8(kAssertsFatal, 1);
        ++reasserts;
    }
    // Two writes are expected: the first pump sets both. Anything beyond that means the
    // title is writing them back, which is a finding rather than a nuisance — the scan
    // said the log byte has no writers at all.
    //
    // Reported on CHANGE, not on value: the first version tested `reasserts == 2`, which
    // is true on every subsequent pump as well and put the same line in the log tens of
    // thousands of times. A counter's log line has to fire on the edge (gotcha 151 wants
    // the number, not the noise).
    static uint64_t reported = 0;
    if (reasserts != reported && (reasserts <= 2 || (reasserts % 1000) == 0))
    {
        reported = reasserts;
        fprintf(stderr,
                "[debug] CZ_GUEST_DIAG: %llu writes so far (2 = set once as expected; "
                "more means the title is clearing them back)\n",
                (unsigned long long)reasserts);
    }
}

// ===================================================================================
// THE PLAYER'S POSITION, THROUGH THE TITLE'S OWN DEBUG-CONSOLE PATH
//
// This retail image keeps its debug console (gotcha 266's shape again), and two of its
// commands -- `setplayerpos` and `getplayerinfo` -- reach the player object by an
// identical five-step lookup, which is what makes the lookup trustworthy rather than a
// reading of one function. Both were disassembled in part 36; `docs/phase5-notes.md`
// §6bl has the transcript. Everything below is that path, executed rather than parsed.
//
// WHY NOT JUST WRITE THE POSITION FIELD. The write path makes a `vtable[0x28]` call
// BEFORE storing, and the store itself is a virtual `vtable[0x84]`. Whatever those do
// -- physics, streaming, zone bookkeeping -- a memory poke skips, and the cost of
// skipping it is a defect that shows up somewhere else entirely and gets investigated
// as its own thing. Calling the title's path costs one indirect call and cannot have
// that class of bug.
//
// The scratch is one guest allocation, reused: these calls take an out-pointer (the
// read) and a vec3 pointer (the write), and both must be GUEST addresses.
static uint32_t g_posScratch = 0;

// The last position read, for the F9 pose to print from the render thread -- which
// must NOT make guest calls itself. The timestamp travels with it so a stale value
// announces itself instead of looking like a fresh one (gotcha 13 at frame scale).
static std::atomic<float> g_playerPos[3] = {};
static std::atomic<long long> g_playerPosAtMs{-1};

static void CallGuestAt(PPCContext& ctx, uint8_t* base, uint32_t addr)
{
    ctx.ctr.u64 = addr;
    PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// The five steps both commands share:
//   mgr  = *(u32*)0x82A57428
//   sess = 0x82483230(mgr, 1)
//   t    = sess->vtable[0x10]()
//   list = *(u32*)(t + 0x7C)
//   obj  = 0x8247B020(list, index)
// IS A LEVEL ACTUALLY RUNNING? The lookup below happily returns an object during the
// prologue and the menus, and calling a virtual on it there faults inside guest code —
// measured, not feared: the same recipe with these pumps armed faults twice where the
// control arm with them disabled faults zero times. So gate on the signal AutoChuck
// already trusts for exactly this question (the debug controller's object), which is
// the project's established "the level exists" predicate rather than a new guess.
static bool LevelIsRunning(uint8_t* base)
{
    return g_gameDebugController && PPC_LOAD_U32(g_gameDebugController + 0x30) != 0;
}

static uint32_t LookupPlayerObject(PPCContext& ctx, uint8_t* base, uint32_t index)
{
    if (!LevelIsRunning(base))
        return 0;
    // EVERY STEP IS TRACED ONCE, because when this chain faults the crash report names
    // a guest address several calls deep and says nothing about WHICH step handed it a
    // bad pointer. Rate-limited to one line per distinct outcome.
    const bool trace = getenv("CZ_POSE_TRACE") != nullptr;
    static uint32_t lastTrace[5] = {};
    uint32_t step[5] = {};
    const uint32_t mgr = PPC_LOAD_U32(0x82A57428);
    step[0] = mgr;
    if (!mgr)
        return 0;
    ctx.r3.u64 = mgr;
    ctx.r4.u64 = 1;
    CallGuestAt(ctx, base, 0x82483230);
    const uint32_t sess = uint32_t(ctx.r3.u64);
    step[1] = sess;
    if (!sess)
        return 0;
    const uint32_t sessVt = PPC_LOAD_U32(sess);
    step[2] = sessVt;
    // A VTABLE POINTER MUST POINT AT THE IMAGE. During a transition this object exists
    // but its vtable slot holds a heap pointer or garbage, and dispatching through it
    // is the fault this chain produced: signal 11 at guest 0, several calls deep.
    if (sessVt < 0x82000000 || sessVt >= 0x82B40000)
        return 0;
    ctx.r3.u64 = sess;
    const uint32_t getT = PPC_LOAD_U32(sessVt + 0x10);
    if (getT < 0x82150000 || getT >= 0x829C3554)
        return 0;
    CallGuestAt(ctx, base, getT);
    const uint32_t t = uint32_t(ctx.r3.u64);
    step[3] = t;
    if (!t)
        return 0;
    const uint32_t list = PPC_LOAD_U32(t + 0x7C);
    step[4] = list;
    if (!list)
        return 0;
    ctx.r3.u64 = list;
    ctx.r4.u64 = index;
    CallGuestAt(ctx, base, 0x8247B020);
    const uint32_t obj = uint32_t(ctx.r3.u64);
    if (trace && memcmp(step, lastTrace, sizeof step) != 0)
    {
        memcpy(lastTrace, step, sizeof step);
        // THE OBJECT'S OWN VTABLE IS THE TEST OF WHETHER IT IS AN OBJECT. A wrong
        // pointer out of this chain is not null — the console's own "player not found"
        // check would not catch it either — so print what it points at and let an
        // out-of-image vtable say so out loud.
        const uint32_t objVt = obj ? PPC_LOAD_U32(obj) : 0;
        fprintf(stderr, "[debug] player lookup: mgr=%08X sess=%08X vt=%08X t=%08X "
                        "list=%08X obj=%08X objVt=%08X%s\n", step[0], step[1], step[2],
                step[3], step[4], obj, objVt,
                (objVt >= 0x82000000 && objVt < 0x82B40000)
                    ? "" : "   <- NOT AN IMAGE VTABLE: this is not a live object");
    }
    return obj;                        // 0 is the console's "error:player not found"
}

static bool EnsurePosScratch()
{
    if (g_posScratch)
        return true;
    // The out-vector at +0 and the write vector at +0x40. No stack here on purpose —
    // the guest calls run on the borrowed thread's own stack.
    void* p = g_heap.Alloc(0x100);
    if (!p)
        return false;
    std::memset(p, 0, 0x100);
    g_posScratch = g_memory.MapVirtual(p);
    return g_posScratch != 0;
}

// getplayerinfo's read, WITHOUT MAKING A CALL AT ALL.
//
// The virtual it dispatches (vtable+0x18 -> 0x82483718) is seven instructions:
//
//     lwz r11,0x1C(r4) ; lwz r10,0x20(r4) ; lwz r9,0x24(r4)
//     stw r11,0(r3)    ; stw r10,4(r3)    ; stw r9,8(r3)    ; blr
//
// — it copies three floats out of the object. So the position IS `obj + 0x1C`, and
// reading it needs no guest call, no scratch buffer and no borrowed thread.
//
// That matters because CALLING it faulted. Every function in the chain is present in
// the recompilation (checked), the object had a real image vtable, and the getter
// itself cannot fault — so the object at the faulting moment was a DIFFERENT CLASS,
// whose +0x18 is some other method taking other arguments. A vtable index is only
// meaningful for the class it was read from, and this chain can hand back more than
// one kind of actor. Hence the guard below: the layout is trusted only for the exact
// vtable the disassembly came from, and anything else is declined out loud rather than
// dispatched into blindly.
static constexpr uint32_t kKnownPlayerVtable = 0x8205D440;

static bool ReadPlayerPos(PPCContext& ctx, uint8_t* base, uint32_t index, float out[3])
{
    const uint32_t obj = LookupPlayerObject(ctx, base, index);
    if (!obj)
        return false;
    const uint32_t vt = PPC_LOAD_U32(obj);
    if (vt != kKnownPlayerVtable)
    {
        static uint32_t saidFor = 0;
        if (saidFor != vt)
        {
            saidFor = vt;
            fprintf(stderr, "[debug] player object %08X has vtable %08X, not the class "
                            "this layout was read from (%08X) — position DECLINED "
                            "rather than guessed\n", obj, vt, kKnownPlayerVtable);
        }
        return false;
    }
    for (int i = 0; i < 3; i++)
    {
        const uint32_t bits = PPC_LOAD_U32(obj + 0x1C + uint32_t(i) * 4);
        std::memcpy(&out[i], &bits, 4);
    }
    return true;
}

// setplayerpos's write: vtable[0x28]() first -- the title's own order -- then
// vtable[0x84](obj, &vec3).
static bool WritePlayerPos(PPCContext& ctx, uint8_t* base, uint32_t index,
                           float x, float y, float z)
{
    if (!EnsurePosScratch())
        return false;
    const uint32_t obj = LookupPlayerObject(ctx, base, index);
    if (!obj)
        return false;
    const uint32_t vt = PPC_LOAD_U32(obj);
    if (vt != kKnownPlayerVtable)
    {
        fprintf(stderr, "[debug] teleport DECLINED: player object %08X has vtable %08X, "
                        "not %08X — the vtable indices below are only valid for that "
                        "class\n", obj, vt, kKnownPlayerVtable);
        return false;
    }
    if (!EnsurePosScratch())
        return false;
    const uint32_t vec = g_posScratch + 0x40;
    const float v[3] = {x, y, z};
    for (int i = 0; i < 3; i++)
    {
        uint32_t bits;
        std::memcpy(&bits, &v[i], 4);
        PPC_STORE_U32(vec + uint32_t(i) * 4, bits);
    }
    ctx.r3.u64 = obj;
    CallGuestAt(ctx, base, PPC_LOAD_U32(vt + 0x28));
    ctx.r3.u64 = obj;
    ctx.r4.u64 = vec;
    CallGuestAt(ctx, base, PPC_LOAD_U32(vt + 0x84));
    return true;
}

// CZ_TELEPORT_FILE=<path> — one line, `x y z` (optionally `x y z player`), applied when
// the file's mtime changes. A FILE and not an environment variable for the same reason
// the texture filter is one: the coordinates worth teleporting to come from a .pose
// captured INSIDE the boot you are looking at, and by then the process has long since
// read its environment. This is what makes an operator's F9 a place you can return to.
static void PumpTeleportFromFile(PPCContext& ctx, uint8_t* base)
{
    static const char* path = getenv("CZ_TELEPORT_FILE");
    if (!path)
        return;
    // SAY THAT IT IS ARMED, once. The first version of this printed only on a
    // successful teleport, so "nothing happened" could not be told from "the pump never
    // ran" — which is exactly what happened during its first test, and cost three runs
    // to notice (gotcha 151).
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        fprintf(stderr, "[debug] CZ_TELEPORT_FILE armed: write `x y z` into %s to move "
                        "the player through the title's own setplayerpos path\n", path);
    }
    static long long lastMtime = -2;
    struct stat st{};
    const long long now = (stat(path, &st) == 0) ? (long long)st.st_mtime : -1;
    if (now == lastMtime)
        return;
    lastMtime = now;
    if (now < 0)
        return;
    FILE* f = fopen(path, "rb");
    if (!f)
        return;
    // THE WRITE IS NOT SAFE YET, AND THIS SAYS SO RATHER THAN CRASHING THE OPERATOR.
    //
    // `setplayerpos`'s setter (vtable+0x84 -> 0x8243A1F0) is a real function -- it calls
    // 0x82439F90 and then writes the position at this+0x620 -- and calling it from the
    // XamInputSetState hook, which is where these pumps run, faults inside guest code.
    // Measured: armed with a teleport request the run dies at 1,593 lines where the
    // duration-matched control reaches 31,994. The console runs that command from the
    // game's own safe point; we do not have that point yet, and finding it is the next
    // task. Until then this refuses unless someone asks for the crash explicitly.
    static const bool allowUnsafe = getenv("CZ_TELEPORT_UNSAFE") != nullptr;
    if (!allowUnsafe)
    {
        fprintf(stderr, "[debug] teleport REFUSED: the setter is not safe to call from "
                        "this hook (it faults; see phase5-notes §6bm). Set "
                        "CZ_TELEPORT_UNSAFE=1 to try it anyway.\n");
        return;
    }
    float x = 0, y = 0, z = 0;
    unsigned index = 0;
    const int got = fscanf(f, "%f %f %f %u", &x, &y, &z, &index);
    fclose(f);
    if (got < 3)
    {
        fprintf(stderr, "[debug] CZ_TELEPORT_FILE: '%s' does not read as `x y z` — "
                        "IGNORED (a teleport that silently did nothing would look "
                        "exactly like one the game refused)\n", path);
        return;
    }
    PPCContext call = ctx;                  // see PumpPlayerPosCache: never the caller's
    float before[3] = {0, 0, 0};
    const bool had = ReadPlayerPos(call, base, index, before);
    if (!WritePlayerPos(call, base, index, x, y, z))
    {
        fprintf(stderr, "[debug] teleport to (%.2f, %.2f, %.2f): NO PLAYER OBJECT — "
                        "is a level running?\n", x, y, z);
        return;
    }
    float after[3] = {0, 0, 0};
    const bool ok = ReadPlayerPos(call, base, index, after);
    // READ BACK. The engine may clamp, drop to ground, or refuse a point inside
    // geometry, and "the call returned" is not "the player moved" — only the second
    // read can tell those apart (gotcha 30's shape: show the thing can fail).
    fprintf(stderr, "[debug] teleport player %u: (%.2f, %.2f, %.2f) -> asked "
                    "(%.2f, %.2f, %.2f) -> now (%.2f, %.2f, %.2f)%s\n",
            index, had ? before[0] : 0.f, had ? before[1] : 0.f, had ? before[2] : 0.f,
            x, y, z, ok ? after[0] : 0.f, ok ? after[1] : 0.f, ok ? after[2] : 0.f,
            ok ? "" : "  (read-back FAILED)");
}

// Keep the cached position fresh for the pose capture, cheaply: one virtual call per
// pump, and only when something is actually going to read it.
static void PumpPlayerPosCache(PPCContext& ctx, uint8_t* base)
{
    // Armed by an F9 capture too, because the READ is measured safe: the same recipe
    // run for 420 s with this on gives 0 guest faults over 32,295 log lines against the
    // control's 0 over 31,994 — matched duration, matched depth. That check exists
    // because an EARLIER version of this path, which made a virtual call instead of
    // reading the field, crashed the run at line 1,593 of the same recipe. The lesson
    // is in the read itself: it makes no guest call at all.
    if (!getenv("CZ_CAPTURE_KEY") && !getenv("CZ_POSE_TRACE"))
        return;
    float p[3];
    // A PRIVATE CONTEXT AND A PRIVATE STACK, never the caller's.
    //
    // The pump is invoked from a hook inside guest code, so `ctx` is a live thread's
    // register file: setting r3/r4/ctr on it to make a call corrupts whatever the
    // hooked function was holding there. The existing AutoChuck write gets away with
    // it because it fires once per state change; this runs every poll, and the first
    // version of it produced a NULL dereference several calls deep in guest code that
    // two runs of the same recipe on the previous binary did not. Copy the context,
    // give it its own stack out of our scratch, and the borrowed thread is untouched.
    // The stack is deliberately the borrowed thread's OWN: a call pushes BELOW the
    // current frame, which is free space by construction, where a hand-made stack in
    // our scratch would grow down into the very vectors it is passing.
    PPCContext call = ctx;
    if (!ReadPlayerPos(call, base, 0, p))
        return;
    for (int i = 0; i < 3; i++)
        g_playerPos[i].store(p[i], std::memory_order_release);
    g_playerPosAtMs.store(DebugElapsedMs(), std::memory_order_release);
    if (getenv("CZ_POSE_TRACE"))
    {
        static long long lastPrint = -100000;
        const long long now = DebugElapsedMs();
        if (now - lastPrint >= 1000)
        {
            lastPrint = now;
            fprintf(stderr, "[debug] player at (%.2f, %.2f, %.2f)\n", p[0], p[1], p[2]);
        }
    }
}

// The pose capture's player half, read from the RENDER thread — so it serves the
// cached value rather than making a guest call, and prints how old it is.
extern "C" int CZ_DebugPlayerPos(float out[3], long long* ageMs)
{
    const long long at = g_playerPosAtMs.load(std::memory_order_acquire);
    if (at < 0)
        return 0;
    for (int i = 0; i < 3; i++)
        out[i] = g_playerPos[i].load(std::memory_order_acquire);
    if (ageMs)
        *ageMs = DebugElapsedMs() - at;
    return 1;
}

void DebugTunables_PumpAutoChuck(PPCContext& ctx, uint8_t* base)
{
    PumpDebugFlagsFromEnvironment(base);
    PumpGuestDiagnosticsFromEnvironment(base);
    PumpAutoChuckFromEnvironment(ctx, base);
    PumpPlayerPosCache(ctx, base);
    PumpTeleportFromFile(ctx, base);
}

// True while a BACK should be injected to close a screen AutoChuck opened. Read from the
// input arm on a guest thread.
bool DebugTunables_WantAutoBack()
{
    const long long until = g_autoBackUntilMs.load(std::memory_order_acquire);
    if (until < 0)
        return false;
    const long long now = DebugElapsedMs();
    if (now > until)
    {
        g_autoBackUntilMs.store(-1, std::memory_order_release);
        static bool said = false;
        if (!said)
        {
            said = true;
            fprintf(stderr, "[debug] AutoChuck close: B released\n");
        }
        return false;
    }
    return now >= g_autoBackFromMs.load(std::memory_order_acquire);
}

// The barrier's predicate. Read from the synthetic-input arm on a guest thread.
uint32_t DebugTunables_ScreenRequestsServiced()
{
    return g_screenRequestsServiced.load(std::memory_order_acquire);
}

void DebugTunables_PumpPendingScreen(PPCContext& ctx, uint8_t* base)
{
    if (!g_pendingScreen.name || !g_frontendTransitionManager)
        return;
    const PendingScreen p = g_pendingScreen;
    g_pendingScreen = {};
    fprintf(stderr, "[debug] %s: the manager appeared at %llds — servicing the HELD "
                    "request now\n", p.name, DebugElapsedSeconds());
    RequestFrontendScreen(ctx, base, p.nameAddress, p.nameLength, p.name);
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
        if (SetAutoChuckState(ctx, base, node - kAutoChuckBase))
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
// EVERY screen transition passes through here, ours and the title's own — which makes it
// the place to learn what the AI is doing behind our back.
//
// AutoChuck opens the map about two minutes into a roam and parks the run on it. The input
// path is NOT the source: a counter on BACK delivered to the guest reads 0 while the map
// opens, and no synthetic input is sent at all for the two minutes before it. So the title
// requests that screen itself, and the only way to close it automatically is to recognise
// it. r4 is the screen HASH the frontend was asked for; logging the distinct ones with a
// timestamp is what lets the map's hash be identified from a run rather than guessed.
PPC_FUNC(sub_827F6D40)
{
    g_frontendTransitionManager = ctx.r3.u32;
    const uint32_t hash = ctx.r4.u32;
    if (getenv("CZ_SCREEN_TRACE"))
    {
        static std::vector<uint32_t> seen;
        const bool isNew = std::find(seen.begin(), seen.end(), hash) == seen.end();
        if (isNew)
            seen.push_back(hash);
        fprintf(stderr, "[screen] transition -> hash %08X at %llds%s\n", hash,
                DebugElapsedSeconds(), isNew ? "   <-- FIRST TIME" : "");
    }
    // Only while an AutoChuck state is being held: an unattended AI run is the one case
    // where nobody is there to close it, and a player who opened the map deliberately
    // would not thank us for shutting it.
    if (g_autoChuckHeld.load(std::memory_order_acquire))
    {
        static const std::vector<uint32_t> closeHashes = [] {
            std::vector<uint32_t> v;
            const char* e = getenv("CZ_AUTOCHUCK_CLOSE_HASHES");
            if (!e)
                return std::vector<uint32_t>{ 0x06903E1Au, 0x890DF3E5u };
            for (const char* p = e; *p;)
            {
                char* end = nullptr;
                const unsigned long h = strtoul(p, &end, 16);
                if (end == p)
                    break;
                v.push_back(uint32_t(h));
                p = (*end == ',') ? end + 1 : end;
            }
            return v;
        }();
        const long long now = DebugElapsedSeconds();
        if (std::find(closeHashes.begin(), closeHashes.end(), hash) != closeHashes.end() &&
            now - g_autoBackLastMs >= 3)   // one press per screen, not per transition
        {
            g_autoBackLastMs = now;
            const long long from = DebugElapsedMs() + AutoCloseDelayMs();
            g_autoBackFromMs.store(from, std::memory_order_release);
            g_autoBackUntilMs.store(from + 500, std::memory_order_release);
            const uint64_t n = g_autoBackCount.fetch_add(1) + 1;
            fprintf(stderr, "[debug] AutoChuck opened screen %08X at %llds — pressing B in "
                            "%lldms to close it (%llu so far; CZ_AUTOCHUCK_CLOSE_HASHES= to "
                            "disable, CZ_AUTOCHUCK_CLOSE_DELAY_MS=N to retime)\n",
                    hash, now, AutoCloseDelayMs(), (unsigned long long)n);
        }
    }
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

// ===================================================================================
// THE POSE ANCHOR — what the F9 capture needs to make a shot reproducible.
//
// WHY THIS EXISTS. Every picture claim in this port has been anchored to "the operator
// walked somewhere and pressed F9", which no headless run can reproduce and no second
// session can revisit: the striped-material class picks a different quality level per
// boot, so "go back to the tanker" is not the same experiment twice. A shot is
// reproducible only if the CAMERA and the PLAYER can be restored, so the capture has to
// record them.
//
// This exposes the player's game object to the renderer's F9 path. It is the same
// pointer `CZ_AUTOCHUCK` drives the AI through (`g_gameDebugController + 0x30`), which
// is the one object in this engine we have already proven we can find and write.
// The renderer dumps its bytes; which offsets hold the position is decided OFFLINE by
// diffing two captures taken in different places, not guessed here (gotcha 214's shape:
// bind a field by what changes with the thing, not by what looks plausible).
extern "C" uint32_t CZ_DebugPlayerObject()
{
    uint8_t* base = g_memory.base;
    return g_gameDebugController ? PPC_LOAD_U32(g_gameDebugController + 0x30) : 0;
}

// Write the head of the player object into an already-open .pose file, and return the
// object address so the caller can report it. Values are printed as BOTH the raw dword
// and the float it would be: a position may be stored either way, and the offline diff
// that names the fields should not need a second capture session to find out.
extern "C" uint32_t CZ_DebugWritePlayerObject(FILE* f, uint32_t bytes)
{
    uint8_t* base = g_memory.base;
    const uint32_t obj = CZ_DebugPlayerObject();
    fprintf(f, "player_object %08X\n", obj);
    fprintf(f, "controller %08X\n", g_gameDebugController);
    if (!obj)
    {
        // SAY SO. A pose file that is silently missing its player half reads exactly
        // like a player who happened to be at the origin (gotchas 25, 151).
        fprintf(f, "# no player object yet (no level running?) — NOT dumped\n");
        return 0;
    }
    for (uint32_t off = 0; off < bytes; off += 4)
        fprintf(f, "obj+%04X %08X %.6f\n", off, PPC_LOAD_U32(obj + off),
                [&] { const uint32_t b = PPC_LOAD_U32(obj + off); float v;
                      memcpy(&v, &b, 4); return v; }());
    return obj;
}
