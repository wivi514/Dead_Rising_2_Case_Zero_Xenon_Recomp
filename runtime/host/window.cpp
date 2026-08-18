#include "window.h"

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

void Host_RequestDebugJump() { g_debugJumpPressed.store(true, std::memory_order_release); }
void Host_RequestDebugEnter() { g_debugEnterPressed.store(true, std::memory_order_release); }
void Host_RequestDebugMenu() { g_debugMenuPressed.store(true, std::memory_order_release); }
void Host_RequestSnapDump() { g_snapDumpPressed.store(true, std::memory_order_release); }

bool Host_ConsumeSnapDumpPressed()
{
    return g_snapDumpPressed.exchange(false, std::memory_order_acq_rel);
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
void Host_Present(uint32_t, uint32_t, uint32_t) {}
void Host_PresentPixels(const uint8_t*, uint32_t, uint32_t) {}
void Host_WindowRun() {}
void Host_RequestQuit(const char*) {}
bool Host_PadState(uint32_t, HostPadState&) { return false; }
void Host_DebugMenuSetItems(const std::vector<std::string>&) {}
void Host_DebugMenuSetVisible(bool) {}
bool Host_DebugMenuConsumeAction(uint32_t&, int32_t&) { return false; }
bool Host_VulkanSwapchainWanted() { return false; }
std::vector<const char*> Host_VulkanInstanceExtensions() { return {}; }
bool Host_VulkanCreateSurface(void*, uint64_t*) { return false; }
void Host_VulkanDrawableSize(uint32_t* w, uint32_t* h) { if (w) *w = 0; if (h) *h = 0; }

#else

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL.h>
// vulkan.h before SDL_vulkan.h so the latter uses the real handle types rather than
// its own forward declarations — the surface below is a VkSurfaceKHR either way, but
// VK_NULL_HANDLE only exists with the real header.
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>

#include "../gpu/vk_renderer.h"

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
    if (nw != g_drawableW.exchange(nw, std::memory_order_acq_rel) ||
        nh != g_drawableH.exchange(nh, std::memory_order_acq_rel))
        fprintf(stderr, "[host] window drawable %ux%u (%s present) — quote this with any "
                        "frame time from this run\n",
                nw, nh, g_renderer ? "readback" : "swapchain");
}
SDL_GameController* g_controller = nullptr;
SDL_JoystickID      g_controllerId = -1;
bool g_inputTrace = false;

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
        default:  return nullptr;
    }
}

void DrawText(int x, int y, const std::string& text, int scale,
              uint8_t r, uint8_t g, uint8_t b)
{
    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    for (char c : text)
    {
        if (const char* bits = Glyph(c))
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (bits[row * 5 + col] == '1')
                    {
                        SDL_Rect p{x + col * scale, y + row * scale, scale, scale};
                        SDL_RenderFillRect(g_renderer, &p);
                    }
        x += 6 * scale;
    }
}

void DrawDebugOverlay()
{
    std::lock_guard<std::mutex> lock(g_debugOverlayMutex);
    if (!g_debugOverlayVisible.load(std::memory_order_acquire))
        return;

    int w = 0, h = 0;
    SDL_GetRendererOutputSize(g_renderer, &w, &h);
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, 8, 26, 96, 225);
    SDL_Rect panel{24, 24, w > 760 ? 720 : w - 48, h - 48};
    SDL_RenderFillRect(g_renderer, &panel);
    SDL_SetRenderDrawColor(g_renderer, 70, 150, 255, 255);
    SDL_RenderDrawRect(g_renderer, &panel);
    DrawText(44, 42, "CASE ZERO DEBUG MENU", 3, 255, 255, 255);
    DrawText(44, 70, "UP/DOWN SELECT  ENTER USE  LEFT/RIGHT EDIT  F4 CLOSE",
             2, 145, 205, 255);

    const size_t rows = panel.h > 120 ? size_t((panel.h - 110) / 18) : 0;
    const size_t start = g_debugOverlaySelection >= rows
        ? g_debugOverlaySelection - rows + 1 : 0;
    for (size_t line = 0; line < rows && start + line < g_debugOverlayItems.size(); ++line)
    {
        const size_t index = start + line;
        const bool selected = index == g_debugOverlaySelection;
        if (selected)
        {
            SDL_SetRenderDrawColor(g_renderer, 35, 105, 205, 255);
            SDL_Rect hi{38, 98 + int(line) * 18, panel.w - 28, 17};
            SDL_RenderFillRect(g_renderer, &hi);
        }
        std::string label = (selected ? "> " : "  ") + g_debugOverlayItems[index];
        if (label.size() > 54) label.resize(54);
        DrawText(44, 101 + int(line) * 18, label, 2,
                 selected ? 255 : 205, selected ? 255 : 225, 255);
    }
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
    fprintf(stderr, "[host] keyboard -> pad 2:");
    for (const auto& k : kKeyMap)
        fprintf(stderr, "  %s=%s", k.keyName, k.padName);
    fprintf(stderr, "\n[host] keyboard -> sticks:  WASD=left stick  IJKL=right stick  "
                    "1/3=LT/RT\n");
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

HostPadState ReadKeyboard()
{
    HostPadState s{};
    static bool f2WasDown = false;
    static bool f3WasDown = false;
    static bool f4WasDown = false;
    static bool f9WasDown = false;
    if (g_keyboardFocus)
    {
        const uint8_t* keys = SDL_GetKeyboardState(nullptr);
        const bool f2Down = keys[SDL_SCANCODE_F2] != 0;
        const bool f3Down = keys[SDL_SCANCODE_F3] != 0;
        const bool f4Down = keys[SDL_SCANCODE_F4] != 0;
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
        fprintf(stderr,
                "[host] pad %u packet %u: buttons=%04X triggers=%u/%u L=(%d,%d) R=(%d,%d)\n",
                userIndex, packet, fresh.buttons, fresh.leftTrigger, fresh.rightTrigger,
                fresh.thumbLX, fresh.thumbLY, fresh.thumbRX, fresh.thumbRY);
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
    fflush(nullptr);
    // _Exit, not exit: guest threads are still running recompiled code against guest
    // memory, and running static destructors underneath them would turn an ordinary
    // quit into a crash report about a subsystem that was working.
    std::_Exit(0);
}

} // namespace

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
    g_wantVulkanSwapchain = getenv("CZ_VK_SWAPCHAIN") != nullptr;

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
    const Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
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
                        "CZ_VK_SWAPCHAIN is NOT in force; falling back to the readback "
                        "present path.\n", SDL_GetError());
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
                "[host] CZ_VK_SWAPCHAIN: this window carries SDL_WINDOW_VULKAN and has "
                "NO SDL_Renderer. The renderer presents its own image; the present "
                "readback and its two copies do not run, and the present MODE is chosen "
                "by the renderer (see the [vk] swapchain line), not by SDL.%s\n"
                "[host] WHAT THIS ARM COSTS, said out loud: the host-rendered F4 debug "
                "overlay is drawn by SDL's renderer and is therefore NOT DRAWN in this "
                "arm. The title's own F2 DebugJump screen is drawn by the GAME and is "
                "unaffected, and so is every other instrument.\n",
                wantVsync ? " CZ_HOST_VSYNC=1 is IGNORED here." : "");
    }
    PublishDrawableSize();

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
                case SDL_KEYDOWN:
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

        PublishPad(0, ReadController());
        PublishPad(1, ReadKeyboard());

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
                        SDL_RenderCopy(g_renderer, g_frameTexture, nullptr, nullptr);
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
