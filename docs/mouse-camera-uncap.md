# Uncapping the mouse camera — imported from Case West (part 93, 2026-09-04)

Case Zero's part-91 mouse camera had a **turn-rate ceiling**: at MOUSE SENS 10 it felt
slow and raising sensitivity did nothing. Case West solved the identical problem
(its part 8); the operator asked to import that work. This records the port.

## Why the part-91 approach could not help

The mouse feeds the game's right-stick source, and the camera-update function turns that
into a turn rate through a **radial magnitude clamp**: it treats (yaw, pitch) as a 2D
vector, clamps `sqrt(yaw^2 + pitch^2)`, then applies a fixed max turn-rate. So any
input-side gain — Case Zero's part-91 `NativeKbm_CameraSurplus` (surplus above the stick
ceiling added into the effective source cells), or Case West's earlier command-query gain
— just makes a longer vector the clamp normalizes straight back. Case West proved it: 8×
and 20× input gain produced identical motion.

## The fix: add to the camera angle directly, past the clamp

The camera update stores the persistent camera angles in the object passed in `r6`
(`r31` inside the function): **yaw at `+0x40`, pitch at `+0x44`** (radians), and those
stores are its final action. We strong-hook it, capture `r6` on entry, and AFTER the
game's own clamped update add our uncapped mouse delta straight onto those accumulators.
Adding to the angle *integral* is stable — the engine's smoother reads its own separate
state, so there is no feedback loop.

Raw per-poll mouse deltas reach the hook via `NativeKbm_AddMouseLook` (fed from
`window.cpp`'s existing relative-mouse block; the stick feed stays so the engine's
"camera is being moved" state still sets).

## Case Zero vs Case West — the only differences

Same Blue Castle engine, different XEX build, so the addresses shifted; the RE
(disassembly-verified) mapping:

| | Case West | **Case Zero** |
|---|---|---|
| camera-update function | `sub_82470DC0` | **`sub_82471EA0`** |
| yaw / pitch store offsets | +0x40 / +0x44 | **+0x40 / +0x44 (identical)** |
| smoother state (untouched) | +0x48 / +0x4c | +0x70 / +0x74 |

`sub_82471EA0` was confirmed by its disassembly: the `fmuls`/`fmadds`/`fsqrts` magnitude
of the stick vector, the clamp, then `fadds prev,delta` → `stfs 0x40(r31)` /
`stfs 0x44(r31)` as the final action. (Case West's `0x82470DC0` is an unrelated function
in Case Zero — do not reuse it.)

## Units, sign, scale (from Case West's measurements; operator re-tunes live)

- **Radians.** The update applies deg→rad internally. `scale = 0.00027` rad per
  (mouse-count × sens) is Case West's operator-dialed landing (SENS 5 ≈ 0.135 rad / 100
  counts). Dial with the MOUSE SENS row or `CZ_KBM_LOOK_SCALE`.
- **Sign** defaults `sx = -1, sy = +1` (mouse-right = look right, mouse-down = look
  down) — the field-delta sign is opposite the stick-input sign. `CZ_KBM_INVERT_X` /
  `CZ_KBM_INVERT_Y` flip each live if Case Zero's build differs.
- **Gated** to `NativeKbm_Active() && MouseDeviceActive() && Settings_MouseCam()` and a
  sane `r6` range — a controller's camera is never touched.

The stick-feed scale in `window.cpp` was also bumped 140 → 350 (Case West commit
4eac54b, 2.5× faster); with the direct look it mainly keeps the input-active state.

## Controls

| control | effect |
|---|---|
| MOUSE SENS row (live) | linear multiplier on look speed (1..10) |
| `CZ_KBM_LOOK_SCALE=<f>` | overrides the 0.00027 base (coarse speed) |
| `CZ_KBM_INVERT_X=1` / `CZ_KBM_INVERT_Y=1` | flip each axis |
| `CZ_KBM_CAM_TRACE=1` | log the applied deltas |

## Owed

Operator verification with a real mouse (headless cannot feed one). The mechanism is a
direct port of a Case-West fix the operator already verified there, with the address
disassembly-verified and the offsets confirmed identical, so confidence is high; the two
things that could differ on this build (sign, base scale) are both live-tunable.
