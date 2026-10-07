#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ppc_recomp_shared.h"

// CZ_ZOMBIE_PARTS_TRACE=1 — the close-range zombie part census (2026-10-07).
//
// WHY THIS EXISTS: the operator reported every zombie changing model when it comes
// close, and the soldier losing his helmet — Xenia keeps it. An F9 census proved the
// helmet is never DRAWN at close range (no draw binds zs_Helmet_* with the close
// model's shader), so the decision is on the CPU, in the title, not in the renderer.
//
// sub_827A3CF0(model, table, 8) is where the close model's sub-meshes are sorted into
// the eight body groups of the table at 0x829D9290 (1 headwear "geo_hat*,...",
// 2 head, 4 torso, 8 vest/apron, 0x10/0x20 arms, 0x40 legs, 0x80 "*"): for each
// sub-mesh i it writes the group bits into the low bits of the word at
// *(model+0x204) + i*0x30, PRESERVING bit 31 (rlwimi r3,r11,0,0,0 at 0x827A3D54).
// This prints, once per model, every sub-mesh's name and that whole 48-byte record,
// plus the record array's address so a live reader (process_vm_readv) can watch the
// same words change afterwards. Read-only; off unless the variable is set.

extern "C" PPC_FUNC(__imp__sub_827A3CF0);

namespace
{
uint32_t Ld32(uint8_t* base, uint32_t va)
{
    uint32_t v;
    memcpy(&v, base + va, 4);
    return __builtin_bswap32(v);
}
}   // namespace

PPC_FUNC(sub_827A3CF0)
{
    static const bool on = getenv("CZ_ZOMBIE_PARTS_TRACE") != nullptr;
    const uint32_t model = ctx.r3.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_827A3CF0(ctx, base);
    if (!on || model == 0)
        return;

    static std::mutex mu;
    static unsigned printed = 0;
    std::lock_guard<std::mutex> lock(mu);
    if (printed >= 2000)
        return;
    ++printed;
    const uint32_t count = Ld32(base, model + 0x24);
    const uint32_t meshes = Ld32(base, model + 0x2C);
    const uint32_t recs = Ld32(base, model + 0x204);
    fprintf(stderr, "[zparts] model %08X lr=%08X meshes=%u recs@%08X\n", model, lr, count, recs);
    for (uint32_t i = 0; i < count && i < 64; ++i)
    {
        const uint32_t e = Ld32(base, meshes + i * 4);
        uint32_t nameVa = e + 4;
        if (base[e + 0x24] >= 0x1F)
            nameVa = Ld32(base, e + 4);
        char name[64] = {};
        for (int k = 0; k < 63; ++k)
        {
            const char c = char(base[nameVa + k]);
            if (c == 0)
                break;
            name[k] = (c >= 32 && c < 127) ? c : '?';
        }
        fprintf(stderr, "[zparts]   %2u %-32s", i, name);
        for (uint32_t w = 0; w < 12; ++w)
            fprintf(stderr, " %08X", Ld32(base, recs + i * 0x30 + w * 4));
        fprintf(stderr, "\n");
    }
}
