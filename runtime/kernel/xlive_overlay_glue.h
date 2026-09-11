// The XenonLive in-game overlay (XenonLive_Launcher/overlay): friends,
// invitations and notifications drawn over the game, toggled with Shift+Tab
// or Back+Start. This is the port's seam to it, so that window.cpp, the
// renderer and xlive_glue.cpp each make one call and none of them includes
// the library. Built when CZ_XLIVE_OVERLAY is on (the default when the
// launcher checkout sits beside this one); otherwise every call is a no-op
// and the game is exactly what it was.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

union SDL_Event;
namespace xlive { class Client; struct Event; }

// -- main thread --------------------------------------------------------------
// Every SDL event. True when the event was the overlay's own toggle and must
// not reach the game.
bool CwOverlay_QueueSdlEvent(const SDL_Event& e);
void CwOverlay_SetWindowSize(int w, int h);
// The overlay owns keyboard, mouse and pad while it is open.
bool CwOverlay_Open();

// -- the library's worker ----------------------------------------------------
void CwOverlay_SetClient(xlive::Client* client);
void CwOverlay_OnEvent(const xlive::Event& event);

// -- the render thread ---------------------------------------------------------
struct CwOverlayVulkan
{
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t queueFamily;
    VkQueue queue;
    uint32_t imageCount;
    VkFormat format;
};
// Draws onto the swapchain image, which must be in TRANSFER_DST and is left
// there. `generation` changes with every swapchain rebuild.
bool CwOverlay_Render(const CwOverlayVulkan& vk, VkCommandBuffer cmd, VkImage image,
                      uint32_t width, uint32_t height, uint64_t generation);
// Before the device is destroyed.
void CwOverlay_Shutdown();
