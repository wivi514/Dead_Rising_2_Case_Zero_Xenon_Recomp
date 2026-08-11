#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// libavcodec-backed XMA2 decoder — one instance per XMA hardware context.
//
// WHY THIS EXISTS
// ---------------
// The 360's APU decodes XMA2 in hardware. A title fills a context's input buffer
// with 2 KB XMA2 packets, points its output buffer at a PCM ring, and the hardware
// walks the packets and writes 16-bit big-endian PCM into that ring. The title's
// OWN software mixer then reads the ring and mixes every voice into the one 5.1
// frame it hands to XAudioSubmitRenderDriverFrame.
//
// With no decoder that ring is never written, so every voice mixes silence and the
// game is mute while every layer above looks healthy — measured here as
// `non-silent=0 maxpeak=0.000000` over 15,000+ submitted frames (open-items 00e).
// That is the defect this file removes half of; kernel/audio.cpp owns the other
// half (the context walk that decides WHEN to decode and WHERE to put the result).
//
// PROVENANCE: lifted from ~/GithubRepo/Fable2XenonRecomp/runtime/audio/xma_decoder.cpp
// (this workspace's own earlier port, so the licence is ours), minus its
// `Xma_StartPump` — that was a Bink/attract-movie side channel that pushed decoded
// audio straight to the device around the guest mixer, and this title has no Bink
// (finding 7) and needs no bypass.
//
// One trap worth carrying across: Xenia does NOT decode whole packets. It parses
// individual XMA frames out of the packet bitstream and feeds a patched ffmpeg
// (`AV_CODEC_ID_XMAFRAMES`) that upstream does not have. Do not try to port its
// `Decode()` verbatim onto stock libavcodec.

struct XmaDecoder;

// Create a decoder for `channels` @ `sampleRate`. Returns nullptr on failure.
XmaDecoder* Xma_Create(int channels, int sampleRate);
void Xma_Destroy(XmaDecoder*);

// Feed one 2 KB XMA2 packet (or a run of them). Decoded interleaved float samples
// are appended to `out`. Returns the number of float samples appended (channels
// interleaved), or -1 on a hard decode error.
int Xma_DecodePacket(XmaDecoder*, const uint8_t* packet, size_t size,
                     std::vector<float>& out);

// Is there an XMA2 decoder in the libavcodec we linked against at all? Asked once
// at startup so "there is no codec on this machine" is a loud line rather than a
// silent per-context failure that looks exactly like the silence we are fixing.
bool Xma_CodecAvailable();

// Diagnostic: decode up to 16 packets of `data` and report what libavcodec made of
// it (samples, real channels/rate, RMS, peak). Returns true if anything decoded.
// This is the positive control for the decoder itself — a wrong sample_rate or
// is_stereo bit produces noise, not silence, and only a number distinguishes them.
bool Xma_Validate(const uint8_t* data, size_t size, int channels, int sampleRate);
