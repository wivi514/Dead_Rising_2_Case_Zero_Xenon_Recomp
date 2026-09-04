#include "shadow_distance.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ppc_recomp_shared.h"

// PARKED (part 93). This is a NON-WORKING experiment kept for the next session to
// resume from — see docs/shadow-distance-investigation.md. Two hook targets were tried
// (the interpolator sub_823C1CC8, never called on this static-lighting title; the
// builder sub_825A89A8, which fires but scaling its globals did NOT move the shadows,
// operator-confirmed). Gated on CZ_SHADOW_DIST only (no menu, no setting) so it ships
// off and bit-identical; factor 1.0 = the stock renderer.

extern "C" PPC_FUNC(__imp__sub_825A89A8);

namespace
{
// The SEVEN active cascade-distance globals sub_823C1CC8 writes (traced in part 93,
// evidence in the function's disassembly at 0x823C1F18-0x823C1F98). These are the
// interpolated Start->End result — the ACTIVE distances the cascade builder
// sub_825A89A8 re-reads every render, NOT the object field the first version wrote.
// Order matches the 7-float cascade array [5,15,35,40,0,0,50] at Still Creek: four
// near splits, two unused (0), one far/outer range.
const uint32_t kCascadeGlobals[7] = {
    0x829DFBFC, 0x829DFC00, 0x829DFC04, 0x829DFC08,   // from Start_CascadeDist[0..3]
    0x829DEB7C, 0x829DEB80, 0x829DEB88,               // from Start_CascadeDist[4..6]
};

bool Trace()
{
    static const bool t = getenv("CZ_SHADOW_DIST_TRACE") != nullptr;
    return t;
}
}   // namespace

float ShadowDistFactor()
{
    // Env only while parked (no persisted setting until it works). 1.0 = stock.
    if (const char* e = getenv("CZ_SHADOW_DIST"))
    {
        const float f = float(atof(e));
        if (f >= 0.25f && f <= 8.0f)
            return f;
    }
    return 1.0f;
}

float ReadG(uint8_t* base, uint32_t va)
{
    uint32_t b;
    memcpy(&b, base + va, 4);
    b = __builtin_bswap32(b);
    float v;
    memcpy(&v, &b, 4);
    return v;
}
void WriteG(uint8_t* base, uint32_t va, float v)
{
    uint32_t b;
    memcpy(&b, &v, 4);
    b = __builtin_bswap32(b);
    memcpy(base + va, &b, 4);
}

// THE HOOK — the cascade BUILDER (sub_825A89A8), which re-reads the active
// cascade-distance globals EVERY render to build the per-cascade near/far splits and
// projection matrices. (The time-of-day interpolator sub_823C1CC8 that writes these
// globals is never called on this title's static-lighting path — 0 calls even with
// shadows rendering — so hooking it did nothing.) Scale the globals at entry so the
// builder reads the extended distances, then RESTORE them at exit so nothing else sees
// scaled values and the next call scales from the original again (no compounding — the
// builder does not write these globals, so restore is safe, unlike the FOV node).
PPC_FUNC(sub_825A89A8)
{
    const float factor = ShadowDistFactor();
    if (factor == 1.0f)
    {
        __imp__sub_825A89A8(ctx, base);   // stock — bit-identical control
        return;
    }
    float saved[7];
    for (int i = 0; i < 7; ++i)
        saved[i] = ReadG(base, kCascadeGlobals[i]);

    if (Trace())
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            fprintf(stderr,
                    "[shadowdist] sub_825A89A8 hooked, factor %.2f — cascade globals "
                    "%.1f %.1f %.1f %.1f %.1f %.1f %.1f (scaling >1.0 ones)\n",
                    factor, saved[0], saved[1], saved[2], saved[3], saved[4], saved[5],
                    saved[6]);
        }
    }
    for (int i = 0; i < 7; ++i)
        if (saved[i] > 1.0f)   // a metric distance; leave 0 slots and sub-1 ratios alone
            WriteG(base, kCascadeGlobals[i], saved[i] * factor);

    __imp__sub_825A89A8(ctx, base);

    for (int i = 0; i < 7; ++i)   // restore, so nobody else sees scaled values
        WriteG(base, kCascadeGlobals[i], saved[i]);
}
