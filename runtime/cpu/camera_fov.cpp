// The camera-fov property watch (part 62) — the instrument for the culling fix.
//
// WHY THIS EXISTS. The FOV slider (renderer-side composite patch) widens what the
// GPU renders, but the title's own CPU culling still tests objects against its
// original frustum, so widened flanks show pop-in — the operator's report. The fix
// direction is to hand the GAME a wider fov so it renders AND culls wide. The
// engine's camera tunables are data-driven NAMED properties — cThirdPersonCam
// registers FOV_Min/+0x15c, FOV_Max/+0x160, FOV_Default/+0x164, FOV_Rate/+0x168
// (registration walked at 0x8246065C..) — all through one universal binder:
//
//     sub_82375518(this, name, count, &field)
//
// (0x82395ff0/0x82396000/0x82396010 are its count=1/2/4 thunks.) This hook watches
// the binder for names containing "FOV" and records each live FIELD ADDRESS, so a
// scanner/poker (or a later applier) can find the camera's fov without a 4 GB
// memory hunt — the earlier value-scan produced only asset-data coincidences
// (§6cs follow-up), because the live object's values were unknown a priori.
//
// LOGGING IS UNCONDITIONAL because registration happens at object construction
// (boot / zone load), not per frame — a handful of lines per run, gotcha 7 safe.

#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <cstring>

#include <cmath>

#include "../gpu/vk_renderer.h"
#include "../host/settings.h"
#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82375518);
extern "C" PPC_FUNC(__imp__sub_8246BF48);

// The camera-behavior PARAM GETTER: f1 = [this+0x14], pushed into a sink object
// (r4) via its vtable+4. The roaming camera's fov turned out to live as a
// behavior param node ("param_1", vtable 0x82062A94, value 43.0 — found by a
// live poke-test against the per-frame view-projection, part 62): poking the
// node's float moves the LIVE camera the same frame, and the title's own CPU
// culling follows (draws 4,984 -> 5,309 at 43 -> 60 deg). Every param in the
// camera system flows through this one getter, so the fov CALL SITE has to be
// distinguished — CZ_FOV_PARAM_TRACE=1 prints each distinct (lr, value) pair
// once, which is the census that finds it.
PPC_FUNC(sub_8246BF48)
{
    static const bool trace = getenv("CZ_FOV_PARAM_TRACE") != nullptr;
    if (trace)
    {
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        static std::mutex mu;
        uint32_t bits;
        memcpy(&bits, base + ctx.r3.u32 + 0x14, 4);
        bits = __builtin_bswap32(bits);
        float v;
        memcpy(&v, &bits, 4);
        std::lock_guard<std::mutex> lock(mu);
        if (seen.emplace(uint32_t(ctx.lr), bits).second)
            fprintf(stderr, "[fovparam] lr=%08X this=%08X value=%f\n",
                    uint32_t(ctx.lr), ctx.r3.u32, v);
    }
    // THE FOV SLIDER, GAME-SIDE (part 62). The census above found exactly ONE
    // call site reading the roaming camera's fov (lr 0x8246E31C, value 43.0 —
    // the OverShoulderCam asset's "FOV" property). Substituting the value HERE
    // hands the widened fov to the game itself, so the world renders AND CULLS
    // at the new fov — the renderer-side composite patch could only widen the
    // rendering, and the title's own CPU culling left pop-in at the flanks (the
    // operator's report).
    //
    // THE FIELD IS STATE, NOT A CONSTANT — the first form of this substitution
    // ("read, add N, call, restore") ran the camera to the 120° clamp in
    // seconds, because the game writes its smoothed fov back through the same
    // node, so +N compounded per frame. The shipped form captures the AUTHORED
    // value the first time the site fires for an object (before any
    // substitution can have disturbed it) and enforces base+N absolutely, no
    // restore; slider back to 0 enforces base exactly. CZ_NO_GAME_FOV=1 is the
    // control arm (the renderer-side patch stays available via CZ_VK_FOV).
    // AN ARM THAT CANNOT BE SHOWN TO HAVE ENGAGED IS NOT AN ARM (gotcha 408 — part 69
    // shipped a control whose variable went into the harness description instead of its
    // env block and reported the result as a null). This is the control for the whole
    // guest-side fov substitution, including part 62's wide-mode over-widen, so a
    // performance session that quotes it has to be able to prove it was on.
    static const bool off = [] {
        const bool o = getenv("CZ_NO_GAME_FOV") != nullptr;
        if (o)
            fprintf(stderr, "[fov] CZ_NO_GAME_FOV=1 — the guest-side fov substitution is "
                            "OFF; the game culls to its own 16:9 frustum\n");
        return o;
    }();
    if (!off && uint32_t(ctx.lr) == 0x8246E31C)
    {
        static std::mutex mu;
        static std::map<uint32_t, uint32_t> baseBits;   // node -> authored value
        static bool wasActive = false;
        const int fovAdj = Settings_Fov();
        // THE WIDE-MODE OVER-WIDEN (part 62, second operator report — §6cu). The
        // 21:9 view is k = (9W)/(16H) wider in tan space than the game's 16:9
        // frustum, so at ANY slider value the flanks show regions the game
        // believes are off-screen (before the composite unstretch this was
        // hidden — the view WAS the 16:9 frustum, stretched). The substitution
        // therefore hands the game v' = 2*atan(k * tan(v/2)) — its 16:9 frustum
        // then covers the 21:9 view's horizontal exactly — and the renderer's
        // composite wide patch narrows the projection back vertically, so the
        // picture is unchanged while the culling covers all of it.
        const float wideK = VkRenderer_WideFovFactor();
        std::lock_guard<std::mutex> lock(mu);
        auto it = baseBits.find(ctx.r3.u32);
        if (it == baseBits.end())
        {
            uint32_t bits;
            memcpy(&bits, base + ctx.r3.u32 + 0x14, 4);
            it = baseBits.emplace(ctx.r3.u32, bits).first;
        }
        if (fovAdj != 0 || wideK != 1.0f || wasActive)
        {
            float v;
            uint32_t sw = __builtin_bswap32(it->second);
            memcpy(&v, &sw, 4);
            v += float(fovAdj);
            if (v < 10.0f)
                v = 10.0f;
            if (v > 120.0f)
                v = 120.0f;
            if (wideK != 1.0f)
            {
                v = 2.0f * std::atan(wideK * std::tan(v * 0.00872664626f)) *
                    57.2957795f;
                if (v > 150.0f)
                    v = 150.0f;
            }
            memcpy(&sw, &v, 4);
            sw = __builtin_bswap32(sw);
            memcpy(base + ctx.r3.u32 + 0x14, &sw, 4);
            if (!wasActive)
            {
                wasActive = true;
                fprintf(stderr, "[fovgame] game-side fov ACTIVE at the camera "
                                "param getter: base%+d deg, wide-culling factor "
                                "%.4f -> %.2f deg (node %08X)\n", fovAdj, wideK,
                        v, ctx.r3.u32);
            }
        }
    }
    __imp__sub_8246BF48(ctx, base);
}

PPC_FUNC(sub_82375518)
{
    static const bool on = getenv("CZ_FOV_PROP_TRACE") != nullptr;
    const uint32_t nameVa = ctx.r4.u32;
    if (on && nameVa >= 0x82000000 && nameVa < 0x82C00000)
    {
        char name[48] = {};
        for (int i = 0; i < 47; i++)
        {
            const char c = char(base[nameVa + i]);
            if (!c || uint8_t(c) < 0x20 || uint8_t(c) > 0x7E)
                break;
            name[i] = c;
        }
        if (strstr(name, "FOV") || strstr(name, "Fov"))
            fprintf(stderr, "[fovprop] \"%s\" obj=%08X count=%u field=%08X\n",
                    name, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32);
    }
    __imp__sub_82375518(ctx, base);
}
