// Persistent host graphics settings — the store behind the resurrected PC options
// screen (part 60).
//
// WHY THIS EXISTS. The title SHIPS a complete PC graphics menu (options_pc.txt in
// data/frontend/fecmn.big — Resolution, DisplayMode, VSync, Shadow and more, with
// every localization string present in str_en.bcs) but the 360 XEX compiled out the
// screen's verb handlers, so the settings the menu names have to live HERE and be
// applied by the host. This module is only the STORE: one struct, one file, one
// mutex. The appliers live where the state they touch lives — window.cpp polls the
// display mode on its own thread (SDL rule, gotcha 99), vk_renderer.cpp consults the
// render scale at init and the present-mode chooser reads vsync.
//
// ENV VARS WIN OVER THE FILE, always. Every one of these settings shadows an existing
// measurement arm (CZ_VK_RES, CZ_VK_SWAPCHAIN_FIFO, CZ_WINDOW_SIZE), and an A/B arm
// that a menu file can silently override is an A/B that cannot be trusted
// (docs/part60-kickoff.md §5). The precedence is enforced at each CONSUMER — the
// consumer knows its own env var — and each consumer logs when it ignores the file.
//
// The file is deliberately human-readable key=value text so an operator can inspect
// or fix it without a tool, and unknown keys are preserved-by-ignoring rather than
// deleted: a settings file written by a NEWER build must survive an older one.
#pragma once

#include <cstdint>
#include <string>

enum class CzDisplayMode : int
{
    Windowed = 0,     // what every run before part 60 was
    Borderless = 1,   // SDL_WINDOW_FULLSCREEN_DESKTOP ("windowed fullscreen")
    Fullscreen = 2,   // SDL_WINDOW_FULLSCREEN, at the desktop display mode
};

// Load the settings file (missing file = defaults, not an error) and remember the
// path for every later Settings_Save(). Called once from main.cpp, after the save
// root exists and BEFORE Host_WindowInit — the display mode is a window-creation
// decision (part 54's note: SDL_WINDOW_VULKAN and fullscreen are creation-time).
void Settings_Load(const std::string& path);

// Write the current values back to the loaded path. Called by every setter, so a
// crash between "changed in the menu" and "process exit" loses nothing.
void Settings_Save();

CzDisplayMode Settings_DisplayMode();
uint32_t      Settings_RenderScale();   // 1..4, multiplier over the title's 1280x720
bool          Settings_VSync();
int           Settings_ShadowTier();    // 0=low 1=medium 2=high

void Settings_SetDisplayMode(CzDisplayMode m);
void Settings_SetRenderScale(uint32_t s);
void Settings_SetVSync(bool on);
void Settings_SetShadowTier(int tier);

// The live-apply seam for the display mode: window.cpp polls this from its own
// thread each loop and applies + clears it. Returns -1 when nothing is pending.
// (A direct SDL call from the guest thread that runs the menu verb would violate
// the one-thread rule that window.cpp exists to keep.)
int  Settings_ConsumePendingDisplayMode();

// The HOST-RENDERED settings panel (part 60's pivot). The shipped PC options
// screen turned out to be a shell — layout and strings present, every behavior
// (input, focus, exit handshake) compiled out of the 360 XEX — so the menu the
// operator asked for is drawn by the host over the game's own options hub and
// driven from the pad seam. This state lives HERE because both sides need it:
// window.cpp draws it, the guest-side input pump mutates it.
bool Settings_OverlayVisible();
void Settings_SetOverlayVisible(bool on);
int  Settings_OverlaySelection();          // 0..3
void Settings_SetOverlaySelection(int row);
