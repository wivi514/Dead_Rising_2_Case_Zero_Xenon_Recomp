// The PP leaderboard's flush timer: the title writes a dirty stats cache at
// most once every 360 s, and a player who saves and opens the board sees the
// old number for up to six minutes (player issue #4, "leaderboard does not
// get properly synced").
//
// WHAT THE TITLE DOES (read out of the image, 2026-09-14). The PP board is
// LEADERBOARD_PRESTIGE_POINTS (index 1 in the name table at 0x829DE540). The
// stats cache (`cStatsCache`, constructed by sub_82579390) takes a stat
// (sub_825794A0, "caching a stat to leaderboard %s") from exactly three
// places: loading a save from the main menu (sub_824D9F30), the SAVE
// completion path in the system-message handler (sub_824C3D20 at 824C42E0),
// and the menu verb handler (sub_82530E98). A board-1 cache sets a DIRTY byte
// (+0xE8); the cache's per-frame update (sub_82567798) accumulates dt into
// `FlushCacheTimer` (+0x1D8) and, once DIRTY and the timer exceeds the float
// at 0x8207BFE0 (360.0, read by the constructor as the timer's initial value
// so the first flush is immediate, and by the update as the threshold —
// those two are its only readers), writes the cache through sub_82567720
// ("trying to write stats to %s") -> cMsGameSession::WriteStats (sub_825CDB50)
// -> XSessionWriteStats (000B0025, kernel/imports.cpp) -> libxlive's durable
// queue -> the server, which aggregates PP as MAX. Measured end to end
// headless with the forced-write arm below: the server row updated within
// ~10 s of the title's write. The six minutes are the title's own choice,
// made for a console where every write cost a Live round trip; XenonLive's
// queue takes them one by one for nothing, and the only writes the cadence
// gates are the ones a save or a load already made.
//
// THE FIX: the threshold becomes CZ_LEADERBOARD_FLUSH_S seconds (default 2;
// `=360` is the title's own value, the control). Stored at the constant's
// address before the constructor and the update read it — the flat map is
// writable and the constant has no other reader.
//
// CZ_LEADERBOARD_TRACE=1 prints the chain; CZ_LEADERBOARD_FORCE_WRITE=N marks
// the cache dirty on the N-th update call (a write of whatever the cache holds
// — the way to watch the chain without a save).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82579390);   // cStatsCache constructor
extern "C" PPC_FUNC(__imp__sub_82567798);   // cStatsCache::Update(dt)
extern "C" PPC_FUNC(__imp__sub_82567720);   // WriteBoard(board, stats)
extern "C" PPC_FUNC(__imp__sub_825CDB50);   // cMsGameSession::WriteStats
extern "C" PPC_FUNC(__imp__sub_825794A0);   // CacheStat(board, stats)

namespace
{
constexpr uint32_t kFlushTimerConst = 0x8207BFE0;   // float, 360.0 as shipped

bool TraceOn()
{
    static const bool on = getenv("CZ_LEADERBOARD_TRACE") != nullptr;
    return on;
}

void ApplyFlushSeconds(uint8_t* base)
{
    static bool applied = false;
    if (applied)
        return;
    applied = true;
    float seconds = 2.0f;
    if (const char* e = getenv("CZ_LEADERBOARD_FLUSH_S"))
    {
        const float v = strtof(e, nullptr);
        if (v > 0.0f)
            seconds = v;
        else
            fprintf(stderr, "[leaderboard] CZ_LEADERBOARD_FLUSH_S=%s is not > 0 — ignored\n", e);
    }
    uint32_t bits = PPC_LOAD_U32(kFlushTimerConst);
    float was;
    memcpy(&was, &bits, sizeof(was));
    if (was != 360.0f)
    {
        // Not the constant this was written for: the image moved under us.
        fprintf(stderr, "[leaderboard] flush-timer constant reads %g, not 360 — left alone\n",
                was);
        return;
    }
    memcpy(&bits, &seconds, sizeof(bits));
    PPC_STORE_U32(kFlushTimerConst, bits);
    fprintf(stderr, "[leaderboard] PP board flush timer 360 s -> %g s "
                    "(CZ_LEADERBOARD_FLUSH_S=360 restores the title's cadence)\n",
            seconds);
}
}

PPC_FUNC(sub_82579390)
{
    ApplyFlushSeconds(base);
    __imp__sub_82579390(ctx, base);
}

PPC_FUNC(sub_82567798)
{
    ApplyFlushSeconds(base);
    if (!TraceOn())
    {
        __imp__sub_82567798(ctx, base);
        return;
    }
    const uint32_t self = ctx.r3.u32;
    static auto last = std::chrono::steady_clock::now();
    static uint32_t calls = 0;
    ++calls;
    const auto now = std::chrono::steady_clock::now();
    static const long forceAt = getenv("CZ_LEADERBOARD_FORCE_WRITE")
        ? strtol(getenv("CZ_LEADERBOARD_FORCE_WRITE"), nullptr, 10) : 0;
    if (forceAt > 0 && calls == uint32_t(forceAt))
    {
        PPC_STORE_U8(self + 0xE8, 1);
        fprintf(stderr, "[lbtrace] FORCED dirty at update call %u\n", calls);
    }
    const bool dirtyBefore = PPC_LOAD_U8(self + 0xE8) != 0;
    uint32_t bits = PPC_LOAD_U32(self + 0x1D8);
    float timer;
    memcpy(&timer, &bits, 4);
    __imp__sub_82567798(ctx, base);
    const bool dirtyAfter = PPC_LOAD_U8(self + 0xE8) != 0;
    if (now - last > std::chrono::seconds(30) || dirtyBefore != dirtyAfter)
    {
        last = now;
        fprintf(stderr, "[lbtrace] update: %u calls, timer %.1f s, dirty %d -> %d, dt %.4f\n",
                calls, timer, dirtyBefore, dirtyAfter, float(ctx.f1.f64));
    }
}

PPC_FUNC(sub_82567720)
{
    const uint32_t board = ctx.r4.u32;
    __imp__sub_82567720(ctx, base);
    if (TraceOn())
        fprintf(stderr, "[lbtrace] WriteBoard(%u) -> %u (trial byte %u)\n", board,
                ctx.r3.u32 & 0xFF, PPC_LOAD_U8(0x82A57BFE));
}

PPC_FUNC(sub_825CDB50)
{
    const uint32_t self = ctx.r3.u32;
    const uint32_t sessState = PPC_LOAD_U32(self + 0x1E8);
    const uint32_t liveState = PPC_LOAD_U32(self + 0x118);
    const uint32_t pending = PPC_LOAD_U32(self + 0x2E8);
    __imp__sub_825CDB50(ctx, base);
    if (TraceOn())
        fprintf(stderr, "[lbtrace] cMsGameSession::WriteStats: session state %u, live "
                        "state %u, %u pending -> %u\n",
                sessState, liveState, pending, ctx.r3.u32 & 0xFF);
}

PPC_FUNC(sub_825794A0)
{
    const uint32_t board = ctx.r4.u32;
    __imp__sub_825794A0(ctx, base);
    if (TraceOn())
        fprintf(stderr, "[lbtrace] CacheStat(board %u)\n", board);
}
