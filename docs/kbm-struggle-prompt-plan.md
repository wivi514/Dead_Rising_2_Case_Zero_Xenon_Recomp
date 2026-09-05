# KB/M struggle ("push back the zombie") prompt — fix plan

**Status: EXECUTED 2026-09-05, same day. Phase 1 answered CASE 1 by reading the layout —
no runtime probe was needed — and the fix shipped as the two-line LEGENDS addition (plus
one same-length string edit). Owed: the operator's grab capture confirming the flash.**

## The phase-1 answer (recorded here so nobody re-derives it)

The struggle prompt is `cFEWidget w_zombie_grapple` in `hud_infobar.txt` (ingame.big).
Its stick icon is a **3-frame `cFEBitmapList`** — `analog_move_left`, `analog_move_center`,
`analog_move_right` — i.e. **the game swaps whole glyph NAMES itself** (case 1, the
hoped-for one). The operator's 10-frame burst shows the center frame steady while NOT
mashing, consistent with the frame following the stick's X (which WASD drives full-scale);
whichever mechanism picks the frame, the alternation lands on those two slot names.
The "LS" label is `IDS_HUD_LS` = the 3-char string `"LS "` at 0x19434 in str_en.bcs,
referenced by NO other layout (censused over every `.txt` in ingame/fecmn/mainmenu/
loading/ratinglogos `.big`), so it was safe to retarget.

## The fix (tools/gen_kbm_icons.py, regenerated assets/game_kbm/)

1. `LEGENDS += analog_move_left=("key","A"), analog_move_right=("key","D")` — the two tilt
   frames become A and D key caps; mashing flashes A↔D via the game's own frame selection
   (the operator's mashing burst confirmed the frame follows the stick's X).
   The center frame FIRST stayed the WASD cluster; the operator's session found it flashing
   WASD between every press. The deeper census retired the sharing worry — the ONLY string
   using `[@analog_move_center]` is the grapple tutorial itself, and the only other layout
   consumer is hud_bossbattle's twist-QTE center; nothing teaches movement with it — so the
   center is now an "A/D" chip, and the grapple tutorial's "LEFT STICK " (unique in the
   bank) reads "A / D KEYS " via a fourth same-length edit.
2. Same-length str edit `\0LS \0` → `\0A/D\0` alongside the PRESS ENTER pair, so the big
   label reads "A/D" instead of "LS". (Same accepted caveat as PRESS ENTER: the string
   cannot device-follow; the glyph art does, via glyph_swap.bin, now 25 entries — the
   runtime parses the count from the file, no code change.)

All three generator gates passed; the bank stays under the 501,900-byte layout pin.
Pad players are unaffected: glyph_swap.bin carries both art sets and device-follow
restores the stick art when a pad is active; `CZ_NO_NATIVE_KBM=1` still disables all of it.

---

*The original plan follows, kept for the recon reasoning.* The operator found
and captured this during the part-96 session but asked to fix it separately.

## The bug (operator's words + capture)

When a zombie GRABS the player, DR2 shows a QTE prompt: mash to push the zombie off. On a
controller it's the LEFT STICK wiggled left↔right. Under our keyboard/mouse mode (parts
91-92, which map the stick to WASD) the prompt correctly switches away from the stick, but
it shows a **static** key hint — an "LS" label with an "A ⟷ D" cluster — instead of the
intended behaviour: **the icon should RAPIDLY ALTERNATE between showing "A" and showing "D"**,
animating the flash so the player reads "press A and D as fast as you can."

Capture (F8 burst during a real grab, zombies-hostile session):
`~/DR2CZ-troubleshooting/kbm-struggle-prompt/burst01_*.png` — 10 consecutive frames. The
camera pans during the grab, so the prompt drifts frame-to-frame; it sits near Chuck's chest,
center-left. Read across the frames: the hint does not animate A↔D.

Reproduce: launch zombies-hostile with god mode so you survive the grab —
`SAFE=1 FLAGS="CHUCK GOD MODE,DISABLE DEATH SEQUENCE" tools/play_session.sh` — walk into a
zombie until grabbed, F8 while grabbed.

## What is already known (the architecture — do NOT re-derive)

- **KB/M input** merges into pad 0 and maps the left stick to WASD (parts 91-92,
  `runtime/cpu/native_kbm.cpp`, `docs/native-kbm-phaseA.md`, `docs/native-kbm-plan.md`).
- **KB/M prompt icons are done by REPLACING the game's own glyph textures**, not by drawing
  an overlay. `tools/gen_kbm_icons.py` rebuilds `data/frontend/fecmn.tex` (25 pad glyphs, one
  bank, 28-byte entry records, DXT5 64x64 slots) with our own key-cap art, into the overlay
  `assets/game_kbm/data/frontend/fecmn.tex`. The game renders a glyph BY NAME; whichever
  texels we baked into that name's slot are what appear. Legend table is `LEGENDS` in
  `gen_kbm_icons.py`. `CZ_NO_NATIVE_KBM=1` turns the whole KB/M path (input + icons) off.
- The struggle prompt's controller glyph is one of the **analog** glyphs. The recon
  (`native-kbm-phaseA.md` A.4/§157) names `analog_move_center` (currently legended to a WASD
  cluster) **and a leftover `pc_analog`** in the same bank — the struggle prompt is one of
  these two; confirm which in phase 1.
- **The operator has DR2-PC installed** (Steam 45740, Proton — part 91). DR2-PC shows this
  exact prompt CORRECTLY under KB/M (A/D alternating). It is the living reference for the
  intended look and timing; capture it there first.

## The one real unknown → PHASE 1 RECON (do this before any code)

**How does the game ANIMATE the struggle prompt, and can the animation be expressed as
"glyph A one instant, glyph D the next"?** Three possibilities, and the fix differs per case:

1. **The game SWAPS between two glyph frames** (e.g. a "stick-left" and a "stick-right"
   glyph, alternated by its HUD timer). → Easiest: bake "A" into one slot and "D" into the
   other and the alternation is free. This is the hoped-for case.
2. **The game draws ONE analog glyph and animates a stick-position dot / rotation on top**
   (the glyph is static; the motion is separate geometry). → Patching the texture cannot make
   it flash A/D; we must either (a) detect the struggle state and drive our own A/D flash, or
   (b) accept a single combined "A⟷D" cap (what it does now) as the honest limit.
3. **A filling meter / single static prompt.** → Same as (2).

Nail it by:
- **Trace which glyph name(s) the game draws during a grab** and at what cadence. The frontend
  draws glyphs by name through the same frontend bitmap path phaseA A.4 documents; add a probe
  (or reuse `CZ_GUEST_DIAG`/`CZ_KBM_TRACE`) that logs the glyph name + frame for the grab
  window. If two names alternate on a timer → case 1. If one name is steady → case 2/3.
- **Read the controller version by eye** (our own game with a pad, or the F8 burst you already
  have shows the KB/M side) and **DR2-PC's KB/M prompt** to see the intended A/D flash rate.
- Cross-check with `gdis.py` on the HUD/QTE code if the trace is ambiguous — find the guest
  function that selects the struggle glyph (grep the padmap/`LEFT_THUMBSTICK_X` consumers,
  phaseA §28-29).

## Fix approach (branch on the phase-1 result)

- **If case 1 (glyph swap):** add the two struggle glyphs to `LEGENDS` in `gen_kbm_icons.py`
  — one `("key","A")`, one `("key","D")` — rebuild the overlay `fecmn.tex`, done. This is the
  clean, in-vocabulary fix and matches how the rest of the KB/M icons work. Verify the swap
  cadence looks like a mash prompt (it's the game's own timer, so it should).
- **If case 2/3 (single animated glyph):** the texture path can't flash. Options, cheapest
  first: (a) bake a single cap that reads as "A⟷D" or "A / D" clearly (a static but correct
  hint — arguably acceptable and low-risk); (b) drive an A/D flash from our runtime by
  detecting the grab/struggle state (the same LEFT_THUMBSTICK_X-consumer the input side
  already hooks) and swapping the glyph slot's texels on a timer, or drawing a small host
  overlay near the prompt. (b) is the faithful-to-DR2-PC result but is more work; scope it
  against the operator's bar (they want the *flash*, so (b) is likely what they want if case 2).

## Verification

- KB/M mode, zombies hostile + god mode (recipe above), get grabbed, F8 burst. Read the frames:
  the icon alternates A↔D at a mash cadence. Compare side-by-side to a DR2-PC grab capture.
- Controller mode still shows the stick prompt (KB/M path off with `CZ_NO_NATIVE_KBM=1`, and
  when a pad is the active device) — do not regress the controller prompt.
- Gate the icon rebuild through the existing `gen_kbm_icons.py` gates (it checks the size pin
  and the bank layout); the overlay `.tex` must stay the pinned 501,900 bytes.

## References
- `runtime/cpu/native_kbm.cpp`, `runtime/cpu/native_kbm.h`, `runtime/cpu/kbm_default_map.h`
- `tools/gen_kbm_icons.py` (icons), `tools/gen_kbm_map.py` (bindings)
- `docs/native-kbm-plan.md` (the commissioned plan), `docs/native-kbm-phaseA.md` (glyph +
  input recon — A.4 is the glyph bank, §28-29 the stick sources)
- Capture: `~/DR2CZ-troubleshooting/kbm-struggle-prompt/`
- Living reference: DR2-PC (Steam 45740) under Proton, its KB/M grab prompt
