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

## Case Zero — the fix that actually worked (and the two that did not)

Same engine, different XEX, and the port did NOT transfer cleanly. Recorded because
the dead ends are the useful part for Case West's own next sibling.

**Finding the live camera — trace from the command, not the shape.** Two functions
matched Case West's clamp+angle-store DISASSEMBLY SHAPE (`sub_82471EA0`, `sub_8246F9A8`,
plus `sub_824676C0`) and ALL were dead — operator-confirmed 0 calls, or hooks that never
fired. The live camera was found empirically: hook the float command query `sub_82805510`
(Case Zero's equivalent of Case West's `sub_827FFE90`, already in native_kbm.cpp) and log
who queries camera command **216** (`COMMAND_USER_CAM_LEFTRIGHT`) / **217** (`UPDOWN`).
The answer, during real gameplay: `lr=0x8246FA6C` / `0x8246FA84`, inside **`sub_8246F9A8`**
— which writes the persistent yaw at **`+0x40`(r31)** / pitch at **`+0x44`(r31)** (r31 =
r6 = the camera object; offsets read off its own `stfs` stores, and identical to Case
West's after all).

**Why the entry hook FAILED — and the bypass.** A strong `PPC_FUNC(sub_8246F9A8)`
override fires **0 times** even across 780k-resolve gameplay: the recompiler executes
that function's code through a path that bypasses its entry symbol (the byte before it,
`0x8246F9A4`, is a null word; both "entries" are dead). So the entry cannot be hooked.

The working bypass: the camera keeps its object in `r31` (non-volatile) and calls the
command query `sub_82805510` FROM INSIDE ITSELF — and that query DOES hook and fire. At
the query hook's entry, before the query's own prologue, `ctx.r31` still holds the
caller's r31 = **the camera object**. Capture it there (`g_camObj`, gated to the camera's
own call sites 0x8246FA6C/0x8246FA84 so the deadzone-checker at 0x821592F0 does not
overwrite it), then add the accumulated mouse delta onto `camera+0x40 / +0x44` from the
per-frame source-publish hook `sub_82804AF8`. Adding to the persistent angle integral is
stable and un-clamped. **Operator-verified working (2026-09-04): "It does work".**
Headless proof of the plumbing: the capture grabbed camera object `A6741348` (in the
physical-alias heap, exactly where Case West's calibration found it) during gameplay.

## Case Zero vs Case West — the address map

| item | Case West | Case Zero |
|---|---|---|
| camera-update function | `sub_82470DC0` (hooked at entry) | **`sub_8246F9A8`** (entry un-hookable; captured via its command query) |
| yaw / pitch store offsets | +0x40 / +0x44 | **+0x40 / +0x44 (identical)** |
| smoother state (untouched) | +0x48 / +0x4c | +0x70 / +0x74 |

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
