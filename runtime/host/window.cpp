#include "window.h"
#include "../gpu/vk_renderer.h"

#include <atomic>
#include <cstdio>

// CZ_HAVE_SDL is set by CMake when the window is built (the default). The headless
// build is not a fallback the runtime can fall INTO — it is a configure-time choice
// (-DCZ_WINDOW=OFF), and it says so on every startup, because "no window" and
// "window whose input is broken" are indistinguishable from a log otherwise.
// The three debug edges live OUTSIDE the CZ_HAVE_SDL split, and deliberately.
//
// They used to be set only by the keyboard, which meant the title's own DebugJump screen —
// the one route into the OUTDOOR world without walking there — was reachable only by a
// human at a window. The flags are plain atomics and their consumer is on the guest thread
// inside `XamInputGetState`, so neither end has ever needed SDL; only the SOURCE did.
// Defining them here lets `CZ_FAKE_PRESS_SEQ`'s F2/F3/F4 entries work in a headless run and
// in a `-DCZ_WINDOW=OFF` build, instead of a stub silently returning false — which is the
// failure shape this project keeps paying for (gotcha 151).
std::atomic<bool> g_debugJumpPressed{false};
std::atomic<bool> g_debugEnterPressed{false};
std::atomic<bool> g_debugMenuPressed{false};

// F9 — DUMP EVERY RESOLVE SNAPSHOT OF THE NEXT FRAME, on demand.
//
// `CZ_VK_SNAP_DUMP` used to fire only at a fixed frame number (`CZ_VK_SNAP_FRAME`, default
// 600), which is a fine trigger for a boot-time question and a bad one for any question
// about a PLACE. The operator asked for this, in the middle of standing on a defect
// waiting for a frame counter to reach 9000: the person who can see the spot is the only
// one who knows when the frame is worth dumping, and making them predict it in advance
// turns a two-minute measurement into a timed errand that misses.
//
// Same shape as the three edges above and for the same reason — an atomic, set by the
// keyboard, consumed elsewhere — so it works from `CZ_FAKE_PRESS_SEQ` too and does not
// need SDL at the consuming end.
std::atomic<bool> g_snapDumpPressed{false};

// F8 — RECORD EVERY PRESENTED FRAME FOR ABOUT A SECOND, on demand.
//
// The operator asked for this by describing a defect that no single frame can show:
// *"The decals how it looks like is pretty much normal but it appears and disappear like
// flicker make it so when I press f8 it records all frame for a second so you can see it."*
//
// That is the right instrument for the right reason. A screenshot of a flicker is a
// screenshot of one phase of it, and which phase you get is luck (gotcha 133 — one frame of
// an animated scene is ONE SAMPLE). F9 answers "what does it look like"; F8 answers "what
// does it do over time", and for an intermittent defect the second question is the only one
// with an answer.
//
// It also discriminates between the two mechanisms that look identical in a still: if the
// decal's DRAW is issued every frame and only the pixels change, the draw is losing a depth
// fight; if the draw list itself changes, the geometry is being dropped somewhere. The
// burst's manifest carries the draw count and the draw fingerprint per frame for exactly
// that reason, so the burst answers the question rather than just illustrating it.
std::atomic<bool> g_burstDumpPressed{false};

void Host_RequestDebugJump() { g_debugJumpPressed.store(true, std::memory_order_release); }
void Host_RequestDebugEnter() { g_debugEnterPressed.store(true, std::memory_order_release); }
void Host_RequestDebugMenu() { g_debugMenuPressed.store(true, std::memory_order_release); }
void Host_RequestSnapDump() { g_snapDumpPressed.store(true, std::memory_order_release); }
void Host_RequestBurstDump() { g_burstDumpPressed.store(true, std::memory_order_release); }

// F7 — MARK THE FRAME TRACE. The operator plays, feels a stutter, and presses this; the
// renderer stamps the current frame number into the trace and the log.
//
// WHY IT EXISTS. Correlating "it stuttered just now" with a frame number is otherwise
// guesswork: a play session is tens of thousands of frames and the worst-frame table keeps
// twelve. Without a marker their report can only be matched to the trace by wall-clock
// eyeballing, which is exactly the kind of loose join that has produced wrong conclusions
// in this project before. A keypress is an EVENT, and the frame it lands on is a fact.
//
// Reaction time is a known and stated limitation: a human presses ~200-500 ms after the
// thing they felt, so the marker names a NEIGHBOURHOOD, not the frame. The reader looks
// backwards from the marker for the worst frame in the preceding second — which is why the
// trace carries every frame rather than only the extremes.
std::atomic<bool> g_markPressed{ false };
void Host_RequestMark() { g_markPressed.store(true, std::memory_order_release); }
bool Host_ConsumeMarkPressed()
{
    return g_markPressed.exchange(false, std::memory_order_acq_rel);
}

bool Host_ConsumeSnapDumpPressed()
{
    return g_snapDumpPressed.exchange(false, std::memory_order_acq_rel);
}

bool Host_ConsumeBurstDumpPressed()
{
    return g_burstDumpPressed.exchange(false, std::memory_order_acq_rel);
}

bool Host_ConsumeDebugJumpPressed()
{
    return g_debugJumpPressed.exchange(false, std::memory_order_acq_rel);
}
bool Host_ConsumeDebugEnterPressed()
{
    return g_debugEnterPressed.exchange(false, std::memory_order_acq_rel);
}
bool Host_ConsumeDebugMenuPressed()
{
    return g_debugMenuPressed.exchange(false, std::memory_order_acq_rel);
}

#ifndef CZ_HAVE_SDL

bool Host_WindowInit()
{
    fprintf(stderr, "[host] built with -DCZ_WINDOW=OFF: no window, no present seam and "
                    "no pad. The boot will reach the title screen and stop (finding "
                    "37).\n");
    return false;
}
bool Host_WindowActive() { return false; }
bool Host_ProgressBegin(const char*) { return false; }
void Host_ProgressUpdate(const char*, float) {}
void Host_ProgressEnd() {}
bool Host_RunLauncher() { return true; }
void Host_Present(uint32_t, uint32_t, uint32_t) {}
void Host_PresentPixels(const uint8_t*, uint32_t, uint32_t) {}
void Host_WindowRun() {}
void Host_RequestQuit(const char*) {}
bool Host_PadState(uint32_t, HostPadState&) { return false; }
void Host_DebugMenuSetItems(const std::vector<std::string>&) {}
void Host_DebugMenuSetVisible(bool) {}
bool Host_DebugMenuConsumeAction(uint32_t&, int32_t&) { return false; }
bool Host_DebugOverlayRender(std::vector<uint8_t>&, uint32_t&, uint32_t&, uint32_t&,
                            uint32_t&, uint32_t&, uint32_t&) { return false; }
bool Host_VulkanSwapchainWanted() { return false; }
std::vector<const char*> Host_VulkanInstanceExtensions() { return {}; }
bool Host_VulkanCreateSurface(void*, uint64_t*) { return false; }
void Host_VulkanDrawableSize(uint32_t* w, uint32_t* h) { if (w) *w = 0; if (h) *h = 0; }
bool Host_DisplaySize(uint32_t* w, uint32_t* h) { if (w) *w = 0; if (h) *h = 0; return false; }
int Host_DisplayModeList(uint32_t*, int) { return 0; }

#else

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <vector>

#include <SDL.h>
// vulkan.h before SDL_vulkan.h so the latter uses the real handle types rather than
// its own forward declarations — the surface below is a VkSurfaceKHR either way, but
// VK_NULL_HANDLE only exists with the real header.
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>

#include "../gpu/vk_renderer.h"
#include "host_paths.h"
#include "settings.h"
#include "../cpu/native_kbm.h"
#include "stfs_extract.h"
#include <filesystem>

namespace {

// XInput's button bits. Written out rather than included from anywhere, because the
// only definition that matters is the one the GUEST uses — these are the values
// Case Zero's own code tests, and duplicating them here next to the key map is what
// makes the map auditable without chasing a header.
constexpr uint16_t XI_DPAD_UP        = 0x0001;
constexpr uint16_t XI_DPAD_DOWN      = 0x0002;
constexpr uint16_t XI_DPAD_LEFT      = 0x0004;
constexpr uint16_t XI_DPAD_RIGHT     = 0x0008;
constexpr uint16_t XI_START          = 0x0010;
constexpr uint16_t XI_BACK           = 0x0020;
constexpr uint16_t XI_LEFT_THUMB     = 0x0040;
constexpr uint16_t XI_RIGHT_THUMB    = 0x0080;
constexpr uint16_t XI_LEFT_SHOULDER  = 0x0100;
constexpr uint16_t XI_RIGHT_SHOULDER = 0x0200;
constexpr uint16_t XI_A              = 0x1000;
constexpr uint16_t XI_B              = 0x2000;
constexpr uint16_t XI_X              = 0x4000;
constexpr uint16_t XI_Y              = 0x8000;

bool          g_active = false;
SDL_Window*   g_window = nullptr;
// Null in the CZ_VK_SWAPCHAIN arm, where the window carries SDL_WINDOW_VULKAN and the
// renderer thread owns presentation. Every use of it in this file is guarded, and the
// guard is `g_renderer` itself rather than a second flag so the two can never disagree.
SDL_Renderer* g_renderer = nullptr;
bool          g_wantVulkanSwapchain = false;
// The window's DRAWABLE size, published by the event loop and read by the renderer's
// pump thread. Atomics rather than an SDL call from the pump: SDL documents window
// queries as belonging to the thread that created the window, and the renderer needs this
// every frame. The loop already sees every resize, so publishing it there is free and
// correct — and it is what tells the swapchain it has gone stale (part 54's blurry
// picture: the swapchain was built once at 1280x720 and a window enlarged after that kept
// being upscaled from it by the compositor).
std::atomic<uint32_t> g_drawableW{ 0 }, g_drawableH{ 0 };

// IT RUNS IN BOTH PRESENT ARMS, and that is a deliberate repair rather than tidiness.
//
// The first version only tracked the drawable when the swapchain wanted it, so the
// readback arm reported its window size NOWHERE — and part 54 then measured a present-path
// A/B into a 1088x612 window, wrote the result down as a property of the internal
// resolution, and had to be corrected by the operator playing it maximised at 2560x1417.
// A present-path number has TWO resolutions and naming only one of them is naming none
// (gotcha 353); the arm that cannot state one of them makes that mistake unavoidable.
//
// So the size is logged on every change in BOTH arms. It costs one line per resize.
void PublishDrawableSize()
{
    if (!g_window)
        return;
    int w = 0, h = 0;
    if (g_renderer)
        SDL_GetRendererOutputSize(g_renderer, &w, &h);
    else
        SDL_Vulkan_GetDrawableSize(g_window, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    const uint32_t nw = uint32_t(w), nh = uint32_t(h);
    // BOTH exchanges must run UNCONDITIONALLY. The first version had them inside one
    // `||`, and `||` short-circuits: on the very first publish the width exchange
    // returned "changed", so the HEIGHT EXCHANGE ON THE RIGHT NEVER EXECUTED — height
    // stayed 0 while this line printed "1280x720" from the locals. The pump then read
    // (1280, 0), clamped the zero to the surface minimum, and built a 1280x1 swapchain
    // the compositor smeared over the window: the launch-stretch defect, fixed by any
    // manual resize because a resize re-runs this with the width now equal, which let
    // the height write finally execute. A side effect on the right of `||` is a write
    // that happens only when the left side is false.
    const uint32_t ow = g_drawableW.exchange(nw, std::memory_order_acq_rel);
    const uint32_t oh = g_drawableH.exchange(nh, std::memory_order_acq_rel);
    if (nw != ow || nh != oh)
        fprintf(stderr, "[host] window drawable %ux%u (%s present) — quote this with any "
                        "frame time from this run\n",
                nw, nh, g_renderer ? "readback" : "swapchain");
}
// THE DISPLAY'S OWN SIZE (part 60 night item 4) — the desktop mode of whichever
// display the window currently sits on, published for the settings panel so its
// Resolution row only offers sizes the player's screen can show. Refreshed from the
// window thread (SDL rule) at init and once a second from the title-bar block, which
// also covers the window being dragged to another monitor without needing a
// display-event subscription of its own. Zero until the window exists; consumers
// treat unknown as "no clamp".
std::atomic<uint32_t> g_displayW{ 0 }, g_displayH{ 0 };

// The display's MODE LIST (part 60, operator revision 3): every distinct WxH the
// display reports that the renderer can honestly produce (Settings_ValidInternalRes)
// and that fits the desktop, ascending — what the Resolution row offers, the way a
// game's own menu mirrors the monitor's list. Guarded by its own mutex because the
// window thread rewrites it on monitor drags while the guest thread's panel input
// reads it.
std::mutex g_displayModesMutex;
std::vector<std::pair<uint32_t, uint32_t>> g_displayModes;

void PublishDisplaySize()
{
    if (!g_window)
        return;
    const int index = SDL_GetWindowDisplayIndex(g_window);
    SDL_DisplayMode mode{};
    if (index < 0 || SDL_GetDesktopDisplayMode(index, &mode) != 0 || mode.w <= 0 ||
        mode.h <= 0)
        return;
    const uint32_t ow = g_displayW.exchange(uint32_t(mode.w), std::memory_order_acq_rel);
    const uint32_t oh = g_displayH.exchange(uint32_t(mode.h), std::memory_order_acq_rel);
    if (ow != uint32_t(mode.w) || oh != uint32_t(mode.h))
        fprintf(stderr, "[host] display %d is %dx%d — the settings panel clamps its "
                        "resolution list to this\n", index, mode.w, mode.h);

    // The mode list, refreshed whenever the desktop size changed (first publish
    // included). SDL reports one entry per (size, refresh, format); the menu wants
    // distinct sizes, so dedupe. Modes the renderer cannot express (odd widths,
    // sub-720 heights, narrower than 16:9 — the 4:3 and 5:4 legacy modes) are
    // filtered here so the panel never offers a row it cannot honor.
    if (ow != uint32_t(mode.w) || oh != uint32_t(mode.h))
    {
        std::vector<std::pair<uint32_t, uint32_t>> modes;
        const int n = SDL_GetNumDisplayModes(index);
        for (int i = 0; i < n; ++i)
        {
            SDL_DisplayMode m{};
            if (SDL_GetDisplayMode(index, i, &m) != 0 || m.w <= 0 || m.h <= 0)
                continue;
            const uint32_t w = uint32_t(m.w), h = uint32_t(m.h);
            if (!Settings_ValidInternalRes(w, h))
                continue;
            if (w > uint32_t(mode.w) || h > uint32_t(mode.h))
                continue;
            if (std::find(modes.begin(), modes.end(), std::make_pair(w, h)) ==
                modes.end())
                modes.emplace_back(w, h);
        }
        std::sort(modes.begin(), modes.end(),
                  [](const auto& a, const auto& b) {
                      return a.second != b.second ? a.second < b.second
                                                  : a.first < b.first;
                  });
        fprintf(stderr, "[host] display %d offers %zu usable modes:", index,
                modes.size());
        for (const auto& [w, h] : modes)
            fprintf(stderr, " %ux%u", w, h);
        fprintf(stderr, "\n");
        std::lock_guard<std::mutex> lock(g_displayModesMutex);
        g_displayModes = std::move(modes);
    }
}

// Apply a display mode to the live window. WINDOW THREAD ONLY (the SDL rule this
// whole file exists to keep): Host_WindowInit calls it once after creation for the
// persisted mode, and the loop calls it when the PC options screen changes the
// setting mid-run. The resulting SIZE_CHANGED event flows through the normal event
// path, so PublishDrawableSize fires and the swapchain rebuilds itself exactly as it
// does for a manual resize — no second resize path to keep correct.
void ApplyDisplayModeNow(CzDisplayMode m)
{
    if (!g_window)
        return;
    switch (m)
    {
        case CzDisplayMode::Windowed:
            SDL_SetWindowFullscreen(g_window, 0);
            break;
        case CzDisplayMode::Borderless:
            SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            break;
        case CzDisplayMode::Fullscreen:
        {
            // Exclusive fullscreen AT THE DESKTOP MODE, never at the window's current
            // size: mode-switching a 2560-wide desktop to 1280x720 because that was
            // the creation size is the classic wrong spelling of "Fullscreen".
            SDL_DisplayMode dm{};
            const int display = SDL_GetWindowDisplayIndex(g_window);
            if (SDL_GetDesktopDisplayMode(display < 0 ? 0 : display, &dm) == 0)
                SDL_SetWindowDisplayMode(g_window, &dm);
            SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN);
            break;
        }
    }
    fprintf(stderr, "[host] display mode -> %s\n",
            m == CzDisplayMode::Windowed ? "windowed"
            : m == CzDisplayMode::Borderless ? "borderless fullscreen"
                                             : "fullscreen");
}

SDL_GameController* g_controller = nullptr;
SDL_JoystickID      g_controllerId = -1;
bool g_inputTrace = false;

// THE INPUT TRACE'S CLOCK, AND WHY THE TRACE WAS USELESS WITHOUT IT.
//
// `CZ_INPUT_TRACE=1` has printed one line per pad state change since phase 3, and every
// line said WHAT was pressed and nothing about WHEN. That is enough to answer "does the
// pad work" and it is not enough for the thing the operator asked for in part 80:
// *"look at the input I do and at what time they happen according to time not frame per
// second so you can reproduce it"*. They had just found DebugJump entries that spawn into
// an 8,500-8,900-draw crowd — the load `part80-kickoff.md` §1 says a CPU item must be
// measured at or not at all — and the only way that route becomes MINE to run is if their
// keystrokes can be transcribed into a `CZ_FAKE_PRESS_SEQ` recipe.
//
// So the line carries milliseconds since process start, on the same epoch as
// `debug_tunables.cpp`'s `[debug] ... at Ns` lines (both are static initialisers, so they
// agree to a few milliseconds). That matters more than the absolute value: the DebugJump
// screen lands anywhere from 24 s to 131 s after boot (gotcha 75), so a recipe anchored on
// process start is a fit to one afternoon and a recipe anchored on the SCREEN LANDING is a
// statement about the game. Having both clocks in one file is what makes the second
// computable from the log after the fact.
//
// It also DECODES, into exactly the vocabulary `CZ_FAKE_PRESS_SEQ` accepts (A, START,
// DOWN, LSUP, RSRIGHT...). A hex button mask is transcribable in principle and nobody does
// it correctly at 40 lines a minute; printing the name the replay side already parses
// makes the transcription mechanical instead of a second place to make a mistake.
static const auto g_inputEpoch = std::chrono::steady_clock::now();

static long long InputElapsedMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_inputEpoch)
        .count();
}

// The button bits, named as the press-sequence names them. Kept here rather than shared
// with `imports.cpp`'s `kButtons` on purpose: that table is the REPLAY side's vocabulary
// and this is the RECORD side's, and if the two ever disagree the trace should say so by
// printing a name the replay rejects — not by silently agreeing because they are the same
// array. The masks are XInput's and are fixed by the contract, not by either table.
struct TracedButton { uint16_t mask; const char* name; };
static constexpr TracedButton kTracedButtons[] = {
    { 0x1000, "A" },     { 0x2000, "B" },      { 0x4000, "X" },     { 0x8000, "Y" },
    { 0x0010, "START" }, { 0x0020, "BACK" },   { 0x0001, "UP" },    { 0x0002, "DOWN" },
    { 0x0004, "LEFT" },  { 0x0008, "RIGHT" },  { 0x0040, "LTHUMB" },{ 0x0080, "RTHUMB" },
    { 0x0100, "LB" },    { 0x0200, "RB" },
};

// A stick axis as a sequence entry name, or nothing when it is inside the deflection
// this printer treats as centred. HALF deflection is the threshold rather than any
// deflection at all: an analog stick at rest reads a few hundred counts of noise, and a
// trace that reported LSUP for noise would put a walk entry into every transcription.
// The replay side only has full deflection, so half is also the point past which the two
// stop being comparable — say the axis is deflected when the recipe could reproduce it.
static void AppendStick(char* out, size_t n, const char* neg, const char* pos, int v)
{
    if (v > 16383)
        snprintf(out + strlen(out), n - strlen(out), ",%s", pos);
    else if (v < -16383)
        snprintf(out + strlen(out), n - strlen(out), ",%s", neg);
}

// The pad snapshot. Written by the event loop, read by whichever guest thread is
// inside XamInputGetState. A mutex rather than an atomic struct because the state is
// 16 bytes (never lock-free) and because the read rate is not a hot path: A5 shows
// 12,365 XamInputGetState calls in a whole boot, i.e. roughly one per frame per
// user, against a lock that is held for a struct copy.
std::mutex   g_padMutex;
// Packet 1, not 0, so that the windowed and headless arms hand the guest the same
// starting number. A control arm that differs from the live path in a field the title
// is entitled to branch on is not a control.
HostPadState g_pads[2] = {
    { 1, 0, 0, 0, 0, 0, 0, 0 }, // user 0: physical controller
    { 1, 0, 0, 0, 0, 0, 0, 0 }, // user 1: keyboard
};

// Keyboard input is gated on window focus. SDL does reset its keyboard state when a
// window loses focus, so this is belt and braces — but the failure it prevents is a
// key held at the moment focus is lost staying "down" in the guest forever, and that
// one presents as the title behaving as though a button is stuck. A GAME CONTROLLER
// is deliberately NOT gated: a pad works whatever window is focused, which is what
// every other application on the machine does.
bool g_keyboardFocus = true;
// MOUSE -> RIGHT-STICK CAMERA (part 91). The census answered the operator's "is PC
// input already in the files?" with a clean no on both halves: the engine carries
// PC-era leftovers (a MOUSE SENSITIVITY row in options_pc.txt, `always_show_mouse`
// in the image) but the 360 XEX compiled the keyboard/mouse handlers out (part 60's
// verb-hash proof covers "Mouse" by name), and the package ships zero KB/M assets —
// no bindings, no key-name table, no prompt icons in 12,481 archive entries. So the
// mouse lives HERE, at the same host seam the keyboard fallback has always used:
// relative-mode deltas become right-stick deflection (velocity-scaled, EMA-smoothed),
// LMB = X (attack), RMB = RT (aim), MMB = Y, and the side buttons LB/RB. Off unless
// the panel's MOUSE CAMERA row says on, so a pad player's build changes nothing.
// Deltas are accumulated by the event loop and consumed by ReadKeyboard — both on
// the window thread, the atomics are belt and braces.
std::atomic<int> g_mouseDX{ 0 }, g_mouseDY{ 0 };
bool g_relativeMouse = false;
// g_debugJumpPressed / Enter / Menu are defined at the top of this file, outside the
// CZ_HAVE_SDL split AND outside this anonymous namespace — the keyboard below is one
// SOURCE of those edges, no longer the only one. The `::` is load-bearing: an unqualified
// redeclaration in here would name a NEW internal-linkage object, and the keyboard would
// then set a flag nobody reads.
using ::g_debugJumpPressed;
using ::g_debugEnterPressed;
using ::g_debugMenuPressed;
std::mutex g_debugOverlayMutex;
std::vector<std::string> g_debugOverlayItems;
std::atomic<bool> g_debugOverlayVisible{false};
size_t g_debugOverlaySelection = 0;
std::atomic<uint32_t> g_debugOverlayActionIndex{0};
std::atomic<int32_t> g_debugOverlayAction{0};

// The frame descriptor the present loop consumes. `seq` is the handshake: the PM4
// executor bumps it, the loop presents when it changes and sleeps when it does not.
// Publishing a counter rather than signalling a condition variable keeps
// Host_Present free of anything that can block a GPU thread (see window.h).
std::mutex   g_frameMutex;
uint32_t     g_frontBuffer = 0, g_frameWidth = 1280, g_frameHeight = 720;
std::atomic<uint64_t> g_swapSeq{ 0 };

// Phase 5's rendered frame. Double-buffered under g_frameMutex: the renderer writes
// `g_pixelsBack` and swaps, the loop reads `g_pixelsFront`. The copy is what keeps the
// two threads independent — the renderer's buffer is its next frame's arena the moment
// Host_PresentPixels returns, so holding a pointer to it would race with the next frame
// rather than with the present.
std::vector<uint8_t> g_pixelsBack, g_pixelsFront;
uint32_t g_pixelsWidth = 0, g_pixelsHeight = 0;
bool g_havePixels = false;
SDL_Texture* g_frameTexture = nullptr;
int g_frameTextureW = 0, g_frameTextureH = 0;

// Set by Host_RequestQuit from the guest thread; read by the loop.
std::atomic<const char*> g_quitReason{ nullptr };

constexpr int kDefaultWidth = 1280, kDefaultHeight = 720;

const char* Glyph(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    switch (c)
    {
        case 'A': return "01110100011000111111100011000110001";
        case 'B': return "11110100011000111110100011000111110";
        case 'C': return "01111100001000010000100001000001111";
        case 'D': return "11110100011000110001100011000111110";
        case 'E': return "11111100001000011110100001000011111";
        case 'F': return "11111100001000011110100001000010000";
        case 'G': return "01111100001000010111100011000101111";
        case 'H': return "10001100011000111111100011000110001";
        case 'I': return "11111001000010000100001000010011111";
        case 'J': return "00111000100001000010100101001001100";
        case 'K': return "10001100101010011000101001001010001";
        case 'L': return "10000100001000010000100001000011111";
        case 'M': return "10001110111010110101100011000110001";
        case 'N': return "10001110011010110011100011000110001";
        case 'O': return "01110100011000110001100011000101110";
        case 'P': return "11110100011000111110100001000010000";
        case 'Q': return "01110100011000110001101011001001101";
        case 'R': return "11110100011000111110101001001010001";
        case 'S': return "01111100001000001110000010000111110";
        case 'T': return "11111001000010000100001000010000100";
        case 'U': return "10001100011000110001100011000101110";
        case 'V': return "10001100011000110001100010101000100";
        case 'W': return "10001100011000110101101011101110001";
        case 'X': return "10001100010101000100010101000110001";
        case 'Y': return "10001100010101000100001000010000100";
        case 'Z': return "11111000010001000100010001000011111";
        case '0': return "01110100011001110101110011000101110";
        case '1': return "00100011000010000100001000010001110";
        case '2': return "01110100010000100010001000100011111";
        case '3': return "11110000010000101110000010000111110";
        case '4': return "00010001100101010010111110001000010";
        case '5': return "11111100001000011110000010000111110";
        case '6': return "01110100001000011110100011000101110";
        case '7': return "11111000010001000100010000100001000";
        case '8': return "01110100011000101110100011000101110";
        case '9': return "01110100011000101111000010000101110";
        case '-': return "00000000000000011111000000000000000";
        case '_': return "00000000000000000000000000000011111";
        case '.': return "00000000000000000000000000110001100";
        case ':': return "00000011000110000000011000110000000";
        case '/': return "00001000100001000100010001000010000";
        case '>': return "10000010000010000010001000100010000";
        case '<': return "00001000100010001000001000001000001";
        default:  return nullptr;
    }
}

// THE DEBUG OVERLAY, AS A LIST OF RECTANGLES — one layout, two backends.
//
// It used to be SDL_RenderFillRect calls inline. Part 54 made the Vulkan swapchain the
// default present path, and a window carrying SDL_WINDOW_VULKAN has no SDL_Renderer, so
// the overlay needed a second backend that rasterises into a buffer the renderer can blit.
//
// Writing that as a second copy of the layout is how two drawings of the same menu drift
// apart — one of them gains a row, or a colour, or a scroll offset, and nobody notices
// until an operator reports that the menu "looks different in the other mode". So the
// LAYOUT is here, once, and it emits rectangles to whatever wants them; the two backends
// are a `SDL_RenderFillRect` and a memory fill, and neither knows anything about menus.
// THE SETTINGS PANEL (part 60) — same one-layout-two-backends contract as the
// debug overlay below, same rect sink, so it works identically in the readback
// and swapchain present arms. Drawn over the game's own Help & Options hub: the
// shipped PC options screen is a shell (its input, focus and exit handshake were
// compiled out of the 360 XEX), so the menu is HOST property end to end — drawn
// here, driven from the pad seam in cpu/pc_options.cpp, persisted by settings.cpp.
template <typename Rect>
void EmitSettingsOverlay(int w, int h, Rect&& rect)
{
    const int panelW = 640, panelH = 460;   // 460: eight rows — part 91 added the
                                            // MOUSE CAMERA pair (part 64 had merged
                                            // the RT tiers INTO the shadow row)
    const int panelX = (w - panelW) / 2, panelY = (h - panelH) / 2 - 30;
    if (panelW <= 0 || panelH <= 0)
        return;
    rect(panelX, panelY, panelW, panelH, 20, 22, 26, 235);
    rect(panelX, panelY, panelW, 2, 200, 170, 60, 255);
    rect(panelX, panelY + panelH - 2, panelW, 2, 200, 170, 60, 255);
    rect(panelX, panelY, 2, panelH, 200, 170, 60, 255);
    rect(panelX + panelW - 2, panelY, 2, panelH, 200, 170, 60, 255);

    auto text = [&](int tx, int ty, const std::string& str, int scale,
                    uint8_t r, uint8_t g, uint8_t b) {
        for (char c : str)
        {
            if (const char* bits = Glyph(c))
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if (bits[row * 5 + col] == '1')
                            rect(tx + col * scale, ty + row * scale, scale, scale,
                                 r, g, b, 255);
            tx += 6 * scale;
        }
    };

    text(panelX + 20, panelY + 16, "PC SETTINGS", 3, 245, 235, 200);
    // The hint line follows the ACTIVE input path: with the native keyboard on,
    // the panel is driven by arrows/Enter/Esc/X (NativeKbm_PanelButtons) and the
    // words say so — "B CLOSE" on a keyboard screen was the operator's report.
    text(panelX + 20, panelY + 46,
         NativeKbm_Active()
             ? "UP/DOWN ROW   LEFT/RIGHT CHANGE   X APPLY   ESC CLOSE"
             : "UP/DOWN ROW   LEFT/RIGHT CHANGE   X APPLY   B CLOSE",
         2, 160, 160, 170);

    static const char* kModeNames[] = { "WINDOW", "BORDERLESS", "FULLSCREEN" };
    static const char* kOnOff[] = { "OFF", "ON" };
    static const char* kTiers[] = { "LOW", "MEDIUM", "HIGH" };
    const uint32_t scale = Settings_RenderScale();
    // The Resolution row shows the PENDING value when one exists (stepped but not
    // yet applied — part 91's apply-button flow), starred so "shown" and "running"
    // cannot be confused; otherwise the persisted internal resolution.
    char resName[26];
    bool resPending = false;
    {
        uint32_t rw = 0, rh = 0, pw = 0, ph = 0;
        Settings_InternalRes(rw, rh);
        Settings_PendingInternalRes(pw, ph);
        resPending = pw && (pw != rw || ph != rh);
        if (resPending)
            snprintf(resName, sizeof resName, "%u X %u *", pw, ph);
        else
            snprintf(resName, sizeof resName, "%u X %u", rw, rh);
    }
    // The frame cap's display name. Values come from the validated set in
    // settings.cpp, so the fallback only fires on a hand-edited file mid-run.
    char capName[8] = "OFF";
    if (const int cap = Settings_FpsCap(); cap > 0)
        snprintf(capName, sizeof capName, "%d", cap);
    // The FOV row shows "OG" at 0 — the plan's language for "exactly the game's own
    // camera" — and a signed degree adjustment otherwise. Applies LIVE.
    char fovName[8] = "OG";
    if (const int fov = Settings_Fov(); fov != 0)
        snprintf(fovName, sizeof fovName, "%+d", fov);
    // ONE SHADOW ROW (part 64, operator's revision): the raster tiers and the RT
    // tiers are values of the SAME setting, because selecting an RT value REPLACES
    // the normal shadow rather than adding to it — "normal shadow would be removed
    // to be replaced by the RT shadow if a rt settings is selected". Two rows
    // implied you could have both, which is what the first build actually did.
    //
    // On a device without ray query the RT values are not offered at all: the row
    // stops at HIGH and the footer says why. Better than showing values that
    // refuse to move (the gamma-slider rule) when the whole class is unavailable.
    static const char* kShadowRow[] = { "LOW",    "MEDIUM",    "HIGH",
                                        "RT LOW", "RT MEDIUM", "RT HIGH" };
    // WHEN THE RT RUNGS ARE NOT OFFERED, SHOW THE RASTER TIER, not the stored RT one.
    // `Settings_ShadowRow()` reports `2 + rtShadows` whenever a saved `cz_settings.txt`
    // carries a non-zero RT tier, so a file written while the rungs existed would print
    // "RT MEDIUM" on a ladder that only goes to HIGH — and the first press would jump to
    // an unrelated value. This was already reachable before part 71 parked the feature,
    // on any device without ray query; parking it just made it the common case. The
    // stored value is not touched, so unparking restores the player's choice.
    const int shadowRow =
        VkRenderer_RtAvailable() ? Settings_ShadowRow() : Settings_ShadowTier();
    // The mouse pair (part 91): the census found no PC input in the package, so
    // the mouse is host-made (window.cpp's ReadKeyboard) and these are its knobs.
    char sensName[4];
    snprintf(sensName, sizeof sensName, "%d", Settings_MouseSens());
    const char* rows[8][2] = {
        { "RESOLUTION", resName },
        { "DISPLAY MODE", kModeNames[int(Settings_DisplayMode()) % 3] },
        { "VSYNC", kOnOff[Settings_VSync() ? 1 : 0] },
        { "SHADOW", kShadowRow[shadowRow % 6] },
        { "FRAME CAP", capName },
        { "FIELD OF VIEW", fovName },
        { "MOUSE CAMERA", kOnOff[Settings_MouseCam() ? 1 : 0] },
        { "MOUSE SENS", sensName },
    };
    const int sel = Settings_OverlaySelection();
    for (int i = 0; i < 8; ++i)
    {
        const int y = panelY + 86 + i * 40;
        if (i == sel)
            rect(panelX + 12, y - 6, panelW - 24, 30, 70, 55, 20, 255);
        text(panelX + 28, y, rows[i][0], 2, i == sel ? 255 : 200,
             i == sel ? 240 : 200, i == sel ? 180 : 205);
        std::string value = std::string("< ") + rows[i][1] + " >";
        text(panelX + panelW - 28 - int(value.size()) * 12, y, value, 2,
             i == sel ? 255 : 190, i == sel ? 240 : 210, i == sel ? 120 : 210);
    }
    // The Shadow row is LIVE as of part 60 (the renderer re-reads the tier each
    // frame), but the tier scales are floored at the title's own 1280x720 base — so
    // at render scale 1 every tier is 1x and the row is honestly inert, which the
    // footer says rather than letting a dead row pretend (the gamma-slider rule).
    // A dead rung must say WHY it is dead, and the two reasons need different words:
    // a device without ray query cannot be fixed by the user, a missing shader variant
    // cache is one build command away (tools/patch_rt_shadow_hlsl.py).
    const int rtWhy = VkRenderer_RtUnavailableReason();
    // The footer leads with the pending-apply hint when one exists — the one moment
    // the player needs telling what X does — and the resolution note otherwise says
    // LIVE, because it is (part 91: applied at the frame boundary on the X press).
    text(panelX + 20, panelY + panelH - 30,
         resPending
             ? "PRESS X TO APPLY THE NEW RESOLUTION"
         : rtWhy == 3
             ? "RESOLUTION: X APPLIES LIVE - RT SHADOWS ARE OFF IN THIS BUILD"
         : rtWhy == 1
             ? "RESOLUTION: X APPLIES LIVE - NO RAY QUERY: RT UNAVAILABLE"
         : rtWhy == 2
             ? "RESOLUTION: X APPLIES LIVE - NO RT SHADER CACHE: SEE THE LOG"
             : (scale > 1 ? "RESOLUTION: X APPLIES LIVE - SHADOW: LIVE"
                          : "RESOLUTION: X APPLIES LIVE - SHADOW INERT AT 720P"),
         2, resPending ? 255 : 150, resPending ? 220 : 140, resPending ? 120 : 120);
}

template <typename Rect>
void EmitDebugOverlay(int w, int h, Rect&& rect)   // rect(x,y,w,h,r,g,b,a)
{
    // Caller holds g_debugOverlayMutex.
    if (Settings_OverlayVisible())
    {
        // The settings panel BORROWS this whole path — emit, both backends, the
        // swapchain blit — instead of duplicating it. Debug menu and settings
        // panel are never wanted at once; if both are up, settings wins.
        EmitSettingsOverlay(w, h, rect);
        return;
    }
    const int panelX = 24, panelY = 24;
    const int panelW = w > 760 ? 720 : w - 48;
    const int panelH = h - 48;
    if (panelW <= 0 || panelH <= 0)
        return;
    // The panel is the ONE translucent element -- 225/255, so the game reads through it.
    // Every other rect is opaque. Alpha is a parameter rather than something a backend
    // infers from the colour, which is what the SDL backend used to do and which broke the
    // moment a second backend needed the same information.
    rect(panelX, panelY, panelW, panelH, 8, 26, 96, 225);            // panel
    rect(panelX, panelY, panelW, 1, 70, 150, 255, 255);              // border, four edges
    rect(panelX, panelY + panelH - 1, panelW, 1, 70, 150, 255, 255);
    rect(panelX, panelY, 1, panelH, 70, 150, 255, 255);
    rect(panelX + panelW - 1, panelY, 1, panelH, 70, 150, 255, 255);

    auto text = [&](int tx, int ty, const std::string& str, int scale,
                    uint8_t r, uint8_t g, uint8_t b) {
        for (char c : str)
        {
            if (const char* bits = Glyph(c))
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if (bits[row * 5 + col] == '1')
                            rect(tx + col * scale, ty + row * scale, scale, scale, r, g, b, 255);
            tx += 6 * scale;
        }
    };

    text(44, 42, "CASE ZERO DEBUG MENU", 3, 255, 255, 255);
    text(44, 70, "UP/DOWN SELECT  ENTER USE  LEFT/RIGHT EDIT  F4 CLOSE", 2, 145, 205, 255);

    const size_t rows = panelH > 120 ? size_t((panelH - 110) / 18) : 0;
    const size_t start = g_debugOverlaySelection >= rows
        ? g_debugOverlaySelection - rows + 1 : 0;
    for (size_t line = 0; line < rows && start + line < g_debugOverlayItems.size(); ++line)
    {
        const size_t index = start + line;
        const bool selected = index == g_debugOverlaySelection;
        if (selected)
            rect(38, 98 + int(line) * 18, panelW - 28, 17, 35, 105, 205, 255);
        std::string label = (selected ? "> " : "  ") + g_debugOverlayItems[index];
        if (label.size() > 54) label.resize(54);
        text(44, 101 + int(line) * 18, label, 2,
             selected ? 255 : 205, selected ? 255 : 225, 255);
    }
}

void DrawDebugOverlay()
{
    std::lock_guard<std::mutex> lock(g_debugOverlayMutex);
    if (!g_debugOverlayVisible.load(std::memory_order_acquire) &&
        !Settings_OverlayVisible())
        return;
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(g_renderer, &w, &h);
    // BLEND stays on for the SDL backend, because that is what it has always looked like
    // and this refactor must not change the picture of the arm it is not about. The panel
    // is drawn at alpha 225 there and opaque in the Vulkan backend, which is the one
    // deliberate difference between them and is noted at the Vulkan blit.
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    EmitDebugOverlay(w, h, [&](int x, int y, int rw, int rh,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
        SDL_Rect p{ x, y, rw, rh };
        SDL_RenderFillRect(g_renderer, &p);
    });
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
}

// The key map, as one table so that printing it and applying it cannot drift apart.
// Every recompilation port ends up with a keyboard fallback, and every one of them
// ends up with the map documented in a README that is wrong by the third session.
struct KeyBinding
{
    SDL_Scancode scancode;
    uint16_t     button;
    const char*  keyName;
    const char*  padName;
};

const KeyBinding kKeyMap[] = {
    { SDL_SCANCODE_RETURN,    XI_START,          "Enter",     "START" },
    { SDL_SCANCODE_KP_ENTER,  XI_START,          "KP Enter",  "START" },
    { SDL_SCANCODE_BACKSPACE, XI_BACK,           "Backspace", "BACK" },
    { SDL_SCANCODE_SPACE,     XI_A,              "Space",     "A" },
    { SDL_SCANCODE_LSHIFT,    XI_B,              "LShift",    "B" },
    { SDL_SCANCODE_E,         XI_X,              "E",         "X" },
    { SDL_SCANCODE_Q,         XI_Y,              "Q",         "Y" },
    { SDL_SCANCODE_Z,         XI_LEFT_SHOULDER,  "Z",         "LB" },
    { SDL_SCANCODE_C,         XI_RIGHT_SHOULDER, "C",         "RB" },
    { SDL_SCANCODE_F,         XI_LEFT_THUMB,     "F",         "L3" },
    { SDL_SCANCODE_G,         XI_RIGHT_THUMB,    "G",         "R3" },
    { SDL_SCANCODE_UP,        XI_DPAD_UP,        "Up",        "D-pad up" },
    { SDL_SCANCODE_DOWN,      XI_DPAD_DOWN,      "Down",      "D-pad down" },
    { SDL_SCANCODE_LEFT,      XI_DPAD_LEFT,      "Left",      "D-pad left" },
    { SDL_SCANCODE_RIGHT,     XI_DPAD_RIGHT,     "Right",     "D-pad right" },
};

// SDL's controller buttons map one-to-one onto XInput's, which is unsurprising —
// SDL_GameController *is* the 360 pad's layout, generalised. Listed anyway so a
// future reader can see there is no cleverness here.
struct PadBinding
{
    SDL_GameControllerButton sdl;
    uint16_t                 button;
};

const PadBinding kPadMap[] = {
    { SDL_CONTROLLER_BUTTON_A, XI_A },
    { SDL_CONTROLLER_BUTTON_B, XI_B },
    { SDL_CONTROLLER_BUTTON_X, XI_X },
    { SDL_CONTROLLER_BUTTON_Y, XI_Y },
    { SDL_CONTROLLER_BUTTON_START, XI_START },
    { SDL_CONTROLLER_BUTTON_BACK, XI_BACK },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, XI_LEFT_THUMB },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, XI_RIGHT_THUMB },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XI_LEFT_SHOULDER },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XI_RIGHT_SHOULDER },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, XI_DPAD_UP },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, XI_DPAD_DOWN },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, XI_DPAD_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XI_DPAD_RIGHT },
};

void PrintKeyMap()
{
    fprintf(stderr, "[host] keyboard -> pad 1 (merged with the controller):");
    for (const auto& k : kKeyMap)
        fprintf(stderr, "  %s=%s", k.keyName, k.padName);
    fprintf(stderr, "\n[host] keyboard -> sticks:  WASD=left stick  IJKL=right stick  "
                    "1/3=LT/RT  (positions, not letters — ZQSD on AZERTY)\n");
    fprintf(stderr, "[host] mouse (when MOUSE CAMERA is ON in Visuals): camera=right "
                    "stick  LMB=X  RMB=RT  MMB=Y  side=LB/RB\n");
    fprintf(stderr, "[host] the window must have keyboard FOCUS for any of this to "
                    "reach the guest.\n");
}

void OpenController(int deviceIndex)
{
    if (g_controller || !SDL_IsGameController(deviceIndex))
        return;
    g_controller = SDL_GameControllerOpen(deviceIndex);
    if (!g_controller)
    {
        fprintf(stderr, "[host] SDL_GameControllerOpen(%d) failed: %s\n", deviceIndex,
                SDL_GetError());
        return;
    }
    g_controllerId =
        SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller));
    fprintf(stderr, "[host] controller attached: %s (instance %d)\n",
            SDL_GameControllerName(g_controller), int(g_controllerId));
}

void CloseController(SDL_JoystickID which)
{
    if (!g_controller || which != g_controllerId)
        return;
    fprintf(stderr, "[host] controller detached (instance %d)\n", int(which));
    SDL_GameControllerClose(g_controller);
    g_controller = nullptr;
    g_controllerId = -1;
}

// A keyboard axis is a pair of keys, and the value it produces is FULL SCALE.
//
// No deadzone and no ramp is applied anywhere in this module, on purpose. XInput
// hands a title raw axis values and every Xbox 360 title applies its own deadzone —
// Case Zero included. Filtering here would be this runtime inventing an input
// characteristic the console does not have, and the symptom of getting it wrong
// (a slightly unresponsive stick) is exactly the kind of thing that gets blamed on
// the game for a whole session.
int16_t KeyAxis(const uint8_t* keys, SDL_Scancode negative, SDL_Scancode positive)
{
    const int v = (keys[positive] ? 1 : 0) - (keys[negative] ? 1 : 0);
    return int16_t(v > 0 ? 32767 : v < 0 ? -32768 : 0);
}

// SDL's stick Y axis points DOWN and XInput's points UP. This is the one conversion
// in the module that is not a rename, and getting it wrong produces a game that
// works perfectly except that up is down — a bug that reads as a guest problem.
int16_t PadAxisY(SDL_GameController* c, SDL_GameControllerAxis axis)
{
    const int v = -int(SDL_GameControllerGetAxis(c, axis));
    return int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

// SDL scancode -> Windows VK, covering exactly the 62 keys the title's own
// source-token table names (docs/native-kbm-phaseA.md A.1). Anything else — and
// in particular the F-keys, which are host debug edges — returns 0 and is never
// pushed to the guest.
uint16_t ScancodeToVk(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return uint16_t(0x41 + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return uint16_t(0x31 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return uint16_t(0x61 + (sc - SDL_SCANCODE_KP_1));
    switch (sc)
    {
        case SDL_SCANCODE_0:        return 0x30;
        case SDL_SCANCODE_KP_0:     return 0x60;
        case SDL_SCANCODE_LEFT:     return 0x25;
        case SDL_SCANCODE_UP:       return 0x26;
        case SDL_SCANCODE_RIGHT:    return 0x27;
        case SDL_SCANCODE_DOWN:     return 0x28;
        case SDL_SCANCODE_SPACE:    return 0x20;
        case SDL_SCANCODE_LSHIFT:   return 0xA0;
        case SDL_SCANCODE_RSHIFT:   return 0xA1;
        case SDL_SCANCODE_LCTRL:    return 0xA2;
        case SDL_SCANCODE_RCTRL:    return 0xA3;
        case SDL_SCANCODE_LALT:     return 0xA4;
        case SDL_SCANCODE_RALT:     return 0xA5;
        case SDL_SCANCODE_ESCAPE:   return 0x1B;
        case SDL_SCANCODE_RETURN:   return 0x0D;
        case SDL_SCANCODE_KP_ENTER: return 0x0D;
        case SDL_SCANCODE_COMMA:    return 0xBC;
        case SDL_SCANCODE_PERIOD:   return 0xBE;
        case SDL_SCANCODE_TAB:      return 0x09;
        default:                    return 0;
    }
}

// One SDL key event into the native path (part 92): the WASD level mask always
// tracks reality (a release must land even if a panel opened mid-hold), the
// keystroke QUEUE is gated on focus and on the overlays owning the keyboard.
void NativeKbmKeyEvent(const SDL_KeyboardEvent& e, bool down)
{
    const SDL_Scancode sc = e.keysym.scancode;
    static uint32_t wasd = 0;
    uint32_t bit = 0;
    switch (sc)
    {
        case SDL_SCANCODE_W: bit = 1u << 0; break;
        case SDL_SCANCODE_S: bit = 1u << 1; break;
        case SDL_SCANCODE_A: bit = 1u << 2; break;
        case SDL_SCANCODE_D: bit = 1u << 3; break;
        default: break;
    }
    if (bit)
    {
        wasd = down ? (wasd | bit) : (wasd & ~bit);
        NativeKbm_MoveKeys(wasd);
    }
    const uint16_t vk = ScancodeToVk(sc);
    if (!vk)
        return;
    // panel levels track EVERY event (releases must land even when the gates
    // below suppress the game-facing push — the stuck-ENTER lesson)
    NativeKbm_PanelKeyLevel(vk, down);
    if (!g_keyboardFocus ||
        g_debugOverlayVisible.load(std::memory_order_acquire) ||
        Settings_OverlayVisible())
        return;
    if (down)
        NativeKbm_NoteDeviceInput(false);
    const SDL_Keymod mod = SDL_Keymod(e.keysym.mod);
    uint16_t mods = 0;
    if (mod & KMOD_SHIFT) mods |= 0x8;
    if (mod & KMOD_CTRL)  mods |= 0x10;
    if (mod & KMOD_ALT)   mods |= 0x20;
    NativeKbm_PushKey(vk, uint16_t(e.keysym.sym < 0x80 ? e.keysym.sym : 0), down,
                      e.repeat != 0, mods);
}

HostPadState ReadKeyboard()
{
    HostPadState s{};
    static bool f2WasDown = false;
    static bool f3WasDown = false;
    static bool f4WasDown = false;
    static bool f7WasDown = false;
    static bool f8WasDown = false;
    static bool f9WasDown = false;
    if (g_keyboardFocus)
    {
        const uint8_t* keys = SDL_GetKeyboardState(nullptr);
        const bool f2Down = keys[SDL_SCANCODE_F2] != 0;
        const bool f3Down = keys[SDL_SCANCODE_F3] != 0;
        const bool f4Down = keys[SDL_SCANCODE_F4] != 0;
        const bool f7Down = keys[SDL_SCANCODE_F7] != 0;
        if (f7Down && !f7WasDown)
            g_markPressed.store(true, std::memory_order_release);
        f7WasDown = f7Down;
        const bool f8Down = keys[SDL_SCANCODE_F8] != 0;
        if (f8Down && !f8WasDown)
            g_burstDumpPressed.store(true, std::memory_order_release);
        f8WasDown = f8Down;
        const bool f9Down = keys[SDL_SCANCODE_F9] != 0;
        if (f9Down && !f9WasDown)
            g_snapDumpPressed.store(true, std::memory_order_release);
        f9WasDown = f9Down;
        if (f2Down && !f2WasDown)
            g_debugJumpPressed.store(true, std::memory_order_release);
        f2WasDown = f2Down;
        if (f3Down && !f3WasDown)
            g_debugEnterPressed.store(true, std::memory_order_release);
        f3WasDown = f3Down;
        if (f4Down && !f4WasDown)
            g_debugMenuPressed.store(true, std::memory_order_release);
        f4WasDown = f4Down;

        // While the host overlay owns the keyboard, do not also hand its navigation
        // presses to the game as controller-2 input.
        if (g_debugOverlayVisible.load(std::memory_order_acquire))
            return s;

        // Part 92: with the NATIVE keyboard live — key bindings spliced into
        // port 0's own command layer and the KEY sources fed there — the merge
        // below runs in a REDUCED form. The first native build wrote sticks and
        // mouse buttons into the source records AFTER the pad's own conversion,
        // and that raced the title's per-frame publish (two writers, whichever
        // landed last at the copy won): aim on a HELD binding died outright and
        // movement flickered. So everything the pad conversion owns goes back
        // THROUGH the XInput state — one writer, real edge semantics, the
        // v1-proven channel: WASD -> left stick, mouse left -> X (attack),
        // mouse right -> the right trigger (aim), mouse middle -> R3 (heavy
        // attack). Keys stay native-only (their source cells have no other
        // writer), so no key arrives twice. The camera feeds the right stick
        // CLAMPED here and hands the unclamped remainder to the native layer,
        // which adds it after the title's own publish — that is what keeps the
        // DR2-PC no-ceiling feel without re-introducing the race.
        // EXCEPTION: while the host settings panel is up the full v1 merge
        // comes back for the panel's lifetime (the panel zeroes what the guest
        // sees anyway). CZ_NO_NATIVE_KBM=1 keeps the full v1 merge always.
        if (NativeKbm_Active() && !Settings_OverlayVisible())
        {
            s.thumbLX = KeyAxis(keys, SDL_SCANCODE_A, SDL_SCANCODE_D);
            s.thumbLY = KeyAxis(keys, SDL_SCANCODE_S, SDL_SCANCODE_W);
            if (g_relativeMouse)
            {
                const int dx = g_mouseDX.exchange(0, std::memory_order_relaxed);
                const int dy = g_mouseDY.exchange(0, std::memory_order_relaxed);
                // DIRECT CAMERA LOOK (imported from Case West, part 93): hand the raw
                // deltas to the native path, which adds them straight onto the camera's
                // yaw/pitch past the engine's radial turn-rate clamp (native_kbm.cpp,
                // sub_82471EA0). The stick feed below stays — it keeps the "camera is
                // being moved" state the engine reads — but the direct add is what
                // gives the uncapped speed that fixes the SENS-10 ceiling.
                NativeKbm_AddMouseLook(dx, dy);
                // RAW per-poll pixels -> stick units; deliberately no EMA and
                // no px/s conversion — DR2 PC's camera is displacement-shaped.
                // 350 (was 140): Case West's 2.5x-faster stick scale (commit 4eac54b);
                // with the direct look above this mainly keeps the input-active state.
                const float sens = float(Settings_MouseSens());
                const float unitsPerPx = sens * sens * 350.0f;
                const float rx = float(dx) * unitsPerPx;
                const float ry = float(-dy) * unitsPerPx;   // screen-down = look down
                auto clampStick = [](float v) {
                    return v > 32767.0f ? 32767.0f : (v < -32767.0f ? -32767.0f : v);
                };
                const float cx = clampStick(float(s.thumbRX) + rx);
                const float cy = clampStick(float(s.thumbRY) + ry);
                // the remainder above the stick's ceiling rides the native path
                NativeKbm_CameraSurplus((float(s.thumbRX) + rx - cx) / 32767.0f,
                                        (float(s.thumbRY) + ry - cy) / 32767.0f);
                s.thumbRX = int16_t(cx);
                s.thumbRY = int16_t(cy);
                const uint32_t mb = SDL_GetMouseState(nullptr, nullptr);
                if (mb & SDL_BUTTON(SDL_BUTTON_LEFT))
                    s.buttons |= XI_X;
                {
                    // BOTH triggers ride RMB: gun aim is the R2 source, and
                    // THROWING a held item needs the L2 source held (stock
                    // padmap: PLAYER_THROW = X pressed while BUTTON_L2 held —
                    // the throw tutorial's LT glyph). But they must NOT rise
                    // in the same instant: PLAYER_THROW_RT is "R2 PRESSED
                    // while L2 held", and simultaneous edges tripped it — the
                    // operator's item flew the moment RMB went down. So the
                    // aim trigger leads and the throw-enable trigger joins
                    // 70 ms later, the way a pad hand naturally staggers them;
                    // the throw itself is LMB (X), like DR2 PC.
                    static uint32_t rmbSince = 0;
                    if (mb & SDL_BUTTON(SDL_BUTTON_RIGHT))
                    {
                        const uint32_t now = SDL_GetTicks();
                        if (!rmbSince)
                            rmbSince = now;
                        s.rightTrigger = 255;
                        if (now - rmbSince >= 70)
                            s.leftTrigger = 255;
                    }
                    else
                        rmbSince = 0;
                }
                if (mb & SDL_BUTTON(SDL_BUTTON_MIDDLE))
                    s.buttons |= XI_RIGHT_THUMB;
            }
            return s;
        }

        for (const auto& k : kKeyMap)
            if (keys[k.scancode])
                s.buttons |= k.button;

        s.thumbLX = KeyAxis(keys, SDL_SCANCODE_A, SDL_SCANCODE_D);
        s.thumbLY = KeyAxis(keys, SDL_SCANCODE_S, SDL_SCANCODE_W);
        s.thumbRX = KeyAxis(keys, SDL_SCANCODE_J, SDL_SCANCODE_L);
        s.thumbRY = KeyAxis(keys, SDL_SCANCODE_K, SDL_SCANCODE_I);
        if (keys[SDL_SCANCODE_1])
            s.leftTrigger = 255;
        if (keys[SDL_SCANCODE_3])
            s.rightTrigger = 255;

        // THE MOUSE (part 91) — see the g_mouseDX comment for why this exists at
        // all. A mouse is a VELOCITY device and a stick is a DEFLECTION device, so
        // the deltas since the last loop become px/s, scale into deflection by the
        // panel's sensitivity, and pass through a short EMA so per-loop delta
        // granularity does not read as jitter. A stopped mouse decays hard toward
        // zero — a camera that keeps drifting after the hand stops is the one thing
        // every first mouse-look implementation ships.
        if (g_relativeMouse)
        {
            static auto lastPoll = std::chrono::steady_clock::now();
            static float emaX = 0.0f, emaY = 0.0f;
            const auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - lastPoll).count();
            lastPoll = now;
            if (dt <= 0.0f || dt > 0.25f)
                dt = 1.0f / 250.0f;
            const int dx = g_mouseDX.exchange(0, std::memory_order_relaxed);
            const int dy = g_mouseDY.exchange(0, std::memory_order_relaxed);
            // Full deflection at ~32767/k px/s of hand speed. The first scale
            // (sens*6.5, ~1000 px/s at 5) read as "too slow even at max" on the
            // operator's 3440 monitor: through a stick API the camera can never
            // exceed the game's full-deflection turn rate, so the only useful
            // mapping is one where ordinary hand speed PEGS the stick and the
            // knob decides how ordinary. Quadratic so the top rungs get properly
            // hot: sens 5 pegs at ~520 px/s, sens 10 at ~130.
            const float sensV = float(Settings_MouseSens());
            const float k = sensV * sensV * 2.5f;
            const float alpha = std::min(1.0f, dt * 45.0f);
            if (dx == 0 && dy == 0)
            {
                emaX *= 0.5f;
                emaY *= 0.5f;
            }
            else
            {
                emaX += ((float(dx) / dt) * k - emaX) * alpha;
                emaY += ((float(dy) / dt) * k - emaY) * alpha;
            }
            auto clampStick = [](float v) {
                return int16_t(v > 32767.0f ? 32767.0f
                                            : (v < -32767.0f ? -32767.0f : v));
            };
            if (emaX != 0.0f)
                s.thumbRX = clampStick(float(s.thumbRX) + emaX);
            if (emaY != 0.0f)
                s.thumbRY = clampStick(float(s.thumbRY) - emaY);   // screen-down = look down
            // Buttons: attack, aim, the two spares on LB/RB. SDL's mouse state is
            // maintained by the same event pump this loop just ran.
            const uint32_t mb = SDL_GetMouseState(nullptr, nullptr);
            if (mb & SDL_BUTTON(SDL_BUTTON_LEFT))
                s.buttons |= XI_X;
            if (mb & SDL_BUTTON(SDL_BUTTON_RIGHT))
                s.rightTrigger = 255;
            if (mb & SDL_BUTTON(SDL_BUTTON_MIDDLE))
                s.buttons |= XI_Y;
            if (mb & SDL_BUTTON(SDL_BUTTON_X1))
                s.buttons |= XI_LEFT_SHOULDER;
            if (mb & SDL_BUTTON(SDL_BUTTON_X2))
                s.buttons |= XI_RIGHT_SHOULDER;
        }
    }

    return s;
}

HostPadState ReadController()
{
    HostPadState s{};
    if (g_controller)
    {
        for (const auto& p : kPadMap)
            if (SDL_GameControllerGetButton(g_controller, p.sdl))
                s.buttons |= p.button;

        const int lx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        const int rx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        s.thumbLX = int16_t(lx);
        s.thumbRX = int16_t(rx);
        s.thumbLY = PadAxisY(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
        s.thumbRY = PadAxisY(g_controller, SDL_CONTROLLER_AXIS_RIGHTY);

        // SDL reports triggers as 0..32767; XInput's are 0..255.
        const int lt =
            SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7;
        const int rt =
            SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
        s.leftTrigger = uint8_t(lt < 0 ? 0 : lt > 255 ? 255 : lt);
        s.rightTrigger = uint8_t(rt < 0 ? 0 : rt > 255 ? 255 : rt);
    }
    return s;
}

bool SameState(const HostPadState& a, const HostPadState& b)
{
    return a.buttons == b.buttons && a.leftTrigger == b.leftTrigger &&
           a.rightTrigger == b.rightTrigger && a.thumbLX == b.thumbLX &&
           a.thumbLY == b.thumbLY && a.thumbRX == b.thumbRX && a.thumbRY == b.thumbRY;
}

// THE PACKET NUMBER IS A CONTRACT, NOT A COUNTER.
//
// XInput changes dwPacketNumber only when the state changes, and a title is entitled
// to compare it against the previous poll and skip re-reading the gamepad struct
// entirely. So a packet number that ticks every poll is not merely wasteful — and a
// constant one with a changing button field hands the guest a press it may
// legitimately ignore. This is the only place the number moves.
void PublishPad(uint32_t userIndex, const HostPadState& fresh)
{
    std::lock_guard<std::mutex> lock(g_padMutex);
    HostPadState& pad = g_pads[userIndex];
    if (SameState(fresh, pad))
        return;
    const uint32_t packet = pad.packet + 1;
    pad = fresh;
    pad.packet = packet;
    if (g_inputTrace)
    {
        // The decoded form, built first so the raw fields can still be printed beside it.
        // BOTH are on the line deliberately: the names are what a recipe is written from,
        // and the raw mask is what says the decoder missed a bit rather than the pad being
        // idle — a decoder with no raw column next to it cannot be shown to be complete.
        char names[192] = "";
        for (const TracedButton& b : kTracedButtons)
            if (fresh.buttons & b.mask)
                snprintf(names + strlen(names), sizeof names - strlen(names), ",%s", b.name);
        AppendStick(names, sizeof names, "LSLEFT", "LSRIGHT", fresh.thumbLX);
        AppendStick(names, sizeof names, "LSDOWN", "LSUP", fresh.thumbLY);
        AppendStick(names, sizeof names, "RSLEFT", "RSRIGHT", fresh.thumbRX);
        AppendStick(names, sizeof names, "RSDOWN", "RSUP", fresh.thumbRY);
        if (fresh.leftTrigger > 127)
            snprintf(names + strlen(names), sizeof names - strlen(names), ",LT");
        if (fresh.rightTrigger > 127)
            snprintf(names + strlen(names), sizeof names - strlen(names), ",RT");
        const long long ms = InputElapsedMs();
        fprintf(stderr,
                "[input] t=%lld.%03llds pad %u packet %u  %-24s | "
                "buttons=%04X triggers=%u/%u L=(%d,%d) R=(%d,%d)\n",
                ms / 1000, ms % 1000, userIndex, packet,
                names[0] ? names + 1 : "NONE (released)", fresh.buttons,
                fresh.leftTrigger, fresh.rightTrigger, fresh.thumbLX, fresh.thumbLY,
                fresh.thumbRX, fresh.thumbRY);
    }
}

void Shutdown(const char* why)
{
    fprintf(stderr, "[host] %s — closing the window and exiting.\n", why);
    // The renderer's counter dump, BEFORE _Exit, or an operator session's counters
    // simply vanish — part 38 lost a whole evening's alpha-mode census this way: the
    // stats block only ran from paths that returned through main, and the window-close
    // path never did. Reading counters is safe here (the guest threads only ever
    // increment them), and the one session that most needs the numbers — a long
    // operator play session — is exactly the one that ends by closing the window.
    ::VkRenderer_DumpStats();
    // PART 71: and write the pipeline cache back, HERE rather than inside DumpStats —
    // see the header comment on why. This is the normal quit path, so it is the one that
    // actually has to fire for the next launch to be warm.
    ::VkRenderer_SavePipelineCache();
    fflush(nullptr);
    // _Exit, not exit: guest threads are still running recompiled code against guest
    // memory, and running static destructors underneath them would turn an ordinary
    // quit into a crash report about a subsystem that was working.
    std::_Exit(0);
}

} // namespace

// ---- THE FIRST-RUN PROGRESS WINDOW (release §2.3, part 85) ----------------------
// See window.h. A separate, deliberately plain window rather than the game window
// early: Host_WindowInit's window carries the whole present-seam decision (Vulkan
// flag, renderer, settings) and none of that exists yet when the extract runs.
// This one is an SDL_Renderer, a background, the shared 5x7 glyphs and one bar —
// created for the first-run work, destroyed before the real window is born.
namespace
{
SDL_Window* g_progWindow = nullptr;
SDL_Renderer* g_progRenderer = nullptr;
std::string g_progTitle;
uint32_t g_progLastDraw = 0;
} // namespace

bool Host_ProgressBegin(const char* title)
{
    if (getenv("CZ_NO_WINDOW"))
        return false;
    if (g_progWindow)
        return true;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "[host] progress window: SDL video init failed (%s) — "
                        "console lines only.\n", SDL_GetError());
        return false;
    }
    g_progWindow = SDL_CreateWindow("Dead Rising 2: Case Zero",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    640, 200, SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_progWindow)
        return false;
    g_progRenderer = SDL_CreateRenderer(g_progWindow, -1, 0);
    if (!g_progRenderer)
    {
        SDL_DestroyWindow(g_progWindow);
        g_progWindow = nullptr;
        return false;
    }
    g_progTitle = title ? title : "";
    g_progLastDraw = 0;
    Host_ProgressUpdate("", 0.0f);
    return true;
}

void Host_ProgressUpdate(const char* line, float fraction)
{
    if (!g_progRenderer)
        return;
    // Pump so the compositor never marks the window unresponsive; drop every event —
    // there is nothing to interact with, and a close request during a 30 s one-time
    // step is better honoured by letting the step finish.
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    // Rate-limit the drawing, not the callers: the extract reports per file and the
    // shader build per shader, and neither should pay for a present each time.
    const uint32_t now = SDL_GetTicks();
    if (g_progLastDraw && now - g_progLastDraw < 33 && fraction < 1.0f)
        return;
    g_progLastDraw = now;

    SDL_SetRenderDrawColor(g_progRenderer, 20, 22, 26, 255);
    SDL_RenderClear(g_progRenderer);
    auto text = [&](int tx, int ty, const char* str, int scale,
                    uint8_t r, uint8_t g, uint8_t b) {
        SDL_SetRenderDrawColor(g_progRenderer, r, g, b, 255);
        for (const char* p = str; *p; ++p)
        {
            if (const char* bits = Glyph(*p))
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if (bits[row * 5 + col] == '1')
                        {
                            SDL_Rect px{tx + col * scale, ty + row * scale, scale, scale};
                            SDL_RenderFillRect(g_progRenderer, &px);
                        }
            tx += 6 * scale;
        }
    };
    text(24, 24, g_progTitle.c_str(), 3, 245, 235, 200);
    if (line && *line)
        text(24, 70, line, 2, 160, 160, 170);
    const int barX = 24, barY = 120, barW = 640 - 48, barH = 22;
    SDL_SetRenderDrawColor(g_progRenderer, 70, 70, 80, 255);
    SDL_Rect frame{barX, barY, barW, barH};
    SDL_RenderFillRect(g_progRenderer, &frame);
    const float f = fraction < 0.f ? 0.f : fraction > 1.f ? 1.f : fraction;
    SDL_SetRenderDrawColor(g_progRenderer, 200, 170, 60, 255);
    SDL_Rect fill{barX + 2, barY + 2, int((barW - 4) * f), barH - 4};
    if (fill.w > 0)
        SDL_RenderFillRect(g_progRenderer, &fill);
    SDL_RenderPresent(g_progRenderer);
}

void Host_ProgressEnd()
{
    if (g_progRenderer)
        SDL_DestroyRenderer(g_progRenderer);
    if (g_progWindow)
        SDL_DestroyWindow(g_progWindow);
    g_progRenderer = nullptr;
    g_progWindow = nullptr;
    // The video subsystem stays up: the real window is usually created next, and
    // tearing SDL down between the two would only add a flash and a race.
}

// ---- THE LAUNCHER (part 86) -----------------------------------------------------
// See window.h. Same construction as the progress window — plain SDL_Renderer, the
// shared glyphs — but interactive and modal. It deliberately owns no game state:
// every row reads and writes through the Settings_* API the in-game panel uses, so
// the two can never disagree about what a setting means, and the install path is
// the same StfsExtract the automatic first run uses.
namespace
{
// The resolutions the launcher cycles through: the common 16:9 ladder, filtered by
// the same validity rule the settings system enforces. The display's own size is
// appended when it is not already present, so "native" is always reachable.
const uint32_t kLauncherRes[][2] = {
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
};

void LauncherText(SDL_Renderer* r, int tx, int ty, const std::string& str, int scale,
                  uint8_t cr, uint8_t cg, uint8_t cb)
{
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    for (char c : str)
    {
        if (const char* bits = Glyph(c))
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (bits[row * 5 + col] == '1')
                    {
                        SDL_Rect px{tx + col * scale, ty + row * scale, scale, scale};
                        SDL_RenderFillRect(r, &px);
                    }
        tx += 6 * scale;
    }
}
} // namespace

bool Host_RunLauncher()
{
    if (getenv("CZ_NO_WINDOW"))
        return true;
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "[launcher] SDL video init failed (%s) — continuing without\n",
                SDL_GetError());
        return true;
    }
    // A display-less environment (a container, a CI box) can still pass SDL init on
    // the DUMMY/OFFSCREEN driver — and a modal loop under a driver that can never
    // deliver input is a hang, not a launcher. Part 86 shipped exactly that hang into
    // the clean-container gate's first-run refusal step before this check existed:
    // the header's "must never take a gate run hostage" promise needs all THREE
    // guards, not two.
    if (const char* drv = SDL_GetCurrentVideoDriver();
        drv && (strcmp(drv, "dummy") == 0 || strcmp(drv, "offscreen") == 0))
    {
        fprintf(stderr, "[launcher] SDL video driver is '%s' (no real display) — "
                        "continuing without the launcher\n", drv);
        return true;
    }
    SDL_Window* win = SDL_CreateWindow("Dead Rising 2: Case Zero",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       720, 420, SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win)
        return true;
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren)
    {
        SDL_DestroyWindow(win);
        return true;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    // The dropped-package install writes its progress into THIS window; declared
    // before the loop so the drop handler below can call it.
    auto drawProgress = [&](const std::string& line, float f) {
        SDL_SetRenderDrawColor(ren, 20, 22, 26, 255);
        SDL_RenderClear(ren);
        LauncherText(ren, 24, 24, "INSTALLING", 3, 245, 235, 200);
        LauncherText(ren, 24, 80, line, 2, 160, 160, 170);
        const int bx = 24, by = 130, bw = 720 - 48, bh = 22;
        SDL_SetRenderDrawColor(ren, 70, 70, 80, 255);
        SDL_Rect frame{bx, by, bw, bh};
        SDL_RenderFillRect(ren, &frame);
        SDL_SetRenderDrawColor(ren, 200, 170, 60, 255);
        SDL_Rect fill{bx + 2, by + 2, int((bw - 4) * (f < 0 ? 0 : f > 1 ? 1 : f)), bh - 4};
        if (fill.w > 0)
            SDL_RenderFillRect(ren, &fill);
        SDL_RenderPresent(ren);
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    };

    int sel = 0;
    std::string notice;
    bool play = false, quit = false;
    while (!play && !quit)
    {
        // ---- state read fresh every frame, through the same API the game uses ----
        const bool installed = std::filesystem::is_regular_file(HostPaths::GameXex());
        uint32_t rw = 0, rh = 0;
        Settings_InternalRes(rw, rh);
        char resBuf[32];
        snprintf(resBuf, sizeof resBuf, "%ux%u", rw, rh);
        char fpsBuf[16];
        snprintf(fpsBuf, sizeof fpsBuf, "%d", Settings_FpsCap());
        char fovBuf[16];
        snprintf(fovBuf, sizeof fovBuf, "+%d", Settings_Fov());
        static const char* kShadowNames[] = { "LOW", "MEDIUM", "HIGH" };
        static const char* kDispNames[] = { "WINDOW", "BORDERLESS", "FULLSCREEN" };
        struct Row { const char* label; std::string value; };
        const Row rows[] = {
            { "PLAY", installed ? "" : "(GAME NOT INSTALLED YET)" },
            { "DISPLAY MODE", kDispNames[int(Settings_DisplayMode()) % 3] },
            { "RESOLUTION", resBuf },
            { "VSYNC", Settings_VSync() ? "ON" : "OFF" },
            { "SHADOWS", kShadowNames[Settings_ShadowTier() % 3] },
            { "FPS CAP", Settings_FpsCap() ? fpsBuf : "OFF" },
            { "FOV", Settings_Fov() ? fovBuf : "DEFAULT" },
        };
        constexpr int kRows = int(sizeof(rows) / sizeof(rows[0]));

        // ---- draw ----
        SDL_SetRenderDrawColor(ren, 20, 22, 26, 255);
        SDL_RenderClear(ren);
        LauncherText(ren, 24, 20, "DEAD RISING 2 - CASE ZERO", 3, 245, 235, 200);
        LauncherText(ren, 24, 52, "UP/DOWN SELECT   LEFT/RIGHT CHANGE   ENTER PLAY", 2,
                     130, 130, 140);
        for (int i = 0; i < kRows; ++i)
        {
            const int y = 96 + i * 34;
            if (i == sel)
            {
                SDL_SetRenderDrawColor(ren, 45, 48, 56, 255);
                SDL_Rect hi{16, y - 6, 720 - 32, 30};
                SDL_RenderFillRect(ren, &hi);
            }
            LauncherText(ren, 24, y, rows[i].label, 2,
                         i == sel ? 245 : 190, i == sel ? 235 : 190, i == sel ? 200 : 195);
            LauncherText(ren, 300, y, rows[i].value, 2, 200, 170, 60);
        }
        const std::string foot = notice.empty()
            ? (installed ? "GAME INSTALLED"
                         : "DROP YOUR XBLA PACKAGE FILE ONTO THIS WINDOW TO INSTALL")
            : notice;
        LauncherText(ren, 24, 96 + kRows * 34 + 14, foot, 2, 160, 160, 170);
        SDL_RenderPresent(ren);

        // ---- input ----
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 250))
            continue;
        switch (e.type)
        {
        case SDL_QUIT:
            quit = true;
            break;
        case SDL_DROPFILE:
        {
            const std::string dropped = e.drop.file;
            SDL_free(e.drop.file);
            // Identity first, size second — the same two questions first_run asks.
            char magic[5] = {};
            if (FILE* f = fopen(dropped.c_str(), "rb"))
            {
                if (fread(magic, 1, 4, f) != 4)
                    magic[0] = 0;
                fclose(f);
            }
            const std::string m = magic;
            if (m != "LIVE" && m != "CON " && m != "PIRS")
            {
                notice = "NOT AN XBOX 360 PACKAGE - IT BEGINS \"" + m + "\"";
                break;
            }
            // Copy it into assets/package (the layout's contract: your package,
            // kept, so the game can always be re-extracted), then extract.
            std::error_code ec;
            const auto pkgDir = HostPaths::Package() / "dropped";
            std::filesystem::create_directories(pkgDir, ec);
            const auto pkgDest = pkgDir / std::filesystem::path(dropped).filename();
            drawProgress("COPYING PACKAGE...", 0.1f);
            std::filesystem::copy_file(dropped, pkgDest,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                notice = "COULD NOT COPY THE PACKAGE IN: " + ec.message();
                break;
            }
            std::string err;
            const bool ok = StfsExtract::Extract(pkgDest, HostPaths::Game(), err,
                [&](uint64_t done, uint64_t total) {
                    char l[64];
                    snprintf(l, sizeof l, "UNPACKING - %u OF %u MB",
                             unsigned(done >> 20), unsigned(total >> 20));
                    drawProgress(l, total ? float(double(done) / double(total)) : 1.f);
                });
            notice = ok ? "INSTALLED - PRESS ENTER TO PLAY"
                        : "INSTALL FAILED: " + err.substr(0, 48);
            break;
        }
        case SDL_KEYDOWN:
        {
            const SDL_Keycode k = e.key.keysym.sym;
            const int dir = (k == SDLK_LEFT) ? -1 : (k == SDLK_RIGHT) ? 1 : 0;
            if (k == SDLK_UP)
                sel = (sel + kRows - 1) % kRows;
            else if (k == SDLK_DOWN)
                sel = (sel + 1) % kRows;
            else if (k == SDLK_RETURN && sel == 0)
                play = true;
            else if (k == SDLK_ESCAPE)
                quit = true;
            else if (dir)
                switch (sel)
                {
                case 1:
                    Settings_SetDisplayMode(
                        CzDisplayMode((int(Settings_DisplayMode()) + dir + 3) % 3));
                    break;
                case 2:
                {
                    // Cycle the ladder; the display's own size joins it so "native"
                    // is always one press away even on odd panels.
                    std::vector<std::pair<uint32_t, uint32_t>> list;
                    for (const auto& p : kLauncherRes)
                        if (Settings_ValidInternalRes(p[0], p[1]))
                            list.push_back({p[0], p[1]});
                    uint32_t dw = 0, dh = 0;
                    if (Host_DisplaySize(&dw, &dh) && Settings_ValidInternalRes(dw, dh))
                    {
                        bool have = false;
                        for (auto& p : list)
                            if (p.first == dw && p.second == dh)
                                have = true;
                        if (!have)
                            list.push_back({dw, dh});
                    }
                    if (list.empty())
                        break;
                    int cur = 0;
                    for (int i = 0; i < int(list.size()); ++i)
                        if (list[i].first == rw && list[i].second == rh)
                            cur = i;
                    const auto& next =
                        list[(cur + dir + int(list.size())) % int(list.size())];
                    Settings_SetInternalRes(next.first, next.second);
                    break;
                }
                case 3:
                    Settings_SetVSync(!Settings_VSync());
                    break;
                case 4:
                    Settings_SetShadowTier((Settings_ShadowTier() + dir + 3) % 3);
                    break;
                case 5:
                {
                    static const int caps[] = { 0, 30, 60, 120 };
                    int cur = 0;
                    for (int i = 0; i < 4; ++i)
                        if (caps[i] == Settings_FpsCap())
                            cur = i;
                    Settings_SetFpsCap(caps[(cur + dir + 4) % 4]);
                    break;
                }
                case 6:
                    Settings_SetFov(std::clamp(Settings_Fov() + dir * 5, 0, 20));
                    break;
                }
            break;
        }
        }
    }

    if (play)
        Settings_Save();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    // Video stays initialized — the progress window or the game window comes next.
    return play;
}

bool Host_WindowInit()
{
    // Leave SIGINT/SIGTERM alone. SDL would otherwise install handlers that turn them
    // into an SDL_QUIT event, which sounds like an improvement and is not: every gate
    // run in this project is `timeout N ./cz_runtime`, and routing SIGTERM through our
    // event loop makes process termination depend on that loop still being alive. The
    // whole point of a gate is that it terminates the same way whatever the runtime is
    // doing.
    //
    // THIS SITS ABOVE THE HEADLESS EARLY RETURN, and that is the whole of part 30's
    // fix. It used to sit below, next to `SDL_Init`, which was correct for as long as
    // the window was the only thing in this runtime that touched SDL. Phase A/V added a
    // second, independent entry point — `Audio_Out_Init` calls
    // `SDL_InitSubSystem(SDL_INIT_AUDIO)` from a guest thread — so a `CZ_NO_WINDOW=1`
    // run returned here before the hint was ever set and then let the audio device
    // install the handlers this comment exists to prevent. The effect was that every
    // headless run with sound IGNORED `timeout` and ran until something killed it:
    // measured at `timeout 20` as exit 124 at 20 s with `CZ_NO_AUDIO_OUT=1` and still
    // alive at 180 s without it. Setting a hint is idempotent and costs nothing, so it
    // belongs at the first point in the process that is guaranteed to run before any
    // SDL call — `main()` calls this at line 310 and does not spawn the guest thread
    // until line 338.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if (getenv("CZ_NO_WINDOW"))
    {
        fprintf(stderr, "[host] CZ_NO_WINDOW=1 — headless: no window, no present, and "
                        "XamInputGetState answers with its neutral pad. This is the "
                        "control arm for every claim about phase 3.\n");
        return false;
    }

    // The signal-handler hint is set at the top of this function, above the headless
    // early return, so that a run with no window still gets it — see the comment there.

#if defined(_WIN32)
    // DPI AWARENESS, BEFORE SDL_Init, AND IT IS NOT COSMETIC.
    //
    // A process that has not declared itself DPI-aware is lied to by Windows: every
    // display query returns the desktop divided by the scale factor. On a 2560x1440
    // panel at 160% scaling, SDL_GetDesktopDisplayMode reports 1600x900 — and
    // PublishDisplaySize below CLAMPS the settings panel's resolution list to that, so
    // the operator's 1440p screen offered 1600x900 as its maximum and there was no way
    // to ask for more. The renderer was never the limit; the query was.
    //
    // permonitorv2 rather than "system": the size must stay right when the window is
    // dragged to a differently-scaled monitor, which is the case "system" gets wrong
    // and is exactly what a laptop with an external display does.
    //
    // SDL_HINT_WINDOWS_DPI_SCALING is deliberately NOT set. It would make SDL report
    // window sizes in DPI-scaled points; this runtime wants physical pixels everywhere,
    // because the swapchain, the scissor rectangles and every recorded frame time are
    // in pixels.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr,
                "[host] SDL_Init failed: %s\n"
                "[host] RUNNING HEADLESS. The boot will reach the title screen and "
                "stop there, because nothing can press a button (finding 37).\n",
                SDL_GetError());
        return false;
    }

    // CZ_VK_SWAPCHAIN=1 — the renderer presents its own image through a Vulkan
    // swapchain on this window instead of reading it back and handing us pixels.
    //
    // THE DECISION HAS TO BE MADE HERE, before the window exists, and that is the whole
    // reason this arm is an env var read in Host_WindowInit rather than a renderer
    // option: `SDL_WINDOW_VULKAN` cannot be added to a window afterwards, and a window
    // carrying it cannot also carry an `SDL_Renderer` (SDL2 has no Vulkan renderer
    // backend — its accelerated backends are GL/GLES/D3D/Metal). So the two present
    // paths are mutually exclusive by construction, which is the honest shape: they are
    // two arms, not a fallback chain.
    // THE DEFAULT SINCE PART 54, on the operator's decision after judging both arms.
    //
    // `CZ_VK_NO_SWAPCHAIN=1` is the control arm and restores the readback present path
    // exactly — three full-frame copies and a GPU->CPU->GPU round trip — which is what
    // every measurement before part 54 was taken on. It is not deprecated: it is the arm
    // that any future present-path claim has to be compared against.
    //
    // WHAT DECIDED IT, recorded here because a default with no reason attached is one
    // nobody can re-open. Their soak A/B, both arms in one session: −21.1% of the frame at
    // ~2,400 draws and −3.5% (+2.4 fps) at ~6,800, which is where they play; and a
    // smoothness win that the frame rate does not carry — frame-time mean against median
    // IN TRANSIT is +3.3% for MAILBOX against +5.9% for the compositor-paced SDL present.
    // The picture correlates with hardware's own screenshot at both internal resolutions.
    //
    // NOTHING IS LOST WITH IT. The one thing this arm could not do was draw the F4 debug
    // overlay, and that was ported rather than accepted (see the blit in vk_renderer.cpp),
    // because a default that quietly removes a feature is exactly the shape this project
    // spends its time undoing.
    g_wantVulkanSwapchain = getenv("CZ_VK_NO_SWAPCHAIN") == nullptr;

    // CZ_WINDOW_SIZE=WxH and CZ_WINDOW_MAXIMIZED=1 — the window as a CONTROLLED VARIABLE.
    //
    // Set at CREATION, not by resizing afterwards, and part 54 paid for the difference.
    // A present-path A/B has to hold the window fixed across its arms (gotcha 353), and
    // the obvious way — `CZ_WINDOW_RESIZE_AT`, which exists as the positive control for the
    // swapchain rebuild — turned out to be useless for it: the window bounced through
    // 1280x720, 1088x613, 2560x1417, 1088x613, 3012x1600, 3544x1881 in a single run as
    // the compositor placed it and SDL's scale conversion argued with it. A variable that
    // moves six times during the run it is meant to hold constant is not a control.
    //
    // MAXIMIZED is the one to reach for, because it is what the operator actually plays
    // and it needs no arithmetic about display scale — this desktop has one output at 85%
    // and one at 100%, so a logical size means two different pixel counts depending on
    // where the window lands.
    int startW = kDefaultWidth, startH = kDefaultHeight;
    if (const char* ws = getenv("CZ_WINDOW_SIZE"))
    {
        int w = 0, h = 0;
        if (sscanf(ws, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 16384 &&
            h <= 16384)
        {
            startW = w;
            startH = h;
        }
        else
            fprintf(stderr, "[host] CZ_WINDOW_SIZE=%s is not a usable WxH — IGNORED.\n", ws);
    }
    // The persisted display mode (part 60's PC options screen). CZ_WINDOW_SIZE and
    // CZ_WINDOW_MAXIMIZED are measurement controls and win over it: a run pinning the
    // window for an A/B must not have the settings file silently un-pin it.
    Uint32 modeFlag = 0;
    if (!getenv("CZ_WINDOW_SIZE") && !getenv("CZ_WINDOW_MAXIMIZED"))
    {
        switch (Settings_DisplayMode())
        {
            case CzDisplayMode::Borderless: modeFlag = SDL_WINDOW_FULLSCREEN_DESKTOP; break;
            case CzDisplayMode::Fullscreen: modeFlag = SDL_WINDOW_FULLSCREEN_DESKTOP; break;
            case CzDisplayMode::Windowed: default: break;
        }
        // Exclusive fullscreen is applied AFTER creation (below): creating directly
        // with SDL_WINDOW_FULLSCREEN would mode-switch the display to the window's
        // 1280x720 creation size, which is never what "Fullscreen" means today.
    }
    const Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | modeFlag |
                               (g_wantVulkanSwapchain ? SDL_WINDOW_VULKAN : 0u) |
                               (getenv("CZ_WINDOW_MAXIMIZED") ? SDL_WINDOW_MAXIMIZED : 0u);
    g_window = SDL_CreateWindow("Dead Rising 2: Case Zero", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, startW, startH,
                                windowFlags);
    if (!g_window && g_wantVulkanSwapchain)
    {
        // Losing the Vulkan flag must not silently cost the window, and it must not
        // silently cost the ARM either: falling back to the copy path while the operator
        // believes they are measuring the swapchain would make the A/B report zero and
        // look like a null result (gotcha 7). So it says which of the two happened.
        fprintf(stderr, "[host] SDL_CreateWindow with SDL_WINDOW_VULKAN failed: %s — "
                        "the swapchain present path is NOT in force; falling back to the "
                        "readback present path. This is a DEGRADED default, not a "
                        "configuration: expect the frame times of part 53.\n",
                SDL_GetError());
        g_wantVulkanSwapchain = false;
        g_window = SDL_CreateWindow("Dead Rising 2: Case Zero", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, startW, startH,
                                    windowFlags & ~Uint32(SDL_WINDOW_VULKAN));
    }
    if (!g_window)
    {
        fprintf(stderr, "[host] SDL_CreateWindow failed: %s — RUNNING HEADLESS.\n",
                SDL_GetError());
        SDL_Quit();
        return false;
    }

    // The persisted EXCLUSIVE fullscreen upgrades the borderless creation flag here,
    // once the window exists to measure its display against (see the flags comment).
    if (modeFlag != 0 && Settings_DisplayMode() == CzDisplayMode::Fullscreen)
        ApplyDisplayModeNow(CzDisplayMode::Fullscreen);

    // SDL2 starts TEXT INPUT by default on desktop, which routes held keys through
    // the OS input method — on the operator's Linux desktop, HOLDING a letter popped
    // the IME's accent picker (à á â...) and the raw keydown was delayed by the
    // press-and-hold timeout. That WAS the part-91/92 "A/S/D take a second to move"
    // symptom: the pad was instant, the headless probes (no SDL, no IME) measured
    // the delivery instant, and only the real desktop path lagged — the delay lived
    // in the input method, before this process ever saw the key. A game window
    // wants scancodes, not composed text.
    SDL_StopTextInput();

    // No SDL_RENDERER_PRESENTVSYNC. The guest's swap rate is the frame clock here
    // (one XE_SWAP per frame, verified against B1), and a vsync-paced present would
    // add a second clock that silently becomes the slower of the two.
    //
    // NOT ASKING FOR VSYNC IS NOT THE SAME AS ASKING FOR NO VSYNC, and part 49 paid for
    // the difference. Omitting the flag leaves the decision to the backend, and a
    // compositor throttles `SDL_RenderPresent` to the display refresh whatever SDL was
    // asked for. That is invisible while the guest is capped at 30 fps — the second
    // clock is the faster one and never binds — and the moment `CZ_FPS_CAP=60` lifted
    // the guest's own cap it became the binding one. Its failure mode is the sharp one:
    // with no triple buffering a frame that takes just OVER 16.67 ms cannot present
    // until the NEXT refresh, so the frame rate snaps 60 -> 30 with nothing in between,
    // which is exactly what the operator reported ("it seems to go back to around
    // 30 fps, pretty sure it's vsync"). Headless reads 62.5 fps because there is no
    // window and therefore no compositor in the path — the arm that localises this.
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    // No SDL_Renderer on a Vulkan window: the renderer thread owns presentation from
    // here on, and everything below that touches `g_renderer` is skipped. What that
    // costs is named out loud at the end of this function rather than discovered later.
    g_renderer = g_wantVulkanSwapchain
        ? nullptr
        : SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer && !g_wantVulkanSwapchain)
    {
        fprintf(stderr, "[host] accelerated renderer unavailable (%s) — falling back "
                        "to software.\n",
                SDL_GetError());
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer && !g_wantVulkanSwapchain)
    {
        fprintf(stderr, "[host] SDL_CreateRenderer failed: %s — RUNNING HEADLESS.\n",
                SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        SDL_Quit();
        return false;
    }

    g_inputTrace = getenv("CZ_INPUT_TRACE") != nullptr;
    g_active = true;

    // ...and say so out loud, per renderer, because the hint above is a REQUEST. SDL
    // 2.0.18 added the explicit call, which is the one that can also FAIL and say so —
    // and under a compositor that owns presentation it may well fail, in which case the
    // right answer is to know that rather than to believe the hint took.
    //
    // CZ_HOST_VSYNC=1 puts it back on: the same-binary control arm, so "the frame rate
    // is capped by the display" and "the frame rate is capped by our own work" can be
    // told apart in one run instead of argued about.
    const bool wantVsync = getenv("CZ_HOST_VSYNC") != nullptr;
    if (g_renderer)
    {
#if SDL_VERSION_ATLEAST(2, 0, 18)
        const int vsRc = SDL_RenderSetVSync(g_renderer, wantVsync ? 1 : 0);
#else
        const int vsRc = -1;
#endif
        SDL_RendererInfo ri{};
        SDL_GetRendererInfo(g_renderer, &ri);
        fprintf(stderr,
                "[host] present vsync: requested %s, SDL_RenderSetVSync %s, renderer "
                "reports PRESENTVSYNC %s\n",
                wantVsync ? "ON (CZ_HOST_VSYNC=1)" : "OFF",
                vsRc == 0 ? "accepted" : "UNAVAILABLE (hint only)",
                (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? "SET — the display is still "
                                                         "pacing us, and the frame rate "
                                                         "above 60 fps will be its "
                                                         "refresh rate"
                                                       : "clear");
    }
    else
    {
        // The swapchain arm chooses its own present mode inside the renderer, and
        // CZ_HOST_VSYNC is meaningless here — say so rather than letting a run be
        // configured with a flag that does nothing (gotcha 5).
        fprintf(stderr,
                "[host] swapchain present (the default; CZ_VK_NO_SWAPCHAIN=1 is the "
                "control arm): this window carries SDL_WINDOW_VULKAN and has "
                "NO SDL_Renderer. The renderer presents its own image; the present "
                "readback and its two copies do not run, and the present MODE is chosen "
                "by the renderer (see the [vk] swapchain line), not by SDL.%s\n"
                "[host] WHAT THIS ARM COSTS, said out loud: the host-rendered F4 debug "
                "overlay is drawn HERE rather than by SDL, at a fixed 1280x720 scaled "
                "to the window, with an opaque panel instead of SDL's 88%% one.\n",
                wantVsync ? " CZ_HOST_VSYNC=1 is IGNORED here." : "");
    }
    PublishDrawableSize();
    PublishDisplaySize();

    fprintf(stderr, "[host] window %dx%d up on SDL video driver '%s'.\n", kDefaultWidth,
            kDefaultHeight, SDL_GetCurrentVideoDriver());
    // The startup message states which of the two present modes this run is in,
    // because a stale claim here is worse than none: this line said "THE WINDOW IS
    // EXPECTED TO BE BLANK: there is no renderer until phase 5" for two sessions after
    // the renderer existed, contradicting the picture on screen — and the operator was
    // the one who had to ignore it.
    if (getenv("CZ_VKDRAW"))
        fprintf(stderr, "[host] renderer requested (CZ_VKDRAW): the window shows the "
                        "rendered frame once VkRenderer_Init succeeds — see the [vk] "
                        "lines for whether it did.\n");
    else
        fprintf(stderr, "[host] no renderer requested (CZ_VKDRAW unset): the window "
                        "presents a flat clear on purpose. The title bar carries the "
                        "live frame count, which is what says the present seam runs.\n");
    PrintKeyMap();

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        OpenController(i);
    if (!g_controller)
        fprintf(stderr, "[host] no game controller attached; keyboard only.\n");
    return true;
}

bool Host_WindowActive()
{
    return g_active;
}

void Host_Present(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    if (!g_active)
        return;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        g_frontBuffer = frontBuffer;
        if (width && height && width <= 4096 && height <= 4096)
        {
            g_frameWidth = width;
            g_frameHeight = height;
        }
    }
    g_swapSeq.fetch_add(1, std::memory_order_release);
}

void Host_PresentPixels(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    if (!g_active || !rgba || !width || !height || width > 4096 || height > 4096)
        return;
    const size_t bytes = size_t(width) * height * 4;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_pixelsBack.size() < bytes)
        g_pixelsBack.resize(bytes);
    memcpy(g_pixelsBack.data(), rgba, bytes);
    g_pixelsBack.swap(g_pixelsFront);
    g_pixelsWidth = width;
    g_pixelsHeight = height;
    g_havePixels = true;
}

// ===================================================================================
// The Vulkan swapchain seam (CZ_VK_SWAPCHAIN=1). See window.h for why it exists and
// what it costs.
// ===================================================================================
// These four are the ONLY functions in this file the renderer thread calls, and none of
// them touches the event loop's state. SDL_Vulkan_GetInstanceExtensions and
// SDL_Vulkan_CreateSurface are documented as safe to call from any thread once the
// window exists; SDL_Vulkan_GetDrawableSize reads the window's size, which the loop only
// ever grows through SDL's own resize handling — a stale value here costs one
// swapchain rebuild, which the renderer does on VK_SUBOPTIMAL_KHR anyway.
// The second backend of EmitDebugOverlay. Called from the renderer's pump thread, which
// is why it takes the same mutex the event loop's key handling does — the menu's selection
// index is written there.
//
// It rasterises THE PANEL, not the screen. See window.h for why that is the correctness of
// the whole thing and not an optimisation.
bool Host_DebugOverlayRender(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                             uint32_t& outX, uint32_t& outY, uint32_t& baseW,
                             uint32_t& baseH)
{
    // The overlay's own logical screen. EmitDebugOverlay places the panel against these,
    // and the caller scales the returned rectangle to whatever the window is.
    constexpr int kW = 1280, kH = 720;
    std::lock_guard<std::mutex> lock(g_debugOverlayMutex);
    if (!g_active || (!g_debugOverlayVisible.load(std::memory_order_acquire) &&
                      !Settings_OverlayVisible()))
        return false;

    // The panel's rectangle, computed the same way EmitDebugOverlay does. Taken from its
    // first emitted rect rather than duplicated: the layout is emitted once and this asks
    // it where it put things, so the two cannot drift.
    int px = 0, py = 0, pw = 0, ph = 0;
    bool havePanel = false;
    EmitDebugOverlay(kW, kH, [&](int x, int y, int rw, int rh, uint8_t, uint8_t, uint8_t,
                                 uint8_t) {
        if (!havePanel) { px = x; py = y; pw = rw; ph = rh; havePanel = true; }
    });
    if (!havePanel || pw <= 0 || ph <= 0)
        return false;

    width = uint32_t(pw);
    height = uint32_t(ph);
    outX = uint32_t(px);
    outY = uint32_t(py);
    baseW = uint32_t(kW);
    baseH = uint32_t(kH);
    rgba.assign(size_t(pw) * ph * 4, 0);
    // Every rect is emitted in SCREEN coordinates and written at panel-relative ones.
    // Anything falling outside the panel is clipped away rather than wrapping, which is
    // what a rect drawn at a negative offset would otherwise do.
    EmitDebugOverlay(kW, kH, [&](int x, int y, int rw, int rh,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        const int x0 = std::max(px, x), y0 = std::max(py, y);
        const int x1 = std::min(px + pw, x + rw), y1 = std::min(py + ph, y + rh);
        for (int sy = y0; sy < y1; ++sy)
        {
            uint8_t* row = rgba.data() + (size_t(sy - py) * pw + (x0 - px)) * 4;
            for (int sx = x0; sx < x1; ++sx)
            {
                // THE ALPHA IS CARRIED, not flattened. The caller composites with a copy,
                // so it does the blend itself against a captured background -- and it can
                // only do that if this says which pixels are translucent.
                row[0] = r; row[1] = g; row[2] = b; row[3] = a;
                row += 4;
            }
        }
    });
    return true;
}

bool Host_VulkanSwapchainWanted()
{
    return g_active && g_wantVulkanSwapchain;
}

std::vector<const char*> Host_VulkanInstanceExtensions()
{
    std::vector<const char*> out;
    if (!Host_VulkanSwapchainWanted())
        return out;
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(g_window, &n, nullptr))
    {
        fprintf(stderr, "[host] SDL_Vulkan_GetInstanceExtensions failed: %s\n",
                SDL_GetError());
        return out;
    }
    out.resize(n);
    if (!SDL_Vulkan_GetInstanceExtensions(g_window, &n, out.data()))
    {
        fprintf(stderr, "[host] SDL_Vulkan_GetInstanceExtensions failed: %s\n",
                SDL_GetError());
        out.clear();
    }
    return out;
}

bool Host_VulkanCreateSurface(void* instance, uint64_t* outSurface)
{
    if (!Host_VulkanSwapchainWanted() || !instance || !outSurface)
        return false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(g_window, static_cast<VkInstance>(instance), &surface))
    {
        fprintf(stderr, "[host] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }
    *outSurface = reinterpret_cast<uint64_t>(surface);
    return true;
}

void Host_VulkanDrawableSize(uint32_t* w, uint32_t* h)
{
    if (w) *w = g_drawableW.load(std::memory_order_acquire);
    if (h) *h = g_drawableH.load(std::memory_order_acquire);
}

// See window.h: the desktop size of the display the window is on, for the settings
// panel's resolution clamp. False (and zeros) until the window thread has published
// one, which headless runs never do — the caller treats that as "no clamp".
bool Host_DisplaySize(uint32_t* w, uint32_t* h)
{
    const uint32_t dw = g_displayW.load(std::memory_order_acquire);
    const uint32_t dh = g_displayH.load(std::memory_order_acquire);
    if (w) *w = dw;
    if (h) *h = dh;
    return dw && dh;
}

int Host_DisplayModeList(uint32_t* wh, int maxPairs)
{
    std::lock_guard<std::mutex> lock(g_displayModesMutex);
    int n = 0;
    for (const auto& [w, h] : g_displayModes)
    {
        if (n >= maxPairs)
            break;
        wh[n * 2] = w;
        wh[n * 2 + 1] = h;
        ++n;
    }
    return n;
}

bool Host_PadState(uint32_t userIndex, HostPadState& out)
{
    if (!g_active || userIndex >= 2)
        return false;
    std::lock_guard<std::mutex> lock(g_padMutex);
    out = g_pads[userIndex];
    return true;
}


void Host_DebugMenuSetItems(const std::vector<std::string>& items)
{
    std::lock_guard<std::mutex> lock(g_debugOverlayMutex);
    g_debugOverlayItems = items;
    if (g_debugOverlayItems.empty())
        g_debugOverlaySelection = 0;
    else if (g_debugOverlaySelection >= g_debugOverlayItems.size())
        g_debugOverlaySelection = g_debugOverlayItems.size() - 1;
}

void Host_DebugMenuSetVisible(bool visible)
{
    std::lock_guard<std::mutex> lock(g_debugOverlayMutex);
    g_debugOverlayVisible.store(visible, std::memory_order_release);
}

bool Host_DebugMenuConsumeAction(uint32_t& itemIndex, int32_t& direction)
{
    direction = g_debugOverlayAction.exchange(0, std::memory_order_acq_rel);
    if (!direction)
        return false;
    itemIndex = g_debugOverlayActionIndex.load(std::memory_order_acquire);
    return true;
}

void Host_RequestQuit(const char* why)
{
    if (!g_active)
        return;
    g_quitReason.store(why, std::memory_order_release);
}

void Host_WindowRun()
{
    if (!g_active)
        return;

    uint64_t presented = 0;
    uint64_t framesAtLastTitle = 0;
    auto lastTitle = std::chrono::steady_clock::now();
    const auto loopStart = lastTitle;
    bool sizedToGuest = false;

    for (;;)
    {
        if (const char* why = g_quitReason.load(std::memory_order_acquire))
            Shutdown(why);

        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
                case SDL_QUIT:
                    Shutdown("window closed");
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    OpenController(e.cdevice.which);
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    CloseController(e.cdevice.which);
                    break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        g_keyboardFocus = false;
                    else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                        g_keyboardFocus = true;
                    // Every event that can change the drawable size, not just RESIZED:
                    // SIZE_CHANGED also covers a programmatic resize and a
                    // maximise/restore, and a move between outputs of different scale
                    // changes the drawable without changing the logical size at all.
                    else if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                             e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                             e.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                             e.window.event == SDL_WINDOWEVENT_RESTORED ||
                             e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)
                        PublishDrawableSize();
                    break;
                case SDL_MOUSEMOTION:
                    // Relative deltas only — absolute positions mean nothing to a
                    // stick. Accumulated here, consumed (and zeroed) by the pad
                    // assembly below in this same loop iteration. The native path
                    // (part 92) keeps its own accumulator so neither consumer can
                    // starve the other.
                    g_mouseDX.fetch_add(e.motion.xrel, std::memory_order_relaxed);
                    g_mouseDY.fetch_add(e.motion.yrel, std::memory_order_relaxed);
                    NativeKbm_MouseDelta(e.motion.xrel, e.motion.yrel);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                {
                    // Level state for the native path's BUTTON_1/2/3 sources.
                    const uint32_t mb = SDL_GetMouseState(nullptr, nullptr);
                    uint32_t mask = 0;
                    if (mb & SDL_BUTTON(SDL_BUTTON_LEFT))   mask |= 1;
                    if (mb & SDL_BUTTON(SDL_BUTTON_RIGHT))  mask |= 2;
                    if (mb & SDL_BUTTON(SDL_BUTTON_MIDDLE)) mask |= 4;
                    NativeKbm_MouseButtons(mask);
                    if (e.type == SDL_MOUSEBUTTONDOWN)
                        NativeKbm_NoteDeviceInput(false);
                    break;
                }
                case SDL_MOUSEWHEEL:
                    if (e.wheel.y != 0)
                        NativeKbm_MouseWheel(e.wheel.y);
                    break;
                case SDL_KEYUP:
                    NativeKbmKeyEvent(e.key, false);
                    break;
                case SDL_KEYDOWN:
                    NativeKbmKeyEvent(e.key, true);
                    if (!e.key.repeat)
                    {
                        std::lock_guard<std::mutex> lock(g_debugOverlayMutex);
                        if (g_debugOverlayVisible.load(std::memory_order_acquire) &&
                            !g_debugOverlayItems.empty())
                        {
                            if (e.key.keysym.scancode == SDL_SCANCODE_UP)
                                g_debugOverlaySelection = g_debugOverlaySelection == 0
                                    ? g_debugOverlayItems.size() - 1
                                    : g_debugOverlaySelection - 1;
                            else if (e.key.keysym.scancode == SDL_SCANCODE_DOWN)
                                g_debugOverlaySelection =
                                    (g_debugOverlaySelection + 1) % g_debugOverlayItems.size();
                            else if (e.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                                     e.key.keysym.scancode == SDL_SCANCODE_KP_ENTER ||
                                     e.key.keysym.scancode == SDL_SCANCODE_SPACE)
                            {
                                g_debugOverlayActionIndex.store(
                                    uint32_t(g_debugOverlaySelection),
                                    std::memory_order_release);
                                g_debugOverlayAction.store(1, std::memory_order_release);
                            }
                            else if (e.key.keysym.scancode == SDL_SCANCODE_LEFT ||
                                     e.key.keysym.scancode == SDL_SCANCODE_RIGHT)
                            {
                                g_debugOverlayActionIndex.store(
                                    uint32_t(g_debugOverlaySelection),
                                    std::memory_order_release);
                                g_debugOverlayAction.store(
                                    e.key.keysym.scancode == SDL_SCANCODE_LEFT ? -1 : 2,
                                    std::memory_order_release);
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        // A display-mode change from the PC options screen (part 60). The verb runs on
        // a guest thread; the SDL calls have to happen HERE, on the window thread.
        if (const int pending = Settings_ConsumePendingDisplayMode(); pending >= 0)
            ApplyDisplayModeNow(CzDisplayMode(pending));

        // CZ_WINDOW_RESIZE_AT=SECS:WxH — THE POSITIVE CONTROL FOR THE SWAPCHAIN REBUILD.
        //
        // The rebuild path fires when the window's drawable size changes, and no headless
        // gate can change a window's size, so without this the fix for part 54's blurry
        // picture would ship on the strength of an argument (gotcha 30). This resizes the
        // window once, at a stated moment, so a run can be checked for the renderer's
        // "window drawable is now WxH" line and for the second `[vk] swapchain` line
        // underneath it.
        //
        // It is also the reproduction of the DEFECT: before the fix, this resize produced
        // no new swapchain at all and the compositor upscaled the old one.
        {
            static const char* resizeEnv = getenv("CZ_WINDOW_RESIZE_AT");
            static bool resized = false;
            if (resizeEnv && !resized)
            {
                int secs = 0, rw = 0, rh = 0;
                if (sscanf(resizeEnv, "%d:%dx%d", &secs, &rw, &rh) == 3 && rw > 0 && rh > 0)
                {
                    const auto up = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - loopStart).count();
                    if (up >= secs)
                    {
                        resized = true;
                        fprintf(stderr, "[host] CZ_WINDOW_RESIZE_AT: resizing the window "
                                        "to %dx%d at %llds — the swapchain must follow\n",
                                rw, rh, (long long)up);
                        SDL_SetWindowSize(g_window, rw, rh);
                        PublishDrawableSize();
                    }
                }
                else
                    resized = true;   // malformed: complain once by doing nothing further
            }
        }

        // Mouse capture tracks the setting, the focus and the overlays — captured
        // only while the mouse is actually driving the camera, released the moment
        // a panel wants a visible cursor context or focus leaves. State-change
        // only: SDL_SetRelativeMouseMode is not free.
        {
            const bool wantRel =
                Settings_MouseCam() && g_keyboardFocus &&
                !g_debugOverlayVisible.load(std::memory_order_acquire) &&
                !Settings_OverlayVisible();
            if (wantRel != g_relativeMouse)
            {
                SDL_SetRelativeMouseMode(wantRel ? SDL_TRUE : SDL_FALSE);
                g_relativeMouse = wantRel;
                g_mouseDX.store(0, std::memory_order_relaxed);
                g_mouseDY.store(0, std::memory_order_relaxed);
                fprintf(stderr, "[host] mouse camera %s\n",
                        wantRel ? "CAPTURED (relative mode)" : "released");
            }
        }

        // THE KEYBOARD/MOUSE MERGE INTO PAD 0 (part 91). The keyboard published as
        // pad 2 from the day the fallback was written, and the operator's first real
        // keyboard-only sitting showed what that costs: the title binds the PLAYER
        // to pad 0, so keyboard input half-works at best, and the settings panel's
        // input pump reads pad-0 polls ONLY — a keyboard-only player could not even
        // reach the MOUSE CAMERA row that would have turned their mouse on. Keyboard
        // and mouse now merge into pad 0 alongside the physical controller (buttons
        // OR, triggers/axes by larger magnitude, so either device can drive and
        // neither can pin a stick the other is using); pad 1 reports idle-connected.
        {
            HostPadState merged = ReadController();
            // Device-follow: deliberate pad input (a button, a trigger, or a
            // stick past the reference deadzone — their pad DRIFTS at 18%, so
            // idle must not count) flips the prompt art to the Xbox glyphs.
            if (merged.buttons || merged.leftTrigger > 40 ||
                merged.rightTrigger > 40 ||
                merged.thumbLX > 8000 || merged.thumbLX < -8000 ||
                merged.thumbLY > 8000 || merged.thumbLY < -8000 ||
                merged.thumbRX > 8000 || merged.thumbRX < -8000 ||
                merged.thumbRY > 8000 || merged.thumbRY < -8000)
                NativeKbm_NoteDeviceInput(true);
            const HostPadState kb = ReadKeyboard();
            // A DRIFTING PAD MUST NOT FIGHT THE KEYBOARD (the operator's first
            // keyboard sitting: their idle controller sat at L=(5539,5956) — 18%
            // deflection — so key releases fell back to the drift vector and every
            // press carried it on the other axis). ONLY while the keyboard/mouse is
            // actually contributing, the controller's sub-deadzone axes are zeroed
            // (7849 = the XInput reference deadzone). Pad-only input is untouched
            // byte-for-byte — the no-deadzone principle at KeyAxis still governs
            // the solo-pad path, where the game applies its own.
            const bool kbActive = kb.buttons || kb.leftTrigger || kb.rightTrigger ||
                                  kb.thumbLX || kb.thumbLY || kb.thumbRX || kb.thumbRY;
            if (kbActive)
            {
                auto dz = [](int16_t& v) {
                    if (v > -7849 && v < 7849)
                        v = 0;
                };
                dz(merged.thumbLX);
                dz(merged.thumbLY);
                dz(merged.thumbRX);
                dz(merged.thumbRY);
            }
            merged.buttons |= kb.buttons;
            if (kb.leftTrigger > merged.leftTrigger)
                merged.leftTrigger = kb.leftTrigger;
            if (kb.rightTrigger > merged.rightTrigger)
                merged.rightTrigger = kb.rightTrigger;
            auto biggerAxis = [](int16_t& dst, int16_t src) {
                if ((src < 0 ? -int(src) : int(src)) > (dst < 0 ? -int(dst) : int(dst)))
                    dst = src;
            };
            biggerAxis(merged.thumbLX, kb.thumbLX);
            biggerAxis(merged.thumbLY, kb.thumbLY);
            biggerAxis(merged.thumbRX, kb.thumbRX);
            biggerAxis(merged.thumbRY, kb.thumbRY);
            PublishPad(0, merged);
            PublishPad(1, HostPadState{});
        }

        const uint64_t seq = g_swapSeq.load(std::memory_order_acquire);
        if (seq != presented)
        {
            presented = seq;

            uint32_t frontBuffer, width, height;
            {
                std::lock_guard<std::mutex> lock(g_frameMutex);
                frontBuffer = g_frontBuffer;
                width = g_frameWidth;
                height = g_frameHeight;
            }

            // The guest states its own front-buffer dimensions in every XE_SWAP, so
            // the window is sized from those rather than from our 1280x720 guess —
            // once, and logged, because a silent resize would make a wrong swap
            // decode look like a windowing quirk.
            if (!sizedToGuest)
            {
                sizedToGuest = true;
                fprintf(stderr,
                        "[host] first present: front buffer %08X, guest says %ux%u\n",
                        frontBuffer, width, height);
                if (int(width) != kDefaultWidth || int(height) != kDefaultHeight)
                    SDL_SetWindowSize(g_window, int(width), int(height));
            }

            // In the CZ_VK_SWAPCHAIN arm there is no SDL_Renderer and nothing to blit:
            // the renderer thread has already presented this frame through its own
            // swapchain. The loop still runs — it owns the event pump, the pad and the
            // title bar — it simply has no picture to draw, so the whole blit/overlay/
            // present block below is skipped and the title-bar clock underneath it keeps
            // reporting, which is what says the present seam is running in this arm too.
            if (g_renderer)
            {
            // Blit the rendered frame if there is one. With CZ_VKDRAW off there
            // never is, and the flat clear below is the honest present: the guest's
            // front buffer holds whatever its allocator left there, and showing it
            // would be noise presented as a frame.
            bool blitted = false;
            {
                std::lock_guard<std::mutex> lock(g_frameMutex);
                if (g_havePixels && g_pixelsWidth && g_pixelsHeight)
                {
                    // Recreate the texture when the frame's size changes, which it
                    // does exactly once (the guest states its own dimensions at the
                    // first swap) unless the title switches resolution.
                    if (!g_frameTexture || g_frameTextureW != int(g_pixelsWidth) ||
                        g_frameTextureH != int(g_pixelsHeight))
                    {
                        if (g_frameTexture)
                            SDL_DestroyTexture(g_frameTexture);
                        g_frameTexture = SDL_CreateTexture(
                            g_renderer, SDL_PIXELFORMAT_ABGR8888,
                            SDL_TEXTUREACCESS_STREAMING, int(g_pixelsWidth),
                            int(g_pixelsHeight));
                        g_frameTextureW = int(g_pixelsWidth);
                        g_frameTextureH = int(g_pixelsHeight);
                        fprintf(stderr, "[host] present texture %dx%d\n",
                                g_frameTextureW, g_frameTextureH);
                    }
                    if (g_frameTexture)
                    {
                        SDL_UpdateTexture(g_frameTexture, nullptr, g_pixelsFront.data(),
                                          int(g_pixelsWidth) * 4);
                        // ASPECT-FIT AS OF PART 60 (same rule and same arm as the
                        // swapchain path — Host_AspectFitRect in window.h is the one
                        // shared computation): the frame keeps its shape inside the
                        // window and the bars are black. `CZ_VK_STRETCH=1` restores
                        // the old full-window stretch. The clear runs only when bars
                        // exist, because SDL keeps the previous frame's pixels in the
                        // bar regions otherwise.
                        static const bool stretchArm =
                            getenv("CZ_VK_STRETCH") != nullptr;
                        SDL_Rect dst{ 0, 0, 0, 0 };
                        SDL_Rect* dstPtr = nullptr;
                        if (!stretchArm)
                        {
                            int ow = 0, oh = 0;
                            SDL_GetRendererOutputSize(g_renderer, &ow, &oh);
                            int32_t fx = 0, fy = 0;
                            uint32_t fw = 0, fh = 0;
                            Host_AspectFitRect(g_pixelsWidth, g_pixelsHeight,
                                               uint32_t(ow > 0 ? ow : 0),
                                               uint32_t(oh > 0 ? oh : 0), fx, fy, fw,
                                               fh);
                            if (fx || fy || int(fw) != ow || int(fh) != oh)
                            {
                                SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0xFF);
                                SDL_RenderClear(g_renderer);
                            }
                            dst = { fx, fy, int(fw), int(fh) };
                            dstPtr = &dst;
                        }
                        SDL_RenderCopy(g_renderer, g_frameTexture, nullptr, dstPtr);
                        blitted = true;
                    }
                }
            }
            if (!blitted)
            {
                SDL_SetRenderDrawColor(g_renderer, 0x14, 0x16, 0x1A, 0xFF);
                SDL_RenderClear(g_renderer);
            }
            DrawDebugOverlay();
            SDL_RenderPresent(g_renderer);
            }
        }
        else
        {
            // Nothing swapped. 1 ms keeps the window responsive at a cost that does
            // not show up against the guest's own load (finding 41's lesson: a poll
            // loop that never sleeps is a busy loop wearing a polite name).
            SDL_Delay(1);
        }

        const auto now = std::chrono::steady_clock::now();
        const auto sinceTitle =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTitle).count();
        if (sinceTitle >= 1000)
        {
            // Cheap and covers monitor drags: the display the window sits on is
            // re-queried at the title-bar cadence rather than via display events.
            PublishDisplaySize();
            const double fps = double(presented - framesAtLastTitle) * 1000.0 / double(sinceTitle);
            framesAtLastTitle = presented;
            lastTitle = now;

            bool rendering;
            {
                std::lock_guard<std::mutex> lock(g_frameMutex);
                rendering = g_havePixels;
            }
            char title[192];
            snprintf(title, sizeof(title),
                     "Dead Rising 2: Case Zero — %s — %llu frames, %.1f fps",
                     rendering ? "rendering" : "no renderer (CZ_VKDRAW=1 to enable)",
                     (unsigned long long)presented, fps);
            SDL_SetWindowTitle(g_window, title);
        }
    }
}

#endif // CZ_HAVE_SDL
