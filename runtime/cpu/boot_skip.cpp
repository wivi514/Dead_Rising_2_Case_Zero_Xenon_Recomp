// The boot-logo skip (part 99, operator request): a launcher toggle that takes the
// Capcom / Blue Castle / Dolby logo sequence out of the boot.
//
// WHERE THE LOGOS ACTUALLY LIVE — established by frame dumps, not by reading the
// frontend data. They are NOT the LegalScreen/BCGIntro top-level states the plan's
// §0 recon guessed (BCGIntro is never even requested on a boot; the measured state
// chain is Startup -> LegalScreen -> Loading -> FrontEnd, with LegalScreen lasting
// 7 ms). The visible sequence — black legal card, then CAPCOM, BLUE CASTLE, DOLBY,
// the DR2 title card — is DATA: fecmn.big's `intro.txt` layout, an event-chained
// cFEAnim timeline (rating -> "capcom" -> "bcg" -> "dolby" -> "DR2Logo" ->
// "animation_done", ~18 s of UseRealTime keyframes). The black legal card
// (`startup.txt`) is load-driven, not timed, and is left alone.
//
// THE MECHANISM is therefore a DATA PATCH, not a hook: overlay_gen.cpp generates
// assets/game_bootskip/data/frontend/fecmn.big — the part-60 patched archive with
// every intro.txt keyframe Time collapsed to a per-anim 1,2,3... — and vfs.cpp
// serves it over the patched one when this toggle is on. Every anim still plays
// and every event still fires, in order, through the title's own machinery; each
// logo is a one-tick flash instead of 4 s. That is the plan §2.2 (b) "shorten,
// don't bypass" shape, delivered without touching guest code.
//
// TWO REFUTED MECHANISMS, so nobody rebuilds them (both were §2.2 (a) shapes —
// substituting the requested top-level state in sub_827E68F8's r4):
//   * LegalScreen -> FrontEnd: guest null-deref within a second — FrontEnd probes
//     `game:\pressstart.txt` (a fallback path no stock boot ever takes) and
//     crashes; the Loading state's FE preload never ran.
//   * LegalScreen -> Loading, and LegalScreen -> BCGIntro: both hang — the
//     skipped state's enter is what kicks the work the next state waits on
//     (files park at #44/startup.tex forever, worker threads in wait-any).
// The states are chained through their side effects; no state is skippable.
// (`skip_startup`, the title's own shipped tunable at 0x82A57BF0, was also tried
// via CZ_DEBUG_TUNABLES and does not shorten the visible sequence.)
//
// What this file still owns: the enabled check (shared with vfs.cpp) and the
// CZ_STATE_TRACE instrument on the state-request function, which is what
// diagnosed the above and costs nothing when the env is unset (a handful of
// calls per boot, none per-frame).
//
// Default OFF: the logos are legal notices, so the skip ships opt-in
// (`skip_intro_logos` in the settings file / the launcher row). CZ_SKIP_INTRO=1|0
// is the dev arm and wins over the setting.

#include "boot_skip.h"

#include "ppc_recomp_shared.h"

#include "../host/settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_827E68F8);

bool BootSkip_Enabled()
{
    static const bool on = [] {
        if (const char* e = getenv("CZ_SKIP_INTRO"))
            return e[0] != '0';
        return Settings_SkipIntroLogos();
    }();
    return on;
}

namespace
{

uint32_t ReadG32(uint8_t* base, uint32_t va)
{
    uint32_t b;
    memcpy(&b, base + va, 4);
    return __builtin_bswap32(b);
}

// sub_829A21C0 interns the ten top-level state names into this table at boot
// (+4 Startup, +8 LegalScreen, +0xC BCGIntro, +0x10 FrontEnd, +0x14 FEToGame,
// +0x18 Loading, ...). The plan doc's 0x82A6912C is a typo; the image says
// `lis 0x82A6; addi -0x6ED4`.
constexpr uint32_t kStateTable = 0x82A5912C;

const char* StateName(uint8_t* base, uint32_t id)
{
    static const struct { uint32_t off; const char* name; } kSlots[] = {
        { 0x04, "Startup" }, { 0x08, "LegalScreen" }, { 0x0C, "BCGIntro" },
        { 0x10, "FrontEnd" }, { 0x14, "FEToGame" }, { 0x18, "Loading" },
        { 0x1C, "InGame" }, { 0x20, "InGameTut1" }, { 0x24, "FEToGameShow" },
        { 0x28, "GameShow" },
    };
    for (const auto& s : kSlots)
        if (id && ReadG32(base, kStateTable + s.off) == id)
            return s.name;
    return "?";
}

} // namespace

// sub_827E68F8 — the top-level state REQUEST (stores the interned name id at
// flowObj+0x24, then dispatches). CZ_STATE_TRACE=1 names every transition; the
// pass-through is otherwise bit-identical.
PPC_FUNC(sub_827E68F8)
{
    if (getenv("CZ_STATE_TRACE"))
        fprintf(stderr, "[state] request %s (id=%08X)\n",
                StateName(base, ctx.r4.u32), ctx.r4.u32);
    __imp__sub_827E68F8(ctx, base);
}
