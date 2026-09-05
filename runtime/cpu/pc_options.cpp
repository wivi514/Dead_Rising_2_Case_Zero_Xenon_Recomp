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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../kernel/memory.h"
#include "../host/settings.h"
#include "../host/window.h"
#include "../gpu/vd.h"
#include "../gpu/vk_renderer.h"
#include "pc_options.h"
#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82379380);
extern "C" PPC_FUNC(__imp__sub_824B9AE0);
extern "C" PPC_FUNC(__imp__sub_827FFB28);
extern "C" PPC_FUNC(__imp__sub_827F6D40);
extern "C" PPC_FUNC(__imp__sub_824D6600);

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
// The frontend transition manager, captured from the transition hook — the object
// whose state words name where a stalled transition is stuck.
std::atomic<uint32_t> g_manager{ 0 };
// The OptionsPC screen object, captured from the layout factory the moment it is
// created for "options_pc.txt" — the object whose vtbl+0x48 finds children.
std::atomic<uint32_t> g_pcRoot{ 0 };
// The widget-root class vtable, LEARNED from the options hub's own enter hook
// (sub_824D6600, the function that hides the PC row) — the one object this
// project has hard evidence is a find-child-capable root. The setup probe only
// ever calls a guest virtual on an object of exactly this class; the first
// operator run crashed because a look-alike vtable passed a shape check.
std::atomic<uint32_t> g_rootVtbl{ 0 };
// The PC screen's STATE object, captured (pointer only, no guest calls) while the
// screen is being built; the pump consumes it AFTER the manager reports the
// transition finished. Running setup during the build was the second crash: the
// engine calls descriptor defaults mid-construction, and a SetFocus on a
// half-built widget tree dies on a not-yet-written vtable slot.
std::atomic<uint32_t> g_pcState{ 0 };
// Once set up: the four spin-row widgets and the host-tracked selection. The
// engine's focus system is never involved — SetState(widget, 1) plays each row's
// own StateFocus animation, which is all "selected" ever meant visually.
uint32_t g_rowWidget[4] = {};
int g_selectedRow = 0;
bool g_setupDone = false;
uint32_t g_prevButtons = 0;
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

// widget->vtbl[+0x4C](widget, state) — the per-widget state/animation trigger the
// hub's own row-hiding code calls (0x824D66E0). State 1 = StateFocus in the
// engine's own state-name table at 0x820B8FE8, which is the row highlight.
void SetWidgetState(PPCContext& ctx, uint8_t* base, uint32_t widget, uint32_t state)
{
    const uint32_t vtbl = LoadU32(base, widget);
    const uint32_t fn = LoadU32(base, vtbl + 0x4C);
    ctx.r3.u64 = widget;
    ctx.r4.u64 = state;
    GuestCall(ctx, base, fn, "set-state");
}

// Light or unlight a whole ROW: the StateFocus animations live on the row's
// CHILDREN (the "highlight" bar, the label, the value text group), so the state
// is pushed one level down as well as onto the group itself.
const uint32_t kRowChildren[] = { H33("highlight"), H33("optionlabel"),
                                  H33("textgroup") };
void SetRowState(PPCContext& ctx, uint8_t* base, uint32_t group, uint32_t state)
{
    SetWidgetState(ctx, base, group, state);
    for (uint32_t childHash : kRowChildren)
    {
        const uint32_t child = FindChildWidget(ctx, base, group, childHash);
        if (child)
            SetWidgetState(ctx, base, child, state);
    }
}

// Map a spin row's index onto the persistent setting and its applier.
void ApplySetting(int row, uint32_t idx)
{
    switch (row)
    {
        case 0:
            Settings_SetRenderScale(idx + 1);
            fprintf(stderr, "[pcopt] resolution %ux%u — applies at next launch\n",
                    1280 * (idx + 1), 720 * (idx + 1));
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
}

// Try to run the PC screen's open-time setup (initial focus + spin sync) with a
// candidate STATE object. Returns quietly unless the candidate proves itself by
// resolving the Resolution row. See the note at the call site in sub_82379380.
static void PcOptionsTrySetup(PPCContext& ctx, uint8_t* base, uint32_t stateObj)
{
    (void)ctx;
    const uint32_t wantVtbl = g_rootVtbl.load(std::memory_order_acquire);
    if (!wantVtbl)
        return;               // the hub has not taught us the root class yet
    if (stateObj < 0x10000 || stateObj >= 0xC0000000)
        return;
    const uint32_t root = LoadU32(base, stateObj + 4);
    if (root < 0x10000 || root >= 0xC0000000)
        return;
    if (LoadU32(base, root) != wantVtbl)
        return;               // not a widget root
    // CAPTURE ONLY. The pump runs the actual setup once the manager says the
    // transition is over — see the comment on g_pcState.
    g_pcState.store(stateObj, std::memory_order_release);
}

} // namespace

bool PcOptions_FilterScreenTransition(PPCContext& ctx, uint8_t* base)
{
    if (Disabled() || !HashAlgorithmChecked(base))
        return false;
    g_manager.store(ctx.r3.u32, std::memory_order_release);
    uint32_t hash = ctx.r4.u32;
    // THE DEFAULT PATH: Visuals opens the HOST settings panel, no guest
    // transition at all — the hub stays alive underneath as the backdrop and
    // gets its input back the moment the panel closes. The native-screen
    // experiment (below) stays as an arm; its record is the part-60 notes.
    if (hash == kOptionsVisual && !getenv("CZ_PCOPT_NATIVE"))
    {
        Settings_SetOverlayVisible(true);
        // Everything currently held (the A that selected Visuals, most of all)
        // counts as already-pressed: without this the panel's first poll sees
        // that A as a fresh edge and instantly closes itself — measured in the
        // headless repro, where the synthetic press outlives the transition.
        g_prevButtons = ~0u;
        static uint64_t opens = 0;
        uint32_t rw = 0, rh = 0;
        Settings_InternalRes(rw, rh);
        // The values the panel is ABOUT TO SHOW, logged at every open — the
        // instrument for the operator's "it resets to 720p when I reopen" report:
        // if this line says the store is right while the screen shows defaults,
        // the defect is presentation-side; if the store is wrong, grep up for the
        // writer.
        fprintf(stderr, "[pcopt] Visuals -> host settings panel (open %llu) — "
                        "showing res=%ux%u mode=%d vsync=%d tier=%d cap=%d fov=%+d\n",
                (unsigned long long)(++opens), rw, rh, int(Settings_DisplayMode()),
                Settings_VSync() ? 1 : 0, Settings_ShadowTier(), Settings_FpsCap(),
                Settings_Fov());
        return true;
    }
    if (hash == kOptionsVisual)
    {
        ctx.r4.u64 = kOptionsPC;
        hash = kOptionsPC;
        // The native ACT path (0x825007C0) stores the verb hash into this global
        // just before calling the transition; a swap that leaves it saying
        // OptionsVisual is silently REFUSED — the first redirect attempt reached
        // the manager and then nothing happened at all, no screen change, no
        // layout read, no frontend log line. Keep the pair consistent.
        constexpr uint32_t kPendingScreenHash = 0x82A59384;
        uint32_t be = __builtin_bswap32(kOptionsPC);
        memcpy(base + kPendingScreenHash, &be, 4);
        static uint64_t redirects = 0;
        fprintf(stderr, "[pcopt] Visuals -> OptionsPC redirect (%llu so far; "
                        "CZ_NO_PC_OPTIONS=1 restores the gamma screen)\n",
                (unsigned long long)(++redirects));
        if (TraceOn())
        {
            const uint32_t mgr = ctx.r3.u32;
            const uint32_t cur = LoadU32(base, mgr + 0x120);
            const uint32_t vtbl = cur ? LoadU32(base, cur) : 0;
            fprintf(stderr, "[pcopt-trace] at redirect: mgr=%08X cur=%08X vtbl=%08X "
                            "slot+4=%08X slot+8=%08X\n", mgr, cur, vtbl,
                    vtbl ? LoadU32(base, vtbl + 4) : 0,
                    vtbl ? LoadU32(base, vtbl + 8) : 0);
            fprintf(stderr, "[pcopt-trace]   dialog flag cur+0x20 = %02X\n",
                    base[cur + 0x20]);
            // The two transition lists the resolver (0x827FCC60) consults: forward
            // (*this+0x18) and back (*this+0x1C); each list object's +0xC is a
            // table of 12-byte {hash, ...} entries, count at table+0x308.
            for (int which = 0; which < 2 && cur; ++which)
            {
                const uint32_t list = LoadU32(base, cur + 0x18 + 4 * which);
                const uint32_t table = list ? LoadU32(base, list + 0xC) : 0;
                if (!table)
                {
                    fprintf(stderr, "[pcopt-trace]   list%d: none\n", which);
                    continue;
                }
                const uint32_t count = LoadU32(base, table + 0x308);
                fprintf(stderr, "[pcopt-trace]   list%d table=%08X count=%u:\n",
                        which, table, count);
                for (uint32_t k = 0; k < count && k < 16; ++k)
                    fprintf(stderr, "[pcopt-trace]     {%08X %08X %08X}\n",
                            LoadU32(base, table + 8 + 12 * k),
                            LoadU32(base, table + 12 + 12 * k),
                            LoadU32(base, table + 16 + 12 * k));
            }
        }
    }
    const bool nowOpen = hash == kOptionsPC;
    if (nowOpen != g_pcScreenOpen.load(std::memory_order_acquire))
    {
        g_spinsSynced.store(false, std::memory_order_release);
        g_pcState.store(0, std::memory_order_release);
        g_setupDone = false;
        memset(g_rowWidget, 0, sizeof(g_rowWidget));
        g_selectedRow = 0;
    }
    g_pcScreenOpen.store(nowOpen, std::memory_order_release);
    return false;
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
        // While the PC screen's open-time setup is pending, test whether THIS call's
        // r3 is the screen's state object: its +4 must be a widget root that
        // resolves the Resolution row by name hash. The state object is descriptor-
        // bound and flows through this shared default constantly; capturing it here
        // is what replaces the compiled-out class enter's knowledge of 'this'.
        if (!g_spinsSynced.load(std::memory_order_acquire))
            PcOptionsTrySetup(ctx, base, ctx.r3.u32);
        // Stall diagnosis (CZ_PCOPT_TRACE): the manager's state words, sampled at
        // most every few thousand calls through this very hot default.
        if (TraceOn())
        {
            static uint64_t calls = 0;
            if (++calls % 20000 == 1)
            {
                const uint32_t m = g_manager.load(std::memory_order_acquire);
                if (m)
                    fprintf(stderr, "[pcopt-trace] manager %08X: cur=%08X "
                                    "state178=%08X target17C=%08X busy16C=%02X%02X "
                                    "b13C=%02X pend9384=%08X\n",
                            m, LoadU32(base, m + 0x120), LoadU32(base, m + 0x178),
                            LoadU32(base, m + 0x17C), base[m + 0x16C],
                            base[m + 0x16D], base[m + 0x13C],
                            LoadU32(base, 0x82A59384));
            }
        }
        const uint32_t stateObj = ctx.r3.u32;
        const uint32_t verb = ctx.r4.u32;
        // r4 is only a verb STRUCT for the ACT-dispatch callers; other callers of
        // this shared default pass small integers (the first probe crashed reading
        // guest 0x3E — the PROT_NONE null page — off r4=0x3A). Past the null page
        // is the cheap admission test; the hash pair is the real one.
        // Message 0x14 is the CLOSE handshake: every working screen's handler
        // answers it with 1 after its cleanup (0x82500AE0: cmpwi r4,0x14 ->
        // bl <leave> -> li r3,1) and the transition manager WAITS on that
        // answer — the compiled-out class returning 0 here is why the PC screen
        // could never be left, with or without our input driver.
        if (verb == 0x14)
        {
            ctx.r3.u64 = 1;
            return;
        }
        if (verb < 0x1000)
        {
            // Not a verb struct — a small message id. Traced because the stalled
            // transition is the manager pumping SOMETHING through this default
            // and being told "unhandled" forever.
            if (TraceOn())
                fprintf(stderr, "[pcopt-trace] default handler: r3=%08X MSG=%u "
                                "lr=%08X\n", stateObj, verb, uint32_t(ctx.lr));
        }
        else
        {
            const uint32_t t1 = LoadU32(base, verb + 4);
            const uint32_t t2 = LoadU32(base, verb + 8);
            if (TraceOn())
                fprintf(stderr, "[pcopt-trace] default handler: r3=%08X r4=%08X "
                                "t1=%08X t2=%08X lr=%08X\n", stateObj, verb, t1, t2,
                        uint32_t(ctx.lr));
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

// The screen FACTORY (name -> constructed screen object), hooked for diagnosis:
// the stalled Visuals->OptionsPC redirect ends with the manager's current screen
// NULL, which is either this factory failing or the screen discarding itself.
// Trace-only; behaviour unchanged.
PPC_FUNC(sub_824B9AE0)
{
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32,
                   a6 = ctx.r6.u32;
    __imp__sub_824B9AE0(ctx, base);
    if (!Disabled())
    {
        char name[24] = {};
        if (a4 >= 0x1000)
            for (int i = 0; i < 23; ++i)
            {
                const char c = char(base[a4 + i]);
                if (!c || uint8_t(c) < 0x20 || uint8_t(c) > 0x7E)
                    break;
                name[i] = c;
            }
        if (!strcmp(name, "options_pc.txt") && ctx.r3.u32)
        {
            g_pcRoot.store(ctx.r3.u32, std::memory_order_release);
            g_spinsSynced.store(false, std::memory_order_release);
        }
        if (getenv("CZ_PCOPT_TRACE"))
            fprintf(stderr, "[pcopt-trace] factory(%08X, %08X \"%s\", %08X, %08X) "
                            "-> %08X\n", a3, a4, name, a5, a6, ctx.r3.u32);
    }
}

// Stall diagnosis hooks (CZ_PCOPT_TRACE only): the child-screen resolver and its
// two halves, around the silent transition-to-nothing.
extern "C" PPC_FUNC(__imp__sub_827FCC60);
extern "C" PPC_FUNC(__imp__sub_827F6338);
extern "C" PPC_FUNC(__imp__sub_827EDA60);

PPC_FUNC(sub_827FCC60)
{
    const bool t = !Disabled() && getenv("CZ_PCOPT_TRACE") &&
                   ctx.r4.u32 == kOptionsPC;
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;
    __imp__sub_827FCC60(ctx, base);
    if (t)
        fprintf(stderr, "[pcopt-trace] resolver(this=%08X, hash=%08X) -> %08X\n",
                a3, a4, ctx.r3.u32);
}

PPC_FUNC(sub_827F6338)
{
    const bool t = !Disabled() && getenv("CZ_PCOPT_TRACE") &&
                   ctx.r4.u32 == kOptionsPC;
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;
    const uint32_t lrsave = uint32_t(ctx.lr);
    __imp__sub_827F6338(ctx, base);
    if (t)
        fprintf(stderr, "[pcopt-trace] list-lookup(list=%08X, hash=%08X) -> %08X "
                        "lr=%08X\n", a3, a4, ctx.r3.u32, lrsave);
}

PPC_FUNC(sub_827EDA60)
{
    const bool t = !Disabled() && getenv("CZ_PCOPT_TRACE");
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;
    __imp__sub_827EDA60(ctx, base);
    if (t)
        fprintf(stderr, "[pcopt-trace] commit(this=%08X, params=%08X, node=%08X) "
                        "-> %08X\n", a3, a4, a5, ctx.r3.u32);
}

// Parse-entry argument sniffer (CZ_PCOPT_TRACE): which register carries the layout
// TEXT and which its length — the seam the in-memory text swap needs.
extern "C" PPC_FUNC(__imp__sub_827846A8);
PPC_FUNC(sub_827846A8)
{
    if (!Disabled() && getenv("CZ_PCOPT_TRACE"))
    {
        char peek[4][28] = {};
        const uint32_t a[4] = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32 };
        for (int k = 0; k < 4; ++k)
            if (a[k] >= 0x10000 && a[k] < 0xC0000000)
                for (int i = 0; i < 27; ++i)
                {
                    const char c = char(base[a[k] + i]);
                    if (!c || uint8_t(c) < 0x20 || uint8_t(c) > 0x7E)
                        break;
                    peek[k][i] = c;
                }
        fprintf(stderr, "[pcopt-trace] parse(r3=%08X \"%s\", r4=%08X \"%s\", "
                        "r5=%08X \"%s\", r6=%08X \"%s\")\n",
                a[0], peek[0], a[1], peek[1], a[2], peek[2], a[3], peek[3]);
    }
    __imp__sub_827846A8(ctx, base);
}


// See pc_options.h. The trigger is measured, not guessed: the manager's state
// word (+0x178) returns to 0 and its current-screen slot (+0x120) is non-null
// once the transition lands; before part 60's trailer fix those reads were the
// instrument that found the silent transition-to-nothing.
void PcOptions_Pump(PPCContext& ctx, uint8_t* base, uint32_t buttons)
{
    if (Disabled())
        return;

    // ---- the host settings panel's input, when it is up ----
    if (Settings_OverlayVisible())
    {
        (void)ctx;
        const uint32_t pressed = buttons & ~g_prevButtons;
        g_prevButtons = buttons;
        if (!pressed)
            return;
        // A COOLDOWN on top of edge detection. The title polls this import many
        // times per frame and from more than one thread, and pure edge detection
        // against one shared previous-state word misfires across those polls —
        // the operator measured it as "one press jumps five values". 180 ms is
        // the classic menu repeat gate; holding a direction is NOT auto-repeat
        // here on purpose, one step per press.
        static std::chrono::steady_clock::time_point lastAction{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastAction < std::chrono::milliseconds(180))
            return;
        lastAction = now;
        constexpr uint32_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004,
                           kRight = 0x0008, kB = 0x2000, kA = 0x1000, kX = 0x4000;
        // One applier for both input styles (Left/Right and the console-style A
        // step): the first version was two hand-copied switch blocks, which is how
        // a fifth row lands in one and not the other. `dir` is +1 or -1.
        auto applyRow = [](int row, int dir) {
            switch (row)
            {
                case 0:
                {
                    // THE DISPLAY'S OWN MODE LIST (operator revision 3): step
                    // through every distinct size the monitor reports that the
                    // renderer can produce — 1920x1080 and friends included, which
                    // no integer multiple of 1280x720 could express. Fallback when
                    // no display list exists (headless, or SDL said nothing): the
                    // synthesized 16:9 ladder, so the row still steps.
                    //
                    // STEPPING MOVES ONLY THE PENDING VALUE (part 91, the
                    // operator's spec: "do not change resolution live every time
                    // the player changes it — only when they apply"). The X press
                    // below persists it and hands it to the renderer's live-apply
                    // seam; leaving the panel discards it. The live path's part-60
                    // freeze was the mid-frame apply placement, fixed with the
                    // relocation to the frame boundary (BeginFrame's note).
                    uint32_t modes[64];
                    int count = Host_DisplayModeList(modes, 32);
                    if (count == 0)
                        for (uint32_t sc = 1; sc <= 4; ++sc)
                        {
                            modes[count * 2] = 1280 * sc;
                            modes[count * 2 + 1] = 720 * sc;
                            ++count;
                        }
                    uint32_t cw = 0, ch = 0;
                    Settings_PendingInternalRes(cw, ch);
                    if (!cw)
                        Settings_InternalRes(cw, ch);
                    int at = 0;
                    for (int i = 0; i < count; ++i)
                        if (modes[i * 2] == cw && modes[i * 2 + 1] == ch)
                            at = i;
                    // CLAMPED at the ends, no wrap — the operator's "it always
                    // shows 720p when I open it": their resolution was the LAST
                    // list entry, so the first right-press wrapped to the smallest
                    // every time, three sessions running. An ordered ladder clamps.
                    at += dir;
                    if (at < 0)
                        at = 0;
                    if (at >= count)
                        at = count - 1;
                    Settings_SetPendingInternalRes(modes[at * 2], modes[at * 2 + 1]);
                    fprintf(stderr, "[pcopt] resolution %ux%u PENDING — X applies\n",
                            modes[at * 2], modes[at * 2 + 1]);
                    break;
                }
                case 1:
                {
                    const int m = (int(Settings_DisplayMode()) + dir + 3) % 3;
                    Settings_SetDisplayMode(CzDisplayMode(m));
                    break;
                }
                case 2:
                    Settings_SetVSync(!Settings_VSync());
                    VkRenderer_RequestSwapchainRebuild();
                    break;
                case 3:
                {
                    // THE SINGLE SHADOW ROW (part 64, operator's revision):
                    // LOW/MEDIUM/HIGH then RT LOW/MEDIUM/HIGH, where an RT value
                    // REPLACES the raster cascade. The ladder stops at HIGH on a
                    // device without ray query rather than offering values that
                    // cannot engage.
                    // PARKED as of part 71 (operator instruction): the RT rungs are
                    // not offered unless CZ_VK_RT_MENU=1, so the span is normally 3.
                    // STEP FROM THE RASTER TIER when they are not offered — stepping
                    // from Settings_ShadowRow() would start at `2 + rtShadows` for any
                    // cz_settings.txt that still carries an RT choice, and the first
                    // press would land somewhere unrelated. The stored RT value is left
                    // alone so unparking restores it.
                    const bool rt = VkRenderer_RtAvailable();
                    const int span = rt ? 6 : 3;
                    const int from = rt ? Settings_ShadowRow() : Settings_ShadowTier();
                    const int r = (from + dir + span) % span;
                    Settings_SetShadowRow(r);
                    // Applied live on both halves: the renderer re-reads the raster
                    // tier once per frame (ShadowScaleThisFrame) and the RT tier the
                    // same way (rtshadow::TierThisFrame).
                    fprintf(stderr, "[pcopt] shadow row %d — live\n", r);
                    break;
                }
                case 4:
                {
                    // The frame cap ladder. OFF is first so the default reads as
                    // "nothing capped", matching the part-54 500-ceiling default.
                    static const int kCaps[] = { 0, 30, 60, 90, 120, 240, 480 };
                    constexpr int kNumCaps = int(sizeof kCaps / sizeof *kCaps);
                    int at = 0;
                    for (int i = 0; i < kNumCaps; ++i)
                        if (kCaps[i] == Settings_FpsCap())
                            at = i;
                    // Clamped like the resolution ladder — wrapping an ordered
                    // list is the same first-press surprise.
                    at += dir;
                    if (at < 0)
                        at = 0;
                    if (at >= kNumCaps)
                        at = kNumCaps - 1;
                    const int cap = kCaps[at];
                    Settings_SetFpsCap(cap);
                    Vd_SetFpsCapLive(cap);   // applies within one pump tick
                    break;
                }
                case 5:
                {
                    // FIELD OF VIEW (part 61): degrees of adjustment, -10..+30,
                    // one degree per press, clamped at the ends (ordered ladder —
                    // gotcha 377). Applies LIVE: the renderer re-reads the value
                    // once per frame and patches recognized scene projections.
                    int fov = Settings_Fov() + dir;
                    if (fov < -10)
                        fov = -10;
                    if (fov > 30)
                        fov = 30;
                    Settings_SetFov(fov);
                    fprintf(stderr, "[pcopt] fov %+d — live\n", fov);
                    break;
                }
                // (the MOUSE CAMERA toggle that sat between FOV and SENS is
                // retired — the mouse camera is always on, operator instruction)
                case 6:
                {
                    // 1..10, clamped like every ordered ladder here (gotcha 377).
                    int sv = Settings_MouseSens() + dir;
                    if (sv < 1)
                        sv = 1;
                    if (sv > 10)
                        sv = 10;
                    Settings_SetMouseSens(sv);
                    fprintf(stderr, "[pcopt] mouse sensitivity %d — live\n", sv);
                    break;
                }
            }
        };
        int sel = Settings_OverlaySelection();
        if (pressed & (kUp | kDown))
        {
            sel = (sel + ((pressed & kDown) ? 1 : 6)) % 7;
            Settings_SetOverlaySelection(sel);
        }
        else if (pressed & (kLeft | kRight))
            applyRow(sel, (pressed & kRight) ? 1 : -1);
        else if (pressed & kA)
            applyRow(sel, 1);   // A steps the selected value forward, console-style
        else if (pressed & kX)
        {
            // X APPLIES the pending resolution (part 91): persist it (every setter
            // saves, so a crash after this loses nothing) and hand it to the
            // renderer's frame-boundary live-apply seam. A press with nothing
            // pending says so instead of doing nothing silently.
            uint32_t pw = 0, ph = 0;
            Settings_PendingInternalRes(pw, ph);
            uint32_t cw = 0, ch = 0;
            Settings_InternalRes(cw, ch);
            if (pw && (pw != cw || ph != ch) && Settings_ValidInternalRes(pw, ph))
            {
                Settings_SetInternalRes(pw, ph);
                VkRenderer_RequestInternalRes(pw, ph);
                Settings_SetPendingInternalRes(0, 0);
                fprintf(stderr, "[pcopt] resolution %ux%u APPLIED LIVE (and saved)\n",
                        pw, ph);
            }
            else
                fprintf(stderr, "[pcopt] X: nothing pending to apply\n");
        }
        else if (pressed & kB)
        {
            // B closes. The hub underneath never saw any of this input; it
            // resumes untouched — and an unapplied pending resolution is
            // discarded by SetOverlayVisible itself.
            Settings_SetOverlayVisible(false);
            fprintf(stderr, "[pcopt] settings panel closed\n");
        }
        return;
    }

    if (!g_pcScreenOpen.load(std::memory_order_acquire))
        return;
    const uint32_t mgr = g_manager.load(std::memory_order_acquire);
    if (!mgr)
        return;

    if (!g_setupDone)
    {
        if (LoadU32(base, mgr + 0x178) != 0)
            return;                         // transition still in flight
        const uint32_t state = g_pcState.load(std::memory_order_acquire);
        if (!state)
            return;                         // TrySetup has not seen the state object
        const uint32_t wantVtbl = g_rootVtbl.load(std::memory_order_acquire);
        const uint32_t root = LoadU32(base, state + 4);
        if (root < 0x10000 || root >= 0xC0000000 ||
            LoadU32(base, root) != wantVtbl)
        {
            g_pcState.store(0, std::memory_order_release);
            return;
        }
        for (int g = 0; g < 4; ++g)
        {
            g_rowWidget[g] = FindChildWidget(ctx, base, root, kGroups[g].hash);
            if (!g_rowWidget[g])
            {
                if (TraceOn())
                    fprintf(stderr, "[pcopt-trace] pump: no %s child yet\n",
                            kGroups[g].name);
                return;                     // rows not built yet; retry next poll
            }
        }
        for (int g = 0; g < 4; ++g)
            SpinSetIndex(ctx, base, g_rowWidget[g], WantIndex(g),
                         kGroups[g].name);
        g_selectedRow = 0;
        SetRowState(ctx, base, g_rowWidget[0], 1);      // StateFocus on row 0
        g_setupDone = true;
        g_prevButtons = buttons;            // swallow whatever opened the screen
        fprintf(stderr, "[pcopt] PC options screen is up: host-driven input "
                        "active (D-pad + B), spins synced\n");
        return;
    }

    // ---- the screen's input handling, host-driven ----
    const uint32_t pressed = buttons & ~g_prevButtons;
    g_prevButtons = buttons;
    if (!pressed)
        return;

    constexpr uint32_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004,
                       kRight = 0x0008, kB = 0x2000;
    if (pressed & (kUp | kDown))
    {
        const int next = (g_selectedRow + ((pressed & kDown) ? 1 : 3)) % 4;
        SetRowState(ctx, base, g_rowWidget[g_selectedRow], 0);    // StateNormal
        SetRowState(ctx, base, g_rowWidget[next], 1);             // StateFocus
        g_selectedRow = next;
    }
    else if (pressed & (kLeft | kRight))
    {
        const uint32_t w = g_rowWidget[g_selectedRow];
        SpinRelay(ctx, base, w, (pressed & kRight) ? kNext : kPrev);
        const uint32_t idx = SpinIndex(base, w);
        fprintf(stderr, "[pcopt] %s -> index %u\n",
                kGroups[g_selectedRow].name, idx);
        ApplySetting(g_selectedRow, idx);
    }
    else if (pressed & kB)
    {
        // Leave to the hub, through the same transition call everything else
        // uses. The pending-hash global is kept in step exactly as the native
        // ACT path keeps it (0x825007C0).
        uint32_t be = __builtin_bswap32(kOptionsHub);
        memcpy(base + 0x82A59384, &be, 4);
        ctx.r3.u64 = mgr;
        ctx.r4.u64 = kOptionsHub;
        ctx.r5.u64 = 0;
        __imp__sub_827F6D40(ctx, base);
        // Calling the __imp__ body directly bypasses the transition hook that
        // normally tracks the open state, so close it out by hand — otherwise
        // this driver would keep eating the D-pad on the hub.
        g_pcScreenOpen.store(false, std::memory_order_release);
        g_setupDone = false;
        g_pcState.store(0, std::memory_order_release);
        memset(g_rowWidget, 0, sizeof(g_rowWidget));
        fprintf(stderr, "[pcopt] B: leaving to the options hub\n");
    }
}


// The options hub's enter — the function whose body hides the OptionsPC row
// (0x824D6708). Hooked READ-ONLY to learn the widget-root class vtable from a
// known-good instance: r3 is the hub's state object, *(r3+4) its widget root.
PPC_FUNC(sub_824D6600)
{
    if (!Disabled())
    {
        const uint32_t root = LoadU32(base, ctx.r3.u32 + 4);
        if (root >= 0x10000 && root < 0xC0000000)
        {
            const uint32_t vt = LoadU32(base, root);
            if (vt >= 0x82000000 && vt < 0x82150000)
                g_rootVtbl.store(vt, std::memory_order_release);
        }
    }
    __imp__sub_824D6600(ctx, base);
}
