// The title's own debug menu, debug-jump screen and dev tunables, switched back on.
//
// WHY THIS EXISTS
// ---------------
// Testing this port means reaching a place in the game, and until now the only two
// ways to do that were an operator playing to it or CZ_FAKE_PRESS_SEQ manufacturing
// its way there over several minutes of fixed 8 s steps (CLAUDE.md gotcha 78 — that
// recipe MANUFACTURES progress, so it can never be a gate). Both are expensive and
// neither is repeatable at a chosen point.
//
// The shipped executable turns out to carry Blue Castle's complete development
// scaffolding. It was never compiled out — only switched off:
//
//   * `common\debugmenu\debugmenu.cpp` and its component header are still in the
//     image's source-path strings, `cDebugMenu` is still a class, and the menu's
//     item tree is intact (System Menu, Chartz Menu, Thread Edit Menu, Controller
//     Assignments, Performance Chartz, GPU Timing Queries, NPC To Spawn, Credits,
//     FontTest, and a header reading "DEBUG-ONLY TUNABLES").
//   * `DebugJump` is a frontend screen listed beside `TitleScreen` and `PressStart`,
//     and its layout `debugjump.txt` SHIPS inside data/frontend/mainmenu.big — at
//     4,144 bytes it is the largest entry in that archive, larger than title.txt.
//   * `God Mode:ON` is referenced by live code in the actor-status overlay.
//
// HOW THE SWITCH WORKS
// --------------------
// 393 boolean tunables live in one contiguous global struct in .data. A single
// loader, `sub_824A2470`, looks each one up BY NAME through sub_82773298 and stores
// the answer into a fixed byte:
//
//     lis   r9, 0x8207
//     addi  r4, r9, <name>        ; e.g. "enable_debug_jump_menu"
//     bl    0x82773298            ; get-bool-by-name
//     stb   r3, 0x7C09(r10)       ; r10 = 0x82A50000  ->  0x82A57C09
//
// and every consumer gates on it with a plain load-and-branch, e.g. at 0x824D6170:
//
//     lbz     r11, 0x7C09(r11)
//     cmplwi  r11, 0
//     beq     <skip>
//
// In a retail build the name lookup finds nothing and every byte comes back 0.
//
// WHY A ONE-SHOT POKE IS ENOUGH
// -----------------------------
// `sub_824A2470` has exactly one caller, and its call chain is three hops off the
// XEX entry point itself:
//
//     sub_825D9F30 (entry)  ->  sub_825D7448  ->  sub_82496D98  ->  sub_824A2470
//
// It runs once, before anything else, and nothing rewrites those bytes afterwards.
// So a post-hook that writes the flags as the loader returns is permanent, and costs
// one env-var read on a function that executes a single time per process. There is no
// per-frame component to this instrument at all, which is the standard gotcha 7 asks
// for — an instrument that cannot perturb what it reports.
//
// WHY A TABLE OF NAMES RATHER THAN RAW ADDRESSES
// ----------------------------------------------
// The addresses below were not read off a disassembly by hand. They were extracted
// mechanically by walking .text for the (addi name / stb offset) pairs above and
// resolving both against their `lis`, then each was CONFIRMED by scanning
// independently for the `lbz` consumers that read it back. The reader count is
// recorded next to every entry, because a tunable nothing reads is a switch that
// does nothing, and shipping one of those silently would be exactly the "a zero is a
// detection failure" trap from the other direction (gotcha 3). The four curated
// names that scanned to ZERO readers are listed at the bottom of the table and
// deliberately excluded — they are almost certainly read through a register base this
// scan does not model, but "almost certainly" is not evidence and this file does not
// offer switches it cannot show are connected.
//
// WHAT IT CANNOT DO
// -----------------
// Only Case Zero's own scenes ship. The image still carries the full Dead Rising 2
// scene list (`yucatan_casino`, `fortune_exterior`, `boss_battle_*`) because the two
// games share an engine, but data/models/environment holds only `prologue`,
// `prologue_menu`, `prologue_menu2`, `prologue_safehouse` and `safehouse`. A jump to
// anything else has no data behind it.
//
// FOR CASE WEST
// -------------
// The same struct will be there, at a different address. Re-derive it with the same
// two scans rather than reusing these constants: find the (addi/stb) pairs in the
// loader, then confirm each with its `lbz` readers.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../kernel/memory.h"
#include "ppc_recomp_shared.h"

// `ppc_recomp_shared.h` declares only the WEAK alias, never the real body, so the
// hook has to declare the one it wraps. `extern "C"` is not optional — the
// recompiler defines it via PPC_FUNC_IMPL, which is `extern "C" PPC_FUNC`, and a
// plain C++ declaration here would mangle differently and fail to link (gotcha 33).
extern "C" PPC_FUNC(__imp__sub_824A2470);

namespace
{

struct Tunable
{
    const char* name;
    uint32_t    address;   // guest address of the one-byte flag
    int         readers;   // consumers found by the independent lbz scan
    const char* note;
};

// Curated from the 393 extracted. Every entry here has at least one confirmed
// reader; `readers` is that count, from the scan described in the header comment.
const Tunable kTunables[] = {
    // --- the three that put the menus back on screen ---
    { "enable_debug_jump_menu",       0x82A57C09, 1,
      "the DebugJump level/mission screen in the main menu" },
    { "enable_quickie_debug_menu",    0x82A57C01, 1,
      "the in-game quick debug menu" },
    { "enable_one_button_debug_menu", 0x82A57BFF, 3,
      "open the debug menu with one button instead of a combo" },
    { "debug_on_controller_2_only",   0x82A57C00, 1,
      "route debug input to pad 2 so pad 1 still plays" },

    // --- progression and flow, the ones that make repeat testing cheap ---
    { "skip_startup",                 0x82A57BEF, 1,
      "skip the startup/logo sequence" },
    { "disable_mainmenu_scene",       0x82A57C07, 4,
      "skip the rendered main-menu backdrop" },
    { "notebook_show_all",            0x82A57BF3, 2,
      "every notebook/combo-card entry unlocked" },
    { "disable_level_up_message",     0x82A57BF4, 2,
      "suppress the level-up interstitial" },
    { "disable_casefiles_popup",      0x82A57BF2, 1,
      "suppress the case-file popup" },
    { "enable_button_through_timed_dialogs", 0x82A57C02, 1,
      "button past dialogs that normally hold for a timer" },
    { "enable_prolog_experience",     0x82A57BF9, 3,
      "force the prologue experience flow" },
    { "enable_trial_experience",      0x82A57BFD, 1,
      "force the TRIAL flow -- see CLAUDE.md finding 1 before setting this" },
    { "boss_use_debug_menu_jump",     0x82A57BF6, 1,
      "let the boss flow use the debug jump" },

    // --- overlays and diagnostics ---
    { "debug_show_loading_time",      0x82A57C08, 6,
      "on-screen load timings" },
    { "display_fe_screen_info",       0x82A57BF1, 1,
      "name the active frontend screen on screen" },
    { "zombie_show_debug_info",       0x82A57C15, 23,
      "per-zombie debug info -- 23 readers, expect a heavy overlay" },
    { "draw_damage_logs",             0x82A57C0A, 1,
      "damage event log" },
    { "ignore_boss_damage",           0x82A57C0D, 4,
      "bosses take no damage" },
    { "hide_changelist",              0x82A57BE3, 1,
      "hide the build changelist watermark" },
    { "enable_dev_only_debug_tiwwchnt", 0x82A57BFE, 26,
      "dev-only master switch, 26 readers -- broad, try it alone" },

    // Deliberately NOT offered, because the reader scan found zero consumers for
    // each and this file does not ship switches it cannot show are connected:
    //   show_actor_source_files   0x82A57BDE
    //   EnablePauseMenuCaseFiles  0x82A57BF5
    //   boss_use_debug_jump       0x82A57BF7
    //   make_game_go_faster       0x82A57C12
};

const Tunable* Find(const std::string& name)
{
    for (const Tunable& t : kTunables)
        if (name == t.name)
            return &t;
    return nullptr;
}

// The set CZ_DEBUG_MENU=1 turns on: enough to reach the menus and to see which
// frontend screen you are on, and nothing that changes gameplay behaviour.
const char* const kMenuPreset[] = {
    "enable_debug_jump_menu",
    "enable_quickie_debug_menu",
    "enable_one_button_debug_menu",
    "display_fe_screen_info",
};

void Apply(uint8_t* base, const Tunable& t, uint8_t value)
{
    // A guest byte needs no endian swap, which is the whole reason these flags are
    // bytes and not ints as far as this hook is concerned.
    const uint8_t before = PPC_LOAD_U8(t.address);
    PPC_STORE_U8(t.address, value);
    fprintf(stderr, "[debug] %-36s @%08X  %u -> %u   (%d reader%s) %s\n",
            t.name, t.address, before, value, t.readers,
            t.readers == 1 ? "" : "s", t.note);
}

void ApplyFromEnvironment(uint8_t* base)
{
    bool any = false;

    if (const char* menu = getenv("CZ_DEBUG_MENU"))
    {
        if (menu[0] && strcmp(menu, "0") != 0)
        {
            fprintf(stderr, "[debug] CZ_DEBUG_MENU=%s -- enabling the title's own "
                            "debug menu and DebugJump screen\n", menu);
            for (const char* name : kMenuPreset)
                if (const Tunable* t = Find(name))
                    Apply(base, *t, 1);
            any = true;
        }
    }

    // CZ_DEBUG_TUNABLES=name[=0|1],name[=0|1],...  -- anything in the table above.
    if (const char* list = getenv("CZ_DEBUG_TUNABLES"))
    {
        std::string spec(list);
        size_t pos = 0;
        while (pos <= spec.size())
        {
            const size_t comma = spec.find(',', pos);
            std::string item = spec.substr(pos, comma == std::string::npos
                                                    ? std::string::npos
                                                    : comma - pos);
            pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;

            if (item.empty())
                continue;

            uint8_t value = 1;
            const size_t eq = item.find('=');
            if (eq != std::string::npos)
            {
                value = static_cast<uint8_t>(atoi(item.c_str() + eq + 1));
                item.resize(eq);
            }

            if (const Tunable* t = Find(item))
            {
                Apply(base, *t, value);
                any = true;
            }
            else
            {
                // Fail loudly and list what IS available. A typo that silently did
                // nothing would be indistinguishable from a tunable that does
                // nothing, and this project does not ship that ambiguity (gotcha 5).
                fprintf(stderr, "[debug] CZ_DEBUG_TUNABLES: unknown name '%s'. "
                                "Known names:\n", item.c_str());
                for (const Tunable& k : kTunables)
                    fprintf(stderr, "[debug]     %-36s %s\n", k.name, k.note);
            }
        }
    }

    if (any)
        fprintf(stderr, "[debug] tunables applied at the entry-point config load; "
                        "nothing rewrites them after this point\n");
}

}  // namespace

// The post-hook. The loader must run FIRST — it writes every one of these bytes from
// the (empty) retail config, so a pre-hook would have its work overwritten
// immediately. This is the ordering the whole instrument depends on.
PPC_FUNC(sub_824A2470)
{
    __imp__sub_824A2470(ctx, base);
    ApplyFromEnvironment(base);
}
