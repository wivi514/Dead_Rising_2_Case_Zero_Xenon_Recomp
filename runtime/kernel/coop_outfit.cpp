// CZ_OUTFIT_TRACE=1: every step of the title's clothing pipeline, printed.
// Co-op plan part 4, item 1 (docs/part113-kickoff.md §2.1).
//
// WHY THIS EXISTS
// ---------------
// The joining Chuck arrived in Still Creek with no chest piece — head, hands,
// jeans and boots placed, torso and arms absent, on BOTH machines — and the
// first guess (the OUTFIT_COOP_DEFAULT row of outfits.csv names a DR2 jacket
// Case Zero does not ship) was patched and measured a null. The seven
// tEventOutfit messages the client sends decode, by their sizes alone, to the
// client's SAVE outfit (OUTFIT_DEFAULT_UNDER: young_chuck ×4, an empty
// facewear, naked hands, young_chuck_under feet), so the chest the client asks
// for is one the host's own Chuck wears. Something between "asked for" and
// "attached" drops it, and the pipeline has four distinct steps that all look
// alike from outside. This prints each of them:
//
//   sub_821B5880(mgr, player, outfitIdx, ...)      dress a player in a whole
//                                                  outfits.csv row (index into
//                                                  the 51-entry db at +0x88,
//                                                  0x11C bytes each; the name
//                                                  table is 0x829D44F8)
//   sub_821B58D8(mgr, player, outfitIdx, part,..)  one part of a row
//   sub_821B5650(mgr, player, ...)                 the row applier: posts one
//                                                  change-part event per
//                                                  non-NONE piece
//   sub_8238C6F8(clothing, part, name, tag)        the NORMAL set-piece: records
//                                                  the name, its hash, and the
//                                                  clothingdatabase.csv row
//                                                  (+0x4B10, via sub_821FE838)
//   sub_82371978(clothing, part, name)             the CO-OP set-piece: records
//                                                  the name and hash ONLY. Its
//                                                  two callers are both in the
//                                                  clothing-report receiver
//                                                  (sub_82570E78)
//   sub_82371B88(clothing, part, name, model, x)   a loaded model attached to
//                                                  a part slot (+0x49A4 +
//                                                  part*0x2C)
//   sub_8254BB70()                                 co-op level entry: player 0
//                                                  -> row 0 (OUTFIT_DEFAULT),
//                                                  player 1 -> row 16
//                                                  (OUTFIT_COOP_DEFAULT_UNDER,
//                                                  a name Case Zero's csv has
//                                                  no row for: seven NONEs)
//   sub_82167318(entry, player)                    "is the player wearing this
//                                                  row" — the receiver's test
//                                                  before it re-dresses
//
// Per-part records live at clothing + part*0x30: +0x4AE4 tag, +0x4AE8 name
// (a 0x24-byte SSO string, length at +0x20, heap pointer at +0 when >= 0x1F),
// +0x4B0C hash, +0x4B10 clothing-db row. The clothing object is the player
// object + 0xCE74 (the receiver's `lwzx r3, r27, 0xCE74`).
//
// Every print is gated on the variable and the hooks pass straight through
// otherwise; a hook on a per-part path is not on the frame path.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ppc_config.h>
#include <ppc_context.h>

#include "memory.h"

extern "C" PPC_FUNC(__imp__sub_821B5880);
extern "C" PPC_FUNC(__imp__sub_821B58D8);
extern "C" PPC_FUNC(__imp__sub_821B5650);
extern "C" PPC_FUNC(__imp__sub_8238C6F8);
extern "C" PPC_FUNC(__imp__sub_82371978);
extern "C" PPC_FUNC(__imp__sub_82371B88);
extern "C" PPC_FUNC(__imp__sub_8254BB70);
extern "C" PPC_FUNC(__imp__sub_82167318);
extern "C" PPC_FUNC(__imp__sub_82165710);
extern "C" PPC_FUNC(__imp__sub_827B87C0);
extern "C" PPC_FUNC(__imp__sub_827C9708);
extern "C" PPC_FUNC(__imp__sub_821B4A38);
extern "C" PPC_FUNC(__imp__sub_82165C68);
extern "C" PPC_FUNC(__imp__sub_82165D00);
extern "C" PPC_FUNC(__imp__sub_821B4F60);

namespace
{
bool Enabled()
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_OUTFIT_TRACE");
        return e && e[0] == '1';
    }();
    return on;
}

constexpr uint32_t kOutfitNames = 0x829D44F8;   // 51 pointers
constexpr uint32_t kLastOutfitIdx = 0x82A073E4; // what sub_821B5650 applies

const char* GuestStr(uint8_t* base, uint32_t p)
{
    return p ? reinterpret_cast<const char*>(base + p) : "(null)";
}

// The title's 0x24-byte string: length byte at +0x20, inline below 0x1F,
// otherwise a heap pointer at +0.
const char* SsoStr(uint8_t* base, uint32_t s)
{
    if (!s)
        return "(null)";
    const uint8_t len = PPC_LOAD_U8(s + 0x20);
    if (len >= 0x1F)
        return GuestStr(base, PPC_LOAD_U32(s));
    return reinterpret_cast<const char*>(base + s);
}

const char* OutfitName(uint8_t* base, int idx)
{
    if (idx < 0 || idx >= 51)
        return "?";
    return GuestStr(base, PPC_LOAD_U32(kOutfitNames + uint32_t(idx) * 4));
}

void PrintRecord(uint8_t* base, uint32_t clothing, uint32_t part)
{
    const uint32_t r = clothing + part * 0x30;
    const uint32_t slot = clothing + part * 0x2C;
    fprintf(stderr, "[outfit]     record part %u: tag %d name '%s' hash %08X dbrow %d | "
                    "attached model %08X '%s'\n",
            part, int32_t(PPC_LOAD_U32(r + 0x4AE4)), SsoStr(base, r + 0x4AE8),
            PPC_LOAD_U32(r + 0x4B0C), int32_t(PPC_LOAD_U32(r + 0x4B10)),
            PPC_LOAD_U32(slot + 0x49A4), SsoStr(base, slot + 0x49AC));
}
} // namespace

PPC_FUNC(sub_821B5880)
{
    if (Enabled())
        fprintf(stderr, "[outfit] SetOutfit player %u -> row %d (%s) r6=%u r7=%u lr %08X\n",
                ctx.r4.u32, int32_t(ctx.r5.u32), OutfitName(base, int32_t(ctx.r5.u32)),
                ctx.r6.u32 & 0xFF, ctx.r7.u32 & 0xFF, uint32_t(ctx.lr));
    __imp__sub_821B5880(ctx, base);
}

PPC_FUNC(sub_821B58D8)
{
    if (Enabled())
        fprintf(stderr, "[outfit] SetOutfitPart player %u row %d (%s) part %d lr %08X\n",
                ctx.r4.u32, int32_t(ctx.r5.u32), OutfitName(base, int32_t(ctx.r5.u32)),
                int32_t(ctx.r6.u32), uint32_t(ctx.lr));
    __imp__sub_821B58D8(ctx, base);
}

PPC_FUNC(sub_821B5650)
{
    if (Enabled())
    {
        const int idx = int32_t(PPC_LOAD_U32(kLastOutfitIdx));
        fprintf(stderr, "[outfit] ApplyRow player %u row %d (%s) r5=%u lr %08X\n",
                ctx.r4.u32, idx, OutfitName(base, idx), ctx.r5.u32 & 0xFF, uint32_t(ctx.lr));
    }
    __imp__sub_821B5650(ctx, base);
}

PPC_FUNC(sub_8238C6F8)
{
    const uint32_t clothing = ctx.r3.u32, part = ctx.r4.u32;
    if (Enabled())
        fprintf(stderr, "[outfit] SetPart clothing %08X part %u '%s' tag %d lr %08X\n",
                clothing, part, GuestStr(base, ctx.r5.u32), int32_t(ctx.r6.u32),
                uint32_t(ctx.lr));
    __imp__sub_8238C6F8(ctx, base);
    if (Enabled() && part < 7)
        PrintRecord(base, clothing, part);
}

PPC_FUNC(sub_82371978)
{
    const uint32_t clothing = ctx.r3.u32, part = ctx.r4.u32;
    if (Enabled())
        fprintf(stderr, "[outfit] CoopSetPart clothing %08X part %u '%s' lr %08X\n",
                clothing, part, GuestStr(base, ctx.r5.u32), uint32_t(ctx.lr));
    __imp__sub_82371978(ctx, base);
    if (Enabled() && part < 7)
        PrintRecord(base, clothing, part);
}

PPC_FUNC(sub_82371B88)
{
    if (Enabled())
        fprintf(stderr, "[outfit] Attach clothing %08X part %u '%s' model %08X x %08X lr %08X\n",
                ctx.r3.u32, ctx.r4.u32, SsoStr(base, ctx.r5.u32), ctx.r6.u32, ctx.r7.u32,
                uint32_t(ctx.lr));
    __imp__sub_82371B88(ctx, base);
}

PPC_FUNC(sub_8254BB70)
{
    if (Enabled())
        fprintf(stderr, "[outfit] DressBoth: player 0 -> row 0, player 1 -> row 16 (%s) lr %08X\n",
                OutfitName(base, 16), uint32_t(ctx.lr));
    __imp__sub_8254BB70(ctx, base);
}

PPC_FUNC(sub_82167318)
{
    const uint32_t entry = ctx.r3.u32, player = ctx.r4.u32;
    __imp__sub_82167318(ctx, base);
    // Only the receiver's call (0x82570FC8) is interesting; sub_821B4D80 asks
    // it for every row of the db in a loop, with a different second argument.
    if (Enabled() && uint32_t(ctx.lr) == 0x82570FCC)
    {
        fprintf(stderr, "[outfit] IsWearing entry %08X player %08X -> %u lr %08X\n",
                entry, player, ctx.r3.u32 & 0xFF, uint32_t(ctx.lr));
        for (uint32_t i = 0; i < 7 && entry; ++i)
            fprintf(stderr, "[outfit]     entry piece %u: hash %08X '%s'\n", i,
                    PPC_LOAD_U32(entry + i * 4), SsoStr(base, entry + 0x1C + i * 0x24));
        for (uint32_t i = 0; i < 7 && player; ++i)
            PrintRecord(base, player + 0xCE74, i);
    }
}

// ---- the load side: from a part name to a model and a texture in memory ----
//
// The clothing manager keeps, per player, 13 load records of 0x128 bytes (from
// mgr+0x10); +0xB8 counts completed files, +0xBC the expected count (13 = idle),
// +0x110 the record's buffer size, +0x11C its buffer. sub_82165C68 sizes a
// record's buffer from a KB table at 0x829D42F0 that has TWO columns — solo
// and "more than one player" (mgr+0x4374 > 1), the second exactly half the
// first (chest 2006 -> 1003 KB) — and the texture-create path sets a byte
// (g_82AC4878+0xB54) for the two-player case before creating. That halving is
// the first suspect for a piece that loads for player 0 and not for player 1.

// (mgr, name, part, outBuf) -> builds "<part prefix><name>"; returns length
PPC_FUNC(sub_82165710)
{
    const uint32_t out = ctx.r6.u32;
    __imp__sub_82165710(ctx, base);
    if (Enabled())
        fprintf(stderr, "[outfit] PieceFile part %u -> '%s' lr %08X\n", ctx.r5.u32,
                GuestStr(base, out), uint32_t(ctx.lr));
}

// (texMgr, name, buf, 3, size|flags, 0) -> texture, 0 on failure
PPC_FUNC(sub_827B87C0)
{
    const uint32_t name = ctx.r4.u32, size = ctx.r7.u32;
    const bool trace = Enabled() && uint32_t(ctx.lr) == 0x8222722C;
    __imp__sub_827B87C0(ctx, base);
    if (trace)
        fprintf(stderr, "[outfit] TexCreate '%s' size %u (%08X) -> %08X\n",
                GuestStr(base, name), size & 0x0FFFFFFF, size, ctx.r3.u32);
}

// (mgr, name, buf, 3, size) -> model, 0 on failure
PPC_FUNC(sub_827C9708)
{
    const uint32_t name = ctx.r4.u32, size = ctx.r7.u32;
    const bool trace = Enabled() && uint32_t(ctx.lr) == 0x822272EC;
    __imp__sub_827C9708(ctx, base);
    if (trace)
        fprintf(stderr, "[outfit] ModelCreate '%s' size %u -> %08X\n",
                GuestStr(base, name), size, ctx.r3.u32);
}

// (mgr, player, part, slot, h1, h2, expected): one file of a piece landed
PPC_FUNC(sub_821B4A38)
{
    const uint32_t mgr = ctx.r3.u32, player = ctx.r4.u32, part = ctx.r5.u32,
                   slot = ctx.r6.u32, expected = ctx.r9.u32;
    const uint32_t rec = mgr + 0x10 + (player * 13 + slot) * 0x128;
    __imp__sub_821B4A38(ctx, base);
    if (Enabled())
        fprintf(stderr, "[outfit] LoadDone player %u part %u slot %u: %u of %u files "
                        "(record buf %u bytes)\n", player, part, slot,
                PPC_LOAD_U32(rec + 0xB8), expected, PPC_LOAD_U32(rec + 0x110));
}

PPC_FUNC(sub_82165C68)
{
    const uint32_t mgr = ctx.r3.u32, player = ctx.r4.u32, rec = ctx.r5.u32;
    __imp__sub_82165C68(ctx, base);
    if (Enabled())
        fprintf(stderr, "[outfit] RecordSize player %u record %u (players %u) -> %u bytes\n",
                player, (rec - mgr - 0x10) / 0x128 - player * 13, PPC_LOAD_U32(mgr + 0x4374),
                ctx.r3.u32);
}

PPC_FUNC(sub_82165D00)
{
    const uint32_t mgr = ctx.r3.u32, player = ctx.r4.u32, rec = ctx.r5.u32;
    __imp__sub_82165D00(ctx, base);
    if (Enabled())
        fprintf(stderr, "[outfit] RecordSize2 player %u record %u -> %u bytes\n",
                player, (rec - mgr - 0x10) / 0x128 - player * 13, ctx.r3.u32);
}

PPC_FUNC(sub_821B4F60)
{
    if (Enabled())
        fprintf(stderr, "[outfit] AllocRecords player %u lr %08X\n", ctx.r4.u32, uint32_t(ctx.lr));
    __imp__sub_821B4F60(ctx, base);
}
