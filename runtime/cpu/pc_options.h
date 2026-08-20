// The resurrected PC graphics options screen (part 60). See pc_options.cpp.
#pragma once

#include <cstdint>

struct PPCContext;

// Called from the sub_827F6D40 hook in debug_tunables.cpp BEFORE the real transition
// runs. May rewrite ctx.r4 (the requested screen hash) — the Visuals->OptionsPC
// redirect — and tracks whether the PC options screen is the one being opened.
void PcOptions_FilterScreenTransition(PPCContext& ctx, uint8_t* base);
