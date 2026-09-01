# Native KB/M — Phase A recon record (part 92)

Executes `docs/native-kbm-plan.md` §1. Everything below was read out of
`assets/game/default_image.bin` (the loaded 360 XEX image) tonight with
`tools/gdis.py`, pointer scans, and the shipped `padmap.txt`
(datafile.big, decompressed) — none of it is inferred from DR2 PC's binary. Where
a claim matters to the implementation it carries the address it was read at, so a
later session can re-verify in seconds. **The single most important sentence:
the 360 build ships the ENTIRE PC input layer except three small pieces — the
keyboard-device CONNECT branch, a keystroke SOURCE (`XamInputGetKeystrokeEx`
returns empty on a console with no chatpad), and the mouse — and every seam
needed to supply those three from the host is identified below.**

## A.1 The binding parser and registry — SHIPS, single, shared

* **The source-token table** is three parallel 95-entry arrays in `.data`:
  names at `0x829F3930`, values at `0x829F3AB0`, categories at `0x829F3C30`.
  Entry 0 is the `NONE` sentinel (value 0xFF, cat 3); **the entry INDEX is the
  engine's source index** (proof: the keystroke handler hardwires sources
  0x34..0x39 for the shift/ctrl/alt pairs, and KEY_LSHIFT is entry 52 = 0x34).
  Categories: **0 = keyboard key (value = Windows VK code)**, 1 = digital pad
  bit (value = XINPUT button mask; DPAD_UP..LEFT are 1/8/2/4), 2 = analog axis
  (LT_X/Y = 0/1, RT_X/Y = 2/3, L1/R1 = the triggers as axes 4/5), 3 = NONE.
  Notable source indices (0-based, entry 0 = NONE):
  `KEY_A..KEY_Z` 1..26, `KEY_1..KEY_0` 27..36, numpad 37..46,
  arrows LEFT/UP/RIGHT/DOWN 47..50, SPACE 51, LSHIFT/RSHIFT 52/53,
  LCTRL/RCTRL 54/55, LALT/RALT 56/57, ESCAPE 58, ENTER 59, COMMA/PERIOD 60/61,
  TAB 62, DPAD_UP/RIGHT/DOWN/LEFT 63..66, **LEFT_THUMBSTICK_X/Y 67/68**,
  LT_DIR/MAG 69/70, **RIGHT_THUMBSTICK_X/Y 71/72**, RT_DIR/MAG 73/74,
  **BUTTON_1..4 75..78**, BACK/START 79/80, L1/L2/L3 81/82/83, R1/R2/R3
  84/85/86, thumb-halves 87..94. The 360 table has **no MOUSE_RAW_*,
  MOUSE_WHEEL_*, F-keys, EQUALS/MINUS**, and the escape token is `KEY_ESCAPE`
  (DR2 PC's keymap writes `KEY_ESC` — a rename our generator must apply).
* **The mode/combiner vocabulary** is one string array at `0x829EF8CC`, but
  ~~one shared enum (NONE=0, AND=1, NOT=2, OR=3, HELD=4, ...)~~ — **RETRACTED
  the same night**: the parser's two lookups start at DIFFERENT offsets into
  that array, so there are TWO enums. Modes (`sub_828039F0`, table at
  `0x829EF8DC`): **HELD=0, PRESSED=1, RELEASED=2, REPEAT=3, ACCELREPEAT=4,
  TAP1=5, TAP2=6, QUICKTIMEDRELEASE=7, NONE=8**. Combiners (`sub_82803A68`,
  table at `0x829EF8CC`): **NONE=0, AND=1, NOT=2, OR=3**. Verified against the
  pad's own parsed records (FRONTEND_A_BUTTON reads mode1=1/PRESSED,
  mode2=8/NONE, comb=0/NONE). Parsing is case-insensitive (the shipped padmap
  mixes `HELD` and `held`).
* **The command registry** is a flat 305-entry name table at `0x829DC810`,
  **command ID = table index**. IDs that matter:
  `COMMAND_KBOARD_EMULATE_LTHUMB_UP/DOWN/LEFT/RIGHT` = **113..116**,
  `COMMAND_USER_CAM_LEFTRIGHT/UPDOWN` = **216/217**,
  `COMMAND_USER_CAM_WEAPONAIM_LEFTRIGHT/UPDOWN` = **218/219**.
  Accessors: id→name at `0x8247ADC0` (and a parallel table at `0x829DC348`
  via `0x8247ADA8`).
* **The parser is ONE function and it takes RAW TEXT**:
  `sub_82803AE0(portRecord, textPtr, alloc)` — tokenizes on `(`/`,`,
  skips space/tab/CR/LF. `sub_82804248(port, text)` is the per-port wrapper:
  record = `0x82AD6CF8 + port*0x10`, alloc = the global at `0x82AD6658`.
  **There is no keymap.txt / mousemap.txt loader in the image** (no strings, no
  path references) — pads parse `xdata/datafile/tofix/padmap.txt`
  (string at `0x820B9DF7`; the actual file resolves to datafile.big /
  preload4.big's `padmap.txt`, 19,473 bytes decompressed, and it is
  **line-format-identical to DR2 PC's padmap** — same 5-argument
  `COMMAND(src, mode, src2, mode2, comb)` form, same mode words).
* **The per-port binding store** (`0x82AD6CF8`, 16 bytes per port, 16 ports):
  +0x4 = binding array (**one 24-byte record per command ID**), +0x8 = count,
  +0xC = the alloc passed in. Binding record: **+0 bound flag (byte), +4 src1
  index, +8 mode1, +0xC src2 index, +0x10 mode2, +0x14 combiner** — the enum
  above, `OR`(3) meaning "fall through to src2" in the float path.

## A.2 The key-state source and the keyboard controller — SHIPS, minus the connect

* **The keyboard controller class exists and is INSTANTIATED**: vtable
  `0x820B9D88`, ctor `0x82806980`, object size 0x2488, device class 0
  (`this+0x23A8`). The factory at `0x82806EA0` creates **12 pad objects**
  (size 0x2898, ctor `0x82806CF0`, class 2) into slots at `0x82AD6E18` and
  **4 keyboard objects** into slots at **`0x82AD6DF8`** (4 × {u8 used; u32
  object}, immediately before the pad slots).
* **Its Update virtual (+0x0C = `0x82806A08`) is the keystroke pump**: calls
  the title's own wrapper `sub_825D7B90(port, 2 /*XINPUT_FLAG_KEYBOARD*/,
  &keystroke)` → **`XamInputGetKeystrokeEx`** (import thunk `0x829C26E4`;
  r3 is a POINTER to the user index; user 0xFF ⇒ `flags |= 0x40000000` any-user).
  The keystroke struct is exactly our `GuestInputKeystroke` (VK u16, unicode
  u16, flags u16, user u8, hid u8). Flags read by the handler: **bit0 keydown,
  bit1 keyup, 0x8 shift, 0x10 ctrl, 0x20 alt** (the modifier bits update the
  six modifier sources directly; the VK is looked up in the token table's
  cat-0 entries and the match's INDEX is the source written).
* **Sources**: per-controller array of 95 × 48-byte records at
  `this+0x11D8`; **value float at +8**; edge/held state maintained inside the
  record (+0x18 held, +0x1B/+0x1C edges, +0x20 press count) by the setter.
  Setter: **`sub_828049D8(this, srcIdx, value, value)`** (tail:
  `sub_82803E10(&rec, f1, f2)`); getter: vtable[+0x04] `0x828049E8(this, idx,
  &out)`. The ctor enables every cat-0 source (`0x828069C8` writes
  `this+0x241C+i` for each key entry).
* **THE ONE MISSING PIECE — the connect.** The periodic port scan
  `sub_82806F60` (callers `0x824916AC`, `0x824A23CC`) walks ports 0..3 with
  `XamInputGetCapabilities(port, 0, &caps)`:
  a connected device with **caps.Type==1** and no bound controller claims a PAD
  slot, binds it (`sub_82803460(port, obj)` → port map `0x82AD65E8`), resets it
  (`sub_82806338`), and parses padmap for that port. A device with
  **caps.Type==2 (keyboard) and no bound controller FALLS THROUGH** — the
  keyboard connect branch was compiled out. The keep-alive half survives: a
  bound class-0 controller whose port reports caps.Type==2 is kept connected
  (`sub_828037F0(obj, 1)`, the connected flag at `this+0x23D4`). So a
  host-side connect (claim slot → set `+0x2414` port, clear `+0x23F0` →
  `sub_82806338` → `sub_82803460` → parse) is **self-sustaining afterwards**,
  provided our `XamInputGetCapabilities` HLE reports Type=2 on that port for
  non-GAMEPAD-flag queries.
* **The input manager tracks the keyboard SEPARATELY**: its port rescan
  (`0x827F6750`) stores the class-3/4 controller as primary (+0x148 obj,
  +0x144 port) and **the class-0 controller at +0x154 — a dedicated keyboard
  slot**. This is the structural basis for pad+keyboard coexistence (how far
  consumers read +0x154 vs the primary is an open question for the first live
  boot; either answer — simultaneous merge or device-follow — is DR2-PC-shaped).
* Ports: `kLocalPadCount=2` in our HLE means ports 0/1 already present as
  gamepads. **The keyboard takes engine port 2** — no collision, and the scan's
  keep-alive works there.

## A.3 Command evaluation and the injection seams

* **Commands are evaluated ON DEMAND, not stored per frame.** Float query
  `0x82805510(cmdId, port)`, bool query `0x828053C8(cmdId, port)`. Both:
  check global enable `0x82AD6649`, port alive (`sub_82804290`), id < count
  `0x82AD6650`, the command's CONTEXT mask (indirect fn `0x82AD6654` vs mask
  `0x82AD664C` — the activation lists elsewhere), then read the port's binding
  record and evaluate against the port's controller sources:
  float = source value **RAW** (src1, falling to src2 on 0.0 when comb==OR);
  bool = `sub_82802F70(ctrl, src1, mode1, src2, mode2, comb)` (edge/held
  evaluation over the source records' state, `sub_82802E20` per source).
* **Consequence 1 — the crispness answer (plan §1 A.3):** the stick response
  curve (`Stick_SensitivityHermite`) lives in the PAD class's own
  state→source conversion; the keyboard class HAS no such conversion (its
  vtable slot is the empty filler). A source written directly on the keyboard
  controller reaches the command value **unfiltered**. WASD → full-scale
  LT_X/LT_Y sources = instant, curve-free movement — the DR2 PC crispness by
  construction.
* **Consequence 2 — the mouse seam (plan §3 C.1):** DR2 PC binds
  `MOUSE_RAW_X/Y` → `COMMAND_USER_CAM_*`. The 360 table lacks those tokens,
  but binding USER_CAM to `RIGHT_THUMBSTICK_X/Y` **on the keyboard port** and
  writing raw mouse deltas into keyboard sources 71/72 each frame is the same
  wiring: value flows source→command with no curve and no ±1 clamp. Mouse
  buttons: sources 75/76/77 (`BUTTON_1/2/3`) written host-side — DR2 PC's
  mousemap uses exactly those tokens for fire/heavy/cam-reset. Wheel: no
  token on 360; DR2's own mousemap pairs every wheel binding with `KEY_1`/
  `KEY_3` alternates, so the host synthesizes those keystrokes for wheel steps.
* **KBOARD_EMULATE (113..116)**: the commands and their names ship, but no
  consumer site was positively identified in the image (the candidate scan
  produced only resource-load noise). Rather than depend on it, the host feeds
  WASD → LT_X/LT_Y sources itself (identical semantics, and provably
  curve-free). The map file therefore does NOT bind KBOARD_EMULATE.

## A.4 The prompt-glyph mechanism (for Phase D)

* Prompt strings in `str_en.bcs` carry inline markup — `[@x_button_ig]`,
  `[@butstart]`, `[@LTbutton_ig]`, `[@dpad_down]`, `[@a_button_ig]`, … —
  and the token names a **frontend bitmap**: the same names appear as
  `File="…"` on `cFEBitmap` widgets in the frontend layouts
  (fecmn.big/ingame.big), with the art in the frontend `.tex` banks.
  Census over str_en.bcs: 27×`x_button_ig`, 20×`RTbutton_ig`, 7×`LTbutton_ig`,
  7×`b_button_ig`, 6×`y_button_ig`, 4×`a_button_ig`, 4×`L3`, 3×`butback`,
  1×`butstart`, dpads, `analog_move_center` — and one leftover `pc_analog`.
  So Phase D = supply OUR key-cap chip art under those names (VFS-served
  patched `.tex`, the gen_pc_options precedent), or a runtime texture swap for
  device-follow. Detail deferred to D; the mechanism is settled.

## What Phase B/C build on this (implemented in `runtime/cpu/native_kbm.cpp`)

1. **Verify before touching anything** (pc_options discipline): the token
   name table reads `KEY_A…`, the four keyboard slots at `0x82AD6DF8` hold
   objects whose vtable is `0x820B9D88`. Any mismatch ⇒ decline loudly, v1
   stays.
2. **Connect** (one-shot, on the guest thread inside the XamInputGetState
   seam): claim keyboard slot 0, `obj+0x2414=2`, `obj+0x23F0=0`,
   `sub_82806338(obj)`, `sub_82803460(2, obj)`, then parse OUR map:
   copy the DR2-PC-format text into guest memory and call
   `sub_82804248(2, text)`. Caps HLE reports Type=2 for port 2 on
   non-GAMEPAD queries ⇒ the title's own scan keeps it connected.
3. **Keystrokes**: real `XamInputGetKeystrokeEx` backed by an SDL-fed queue
   (scancode→VK for the 62 table keys, keydown/keyup/repeat + modifier flags).
4. **Per-frame feed** on the keyboard controller: WASD→sources 67/68,
   mouse deltas×sens→71/72, mouse buttons→75/76/77, wheel→synthetic
   KEY_1/KEY_3 keystrokes.
5. **The map file**: `tools/gen_kbm_map.py` emits the DR2-PC defaults
   translated into CZ vocabulary (ESC rename, F-keys dropped, MOUSE tokens
   substituted per A.3), validated at generation time against the image's own
   command and token tables.
6. `CZ_NO_NATIVE_KBM=1` = whole-feature arm → v1 exactly as shipped.

## ~~Open questions, deliberately left to the first live run~~ — ANSWERED THE
## SAME NIGHT (phases B+C executed; this section is now the record)

The plan above was BUILT, worked end to end, and was then REPLACED by a better
design its own evidence pointed to. Keep both halves: the port-2 findings are
what make the port-0 design trustworthy.

* **The port-2 keyboard controller chain WORKS**: verify → claim slot → bind →
  the title's parser resolved all 133 generated bindings for port 2, the
  context pass set their active flags, and a synthetic ENTER
  (`CZ_KBM_TEST_KEYS`) fired `COMMAND_FRONTEND_A_BUTTON` into the title's own
  ENGAGEMENT scan (`sub_827F85F8`, reached from the manager update at
  `0x828016B0`) — which then **crashed on a null** in profile machinery
  (`[0x82AD5EF8]`-rooted) that the 360 build never expected to run for a
  class-0 controller. The keyboard support is compiled out one level FURTHER UP
  than the connect. Do not resurrect the port-2 connect without fixing that.
* **Engagement is COMMAND-driven and scans every port** — the query histogram
  (`CZ_KBM_TRACE=1` hooks on `0x828053C8`/`0x82805510`) shows bool/float
  command queries against all 16 ports continuously. Port 0's engagement path
  is exercised by every pad press of every boot — the fully-supported road.
* **The shipped design (runtime/cpu/native_kbm.cpp) therefore rides PORT 0**:
  key bindings SPLICED directly into port 0's live binding records (the layout
  is proven — see the corrections below), key sources fed into the pad-0
  controller object, stick sources overridden AFTER the pad's own conversion
  (a strong hook on `sub_828070E0`), mouse buttons riding the pad's
  face-button/trigger sources (left→BUTTON_3/X, right→BUTTON_R2, middle→
  BUTTON_4/Y — DR2 PC's mousemap semantics through the pad's own two-source
  lines, which is why the mousemap lines need no splice at all). Headless
  proof: ENTER taps advance the title into the main menu (save file +
  casefiles.tex open) where the control arm parks at file #84.

Three parser/record findings from the build-out, each paid for in a failed run:

* **A delimiter must be followed by WHITESPACE.** The parser advances past a
  delimiter by skipping characters UNTIL whitespace, so `CMD(KEY_TAB, ...`
  swallows `KEY_TAB` whole. The shipped padmap always has a tab after `(`;
  a generated map without it loses EVERY line ("305 commands, 0 parsed" with
  the pad port's own parse as the oracle proving the call was fine).
* **Binding-record byte +0 is CONTEXT-MAINTAINED, not a parse product** — the
  pad port's own census of it oscillated 115↔248 across one boot. Gate a parse
  on src1 (+4) being non-zero, never on the flag.
* The mode-enum retraction recorded in §A.1 above.

Still genuinely open (operator session):

* Feel acceptance — A/S/D crispness (the override bypasses the deadzone
  rescale entirely) and camera speed parity with DR2 PC (raw deltas are
  unclamped; the MOUSE SENS row scales them).
* Whether the title ever re-parses padmap mid-session (the splice re-applies
  via its sentinel if so — the log line says when).
* `DlgKeyboard` typing through the now-real `XamInputGetKeystrokeEx`.
