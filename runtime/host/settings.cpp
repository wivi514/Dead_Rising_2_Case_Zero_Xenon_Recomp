// The persistent-settings store. See settings.h for why it exists and the
// env-wins rule its consumers enforce.

#include "settings.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{

struct State
{
    CzDisplayMode displayMode = CzDisplayMode::Windowed;
    uint32_t renderScale = 1;
    bool vsync = false;         // false = MAILBOX (the part-54 default), true = FIFO
    int shadowTier = 2;         // the title rendered at full shadow resolution until now
    int fpsCap = 0;             // 0 = OFF, i.e. the part-54 500-ceiling that never binds
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
            "render_scale=%u\n"     // 1..4 over 1280x720
            "vsync=%d\n"
            "shadow_tier=%d\n"     // 0 low, 1 medium, 2 high
            "fps_cap=%d\n",        // 0 = off, else 30/60/90/120/240/480
            int(g_state.displayMode), g_state.renderScale, g_state.vsync ? 1 : 0,
            g_state.shadowTier, g_state.fpsCap);
    fclose(f);
}

} // namespace

void Settings_Load(const std::string& path)
{
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
        else if (!strcmp(key, "render_scale") && v >= 1 && v <= 4)
            g_state.renderScale = uint32_t(v);
        else if (!strcmp(key, "vsync"))
            g_state.vsync = v != 0;
        else if (!strcmp(key, "shadow_tier") && v >= 0 && v <= 2)
            g_state.shadowTier = int(v);
        else if (!strcmp(key, "fps_cap"))
        {
            if (ValidFpsCap(v))
                g_state.fpsCap = int(v);
            else
                fprintf(stderr, "[settings] fps_cap=%ld is not one of "
                                "0/30/60/90/120/240/480 — using OFF\n", v);
        }
        // Unknown keys: ignored on purpose — see the header comment.
    }
    fclose(f);
    fprintf(stderr, "[settings] %s: display_mode=%d render_scale=%u vsync=%d "
                    "shadow_tier=%d fps_cap=%d\n", path.c_str(),
            int(g_state.displayMode), g_state.renderScale, g_state.vsync ? 1 : 0,
            g_state.shadowTier, g_state.fpsCap);
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
}

int Settings_OverlaySelection()
{
    return g_overlaySelection.load(std::memory_order_acquire);
}

void Settings_SetOverlaySelection(int row)
{
    g_overlaySelection.store(row, std::memory_order_release);
}
