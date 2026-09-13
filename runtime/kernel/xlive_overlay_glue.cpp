#include "xlive_overlay_glue.h"

#if CZ_HAVE_XLIVE_OVERLAY
#include <xlive_overlay/overlay.h>

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

// NO CLIENT, NO OVERLAY (Case West's aae9fca, ported the same evening). Since online
// became launcher-only an offline start returns from the xlive start BEFORE SetClient,
// and the overlay's Render returns false without a client — but its QueueSdlEvent still
// toggled `open` on Shift+Tab, so the player got an INVISIBLE overlay that owned the pad
// and the mouse: "I lost control of the game" on the AMD test machine, Case West's first
// v1.1.0 sitting. Before online went launcher-only libxlive started on every run, so
// the overlay always had a client and this could not happen. The seam is here rather
// than in the launcher repo: the glue knows whether it ever handed a client over.
static std::atomic<bool> g_haveClient{ false };

bool CwOverlay_QueueSdlEvent(const SDL_Event& e)
{
    if (!g_haveClient.load(std::memory_order_acquire))
    {
        // Say why, once, on the hotkey the player actually pressed.
        static bool said = false;
        if (!said && e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_TAB &&
            (e.key.keysym.mod & KMOD_SHIFT))
        {
            said = true;
            fprintf(stderr, "[overlay] Shift+Tab ignored: the XenonLive overlay has no client "
                            "— this game was not started through the XenonLive launcher "
                            "(offline default profile), so there is nothing to show\n");
        }
        return false;
    }
    return xlive_overlay::Overlay::Instance().QueueSdlEvent(e);
}
void CwOverlay_SetWindowSize(int w, int h)
{
    xlive_overlay::Overlay::Instance().SetWindowSize(w, h);
}
bool CwOverlay_Open()
{
    return g_haveClient.load(std::memory_order_acquire) && xlive_overlay::Overlay::Instance().open();
}
void CwOverlay_SyncTextInput()
{
    static bool on = false;
    const bool want = CwOverlay_Open() && xlive_overlay::Overlay::Instance().wants_text_input();
    if (want == on)
        return;
    on = want;
    if (want)
    {
        SDL_StartTextInput();
        static bool said = false;
        if (!said)
        {
            said = true;
            fprintf(stderr, "[overlay] a text box has focus: SDL text input ON for it (off again "
                            "when it loses focus)\n");
        }
    }
    else
        SDL_StopTextInput();
}
void CwOverlay_SetClient(xlive::Client* client, uint32_t titleId)
{
    xlive_overlay::Overlay::Instance().SetClient(client, titleId);
    g_haveClient.store(client != nullptr, std::memory_order_release);
    // CZ_XLIVE_OVERLAY_OPEN=1: start with the overlay open, so a headless run
    // with CW_VK_SWAPCHAIN_DUMP can photograph it without anyone pressing
    // Shift+Tab. A test arm, like every other switch in this runtime.
    if (const char* open = std::getenv("CZ_XLIVE_OVERLAY_OPEN"); open && open[0] == '1')
        if (!xlive_overlay::Overlay::Instance().open())
            xlive_overlay::Overlay::Instance().Toggle();
}
void CwOverlay_OnEvent(const xlive::Event& event)
{
    xlive_overlay::Overlay::Instance().OnEvent(event);
}
static bool OverlayOff()
{
    static const bool off = [] {
        const char* v = std::getenv("CZ_XLIVE_OVERLAY");
        return v && v[0] == '0';
    }();
    return off;
}
bool CwOverlay_Notify(const char* text, double seconds, const char* tag)
{
    if (OverlayOff())
        return false;
    xlive_overlay::Overlay::Instance().Notify(text, seconds, tag ? tag : "");
    return true;
}
void CwOverlay_Dismiss(const char* tag)
{
    if (!OverlayOff())
        xlive_overlay::Overlay::Instance().Dismiss(tag ? tag : "");
}
bool CwOverlay_Render(const CwOverlayVulkan& vk, VkCommandBuffer cmd, VkImage image,
                      uint32_t width, uint32_t height, uint64_t generation)
{
    // CZ_XLIVE_OVERLAY=0: the off switch, so a build with the overlay can still
    // run without it drawing or initialising anything.
    if (OverlayOff())
        return false;
    xlive_overlay::VulkanHandles h;
    h.instance = vk.instance;
    h.physical = vk.physical;
    h.device = vk.device;
    h.queue_family = vk.queueFamily;
    h.queue = vk.queue;
    h.image_count = vk.imageCount;
    h.color_format = vk.format;
    h.api_version = VK_API_VERSION_1_3;
    return xlive_overlay::Overlay::Instance().Render(h, cmd, image, width, height, generation);
}
void CwOverlay_Shutdown() { xlive_overlay::Overlay::Instance().Shutdown(); }

#else

bool CwOverlay_QueueSdlEvent(const SDL_Event&) { return false; }
void CwOverlay_SetWindowSize(int, int) {}
bool CwOverlay_Open() { return false; }
void CwOverlay_SyncTextInput() {}
void CwOverlay_SetClient(xlive::Client*, uint32_t) {}
void CwOverlay_OnEvent(const xlive::Event&) {}
bool CwOverlay_Notify(const char*, double, const char*) { return false; }
void CwOverlay_Dismiss(const char*) {}
bool CwOverlay_Render(const CwOverlayVulkan&, VkCommandBuffer, VkImage, uint32_t, uint32_t, uint64_t)
{
    return false;
}
void CwOverlay_Shutdown() {}

#endif
