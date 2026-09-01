#!/usr/bin/env python3
"""Generate the native keyboard/mouse binding map (part 92, native-kbm-plan phase B).

WHY THIS EXISTS. The 360 XEX ships the whole PC command-binding layer — the parser
(sub_82803AE0 takes raw text in exactly DR2 PC's 5-argument line format), the
95-entry source-token table, the mode enum, and 305 commands — but no keyboard
bindings: the shipped padmap.txt is pad-only and there is no keymap.txt loader in
the image (docs/native-kbm-phaseA.md). The runtime connects the title's own
keyboard controller class on engine port 2 and parses THIS map for that port with
the title's own parser (runtime/cpu/native_kbm.cpp).

The bindings are the DR2 PC defaults (the operator's commission: "exactly like
dead rising 2 PC") transcribed and translated into Case Zero's vocabulary:

  * KEY_ESC -> KEY_ESCAPE (the 360 table's spelling).
  * F-key lines dropped (the 360 token table has no F keys): the two
    OLD_PAUSEMENU bindings and COMMAND_PC_F2.
  * KEY_EQUALS/KEY_MINUS (map zoom) -> KEY_PERIOD/KEY_COMMA (nearest tokens
    that exist).
  * MOUSE_RAW_X/Y -> RIGHT_THUMBSTICK_X/Y. The token does not exist on 360;
    the runtime writes raw mouse deltas into the keyboard controller's RT
    sources each frame, which reaches the same commands the same way (the
    on-demand evaluator reads the source value raw — phaseA A.3).
  * MOUSE_WHEEL_UP/DOWN -> KEY_3/KEY_1. DR2 PC's own mousemap already pairs
    every wheel binding with KEY_1/KEY_3 alternates; the runtime synthesizes
    those keystrokes for wheel steps, so one binding serves both.
  * COMMAND_KBOARD_EMULATE_* lines are NOT emitted: the runtime feeds
    WASD -> LEFT_THUMBSTICK sources directly (no engine consumer for those
    commands was found in the image, and the direct feed is provably
    curve-free — phaseA A.3).

EVERY line is validated against the image's OWN tables before it is emitted —
command names against the 305-entry table at 0x829DC810, source tokens against
the 95-entry table at 0x829F3930, modes against the enum at 0x829EF8CC. A line
that fails validation is a hard error, not a skip: a silently dropped binding is
a key that does nothing in a session three weeks from now.

Output: runtime/cpu/kbm_default_map.h (a C string literal, committed — the
bindings are our own functional data, no Capcom bytes). The runtime also accepts
a player-editable override next to the executable (kbmap.txt, same format).

Usage:
    python3 tools/gen_kbm_map.py            # validate + write the header
    python3 tools/gen_kbm_map.py --print    # show the map text
"""

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IMAGE = REPO / "assets/game/default_image.bin"
OUT = REPO / "runtime/cpu/kbm_default_map.h"

BASE = 0x82000000
CMD_TABLE = 0x829DC810
CMD_COUNT = 305
TOK_NAMES = 0x829F3930
TOK_COUNT = 95
MODE_NAMES = 0x829EF8CC
MODE_COUNT = 12  # none and not or held pressed released repeat accelrepeat tap1 tap2 quicktimedrelease

# The map. Each entry: (command, src1, mode1, src2, mode2, comb).
# Transcribed from DR2 PC's data/controls/keymap.txt + mousemap.txt with the
# substitutions documented in the module docstring. Order follows the source
# files so a diff against them stays readable.
BINDINGS = [
    # --- menus / frontend (keymap.txt) ---
    ("COMMAND_FRONTEND_PAUSEMENU",  "KEY_TAB",      "PRESSED", "KEY_LCONTROL", "HELD",    "AND"),
    ("COMMAND_PAUSEMENU",           "KEY_TAB",      "PRESSED", "KEY_LCONTROL", "HELD",    "AND"),
    ("COMMAND_PAUSEMENU_UP",        "KEY_W",        "PRESSED", "KEY_UP",       "PRESSED", "OR"),
    ("COMMAND_PAUSEMENU_DOWN",      "KEY_S",        "PRESSED", "KEY_DOWN",     "PRESSED", "OR"),
    ("COMMAND_PAUSEMENU_LEFT",      "KEY_A",        "PRESSED", "KEY_LEFT",     "PRESSED", "OR"),
    ("COMMAND_PAUSEMENU_RIGHT",     "KEY_D",        "PRESSED", "KEY_RIGHT",    "PRESSED", "OR"),
    ("COMMAND_PAUSEMENU_SELECT",    "KEY_ENTER",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PAUSEMENU_BACK",      "KEY_ESCAPE",   "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PAUSEMENU_FAST",      "KEY_LSHIFT",   "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PAUSEMENU_SLOW",      "KEY_LCONTROL", "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_UP",         "KEY_UP",       "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_DOWN",       "KEY_DOWN",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_LEFT",       "KEY_LEFT",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_RIGHT",      "KEY_RIGHT",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_A_BUTTON",   "KEY_ENTER",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_A_BUTTON_RELEASE", "KEY_ENTER", "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_B_BUTTON",   "KEY_ESCAPE",   "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_X_BUTTON",   "KEY_X",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_Y_BUTTON",   "KEY_C",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_L1_BUTTON",  "KEY_1",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_R1_BUTTON",  "KEY_2",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_L2_BUTTON",  "KEY_3",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_R2_BUTTON",  "KEY_4",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_START_BUTTON", "KEY_ENTER",  "PRESSED", "KEY_ESCAPE", "PRESSED", "OR"),
    ("COMMAND_PLAYER_DISMISS_DIALOG", "KEY_Q",      "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_RIGHT_HELD", "KEY_C",        "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_HIGHROLLERS_POKER_QUIT",  "KEY_Q",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_HIGHROLLERS_POKER_TIPS",  "KEY_H",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_HIGHROLLERS_POKER_PAUSE", "KEY_ESCAPE", "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_MAP_L2",     "KEY_4",        "ACCELREPEAT", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_MAP_R2",     "KEY_3",        "ACCELREPEAT", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_MAP_L1",     "KEY_PERIOD",   "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_MAP_R1",     "KEY_COMMA",    "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_GENACTION1", "KEY_Z",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_GENACTION2", "KEY_SPACE",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_GENACTION3", "KEY_M",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MINIGAME_OK",  "KEY_SPACE",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MINIGAME_EXIT", "KEY_E",       "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MINIGAME_A",   "KEY_S",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MINIGAME_B",   "KEY_D",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MINIGAME_X",   "KEY_A",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MINIGAME_Y",   "KEY_W",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_VIEW_MAP",         "KEY_TAB",      "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_SKIP_SCRIPT",         "KEY_ENTER",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_SKIP_CINEMATIC",      "KEY_ESCAPE",   "PRESSED", "KEY_SPACE", "PRESSED", "OR"),
    ("COMMAND_AI_PAUSE_GAME",       "KEY_ESCAPE",   "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_INTERACT_WITH_PHONE", "KEY_C",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_INTERACT_WITH_WATCH", "KEY_T",     "PRESSED", "NONE", "NONE", "NONE"),
    # --- movement / camera (analog sources the runtime feeds host-side) ---
    ("COMMAND_PLAYER_X",            "LEFT_THUMBSTICK_X",   "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_Y",            "LEFT_THUMBSTICK_Y",   "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_RUN_DIR",      "LEFT_THUMBSTICK_DIR", "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_RUN_MAG",      "LEFT_THUMBSTICK_MAG", "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_USER_CAM_LEFTRIGHT",  "RIGHT_THUMBSTICK_X",  "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_USER_CAM_UPDOWN",     "RIGHT_THUMBSTICK_Y",  "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_USER_CAM_WEAPONAIM_LEFTRIGHT", "RIGHT_THUMBSTICK_X", "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_USER_CAM_WEAPONAIM_UPDOWN",    "RIGHT_THUMBSTICK_Y", "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_USER_CAM_RESET",      "BUTTON_3",     "PRESSED", "NONE", "NONE", "NONE"),
    # --- player actions (keymap.txt) ---
    ("COMMAND_PLAYER_JUMP",         "KEY_SPACE",    "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_CROUCH",       "KEY_C",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_DODGE",        "KEY_LCONTROL", "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_BUTTON_PRESS", "KEY_E",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_OBJECT_PICKUP", "KEY_E",       "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_USE",          "KEY_E",        "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_ITEMS_DROP",   "KEY_DOWN",     "PRESSED", "KEY_X", "PRESSED", "OR"),
    ("COMMAND_PLAYER_ITEMS_HIDE",   "KEY_UP",       "PRESSED", "KEY_2", "PRESSED", "OR"),
    ("COMMAND_PLAYER_CALLOUT",      "KEY_Q",        "PRESSED", "NONE", "NONE", "NONE"),
    # --- vehicles (keymap.txt) ---
    ("COMMAND_AI_VEHICLE_ENTER_EXIT", "KEY_E",      "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_VEHICLE_STEERING_X", "LEFT_THUMBSTICK_X", "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_VEHICLE_STEERING_Y", "LEFT_THUMBSTICK_Y", "NONE", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_VEHICLE_ACCELERATE", "KEY_W",      "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_AI_VEHICLE_ACCELERATE_TRIGGER", "KEY_W", "HELD", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_VEHICLE_BRAKE",    "KEY_S",        "HELD",    "KEY_SPACE", "PRESSED", "OR"),
    ("COMMAND_AI_VEHICLE_BRAKE_TRIGGER", "KEY_S",   "HELD",    "KEY_SPACE", "PRESSED", "OR"),
    # --- mouse (mousemap.txt; BUTTON_1/2/3 sources are fed from the real mouse,
    #     wheel arrives as synthetic KEY_1/KEY_3 keystrokes) ---
    ("COMMAND_FRONTEND_ML_BUTTON",  "BUTTON_1",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_RML_BUTTON", "BUTTON_1",     "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_MR_BUTTON",  "BUTTON_2",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_MW_UP",      "KEY_3",        "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_FRONTEND_MW_DOWN",    "KEY_1",        "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MAIN_ITEMS_CYCLE_LEFT",       "KEY_1", "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_MAIN_ITEMS_CYCLE_RIGHT",      "KEY_3", "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_SECONDARY_ITEMS_CYCLE_LEFT",  "KEY_1", "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_SECONDARY_ITEMS_CYCLE_RIGHT", "KEY_3", "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_ZOOM_IN",      "KEY_3",        "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_ZOOM_OUT",     "KEY_1",        "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_FIRE_WEAPON",  "BUTTON_1",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_RAPID_FIRE",   "BUTTON_1",     "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_QUICK_ATTACK", "BUTTON_1",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HEAVY_ATTACK", "BUTTON_2",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_CHARGE",       "BUTTON_1",     "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_CHARGEATTACK_SHORT", "BUTTON_1", "HELD",  "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_CHARGEATTACK_LONG",  "BUTTON_1", "HELD",  "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_RAPID_FIRE_RT", "BUTTON_1",    "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_RAPID_FIRE_LT", "BUTTON_2",    "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_TOGGLE_ALTERNATE_WEAPON_VIEW", "BUTTON_2", "HELD", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_THROW",        "BUTTON_2",     "HELD",    "BUTTON_1", "PRESSED", "AND"),
    ("COMMAND_PLAYER_THROW_RT",     "BUTTON_2",     "HELD",    "BUTTON_1", "PRESSED", "AND"),
    ("COMMAND_PLAYER_THROW_CHARGE", "BUTTON_2",     "HELD",    "BUTTON_1", "PRESSED", "AND"),
    ("COMMAND_PLAYER_PRIME",        "BUTTON_1",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_RELEASE_PRIMED", "BUTTON_1",   "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_QUICK_TIMED_RELEASE", "BUTTON_1", "QUICKTIMEDRELEASE", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_SPRAY",        "BUTTON_1",     "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_CALL_SURVIVOR_GOTO_POINT", "BUTTON_2", "HELD", "KEY_Q", "PRESSED", "AND"),
    # --- hand-to-hand (mousemap.txt) ---
    ("COMMAND_PLAYER_HAND_TO_HAND_SHIFT",          "KEY_LSHIFT", "HELD",     "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_PUNCH_HELD",     "KEY_Q",      "HELD",     "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_PUNCH_RELEASED", "KEY_Q",      "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_KICK",           "KEY_LSHIFT", "HELD",     "KEY_Q", "PRESSED", "AND"),
    ("COMMAND_PLAYER_HAND_TO_HAND_KICK_HELD",      "KEY_LSHIFT", "HELD",     "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_KICK_RELEASED",  "KEY_LSHIFT", "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_A_HELD",     "KEY_SPACE", "HELD",     "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_A_RELEASED", "KEY_SPACE", "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_A_PRESSED",  "KEY_SPACE", "PRESSED",  "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_B_HELD",     "KEY_E",     "HELD",     "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_B_RELEASED", "KEY_E",     "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_B_PRESSED",  "KEY_E",     "PRESSED",  "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_X_HELD",     "BUTTON_1",  "HELD",     "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_X_RELEASED", "BUTTON_1",  "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_X_PRESSED",  "BUTTON_1",  "PRESSED",  "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_Y_HELD",     "KEY_Q",     "HELD",     "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_Y_RELEASED", "KEY_Q",     "RELEASED", "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_HAND_TO_HAND_Y_PRESSED",  "KEY_Q",     "PRESSED",  "NONE", "NONE", "NONE"),
    # --- rideables / pushables (mousemap.txt) ---
    ("COMMAND_AI_RIDEABLE_ON",    "BUTTON_1",          "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_RIDEABLE_OFF",   "KEY_E",             "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_RIDEABLE_TURN",  "LEFT_THUMBSTICK_X", "NONE",    "NONE", "NONE", "NONE"),
    ("COMMAND_AI_RIDEABLE_BRAKE", "KEY_S",             "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_AI_RIDEABLE_PUMP",  "KEY_W",             "HELD",    "BUTTON_1", "HELD", "OR"),
    ("COMMAND_AI_RIDEABLE_JUMP",  "KEY_SPACE",         "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_AI_PUSHABLE_TURN",  "LEFT_THUMBSTICK_X", "NONE",    "NONE", "NONE", "NONE"),
    ("COMMAND_AI_PUSHABLE_ACCELERATE", "KEY_W",        "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_AI_PUSHABLE_REVERSE",    "KEY_S",        "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_AI_PUSHABLE_DUMP",       "BUTTON_1",     "PRESSED", "NONE", "NONE", "NONE"),
    ("COMMAND_AI_PUSHABLE_ALTERNATE_CHARGE", "BUTTON_1", "HELD",  "NONE", "NONE", "NONE"),
    ("COMMAND_AI_PUSHABLE_CHARGE",     "KEY_SPACE",    "HELD",    "NONE", "NONE", "NONE"),
    ("COMMAND_PLAYER_PUSH_HAMSTER_BALL", "BUTTON_1",   "PRESSED", "BUTTON_1", "HELD", "OR"),
]


def read_table(data, addr, count):
    names = set()
    for i in range(count):
        (p,) = struct.unpack_from(">I", data, addr - BASE + 4 * i)
        s = p - BASE
        e = s
        while e < len(data) and 32 <= data[e] < 127:
            e += 1
        names.add(data[s:e].decode())
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--print", action="store_true", dest="show")
    args = ap.parse_args()

    data = IMAGE.read_bytes()
    commands = read_table(data, CMD_TABLE, CMD_COUNT)
    tokens = read_table(data, TOK_NAMES, TOK_COUNT) | {"NONE"}
    modes = {m.upper() for m in read_table(data, MODE_NAMES, MODE_COUNT)}

    errors = []
    for cmd, s1, m1, s2, m2, comb in BINDINGS:
        if cmd not in commands:
            errors.append(f"unknown command {cmd}")
        for tok in (s1, s2):
            if tok not in tokens:
                errors.append(f"{cmd}: unknown source token {tok}")
        for mode in (m1, m2, comb):
            if mode.upper() not in modes:
                errors.append(f"{cmd}: unknown mode/comb {mode}")
    if errors:
        for e in errors:
            print("ERROR:", e, file=sys.stderr)
        sys.exit(1)

    # THE SPACE AFTER '(' IS LOAD-BEARING. The title's parser advances PAST each
    # delimiter by skipping characters UNTIL WHITESPACE (sub_82803AE0's
    # inter-token loops) — a delimiter glued to the next token swallows that
    # token whole, and the first build of this map lost every line to exactly
    # that (part 92: "305 commands, 0 parsed" with the pad's own map as the
    # oracle proving the call was fine).
    text_lines = []
    for cmd, s1, m1, s2, m2, comb in BINDINGS:
        text_lines.append(f"{cmd}( {s1}, {m1}, {s2}, {m2}, {comb})")
    text = "\n".join(text_lines) + "\n"

    if args.show:
        print(text)
        return

    with open(OUT, "w") as f:
        f.write("// The native keyboard/mouse default binding map (part 92).\n")
        f.write("// GENERATED by tools/gen_kbm_map.py, which validates every line against the\n")
        f.write("// image's own command/token/mode tables — regenerate there, do not edit here.\n")
        f.write("// The text is in the title's own padmap.txt format and is parsed at runtime\n")
        f.write("// by the title's own parser (sub_82804248) for the keyboard port.\n")
        f.write("#pragma once\n\n")
        f.write("static const char kKbmDefaultMap[] =\n")
        for ln in text_lines:
            f.write(f'    "{ln}\\n"\n')
        f.write("    ;\n")
    print(f"validated {len(BINDINGS)} bindings against the image; wrote {OUT}")


if __name__ == "__main__":
    main()
