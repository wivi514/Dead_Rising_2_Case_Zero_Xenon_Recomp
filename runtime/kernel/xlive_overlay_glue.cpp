#include "xlive_overlay_glue.h"

#if CZ_HAVE_XLIVE_OVERLAY
#include <xlive_overlay/overlay.h>

#include <cstdlib>

bool CwOverlay_QueueSdlEvent(const SDL_Event& e)
{
    return xlive_overlay::Overlay::Instance().QueueSdlEvent(e);
}
void CwOverlay_SetWindowSize(int w, int h)
{
    xlive_overlay::Overlay::Instance().SetWindowSize(w, h);
}
bool CwOverlay_Open() { return xlive_overlay::Overlay::Instance().open(); }
void CwOverlay_SetClient(xlive::Client* client, uint32_t titleId)
{
    xlive_overlay::Overlay::Instance().SetClient(client, titleId);
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
bool CwOverlay_Render(const CwOverlayVulkan& vk, VkCommandBuffer cmd, VkImage image,
                      uint32_t width, uint32_t height, uint64_t generation)
{
    // CZ_XLIVE_OVERLAY=0: the off switch, so a build with the overlay can still
    // run without it drawing or initialising anything.
    static const bool off = [] {
        const char* v = std::getenv("CZ_XLIVE_OVERLAY");
        return v && v[0] == '0';
    }();
    if (off)
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
void CwOverlay_SetClient(xlive::Client*, uint32_t) {}
void CwOverlay_OnEvent(const xlive::Event&) {}
bool CwOverlay_Render(const CwOverlayVulkan&, VkCommandBuffer, VkImage, uint32_t, uint32_t, uint64_t)
{
    return false;
}
void CwOverlay_Shutdown() {}

#endif
