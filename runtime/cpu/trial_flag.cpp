// enable_trial_experience (0x82A57BFE) follows the licence we answer, always.
//
// WHY THIS EXISTS (co-op part 6, 2026-09-12). The laptop's main menu grew an
// UNLOCK FULL GAME row and its co-op join sat in INVITESTATE_CHAR_LOADING for
// ever: the title was running its TRIAL EXPERIENCE. The flag is the tunable
// `enable_trial_experience` — the TitleScreen keeps the TrialUnlock row while it
// is set (sub_824D8A00), and the joiner's load never finishes under it. Three
// writers: the tunables loader (sub_824A2470, from the config), the game init
// (sub_82496D98) which then FORCES it to 1, and one consumer of the licence mask
// (0x82501924: `flag &= (mask == 0)`) that clears it again — but that consumer
// sits behind a state test the title makes about the signed-in user, and behind
// the release byte (with CZ_GUEST_DIAG=1 the flag read 1 on this box too), so
// whether a boot ends up licensed or trial depends on boot ORDER: the Linux host
// read 0 at the title, the laptop read 1 in the same evening's build.
//
// Our XamContentGetLicenseMask answers 1 (the package is the full game — the one
// place getting the value wrong boots a different game, imports.cpp). The title's
// own rule is "mask != 0 -> not trial", so this applies that rule where the init
// last touched the flag, unconditionally. CZ_TRIAL_EXPERIENCE=1 leaves the
// title's own value (the control; and the way to look at the trial UI).
#include <cstdio>
#include <cstdlib>

#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82496D98);

namespace
{
constexpr uint32_t kEnableTrialExperience = 0x82A57BFE;
}

PPC_FUNC(sub_82496D98)
{
    __imp__sub_82496D98(ctx, base);
    static const bool leave = getenv("CZ_TRIAL_EXPERIENCE") != nullptr;
    const uint8_t was = PPC_LOAD_U8(kEnableTrialExperience);
    if (leave)
    {
        fprintf(stderr, "[license] enable_trial_experience left at %u (CZ_TRIAL_EXPERIENCE)\n", was);
        return;
    }
    PPC_STORE_U8(kEnableTrialExperience, 0);
    fprintf(stderr, "[license] enable_trial_experience %u -> 0: the package is licensed "
                    "(CZ_TRIAL_EXPERIENCE=1 leaves the title's own value)\n", was);
}
