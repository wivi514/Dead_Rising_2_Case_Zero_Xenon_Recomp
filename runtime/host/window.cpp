#include "window.h"

#include <cstdio>

// CZ_HAVE_SDL is set by CMake when the window is built (the default). The headless
// build is not a fallback the runtime can fall INTO — it is a configure-time choice
// (-DCZ_WINDOW=OFF), and it says so on every startup, because "no window" and
// "window whose input is broken" are indistinguishable from a log otherwise.
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
bool Host_PadState(HostPadState&) { return false; }

#else

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL.h>

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
SDL_Renderer* g_renderer = nullptr;
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
HostPadState g_pad{ 1, 0, 0, 0, 0, 0, 0, 0 };

// Keyboard input is gated on window focus. SDL does reset its keyboard state when a
// window loses focus, so this is belt and braces — but the failure it prevents is a
// key held at the moment focus is lost staying "down" in the guest forever, and that
// one presents as the title behaving as though a button is stuck. A GAME CONTROLLER
// is deliberately NOT gated: a pad works whatever window is focused, which is what
// every other application on the machine does.
bool g_keyboardFocus = true;

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
    fprintf(stderr, "[host] keyboard -> pad:");
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

HostPadState ReadDevices()
{
    HostPadState s{};
    if (g_keyboardFocus)
    {
        const uint8_t* keys = SDL_GetKeyboardState(nullptr);

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

    // The controller is OR'd on top of the keyboard rather than replacing it: a pad
    // being plugged in should not silently disable the keys, and a title that is
    // being driven by one of them is not confused by the other reading neutral.
    if (g_controller)
    {
        for (const auto& p : kPadMap)
            if (SDL_GameControllerGetButton(g_controller, p.sdl))
                s.buttons |= p.button;

        const int lx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        const int rx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        if (s.thumbLX == 0) s.thumbLX = int16_t(lx);
        if (s.thumbRX == 0) s.thumbRX = int16_t(rx);
        if (s.thumbLY == 0) s.thumbLY = PadAxisY(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (s.thumbRY == 0) s.thumbRY = PadAxisY(g_controller, SDL_CONTROLLER_AXIS_RIGHTY);

        // SDL reports triggers as 0..32767; XInput's are 0..255.
        const int lt =
            SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7;
        const int rt =
            SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
        if (s.leftTrigger == 0) s.leftTrigger = uint8_t(lt < 0 ? 0 : lt > 255 ? 255 : lt);
        if (s.rightTrigger == 0) s.rightTrigger = uint8_t(rt < 0 ? 0 : rt > 255 ? 255 : rt);
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
void PublishPad(const HostPadState& fresh)
{
    std::lock_guard<std::mutex> lock(g_padMutex);
    if (SameState(fresh, g_pad))
        return;
    const uint32_t packet = g_pad.packet + 1;
    g_pad = fresh;
    g_pad.packet = packet;
    if (g_inputTrace)
        fprintf(stderr,
                "[host] pad packet %u: buttons=%04X triggers=%u/%u L=(%d,%d) R=(%d,%d)\n",
                packet, fresh.buttons, fresh.leftTrigger, fresh.rightTrigger,
                fresh.thumbLX, fresh.thumbLY, fresh.thumbRX, fresh.thumbRY);
}

void Shutdown(const char* why)
{
    fprintf(stderr, "[host] %s — closing the window and exiting.\n", why);
    fflush(nullptr);
    // _Exit, not exit: guest threads are still running recompiled code against guest
    // memory, and running static destructors underneath them would turn an ordinary
    // quit into a crash report about a subsystem that was working.
    std::_Exit(0);
}

} // namespace

bool Host_WindowInit()
{
    if (getenv("CZ_NO_WINDOW"))
    {
        fprintf(stderr, "[host] CZ_NO_WINDOW=1 — headless: no window, no present, and "
                        "XamInputGetState answers with its neutral pad. This is the "
                        "control arm for every claim about phase 3.\n");
        return false;
    }

    // Leave SIGINT/SIGTERM alone. SDL would otherwise install handlers that turn them
    // into an SDL_QUIT event, which sounds like an improvement and is not: every gate
    // run in this project is `timeout N ./cz_runtime`, and routing SIGTERM through our
    // event loop makes process termination depend on that loop still being alive. The
    // whole point of a gate is that it terminates the same way whatever the runtime is
    // doing.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr,
                "[host] SDL_Init failed: %s\n"
                "[host] RUNNING HEADLESS. The boot will reach the title screen and "
                "stop there, because nothing can press a button (finding 37).\n",
                SDL_GetError());
        return false;
    }

    g_window = SDL_CreateWindow("Dead Rising 2: Case Zero", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, kDefaultWidth, kDefaultHeight,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
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
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer)
    {
        fprintf(stderr, "[host] accelerated renderer unavailable (%s) — falling back "
                        "to software.\n",
                SDL_GetError());
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer)
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

    fprintf(stderr, "[host] window %dx%d up on SDL video driver '%s'.\n", kDefaultWidth,
            kDefaultHeight, SDL_GetCurrentVideoDriver());
    fprintf(stderr, "[host] THE WINDOW IS EXPECTED TO BE BLANK: there is no renderer "
                    "until phase 5. The title bar carries the live frame count, which "
                    "is what says the present seam is running.\n");
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

bool Host_PadState(HostPadState& out)
{
    if (!g_active)
        return false;
    std::lock_guard<std::mutex> lock(g_padMutex);
    out = g_pad;
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
                    break;
                default:
                    break;
            }
        }

        PublishPad(ReadDevices());

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
            SDL_RenderPresent(g_renderer);
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
