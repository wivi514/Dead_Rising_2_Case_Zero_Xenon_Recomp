// The audio kernel surface: the XAudio render-driver client and the XMA decoder's
// context array. See audio.cpp for what the guest told us about both.
#pragma once

// Allocates the XMA context array and publishes its physical address into the XMA
// decoder's register aperture. Must run before any guest code, because the guest
// reads that register once during its own audio init and caches the answer.
void Audio_Init();
