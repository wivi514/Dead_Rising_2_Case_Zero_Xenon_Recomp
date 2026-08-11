#pragma once
#include <cstddef>
#include <cstdint>

// Host audio output for the guest's XAudio render driver.
//
// The 360's audio engine mixes every voice — SFX, dialogue, decoded XMA music —
// into ONE planar 5.1 float frame and hands it to XAudioSubmitRenderDriverFrame
// every ~5.333 ms. That frame is the entirety of what the game wants you to hear,
// so this module is deliberately dumb: downmix it to stereo and queue it. Anything
// that is wrong with the SOUND is upstream of here, and the peak of the frame
// arriving (`CZ_AUDIO_TRACE`) is what tells the two apart.
//
// PROVENANCE: ~/GithubRepo/Fable2XenonRecomp/runtime/audio/audio_out.cpp, this
// workspace's own earlier port. Its music mix-ring (`Audio_PushMusic`) is dropped:
// that existed to inject Bink movie audio around the guest mixer, and this title
// has no Bink (finding 7).

// Submit one render-driver frame: a planar big-endian float32 buffer of 6 planes x
// 256 samples (samples[channel * 256 + sample]), already translated to a host
// pointer. Safe to call with no device — it becomes a no-op.
void Audio_Out_SubmitFrame(const void* hostSamples);

// Stereo sample-frames still queued on the output device, or -1 when there is no
// device (CZ_NO_AUDIO_OUT, or the open failed). This is what lets the render
// callback be paced by DEMAND rather than by a clock: any fixed-interval pump has
// a rate error, and at 48 kHz a 2% deficit drains the device queue on a cycle and
// stutters continuously (Fable 2 measured 184/s against the 187.5 that 256-sample
// frames at 48 kHz require).
int Audio_Out_QueuedFrames();

// Has an output device actually been opened? Reported once at startup so "there is
// no sound because there is no device" is a line in the log rather than an
// inference from silence.
bool Audio_Out_Available();
