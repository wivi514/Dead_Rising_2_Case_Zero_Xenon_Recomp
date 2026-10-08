#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ppc_recomp_shared.h"

// FAR SOLDIERS WEAR THE HELMET WHATEVER THEIR VARIANT SAYS (open item 0zf, 2026-10-08).
//
// A zombie's look is a per-zombie 64-bit HIDE mask (actor+0x40, set from spawn data by
// sub_82437600 at 0x82437978) tested against a per-mesh table built for each zombie type
// and LOD: a mesh is skipped when `entry & mask != 0` (sub_827ADD40). Each table entry is
// built by sub_827ACF00 from the MESH NAME through the wildcard classifier sub_827A2E10
// (the same classifier the survivor body-group table at 0x829D9290 goes through), so the
// variant a mesh belongs to comes from its name: `head2_...` -> 0x00020000,
// `headwear2...` -> 0x04000000, `chestupper_neck2...` -> 0x00020000.
//
// The far soldier model (zombie_soldier_3.big) has two meshes exported with the digit
// AFTER "Shape" — `headwearShape2Deformed` and `chestupper_neckShape2Deformed` — where every
// other mesh in every zombie model (12 archives, LOD2/3/4, censused) has it before. Their
// entries come out without the variant bit (helmet 0x101 where the close model's is
// 0x04000101), so the far helmet is never hidden: every far soldier wears it, and a soldier
// whose variant is not head 3 loses it at the close-model swap. Xenia shows the same (the
// operator, 2026-10-08) — it is the shipped data, not the recompilation. The operator's
// call was to make the far LOD follow the variant.
//
// THE FIX classifies the name the artist meant: for `<part>Shape<digit>Deformed...` the
// classifier is handed `<part><digit>ShapeDeformed...` — the same length, rewritten IN PLACE
// for the duration of the call and restored after, so the mesh keeps its real name for
// every other lookup. The pattern requires "Deformed" after the digit, which every LOD3
// zombie mesh carries and no world `geo_*Shape1` name does.
//
// Prediction: the far soldier helmet entry (BB19CD38 in a fresh boot) reads 0x04000101 and
// the head-2 neck 0x00020004, exactly the close model's bits; far soldiers whose variant is
// not head 3 are bare-headed or balaclava'd at every distance. CZ_ZOMBIE_NAME_FIX=0 is the
// control (the shipped classification).

extern "C" PPC_FUNC(__imp__sub_827A2E10);

namespace
{
bool Enabled()
{
    static const bool on = [] {
        const char* e = std::getenv("CZ_ZOMBIE_NAME_FIX");
        return !(e && std::strcmp(e, "0") == 0);
    }();
    return on;
}

// Returns the index of the digit to move (the char right after "Shape"), or -1.
int MisplacedDigit(const char* s, size_t n)
{
    static const char kShape[] = "Shape";
    for (size_t i = 1; i + 5 + 1 + 8 <= n; ++i)
    {
        if (std::memcmp(s + i, kShape, 5) != 0)
            continue;
        const size_t d = i + 5;
        if (!std::isdigit(static_cast<unsigned char>(s[d])))
            return -1;
        if (std::memcmp(s + d + 1, "Deformed", 8) != 0)
            return -1;
        return int(d);
    }
    return -1;
}
}   // namespace

PPC_FUNC(sub_827A2E10)
{
    const uint32_t nameVa = ctx.r5.u32;
    if (!Enabled() || nameVa == 0)
    {
        __imp__sub_827A2E10(ctx, base);
        return;
    }
    char* name = reinterpret_cast<char*>(base + nameVa);
    const size_t n = strnlen(name, 64);
    const int d = MisplacedDigit(name, n);
    if (d < 0)
    {
        __imp__sub_827A2E10(ctx, base);
        return;
    }
    // "<part>Shape<digit>" -> "<part><digit>Shape": shift "Shape" right by one, put the
    // digit where the 'S' was. Same length; restored after the call.
    char saved[64];
    std::memcpy(saved, name, n + 1);
    const char digit = name[d];
    std::memmove(name + d - 4, name + d - 5, 5);
    name[d - 5] = digit;
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1, std::memory_order_relaxed) < 8)
        std::fprintf(stderr, "[zvariant] classifying '%s' as '%s' (misplaced variant digit; "
                             "CZ_ZOMBIE_NAME_FIX=0 restores the shipped classification)\n",
                     saved, name);
    __imp__sub_827A2E10(ctx, base);
    std::memcpy(name, saved, n + 1);
}
