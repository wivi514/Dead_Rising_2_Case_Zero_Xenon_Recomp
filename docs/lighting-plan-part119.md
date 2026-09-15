# Part 119 plan — interiors are too dark: the picture after the tone map

**The operator's report (2026-09-15, closing the v1.1.1 build):** *"Lighting is
different in Case Zero compared to the Case Zero on Xbox 360, interiors are really dark
compared to what it was on Xbox 360."* Written for a fresh conversation. Read this file,
then `CLAUDE.md`'s evidence rules; nothing below has been measured on our renderer yet
except §1's three facts, which were read from the captures while writing it.

## §0. The shape of the question, and why "the lighting" is probably the wrong subject

Nine parts of this port (26-33, 45) went into the SCENE — the ground plateau, the
laundered interpolants, the shadow atlas — and closed with the scene buffer matching
hardware where it was measured (open-items 0s/0t/0u, phase5 §6ba-§6bg). What has
NEVER been compared against hardware is what happens to a finished frame between the
tone map's output and the screen: the front buffer's FORMAT and the display controller's
GAMMA RAMP. Both exist in every capture, both are ignored by this renderer, and a
transfer-curve error is exactly what "interiors dark, exteriors fine" looks like — a
gamma-shaped error is largest in the low and middle values and vanishes at white.

So the order is: **the post-tone-map transfer first (§2, one afternoon, an oracle in
hand), the tone map's own constants second (§3), the scene last (§4)** — the reverse of
what the report's wording suggests, because the cheap hypothesis with an oracle comes
before the expensive one without.

## §1. What is already known (read from the captures on 2026-09-15)

1. **Hardware loads a NON-IDENTITY display gamma ramp, and it is the same table at an
   interior and an exterior.** `tools/xtr_gamma_ramp.py` on `w4_bathroom`,
   `Big_buck_hardware_store_03` and `w1_spawn`: the 256-entry 10:10:10 table sits BELOW
   identity everywhere but white — input 32/255 → 66/1023 (16.5/255), 64 → 193 (48),
   128 → 462 (115) — an exponent of ~1.15-1.3. The PWL table is the standard 128-entry
   ramp (`(0,512)` … `(65472,0)`). One `GammaRamp` record per capture, `rw=0`.
2. **This runtime has never read a DC_LUT register.** `grep -i "gamma\|DC_LUT"` over
   `pm4.cpp`, `xenos.h`, `vk_renderer.cpp`: nothing but a comment about the gamma
   slider. Whatever the ramp does on hardware, and whatever Xenia does with it at swap,
   ours does not.
3. **The copy-destination format is printed and never decoded.** `RB_COPY_DEST_INFO`
   (0x231B) reaches one `fprintf` in `DoResolve` (vk_renderer.cpp ~27238); its format
   field — where `k_8_8_8_8_GAMMA` (1) differs from `k_8_8_8_8` (0) by a piecewise-linear
   ENCODE on write — is not consulted. Neither is `RB_COLOR_INFO`'s format for the pass
   that the swap presents (the renderer's `rtFmt=0 on 600 of 600 passes` census in
   open-items 0s counted SCENE passes, not the final one).
4. Earlier facts that bound the space: the title's auto-exposure runs here and adapts
   across the range hardware's two point measurements occupy (phase5 §6bg: ours
   0.200-0.354 over an era, hardware 0.331 at `w1_spawn`, 0.298 at `w7_slotmachine`) —
   so the exposure is not stuck; the grading LUT is real, applied, and byte-identical
   to the disc's (open-items 0v/§6ax); the tone-map shader carries named constants
   `gFinalGammaParameters`, `gLumRangeLow_High_MinLumPlusRes`,
   `gResidualRangeLowMidHigh` (image strings at 0x820B408C..0x820B40C8) — the gamma the
   360 Visuals meter drove is a SHADER constant, and part 60 recorded "gamma genuinely
   does nothing in our runtime" without saying why.
5. **Eight hardware interiors are on disk, each with its frame and its screenshot:**
   `Xenia logs/R2_world/{w4_bathroom,w6_register_door,w7_slotmachine}` and
   `Xenia logs/R4_world/Big_buck_hardware_store_01..08` (`.xtr` + `.png`). The `.png`
   is Xenia's OUTPUT — after its gamma-ramp application — and the `.xtr` holds the
   register stream and every resolve's bytes. That is an oracle for every stage of §2.

## §2. Item 1 — the transfer curve after the tone map (do this first)

**Hypothesis A:** the final resolve writes a `k_8_8_8_8_GAMMA` front buffer (PWL
encode — a brightening curve on write, roughly sRGB-shaped) and the display ramp of §1.1
then darkens it partly back; we skip both, so our front buffer holds the tone map's
LINEAR output and the screen shows it as-is: dark mids and shadows, white untouched.
That prediction — the error is a smooth monotone curve of the pixel VALUE, the same
curve indoors and out — is falsifiable in one comparison.

Steps, in order; each answer changes the next:

1. **Name the front buffer's format from the capture.** Extend
   `tools/xtr_resolve_census.py` to print `RB_COPY_DEST_INFO`'s format field (and
   `RB_COLOR_INFO`'s) per resolve; read the final 1280x720 resolve (`04BDC000` in the
   store captures, `clr c/d 00`). Also the swap: which surface `Event 0` presents and
   through which format. Expected: `k_8_8_8_8_GAMMA` on the front buffer, plain on the
   scene passes. If the front buffer is PLAIN 8_8_8_8, hypothesis A loses its first
   half and only the ramp remains — go to step 4.
2. **Read Xenia's swap path for the reference semantics — and record the licence
   before reading it** (Xenia is BSD-3-Clause; structural reference, note it in
   `docs/rtx-remix-prior-art.md`'s style): `src/xenia/gpu/d3d12/d3d12_command_processor.cc`
   `IssueSwap` and the `apply_gamma` shaders, `xenos.h` for the PWL curve
   (`k_8_8_8_8_GAMMA` uses the Xenos piecewise-linear encode: 4 segments, NOT sRGB), and
   `register_table.inc` for `DC_LUT_RW_INDEX` / `DC_LUT_30_COLOR` / `DC_LUT_PWL_DATA` /
   `DC_LUTA_CONTROL`. The question to answer: which of the two tables Xenia applies for
   an 8-bit front buffer, and whether the PWL ENCODE at resolve is applied separately
   from the ramp.
3. **Measure before building.** Take hardware's final frame from the `.xtr` (the
   `MemoryWrite` of the 1280x720 resolve — the bytes are IN the capture, gotcha 259; a
   small tool, `xtr_frame_extract.py`, next to `xtr_resolve_census.py`) and Xenia's
   `.png` of the same spot. Fit the pixel-value mapping front-buffer→png as a per-channel
   curve: that curve IS what Xenia applied (ramp, PWL decode, both). Then take OUR front
   buffer at the same spot (an operator F9 in the Big Buck store; `CZ_VK_SNAP_DUMP` gives
   every resolve) and compare its value histogram against hardware's front buffer, not
   against the png: if the two FRONT BUFFERS agree and only the pngs differ, the whole
   defect is the missing transfer and §3/§4 are not needed. If our front buffer is
   already darker than hardware's, the transfer is at most part of it — proceed to §3
   with that number.
4. **Build it as one arm with one control.** Read the DC_LUT writes in `pm4.cpp` into
   a 256-entry table (they arrive as ordinary register writes through the ring; the
   index register auto-increments — check the capture's `Registers` records for the
   pattern), and apply at PRESENT the same thing Xenia applies: the PWL decode of a
   GAMMA front buffer if step 1 said so, then the ramp. In the present blit (readback
   and swapchain paths both go through one place — part 54's seam), never in the scene.
   `CZ_VK_NO_GAMMA_RAMP=1` the control arm; a `[vk] gamma ramp: <identity|table>` line
   once, and a counter of ramp loads per run. The instrument must show the table it
   loaded (first/mid/last entries) so a wrong decode of the 10:10:10 packing cannot
   hide behind "applied".
5. **Gates.** (a) The picture gate `tools/frame_signature.py` against capture E (E3 is
   +0.84 today, luminance correlation): hardware's screenshots INCLUDE the ramp, so a
   correct transfer should move E3 UP, and a fall is a refutation. (b) The step-3
   histogram: our png-equivalent vs Xenia's png at the store, medians within a few
   levels. (c) The operator's eye in the Big Buck store, the bathroom and Barnyard
   Bonanza, side by side with the R4/R2 pngs — SHAPE questions (is the dark end lifted
   without the highlights blowing) are theirs. (d) `CZ_VK_NO_GAMMA_RAMP=1` restores
   today's picture exactly (a null pair, `frame_matched_diff.py` at the menu where
   frames match).

**Kill criterion for item 1:** if step 3 shows our front buffer already ≥30% darker
in median luma than hardware's at a matched interior, the transfer is not the main
term; still ship it if step 5(a) moves up, and carry the number into §3.

## §3. Item 2 — the tone map's constants (`gFinalGammaParameters` and friends)

Only if §2 leaves a residual. The three named constants are pixel-shader constants of
the post pass; `tools/xtr_draw_constants.py` prints hardware's values at the capture's
tone-map draw (find it by shader: the post chain's full-screen draws, `xtr_draw_bindings.py`
names them), and `CZ_VK_EXPOSURE_TRACE` / a `CZ_VK_PS_CONST_DUMP` of the same registers
prints ours at the same spot. Compare every constant the tone-map draw reads, the way
§6bg compared the ground draw's 32 — a table, not an impression. The gamma parameter is
the first suspect only because part 60 wrote "gamma does nothing in our runtime" and
never explained it: find what the Visuals meter WROTE (the profile setting → which
register), and whether our profile path leaves it at the console's default or at zero.
`CZ_VK_PS_CONST_SCALE="N.w=..."` (exists) is the arm to test a candidate without
building anything.

## §4. Item 3 — the scene itself (last, and only with a residual and a number)

If the front buffers still disagree after §2 and §3, the interior difference is in
the lit colour, and the candidates are the things an interior has that a street does
not: local lights (how does this engine feed them — constants per draw, a light list
texture, loop constants?), lightmaps and light probes in HDR formats
(`k_16_16_16_16_FLOAT`, `k_2_10_10_10_FLOAT` / 7e3 — a float texture bound as UNORM is
a darkening error), and the cube map the title renders (part 26). The method is part
27's: `xtr_draw_bindings.py` + `xtr_draw_constants.py` + `xtr_draw_vertices.py` on ONE
interior draw (a wall in the Big Buck store), every input compared, the first
disagreement named. Do not start here; nine parts say the scene is where the expensive
wrong turns live.

## §5. What the operator provides

- **F9 captures at three matched interiors**: the Big Buck hardware store (R4 spots 01
  and 03 are the aisle and the counter), Uncle Bill's bathroom (w4), Barnyard Bonanza
  (w7), standing where the R2/R4 pngs were taken, camera still. `CZ_VK_SNAP_DUMP` armed
  on that run so the front buffer and every resolve of the F9 frame are on disk.
- One F9 outdoors at `w1_spawn` for the control (the ramp is the same table there).
- After the arm ships: the three-way eye test (default / `CZ_VK_NO_GAMMA_RAMP=1` /
  the Xenia png) at the store.

## §6. Things not to do

- Do not touch the tone map, exposure or any scene constant before §2 has its number:
  every one of those has an arm already and none of them is a transfer curve.
- Do not "fix" darkness with a host-side brightness slider; the gamma-slider rule
  (part 60) applies to the fix too — the picture must come from the title's own tables.
- Do not compare against Xenia's png as a pixel diff; the screenshots were taken on a
  return trip (camera and clock differ). Histograms and medians, or the front buffer
  inside the `.xtr`.

## §7. Execution record (part 119, 2026-09-15) — §2 RUN; the transfer goes the OTHER WAY

Read `phase5-notes.md` §6fb for the measurements. In the plan's own terms:

* **§2.1 answered: the front buffer is PLAIN `k_8_8_8_8`** (rt format 0, dest format 6,
  unorm, endian 0) in all three captures, and no resolve in the frame is
  `k_8_8_8_8_GAMMA`. Hypothesis A's first half is refuted; only the ramp remained.
  `tools/xtr_resolve_census.py` prints the format pair and the swap now.
* **§2.2 done, licence recorded** (Xenia BSD-3-Clause, structural reference: the swap
  applies the 256-entry table for an 8-bit front buffer as `table[uint(x*255+0.5)]/1023`).
* **§2.3 half done.** Hardware's front buffer is in each single-frame capture as the
  title's own `MemoryRead` of the previous frame (`tools/xtr_frame_extract.py`); Xenia's
  PNG = ramp(front buffer) to ±1-6 levels on all eight R4 frames. **The other half — OUR
  front buffer at the same spot — needs the operator's F9s** (§5), which is unchanged.
* **§2.4 built:** `CZ_VK_GAMMA_RAMP=1` (pm4.cpp DC_LUT capture + `gpu/gamma_ramp.hlsl`
  at present), with the printed table and the counters the plan asked for. The control
  arm is the DEFAULT, because the ramp darkens and the report is "too dark" — the plan's
  polarity was wrong: **skipping the ramp makes us BRIGHTER than Xenia, not darker.**
* **§2.5 gates:** (b) the GPU pass equals the Python reference to rounding; (d) the null
  pair holds (1,409/1,409 logo frames). (a) E3 is a normalised correlation and cannot
  read a transfer curve; not run. (c) is the operator's.
* **§3, one item taken:** `gFinalGammaParameters` is the Visuals meter's constant, from
  global `0x829EDCF4`; a live poke (`tools/guest_poke.py`) moves nothing in our frame.
  Which pass consumes it and whether it runs is the first §3 question if the F9s say
  our front buffer is darker than hardware's (`open-items.md` 0zc).
* **§4 untouched.**

**The kill criterion is recast.** If the operator's interior front buffer is within a few
levels of hardware's (30.2 at w4, 29.3 at w7), this renderer has no lighting defect and the
difference is the DISPLAY — Xenia's type-2 ramp, a console's type-1 identity, the TV vs
the monitor, or a raised Gamma meter on the console — with `CZ_VK_GAMMA_RAMP=1` the
Xenia-matching option. If ours is darker by more than that, §3 then §4 are live and the
number says by how much.
