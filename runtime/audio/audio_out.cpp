#include "audio_out.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef CZ_HAVE_SDL
#include <SDL.h>
#endif

// The XAudio render-driver frame format, from the title's own mixing loop
// (sub_828867E8, quoted in kernel/audio.cpp): 48 kHz, 6 channels, 256 samples per
// frame, PLANAR big-endian float32 with a 1024-byte channel stride.
namespace {

constexpr int kHz = 48000;
constexpr int kChannels = 6;
constexpr int kSamples = 256;

#ifdef CZ_HAVE_SDL
SDL_AudioDeviceID g_dev = 0;
#endif
bool g_inited = false;
bool g_disabled = false;
std::once_flag g_initOnce;

inline float SwapF(uint32_t beBits)
{
    const uint32_t le = __builtin_bswap32(beBits);
    float f;
    memcpy(&f, &le, sizeof f);
    return f;
}

// CZ_NO_AUDIO_OUT=1 keeps the whole pipeline — pump, decode, mix — and opens no
// device. It is the same-binary control arm for every claim of the form "you can
// now hear X", and it is also what a headless gate run wants: a CI machine with no
// sound card must not be a different code path from a desktop with one.
void EnsureDevice()
{
    std::call_once(g_initOnce, [] {
        g_inited = true;
        if (getenv("CZ_NO_AUDIO_OUT"))
        {
            g_disabled = true;
            fprintf(stderr, "[audio] output disabled (CZ_NO_AUDIO_OUT) — frames are "
                            "mixed and dropped\n");
            return;
        }
#ifndef CZ_HAVE_SDL
        g_disabled = true;
        fprintf(stderr, "[audio] no SDL in this build — no output device\n");
#else
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        {
            g_disabled = true;
            fprintf(stderr, "[audio] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            return;
        }
        SDL_AudioSpec want{}, have{};
        want.freq = kHz;
        want.format = AUDIO_F32SYS;
        want.channels = 2;   // downmix 5.1 -> stereo: the widest host compatibility
        want.samples = 2048; // ~43 ms device buffer, to absorb scheduling jitter
        want.callback = nullptr; // queue-based, so the guest's pump stays in charge
        g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!g_dev)
        {
            g_disabled = true;
            fprintf(stderr, "[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(g_dev, 0);
        fprintf(stderr,
                "[audio] output open: %d Hz stereo (device freq=%d ch=%d samples=%d) "
                "driver='%s'\n",
                kHz, have.freq, have.channels, have.samples,
                SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(null)");
#endif
    });
}

} // namespace

bool Audio_Out_Available()
{
    EnsureDevice();
#ifdef CZ_HAVE_SDL
    return !g_disabled && g_dev != 0;
#else
    return false;
#endif
}

int Audio_Out_QueuedFrames()
{
#ifdef CZ_HAVE_SDL
    EnsureDevice();
    if (g_disabled || !g_dev)
        return -1;
    return int(SDL_GetQueuedAudioSize(g_dev) / (2 * sizeof(float)));
#else
    return -1;
#endif
}

void Audio_Out_SubmitFrame(const void* hostSamples)
{
#ifdef CZ_HAVE_SDL
    if (!hostSamples)
        return;
    EnsureDevice();
    if (g_disabled || !g_dev)
        return;

    // If the queue backs up — the host is slower than 48 kHz, or the device is
    // paused — drop this frame rather than build unbounded latency. ~200 ms cap.
    if (SDL_GetQueuedAudioSize(g_dev) > kHz * 2 * sizeof(float) / 5)
        return;

    const auto* planar = reinterpret_cast<const uint32_t*>(hostSamples);
    float stereo[kSamples * 2];
    // XAudio 5.1 channel order: 0=FL 1=FR 2=FC 3=LFE 4=BL 5=BR.
    for (int i = 0; i < kSamples; i++)
    {
        const float fl = SwapF(planar[0 * kSamples + i]);
        const float fr = SwapF(planar[1 * kSamples + i]);
        const float fc = SwapF(planar[2 * kSamples + i]);
        const float lfe = SwapF(planar[3 * kSamples + i]);
        const float bl = SwapF(planar[4 * kSamples + i]);
        const float br = SwapF(planar[5 * kSamples + i]);
        float l = fl + 0.707f * fc + 0.707f * bl + 0.5f * lfe;
        float r = fr + 0.707f * fc + 0.707f * br + 0.5f * lfe;
        stereo[i * 2 + 0] = l < -1.f ? -1.f : (l > 1.f ? 1.f : l);
        stereo[i * 2 + 1] = r < -1.f ? -1.f : (r > 1.f ? 1.f : r);
    }
    SDL_QueueAudio(g_dev, stereo, sizeof stereo);
#else
    (void)hostSamples;
#endif
}
