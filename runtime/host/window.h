// The host seam: one SDL window, one event loop, one present, one pad.
//
// WHY THIS IS ONE MODULE AND NOT THREE
// ------------------------------------
// A window, a present and an input device look like three separate features, and in
// SDL they are one thread. SDL's video subsystem must be initialised, pumped and
// presented from a single thread — the one that created the window — and the input
// events arrive on that same pump. Splitting them across files would mean either
// three thread-affinity comments that have to agree, or a cross-thread call that
// works on X11 and stops working the day someone runs it elsewhere. So the rule
// lives in one place: **everything here except Host_Present and Host_PadState runs on
// the thread that called Host_WindowInit, which is the process's main thread.**
//
// The two exceptions are the seams to the rest of the runtime, and both are
// deliberately trivial:
//   * Host_Present is called from the PM4 executor (the vblank pump thread) and only
//     publishes a frame descriptor. It never touches SDL.
//   * Host_PadState is called from whatever guest thread polls XamInputGetState and
//     only reads the snapshot the event loop published. It never touches SDL either.
//
// WHAT PHASE 3 IS AND IS NOT
// --------------------------
// A blank window is the CORRECT result of this phase. There is no renderer yet
// (phase 5): the command processor parses draws and never rasterises one, so the
// front buffer this module is handed contains whatever the guest's allocator left
// there. Presenting it as a texture would show noise and invite someone to debug a
// renderer that does not exist. So the window clears to a flat colour and puts the
// live frame count and rate in its title bar, which is the honest signal that the
// present seam is running at the guest's own swap rate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Bring up the window, the renderer and the game-controller subsystem.
//
// Returns false when the runtime is deliberately headless (CZ_NO_WINDOW=1) or when
// SDL cannot open a display. Both are LOUD: the caller keeps running without a
// window, because every gate this port owns is a log diff and none of them needs
// pixels, but a run that quietly lost its window would look like a run whose input
// stopped working.
bool Host_WindowInit();

// True once Host_WindowInit has succeeded. The input path asks this rather than
// assuming, because "no window" and "window with nothing pressed" are different
// claims to make to the guest.
bool Host_WindowActive();

// The present seam. Called from the PM4 executor when it reaches an XE_SWAP packet
// — i.e. exactly once per guest frame — with the front buffer's guest address and
// dimensions as VdSwap wrote them.
//
// NON-BLOCKING BY CONSTRUCTION. On console VdSwap does not wait for the scanout; the
// ring's own flow control paces the title, and this runtime already reproduces that
// (findings 38-39). Blocking here would insert a frame pacer the guest never asked
// for and would silently become the thing that limits the frame rate.
void Host_Present(uint32_t frontBuffer, uint32_t width, uint32_t height);

// Phase 5's other half of the present seam: the rendered frame itself, as tightly
// packed RGBA8 rows.
//
// Host_Present says WHEN a frame happened and what the guest called it; this says what
// it looks like. They stay separate because the frame clock has to keep working with
// no renderer — CZ_VKDRAW off is the control arm for every claim phase 5 makes, and in
// that arm the window still counts frames at the guest's own swap rate.
//
// Copies the pixels. Called from the PM4 executor's thread, which must not be made to
// wait on the window's, and the buffer it is handed belongs to the renderer's next
// frame the moment this returns.
void Host_PresentPixels(const uint8_t* rgba, uint32_t width, uint32_t height);

// The window's event loop. Runs on the calling (main) thread until the window is
// closed, then terminates the process. Returns immediately if there is no window.
void Host_WindowRun();

// Ask the loop to shut the process down. Called when the guest entry point returns —
// without it, a title that exits leaves a live window in front of a process with no
// guest in it, which is indistinguishable from a hang.
void Host_RequestQuit(const char* why);

// ASPECT-CORRECT PRESENTATION (part 60). The fitted rectangle for presenting a
// srcW x srcH frame inside a dstW x dstH window without changing its shape: the
// largest centered rectangle with the source's aspect ratio that fits. A 16:9
// frame on a 21:9 display gets side bars; a 21:9 frame on a 16:9 display gets
// top/bottom bars. This is the ONE computation both present paths share — the
// Vulkan swapchain blit and the SDL readback copy — because two copies of an
// aspect division are how the two arms drift into showing different pictures.
// `CZ_VK_STRETCH=1` is the control arm (each caller checks it); stretch was the
// only behavior before part 60.
//
// Inline and header-only on purpose: it is called once per presented frame from
// two modules and pure arithmetic, and 64-bit intermediates keep 5120x2880-scale
// products out of overflow.
inline void Host_AspectFitRect(uint32_t srcW, uint32_t srcH, uint32_t dstW,
                               uint32_t dstH, int32_t& x, int32_t& y, uint32_t& w,
                               uint32_t& h)
{
    if (!srcW || !srcH || !dstW || !dstH)
    {
        x = y = 0;
        w = dstW;
        h = dstH;
        return;
    }
    if (uint64_t(srcW) * dstH >= uint64_t(dstW) * srcH)
    {
        // Source is at least as wide as the window, proportionally: full width,
        // letterbox (top/bottom bars).
        w = dstW;
        h = uint32_t(uint64_t(dstW) * srcH / srcW);
    }
    else
    {
        // Window is wider than the source: full height, pillarbox (side bars).
        h = dstH;
        w = uint32_t(uint64_t(dstH) * srcW / srcH);
    }
    if (!w) w = 1;
    if (!h) h = 1;
    x = int32_t(dstW - w) / 2;
    y = int32_t(dstH - h) / 2;
}

// One XInput-shaped pad state, in XInput's units and sign conventions (NOT SDL's —
// see the axis note in window.cpp).
struct HostPadState
{
    uint32_t packet;  // changes only when the state below changes
    uint16_t buttons; // XINPUT_GAMEPAD_* bits
    uint8_t leftTrigger, rightTrigger;
    int16_t thumbLX, thumbLY, thumbRX, thumbRY;
};

// The pad state the guest should see. Pad 0 is the physical SDL controller; pad 1 is
// the keyboard. False when there is no window or the index is not one of those two.
bool Host_PadState(uint32_t userIndex, HostPadState& out);

// One-shot F2 edge used by the title's explicit DebugJump bridge. Consuming the
// edge keeps the frontend request independent of the guest's controller polling
// rate and prevents a held key from requesting the screen every frame.
bool Host_ConsumeDebugJumpPressed();
bool Host_ConsumeDebugEnterPressed();
bool Host_ConsumeDebugMenuPressed();

// The same three edges, requested WITHOUT a keyboard — what `CZ_FAKE_PRESS_SEQ`'s F2/F3/F4
// entries call. A headless run has no SDL keyboard, so before this the title's own
// DebugJump screen was reachable only by a human at a window, which made the one route to
// the OUTDOOR world operator-only (gotcha 190). The flags are plain atomics and their
// consumer runs on the guest thread inside `XamInputGetState`, so neither end needs a
// window; that is why these live outside the CZ_HAVE_SDL split rather than beside the
// keyboard that used to be their only source.
void Host_RequestDebugJump();
void Host_RequestDebugEnter();
void Host_RequestDebugMenu();

// F9 — dump every resolve snapshot of the NEXT frame, on demand, into
// `CZ_VK_SNAP_DUMP`'s directory. The renderer consumes the edge at present time.
//
// The fixed `CZ_VK_SNAP_FRAME` trigger it joins is right for a boot-time question and
// useless for a question about a PLACE: the operator has to predict, before the run
// starts, which frame number they will be standing on the defect at. This inverts that —
// the person who can see the spot presses the key.
void Host_RequestSnapDump();
bool Host_ConsumeSnapDumpPressed();

// F8 — record EVERY presented frame for about a second, into CZ_BURST_DUMP's directory,
// with a manifest carrying each frame's draw count and fingerprints. For a defect that
// FLICKERS, which no single screenshot can show. See its definition in window.cpp.
void Host_RequestBurstDump();
bool Host_ConsumeBurstDumpPressed();

// ===================================================================================
// THE VULKAN SWAPCHAIN SEAM — CZ_VK_SWAPCHAIN=1 (part 54, plan §7)
// ===================================================================================
// Everything above presents by COPYING: the renderer reads its colour target back into
// host memory, `Host_PresentPixels` copies that into the window's back buffer, and the
// event loop uploads it again with `SDL_UpdateTexture`. Three full frames of traffic and
// a GPU->CPU->GPU round trip, per presented frame, for pixels that never left the card
// in the first place.
//
// That was the right trade while the guest ran at 30 fps into a 1280x720 target — 3.5 MB
// a frame is nothing, and it kept Vulkan off the window's thread, which is the separation
// phase 3 was built around. Part 53's internal resolution knob changed the arithmetic:
// the copy is the scale SQUARED, so at `CZ_VK_RES=2560x1440` it is 14.1 MB a frame, and
// MEASURED (part 54, windowed, ~3,700 draws) it is:
//
//     1280x720    readback 8.1-8.7% of the frame     ~0.65 ms
//     2560x1440   readback 16.4-17.9%                ~1.7-2.2 ms
//
// — the largest single non-draw phase at 2x, and the only cost in this renderer that
// grows when the operator raises the resolution.
//
// So this seam exists to let the renderer present the image it already has, in place.
// It is an ARM, not a replacement: without `CZ_VK_SWAPCHAIN` not one line of it runs and
// the copy path above is untouched, which is what makes the two same-binary arms of one
// A/B rather than a rewrite that has to be trusted.
//
// THE FLAG HAS TO BE DECIDED AT WINDOW-CREATION TIME. `SDL_WINDOW_VULKAN` cannot be added
// to a window that already exists, and a window carrying it cannot also carry an
// `SDL_Renderer` — so the choice is made once, in Host_WindowInit, before the guest
// starts. The renderer asks about it later, from the pump thread, through
// Host_VulkanSwapchainWanted().
bool Host_VulkanSwapchainWanted();

// The instance extensions SDL needs for a surface on this window (VK_KHR_surface plus the
// platform one). Empty when there is no Vulkan window, which is the honest answer for a
// headless run and makes the renderer's "no swapchain here" branch data-driven rather
// than a second env-var read.
std::vector<const char*> Host_VulkanInstanceExtensions();

// Create the surface for this window. `instance` and `outSurface` are `VkInstance` and
// `VkSurfaceKHR*` passed as void*/uintptr_t so that this header — which the whole runtime
// includes — does not have to pull in vulkan.h. Returns false and says why on failure.
bool Host_VulkanCreateSurface(void* instance, uint64_t* outSurface);

// The window's drawable size in pixels, which on a scaled display is NOT its logical
// size. The swapchain is created at this extent and re-created when it changes.
void Host_VulkanDrawableSize(uint32_t* w, uint32_t* h);

// The desktop size of the DISPLAY the window currently sits on (part 60): what the
// settings panel clamps its resolution list against, so the menu reads like a game's
// and not like a debug knob — no 5120x2880 entry on a 1440p monitor. Refreshed by the
// window thread once a second, which also covers a window dragged between monitors.
// Returns false (and zeros) when no window has published one — headless runs — and
// every consumer treats that as "no clamp" rather than refusing.
bool Host_DisplaySize(uint32_t* w, uint32_t* h);

// The display's usable MODE LIST: distinct WxH pairs the display reports AND the
// renderer can produce (Settings_ValidInternalRes), ascending, written as w,h pairs
// into `wh` (2*maxPairs u32 capacity). Returns the pair count; 0 when headless or
// not yet published — the caller falls back to a synthesized list.
int Host_DisplayModeList(uint32_t* wh, int maxPairs);

// The debug overlay, rasterised into an RGBA8 buffer for a caller that has no
// SDL_Renderer — i.e. the Vulkan swapchain present path, which is the default since part
// 54. Returns false when the menu is not open, in which case nothing is written.
//
// The buffer is a FIXED logical size (the out params say which) rather than the window's,
// because the caller scales it: rasterising a 3440x1368 overlay on the CPU every frame to
// draw a menu panel would be a real cost for a debug feature, and the glyphs are 5x7
// blocks that scale without looking any worse than they already do.
//
// It exists because making the swapchain the default would otherwise have DELETED the F4
// menu, and the one honest way to ship a default is that nothing is quietly lost with it.
// `rgba` is the PANEL ONLY, `width`/`height` are its size, and `x`/`y` are where it sits
// inside a `baseW` x `baseH` logical screen so the caller can scale the rectangle to a
// window of any size.
//
// IT IS THE PANEL AND NOT THE SCREEN, and that is the whole correctness of it: the caller
// composites with a BLIT, and a blit is a copy, not a blend. A full-screen overlay bitmap
// with transparent margins therefore overwrites everything around the panel with black —
// which is exactly what the first version of this did, hiding the entire game behind the
// menu. Handing back only the panel's rectangle means the copy touches only the pixels
// the menu actually occupies, and needs no blending to be correct.
bool Host_DebugOverlayRender(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                             uint32_t& x, uint32_t& y, uint32_t& baseW, uint32_t& baseH);

// Host-rendered replacement for the retail build's missing blue debug-menu layer.
// The labels still come from the genuine guest cDebugMenu nodes.
void Host_DebugMenuSetItems(const std::vector<std::string>& items);
void Host_DebugMenuSetVisible(bool visible);
bool Host_DebugMenuConsumeAction(uint32_t& itemIndex, int32_t& direction);
