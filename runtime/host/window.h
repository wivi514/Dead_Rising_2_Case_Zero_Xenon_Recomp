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

// Host-rendered replacement for the retail build's missing blue debug-menu layer.
// The labels still come from the genuine guest cDebugMenu nodes.
void Host_DebugMenuSetItems(const std::vector<std::string>& items);
void Host_DebugMenuSetVisible(bool visible);
bool Host_DebugMenuConsumeAction(uint32_t& itemIndex, int32_t& direction);
