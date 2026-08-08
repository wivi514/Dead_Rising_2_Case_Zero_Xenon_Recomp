// The audio kernel surface: the XAudio render-driver client and the XMA decoder's
// context array. See audio.cpp for what the guest told us about both.
#pragma once

#include <cstdint>

// Allocates the XMA context array and publishes its physical address into the XMA
// decoder's register aperture. Must run before any guest code, because the guest
// reads that register once during its own audio init and caches the answer.
void Audio_Init();

// Where the context array landed, and which slots XMACreateContext has handed out.
// Exposed for the XMA state probe in cpu/guest_probe.cpp: the guest's own "is this
// voice still playing" predicate reads bits out of these 64-byte blocks, so the
// probe has to be able to print them beside the answer the guest computed from them.
// `count` is the number of slots, not the number in use; `inUse` says which.
uint32_t Audio_XmaContextArray();
unsigned Audio_XmaContextCount();
unsigned Audio_XmaContextSize();
bool Audio_XmaContextInUse(unsigned index);
