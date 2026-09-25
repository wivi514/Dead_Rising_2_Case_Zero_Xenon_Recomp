// The speedrun status block — see speedrun_block.h for the contract and
// docs/speedrun-block.md for the reader's side.
//
// WHERE EVERY NUMBER IN HERE COMES FROM. Nothing below is guessed; each address was
// read out of the title's own code with tools/gdis.py, and the note beside it names the
// instruction that states it. That matters because all of it is silent when wrong — a
// bad pointer chain yields a plausible zero, which a load remover would read as
// "not loading" forever.
//
//   TOP-LEVEL GAME STATE
//   sub_824B57C8 is the engine's own "which top-level state am I in" accessor:
//       lis r9,0x82AD ; lwz r9,0x5EF8(r9)      r9 = *(0x82AD5EF8), the game manager
//       lwz r9,0x24(r9)                        +0x24 = the CURRENT state's name id
//       lis r11,0x82A6 ; addi r10,r11,-0x6ED4  r10 = 0x82A5912C, the state-name table
//       loop r11 = r10 .. r10+0x2C, index r8:  if *r11 == r9 return r8
//   so the state is an INDEX into an 11-entry table of interned name hashes.
//   sub_829A21C0 fills that table at boot, in this order, and the strings it interns
//   are the names below. Slot 0 is never written by it and reads 0.
//
//   The chain was confirmed live before this file was written: a headless DebugJump run
//   with CZ_STATE_TRACE=1 printed exactly
//       Startup -> LegalScreen -> Loading -> FrontEnd -> FEToGame -> Loading -> InGame
//   with the ids 26344A65 / 267CDB6F / 817315A6 / D6BB036E / B3876116 / A334C769, i.e.
//   a level load IS a top-level Loading state and not something private to the level
//   system. That run is why `isLoading` is FEToGame-or-Loading and nothing cleverer.
//
//   CINEMATICS
//   sub_8248F728 reaches the cinematic manager the same way every frame:
//       lis r27,0x82A5 ; lwz r11,0x7428(r27)   r11 = *(0x82A57428)
//       lwz r11,0x2C(r11) ; lwz r3,8(r11)      r3  = the cinematic manager
//       lwz r11,0x1570(r3)                     +0x1570 = the RUNNING cinematic, 0 if none
//   and it only calls the manager's update (sub_82478FC8) when that word is non-zero,
//   which is what makes the word itself the "is a cutscene playing" predicate rather
//   than a side effect of one.
//
//   sub_82478FC8's debug overlay reads two more fields, and it is the authority on both:
//       lbz r11,0x1C(cinematic)  -> "Exclusive Cinematic" / "Non-Exclusive Cinematic"
//       lbz r11,0x15BC(manager)  -> the length of the manager's name string
//   The name is assigned in sub_8247A828 (the manager's PlayCinematic) at
//       addi r3,r31,0x159C ; bl sub_8277A758
//   so manager+0x159C is a string object, and sub_827740F0 — the assign it tail-calls —
//   states that class's layout exactly: `char buf[0x20]; uint8 len;`, inline while
//   len < 0x1F and a heap pointer in the first word once len >= 0x1F. That threshold is
//   load-bearing here: "701_chuck_arrives_in_town" is 25 bytes and inline,
//   "707_give_katey_zombrex_psycho_intro" is 35 and lives on the heap, so a decoder that
//   only handled one of the two cases would work through most of the game and produce
//   rubbish at the mission a runner most wants to split on.
//
//   THE CROSS-CHECK. Reading the string out of the manager is a decode we wrote, so it
//   gets an oracle: the hook on sub_8247A828 below also captures the name the engine was
//   PASSED, and CZ_SPEEDRUN_TRACE=1 prints the two side by side at every cinematic start.
//   If they ever disagree, the decode is wrong and the log says so rather than the block
//   quietly carrying a bad name.
//
// Default ON. Its cost is one 4 KB page, and per PRESENTED frame (not per draw) a dozen
// guest loads, an 11-entry scan of the state table and a 256-byte store — stated as a
// structure rather than as milliseconds, because nobody has measured it and this project
// does not quote numbers it has not taken. CZ_SPEEDRUN_BLOCK=0 switches it off entirely,
// which is also the control arm if a frame-time A/B is ever wanted.

#include "speedrun_block.h"

#include "ppc_recomp_shared.h"

#include "../host/host_paths.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
// Linux 4.17 / glibc 2.28. Declared here so an older build host still compiles; on a
// kernel that does not know the flag the mmap simply ignores it, which the
// address check right after it catches.
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

extern "C" PPC_FUNC(__imp__sub_8247A828);

namespace
{

constexpr uint32_t kGameManagerPtr = 0x82AD5EF8;  // -> game manager; +0x24 is the state id
constexpr uint32_t kStateTable     = 0x82A5912C;  // 11 interned name hashes
constexpr uint32_t kStateSlots     = 11;
constexpr uint32_t kCineRootPtr    = 0x82A57428;  // -> +0x2C -> +0x08 = cinematic manager
constexpr uint32_t kCineCurrent    = 0x1570;      // manager: the running cinematic
constexpr uint32_t kCineNameBuf    = 0x159C;      // manager: name string, buf or pointer
constexpr uint32_t kCineNameLen    = 0x15BC;      // manager: that string's length byte
constexpr uint32_t kCineExclusive  = 0x001C;      // cinematic: the exclusive-NIS flag

// Index order is sub_829A21C0's store order: slot 0 is unwritten, then +0x04 up.
const char* const kStateNames[kStateSlots] = {
    "None", "Startup", "LegalScreen", "BCGIntro", "FrontEnd", "FEToGame",
    "Loading", "InGame", "InGameTut1", "FEToGameShow", "GameShow",
};
constexpr uint32_t kStateFEToGame = 5;
constexpr uint32_t kStateLoading  = 6;
constexpr uint32_t kStateInGame   = 7;
constexpr uint32_t kStateTut1     = 8;
constexpr uint32_t kStateGameShow = 10;

CzSpeedrunBlock* g_block = nullptr;
uint64_t g_frame = 0;
uint32_t g_loadCount = 0;
uint32_t g_cineCount = 0;
bool g_wasLoading = false;
uint32_t g_lastCine = 0;
bool g_decodeTrusted = true;

// What sub_8247A828 was actually handed, for the decode's cross-check. Written on a
// guest thread, read on the pump thread; a torn read would only mis-print a log line,
// but the atomic costs nothing here (a few calls a session) and keeps it honest.
std::atomic<uint32_t> g_playedNameVa{ 0 };

bool Enabled()
{
    static const bool on = [] {
        const char* e = getenv("CZ_SPEEDRUN_BLOCK");
        return !e || e[0] != '0';
    }();
    return on;
}

bool Tracing()
{
    static const bool on = getenv("CZ_SPEEDRUN_TRACE") != nullptr;
    return on;
}

// Guest reads. The whole 4 GB is mapped, so these cannot fault — but an unvalidated
// pointer still yields a number that MEANS nothing, so every dereference is gated on
// the pointer looking like a guest address at all.
inline bool PlausibleVa(uint32_t va, uint32_t need)
{
    return va >= 0x10000u && uint64_t(va) + need < uint64_t(PPC_MEMORY_SIZE);
}

inline uint32_t Read32(const uint8_t* base, uint32_t va)
{
    uint32_t v;
    memcpy(&v, base + va, 4);
    return __builtin_bswap32(v);
}

inline uint32_t Deref(const uint8_t* base, uint32_t va, uint32_t fieldNeed)
{
    if (!PlausibleVa(va, 4))
        return 0;
    const uint32_t p = Read32(base, va);
    return PlausibleVa(p, fieldNeed) ? p : 0;
}

// The string class sub_827740F0 defines: inline below 0x1F, a heap pointer at or above.
void ReadGuestString(const uint8_t* base, uint32_t objVa, uint32_t bufOff, uint32_t lenOff,
                     char* out, size_t outSize)
{
    out[0] = '\0';
    if (!objVa || !PlausibleVa(objVa, lenOff + 1))
        return;
    uint32_t len = base[objVa + lenOff];
    if (!len)
        return;
    uint32_t src = objVa + bufOff;
    if (len >= 0x1F)
    {
        src = Deref(base, objVa + bufOff, 1);
        if (!src)
            return;
        if (len > 63)      // the heap block sub_827740F0 copies into is 0x40 bytes
            len = 63;
    }
    if (len >= outSize)
        len = uint32_t(outSize) - 1;
    if (!PlausibleVa(src, len))
        return;
    memcpy(out, base + src, len);
    out[len] = '\0';
    // A name is ASCII. Anything else means the decode landed somewhere wrong, and an
    // empty field is a far better answer than a convincing one built out of noise.
    for (uint32_t i = 0; i < len; ++i)
    {
        if (uint8_t(out[i]) < 0x20 || uint8_t(out[i]) > 0x7E)
        {
            out[0] = '\0';
            return;
        }
    }
}

void CopyCString(const uint8_t* base, uint32_t va, char* out, size_t outSize)
{
    out[0] = '\0';
    if (!PlausibleVa(va, 1))
        return;
    size_t i = 0;
    for (; i + 1 < outSize; ++i)
    {
        const char c = char(base[va + i]);
        if (!c)
            break;
        out[i] = c;
    }
    out[i] = '\0';
}

} // namespace

void SpeedrunBlock_Init()
{
    if (!Enabled() || g_block)
        return;

    void* p = nullptr;
#if defined(_WIN32)
    p = VirtualAlloc(reinterpret_cast<void*>(CZ_SPEEDRUN_BLOCK_ADDRESS), 4096,
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p)
        p = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    p = mmap(reinterpret_cast<void*>(CZ_SPEEDRUN_BLOCK_ADDRESS), 4096,
             PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || p != reinterpret_cast<void*>(CZ_SPEEDRUN_BLOCK_ADDRESS))
    {
        if (p != MAP_FAILED)
            munmap(p, 4096);
        p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            p = nullptr;
    }
#endif
    if (!p)
    {
        fprintf(stderr, "[speedrun] could not map the status block — the load-remover "
                        "interface is unavailable this run (CZ_SPEEDRUN_BLOCK=0 to "
                        "silence this)\n");
        return;
    }

    g_block = static_cast<CzSpeedrunBlock*>(p);
    memset(g_block, 0, sizeof(*g_block));
    memcpy(g_block->magic, CZ_SPEEDRUN_BLOCK_MAGIC, 8);
    g_block->version = CZ_SPEEDRUN_BLOCK_VERSION;
    g_block->size = sizeof(CzSpeedrunBlock);
    g_block->gameState = 0xFFFFFFFFu;

    const bool atFixed = p == reinterpret_cast<void*>(CZ_SPEEDRUN_BLOCK_ADDRESS);
    fprintf(stderr, "[speedrun] status block v%u at %p (%s), %zu bytes, magic %s\n",
            CZ_SPEEDRUN_BLOCK_VERSION, p,
            atFixed ? "the documented fixed address" : "RELOCATED — the fixed address "
                                                       "was taken; scan for the magic",
            sizeof(CzSpeedrunBlock), CZ_SPEEDRUN_BLOCK_MAGIC);

    // A relocated block is still findable, but only if something says where it went.
    // The file is written unconditionally so a reader never has to care which happened.
    const auto path = HostPaths::Root() / "cz_speedrun_block.txt";
    std::ofstream f(path);
    if (f)
        f << "address=0x" << std::hex << reinterpret_cast<uintptr_t>(p) << "\n"
          << std::dec << "version=" << CZ_SPEEDRUN_BLOCK_VERSION << "\n"
          << "size=" << sizeof(CzSpeedrunBlock) << "\n"
          << "magic=" << CZ_SPEEDRUN_BLOCK_MAGIC << "\n"
          << "fixed=" << (atFixed ? 1 : 0) << "\n";
}

void SpeedrunBlock_Publish(uint8_t* base)
{
    CzSpeedrunBlock* b = g_block;
    if (!b || !base)
        return;

    // Resolve the top-level state: *(0x82AD5EF8) -> +0x24 -> index in the name table.
    uint32_t flow = Deref(base, kGameManagerPtr, 0x28);
    uint32_t stateId = flow ? Read32(base, flow + 0x24) : 0;
    uint32_t stateIndex = 0xFFFFFFFFu;
    if (stateId)
    {
        for (uint32_t i = 0; i < kStateSlots; ++i)
        {
            if (Read32(base, kStateTable + i * 4) == stateId)
            {
                stateIndex = i;
                break;
            }
        }
    }

    // Resolve the cinematic manager: *(0x82A57428) -> +0x2C -> +0x08.
    uint32_t cineMgr = 0;
    if (const uint32_t root = Deref(base, kCineRootPtr, 0x30))
        if (const uint32_t mid = Deref(base, root + 0x2C, 0x0C))
            cineMgr = Deref(base, mid + 0x08, kCineNameLen + 1);
    const uint32_t cine = cineMgr ? Deref(base, cineMgr + kCineCurrent, kCineExclusive + 1) : 0;

    const bool loading = stateIndex == kStateLoading || stateIndex == kStateFEToGame;
    if (loading && !g_wasLoading)
        ++g_loadCount;
    g_wasLoading = loading;

    if (cine && cine != g_lastCine)
    {
        ++g_cineCount;
        // THE DECODE'S ORACLE, and it reports itself. The two sources are independent:
        // `passed` is the plain const char* the engine was handed, `decoded` is our
        // reading of the string class it stored it in. Under CZ_SPEEDRUN_TRACE every
        // start prints both; without it, the FIRST disagreement still prints once,
        // because a name field that is quietly wrong is worse than one that is empty
        // and nothing else in this runtime would ever notice.
        char passed[64];
        CopyCString(base, g_playedNameVa.load(std::memory_order_acquire), passed,
                    sizeof(passed));
        char decoded[64];
        ReadGuestString(base, cineMgr, kCineNameBuf, kCineNameLen, decoded,
                        sizeof(decoded));
        const bool agree = strcmp(passed, decoded) == 0;
        static bool warned = false;
        if (Tracing() || (!agree && !warned))
        {
            if (!agree)
                warned = true;
            fprintf(stderr, "[speedrun] cinematic #%u starts: PlayCinematic said '%s', "
                            "manager+%04X decodes '%s' — %s\n",
                    g_cineCount, passed, kCineNameBuf, decoded,
                    agree ? "AGREE" : "DISAGREE: the string decode is wrong, the block "
                                      "falls back to the passed name");
        }
        // A disagreement means the decode cannot be trusted for the rest of the run
        // either — the failing case is a string-length class, not this one cinematic.
        if (!agree && passed[0])
            g_decodeTrusted = false;
    }
    g_lastCine = cine;

    ++g_frame;

    // Seqlock: odd while the body is inconsistent. The release/acquire pair is what
    // makes an out-of-process reader's retry loop meaningful on a weakly ordered core.
    __atomic_store_n(&b->seq, b->seq | 1u, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    b->frame = g_frame;
    b->monotonicNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
    b->guestBase = uint64_t(uintptr_t(base));
    b->gameState = stateIndex;
    b->gameStateId = stateId;
    b->isLoading = loading ? 1 : 0;
    b->isCutscene = cine ? 1 : 0;
    b->isCutsceneExclusive = (cine && base[cine + kCineExclusive]) ? 1 : 0;
    b->isInGame = (stateIndex == kStateInGame || stateIndex == kStateTut1 ||
                   stateIndex == kStateGameShow) ? 1 : 0;
    b->loadCount = g_loadCount;
    b->cutsceneCount = g_cineCount;
    b->guestFlowObject = flow;
    b->guestCineManager = cineMgr;
    b->guestCurrentCine = cine;

    if (stateIndex < kStateSlots)
        snprintf(b->gameStateName, sizeof(b->gameStateName), "%s", kStateNames[stateIndex]);
    else
        b->gameStateName[0] = '\0';

    // Only overwrite the name with something. The field's promise is "the cinematic that
    // started LAST", so a frame where the manager's string cannot be read must leave the
    // previous answer standing rather than blank it.
    char name[64];
    if (g_decodeTrusted)
        ReadGuestString(base, cineMgr, kCineNameBuf, kCineNameLen, name, sizeof(name));
    else
        name[0] = '\0';
    if (!name[0])
        CopyCString(base, g_playedNameVa.load(std::memory_order_acquire), name,
                    sizeof(name));
    if (name[0])
        snprintf(b->cutsceneName, sizeof(b->cutsceneName), "%s", name);

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&b->seq, (b->seq + 1u) & ~1ull, __ATOMIC_RELEASE);
}

// sub_8247A828 — cCinematicManager::PlayCinematic(this, playerIndex, const char* name).
// The ONLY entry to a cinematic: its seven callers are the mission and script systems,
// and it is what assigns the manager's own name string. Hooked purely as the oracle for
// that string's decode (see the header comment); the pass-through is bit-identical.
PPC_FUNC(sub_8247A828)
{
    g_playedNameVa.store(ctx.r5.u32, std::memory_order_release);
    __imp__sub_8247A828(ctx, base);
}
