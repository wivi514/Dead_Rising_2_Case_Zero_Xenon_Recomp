// The audio kernel surface: XAudio's render-driver client and the XMA decoder's
// context array.
//
// WHY THIS MODULE EXISTS
// ----------------------
// Phase 1 left all seven audio imports as generated honest-failure stubs, and that
// left one real divergence in the A5 gate: hardware calls
// XAudioSubmitRenderDriverFrame and we never did. It is not a call the title makes
// directly — it is made from a callback the AUDIO DRIVER invokes, so with no driver
// there is nothing to invoke it and the whole subsystem is silently absent rather
// than visibly broken. Right behind it in the gate sit XMACreateContext (position
// 118) and MmMapIoSpace (119).
//
// Every structural claim below is read out of the title's own code, because the
// capture cannot supply it: Xenia's log prints an import's pointer arguments as
// they were BEFORE the call, so it never shows what an audio import wrote (gotcha
// 60). The relevant guest functions are quoted at each implementation.
//
// GOTCHA 5 IS THE GOVERNING RULE HERE. Fable 2 lost weeks to an XMA context call
// that faked success. Note the shape of the trap in this title, because it is the
// opposite of the usual one: sub_8285EE58 tests XMACreateContext's result with
//
//     mr. r23,r3
//     blt 0x8285ef5c        ; SIGNED compare
//
// so a POSITIVE return reads as success and 0xC0000002 (negative) correctly reads
// as failure. The generated stub was therefore already honest — which is why
// nothing downstream of it ever ran, and why the fix here is to make the call
// genuinely succeed rather than to stop it lying.
//
// WHAT THIS IS NOT
// ----------------
// There is no audio OUTPUT here and no XMA decoding. Submitted frames are counted
// and dropped. That is a real null sink, not a fake success: the contract of
// XAudioSubmitRenderDriverFrame is "the driver has taken this buffer", and a
// driver that takes a buffer and discards it is a thing that exists. Decoding XMA
// (the contexts allocated below describe compressed streams the hardware decoder
// would consume) and mixing to a host device are phase 5.
#include "audio.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <xbox.h>

#include "../cpu/guest_thread.h"
#include "guestcall.h"
#include "heap.h"
#include "klog.h"
#include "memory.h"

namespace {

bool AudioTrace()
{
    static const bool on = getenv("CZ_AUDIO_TRACE") != nullptr;
    return on;
}

// ---------------------------------------------------------------------------
// The XMA decoder's register aperture
// ---------------------------------------------------------------------------
//
// The 360's XMA hardware has a memory-mapped register file, and this title reaches
// into it directly rather than going through the kernel. Two registers, both
// recovered from the image:
//
// sub_8285EDF8, in full, is the whole of the title's XMA hardware discovery:
//
//     lis   r11,32746        ; r11 = 0x7FEA0000
//     lis   r10,-32082       ; r10 = 0x82AE0000
//     ori   r11,r11,6144     ; r11 = 0x7FEA1800
//     lwbrx r11,0,r11        ; byte-REVERSED load: the register file is little-endian
//     stw   r11,-28072(r10)  ; cache it at 0x82AD9258
//     blr
//
// so [0x7FEA1800] holds the PHYSICAL address of the context array, and the title
// caches it. It then turns a context pointer into a context index by
// (MmGetPhysicalAddress(ctx) - cachedBase) >> 6 (sub_8285EE58), which is the
// constraint that matters to us: contexts must be 64 bytes apart, in one array,
// starting at the physical address we publish here. Hand out contexts from
// anywhere else and the index is garbage.
//
// With the index in hand the title sets an enable bit:
//
//     rlwinm r9,r11,27,21,31    ; index >> 5
//     addis  r9,r9,8187         ; + 0x1FFB0000
//     addi   r9,r9,-31072       ; - 0x7960     -> 0x1FFA86A0 + (index >> 5)
//     rlwinm r10,r9,2,0,29      ; << 2         -> 0x7FEA1A80 + (index >> 5) * 4
//     stwbrx r11,0,r10          ; little-endian again
//     eieio
//
// i.e. a 1-bit-per-context "kick" bitmap at 0x7FEA1A80. We do not implement it:
// our address space is one flat mapping, so those stores land in ordinary memory
// and are inert. That is the right non-answer for now (nothing decodes yet) but it
// is a real gap — when a decoder arrives, this aperture has to become trapped MMIO
// rather than RAM, or the kick will be written and never noticed.
//
// Note both accesses are byte-reversed (lwbrx/stwbrx). This register file is
// little-endian on a big-endian machine, which is exactly why publishing the base
// as a plain be<uint32_t> would hand the guest a byte-swapped address.
constexpr uint32_t kXmaContextArrayRegister = 0x7FEA1800;

// The hardware's context descriptors are 64 bytes and there are 320 of them.
//
// The 64 is not a guess — it is forced by the title's own `>> 6` above, and A1's
// `MmMapIoSpace(2, phys, 64, 0x404)` says it a second time. The 320 is the 360's
// context count, which is the one number here NOT derived from this image; it is
// bounded generously rather than exactly, and the capture says that is fine —
// docs/xenia-capture-analysis.md §11 measures Case Zero using 0-17+ contexts in
// GAMEPLAY (A2), an order of magnitude below this. If it is ever wrong the failure
// is loud rather than silent: exhaustion returns a real error and logs
// (XMACreateContext_x).
constexpr uint32_t kXmaContextSize = 64;
constexpr uint32_t kXmaContextCount = 320;

std::mutex g_xmaMutex;
uint32_t g_xmaContextArray = 0;             // guest address of the array
bool g_xmaContextUsed[kXmaContextCount] = {};

// ---------------------------------------------------------------------------
// The render-driver client
// ---------------------------------------------------------------------------
//
// XAudioRegisterRenderDriverClient does not take a callback — it takes a POINTER to
// a {function, context} pair. sub_82886B70 builds it on its own stack:
//
//     lis  r11,-32120 / addi r11,r11,27056   ; r11 = 0x828869B0   (the callback)
//     addi r9,r31,-4                         ; r9  = its context argument
//     stw  r11,88(r1) / stw r9,92(r1)        ; the pair
//     addi r3,r1,88                          ; arg1 = &pair
//     addi r4,r31,28                         ; arg2 = &driverHandle  (OUT)
//     bl   XAudioRegisterRenderDriverClient
//
// and A5 confirms it from the other side, printing the first dword of arg1:
//
//     XAudioRegisterRenderDriverClient(7018EEC8(828869B0), E5688B80(00000000))
//
// The driver then calls that callback whenever it wants a frame. Nothing in the
// title ever calls it — which is why a stubbed registration makes the entire audio
// path vanish without a single error.
//
// THE CALLBACK DOES NOT RECEIVE THE CONTEXT. IT RECEIVES A POINTER TO IT.
// -----------------------------------------------------------------------
// sub_828869B0, the registered callback, is a two-instruction thunk:
//
//     lwz r3,0(r3)
//     b   0x828867E8
//
// so it dereferences its argument before doing anything. Passing the context
// straight through in r3 — the obvious reading, and what a naive driver would do —
// hands the real body a pointer one indirection too shallow, and it then reads its
// wait objects, its buffer and its driver handle out of whatever happens to sit
// there. Here that was a table of audio constants in .rdata, so every wait object
// came back NULL and (before kernel/imports.cpp learned to check) took the host
// down with a null dereference inside our own kernel.
//
// The object layout, from its constructor sub_828869B8, is what proves the
// indirection is deliberate rather than a quirk:
//
//     stw r9,0(r3)     ; primary vptr   (0x820D241C)
//     stw r8,4(r3)     ; secondary vptr (0x820D23F0) -- +12 of it is sub_82886B70
//     stw r11,32(r3) / 40 / 44 / 48 / std r11,56(r3)  ; all zeroed
//
// sub_82886B70 is slot 3 of the SECOND vtable, so its `this` is obj+4 and the
// `addi r9,r31,-4` that builds the context is recovering obj+0. Meanwhile the
// callback body sub_828867E8 reads its driver handle from +32, its wait objects
// from +44/+48 and its frame counter from +56 — measured from obj+0, matching the
// constructor field for field. So the body wants obj, the registration supplies
// obj, and the thunk in between performs one load: the driver must pass a POINTER
// to the registered context, not the context.
//
// That also explains why it cannot be the caller's {callback, context} pair
// itself: sub_82886B70 builds that on its own stack (`addi r3,r1,88`) and returns
// immediately, so the driver has to have copied it. We keep our own cell for the
// same reason.
std::atomic<uint32_t> g_clientCallback{ 0 };
// Guest-side cell whose single dword holds the context; its address is what the
// callback is actually invoked with.
uint32_t g_clientContextCell = 0;
std::atomic<bool> g_pumpRunning{ false };
std::atomic<uint64_t> g_framesSubmitted{ 0 };

// The handle handed back through arg2. The title treats it as opaque — it stores it
// and passes it straight back to Submit/Unregister — so any value would do; this is
// the one Xenia hands out (A5: `XAudioSubmitRenderDriverFrame(41550000, ...)`), and
// matching it costs nothing and makes our trace lines diff against the capture's
// verbatim. Non-zero matters, though: sub_828868D8 skips the unregister entirely on
// a zero handle.
constexpr uint32_t kDriverHandle = 0x41550000;

// One driver frame is 256 samples x 6 channels of 32-bit float, planar. Derived
// from the mixing loop in sub_828867E8, which de-interleaves into its own stack:
//
//     li     r8,256              ; 256 samples
//     li     r9,6                ; 6 channels
//     lfs    f0,0(r10) / addi r10,r10,4
//     stfsu  f0,1024(r7)         ; channel stride 1024 bytes = 256 floats
//
// 5.333 ms of audio at 48 kHz, which is the rate the pump below has to hold: with
// no host audio device to pace us, the callback's cadence IS the game's audio
// clock. A5 bears the number out — 19,685 frames over its boot.
constexpr uint32_t kFrameSamples = 256;
constexpr uint32_t kFrameChannels = 6;
constexpr uint32_t kFrameBytes = kFrameSamples * kFrameChannels * 4;
constexpr int kDefaultFrameUs = 5333;

// ---------------------------------------------------------------------------
// CZ_XMA_NULL_DECODER=1 — AN ARM, NOT A FEATURE.
// ---------------------------------------------------------------------------
//
// There is no XMA decoder in this runtime, and the consequence is not only that the
// game is silent. The decoder is also the only thing on the console that ever CLEARS
// an XMA context's input-buffer-valid bits: the guest sets them when it hands over
// packets, hardware clears them as it consumes them, and the title's own
// sub_8285EFE0 reads exactly those two bits to answer "has this voice run dry".
// With nothing clearing them, every voice this title has ever started is still
// playing, for the life of the process.
//
// This arm makes the decoder consume its input and produce nothing — the minimum a
// decoder does that is observable from guest code. It fabricates progress the real
// hardware would only make after actually decoding the audio, so it is held to the
// same standard as CZ_FAKE_START_MS (gotcha 78): it announces itself once, loudly,
// and it must never be on for a gate run. Its whole purpose is to answer one
// question — does anything downstream of "this voice finished" move? — which no
// passive instrument can, because "waiting for a voice" and "idle" look identical
// from outside (gotcha 77).
//
// CZ_XMA_NULL_DECODER_MS_PER_PKT=N sets the RATE, in milliseconds of audio per
// 2048-byte packet (default 40). The rate is load-bearing rather than cosmetic: at
// the instant setting (0) a voice is dry before anything can poll it, so IsPlaying
// reads FALSE from the moment it starts and the arm tests "nothing ever plays"
// rather than "everything plays and then finishes" — the opposite end of the same
// axis from the stock runtime, which answers TRUE forever. Only a realistic rate
// produces the third configuration, a voice that is observably playing and then
// observably done, and that is the one a completion-gated cinematic needs.
//
// 40 ms is the natural full packet at this title's own declared format: the context
// words say 48 kHz with `subframe_decode_count = 4`, i.e. 4 x 128 = 512 samples per
// decode step, and a 2048-byte XMA packet carrying four 512-sample frames is
// 2048 samples = 42.7 ms. It is an estimate and is labelled as one — the arm exists
// to move a state machine, not to reproduce a bitrate.
bool NullDecoderEnabled()
{
    static const bool on = getenv("CZ_XMA_NULL_DECODER") != nullptr;
    return on;
}

// Called once per driver frame; `frameUs` is that frame's period, so the rate holds
// even when CZ_AUDIO_FRAME_US changes it.
void NullDecoderConsume(int frameUs)
{
    static const int msPerPacket = []
    {
        const char* env = getenv("CZ_XMA_NULL_DECODER_MS_PER_PKT");
        return env ? std::max(0, atoi(env)) : 40;
    }();

    // Fractional packets per frame, accumulated so a rate slower than one packet per
    // frame is expressible at all. At 40 ms/packet and a 5.333 ms frame this retires
    // one packet every 7.5 frames.
    static double credit = 0.0;
    uint32_t retire = 0;
    if (msPerPacket > 0)
    {
        credit += (frameUs / 1000.0) / msPerPacket;
        retire = uint32_t(credit);
        credit -= retire;
        if (!retire)
            return;
    }

    std::lock_guard<std::mutex> lock(g_xmaMutex);
    if (!g_xmaContextArray)
        return;
    uint8_t* base = g_memory.base;
    for (uint32_t i = 0; i < kXmaContextCount; i++)
    {
        if (!g_xmaContextUsed[i])
            continue;
        const uint32_t va = g_xmaContextArray + i * kXmaContextSize;
        uint32_t d0 = PPC_LOAD_U32(va);
        if (!((d0 >> 20) & 3))
            continue;                       // already dry — nothing to consume

        const uint32_t packets = d0 & 0xFFF;   // input buffer 0's packet count
        if (retire && packets > retire)
        {
            // Retire part of the buffer. The count is the field the guest wrote, so
            // decrementing it is the same statement the hardware makes as it walks
            // the packets, one step short of clearing the valid bit.
            d0 = (d0 & ~0xFFFu) | (packets - retire);
        }
        else
        {
            // The buffer is spent: clear both valid bits, which is the transition
            // sub_8285EFE0 is looking for.
            d0 &= ~(3u << 20);
        }
        PPC_STORE_U32(va, d0);
    }
}

// The client callback, on its own guest thread.
//
// It needs a GuestThreadContext for the same reason the graphics interrupt pump
// does: it runs recompiled guest code, so it needs its own PCR/TLS/TEB and stack.
// And it needs its own THREAD because sub_828867E8 blocks:
//
//     bl KeWaitForMultipleObjects(2, {ready, buffer}, WaitAny, 3, 1, 0, NULL, ...)
//     cmplwi cr6,r3,1
//     bne    cr6,<skip>          ; only object[1] means "a buffer is ready"
//
// with a NULL timeout, i.e. forever. That is not a defect to work around — it is
// how the driver and the game's mixer thread rendezvous, and A5 shows the same
// shape: the callback runs on a thread that is not one of the guest's own
// (`0100001C`, Xenia's audio worker) and waits on 2 objects, while the game's mixer
// thread F80000E8 waits on 3.
void RenderDriverPump()
{
    // CPU 4. Arbitrary and labelled as such: nothing in either capture says which
    // hardware thread the 360 routes the audio driver to, and unlike the graphics
    // ISR (CPU 2, which the console documents) there is no number to match. It is
    // distinct from the guest's own threads, which is the only property used.
    GuestThreadContext threadContext(4);
    uint8_t* base = g_memory.base;

    const char* env = getenv("CZ_AUDIO_FRAME_US");
    const int frameUs = env ? std::max(100, atoi(env)) : kDefaultFrameUs;
    KLOG("render driver pump started (%d us/frame, %u samples x %u channels)\n", frameUs,
         kFrameSamples, kFrameChannels);
    if (NullDecoderEnabled())
        fprintf(stderr,
                "[audio] *** CZ_XMA_NULL_DECODER IS ON: every XMA context's input is "
                "retired without being decoded. THIS FABRICATES PLAYBACK PROGRESS — "
                "it is a measurement arm and must not be on for a gate run. ***\n");

    // Sleep BEFORE the first callback, not after it. A render driver asks for a
    // frame when its frame clock ticks, never because a client just registered, and
    // the difference is observable here: the title registers the client from one
    // thread while the mixer thread is still constructing the object the callback
    // dereferences (sub_828869B0 is `lwz r3,0(r3); b sub_828867E8` — the context is a
    // pointer to a pointer, and the inner one is written last). Called immediately,
    // the callback reads two null wait objects.
    //
    // The delay is fidelity, NOT the fix — a race lost by 5 ms is still a race.
    // What makes this correct regardless of timing is that a wait on a null object
    // now fails instead of hanging (kernel/imports.cpp), so a callback that arrives
    // early returns without mixing and the next frame retries. A5 shows hardware
    // taking the same shape: 709 log lines separate the registration from the
    // first XAudioSubmitRenderDriverFrame.
    auto deadline = std::chrono::steady_clock::now();
    for (;;)
    {
        // Pace first, so the very first frame is a frame period after registration.
        // Never bank credit: the callback blocks until the game's mixer has a
        // buffer, so after a long stall `deadline` is far in the past and sleeping
        // until it would fire a burst of catch-up frames the game never produced.
        deadline += std::chrono::microseconds(frameUs);
        const auto now = std::chrono::steady_clock::now();
        if (deadline < now)
            deadline = now;
        std::this_thread::sleep_until(deadline);

        // Before the callback, so a frame the guest is about to inspect sees the
        // contexts in the state a decoder running concurrently would have left them.
        if (NullDecoderEnabled())
            NullDecoderConsume(frameUs);

        const uint32_t callback = g_clientCallback.load();
        if (!callback)
            continue;

        PPCFunc* func = g_memory.FindFunction(callback);
        if (!func)
        {
            KLOG("render driver callback %08X was not recompiled — no audio frames "
                 "will be requested\n",
                 callback);
            return;
        }

        // The cell, not the context — see the note above the declaration.
        threadContext.ppcContext.r3.u64 = g_clientContextCell;
        try
        {
            func(threadContext.ppcContext, base);
        }
        catch (const GuestThreadExit&)
        {
            KLOG("render driver callback terminated its thread — pump stopping\n");
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// The imports
// ---------------------------------------------------------------------------

// One out-parameter, and exactly one bit of it is load-bearing. sub_82886B70 does:
//
//     lwz     r11,80(r1)           ; the value we write here
//     rlwinm  r11,r11,0,0,0        ; & 0x80000000     <- the ONLY bit it reads
//     subfic/subfe/rlwinm/addi     ; -> 4 if clear, 2 if set
//     rlwinm  r11,r11,8,0,23       ; << 8  -> 0x400 or 0x200
//
// and the mixing loop in the same object then de-interleaves with a hard-coded
// 1024-byte (= 4 bytes/sample) channel stride regardless. So bit 31 must be CLEAR
// or the title's own two halves disagree about its sample size. That is a
// constraint the guest states about itself, and it is the only one we have: no
// other bit of this value is read anywhere in the image, and the capture cannot
// show what Xenia returned (gotcha 60). So 0x00010001 is a plausible stereo config
// with bit 31 clear, and the ONLY part of it this title can observe is that bit.
// Do not read the low bits as a claim about the console's encoding — they are not
// evidence, and nothing here depends on them.
uint32_t XAudioGetSpeakerConfig_x(be<uint32_t>* config)
{
    if (!config)
        return STATUS_INVALID_PARAMETER;
    *config = 0x00010001;
    return STATUS_SUCCESS;
}

uint32_t XAudioRegisterRenderDriverClient_x(be<uint32_t>* callbackPair,
                                            be<uint32_t>* driverHandle)
{
    if (!callbackPair || !driverHandle)
        return STATUS_INVALID_PARAMETER;

    const uint32_t callback = callbackPair[0];
    const uint32_t context = callbackPair[1];
    *driverHandle = kDriverHandle;

    // The cell outlives this call because the callback dereferences it on every
    // frame, long after the caller's stack copy of the pair is gone.
    if (!g_clientContextCell)
    {
        void* cell = g_heap.Alloc(sizeof(uint32_t));
        if (!cell)
        {
            KLOG("XAudioRegisterRenderDriverClient: out of heap for the context cell\n");
            return STATUS_NO_MEMORY;
        }
        g_clientContextCell = g_memory.MapVirtual(cell);
    }
    *reinterpret_cast<be<uint32_t>*>(g_memory.Translate(g_clientContextCell)) = context;

    KLOG("XAudioRegisterRenderDriverClient callback=%08X context=%08X (via cell %08X) "
         "-> handle %08X\n",
         callback, context, g_clientContextCell, kDriverHandle);

    g_clientCallback = callback;

    // CZ_NO_AUDIO_PUMP=1 registers the client and answers every call honestly but
    // never invokes the callback — so the driver exists and no audio frame is ever
    // requested. This is the control arm for every claim of the form "driving the
    // callback did X" (gotcha 7): registering the client and pumping it are two
    // changes this module makes at once, and they need to be separable in the same
    // binary. It is also the fastest way to ask whether a hang lives in the guest's
    // mixer thread or in our pacing of it.
    if (getenv("CZ_NO_AUDIO_PUMP"))
    {
        KLOG("CZ_NO_AUDIO_PUMP: client registered but the callback will never run\n");
        return STATUS_SUCCESS;
    }

    bool expected = false;
    if (g_pumpRunning.compare_exchange_strong(expected, true))
        std::thread(RenderDriverPump).detach();
    return STATUS_SUCCESS;
}

uint32_t XAudioUnregisterRenderDriverClient_x(uint32_t handle)
{
    KLOG("XAudioUnregisterRenderDriverClient(%08X) after %llu frames\n", handle,
         static_cast<unsigned long long>(g_framesSubmitted.load()));
    g_clientCallback = 0;
    return STATUS_SUCCESS;
}

// The null sink. `frame` is kFrameBytes of planar float; we take it and drop it.
//
// The peak scan is behind CZ_AUDIO_TRACE because it touches 6 KB every frame, and
// because of what it answers: "the pump is running" and "the
// game is actually producing audio" are different claims, and a frame count alone
// cannot separate them from a mixer that is submitting silence.
uint32_t XAudioSubmitRenderDriverFrame_x(uint32_t handle, be<uint32_t>* frame)
{
    const uint64_t n = g_framesSubmitted.fetch_add(1);
    if (AudioTrace() && (n % 512) == 0)
    {
        float peak = 0.0f;
        if (frame)
        {
            for (uint32_t i = 0; i < kFrameBytes / 4; i++)
            {
                const uint32_t bits = frame[i];
                float s;
                memcpy(&s, &bits, sizeof(s));
                const float a = s < 0.0f ? -s : s;
                if (a > peak)
                    peak = a;
            }
        }
        KLOG("audio frame %llu handle=%08X peak=%.4f\n",
             static_cast<unsigned long long>(n), handle, peak);
    }
    return STATUS_SUCCESS;
}

// A5: XAudioGetVoiceCategoryVolume(00000001, 7042FD40(0)), 19,685 times.
//
// The out-parameter is a float — sub_82857B88 reads it back with `lfs f0,80(r1)`,
// compares it against a cached copy at +524 and only walks its voice list when it
// changed. So the value must be STABLE, and 1.0 (unattenuated) is the only choice
// that is not a claim about a user volume setting we do not model.
//
// The capture cannot tell us what Xenia returned, and it is worth saying why,
// because the log looks like it should: the printed `(0)` and `(1.4013E-45)` are
// the slot's contents BEFORE each call, and 1.4013E-45 is the float reading of the
// integer 1 the guest itself left there. Gotcha 60 — no capture shows what an
// import wrote.
uint32_t XAudioGetVoiceCategoryVolume_x(uint32_t category, be<uint32_t>* volume)
{
    if (!volume)
        return STATUS_INVALID_PARAMETER;
    const float one = 1.0f;
    uint32_t bits;
    memcpy(&bits, &one, sizeof(bits));
    *volume = bits;
    return STATUS_SUCCESS;
}

// sub_8285EE58 walks its own 96-byte-per-entry stream table and calls this once per
// entry that does not have a context yet:
//
//     lwz  r11,64(r31)         ; entry->context
//     cmplwi cr6,r11,0
//     bne  cr6,<next>          ; already has one
//     mr   r3,r30              ; r3 = &entry->context   <- ONE argument, an OUT pointer
//     bl   XMACreateContext
//     mr.  r23,r3
//     blt  <fail>              ; signed: negative is failure
//
// then (when its flag 0x4 is set) copies 48 bytes of stream description into the
// context with three lvx128/stvx128 pairs and maps it for the hardware:
//
//     MmMapIoSpace(2, MmGetPhysicalAddress(ctx), 64, 0x404)
//
// which is A1 line 54,145 — gate position 84 — and 64 there is the third
// independent witness that a context is 64 bytes.
uint32_t XMACreateContext_x(be<uint32_t>* contextOut)
{
    if (!contextOut)
        return STATUS_INVALID_PARAMETER;

    std::lock_guard<std::mutex> lock(g_xmaMutex);
    if (!g_xmaContextArray)
    {
        // Audio_Init failed, or was never called. Say so rather than handing back a
        // pointer into nothing: the guest is about to compute a context index from
        // this address and program hardware with it.
        *contextOut = 0;
        KLOG("XMACreateContext: no context array — audio init did not run\n");
        return 0xC0000017; // STATUS_NO_MEMORY (negative -> the guest's `blt` fails)
    }

    for (uint32_t i = 0; i < kXmaContextCount; i++)
    {
        if (g_xmaContextUsed[i])
            continue;
        g_xmaContextUsed[i] = true;
        const uint32_t guest = g_xmaContextArray + i * kXmaContextSize;
        memset(g_memory.Translate(guest), 0, kXmaContextSize);
        *contextOut = guest;
        KLOG("XMACreateContext -> context %u at %08X (phys %08X)\n", i, guest,
             guest & 0x1FFFFFFF);
        return STATUS_SUCCESS;
    }

    // Loud, because the count above is the one number here that was not forced by
    // the guest's own arithmetic. If this ever fires, raise kXmaContextCount.
    *contextOut = 0;
    KLOG("XMACreateContext: all %u contexts are in use — raise kXmaContextCount\n",
         kXmaContextCount);
    return 0xC0000017;
}

// sub_8285EF68, the teardown, passes the context pointer itself and then zeroes its
// own entry->context. One argument.
void XMAReleaseContext_x(uint32_t context)
{
    if (!context)
        return;

    std::lock_guard<std::mutex> lock(g_xmaMutex);
    if (context < g_xmaContextArray ||
        context >= g_xmaContextArray + kXmaContextCount * kXmaContextSize ||
        (context - g_xmaContextArray) % kXmaContextSize != 0)
    {
        KLOG("XMAReleaseContext(%08X): not one of ours — ignoring\n", context);
        return;
    }
    g_xmaContextUsed[(context - g_xmaContextArray) / kXmaContextSize] = false;
}

} // namespace

void Audio_Init()
{
    // Page-aligned, and taken from the TOP of the physical arena. Both are for
    // fidelity to what the capture shows. A1 has the title reserving 447 MB at
    // physical 0x03D93000 and Xenia's XMA context array sitting at 0x1FCAA000 — one
    // page past that reservation's end, i.e. allocated out of the guest's way rather
    // than in front of it. Allocating bottom-up here moved the title's own 447 MB
    // block by 20 KB, which is a change to an unrelated subsystem bought for nothing
    // (gotcha 9: the guest builds its map out of the numbers the kernel returns).
    void* host = g_heap.AllocPhysical(kXmaContextCount * kXmaContextSize, 0x1000, true);
    if (!host)
    {
        KLOG("Audio_Init: could not allocate the XMA context array\n");
        return;
    }
    memset(host, 0, kXmaContextCount * kXmaContextSize);
    g_xmaContextArray = g_memory.MapVirtual(host);

    // The physical address, LITTLE-endian, because the guest reads this register
    // with lwbrx. Writing it big-endian would hand sub_8285EDF8 a byte-swapped base
    // and every context index computed from it would be nonsense — and nothing
    // would report an error, because the index is only ever used to pick which
    // hardware bit to set.
    const uint32_t phys = g_xmaContextArray & 0x1FFFFFFF;
    *reinterpret_cast<uint32_t*>(g_memory.Translate(kXmaContextArrayRegister)) = phys;

    fprintf(stderr,
            "[audio] XMA context array: %u x %u bytes at %08X (phys %08X), published "
            "to the decoder register at %08X\n",
            kXmaContextCount, kXmaContextSize, g_xmaContextArray, phys,
            kXmaContextArrayRegister);
}

uint32_t Audio_XmaContextArray() { return g_xmaContextArray; }
unsigned Audio_XmaContextCount() { return kXmaContextCount; }
unsigned Audio_XmaContextSize() { return kXmaContextSize; }
bool Audio_XmaContextInUse(unsigned index)
{
    return index < kXmaContextCount && g_xmaContextUsed[index];
}

GUEST_FUNCTION_HOOK(__imp__XAudioGetSpeakerConfig, XAudioGetSpeakerConfig_x)
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume_x)
GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient_x)
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient,
                    XAudioUnregisterRenderDriverClient_x)
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame_x)
GUEST_FUNCTION_HOOK(__imp__XMACreateContext, XMACreateContext_x)
GUEST_FUNCTION_HOOK(__imp__XMAReleaseContext, XMAReleaseContext_x)
