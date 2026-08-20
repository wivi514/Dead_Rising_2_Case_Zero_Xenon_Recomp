// The title's own PC graphics menu, switched back on (part 60).
//
// WHAT SHIPS IN THE 360 PACKAGE, all verified offline before this file was written:
//   * data/frontend/fecmn.big carries options_pc.txt — a complete PC settings screen
//     (Resolution, DisplayMode, VSync, Shadow, Multisampling, ...) — and the path
//     manifests (path_fe.txt, path_pause.txt) wire `OptionsPC` into BOTH menu graphs
//     with that layout file.
//   * str_en.bcs resolves every label the layout names ("PC Settings", "Resolution",
//     "Fullscreen", even the revert-countdown dialog). So does every other language.
//   * The screen NAME is registered: the hub's ACT dispatcher (sub_825003A0's chain
//     at 0x825003C0) compares the verb hash against five interned screen-name hashes
//     INCLUDING OptionsPC's (global 0x82A58D28) and routes any match into the same
//     transition call (sub_827F6D40) the DebugJump resurrection already drives.
//   * The hub's enter code HIDES the OptionsPC row on this build (0x824D6708: find
//     child by name hash, clear flag 0x00800000) — which is why the operator has
//     never seen it.
//
// WHAT DOES NOT SHIP: the screen's verb HANDLERS. Every working options screen has a
// per-screen ACT virtual that relays Prev/Next to the spin widget and stores the
// result (the model is the OptionsGameplay handler at 0x8251EEC8) — and a handler
// exists if and only if its verb-name string exists in the image, because every
// comparison hashes the NAME at runtime (sub_8276E398; no precomputed hash constants
// anywhere — verified by scanning .text for every candidate hash value, including
// the hashes of verbs KNOWN to work, which also found nothing). "VSync",
// "Fullscreen", "DisplayMode", "Msaa", "Mouse", "Texture" do not exist in the image:
// those handlers were compiled out of the 360 build. This file is their replacement.
//
// HOW IT WORKS:
//   1. tools/gen_pc_options.py rewrites options_pc.txt (served via the VFS overlay)
//      to the working 360 spinner idiom: each kept row's CTSelect is a cFETextList
//      with the values baked in, and the verbs are ACT:Prev:<Group>/ACT:Next:<Group>.
//   2. The sub_827F6D40 hook redirects a Visuals open to OptionsPC — the operator
//      asked for the gamma-only Visuals screen to be REPLACED, and redirecting the
//      one transition covers both the pause menu and the main menu without touching
//      the hub layout or its row-hiding enter code.
//   3. The OptionsPC screen's descriptor routes its ACT slot to the shared default
//      handler sub_82379380 ("return 0" — the filler the descriptor tables use for
//      screens with no handler of their own). We hook that default: if the PC screen
//      is the one open and the verb's token hashes match our groups, WE are the
//      handler — relay the direction to the spin widget exactly the way the title's
//      own handlers do (widget+0xC0 virtual +0xC advances and wraps natively), read
//      back the index (widget->0x3FC node, +4), and apply it host-side through
//      host/settings.cpp.
//
// CZ_NO_PC_OPTIONS=1 is the whole-feature control arm (no redirect, no handler —
// the shipped Visuals screen, byte for byte when CZ_NO_PATCHED_ASSETS=1 is also
// set). CZ_PCOPT_TRACE=1 logs every call that reaches the hooked default while the
// PC screen is open — the instrument that finds the enter/other slots' signatures
// without guessing them.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../kernel/memory.h"
#include "../host/settings.h"
#include "../gpu/vk_renderer.h"
#include "pc_options.h"
#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82379380);

namespace
{

bool Disabled()
{
    static const bool off = getenv("CZ_NO_PC_OPTIONS") != nullptr;
    return off;
}

bool TraceOn()
{
    static const bool on = getenv("CZ_PCOPT_TRACE") != nullptr;
    return on;
}

uint32_t LoadU32(uint8_t* base, uint32_t addr)
{
    uint32_t v;
    memcpy(&v, base + addr, 4);
    return __builtin_bswap32(v);
}

// The title's own name hash (sub_8276E398, transcribed): h = h*33 ^ sign-extended
// byte. ASCII never sets the sign bit, so the extension never matters for these
// names — stated so nobody "fixes" it into a mismatch.
uint32_t H33(const char* s)
{
    uint32_t h = 0;
    for (; *s; ++s)
        h = (h * 33) ^ uint32_t(int32_t(int8_t(*s)));
    return h;
}

// Interned-hash globals the guest computes at static init — read to CHECK our H33
// against the title's own answer, once, the first time the filter could matter. A
// silent algorithm mismatch would present as a menu that ignores every press.
constexpr uint32_t kGuestHashOptionsVisual = 0x82A58D20;
constexpr uint32_t kGuestHashOptionsPC     = 0x82A58D28;

const uint32_t kPrev = H33("Prev");
const uint32_t kNext = H33("Next");
const uint32_t kNavBack = H33("NavBack");
const uint32_t kOptionsVisual = H33("OptionsVisual");
const uint32_t kOptionsPC = H33("OptionsPC");
const uint32_t kOptionsHub = H33("Options");

struct Group
{
    const char* name;
    uint32_t hash;
};
const Group kGroups[] = {
    { "Resolution", H33("Resolution") },
    { "DisplayMode", H33("DisplayMode") },
    { "VSync", H33("VSync") },
    { "Shadow", H33("Shadow") },
};

// Is the OptionsPC screen the one most recently opened? Set by the transition
// filter, which sees EVERY screen change (ours and the title's own), so it clears
// itself the moment anything else opens.
std::atomic<bool> g_pcScreenOpen{ false };
// The screen-state object our handler last saw, and whether this OPEN of the screen
// has had its spins synced to the persisted settings yet.
std::atomic<bool> g_spinsSynced{ false };

uint64_t g_verbCount = 0;

// One-time check that our H33 IS the title's hash. Compares against the interned
// globals the guest wrote at static init; a zero global means init has not run yet
// and the check stays pending rather than passing vacuously.
bool HashAlgorithmChecked(uint8_t* base)
{
    static std::atomic<int> state{ 0 }; // 0 pending, 1 ok, -1 mismatch
    int s = state.load(std::memory_order_acquire);
    if (s != 0)
        return s > 0;
    const uint32_t guestVisual = LoadU32(base, kGuestHashOptionsVisual);
    const uint32_t guestPC = LoadU32(base, kGuestHashOptionsPC);
    if (!guestVisual || !guestPC)
        return false; // static init has not interned them yet
    if (guestVisual == kOptionsVisual && guestPC == kOptionsPC)
    {
        state.store(1, std::memory_order_release);
        return true;
    }
    state.store(-1, std::memory_order_release);
    fprintf(stderr, "[pcopt] HASH ALGORITHM MISMATCH: guest OptionsVisual=%08X "
                    "ours=%08X, guest OptionsPC=%08X ours=%08X — the PC options "
                    "screen is DISABLED this run (every filter would be blind)\n",
            guestVisual, kOptionsVisual, guestPC, kOptionsPC);
    return false;
}

// Call a guest function by address, loudly refusing an address the dispatch table
// does not know — a vtable slot that is not a recompiled function start would
// otherwise be a silent no-op wearing a working feature's name.
bool GuestCall(PPCContext& ctx, uint8_t* base, uint32_t fnAddr, const char* what)
{
    PPCFunc* fn = g_memory.FindFunction(fnAddr);
    if (!fn)
    {
        fprintf(stderr, "[pcopt] %s: %08X is not a known function start — REFUSED\n",
                what, fnAddr);
        return false;
    }
    fn(ctx, base);
    return true;
}

// screen->vtbl[+0x48](screen, nameHash) — find a child widget by name hash. The
// idiom is transcribed from the hub's own enter code at 0x824D66B0.
uint32_t FindChildWidget(PPCContext& ctx, uint8_t* base, uint32_t screen,
                         uint32_t nameHash)
{
    const uint32_t vtbl = LoadU32(base, screen);
    const uint32_t fn = LoadU32(base, vtbl + 0x48);
    ctx.r3.u64 = screen;
    ctx.r4.u64 = nameHash;
    if (!GuestCall(ctx, base, fn, "find-child"))
        return 0;
    return ctx.r3.u32;
}

// (widget+0xC0)->vtbl[+0xC](obj, directionHash, 0x10, 0) — the spin advance every
// working screen handler relays (0x8251EF50 is the transcription source). The spin
// group itself wraps, animates and updates its CTSelect text.
void SpinRelay(PPCContext& ctx, uint8_t* base, uint32_t widget, uint32_t dirHash)
{
    const uint32_t obj = widget + 0xC0;
    const uint32_t vtbl = LoadU32(base, obj);
    const uint32_t fn = LoadU32(base, vtbl + 0xC);
    ctx.r3.u64 = obj;
    ctx.r4.u64 = dirHash;
    ctx.r5.u64 = 0x10;
    ctx.r6.u64 = 0;
    GuestCall(ctx, base, fn, "spin-relay");
}

// The current selection index — sub_827E9968's two loads, done directly:
// *(widget+0x3FC) is the selection node, its +4 the index. No call needed.
uint32_t SpinIndex(uint8_t* base, uint32_t widget)
{
    const uint32_t node = LoadU32(base, widget + 0x3FC);
    return node ? LoadU32(base, node + 4) : 0;
}

// Rotate one spin group to `want` by relaying Next — the title's own advance path,
// so the CTSelect text and animations stay right. Bounded: no list here is longer
// than 4, and a spin that will not converge is reported, not spun forever.
void SpinSetIndex(PPCContext& ctx, uint8_t* base, uint32_t widget, uint32_t want,
                  const char* name)
{
    for (int guard = 0; guard < 8; ++guard)
    {
        if (SpinIndex(base, widget) == want)
            return;
        SpinRelay(ctx, base, widget, kNext);
    }
    fprintf(stderr, "[pcopt] %s: spin did not converge on index %u (at %u)\n", name,
            want, SpinIndex(base, widget));
}

// The persisted settings, as spin indices, in the patched layout's list orders.
uint32_t WantIndex(int group)
{
    switch (group)
    {
        case 0: return Settings_RenderScale() - 1;          // 720p,1440p,2160p,2880p
        case 1: return uint32_t(Settings_DisplayMode());    // Window,Borderless,Full
        case 2: return Settings_VSync() ? 1u : 0u;          // Off,On
        case 3: return uint32_t(Settings_ShadowTier());     // Low,Medium,High
    }
    return 0;
}

// Sync every spin to the persisted settings. Runs on the first verb this open (and
// on open once a construction trigger is wired — see the trace note below).
void SyncAllSpins(PPCContext& ctx, uint8_t* base, uint32_t screen)
{
    for (int g = 0; g < 4; ++g)
    {
        const uint32_t widget = FindChildWidget(ctx, base, screen, kGroups[g].hash);
        if (!widget)
        {
            fprintf(stderr, "[pcopt] sync: spin group %s not found on the screen\n",
                    kGroups[g].name);
            continue;
        }
        SpinSetIndex(ctx, base, widget, WantIndex(g), kGroups[g].name);
    }
}

// A verb landed on one of our groups: relay it, read the result, apply it.
void HandleGroupVerb(PPCContext& ctx, uint8_t* base, uint32_t stateObj, int group,
                     uint32_t dirHash)
{
    const uint32_t screen = LoadU32(base, stateObj + 4);
    if (!screen)
    {
        fprintf(stderr, "[pcopt] verb: state object %08X has no screen at +4\n",
                stateObj);
        return;
    }
    if (!g_spinsSynced.exchange(true, std::memory_order_acq_rel))
        SyncAllSpins(ctx, base, screen);

    const uint32_t widget = FindChildWidget(ctx, base, screen, kGroups[group].hash);
    if (!widget)
    {
        fprintf(stderr, "[pcopt] verb: group %s not found\n", kGroups[group].name);
        return;
    }
    SpinRelay(ctx, base, widget, dirHash);
    const uint32_t idx = SpinIndex(base, widget);
    fprintf(stderr, "[pcopt] %s -> index %u\n", kGroups[group].name, idx);

    switch (group)
    {
        case 0:
            Settings_SetRenderScale(idx + 1);
            fprintf(stderr, "[pcopt] resolution %ux%u — applies at next launch "
                            "(the render targets are built around the scale at "
                            "renderer init)\n", 1280 * (idx + 1), 720 * (idx + 1));
            break;
        case 1:
            Settings_SetDisplayMode(CzDisplayMode(int(idx)));
            break;
        case 2:
            Settings_SetVSync(idx == 1);
            VkRenderer_RequestSwapchainRebuild();
            break;
        case 3:
            Settings_SetShadowTier(int(idx));
            break;
    }
    ++g_verbCount;
}

} // namespace

void PcOptions_FilterScreenTransition(PPCContext& ctx, uint8_t* base)
{
    if (Disabled() || !HashAlgorithmChecked(base))
        return;
    uint32_t hash = ctx.r4.u32;
    if (hash == kOptionsVisual)
    {
        ctx.r4.u64 = kOptionsPC;
        hash = kOptionsPC;
        static uint64_t redirects = 0;
        fprintf(stderr, "[pcopt] Visuals -> OptionsPC redirect (%llu so far; "
                        "CZ_NO_PC_OPTIONS=1 restores the gamma screen)\n",
                (unsigned long long)(++redirects));
    }
    const bool nowOpen = hash == kOptionsPC;
    if (nowOpen && !g_pcScreenOpen.load(std::memory_order_acquire))
        g_spinsSynced.store(false, std::memory_order_release);
    g_pcScreenOpen.store(nowOpen, std::memory_order_release);
}

// The shared "return 0" default handler out of the screen descriptor tables. For
// every screen whose slot holds it, it means "this screen does not handle that" —
// which for the OptionsPC screen's ACT slot is exactly the hole this hook fills.
// The filter is three loads and two compares, only after a cheap atomic says the
// PC screen is open; every other caller falls straight through.
PPC_FUNC(sub_82379380)
{
    if (!Disabled() && g_pcScreenOpen.load(std::memory_order_acquire))
    {
        const uint32_t stateObj = ctx.r3.u32;
        const uint32_t verb = ctx.r4.u32;
        if (verb)
        {
            const uint32_t t1 = LoadU32(base, verb + 4);
            const uint32_t t2 = LoadU32(base, verb + 8);
            if (TraceOn())
                fprintf(stderr, "[pcopt-trace] default handler: r3=%08X r4=%08X "
                                "t1=%08X t2=%08X\n", stateObj, verb, t1, t2);
            if (t1 == kPrev || t1 == kNext)
            {
                for (int g = 0; g < 4; ++g)
                    if (t2 == kGroups[g].hash)
                    {
                        HandleGroupVerb(ctx, base, stateObj, g, t1);
                        ctx.r3.u64 = 1; // handled
                        return;
                    }
            }
        }
    }
    __imp__sub_82379380(ctx, base);
}
