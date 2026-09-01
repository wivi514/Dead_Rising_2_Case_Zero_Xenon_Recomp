# Native keyboard/mouse — the DR2-PC-exact plan (part 91, operator commission)

**The operator's instruction:** *"prepare a plan to implement it exactly like dead
rising 2 PC install and implement the icons."* They installed DR2 PC (Steam,
`~/.local/share/Steam/steamapps/common/Dead Rising 2/`, appid 45740, runs under
Proton) specifically so the official port is the reference. This plan is built from
tonight's censuses of BOTH games, and every claim below about what ships where was
measured, not assumed.

## §0 The evidence base (2026-09-01, all verified tonight)

**How DR2 PC does it** (`data/controls/{keymap,mousemap,padmap}.txt` — read):
* One COMMAND-LAYER binding system: `COMMAND_X(source, mode, source2, mode2, AND|OR)`,
  sources = `KEY_*`, `BUTTON_1..4` (mouse), `MOUSE_RAW_X/Y`, `MOUSE_WHEEL_*`,
  `*_THUMBSTICK_*`; modes = PRESSED/RELEASED/HELD/ACCELREPEAT/NONE.
* **Movement = `COMMAND_KBOARD_EMULATE_LTHUMB_{UP,DOWN,LEFT,RIGHT}` on
  KEY_W/S/A/D HELD** — the official port emulates the left stick from keys, in-engine.
  The operator reports it CRISP (no A/S/D delay), so whatever the emulation feeds is
  not subject to the delay our host-side stick emulation exhibits.
* **Camera = `MOUSE_RAW_X/Y` bound to `COMMAND_USER_CAM_{LEFTRIGHT,UPDOWN}` and
  `COMMAND_USER_CAM_WEAPONAIM_*`** — raw deltas into the SAME commands the pad's
  right stick feeds (their padmap binds RIGHT_THUMBSTICK_X to the same command,
  IDENTICAL line to Case Zero's own padmap). The operator reports it "way faster"
  than any stick mapping — raw look has no full-deflection turn-rate ceiling.
* Mouse buttons: BUTTON_1 = fire/quick attack (PRESSED) and charge (HELD);
  BUTTON_2 = heavy attack / aim-alternate (HELD); BUTTON_3 = cam reset;
  wheel = item cycling and zoom. Menus: ENTER/ESC + WASD-as-arrows;
  minigame A/B/X/Y on S/D/A/W.
* **Prompt icons are plain KEY-CAP CHIPS** — a dark rounded square with the white
  key legend (`S` Save Game, `ENTER` SELECT, `ESC` BACK; operator screenshots
  `userdata/172072341/760/remote/45740/screenshots/2026090102*.jpg`). There is no
  elaborate art to import: **our own original chip drawing reproduces the style with
  zero Capcom content, which retires the copyright question entirely** (asked and
  answered: we do NOT bundle Capcom assets; we do not need to).
* Their PC Settings screen = the shell Case Zero ships (options_pc.txt) with rows we
  already resurrected host-side, plus `Controller Enable/Disable` and
  `Mouse Sensitivity` as a 1..n pip slider. Config lands in
  `Documents/My Games/Dead Rising 2/rendersettings.ini` (plain key=value).

**What Case Zero's 360 XEX ALREADY CARRIES** (image censuses):
* The command parser + registry (padmap.txt IS loaded — `data/preload4.big`,
  dev path string `xdata/datafile/tofix/padmap.txt`), with bindings line-identical
  to DR2 PC's padmap for the commands that matter
  (`COMMAND_USER_CAM_LEFTRIGHT( RIGHT_THUMBSTICK_X, ...)`).
* **74 `KEY_*` tokens, `BUTTON_1..4`, and ALL FOUR `COMMAND_KBOARD_EMULATE_LTHUMB_*`
  names.** The PC input vocabulary largely SHIPS in the 360 build.
* The title imports `XamInputGetKeystrokeEx` (the 360's USB-keyboard/chatpad API);
  our HLE currently stubs it. `DlgKeyboard` ships (the on-screen keyboard).
* **Absent**: `MOUSE_RAW_*` and `MOUSE_WHEEL_*` tokens, the DirectInput reader, and
  (presumed, to be verified in A.1) the keymap.txt/mousemap.txt loaders. Also
  `Stick_Sensitivity`/`Stick_SensitivityHermite_0/1` ship — the stick response
  curve that is the prime suspect for the A/S/D delay our stick emulation hits and
  the PC's command-layer emulation seems to bypass.
* Zero KB/M prompt icon assets in the package (12,481 entries censused).

**What our runtime already has** (shipped tonight as v1, commit e8fe508):
keyboard+mouse merged into pad 1 with the drift guard, mouse-as-right-stick with the
sensitivity ladder, the panel rows (MOUSE CAMERA / MOUSE SENS), persistence. This v1
STAYS as the fallback arm for the whole plan (`CZ_NO_NATIVE_KBM=1` once the native
path exists). Precedent infrastructure the plan reuses: the VFS asset overlay + a
generator (`tools/gen_pc_options.py` for options_pc.txt), guest verb hooking with the
title's own H33 name hash (`cpu/pc_options.cpp`), guest camera-value patching
(`cpu/camera_fov.cpp`), the guest disassembler (`tools/gdis.py`).

## §1 PHASE A — recon in the XEX (one session; every later phase consumes it)

* **A.1 The binding parser and registry.** From the `padmap.txt` path string and the
  KEY_/BUTTON_ token table: where lines parse, where per-command bindings live, and
  whether the parser accepts KEY_ tokens from padmap.txt on the 360 build (single
  shared parser is the bet — the token table is right there). Also: do
  keymap.txt/mousemap.txt loader calls survive anywhere?
* **A.2 The key-state source.** What the 360 build reads for KEY_ state:
  `XamInputGetKeystrokeEx`-fed event queue into an engine key array, or a state API.
  Deliverable: the exact seam our SDL keyboard must feed.
* **A.3 The command VALUE store and per-frame evaluation** — where
  `COMMAND_USER_CAM_LEFTRIGHT`'s float lands each frame from the thumbstick, i.e.
  the injection seam for raw mouse deltas. Also the KBOARD_EMULATE evaluation (what
  it writes, and whether it bypasses the Hermite stick curve — the A/S/D crispness
  question; the operator's pending pad-flick test feeds this too).
* **A.4 The prompt-glyph mechanism.** How "Press A" resolves to art on 360 (font
  glyph in a .bcf vs texture atlas), and the command→glyph mapping table. Compare
  against DR2 PC's exe/assets for how the key-chip legend is drawn (chip + text, or
  per-key glyphs). (Recon note: DR2 PC's .big entries use a compression variant
  `tools/big_decompress` does not parse yet — fix in passing; their
  keymap/mousemap/padmap are loose files, already read.)

## §2 PHASE B — keyboard through the title's own layer

* **B.1** Serve an augmented `padmap.txt` via the VFS overlay (new
  `tools/gen_kbm_map.py`, gen_pc_options.py's pattern): add KEY_ alternates
  mirroring DR2 PC's keymap.txt DEFAULTS exactly (Enter/Esc menus, WASD menu
  arrows, minigame S/D/A/W, `KBOARD_EMULATE_LTHUMB_*` on WASD, dismiss on Q, etc.).
  If A.1 finds a live keymap.txt loader instead, serve keymap.txt verbatim-style.
* **B.2** Implement the key-state source A.2 named — most likely a real
  `XamInputGetKeystrokeEx` (SDL scancode → 360 VK + make/break queue). Hidden
  benefit: `DlgKeyboard` (save-name entry) starts working with a real keyboard.
* **B.3** Gate: with the native path live, retire the host kb→button merge behind
  `CZ_NO_NATIVE_KBM=1` (the v1 arm); keyboard focus/capture rules unchanged.
* **B.4 Acceptance = the operator's complaint**: A/S/D taps crisp like DR2 PC.

## §3 PHASE C — the mouse exactly like the PC port

* **C.1** Inject raw deltas at the A.3 seam into `COMMAND_USER_CAM_{LEFTRIGHT,UPDOWN}`
  (+ the WEAPONAIM pair when aiming) — additive with the stick like the PC's
  two-source binding, scaled by the panel's MOUSE SENS (pip-slider semantics).
  The stick-path mouse (v1) turns off when this engages.
* **C.2** Mouse buttons per DR2 mousemap: BUTTON_1/2 exist in CZ's vocabulary — bind
  fire/quick-attack/charge and heavy/aim exactly as their lines read; wheel (token
  absent on 360) maps host-side onto the item-cycle commands A.3 locates.
* **C.3 Acceptance**: camera parity by the operator's hand against DR2 PC on the
  same mouse and pad — "way faster" closed.

## §4 PHASE D — the icons (our own art, the official port's style)

* **D.1** Draw ORIGINAL key-cap chip art: dark rounded chip, white legend, sized to
  the 360 glyph metrics A.4 reports. Ours from scratch — the style is generic; no
  Capcom bytes.
* **D.2** Using A.4's mapping: swap prompts to key chips when KB/M is active —
  preferred form per A.4 (font-glyph replacement via a VFS-served patched .bcf/.tex,
  or runtime texture substitution keyed the way the renderer already keys textures).
  Legend text must FOLLOW the live binding from B.1's map, not hardcode (DR2 shows
  `S` on save because keymap says so).
* **D.3** Device-follow polish: swap icon set by last-used device if A.4 makes it
  cheap; otherwise the KB/M toggle decides, like the PC port's Controller
  Enable/Disable row (which we add for parity).

## §5 Rules, arms and gates (the standing ones apply)

Every phase ships with its off-arm and the v1 layer remains the fallback throughout;
`--smoke` per build; no gate run on synthetic input; picture gates untouched by input
work; guest hooks follow pc_options.cpp's discipline (H33 checked against the title's
own interned hashes before any hook fires; capture-then-verify before any guest call).
The operator is the acceptance instrument for feel (crispness, camera speed) — that
is a SHAPE question, theirs by standing rule. Sequencing: A is one focused session;
B and C can land in either order after it (C is the bigger felt win); D depends only
on A.4 and can parallel B/C.

## §6 Owed inputs

* The operator's pad-flick test (our build: flick the pad stick vs tap A) — splits
  "our values" from "the title's stick curve" for the A/S/D delay; A.3 answers it
  from the code side regardless, so the plan does not block on it.
* Nothing else — DR2 PC stays installed as the living reference.
