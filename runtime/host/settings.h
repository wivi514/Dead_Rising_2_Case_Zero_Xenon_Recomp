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
uint32_t      Settings_RenderScale();   // LEGACY view of the internal res: round(H/720).
                                        // Kept for the shadow-tier footer and the parked
                                        // guest-screen arm; new code reads Settings_InternalRes
bool          Settings_VSync();
int           Settings_ShadowTier();    // 0=low 1=medium 2=high
int           Settings_FpsCap();        // 0=OFF (the 500 ceiling that never binds),
                                        // else 30/60/90/120/240/480
int           Settings_Fov();           // FIELD OF VIEW (part 61): degrees of
                                        // ADJUSTMENT from the game's own camera,
                                        // -10..+30 in steps of 1; 0 = OG, the
                                        // default and the bit-identical control
int           Settings_RtShadows();     // RT tier (part 64): 0 = none (the raster
                                        // cascade), 1/2/3 = RT LOW/MEDIUM/HIGH.
                                        // Env CZ_VK_RT_SHADOWS wins over this.
                                        // NON-ZERO REPLACES the raster shadow — the
                                        // operator's spec: one SHADOW row whose RT
                                        // values remove the normal shadow rather
                                        // than adding to it.
// The single SHADOW row the panel shows (operator revision, part 64): index 0..5 =
// LOW / MEDIUM / HIGH / RT LOW / RT MEDIUM / RT HIGH, derived from the two stored
// values above so an existing cz_settings.txt keeps its meaning.
int           Settings_ShadowRow();
void          Settings_SetShadowRow(int row);
int           Settings_Aspect();        // LEGACY: 1 when the internal res is wider than
                                        // 16:9. Derived; new code reads Settings_InternalRes

// THE INTERNAL RESOLUTION (part 60, operator revision 3). The store carries an
// explicit WIDTH x HEIGHT instead of a scale+aspect pair, because the menu now
// lists the DISPLAY'S OWN MODES (the operator: "show all resolution compatible
// with the player monitor" — 1920x1080 and friends, which no integer multiple of
// 1280x720 can express). The renderer scales RATIONALLY: Y by H/720, X by W/1280,
// both TRUNCATING (gotcha 373's overrun guarantee holds for any fixed rational).
// Valid: any height from 720 to 2880 (the rational converters do not care whether
// H/720 is "nice" — 900 works as well as 1080), width even, at least 16:9 for the
// height (narrower would need a sub-1 X factor and a fov CROP, which this port
// refuses rather than ships), and at most 6880 wide. Legacy render_scale/aspect keys still load and are
// converted, so an existing cz_settings.txt keeps its meaning.
bool Settings_ValidInternalRes(uint32_t w, uint32_t h);
void Settings_InternalRes(uint32_t& w, uint32_t& h);
void Settings_SetInternalRes(uint32_t w, uint32_t h);

// THE PENDING RESOLUTION (part 91) — what the panel's Resolution row is SHOWING but
// the player has not APPLIED yet. Stepping the row moves only this; the X press
// persists it (Settings_SetInternalRes) and hands it to the renderer's live-apply
// seam; leaving the panel discards it. 0,0 = nothing pending (the row shows the
// persisted value). NOT saved to the file — it is UI state that happens to be
// shared between the input pump (cpu/pc_options.cpp) and the drawer
// (host/window.cpp), which is the same reason the overlay selection lives here.
void Settings_PendingInternalRes(uint32_t& w, uint32_t& h);
void Settings_SetPendingInternalRes(uint32_t w, uint32_t h);

void Settings_SetDisplayMode(CzDisplayMode m);
void Settings_SetRenderScale(uint32_t s);
void Settings_SetVSync(bool on);
void Settings_SetShadowTier(int tier);
void Settings_SetFpsCap(int fps);
void Settings_SetFov(int deg);

// THE MOUSE (part 91). The census found no usable PC input in the 360 package —
// leftovers only (a MOUSE SENSITIVITY row in options_pc.txt, `always_show_mouse` in
// the image, handlers compiled out per part 60's verb-hash proof) and zero KB/M
// prompt icons — so the mouse is host-made: window.cpp turns relative deltas into
// right-stick camera (LMB=X, RMB=RT, MMB=Y, X1/X2=LB/RB) behind these two knobs.
// OFF by default so a pad player's build changes nothing.
bool Settings_MouseCam();
void Settings_SetMouseCam(bool on);
int  Settings_MouseSens();    // 1..10, default 5
void Settings_SetMouseSens(int s);
void Settings_SetRtShadows(int tier);
void Settings_SetAspect(int aspect);

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
int  Settings_OverlaySelection();          // 0..7 (eight rows as of part 91)
void Settings_SetOverlaySelection(int row);
