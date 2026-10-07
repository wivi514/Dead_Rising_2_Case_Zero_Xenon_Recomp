// THE GUEST ARRIVES DRESSED AND THE HOST SEES HIM NAKED. Co-op, 2026-09-29.
//
// THE REPORT. "When the guest arrives in a session they are fully clothed but often
// they appear invisible to the host, or with the torso missing." Fully clothed on his
// OWN screen, so his save and his own player are fine; it is the COPY of him on the
// other machine that is wrong. And INTERMITTENT, which is what separates this from part
// 4's `chest_NONE` — that one was deterministic, it is fixed as data, and it is not this.
//
// WHY "INVISIBLE" AND "NO TORSO" ARE ONE DEFECT AT TWO MAGNITUDES. In this engine a
// character IS his seven clothing pieces. The load requester's own prefix table
// (0x829D42B8) names them and settles it without a guess: 0 headwear, 1 head,
// 2 facewear, 3 chest, 4 hands, 5 leg, 6 feet. Nothing else of Chuck is drawn. So "the
// torso is missing" is exactly "piece 3 never arrived" and "invisible" is exactly "none
// of the seven arrived".
//
// THE CHAIN, read out of the title's own code, with the two ends measured on a live run:
//
//   1. REPORT   the joiner sends seven tEventOutfit messages; the receiver
//               `sub_82570E78` hands each to `sub_82371978(clothing, part, name)`.
//               **`clothing = *(player + 0xCE74)` — A LOAD** (`lwzx r3, r27, 0xCE74`
//               there, `lwz r3, 0(player + 0xCE74)` in the co-op flow at 0x82582A9C).
//   2. RECORD   `clothing + part*0x30`: +0x4AE8 the name (a 0x24-byte SSO string),
//               +0x4B0C its hash, +0x4B10 the clothingdatabase row. The co-op setter
//               writes the name and the hash and NOTHING ELSE; the single-player setter
//               `sub_8238C6F8` also writes the tag and the db row.
//   3. LOAD     one change-part event per part; `sub_82271BB0` takes it and calls the
//               requester `sub_82270290`, which builds "<prefix><name>" and streams.
//   4. ARRIVE   the piece lands in the clothing manager's per-player LOAD RECORD:
//               `mgr + 0x10 + (mgrPlayerIdx*13 + part) * 0x128`, and that record is the
//               readable answer to "is this piece here".
//
// THE RECORD'S LAYOUT WAS MEASURED, NOT GUESSED, and part 2 paid for it. Chuck's default
// outfit has an EMPTY facewear, so record 2 is a piece that was never requested sitting
// in the same dump as six that were (CZ_COOP_OUTFIT_DUMP=1 prints it again any time):
//
//   field      a piece that arrived (rec 0,1,3,4,5,6)   the empty one (rec 2)
//   +0x00      the piece's own name, inline SSO         (empty)
//   +0x38      a model handle (0x175, 0x176, …)         0xFFFFFFFF
//   +0x78      a texture handle (0x166, 0x167, …)       0xFFFFFFFF
//   +0xB8      1 (files landed)                         0
//   +0xBC      the part index it serves                 13 = idle
//   +0x11C     the streaming budget in BYTES            (the part's budget)
//
// So **a piece is present iff its record's +0x38 is a real handle**, and the record's
// own name at +0x00 is a free cross-check that we are reading the right record at all.
//
// RETRACTED IN PLACE, because it cost this part two runs and it will cost the next one
// the same: `clothing + part*0x2C + 0x49A4` IS a seven-slot attached-model array, it is
// written by `sub_82371B88` and read by `sub_82371A70`, and **it is never written for
// the PLAYER**. Its callers are all inside `sub_82165DE8`, a different actor path. A
// first cut of this file used it as the "attached" test and reported a correctly dressed
// single-player Chuck as missing all six of his pieces. The positive control below is
// the only reason that was caught here instead of in an operator's co-op session.
//
// THE CONTROL IS BUILT IN, and it is why this is allowed to act on what it finds:
//   * the LOCAL player is swept by the same code as the remote one. He is visibly
//     correct — someone is looking at him — so if he ever reads BAD the reading is
//     wrong, and the repair refuses and says so.
//   * the load record's own name must match the clothing record's name; if it does not,
//     we are reading the wrong record and nothing here means anything.
//   * CZ_COOP_OUTFIT_CHECK_SOLO=1 runs the whole sweep in SINGLE PLAYER, where the
//     answer must be "all seven". A check that has only ever been silent has not been
//     shown capable of reading a correct player (gotcha 30).
//
// THE REPAIR is the title's own change-part event, rebuilt field for field from the
// co-op flow's own per-part post (0x82582A60..0x82582AD4) with the name taken from the
// record the report already wrote. It is not a new mechanism: it is the same event the
// join posts, posted again for the one part that did not come back. It fires only after
// a part has read missing for three consecutive sweeps (~6 s, past any honest async
// load), at most three times per part, and never while a control is bad.
//
// OPERATOR-VERIFIED ON TWO MACHINES, 2026-09-29, and the log and their eye agree. The
// guest arrived with all six of his named pieces NOT LOADED and rendering invisible; six
// re-posts went out on attempt 1; every one answered `LoadDone player 1 part N: 1 of 1
// files`; the next sweep read `player 1 (the other machine) has all 6 of the pieces he is
// wearing`, and they reported him visible. Attempts 2 and 3 never fired, 0 desyncs.
//
// WHAT THAT RUN SAYS THE DEFECT IS, and it is upstream of this file: his names ARRIVED
// and were RECORDED correctly (`young_chuck`, `naked`, `young_chuck_under`) — so nothing
// was ever wrong with the report. Nothing asked for the FILES. The title's own per-part
// change-part events at join do not take effect; re-posting the identical events does.
// That is a timing defect in the join, the same shape part 6 found when a row applied at
// level start went nowhere because the player did not exist yet. This file is the
// backstop, not the cure; the cure is finding why the join's own posts are dropped.
//
// TWO SUSPECTS THIS FILE CARRIED AND THAT RUN KILLED — do not re-buy either:
//   * "the guest is not registered with the clothing manager". He IS:
//     `players: [B925ABE0 B92744F0 B928DE00 B92A7710]` matched GetUserPlayer entry for
//     entry, all four. The engine preallocates four, so the mgrIdx < 0 branch below is
//     effectively unreachable and is kept only as an assertion.
//   * "the two-player budget halving starves the chest". `mgr+0x4374` read **1**, not 2,
//     so the SOLO column was selected and the chest kept its full 2006 KB. The halving
//     was not in play at all. The sweep still prints the live budget beside every missing
//     piece, because that is what made this answerable in one log rather than in a round
//     of experiments — and because a session where +0x4374 does read 2 may yet exist.
//
// ARMS
//   CZ_COOP_OUTFIT_CHECK=0    the whole sweep off (ON by default; one pass over two
//                             players x seven parts every 2 s)
//   CZ_COOP_OUTFIT_CHECK_MS=N the sweep period, default 2000
//   CZ_COOP_OUTFIT_REPAIR=0   THE CONTROL ARM: check and report, repair nothing
//   CZ_COOP_OUTFIT_VERBOSE=1  print the whole table every sweep, not just on a change
//   CZ_COOP_OUTFIT_CHECK_SOLO=1  the positive control: sweep with one dressed player
//   CZ_COOP_OUTFIT_DUMP=1     dump all 13 load records per player once, the way the
//                             layout above was measured
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <ppc_config.h>
#include <ppc_context.h>

#include "coop_objects.h"
#include "memory.h"

extern "C" PPC_FUNC(__imp__sub_82371978);
bool CoopOutfit_ReportedEmpty(uint32_t slot);

namespace
{
using coop::GuestCall;
using coop::LoadU32;

// ---------------------------------------------------------------- the title's own
constexpr uint32_t kGameStateOwner = 0x82A57428;  // +0x2C = the world; sub_82483230(mgr,1)
constexpr uint32_t kWorldUserPlayers = 0x7C;      // GetUserPlayer's array
constexpr uint32_t kWorldLocalPlayer = 0x80;      // the player index at THIS machine
constexpr uint32_t kWorldGame = 0x78;             // +0x2C clothing mgr, +0x70 event mgr
constexpr uint32_t kPlayerClothing = 0xCE74;      // *(player + 0xCE74) — A LOAD
constexpr uint32_t kFnGetUserPlayer = 0x82482AD8; // (world, idx) -> player, or 0
constexpr uint32_t kFnEventCtor = 0x8247CAA8;     // (evt, player, part, -1, name, flag)
constexpr uint32_t kFnEventPost = 0x82188488;     // (eventMgr, evt, file, line)
constexpr uint32_t kEventVTable = 0x8200AFD4;     // the change-part event's vtable
constexpr uint32_t kEventTypeId = 0x2F;
constexpr uint32_t kPostFile = 0x8207EC20;        // the co-op flow's own file/line, kept
constexpr uint32_t kPostLine = 0x85B;             // so a title-side log names the source

constexpr uint32_t kMgrPlayers = 0x428C;          // four player pointers
constexpr uint32_t kMgrPlayerCount = 0x4374;
constexpr uint32_t kMgrRecords = 0x10;            // 13 x 0x128 per player
constexpr uint32_t kRecordStride = 0x128;
constexpr uint32_t kRecordsPerPlayer = 13;
constexpr uint32_t kRecModel = 0x38;              // 16 handles, 0xFFFFFFFF = empty
constexpr uint32_t kRecTexture = 0x78;
constexpr uint32_t kRecFiles = 0xB8;
constexpr uint32_t kRecPart = 0xBC;               // the part it serves; 13 = idle
constexpr uint32_t kRecBudget = 0x11C;            // the streaming budget, in bytes
constexpr uint32_t kEmptyHandle = 0xFFFFFFFFu;

constexpr int kParts = 7;
constexpr int kMaxPlayers = 4;                    // what GetUserPlayer bounds-checks to
const char* const kPartName[kParts] = {"headwear", "head", "facewear", "chest",
                                       "hands",    "leg",  "feet"};

// The title's 0x24-byte string: length at +0x20, inline below 0x1F, else a heap pointer
// at +0. This is `sub_823D41D8` inlined — two instructions, not worth a guest call.
uint32_t SsoPtr(uint8_t* base, uint32_t s)
{
    return PPC_LOAD_U8(s + 0x20) >= 0x1F ? PPC_LOAD_U32(s) : s;
}

const char* SsoChars(uint8_t* base, uint32_t s)
{
    const uint32_t p = SsoPtr(base, s);
    return p ? reinterpret_cast<const char*>(base + p) : "";
}

uint32_t RecordName(uint32_t clothing, int part) { return clothing + part * 0x30 + 0x4AE8; }

// A piece name that means "wear nothing here". The requester agrees: an empty name never
// becomes a file, and "NONE" is the csv's own spelling of the same thing.
bool MeansNothing(const char* n) { return !n || !*n || std::strcmp(n, "NONE") == 0; }

bool Handle(uint32_t h) { return h != 0 && h != kEmptyHandle; }

// ---------------------------------------------------------------- the arms
bool EnvOn(const char* name, bool dflt)
{
    const char* e = std::getenv(name);
    if (!e)
        return dflt;
    return e[0] != '0';
}

bool CheckOn()   { static const bool v = EnvOn("CZ_COOP_OUTFIT_CHECK", true);   return v; }
bool RepairOn()  { static const bool v = EnvOn("CZ_COOP_OUTFIT_REPAIR", true);  return v; }
bool Verbose()   { static const bool v = EnvOn("CZ_COOP_OUTFIT_VERBOSE", false); return v; }
bool DumpOn()    { static const bool v = EnvOn("CZ_COOP_OUTFIT_DUMP", false);   return v; }
// The positive control: sweep with one dressed player. Repair stays off in that mode —
// a solo player has nobody to be out of sync with, so anything it found would be a
// misreading, which is the entire point of running it.
bool SoloControl() { static const bool v = EnvOn("CZ_COOP_OUTFIT_CHECK_SOLO", false); return v; }

// THE JOIN WINDOW (2026-10-07, the operator's instruction): "every 10 seconds for the
// next 3 minutes after a guest joins, check his outfit and apply it if it isn't".
int WindowS()
{
    static const int v = [] {
        const char* e = std::getenv("CZ_COOP_OUTFIT_WINDOW_S");
        return e ? std::atoi(e) : 180;
    }();
    return v;
}
int WindowEveryS()
{
    static const int v = [] {
        const char* e = std::getenv("CZ_COOP_OUTFIT_WINDOW_EVERY_S");
        const int x = e ? std::atoi(e) : 0;
        return x > 0 ? x : 10;
    }();
    return v;
}

// The default outfit's piece name for one part: row 17 (OUTFIT_DEFAULT_UNDER) of the
// outfit table, read where sub_821B5650 reads it — rowDef = *(mgr+0x429C) + row*0x11C
// + 0x88, piece name at rowDef + part*0x24 + 0x1C (the 0x24-byte string).
constexpr uint32_t kDefaultRow = 17;
uint32_t DefaultPieceName(uint8_t* base, uint32_t mgr, int part)
{
    const uint32_t table = PPC_LOAD_U32(mgr + 0x429C);
    if (!table)
        return 0;
    const uint32_t rowDef = table + kDefaultRow * 0x11C + 0x88;
    return SsoPtr(base, rowDef + uint32_t(part) * 0x24 + 0x1C);
}

int PeriodMs()
{
    static const int ms = [] {
        const char* e = std::getenv("CZ_COOP_OUTFIT_CHECK_MS");
        const int v = e ? std::atoi(e) : 0;
        return v > 0 ? v : 2000;
    }();
    return ms;
}

// ---------------------------------------------------------------- what the wire said
//
// The report is the only statement of what the OTHER machine believes he is wearing, and
// it arrives long before any of this can be checked, so it is kept. Keyed by the clothing
// object, which is what the receiver hands us.
struct Report
{
    uint32_t clothing = 0;
    bool have[kParts] = {};
    char name[kParts][0x21] = {};
};
Report g_report[kMaxPlayers];
std::mutex g_reportMu;   // the receiver's thread writes it, the frame hook reads it

Report* ReportFor(uint32_t clothing, bool create)
{
    for (auto& r : g_report)
        if (r.clothing == clothing)
            return &r;
    if (!create)
        return nullptr;
    for (auto& r : g_report)
        if (!r.clothing)
        {
            r = Report{};
            r.clothing = clothing;
            return &r;
        }
    g_report[0] = Report{};          // four have reported and co-op has two: reuse
    g_report[0].clothing = clothing;
    return &g_report[0];
}

// ---------------------------------------------------------------- per-player state
struct Watch
{
    uint32_t clothing = 0;
    uint8_t badStreak[kParts] = {};
    uint8_t attempts[kParts] = {};
    uint16_t lastMask = 0;   // badMask | wrongRecordMask<<8: re-announce when EITHER moves
    bool announced = false;
    bool refused = false;
    uint8_t nothingStreak = 0;   // sweeps in a row with not one named piece
    std::chrono::steady_clock::time_point windowStart{}, windowNext{};
    bool windowOpen = false, windowClosedSaid = false;
    unsigned windowPosts = 0;
};
Watch g_watch[kMaxPlayers];

// ---------------------------------------------------------------- the repair
//
// Rebuilt field for field from the co-op flow's own per-part post, 0x82582A60.. — the
// very event a join uses to dress the remote player. The scratch lives on the guest
// stack BELOW the frame we were called in, and the callee's stack pointer is pushed
// below that again, so nothing we write can end up under a callee's feet.
bool PostChangePart(PPCContext& ctx, uint8_t* base, uint32_t world, uint32_t player,
                    int part, uint32_t namePtr)
{
    const uint32_t game = LoadU32(base, world + kWorldGame);
    const uint32_t eventMgr = game ? LoadU32(base, game + 0x70) : 0;
    if (!eventMgr)
    {
        fprintf(stderr, "[outfit] cannot re-post the %s change: no event manager "
                        "(world %08X game %08X)\n", kPartName[part], world, game);
        return false;
    }

    const uint32_t evt = (ctx.r1.u32 - 0x200) & ~0xFu;   // 0x48 bytes are ours
    PPCContext call = ctx;
    call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;

    PPC_STORE_U32(evt + 0x00, kEventVTable);
    PPC_STORE_U32(evt + 0x04, 0);
    PPC_STORE_U32(evt + 0x08, kEventTypeId);
    PPC_STORE_U8(evt + 0x0C, 0);
    PPC_STORE_U32(evt + 0x10, 3);          // the constructor overwrites this with 0
    PPC_STORE_U32(evt + 0x14, 0);
    PPC_STORE_U32(evt + 0x18, 0xFFFFFFFFu);
    PPC_STORE_U32(evt + 0x1C, 0xFFFFFFFFu);
    PPC_STORE_U8(evt + 0x20, 0);
    PPC_STORE_U8(evt + 0x40, 0);

    call.r3.u64 = evt;
    call.r4.u64 = player;
    call.r5.u64 = uint32_t(part);
    call.r6.u64 = 0xFFFFFFFFu;
    call.r7.u64 = namePtr;
    call.r8.u64 = 1;                       // what the co-op flow passes here
    if (!GuestCall(call, base, kFnEventCtor, "outfit-event-ctor"))
        return false;

    call.r3.u64 = eventMgr;
    call.r4.u64 = evt;
    call.r5.u64 = kPostFile;
    call.r6.u64 = kPostLine;
    return GuestCall(call, base, kFnEventPost, "outfit-event-post");
}

// Put a name back on the record when the record itself lost it. `sub_82371978` is the
// receiver's own writer; the IMPL is called rather than the hooked symbol so this does
// not re-enter our own report hook and re-arm part 6's save-less dress.
void RewriteRecord(PPCContext& ctx, uint8_t* base, uint32_t clothing, int part,
                   const char* name)
{
    const uint32_t str = (ctx.r1.u32 - 0x180) & ~0xFu;
    const size_t n = std::strlen(name);
    for (size_t i = 0; i < 0x20; ++i)
        PPC_STORE_U8(str + uint32_t(i), uint8_t(i <= n ? name[i] : 0));
    PPCContext call = ctx;
    call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
    call.r3.u64 = clothing;
    call.r4.u64 = uint32_t(part);
    call.r5.u64 = str;
    __imp__sub_82371978(call, base);
}

// ---------------------------------------------------------------- the sweep
struct Row
{
    uint32_t player = 0, clothing = 0;
    int mgrIdx = -1;                  // this player's index in the clothing manager
    const char* recorded[kParts] = {};// what he is recorded as wearing
    const char* loaded[kParts] = {};  // what the load record says it holds
    uint32_t model[kParts] = {};
    uint32_t budget[kParts] = {};
    uint8_t badMask = 0;              // named, and the piece is not here
    uint8_t namedMask = 0;
    uint8_t wrongRecordMask = 0;      // the record holds a DIFFERENT piece's name
    bool dressed = false;
};

uint32_t RecordOf(uint32_t mgr, int mgrIdx, int part)
{
    return mgr + kMgrRecords +
           (uint32_t(mgrIdx) * kRecordsPerPlayer + uint32_t(part)) * kRecordStride;
}

bool ReadPlayer(PPCContext& ctx, uint8_t* base, uint32_t world, uint32_t mgr, int idx,
                Row& row)
{
    PPCContext call = ctx;
    call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
    call.r3.u64 = world;
    call.r4.u64 = uint32_t(idx);
    if (!GuestCall(call, base, kFnGetUserPlayer, "get-user-player"))
        return false;
    row.player = call.r3.u32;
    if (row.player < 0x10000 || row.player >= 0xF0000000u)
        return false;
    row.clothing = PPC_LOAD_U32(row.player + kPlayerClothing);
    if (row.clothing < 0x10000 || row.clothing >= 0xF0000000u)
        return false;

    // The load records are indexed by the CLOTHING MANAGER's own player index, which is
    // the one the change-part handler resolves the same way (0x82271EA4): find the
    // player pointer in the four-entry array at mgr+0x428C.
    for (int i = 0; i < kMaxPlayers; ++i)
        if (PPC_LOAD_U32(mgr + kMgrPlayers + uint32_t(i) * 4) == row.player)
        {
            row.mgrIdx = i;
            break;
        }

    for (int p = 0; p < kParts; ++p)
    {
        row.recorded[p] = SsoChars(base, RecordName(row.clothing, p));
        if (row.mgrIdx >= 0)
        {
            const uint32_t rec = RecordOf(mgr, row.mgrIdx, p);
            row.loaded[p] = SsoChars(base, rec);
            row.model[p] = PPC_LOAD_U32(rec + kRecModel);
            row.budget[p] = PPC_LOAD_U32(rec + kRecBudget);
        }
        else
        {
            row.loaded[p] = "";
        }
        if (MeansNothing(row.recorded[p]))
            continue;
        row.dressed = true;
        row.namedMask |= uint8_t(1u << p);
        if (row.mgrIdx < 0)
            continue;                       // cannot judge; reported separately
        if (!Handle(row.model[p]))
            row.badMask |= uint8_t(1u << p);
        else if (std::strcmp(row.loaded[p], row.recorded[p]) != 0)
            row.wrongRecordMask |= uint8_t(1u << p);
    }
    return true;
}

void PrintRow(int idx, bool local, const Row& row)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "[outfit] player %d%s clothing %08X slot %d:", idx,
                     local ? " (this machine)" : " (the other machine)", row.clothing,
                     row.mgrIdx);
    for (int p = 0; p < kParts && n > 0 && n < int(sizeof buf) - 1; ++p)
    {
        const char* want = row.recorded[p];
        const char* mark = (row.badMask & (1u << p))          ? " NOT LOADED"
                           : (row.wrongRecordMask & (1u << p)) ? " MISMATCHED"
                                                               : "";
        const int w = snprintf(buf + n, sizeof buf - size_t(n), " %s=%.32s%s",
                               kPartName[p], MeansNothing(want) ? "-" : want, mark);
        if (w < 0)
            break;
        n = (n + w < int(sizeof buf)) ? n + w : int(sizeof buf) - 1;
    }
    fprintf(stderr, "%s\n", buf);
}

// CZ_COOP_OUTFIT_DUMP=1: the clothing manager's per-player load records, one shot. This
// is how the layout above was measured and it is how the next surprise gets measured;
// part 2 (facewear) is empty on Chuck's default outfit, so the dump always carries its
// own negative control.
void DumpRecords(uint8_t* base, uint32_t mgr, int mgrIdx)
{
    for (int rec = 0; rec < int(kRecordsPerPlayer); ++rec)
    {
        const uint32_t r = RecordOf(mgr, mgrIdx, rec);
        char line[900];
        int n = snprintf(line, sizeof line, "[outfit] DUMP slot %d record %2d:", mgrIdx, rec);
        for (uint32_t off = 0; off < kRecordStride && n > 0 && n < int(sizeof line) - 24;
             off += 4)
        {
            const uint32_t v = PPC_LOAD_U32(r + off);
            if (!v)
                continue;
            const int w = snprintf(line + n, sizeof line - size_t(n), " +%03X=%08X", off, v);
            if (w < 0)
                break;
            n = (n + w < int(sizeof line)) ? n + w : int(sizeof line) - 1;
        }
        fprintf(stderr, "%s\n", line);
    }
}
} // namespace

// The receiver recorded one piece of a remote player's outfit report. Keep the name: it
// is the only statement of what the other machine believes he is wearing, and the record
// we later check can be wrong in exactly the way that matters.
void CoopOutfitVerify_OnReport(uint8_t* base, uint32_t clothing, uint32_t part,
                               uint32_t namePtr)
{
    if (!CheckOn() || part >= kParts || !clothing)
        return;
    const char* name = namePtr ? reinterpret_cast<const char*>(base + namePtr) : "";
    std::lock_guard<std::mutex> lk(g_reportMu);
    Report* r = ReportFor(clothing, true);
    if (!r)
        return;
    std::snprintf(r->name[part], sizeof r->name[part], "%s", name);
    r->have[part] = true;
}

// Per frame, from the game session's update (coop_host.cpp). Throttled to one sweep a
// period; every guest call it makes is on a copy of the context.
void CoopOutfitVerify_Tick(PPCContext& ctx, uint8_t* base)
{
    if (!CheckOn())
        return;

    static thread_local bool inSweep = false;
    if (inSweep)
        return;                       // a guest call here can re-enter this hook
    // try_lock, not lock: the sweep makes guest calls while it holds this, and parking a
    // GUEST thread inside an instrument manufactures the stability it reports (gotcha 7).
    // Skipping a sweep costs two seconds of resolution.
    static std::mutex sweepMu;
    static std::chrono::steady_clock::time_point next{};
    std::unique_lock<std::mutex> sweepLock(sweepMu, std::try_to_lock);
    if (!sweepLock.owns_lock())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now < next)
        return;
    next = now + std::chrono::milliseconds(PeriodMs());

    const uint32_t owner = PPC_LOAD_U32(kGameStateOwner);
    const uint32_t world = owner ? PPC_LOAD_U32(owner + 0x2C) : 0;
    if (!world || !PPC_LOAD_U32(world + kWorldUserPlayers))
        return;
    const uint32_t game = PPC_LOAD_U32(world + kWorldGame);
    const uint32_t mgr = game ? PPC_LOAD_U32(game + 0x2C) : 0;
    if (!mgr)
        return;

    inSweep = true;
    const uint32_t localIdx = PPC_LOAD_U32(world + kWorldLocalPlayer);
    Row rows[kMaxPlayers];
    bool present[kMaxPlayers] = {};
    int playerCount = 0;
    for (int i = 0; i < kMaxPlayers; ++i)
    {
        present[i] = ReadPlayer(ctx, base, world, mgr, i, rows[i]);
        if (present[i])
            playerCount++;
    }

    // COUNT PLAYERS, NOT DRESSED PLAYERS. The first cut required two DRESSED players,
    // and a player wearing nothing at all is precisely the "invisible" report this file
    // exists for — so the sweep returned in silence on the very session it was built to
    // read (2026-09-29, the first two-machine run). A gate written from the healthy case
    // excludes the defect.
    if (playerCount < (SoloControl() ? 1 : 2))
    {
        inSweep = false;
        return;                       // single player: nothing to check, nothing to say
    }

    // THE CONTROL. The player at this machine is visibly correct — someone is looking at
    // him. If he reads BAD, the reading is wrong, and acting on it would be acting on a
    // broken instrument.
    const bool controlBad = localIdx < kMaxPlayers && present[localIdx] &&
                            (rows[localIdx].badMask || rows[localIdx].wrongRecordMask);

    // Said once, so a log can show the check ran at all: a silent check and an absent
    // check read identically.
    static bool announcedOnce = false;
    if (!announcedOnce)
    {
        announcedOnce = true;
        fprintf(stderr, "[outfit] co-op clothing check is running (local player %u, %d "
                        "player%s, %u in the clothing manager, every %d ms; "
                        "CZ_COOP_OUTFIT_CHECK=0 is off, CZ_COOP_OUTFIT_REPAIR=0 is the "
                        "control)%s\n",
                localIdx, playerCount, playerCount == 1 ? "" : "s",
                PPC_LOAD_U32(mgr + kMgrPlayerCount), PeriodMs(),
                SoloControl() ? " [SOLO POSITIVE CONTROL: one player is enough and "
                                "nothing will be repaired]" : "");
        // The clothing manager's own four-entry player array, printed raw beside the
        // player objects GetUserPlayer hands out. Everything downstream — the load
        // records AND the row applier's per-part events — is indexed through it, so a
        // player missing from it is dressed into somebody else's Chuck.
        fprintf(stderr, "[outfit] clothing manager %08X players: [%08X %08X %08X %08X]; "
                        "GetUserPlayer: [%08X %08X %08X %08X]\n", mgr,
                PPC_LOAD_U32(mgr + kMgrPlayers), PPC_LOAD_U32(mgr + kMgrPlayers + 4),
                PPC_LOAD_U32(mgr + kMgrPlayers + 8), PPC_LOAD_U32(mgr + kMgrPlayers + 12),
                rows[0].player, rows[1].player, rows[2].player, rows[3].player);
        for (int i = 0; i < kMaxPlayers; ++i)
            if (present[i])
                PrintRow(i, uint32_t(i) == localIdx, rows[i]);
        if (DumpOn())
            for (int i = 0; i < kMaxPlayers; ++i)
                if (present[i] && rows[i].mgrIdx >= 0)
                    DumpRecords(base, mgr, rows[i].mgrIdx);
    }

    for (int i = 0; i < kMaxPlayers; ++i)
    {
        if (!present[i])
            continue;
        Watch& w = g_watch[i];
        if (w.clothing != rows[i].clothing)
        {
            w = Watch{};
            w.clothing = rows[i].clothing;
        }
        const bool local = uint32_t(i) == localIdx;
        const char* who = local ? " (this machine)" : " (the other machine)";
        const uint8_t mask = rows[i].badMask;

        if (Verbose())
            PrintRow(i, local, rows[i]);

        // A player the clothing manager does not know cannot have load records at all,
        // and that is a finding rather than a missing piece. Say it once per player.
        if (rows[i].mgrIdx < 0)
        {
            if (!w.refused)
            {
                w.refused = true;
                fprintf(stderr, "[outfit] player %d%s is dressed but is NOT registered "
                                "with the clothing manager (none of %08X+0x428C's four "
                                "entries is %08X) — this machine cannot tell whether his "
                                "pieces arrived\n", i, who, mgr, rows[i].player);
            }
            continue;
        }

        // THE JOIN WINDOW. The two-sweep, three-attempt repair below was built for a
        // piece that is NAMED and missing; a save-less guest is named NOTHING on the
        // host, because the host's own default dress goes through the row applier
        // sub_821B5650, whose first test is `player < mgr+0x4374` — and that count
        // reads 1 in co-op (2026-10-07 log: "1 in the clothing manager"), so dressing
        // slot 1 returned before posting a single piece. The window posts each piece to
        // HIS Chuck directly, with the co-op flow's own change-part event: the default
        // outfit's pieces when he wears nothing (what his own machine dresses him in),
        // the missing pieces when he wears some.
        // DR2 co-op is two players: the window watches the OTHER one, not the two
        // pre-allocated empty slots the first run also watched (and waited on for ever).
        const uint32_t otherIdx = localIdx == 0 ? 1u : 0u;
        if (!local && uint32_t(i) == otherIdx && RepairOn() && !SoloControl() && WindowS() > 0)
        {
            if (!w.windowOpen && !w.windowClosedSaid)
            {
                w.windowOpen = true;
                w.windowStart = now;
                w.windowNext = now;
                fprintf(stderr, "[outfit] player %d%s joined: checking his outfit every %d s "
                                "for %d s and applying what is missing "
                                "(CZ_COOP_OUTFIT_WINDOW_S=0 is off)\n",
                        i, who, WindowEveryS(), WindowS());
            }
            if (w.windowOpen && now - w.windowStart > std::chrono::seconds(WindowS()))
            {
                w.windowOpen = false;
                w.windowClosedSaid = true;
                fprintf(stderr, "[outfit] player %d%s: the %d s join window closed after %u "
                                "post(s); he %s\n",
                        i, who, WindowS(), w.windowPosts,
                        !rows[i].namedMask ? "STILL WEARS NOTHING here"
                        : mask             ? "is still missing pieces here"
                                           : "is fully dressed here");
            }
            if (w.windowOpen && now >= w.windowNext && !controlBad)
            {
                w.windowNext = now + std::chrono::seconds(WindowEveryS());
                const int secs = int(std::chrono::duration_cast<std::chrono::seconds>(
                                         now - w.windowStart).count());
                // The default outfit ONLY for a player whose own report said he wears
                // nothing (all seven names empty: no save on his side). A dressed
                // player's report can land after this sweep, and the default must never
                // overwrite a real outfit.
                bool reportedEmpty = CoopOutfit_ReportedEmpty(uint32_t(i));
                {
                    std::lock_guard<std::mutex> lk(g_reportMu);
                    const Report* rep = ReportFor(rows[i].clothing, false);
                    if (rep && !reportedEmpty)
                    {
                        reportedEmpty = true;
                        for (int p = 0; p < kParts; ++p)
                            if (!rep->have[p] || !MeansNothing(rep->name[p]))
                                reportedEmpty = false;
                    }
                }
                if (!rows[i].namedMask && !reportedEmpty)
                {
                    fprintf(stderr, "[outfit] +%3d s: player %d%s wears nothing here yet, and "
                                    "his outfit report has not said he has none — waiting\n",
                            secs, i, who);
                }
                else if (!rows[i].namedMask)
                {
                    int posted = 0;
                    for (int p = 0; p < kParts; ++p)
                    {
                        const uint32_t namePtr = DefaultPieceName(base, mgr, p);
                        if (!namePtr ||
                            MeansNothing(reinterpret_cast<const char*>(base + namePtr)))
                            continue;
                        if (PostChangePart(ctx, base, world, rows[i].player, p, namePtr))
                            posted++;
                    }
                    w.windowPosts += unsigned(posted);
                    fprintf(stderr, "[outfit] +%3d s: player %d%s wears NOTHING here — posted "
                                    "%d piece(s) of the default outfit (row %u) to his Chuck\n",
                            secs, i, who, posted, kDefaultRow);
                }
                else if (mask)
                {
                    int posted = 0;
                    for (int p = 0; p < kParts; ++p)
                    {
                        if (!(mask & (1u << p)))
                            continue;
                        const uint32_t namePtr = SsoPtr(base, RecordName(rows[i].clothing, p));
                        if (namePtr && PostChangePart(ctx, base, world, rows[i].player, p, namePtr))
                            posted++;
                    }
                    w.windowPosts += unsigned(posted);
                    fprintf(stderr, "[outfit] +%3d s: player %d%s is missing %d piece(s) here — "
                                    "re-posted %d\n",
                            secs, i, who, __builtin_popcount(mask), posted);
                    PrintRow(i, local, rows[i]);
                }
                continue;   // the window acted (or found him dressed) this sweep
            }
        }

        const uint16_t both = uint16_t(mask | (rows[i].wrongRecordMask << 8));
        if (both != w.lastMask)
        {
            w.lastMask = both;
            w.announced = false;
        }
        if (!rows[i].namedMask)
        {
            // WAIT TWO SWEEPS, for the same reason the missing case does. During the
            // level load NOBODY is dressed yet, including the player at this machine,
            // and the first two-machine run printed "PLAYER 0 (this machine) IS WEARING
            // NOTHING" about a host who was visibly fine four seconds later. A line that
            // cries wolf on every join teaches the operator to skip the line that matters.
            if (w.nothingStreak < 255)
                w.nothingStreak++;
            if (w.nothingStreak < 2)
                continue;
            if (!w.announced)
            {
                w.announced = true;
                fprintf(stderr, "[outfit] PLAYER %d%s IS WEARING NOTHING — all seven of "
                                "his piece names are empty on this machine, so he renders "
                                "INVISIBLE here. Nothing dressed him: either his outfit "
                                "report was empty and the default-outfit dress landed on "
                                "another player, or it never arrived.\n", i, who);
                PrintRow(i, local, rows[i]);
            }
            continue;
        }
        w.nothingStreak = 0;
        if (!mask)
        {
            std::memset(w.badStreak, 0, sizeof w.badStreak);
            if (!w.announced)
            {
                w.announced = true;
                fprintf(stderr, "[outfit] player %d%s has all %d of the pieces he is "
                                "wearing\n", i, who, __builtin_popcount(rows[i].namedMask));
                // Not "missing", but not right either: he is recorded as wearing one
                // garment and the slot that loaded holds another, so he is dressed in
                // the wrong thing on this machine. Reported, never repaired — the report
                // this part was written for is an ABSENT piece, and a repair for a
                // symptom nobody has seen would be a guess.
                for (int p = 0; p < kParts; ++p)
                    if (rows[i].wrongRecordMask & (1u << p))
                        fprintf(stderr, "[outfit]     ...but his %s is recorded as '%s' "
                                        "while the piece that loaded is '%s'\n",
                                kPartName[p], rows[i].recorded[p], rows[i].loaded[p]);
            }
            continue;
        }

        // Count the streaks BEFORE saying anything. A piece is legitimately absent for
        // the second or two it is streaming, and a line that fires on every normal join
        // teaches the operator to ignore the line that matters. Two sweeps (~4 s) is
        // past any load this trace has ever recorded; the repair still waits for three.
        uint8_t worst = 0;
        for (int p = 0; p < kParts; ++p)
        {
            if (!(mask & (1u << p)))
            {
                w.badStreak[p] = 0;
                continue;
            }
            if (w.badStreak[p] < 255)
                w.badStreak[p]++;
            if (w.badStreak[p] > worst)
                worst = w.badStreak[p];
        }
        if (worst < 2)
            continue;

        if (!w.announced)
        {
            w.announced = true;
            fprintf(stderr, "[outfit] PLAYER %d%s IS MISSING CLOTHING — he is recorded as "
                            "wearing pieces this machine never loaded, so he renders %s "
                            "here:\n", i, who,
                    mask == rows[i].namedMask ? "INVISIBLE" : "with holes");
            PrintRow(i, local, rows[i]);
            const uint32_t players = PPC_LOAD_U32(mgr + kMgrPlayerCount);
            for (int p = 0; p < kParts; ++p)
                if (mask & (1u << p))
                    fprintf(stderr, "[outfit]     %s '%s' has no model — its streaming "
                                    "budget in this session is %u KB (%u player%s in the "
                                    "clothing manager; the solo budget for this part is "
                                    "%u KB)\n",
                            kPartName[p], rows[i].recorded[p], rows[i].budget[p] / 1024,
                            players, players == 1 ? "" : "s",
                            PPC_LOAD_U32(0x829D42F0 + uint32_t(p) * 8));
        }

        for (int p = 0; p < kParts; ++p)
        {
            if (!(mask & (1u << p)))
                continue;
            if (w.badStreak[p] < 3 || !RepairOn() || SoloControl() || w.attempts[p] >= 3)
                continue;
            if (controlBad)
            {
                if (!w.refused)
                {
                    w.refused = true;
                    fprintf(stderr, "[outfit] NOT repairing: the player at THIS machine "
                                    "reads as missing clothing too, and he is visibly "
                                    "correct — so this check is misreading the game, not "
                                    "finding a defect\n");
                }
                continue;
            }
            w.attempts[p]++;

            // The record may itself have lost the name the wire delivered; put it back
            // before asking for the piece, or the request is a request for nothing.
            uint32_t namePtr = SsoPtr(base, RecordName(rows[i].clothing, p));
            char reported[0x21] = {};
            {
                std::lock_guard<std::mutex> lk(g_reportMu);
                const Report* rep = ReportFor(rows[i].clothing, false);
                if (rep && rep->have[p])
                    std::snprintf(reported, sizeof reported, "%s", rep->name[p]);
            }
            if (MeansNothing(rows[i].recorded[p]) && !MeansNothing(reported))
            {
                fprintf(stderr, "[outfit]     the record for %s is empty but the report "
                                "said '%s' — writing it back\n", kPartName[p], reported);
                RewriteRecord(ctx, base, rows[i].clothing, p, reported);
                namePtr = SsoPtr(base, RecordName(rows[i].clothing, p));
            }
            if (!namePtr)
                continue;

            fprintf(stderr, "[outfit]     asking for player %d's %s ('%s') again — attempt "
                            "%u of 3 (CZ_COOP_OUTFIT_REPAIR=0 is the control)\n",
                    i, kPartName[p], reinterpret_cast<const char*>(base + namePtr),
                    w.attempts[p]);
            if (!PostChangePart(ctx, base, world, rows[i].player, p, namePtr))
                w.attempts[p] = 3;     // the post itself refused; do not spin on it
            else if (w.attempts[p] == 3)
                fprintf(stderr, "[outfit]     that was the last attempt for %s. If it "
                                "stays missing the piece is not being dropped, it is "
                                "failing to LOAD — compare its budget above against the "
                                "solo one, and run CZ_OUTFIT_TRACE=1 for the request and "
                                "the file\n", kPartName[p]);
        }
    }
    inSweep = false;
}
