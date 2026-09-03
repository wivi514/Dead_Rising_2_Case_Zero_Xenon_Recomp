# Part 92 kickoff — the native KB/M build is the commissioned work; the release waits behind it

## 0-live. PART 92 PROGRESS (updated in place as the part runs)

**Phases A, B and C of `docs/native-kbm-plan.md` are BUILT and headlessly
proven; `docs/native-kbm-phaseA.md` is the complete record** (the recon, the
executed build-out, its three paid-for parser findings, and one retraction in
place). The shape that shipped is NOT the plan's phase-B shape — the plan's
port-2 keyboard-controller connect was built, worked to the point of
engagement, and crashed in the title's dormant profile machinery; the shipped
design splices the DR2-PC key defaults into PORT 0's live binding records and
feeds the pad-0 controller's sources directly (key events through the title's
own setter; WASD overriding the stick post-conversion, curve-free; mouse
deltas ADDITIVE and UNCLAMPED on the right-stick sources; mouse buttons on the
pad's own face-button sources, which is why DR2's mousemap lines need no
splice at all). 86 of 93 key bindings land; `CZ_NO_NATIVE_KBM=1` restores v1
wholly; the headless proof is ENTER taps (`CZ_KBM_TEST_KEYS`) advancing the
title into the main menu where the control arm parks at file #84.

**Phase D shipped the same night**: all 25 pad glyphs live in fecmn.tex
(nested in preload4.big — evicted onto the loose-file road part 60 proved,
served from a new `assets/game_kbm` overlay only while the native keyboard is
on); `tools/gen_kbm_icons.py` draws our own key-cap chips, compresses them
with the real LZX encoder (the guest crashes on degenerate streams — part
60), pads to layout.bin's 501,900-byte pin, and gates everything (identity
repack, hash identity, round-trip). The game-identical decode of every chip
was rendered and looked at; the boot serves and decodes the bank clean.

**THE WHOLE PLAN IS EXECUTED AND OPERATOR-ACCEPTED (2026-09-02)** — their
sitting drove five fix rounds, each conviction from their own reports/logs:
the publish race (post-conversion source writes fought the title's converter;
single-writer via the XInput merge + effective-cell camera surplus), the icon
crop (the widget samples the used-extent region only), the splice racing the
title's own padmap parse (stable-parse gate + whole-set intact check), THE
A/S/D SECOND (SDL text-input mode routed held keys through the desktop IME —
the accent-picker popup was the tell; SDL_StopTextInput), my trace histogram's
snprintf stack smash (which had manufactured the "engagement crash" — retracted
in phaseA), the Visuals panel driven by keyboard (panel-only button mirror;
then the stuck-ENTER release), PRESS ENTER at the title, and DEVICE-FOLLOW
PROMPT ART (both art sets shipped; texel-identified in guest memory —
page-aligned, headerless; instant flips, rate-limited rescans after the scan
storm dropped them below 30 fps once). Their verdict: input "works perfectly",
performance back to normal, Visuals menu working.

~~Owed: the operator's session~~ — delivered; B.4/C.3 acceptance is met (their
words across the sitting: movement/aim/camera "work perfectly", chips sighted
in-game). Still open, non-blocking: legend refinements as they meet more
prompts, and the title-screen string not device-following (boot-time choice).
Arms: `CZ_NO_NATIVE_KBM=1` = v1 wholly; `CZ_NO_KB_PROMPTS=1` = pad art with
the keyboard live; the picture-complaint bisection order gains
`CZ_NO_PATCHED_ASSETS=1` for anything frontend.

**THIS IS THE LIVE HAND-OFF**, superseding `part91-kickoff.md` (kept as part 91's
record: the live-resolution apply, KB/M v1, and the §0b/§0c/§0d performance-parking
and visual-report closures).

## 0. What part 91 delivered (records: `phase5-notes.md` §6em, §6en; `docs/native-kbm-plan.md`)

* **Live internal-resolution apply from the Visuals panel — operator-verified**
  ("It is perfect tried multiple resolution and it worked"). Stepping the row is
  PENDING (starred value, footer hint), **X saves + applies live** at the frame
  boundary; B/reopen discards. The part-60 live path's freeze was its MID-FRAME
  apply placement (a wait-idle cannot cover recorded-but-unsubmitted references —
  the RetiredImage shape); relocated to BeginFrame's non-recording entry. Gated
  headless both directions (`CZ_VK_LIVE_RES_TEST=<frame>:<w>x<h>`) and by the
  operator through the real menu. §6em.
* **Keyboard/mouse v1 (e8fe508)**: kb+mouse MERGED INTO PAD 0 (keyboard-as-pad-2 was
  structurally dead — the title binds the player and the panel pump to pad 0), the
  drifting-pad guard, mouse→right-stick camera with the panel's MOUSE CAMERA /
  MOUSE SENS rows. Working, with two honest gaps the operator felt: A/S/D tap
  delay (the title's own stick curve is the suspect) and the camera's
  full-deflection turn-rate ceiling. §6en.
* **The input censuses that reshape everything**: Case Zero's XEX ships most of the
  PC input vocabulary (74 KEY_ tokens, BUTTON_1..4, the KBOARD_EMULATE_LTHUMB
  commands, the USER_CAM consumers, a padmap line-identical to DR2 PC's); DR2 PC
  (installed by the operator as the living reference, Steam 45740 under Proton at
  `~/.local/share/Steam/steamapps/common/Dead Rising 2/`) does movement by the SAME
  stick-emulation idea, camera by RAW deltas into the SAME commands, and prompts as
  generic key-cap chips — **no Capcom assets are needed or shipped for any of it**.

## 1. THE BOARD

0. **EXECUTE `docs/native-kbm-plan.md`** — the operator's commission ("implement it
   exactly like dead rising 2 PC install and implement the icons"), to be started
   fresh. Phase A (recon: binding parser, key-state seam, command-value seam,
   prompt-glyph mechanism) is one focused session and everything else consumes it;
   then B (native keyboard), C (raw mouse-look), D (our-own-art key-cap icons).
   Acceptance = the operator's two feel complaints + prompts showing keys.
   Owed input, non-blocking: their pad-flick test (§6en).
1. **Then the release board** (`part91-kickoff.md` §1's release section, deferred by
   the operator until the fixes are done): Windows leg rebuild (the shipped bundle
   predates parts 87-91 ENTIRELY now — the KB/M work is new SDL-side code that must
   build on czwin), bundle save verification, repackage + re-gate both artifacts,
   glibc floor / AppImage, first-boot pre-warm screen polish.
2. Standing checks when convenient: natural level-up, combo bench vs phantom cards.

## 2. Parked subjects (unchanged)

Performance (part91-kickoff §0c/§0d — the resume list is §0d); RT shadows (part 70).
The picture-complaint bisection order, updated part 93 now that **2x MSAA is
default-on**: `CZ_VK_MSAA=0` FIRST (the single-sample pre-part-93 renderer bit for
bit), then `CZ_VK_NO_DEFERRED_CLEAR=1` → `CZ_VK_DEFER_FULL_RECT=1` →
`CZ_VK_NO_PAR_RECORD=1`.

**Part 93 (2026-09-03): MSAA built, tested, and DEFAULTED to 2x.** `CZ_VK_MSAA=N`
(true multisampled EDRAM, `docs/msaa-plan.md` §9) works and reaches the screen
(proven by a sharpness A/B + a zoomed still), but it does NOT fix the part-92 hair
flicker — that is a translucency-ordering instability, not an edge-aliasing one, so
the faithful hair fix is OIT/card-sorting (`docs/hair-flicker-part92.md`). MSAA is
kept as a working game-wide AA feature; the operator set 2x as the default (cost
+0.85 ms GPU at 1440p, frame time unmoved on the dev machine). `CZ_VK_MSAA=0` is the
control arm. Owed: an AA-vs-cost read on the OPERATOR's machine, and the
ship-in-release-bundle decision.

## 3. Gates inherited (unchanged)

`--smoke` per build; PM4 oracles after any pm4.cpp change; `truncated=0`; sync
validation for barrier/clear changes; every new default ships with its off-arm; no
gate run on synthetic input; guest hooks follow pc_options.cpp's discipline (H33
verified against the title's interned hashes; capture-then-verify before guest
calls). The operator is the acceptance instrument for input FEEL — a shape question,
theirs by standing rule.

## 4. For Case West (standing send-back)

The input findings transfer whole: the command-layer vocabulary almost certainly
ships in CW's XEX too (same engine, same era) — census it before building anything
host-side, and go straight for the native path there. The key-cap-chip icon style is
generic; whatever D ships here lifts directly.
