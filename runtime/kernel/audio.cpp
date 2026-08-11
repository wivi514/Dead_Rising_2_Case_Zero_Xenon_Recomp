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
// WHAT THIS IS NOT — CORRECTED IN PHASE A/V, AND THE OLD TEXT KEPT BECAUSE IT WAS
// TRUE FOR NINE PARTS
// -------------------------------------------------------------------------------
// This block used to read: "There is no audio OUTPUT here and no XMA decoding.
// Submitted frames are counted and dropped. That is a real null sink, not a fake
// success ... Decoding XMA and mixing to a host device are phase 5." That was an
// honest description of a null sink and it was the right thing to ship at the
// time — but it is also the exact shape of a subsystem that cannot report its own
// absence, and it took a two-sided instrument to establish that the silence was
// upstream of it rather than in it (open item 00e).
//
// Both halves exist now:
//   * `XmaDecodeThread` below decodes each context's XMA2 input into its PCM output
//     ring, which is the link the title's own mixer reads from;
//   * `runtime/audio/audio_out.cpp` queues the mixed 5.1 frame to an SDL device.
// `CZ_NO_XMA_DECODE=1` and `CZ_NO_AUDIO_OUT=1` restore the two old behaviours
// independently on the same binary, which is what makes either one measurable.
#include "audio.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <xbox.h>

#include "../audio/audio_out.h"
#include "../audio/xma_decoder.h"
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

// Defined with the submit path below; declared here because the pump runs the
// self-test before it asks the guest for its first frame.
void AudioScanSelfTest();

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

// ---------------------------------------------------------------------------
// THE XMA DECODER
// ---------------------------------------------------------------------------
//
// This is the half of the audio path that was missing, and it is why the game is
// mute. The chain is:
//
//   guest fills a context's INPUT buffer with 2 KB XMA2 packets
//     -> HARDWARE decodes them and writes 16-bit big-endian PCM into the
//        context's OUTPUT ring
//     -> the title's own software mixer reads that ring, mixes every voice, and
//        hands one 5.1 float frame to XAudioSubmitRenderDriverFrame
//
// Every link but the middle one already worked here, which is exactly why the
// symptom was so quiet: the mixer ran at the right cadence, submitted 74,753
// frames in one 400 s run, and every sample of every one of them was zero
// (`non-silent=0 maxpeak=0.000000`). A mixer mixing nothing looks identical to a
// mixer that is broken. Open item 00e is the measurement that separated them.
//
// WE DO NOT DECODE ON THE KICK. The title arms a context by setting a bit in the
// write-only register bitmap at 0x7FEA1A80 (quoted at the top of this file), and
// our address space is one flat mapping with no MMIO trapping — the store lands in
// ordinary RAM. Worse, the guest's arm loop writes every context's bit to the SAME
// dword in a tight loop, so a poller would routinely observe one kick where three
// happened. So we drive each context from ITS OWN STATE instead: input valid, an
// output ring, and room in it. That is strictly more forgiving than the hardware
// and cannot miss an edge, because it is not looking at edges.
//
// THE CONTEXT LAYOUT IS CHECKED, NOT ASSUMED. The bitfields below come from the
// 360's XMA_CONTEXT_DATA, and two of them this project derived independently from
// THIS title's own code (dword0 bits 20/21 are the input-valid flags — sub_8285EFE0
// reads exactly those; dword0's low 12 bits are input buffer 0's packet count).
// The rest — in particular WHICH dwords hold the three buffer pointers — is
// hardware documentation, i.e. a recollection until something here checks it. So
// `LayoutLooksSane` below tests the pointer dwords against the guest address space
// before the first decode and declines LOUDLY rather than decoding garbage. A
// wrong layout must not be able to present as silence, because silence is the
// symptom we are trying to remove (gotcha 5, and gotcha 30's rule that an
// instrument has to be able to fail).
constexpr uint32_t kXmaBytesPerPacket = 2048;
constexpr uint32_t kXmaBitsPerPacket = kXmaBytesPerPacket * 8;
constexpr uint32_t kXmaOutputBlockBytes = 256;  // the output ring's granularity
constexpr uint32_t kXmaSamplesPerFrame = 512;   // one XMA decode frame, per channel

// The 2-bit sample_rate field's encoding.
constexpr int kXmaSampleRates[4] = { 24000, 32000, 44100, 48000 };

// XMA_CONTEXT_DATA as sixteen big-endian dwords. Written out as explicit shifts
// rather than as a bitfield struct, because a bitfield's packing would have to
// match the 360 compiler's on a big-endian target — and because this way the
// layout is legible in the diff that gets it wrong.
struct XmaCtx
{
    uint32_t dw[16];

    // DWORD 0: input_buffer_0_packet_count:12, loop_count:8 (+12),
    //          input_buffer_0_valid:1 (+20), input_buffer_1_valid:1 (+21),
    //          output_buffer_block_count:5 (+22), output_buffer_write_offset:5 (+27)
    uint32_t in0Packets() const { return dw[0] & 0xFFF; }
    bool in0Valid() const { return (dw[0] >> 20) & 1; }
    bool in1Valid() const { return (dw[0] >> 21) & 1; }
    uint32_t outBlocks() const { return (dw[0] >> 22) & 0x1F; }
    uint32_t outWriteOffset() const { return (dw[0] >> 27) & 0x1F; }

    // DWORD 1: input_buffer_1_packet_count:12, loop_subframe_start:2 (+12),
    //          loop_subframe_end:3 (+14), loop_subframe_skip:3 (+17),
    //          subframe_decode_count:4 (+20), subframe_skip_count:3 (+24),
    //          sample_rate:2 (+27), is_stereo:1 (+29), unk:1, output_buffer_valid:1
    uint32_t in1Packets() const { return dw[1] & 0xFFF; }
    uint32_t sampleRateId() const { return (dw[1] >> 27) & 3; }
    bool isStereo() const { return (dw[1] >> 29) & 1; }
    bool outValid() const { return (dw[1] >> 31) & 1; }

    uint32_t inReadOffsetBits() const { return dw[2] & 0x3FFFFFF; }  // BITS, not bytes
    bool currentBuffer() const { return (dw[4] >> 31) & 1; }
    uint32_t in0Ptr() const { return dw[5]; }
    uint32_t in1Ptr() const { return dw[6]; }
    uint32_t outPtr() const { return dw[7]; }
    uint32_t outReadOffset() const { return dw[9] & 0x1F; }

    void setIn0Valid(bool v) { dw[0] = (dw[0] & ~(1u << 20)) | (uint32_t(v) << 20); }
    void setIn1Valid(bool v) { dw[0] = (dw[0] & ~(1u << 21)) | (uint32_t(v) << 21); }
    void setOutWriteOffset(uint32_t o)
    {
        dw[0] = (dw[0] & ~(0x1Fu << 27)) | ((o & 0x1F) << 27);
    }
    void setOutValid(bool v) { dw[1] = (dw[1] & ~(1u << 31)) | (uint32_t(v) << 31); }
    void setInReadOffsetBits(uint32_t b) { dw[2] = (dw[2] & ~0x3FFFFFFu) | (b & 0x3FFFFFF); }
    void setCurrentBuffer(bool v) { dw[4] = (dw[4] & ~(1u << 31)) | (uint32_t(v) << 31); }
};

// Host-side state for one context: the decoder and whatever PCM it has produced
// that has not been handed to the guest yet.
struct XmaHostCtx
{
    XmaDecoder* dec = nullptr;
    int decChannels = 0;
    int decRate = 0;
    std::vector<float> pcm;
    size_t pcmPos = 0;
    bool announced = false;
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t starves = 0;
    float peak = 0.0f;
    // "The ring got no audio" has two completely different causes and they were
    // indistinguishable in the first version of this: libavcodec REFUSED the packet
    // (wrong format, wrong extradata, not XMA2 at that address), or it accepted it
    // and returned fewer samples than a whole decode frame. The first is a defect
    // here; the second is normal on the first packet or two while the decoder
    // primes. Counting them apart is the difference between "our decoder is wrong"
    // and "the guest has not streamed enough yet".
    uint64_t decodeCalls = 0;
    uint64_t decodeRefused = 0;
    uint64_t samplesOut = 0;
    // How often we told the guest "ring full". This is the direct measure of the
    // handshake that open-items 00j is about: a live 50 Hz sample caught it firing
    // ~94 times in 8 s on every dialogue voice and never on the music voice, and the
    // prediction for the ring fix is that this rate collapses. Counting it here
    // means the claim is checkable from a headless log instead of needing an
    // operator stuck in the defect (gotcha 151 — an arm with no counter cannot be
    // shown to have engaged).
    uint64_t ringFull = 0;
    bool probed = false;
};

XmaHostCtx g_xmaHost[kXmaContextCount];
std::atomic<bool> g_xmaDecodeRunning{ false };
std::atomic<uint64_t> g_xmaFramesDecoded{ 0 };
uint32_t g_xmaMaxPeakBits = 0;

bool XmaDecodeLog()
{
    static const bool on = getenv("CZ_XMA_DECODE_LOG") != nullptr;
    return on;
}

// CZ_NO_XMA_DECODE=1 is the SAME-BINARY CONTROL ARM for the whole of this section:
// contexts are still allocated, the register file is still published, the pump
// still runs — nothing decodes. That is the runtime as it was before this part, so
// every "you can now hear X" claim has an off-state measured on the same build
// (gotcha 7).
bool XmaDecodeDisabled()
{
    static const bool off = getenv("CZ_NO_XMA_DECODE") != nullptr;
    return off;
}

// PPC_LOAD_U32/PPC_STORE_U32 expand to expressions referring to a local `base`,
// which the recompiled functions have as a parameter and we do not; naming it here
// keeps the macro usable and the byte-swapping in one place.
inline uint32_t XmaLoadBE(uint32_t va)
{
    uint8_t* base = g_memory.base;
    return PPC_LOAD_U32(va);
}

void XmaReadCtx(uint32_t va, XmaCtx& c)
{
    for (int w = 0; w < 16; w++)
        c.dw[w] = XmaLoadBE(va + w * 4);
}

void XmaWriteDword(uint32_t addr, int w, uint32_t v)
{
    uint8_t* base = g_memory.base;
    PPC_STORE_U32(addr + w * 4, v);
}

// THE CONTEXT'S THREE BUFFER POINTERS ARE PHYSICAL ADDRESSES, NOT VIRTUAL ONES,
// AND THAT IS THE WHOLE DEFECT THIS SECTION EXISTS TO NOT HAVE.
//
// The XMA decoder is a DMA device: it addresses physical memory, so a title writes
// physical addresses into the context and the console's MMU makes them the same
// bytes the CPU sees through a cached virtual alias. Our address space is one flat
// 4 GB map where the physical arena is a WINDOW at 0xA0000000 (kernel/memory.h:
// three views of one memfd at 0xA0000000/0xC0000000/0xE0000000), so a physical
// address and its virtual alias are two different offsets into `base` and nothing
// aliases them for us.
//
// Measured, not reasoned. The title's first voice is the title-screen music:
//
//   NtReadFile('game:\data\audio\music.big', 131072 bytes @ 16666624)
//        -> 131072 into A2538000
//   [xma] ctx0 in0=02538000 64 pkts (131072 bytes): 0 non-zero (0.00%)
//
// Same page, two addresses, 0xA0000000 apart — and 16,666,624 is exactly
// `PressStartPrologue.xma`'s offset in the archive while 131,072 is exactly 64
// packets. The guest was doing everything right and reading the file into the
// buffer it had told the hardware about; we were looking at the wrong copy of it.
// A decoder pointed at the untranslated address reads a page of zeros, decodes
// silence, and reproduces the exact symptom it was written to fix.
//
// This is the same convention MmGetPhysicalAddress_x already implements in the
// other direction (`address >= 0xA0000000 ? address & 0x1FFFFFFF : address`), so
// the two are inverses and neither is a guess.
constexpr uint32_t kPhysicalWindow = 0xA0000000;
constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;

inline uint32_t XmaPhysToVirtual(uint32_t phys)
{
    // Already a virtual alias? Leave it. Nothing in this title does that today, but
    // an XMA context filled by some other path should not be corrupted by a blind
    // OR, and the check costs one compare.
    if (phys >= kPhysicalWindow)
        return phys;
    return kPhysicalWindow | (phys & kPhysicalMask);
}

// Is this a plausible guest buffer pointer? Takes the PHYSICAL value as the guest
// wrote it, and judges the address we would actually touch.
//
// THE FIRST VERSION OF THIS WAS VACUOUS and the compiler said so: `p <
// PPC_MEMORY_SIZE` is always true for a uint32_t against a 4 GB map, so the test
// could not have rejected anything (gotcha 30 — a check that has never failed has
// not been shown capable of failing, and this one was provably incapable). The
// failable form tests the two things a MIS-ASSIGNED dword actually looks like: a
// bitfield or a small count, which is below the physical arena's floor once
// translated, and a misaligned value, where every buffer the XMA hardware is
// handed is at least dword-aligned.
bool XmaPlausiblePtr(uint32_t phys, uint32_t bytes)
{
    if (!phys || (phys & 3))
        return false;
    const uint64_t v = XmaPhysToVirtual(phys);
    return v + bytes <= uint64_t(PPC_MEMORY_SIZE);
}

// Checked once per context, the first time it goes live, and it is allowed to
// REFUSE. A context whose declared pointers are not addresses means the dword
// assignment above is wrong for this title, and decoding from it would produce
// noise or silence — both of which look like the defect we are fixing.
bool XmaLayoutLooksSane(unsigned i, uint32_t va, const XmaCtx& c)
{
    const bool cur = c.currentBuffer();
    const uint32_t inPtr = cur ? c.in1Ptr() : c.in0Ptr();
    const bool inOk = XmaPlausiblePtr(inPtr, kXmaBytesPerPacket);
    const bool outOk = XmaPlausiblePtr(c.outPtr(), c.outBlocks() * kXmaOutputBlockBytes);
    if (inOk && outOk)
        return true;
    fprintf(stderr,
            "[xma] ctx%u DECLINED: the context layout does not hold here — in%d=%08X "
            "out=%08X blocks=%u. Decoding was skipped rather than guessed. Raw:\n"
            "[xma]   %08X %08X %08X %08X %08X %08X %08X %08X\n"
            "[xma]   %08X %08X %08X %08X %08X %08X %08X %08X\n",
            i, cur ? 1 : 0, inPtr, c.outPtr(), c.outBlocks(), c.dw[0], c.dw[1], c.dw[2],
            c.dw[3], c.dw[4], c.dw[5], c.dw[6], c.dw[7], c.dw[8], c.dw[9], c.dw[10],
            c.dw[11], c.dw[12], c.dw[13], c.dw[14], c.dw[15]);
    (void)va;
    return false;
}

// Decode one 2 KB packet of the context's CURRENT input buffer. Returns false when
// there is nothing more to decode, having performed the buffer retirement the
// hardware would perform — which is the transition sub_8285EFE0 is watching for,
// and the thing `CZ_XMA_NULL_DECODER` had to fake because nothing here did it.
bool XmaDecodeOnePacket(unsigned i, uint32_t va, XmaHostCtx& hc, XmaCtx& c)
{
    const bool cur = c.currentBuffer();
    const uint32_t inPtr = cur ? c.in1Ptr() : c.in0Ptr();
    const uint32_t count = cur ? c.in1Packets() : c.in0Packets();
    const bool valid = cur ? c.in1Valid() : c.in0Valid();
    if (!valid || !inPtr || !count)
        return false;

    const uint32_t packet = c.inReadOffsetBits() / kXmaBitsPerPacket;
    if (packet >= count)
    {
        // The buffer is consumed. Invalidate it, and swap to the other one ONLY IF THE
        // OTHER ONE IS ACTUALLY VALID.
        //
        // The unconditional swap this used to do is what killed the cinematic audio
        // after 4.9 seconds of a 316-second clip (open-items 00j). XMA_CONTEXT_DATA has
        // two input buffers and the hardware alternates them, so "retire this one and
        // move to the other" reads like the obvious transcription. **This title never
        // uses buffer 1.** Censused over a whole prologue run: 136 context dumps, and
        // `in1Ptr` is 0 in every single one while `in1Valid` is never set on any
        // context. It streams by re-arming buffer 0 IN PLACE, swapping only the pointer
        // — ctx0's alternates A2538000 / A255E000 forever while `currentBuffer` stays 0.
        //
        // So the swap moved the context to a buffer that does not exist, and nothing
        // could ever move it back: the walk reads `currentBuffer` at its top to decide
        // which buffer to look at, sees buffer 1, finds it invalid, and returns false
        // for the rest of the process. The guest can re-arm buffer 0 as often as it
        // likes and we will not look at it. `ctx7` was caught in exactly that state —
        // `valid=00 cur=1 readPacket=0`, unchanged across 28 consecutive dumps — while
        // its two sibling contexts sat at packet 61 and 63 of 64 with full output rings,
        // which is what a 5.1 mixer waiting on a dead third stream looks like.
        //
        // Staying put is also what the hardware does. The buffer-switch is conditional
        // on the other buffer being flagged; with a single-buffer producer the decoder
        // parks on the buffer it has and resumes when the guest re-flags it, which is
        // precisely the transition sub_8285EFE0 polls for. For a genuine double-buffered
        // stream this is a no-op, because there the other buffer IS valid.
        const bool otherValid = cur ? c.in0Valid() : c.in1Valid();
        if (cur)
            c.setIn1Valid(false);
        else
            c.setIn0Valid(false);
        if (otherValid)
            c.setCurrentBuffer(!cur);
        c.setInReadOffsetBits(0);
        XmaWriteDword(va, 0, c.dw[0]);
        XmaWriteDword(va, 2, c.dw[2]);
        XmaWriteDword(va, 4, c.dw[4]);
        return false;
    }

    const int channels = c.isStereo() ? 2 : 1;
    const int rate = kXmaSampleRates[c.sampleRateId()];
    if (!hc.dec || hc.decChannels != channels || hc.decRate != rate)
    {
        Xma_Destroy(hc.dec);
        hc.dec = Xma_Create(channels, rate);
        hc.decChannels = channels;
        hc.decRate = rate;
        hc.pcm.clear();
        hc.pcmPos = 0;
        if (!hc.dec)
            return false;
    }

    const uint32_t srcPhys = inPtr + packet * kXmaBytesPerPacket;
    if (!XmaPlausiblePtr(srcPhys, kXmaBytesPerPacket))
        return false;
    const uint32_t src = XmaPhysToVirtual(srcPhys);
    if (hc.pcmPos)
    {
        hc.pcm.erase(hc.pcm.begin(), hc.pcm.begin() + hc.pcmPos);
        hc.pcmPos = 0;
    }

    // The first packet of a context, dumped and independently validated, because a
    // decoder that returns nothing is silent in exactly the way the bug is. The
    // header bytes say whether this is XMA2 at all; `Xma_Validate` says what
    // libavcodec makes of a run of packets, INDEPENDENTLY of the ring plumbing
    // around it. Without this pair, "0 frames decoded" cannot be attributed.
    if (!hc.probed && XmaDecodeLog())
    {
        hc.probed = true;
        const uint8_t* p = g_memory.base + src;
        fprintf(stderr,
                "[xma] ctx%u first packet @%08X: %02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X\n",
                i, src, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
                p[11]);
        Xma_Validate(p, kXmaBytesPerPacket * (count > 16 ? 16 : count),
                     c.isStereo() ? 2 : 1, kXmaSampleRates[c.sampleRateId()]);
    }

    const int got = Xma_DecodePacket(hc.dec, g_memory.base + src, kXmaBytesPerPacket, hc.pcm);
    hc.decodeCalls++;
    if (got < 0)
        hc.decodeRefused++;
    else
        hc.samplesOut += uint64_t(got);
    hc.packets++;

    c.setInReadOffsetBits((packet + 1) * kXmaBitsPerPacket);
    XmaWriteDword(va, 2, c.dw[2]);
    return true;
}

// Move decoded PCM into the guest's output ring as 16-bit BIG-ENDIAN samples.
//
// The hardware writes whole DECODE FRAMES — 512 samples per channel — and never a
// partial one, and the ring's offsets are in 256-byte blocks (4 blocks mono, 8
// stereo). Writing a sub-frame amount leaves the mixer reading a frame that is half
// this decode and half the last one, which is a buzz rather than silence and would
// be diagnosed as a decoder fidelity problem.
void XmaFillOutput(unsigned i, uint32_t va, XmaHostCtx& hc, XmaCtx& c)
{
    const uint32_t blocks = c.outBlocks();
    if (!blocks || !XmaPlausiblePtr(c.outPtr(), blocks * kXmaOutputBlockBytes))
        return;
    const uint32_t outPtr = XmaPhysToVirtual(c.outPtr());

    const uint32_t channels = c.isStereo() ? 2u : 1u;
    const uint32_t frameSamples = kXmaSamplesPerFrame * channels;
    const uint32_t frameBlocks = (frameSamples * 2) / kXmaOutputBlockBytes;
    if (!frameBlocks || frameBlocks > blocks)
        return;

    uint32_t write = c.outWriteOffset() % blocks;
    const uint32_t read = c.outReadOffset() % blocks;
    const uint32_t ringBytes = blocks * kXmaOutputBlockBytes;

    // A REAL AMBIGUITY, AND A REFUTED HYPOTHESIS. READ BOTH BEFORE CHANGING THIS.
    //
    // `write == read` means EMPTY here and FULL twenty lines below. That is one state
    // with two opposite meanings, it is genuinely wrong, and two offsets can only
    // encode `blocks` fill levels so the collision is structural. It was also the
    // leading candidate for open-items 00j — the cinematic that plays forward ~1 s,
    // backward ~1 s, forever, from the moment a character speaks — because a live
    // 50 Hz sample of the stuck process caught `output_buffer_valid` toggling ~94
    // times in 8 s on every mono dialogue voice and never once on the stereo music
    // voice, and because the amount of audio buffered IS the latency this title feeds
    // to a PID controller that slews cinematic playback rate (`Cine.Audio P-gain` /
    // `I-gain` / `D-gain` / `Cor Latency`).
    //
    // IT IS NOT THE CAUSE. Repaired properly — reserve one slot so `write` can never
    // land on `read`, making "equal" unambiguously empty — and measured on the
    // prologue against the same recipe, the loop did not move at all:
    //
    //     before   runs/distinct 6.13   distinct 1170
    //     after    runs/distinct 6.14   distinct 1170
    //
    // An identical `distinct` is the same scene revisiting the same pose set the same
    // way. So the ring handshake is not what the PID is reacting to, and the repair
    // was reverted rather than kept: it also made the "full" signal fire on every
    // fill instead of ~12 times a second, because the post-loop test it needed
    // (`freeBlocks <= frameBlocks`) is exactly the loop's own exit condition and
    // therefore vacuous — the same shape of mistake as a bounds check that cannot
    // fail. Keeping a behaviour change that is motivated by a refuted hypothesis and
    // measured to make one number worse is not a trade this file makes.
    //
    // The ambiguity is still real and still worth fixing one day, on its own
    // evidence: the damage it can do is overwriting up to three frames (~32 ms) of
    // audio the mixer has not played, which is a click and not a loop. Fix it with
    // its own prediction and its own arm, not as a rider on something else.
    uint32_t freeBlocks = (write == read) ? blocks : ((read - write + blocks) % blocks);

    // A BOUND ON PACKETS PER TICK, and it is not a performance guard.
    //
    // Without it, a decoder that returns nothing walks the entire input buffer in a
    // single 1 ms tick — 64 packets, ~2.7 s of audio — and then retires it, which
    // reads downstream as "the voice finished" and destroys the evidence. The first
    // run of this code did exactly that: 64 packets consumed, zero samples produced,
    // buffer retired, and the only symptom was `0f/starve2`. One XMA2 packet carries
    // ~2,048 samples per channel and a decode frame is 512, so any healthy decoder
    // needs at most one packet per frame; 8 is generous and still stops a runaway.
    unsigned packetBudget = 8;

    bool wrote = false;
    while (freeBlocks >= frameBlocks)
    {
        while (hc.pcm.size() - hc.pcmPos < frameSamples)
        {
            if (!packetBudget--)
                goto done;
            if (!XmaDecodeOnePacket(i, va, hc, c))
                goto done;
        }

        for (uint32_t s = 0; s < frameSamples; s++)
        {
            float f = hc.pcm[hc.pcmPos + s];
            f = f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
            const int16_t v = int16_t(f * 32767.0f);
            const uint32_t byteOff = (write * kXmaOutputBlockBytes + s * 2) % ringBytes;
            uint8_t* dst = g_memory.base + outPtr + byteOff;
            dst[0] = uint8_t(uint16_t(v) >> 8);   // big-endian, as the mixer reads it
            dst[1] = uint8_t(uint16_t(v) & 0xFF);
            const float a = f < 0 ? -f : f;
            if (a > hc.peak)
                hc.peak = a;
        }
        hc.pcmPos += frameSamples;
        write = (write + frameBlocks) % blocks;
        freeBlocks -= frameBlocks;
        hc.frames++;
        g_xmaFramesDecoded.fetch_add(1, std::memory_order_relaxed);
        wrote = true;
    }
done:
    if (!wrote)
    {
        // The guest had room and we produced nothing: its streaming is behind, or
        // the ring is too small for a whole frame. Counted, because a stutter has to
        // be attributable rather than guessed at.
        if (freeBlocks >= frameBlocks)
            hc.starves++;
        return;
    }

    c.setOutWriteOffset(write);
    XmaWriteDword(va, 0, c.dw[0]);

    if (write == read)
    {
        hc.ringFull++;          // the `full` column of CZ_XMA_DECODE_LOG
        c.setOutValid(false);   // ring full: the guest drains it and re-flags
        XmaWriteDword(va, 1, c.dw[1]);
    }
}

void XmaDecodeThread()
{
    uint64_t tick = 0;
    bool declined[kXmaContextCount] = {};
    while (g_xmaDecodeRunning.load())
    {
        std::this_thread::sleep_for(std::chrono::microseconds(1000));

        std::lock_guard<std::mutex> lock(g_xmaMutex);
        if (!g_xmaContextArray)
            continue;
        for (unsigned i = 0; i < kXmaContextCount; i++)
        {
            if (!g_xmaContextUsed[i] || declined[i])
                continue;
            const uint32_t va = g_xmaContextArray + i * kXmaContextSize;
            XmaCtx c{};
            XmaReadCtx(va, c);
            if (!c.outPtr() || !c.outBlocks())
                continue;
            if (!c.in0Valid() && !c.in1Valid())
                continue;
            if (!c.outValid())
                continue;
            if (!g_xmaHost[i].announced)
            {
                if (!XmaLayoutLooksSane(i, va, c))
                {
                    declined[i] = true;
                    continue;
                }
                g_xmaHost[i].announced = true;
                if (XmaDecodeLog())
                    fprintf(stderr,
                            "[xma] ctx%u live: in0=%08X(%u pkts,v%d) in1=%08X(%u pkts,v%d) "
                            "out=%08X blocks=%u stereo=%d rate=%d\n",
                            i, c.in0Ptr(), c.in0Packets(), c.in0Valid(), c.in1Ptr(),
                            c.in1Packets(), c.in1Valid(), c.outPtr(), c.outBlocks(),
                            c.isStereo(), kXmaSampleRates[c.sampleRateId()]);
            }
            XmaFillOutput(i, va, g_xmaHost[i], c);
            uint32_t bits;
            memcpy(&bits, &g_xmaHost[i].peak, sizeof(bits));
            if (bits > g_xmaMaxPeakBits)
                g_xmaMaxPeakBits = bits;
        }

        // Per-context activity every 5 s: which voices decoded and how loud. That is
        // what tells a speech line from a voice that is playing silence.
        if (XmaDecodeLog() && ++tick % 5000 == 0)
        {
            char line[512];
            size_t o = 0;
            unsigned live = 0;
            for (unsigned i = 0; i < kXmaContextCount && o < sizeof(line) - 40; i++)
            {
                if (!g_xmaContextUsed[i])
                    continue;
                live++;
                // `decodeCalls` is in this predicate deliberately: without it the
                // line hides exactly the context this instrument exists to find —
                // one that is being asked to decode and refusing (gotcha 264, a
                // filter that selects on the property under test).
                if (!g_xmaHost[i].frames && !g_xmaHost[i].starves &&
                    !g_xmaHost[i].decodeCalls)
                    continue;
                o += snprintf(line + o, sizeof(line) - o,
                              " %u:%lluf/pk%.2f/starve%llu/pkt%llu/refused%llu/smp%llu/full%llu", i,
                              (unsigned long long)g_xmaHost[i].frames, g_xmaHost[i].peak,
                              (unsigned long long)g_xmaHost[i].starves,
                              (unsigned long long)g_xmaHost[i].decodeCalls,
                              (unsigned long long)g_xmaHost[i].decodeRefused,
                              (unsigned long long)g_xmaHost[i].samplesOut,
                              (unsigned long long)g_xmaHost[i].ringFull);
                g_xmaHost[i].frames = 0;
                g_xmaHost[i].starves = 0;
                g_xmaHost[i].peak = 0.0f;
                g_xmaHost[i].decodeCalls = 0;
                g_xmaHost[i].decodeRefused = 0;
                g_xmaHost[i].samplesOut = 0;
                g_xmaHost[i].ringFull = 0;
            }
            fprintf(stderr, "[xma] %u ctx allocated, active:%s\n", live,
                    o ? line : " (none decoded)");
        }
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
    if (AudioTrace())
        AudioScanSelfTest();
    if (NullDecoderEnabled())
        fprintf(stderr,
                "[audio] *** CZ_XMA_NULL_DECODER IS ON: every XMA context's input is "
                "retired without being decoded. THIS FABRICATES PLAYBACK PROGRESS — "
                "it is a measurement arm and must not be on for a gate run. ***\n");

    // THE PUMP'S RATE IS A NUMBER, NOT AN INTENTION.
    //
    // Each render callback produces exactly 256 samples, so the callback rate IS the
    // output sample rate: 48 kHz demands 187.5/s. Fable 2's port slept a fixed
    // `sleep_for(5333us)` and measured 183-185/s, and that ~2% deficit drains the
    // device queue on a cycle and stutters everything continuously — a defect that
    // sounds exactly like a broken decoder, which is why `docs/audio-xma.md` records
    // it as a trap rather than as a fix.
    //
    // Ours has always been `sleep_until` on an accumulating deadline rather than
    // `sleep_for`, so it does not have that defect — but nobody had ever printed the
    // number, and both `docs/open-items.md` 00e and the phase A/V plan describe this
    // pump as the broken kind. A rate nobody measured is a rate nobody knows
    // (gotcha 13). This line is one `if` per frame off the hot path and it settles it.
    uint64_t rateFrames = 0;
    auto rateSince = std::chrono::steady_clock::now();

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

        // The callback has now called XAudioSubmitRenderDriverFrame (or declined to,
        // which is itself the thing this counts). Rate over a whole second, so a
        // single late wake-up cannot be read as a deficit.
        if (AudioTrace() && ++rateFrames >= 512)
        {
            const auto t = std::chrono::steady_clock::now();
            const double secs =
                std::chrono::duration<double>(t - rateSince).count();
            if (secs >= 1.0)
            {
                fprintf(stderr,
                        "[audio] pump rate %.1f callbacks/s (48 kHz at %u samples "
                        "needs %.1f), queued=%d frames\n",
                        rateFrames / secs, kFrameSamples, 48000.0 / kFrameSamples,
                        Audio_Out_QueuedFrames());
                rateFrames = 0;
                rateSince = t;
            }
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
//
// THE INSTRUMENT WAS REWRITTEN IN PART 26, BECAUSE ITS FIRST VERSION COULD NOT TELL
// SILENCE FROM BLINDNESS — which is the one distinction the whole audio item turns
// on. It sampled one frame in 512 and printed a peak that read 0.0000 both when the
// mixer handed us a silent buffer and when it handed us a NULL pointer, because the
// scan sat inside `if (frame)` and `peak` was initialised to zero (gotcha 151: an
// arm with no counter cannot be shown to have engaged). Part 25 read 53 samples of
// `peak=0.0000` and concluded "the guest is handing us silence"; that conclusion was
// not yet supported by the instrument that produced it.
//
// Three changes, each closing one of those holes:
//   * EVERY frame is scanned, not one in 512. The cost is ~187 x 6 KB = 1.1 MB/s and
//     only when the trace is on, against a sampled statistic that could have missed a
//     sound entirely — 53 of 27,000 frames is 0.2% of the run (gotcha 109).
//   * The NULL-frame case is counted separately, so "silent" and "there was nothing
//     there" are different numbers rather than the same zero.
//   * The scan is SELF-TESTED at pump start on a synthetic buffer whose peak is known
//     (AudioScanSelfTest below). A scanner that reads the wrong byte order or the
//     wrong length reports zeros on any input, which is indistinguishable from
//     silence — so the scanner has to be shown capable of reporting non-silence
//     before its zeros mean anything (gotcha 30).
float AudioFramePeak(const be<uint32_t>* frame)
{
    float peak = 0.0f;
    for (uint32_t i = 0; i < kFrameBytes / 4; i++)
    {
        const uint32_t bits = frame[i];
        float s;
        memcpy(&s, &bits, sizeof(s));
        const float a = s < 0.0f ? -s : s;
        if (a > peak)
            peak = a;
    }
    return peak;
}

// The positive control for AudioFramePeak: a frame of big-endian 0.5f, built the same
// way the guest's mixer would leave one, scanned by the same code path. If this does
// not print 0.5000 the scanner is broken and every zero it reports downstream is
// meaningless.
void AudioScanSelfTest()
{
    std::vector<be<uint32_t>> probe(kFrameBytes / 4, 0u);
    const float zeroPeak = AudioFramePeak(probe.data());

    const float half = 0.5f;
    uint32_t bits;
    memcpy(&bits, &half, sizeof(bits));
    for (auto& w : probe)
        w = bits;

    KLOG("audio scan self-test: zero frame peak=%.4f (expect 0.0000), "
         "loud frame peak=%.4f (expect 0.5000)\n",
         zeroPeak, AudioFramePeak(probe.data()));
}

uint32_t XAudioSubmitRenderDriverFrame_x(uint32_t handle, be<uint32_t>* frame)
{
    const uint64_t n = g_framesSubmitted.fetch_add(1);

    // The whole of the output path: this frame is everything the title's mixer
    // wants you to hear, already summed. Downmix and queue it. Deliberately BEFORE
    // the trace block, so `CZ_AUDIO_TRACE` cannot change what is played.
    if (frame)
        Audio_Out_SubmitFrame(frame);

    if (AudioTrace())
    {
        static std::atomic<uint64_t> nullFrames{0};
        static std::atomic<uint64_t> nonSilent{0};
        static std::atomic<uint64_t> firstNonSilent{0};
        static std::atomic<uint32_t> maxPeakBits{0};

        float peak = 0.0f;
        if (!frame)
            nullFrames.fetch_add(1);
        else
            peak = AudioFramePeak(frame);

        if (peak > 0.0f)
        {
            if (nonSilent.fetch_add(1) == 0)
                firstNonSilent.store(n);
            uint32_t bits;
            memcpy(&bits, &peak, sizeof(bits));
            // Positive floats compare in the same order as their bit patterns, so a
            // max over the bits is a max over the values without a CAS on a float.
            uint32_t prev = maxPeakBits.load();
            while (bits > prev && !maxPeakBits.compare_exchange_weak(prev, bits))
                ;
        }

        if ((n % 512) == 0)
        {
            float maxPeak;
            const uint32_t bits = maxPeakBits.load();
            memcpy(&maxPeak, &bits, sizeof(maxPeak));
            // The guest ADDRESS of the buffer is on the line because the self-test can
            // only prove the scanner reads floats correctly — it cannot prove we are
            // reading the buffer the mixer wrote. If this address is stable across the
            // run while the samples stay zero, the mixer is being handed one buffer and
            // filling it with silence; if it moves, we are following the guest's own
            // rotation and still finding silence. Either reading is a fact about the
            // GUEST. A zero here would say the argument never arrived at all.
            const uint32_t va =
                frame ? uint32_t(reinterpret_cast<uint8_t*>(frame) - g_memory.base) : 0;
            KLOG("audio frame %llu handle=%08X buf=%08X peak=%.4f | frames=%llu null=%llu "
                 "non-silent=%llu (first at %llu) maxpeak=%.6f\n",
                 static_cast<unsigned long long>(n), handle, va, peak,
                 static_cast<unsigned long long>(n + 1),
                 static_cast<unsigned long long>(nullFrames.load()),
                 static_cast<unsigned long long>(nonSilent.load()),
                 static_cast<unsigned long long>(firstNonSilent.load()), maxPeak);
        }
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
    const uint32_t index = (context - g_xmaContextArray) / kXmaContextSize;
    g_xmaContextUsed[index] = false;

    // Tear the decoder down with the context. Not housekeeping: a decoder carries
    // the stream's state, so a context reused for a DIFFERENT voice that inherited
    // the previous stream's decoder would decode the new packets against the old
    // history — which is noise, and noise sounds like a broken decoder rather than
    // like a leak.
    Xma_Destroy(g_xmaHost[index].dec);
    g_xmaHost[index] = XmaHostCtx{};
    memset(g_memory.base + context, 0, kXmaContextSize);
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

    // The decoder. Three ways it can legitimately not start, and each of them says
    // so, because a silent game with no line in the log is the exact failure this
    // whole item was about.
    if (XmaDecodeDisabled())
    {
        fprintf(stderr, "[audio] XMA decode disabled (CZ_NO_XMA_DECODE) — the control "
                        "arm: contexts allocate, nothing decodes, the game is mute\n");
        return;
    }
    if (NullDecoderEnabled())
    {
        // Mutually exclusive by construction. The null arm exists to move the guest's
        // state machine WITHOUT decoding, so running it alongside a real decoder would
        // retire packets the decoder had not read and produce a third behaviour that
        // is neither arm — and it fabricates progress, so it must win nothing silently.
        fprintf(stderr, "[audio] CZ_XMA_NULL_DECODER is set, so the REAL decoder is not "
                        "starting. Unset it to hear anything.\n");
        return;
    }
    if (!Xma_CodecAvailable())
    {
        fprintf(stderr, "[audio] libavcodec has no XMA2 decoder — the game will be mute. "
                        "This is a build/packaging problem, not a guest one.\n");
        return;
    }

    g_xmaDecodeRunning.store(true);
    std::thread(XmaDecodeThread).detach();
    fprintf(stderr, "[audio] XMA decoder running (poll 1 ms over %u contexts); "
                    "CZ_NO_XMA_DECODE=1 is the control arm\n",
            kXmaContextCount);
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
