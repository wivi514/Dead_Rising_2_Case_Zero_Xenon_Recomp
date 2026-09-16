# Part 120 plan — the NIGHT is black on both emulators and not on hardware: the exposure controller

**EXECUTED 2026-09-16 (part 120) — `phase5-notes.md` §6fc is the record.** §1's branch
was (b) and the term is not a GPU term: the title's controller reads its 1x1 luminance
resolve back on the CPU (`sub_825D65A8` -> `sub_825D6D18`), this renderer never wrote a
resolve into guest memory, and the operator's Xenia runs `readback_resolve = "none"`.
The keyframes are `prologue.csv` (night min 0.1 / max 1.5 / desired 0.03); the
controller formula is the same on DR2 PC's `LuminanceToExposure.bcp` (read with
`tools/d3d9_disasm.py`). Built: the tiny-resolve write-back in `DoResolve` /
`RetireOldestFrame` (`CZ_VK_NO_RESOLVE_WRITEBACK=1` the control). Midnight garage
mean luma 5.8 -> 32.1, exposure 0.10 -> 1.50 (the ceiling). §2 is not needed; §3 is
unchanged; §4 (the Gamma meter) is deferred until the operator has seen the new picture.
Owed: the operator's eye (night AND day garage), and the Xenia oracle
(`readback_resolve = "fast"`, capture request Round O).

**Written 2026-09-16, 02:00, at the end of part 119's operator session. Read
`phase5-notes.md` §6fb and `Xenia logs/R7_night/R7_NOTES.md` first — every number
below is from them.** Part 119's plan (`lighting-plan-part119.md`) is executed; its
§2 hypothesis (the post-tone-map transfer) is refuted as the mechanism, and this is the
successor for a fresh conversation.

## §0. What is established, and the one thing that is not

1. **Our front buffer matches Xenia's, day and night, to the digit that matters.**
   Garage by day: hardware fb mean 19.8-21.9, ours ~24.7. Garage at night (real
   19:0x, Katey dosed, and the pinned 22:00 both): hardware fb mean 6.4 / median 0 /
   p90 3.8; ours 5.8 / 0. The title's exposure scalar `pc(14).w` walks **0.386 →
   0.243 → 0.100** through dusk on hardware's traces and sits at **0.100 — its
   floor** from 19:00; ours reads the same floor. Day and night are the SAME 99 shader
   pairs with the same texture formats: night is constants and exposure only.
2. **The display ramp is not the mechanism.** It is Direct3D's
   `rec709_encode(srgb_decode(x))` for a type-2 `VdGetCurrentDisplayGamma`; it can
   only darken; Xenia applies it (its PNGs are table[fb] to +0.00); we never did.
   `CZ_VK_GAMMA_RAMP=1` applies it as an arm; the operator's verdict on it in the
   junkyard: "contrast is too intense" — and that was the SECOND table (§3 below).
3. **Real hardware shows a readable night.** Two YouTube frames of a 360 in the
   safehouse garage and Bob's Fish 'n Hunt at night (`~/DR2CZ-troubleshooting/part119/`
   — the operator's screenshots of the video; the link is still owed for a measured
   number), and the operator's own Series X in backward compatibility: dim, blue,
   walls and props readable, blacks not crushed. Both emulators show median 0 with 90%
   of pixels ≤ 4. A capture chain lifts blacks; it cannot invent wall texture where
   the emulated frame holds zeros. **Something real hardware does, neither emulator
   does — and everything this port has ever compared against is Xenia.**

The unknown is WHICH term. The candidates, in the order to test:

## §1. Item 1 — the exposure controller's floor at night (the lead)

The scalar goes DOWN through a darkening dusk. An auto-exposure targeting a mean
luminance goes UP. So either (a) the allowed range is keyframed by time of day and the
night keyframe pins the maximum at 0.1 — the designer's dark night, and hardware's
brighter picture comes from elsewhere — or (b) the range is wide and the CONTROLLER
measures the night as too bright, i.e. the luminance it reads back is wrong on both
emulators. (b) would be the whole report in one number.

What is known about the controller, from the image:
* `sub_823C29F0` serialises a day/night transition keyframe object with four floats:
  **`Start_ExposureMinimum` at +0x70, `End_ExposureMinimum` +0x74,
  `Start_ExposureMaximum` +0x78, `End_ExposureMaximum` +0x7C** (property names at
  0x82055858..0x82055898; the class carries `DayNightTransition` / `w_day_night` /
  `w_daynight_slate` in the image — `strings` around 0x820672dc).
* `sub_8259BD78` reads `mExposureMaximum` / `mExposureMinimum` / `mDesiredLuminance`
  (names at 0x82089734..0x8208975c) through the property getter `sub_8276DF08` — the
  runtime controller's parameters.
* `gLuminanceToExposure` (0x8208b09c) is a SHADER constant name: part of the mapping
  from measured luminance to exposure runs on the GPU.
* The luminance chain is the ten 16_16_FLOAT → 16_FLOAT resolves a frame
  (`xtr_resolve_census.py` format-pair block: rt 6 → dest 30, num 7 = float, **endian
  1** = 8-in-16), ending in small surfaces (`18770000`.. in the store frame, `1D560000`).
  No draw in the frame SAMPLES a 16_FLOAT texture (`xtr_draw_bindings.py`: no format
  30 anywhere), so the final value reaches the CPU by a memory read of the resolve
  destination — the one path a trace cannot show and an emulator can get wrong in
  silence.

Steps:
1. **Read the keyframes live** (`tools/guest_poke.py`'s read twin; or `gdb -p` on a
   parked headless run): find the transition object(s) by scanning for the property
   table / vtable of `sub_823C29F0`'s class and print the four floats at +0x70..+0x7C
   for every keyframe, with the hour each applies to. If the night keyframe's
   `ExposureMaximum` is 0.1, (a) is true by data and the night's brightness must come
   from a term other than exposure → §2. If the night maximum is well above 0.1, the
   controller is choosing the floor → (b), continue.
2. **Print what the controller reads.** Hook the CPU read of the final luminance
   resolve (the destination address of the LAST 16_FLOAT resolve each frame; the
   `[vk] resolve` trace names it) — `CZ_ARG_PROBE` on the guest function that
   converts the float16, or a watchpoint on that address — and log the value per
   frame beside `pc(14).w` (`CZ_VK_EXPOSURE_TRACE`) through a pinned dusk (§3 of the
   R7 notes: `CZ_DEBUG_FLAGS="DISABLE TIME OF DAY"` + `guest_poke.py 82A578D0 f32:H`
   for H = 16, 18, 19, 20, 22). A luminance that RISES as the scene darkens is the
   defect; a luminance that falls while E still falls means E is not key/L and the
   formula needs reading.
3. **Check the resolve itself against the shader's own view of it.** Dump our
   16_FLOAT resolve destination bytes (`CZ_VK_SNAP_DUMP` gives the snapshot; the guest
   memory bytes are what the CPU reads) at a night frame and decode them as float16
   with and without the 8-in-16 swap; compare with the snapshot's pixel values. A
   mismatch is a resolve-format defect ours would share with... nobody — Xenia is
   independent code — so agreement with the snapshot is expected and the interesting
   outcome is the VALUE: what luminance does the chain report for a black room?
4. **The Xenia side of (b) for free:** the R7 traces carry every constant of the
   frame; `xtr_draw_constants.py --all` on `transition_1/2/3` for the post-chain draws
   prints `gLuminanceToExposure` and the tone-map constants at each dusk step — the
   controller's inputs as hardware(Xenia) had them. If Xenia's luminance→exposure
   constants show the same floor, the two emulators agree on the readback too, and the
   difference with hardware is upstream of both (the float16 EDRAM blend, §2 item 2).

**Kill/branch criterion.** Step 1 alone decides the branch. Do not touch the tone map
or any light before it is read.

## §2. Item 2 — a night term both emulators drop (only if §1 says the exposure is by data)

Ordered by how cheap the test is:
1. **The night keyframe's other fields.** The same object holds the sun/ambient/fog
   colours for the night; print them all (step 1's dump) and compare with the
   constants hardware's night trace shows (`xtr_draw_constants.py` on
   `transition_3`, the ground shader `ad65b98593f95926`, pc(19)/(22)/(23)/(24)). If
   Xenia's night constants equal ours, the guest computed the same night on both and
   the term is on the GPU side.
2. **Float16 EDRAM behaviour.** The scene EDRAM is 8_8_8_8 but the luminance chain is
   16_16_FLOAT with blending; Xenos float16 render targets clamp/flush differently
   from a host R16G16_SFLOAT (denormals, the 7e3 path is not involved here). A
   luminance chain that underflows to zero on hardware and not on the emulators —
   or the reverse — moves E. Test: force the chain's EDRAM format to a wider host
   format (`R32G32_SFLOAT`) as an arm; if E moves, the format's precision is the term.
3. **The cube map the title renders** (part 26) as the night ambient source: at
   night the sky is dark on both emulators — is it dark on hardware? Unanswerable
   without a hardware capture; last.

## §3. Item 3 — the SECOND gamma table our runtime loads (independent; small)

`[vk] gamma ramp: ... load #2 ... [32]=16 [64]=67 [128]=305` appears ~2 minutes into
a boot here: `sub_828470A0`, a Direct3D device vtable method (vtable at 0x8213DDC8),
re-applies a ramp stored in the device at +0x5580/+0x5594 after `sub_82851740(dev,
+0x5584, +0x5588, +0x5590, 1)` — a mode/present-parameter change. No Xenia gameplay
capture (R2, R4, R7: all taken 30+ minutes into a session) carries this table; every
one carries the first. So either our runtime triggers a D3D path Xenia does not (a Vd
answer, the resolution apply, the settings load) or the second ramp is written by the
game from the Gamma meter's value and Xenia's captures were made with a profile that
never wrote it. Find the caller of the vtable slot (a `bctrl` through 0x8213DDC8's
table — `gdis.py --find-uses 0x8213DDC8`) and the ramp that +0x5594 points at (is it
the identity `sub_8284DA48` built, or a game table?). Until answered
`CZ_VK_GAMMA_RAMP_FIRST=1` is the Xenia-equivalent arm and the plain arm is the
"too intense" one.

## §4. What ships, whatever §1-§2 find

The operator's monitor is sRGB and the console's picture went through a Rec.709 ramp
and a television. A player on a PC needs a **working brightness control**, and the
title has one: the Visuals Gamma meter → `gFinalGammaParameters` → a post pass — inert
here (§6fb §5, global 0x829EDCF4). Whether the pass runs, which shader consumes the
constant and why it moves nothing is a bounded question (`CZ_VK_PSBIND`, the constant
census on a frame with the global poked to 2.0), and making it work then restoring the
row in the options screen (`options_pc.txt`, part 60) is the shape the plan's §6 rule
allows: the picture comes from the title's own tables.

## §5. Tools and captures this plan uses

* `tools/xenia_poke.py` (czwin `C:\cz\xenia_poke.py`): `hour H` pins Xenia's lighting,
  `clock H M` sets its mission clock (found on the heap by vtable 0x820112C0), `off`.
  `schtasks /run /tn cz_xenia` launches Xenia on the operator's desktop from ssh.
* `tools/guest_poke.py` for ours; `CZ_DEBUG_FLAGS="DISABLE TIME OF DAY"` + the float
  at 0x82A578D0 pins our lighting hour; the mission clock object is found by the same
  vtable scan (it was 0xB90C22A0 in two runs here; hour at +0x18, minute +0x1C).
* `tools/xtr_frame_extract.py` — hardware's front buffer out of any single-frame
  capture; `xtr_draw_constants.py` — `pc(14).w` and the rest per draw.
* Captures: `Xenia logs/R7_night/` (garage day, pinned night, Uncle Bill's pinned
  night, the real dusk transition_1..3) with `R7_NOTES.md`; ours at 19:00 with Katey:
  `~/DR2CZ-troubleshooting/part119/ours_19h_garage_katey_rec709first.ppm` (+ census).
* Owed from the operator: the YouTube link (to measure the hardware frames), and a
  phone photo of the Series X garage at night next to ours on the same screen.
