// The persistent-settings store. See settings.h for why it exists and the
// env-wins rule its consumers enforce.

#include "settings.h"
#include "window.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{

struct State
{
    CzDisplayMode displayMode = CzDisplayMode::Windowed;
    uint32_t resW = 1280, resH = 720;   // the INTERNAL resolution (primary since rev 3)
    uint32_t renderScale = 1;           // legacy mirror, kept in sync for old readers
    bool vsync = false;         // false = MAILBOX (the part-54 default), true = FIFO
    int shadowTier = 2;         // the title rendered at full shadow resolution until now
    int fpsCap = 0;             // 0 = OFF, i.e. the part-54 500-ceiling that never binds
    int fov = 0;                // degrees of fov adjustment, -10..+30; 0 = OG (part 61)
    int aspect = 0;             // 0 = 16:9 (the title's own), 1 = 21:9 (part 60 wide mode)
    int rtShadows = 0;          // 0 = none (default), 1/2/3 = RT LOW/MED/HIGH.
                                // Non-zero REPLACES the raster cascade (part 64,
                                // operator's spec).
};

// The frame-cap values the panel offers. A set rather than a range because the vblank
// period only lands exactly on these (docs/part60-night-plan.md item 5), and a value
// written by hand into the file that is not one of them is clamped to OFF loudly.
bool ValidFpsCap(long v)
{
    return v == 0 || v == 30 || v == 60 || v == 90 || v == 120 || v == 240 || v == 480;
}

std::mutex g_mutex;
State g_state;
std::string g_path;

// -1 = nothing pending. Written by Settings_SetDisplayMode from whichever guest
// thread runs the menu verb, consumed by the window thread's loop.
std::atomic<int> g_pendingDisplayMode{ -1 };

void SaveLocked()
{
    if (g_path.empty())
        return;
    FILE* f = fopen(g_path.c_str(), "w");
    if (!f)
    {
        // Loudly: a menu whose changes silently do not survive a restart is the
        // gamma slider all over again.
        fprintf(stderr, "[settings] COULD NOT WRITE %s — changes will not survive "
                        "a restart\n", g_path.c_str());
        return;
    }
    fprintf(f,
            "# Dead Rising 2: Case Zero recomp — graphics settings.\n"
            "# Written by the in-game PC Settings screen; safe to edit by hand.\n"
            "# Env vars (CZ_VK_RES, CZ_VK_SWAPCHAIN_FIFO, ...) always win over this file.\n"
            "display_mode=%d\n"     // 0 windowed, 1 borderless, 2 fullscreen
            "res_w=%u\n"            // internal render resolution (primary)
            "res_h=%u\n"
            "render_scale=%u\n"     // legacy mirror: round(res_h/720), for old builds
            "vsync=%d\n"
            "shadow_tier=%d\n"     // 0 low, 1 medium, 2 high
            "fps_cap=%d\n"         // 0 = off, else 30/60/90/120/240/480
            "fov=%d\n"             // field-of-view adjustment in degrees, -10..+30, 0 = OG
            "aspect=%d\n"          // 0 = 16:9, 1 = 21:9 (applies at next launch)
            "rt_shadows=%d\n",     // 0 = OG, 1 = RT LOW (needs a ray-query device)
            int(g_state.displayMode), g_state.resW, g_state.resH, g_state.renderScale,
            g_state.vsync ? 1 : 0, g_state.shadowTier, g_state.fpsCap, g_state.fov,
            g_state.aspect, g_state.rtShadows);
    fclose(f);
}

} // namespace

void Settings_Load(const std::string& path)
{
    // CZ_TEST_PANEL=1 — open the settings panel immediately, headlessly: the
    // presentation repro for "the row shows 720p at open while the store says
    // otherwise". Display-only; no input needed.
    if (getenv("CZ_TEST_PANEL"))
        Settings_SetOverlayVisible(true);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
    {
        fprintf(stderr, "[settings] no %s yet — using defaults (windowed, 1280x720, "
                        "vsync off, shadow high)\n", path.c_str());
        return;
    }
    char line[256];
    bool sawResWH = false;
    uint32_t legacyScale = 0;
    int legacyAspect = -1;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#')
            continue;
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char* key = line;
        const long v = strtol(eq + 1, nullptr, 10);
        if (!strcmp(key, "display_mode") && v >= 0 && v <= 2)
            g_state.displayMode = CzDisplayMode(v);
        else if (!strcmp(key, "res_w") && v > 0)
        {
            g_state.resW = uint32_t(v);
            sawResWH = true;
        }
        else if (!strcmp(key, "res_h") && v > 0)
        {
            g_state.resH = uint32_t(v);
            sawResWH = true;
        }
        else if (!strcmp(key, "render_scale") && v >= 1 && v <= 4)
            legacyScale = uint32_t(v);
        else if (!strcmp(key, "vsync"))
            g_state.vsync = v != 0;
        else if (!strcmp(key, "shadow_tier") && v >= 0 && v <= 2)
            g_state.shadowTier = int(v);
        else if (!strcmp(key, "rt_shadows") && v >= 0 && v <= 1)
            g_state.rtShadows = int(v);
        else if (!strcmp(key, "aspect") && v >= 0 && v <= 1)
            legacyAspect = int(v);
        else if (!strcmp(key, "fps_cap"))
        {
            if (ValidFpsCap(v))
                g_state.fpsCap = int(v);
            else
                fprintf(stderr, "[settings] fps_cap=%ld is not one of "
                                "0/30/60/90/120/240/480 — using OFF\n", v);
        }
        else if (!strcmp(key, "fov"))
        {
            if (v >= -10 && v <= 30)
                g_state.fov = int(v);
            else
                fprintf(stderr, "[settings] fov=%ld is outside -10..+30 — "
                                "using OG (0)\n", v);
        }
        // Unknown keys: ignored on purpose — see the header comment.
    }
    fclose(f);
    // A file from before revision 3 has no res_w/res_h: convert its scale+aspect
    // pair. The wide width uses the exact-21:9 42/32 factor the old build meant —
    // the display-derived width takes over the next time the menu writes.
    if (!sawResWH && legacyScale)
    {
        g_state.resH = 720 * legacyScale;
        g_state.resW = legacyAspect == 1 ? (1280 * legacyScale * 42) / 32
                                         : 1280 * legacyScale;
        fprintf(stderr, "[settings] legacy render_scale=%u aspect=%d converted to "
                        "%ux%u\n", legacyScale, legacyAspect < 0 ? 0 : legacyAspect,
                g_state.resW, g_state.resH);
    }
    if (!Settings_ValidInternalRes(g_state.resW, g_state.resH))
    {
        fprintf(stderr, "[settings] res %ux%u is not one this renderer can produce — "
                        "using 1280x720\n", g_state.resW, g_state.resH);
        g_state.resW = 1280;
        g_state.resH = 720;
    }
    // Keep the legacy mirrors coherent for old readers of the struct.
    g_state.renderScale = (g_state.resH + 360) / 720;
    if (g_state.renderScale < 1) g_state.renderScale = 1;
    if (g_state.renderScale > 4) g_state.renderScale = 4;
    g_state.aspect = uint64_t(g_state.resW) * 9 > uint64_t(g_state.resH) * 16 ? 1 : 0;
    fprintf(stderr, "[settings] %s: display_mode=%d res=%ux%u vsync=%d "
                    "shadow_tier=%d fps_cap=%d fov=%d\n", path.c_str(),
            int(g_state.displayMode), g_state.resW, g_state.resH,
            g_state.vsync ? 1 : 0, g_state.shadowTier, g_state.fpsCap, g_state.fov);
}

void Settings_Save()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    SaveLocked();
}

CzDisplayMode Settings_DisplayMode()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.displayMode;
}

uint32_t Settings_RenderScale()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.renderScale;
}

bool Settings_VSync()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.vsync;
}

int Settings_ShadowTier()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.shadowTier;
}

int Settings_FpsCap()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.fpsCap;
}

int Settings_Aspect()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.aspect;
}

int Settings_Fov()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.fov;
}

// The panel's single SHADOW row. 0..2 are the raster tiers (RT off); 3..5 select
// RT LOW/MEDIUM/HIGH, which REPLACE the raster cascade rather than adding to it.
// The raster tier is REMEMBERED while an RT value is selected, so stepping back
// down the row returns the quality the player had rather than resetting it.
int Settings_ShadowRow()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.rtShadows ? 2 + g_state.rtShadows : g_state.shadowTier;
}

void Settings_SetShadowRow(int row)
{
    if (row < 0 || row > 5)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (row < 3)
        {
            g_state.rtShadows = 0;
            g_state.shadowTier = row;
        }
        else
            g_state.rtShadows = row - 2;
        SaveLocked();
    }
}

int Settings_RtShadows()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.rtShadows;
}

void Settings_SetRtShadows(int tier)
{
    if (tier < 0 || tier > 3)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.rtShadows = tier;
    SaveLocked();
    // Applied LIVE: rtshadow::TierThisFrame re-reads it per frame, vk_renderer.cpp.
}

void Settings_SetFov(int deg)
{
    if (deg < -10 || deg > 30)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.fov = deg;
    SaveLocked();
    // Applied LIVE: the renderer re-reads the value once per frame (the shadow-tier
    // pattern) in FovHalfRadThisFrame, vk_renderer.cpp.
}

// See settings.h for the rule. The caps: 2880 tall / 6880 wide is 4x the title's
// frame in each direction, the same ceiling the integer path had.
bool Settings_ValidInternalRes(uint32_t w, uint32_t h)
{
    return h >= 720 && h <= 2880 && (w & 1) == 0 && w >= 1280 && w <= 6880 &&
           uint64_t(w) * 9 >= uint64_t(h) * 16;
}

void Settings_InternalRes(uint32_t& w, uint32_t& h)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    w = g_state.resW;
    h = g_state.resH;
}

void Settings_SetInternalRes(uint32_t w, uint32_t h)
{
    if (!Settings_ValidInternalRes(w, h))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.resW = w;
    g_state.resH = h;
    g_state.renderScale = std::min(4u, std::max(1u, (h + 360) / 720));
    g_state.aspect = uint64_t(w) * 9 > uint64_t(h) * 16 ? 1 : 0;
    SaveLocked();
}


void Settings_SetDisplayMode(CzDisplayMode m)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state.displayMode = m;
        SaveLocked();
    }
    g_pendingDisplayMode.store(int(m), std::memory_order_release);
}

void Settings_SetRenderScale(uint32_t s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (s < 1 || s > 4)
        return;
    g_state.renderScale = s;
    // Legacy path (the parked guest-screen arm): keep the primary in step.
    g_state.resH = 720 * s;
    g_state.resW = g_state.aspect ? (1280 * s * 42) / 32 : 1280 * s;
    SaveLocked();
    // Live apply happens through VkRenderer_RequestRenderScale, called by whoever
    // changed the setting — the renderer swaps its scale-dependent resources at
    // the next frame boundary (part 60).
}

void Settings_SetVSync(bool on)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.vsync = on;
    SaveLocked();
}

void Settings_SetShadowTier(int tier)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (tier < 0 || tier > 2)
        return;
    g_state.shadowTier = tier;
    SaveLocked();
}

void Settings_SetAspect(int aspect)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (aspect < 0 || aspect > 1)
        return;
    g_state.aspect = aspect;
    SaveLocked();
    // Applies at the NEXT LAUNCH: the wide mode reshapes every render-pipeline
    // surface, and the renderer latches it once at boot (WideMode in
    // vk_renderer.cpp) the way the render scale is latched.
}

void Settings_SetFpsCap(int fps)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ValidFpsCap(fps))
        return;
    g_state.fpsCap = fps;
    SaveLocked();
    // Live apply goes through Vd_SetFpsCapLive, called by the panel handler that
    // changed the setting — the store stays appliance-free like every other row.
}

int Settings_ConsumePendingDisplayMode()
{
    return g_pendingDisplayMode.exchange(-1, std::memory_order_acq_rel);
}

namespace
{
std::atomic<bool> g_overlayVisible{ false };
std::atomic<int> g_overlaySelection{ 0 };
// One word so a torn W/H pair cannot exist between the pump and the drawer.
std::atomic<uint64_t> g_pendingRes{ 0 };
}

bool Settings_OverlayVisible()
{
    return g_overlayVisible.load(std::memory_order_acquire);
}

void Settings_SetOverlayVisible(bool on)
{
    g_overlayVisible.store(on, std::memory_order_release);
    if (on)
        g_overlaySelection.store(0, std::memory_order_release);
    // Opening OR closing the panel discards an unapplied resolution — the row must
    // never come up showing a stale pending from a previous visit, and leaving
    // without X means "keep what I have" (part 91, the operator's apply-button spec).
    g_pendingRes.store(0, std::memory_order_release);
}

void Settings_PendingInternalRes(uint32_t& w, uint32_t& h)
{
    const uint64_t v = g_pendingRes.load(std::memory_order_acquire);
    w = uint32_t(v >> 32);
    h = uint32_t(v);
}

void Settings_SetPendingInternalRes(uint32_t w, uint32_t h)
{
    g_pendingRes.store(w && h ? (uint64_t(w) << 32) | h : 0,
                       std::memory_order_release);
}

int Settings_OverlaySelection()
{
    return g_overlaySelection.load(std::memory_order_acquire);
}

void Settings_SetOverlaySelection(int row)
{
    g_overlaySelection.store(row, std::memory_order_release);
}
