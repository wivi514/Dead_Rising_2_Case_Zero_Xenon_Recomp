// The resurrected PC graphics options screen (part 60). See pc_options.cpp.
#pragma once

#include <cstdint>

struct PPCContext;

// Called from the sub_827F6D40 hook in debug_tunables.cpp BEFORE the real transition
// runs. Returns true when the transition must be SWALLOWED (the hook then returns 0
// without running the real body): selecting Visuals opens the host-rendered
// settings panel instead of any guest screen. CZ_PCOPT_NATIVE=1 restores the
// part-60 native-screen experiment (redirect to the shipped OptionsPC shell).
bool PcOptions_FilterScreenTransition(PPCContext& ctx, uint8_t* base);

// Called from the XamInputGetState pump (a guest thread with a usable context,
// like every DebugTunables_Pump*). Once the transition into the PC screen has
// settled it lights the first row and rotates every spin to the persisted
// settings; after that it IS the screen's input handling — selection, value
// spinning and back-navigation driven from the host, because the screen's own
// class (input included) was compiled out of the 360 build. `buttons` is the
// pad word the guest just received from this very poll.
void PcOptions_Pump(PPCContext& ctx, uint8_t* base, uint32_t buttons);
