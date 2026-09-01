# Part 92 kickoff — the native KB/M build is the commissioned work; the release waits behind it

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
The picture-complaint bisection order stands: `CZ_VK_NO_DEFERRED_CLEAR=1` →
`CZ_VK_DEFER_FULL_RECT=1` → `CZ_VK_NO_PAR_RECORD=1`.

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
