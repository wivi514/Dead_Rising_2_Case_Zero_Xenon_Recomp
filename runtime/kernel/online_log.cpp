// The title's ONLINE logger, tapped. CZ_ONLINE_LOG=N.
//
// WHY THIS EXISTS
// ---------------
// Case Zero's co-op layer — the matchmaking state machine, the session, the
// link manager, the join coordinator — reports through its own logger, not
// through the engine's debug printf that CZ_GUEST_LOG prints. Its 2,000-odd
// format strings ("HW MM session state transition to %s", "Received handshake
// but currently has no server!", "Accepted client from %s") are the only
// account of what the online code decided and why. A retail build formats
// each line and drops it. Without this tap a refused join is silence.
//
// It is the Case West tap (runtime/kernel/guest_log.cpp there) carried over:
// Case Zero links the same online layer, and the logger pair has the same
// shape at a different address —
//
//   sub_8255B968(this, level, fmt, ...)   level 1 = error .. 4 = chatter
//   sub_8255B910(this, level0, fmt, ...)  the same, zero-based: adds one,
//                                         range-checks 1..6, then formats
//                                         identically (sub_8254BF88)
//
// Both were found from their format strings: "HW MM session state transition
// to %s" is passed in r5 with 2 in r4 to sub_8255B968 at 0x825CB3D4; the
// zero-based twin is the function immediately before it, byte-for-byte the
// shape Case West's sub_8252A030 has beside sub_8252A098. Lines from the
// zero-based one are marked [title:N*].
//
// sub_82567080(ok, file, message, line) is the online code's DESYNC check:
// when `ok` is false it formats "*** DESYNC ***: <message> > <file>:<line>".
// In this image that print is already behind the CZ_GUEST_DIAG byte
// (0x829EC974), but that switch puts 2,013 formatting sites on the frame
// path (gotcha 7); this tap prints the failed check alone, at level 1.
//
// Named CZ_ONLINE_LOG rather than Case West's CW_GUEST_LOG because here
// CZ_GUEST_LOG already means the engine printf sink, and the two are
// different routines with different populations of callers. The title's own
// name for this subsystem is `online_log` (its verbose level is a tunable,
// `online_log.verbose_level`, at 0x82A57BB0).
//
// CZ_ONLINE_LOG=1 prints levels 1..3; CZ_ONLINE_LOG=N prints up to N.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <ppc_config.h>
#include <ppc_context.h>

extern "C" PPC_FUNC(__imp__sub_8255B968);
extern "C" PPC_FUNC(__imp__sub_8255B910);
extern "C" PPC_FUNC(__imp__sub_82567080);
size_t GuestFormat(char* out, size_t cap, const char* fmt, PPCContext& ctx, uint8_t* base,
                   size_t firstArg);

namespace
{
int g_onlineLogLevel = -1; // -1: not read yet; 0: off

int OnlineLogLevel()
{
    if (g_onlineLogLevel < 0)
    {
        const char* env = std::getenv("CZ_ONLINE_LOG");
        g_onlineLogLevel = 0;
        if (env && *env && *env != '0')
        {
            const int n = std::atoi(env);
            g_onlineLogLevel = n > 1 ? n : 3;
        }
    }
    return g_onlineLogLevel;
}

// A line identical to the one before it is counted, not printed: with no
// voice engine the session reports "User 0 cannot be added to the chat" every
// frame (23,781 times in one 3-minute host run, part 2), and a log that is
// 78% one line hides the state machine it exists to show. The count is
// printed when the next different line arrives, so nothing is lost.
std::mutex g_lineMutex;   // the online layer logs from more than one guest thread
char g_lastLine[1024];
unsigned g_lastRepeats = 0;

void OnlineLogLine(PPCContext& ctx, uint8_t* base, int level, const char* mark)
{
    if (OnlineLogLevel() >= level && ctx.r5.u32 != 0)
    {
        char buf[1024];
        const char* fmt = reinterpret_cast<const char*>(base + ctx.r5.u32);
        GuestFormat(buf, sizeof buf, fmt, ctx, base, 3);
        const size_t n = std::strlen(buf);
        std::lock_guard<std::mutex> lock(g_lineMutex);
        if (std::strcmp(buf, g_lastLine) == 0)
        {
            g_lastRepeats++;
            return;
        }
        if (g_lastRepeats)
            fprintf(stderr, "[title] (previous line repeated %u more time%s)\n", g_lastRepeats,
                    g_lastRepeats == 1 ? "" : "s");
        g_lastRepeats = 0;
        std::strncpy(g_lastLine, buf, sizeof g_lastLine - 1);
        fprintf(stderr, "[title:%d%s] %s%s", level, mark, buf,
                (n && buf[n - 1] == '\n') ? "" : "\n");
    }
}
} // namespace

PPC_FUNC(sub_8255B968)
{
    OnlineLogLine(ctx, base, int(ctx.r4.u32), "");
    __imp__sub_8255B968(ctx, base);
}

PPC_FUNC(sub_8255B910)
{
    OnlineLogLine(ctx, base, int(ctx.r4.u32) + 1, "*");
    __imp__sub_8255B910(ctx, base);
}

PPC_FUNC(sub_82567080)
{
    if (OnlineLogLevel() >= 1 && (ctx.r3.u32 & 0xff) == 0 && ctx.r5.u32 != 0)
    {
        const char* msg = reinterpret_cast<const char*>(base + ctx.r5.u32);
        const char* file = ctx.r4.u32 ? reinterpret_cast<const char*>(base + ctx.r4.u32) : "?";
        const char* slash = std::strrchr(file, '/');
        if (!slash)
            slash = std::strrchr(file, '\\');
        fprintf(stderr, "[title:desync] %s  (%s:%u)\n", msg, slash ? slash + 1 : file,
                ctx.r6.u32);
    }
    __imp__sub_82567080(ctx, base);
}
