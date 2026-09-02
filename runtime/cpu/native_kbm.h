#pragma once
// Native keyboard/mouse through the title's OWN input layer (part 92).
// See native_kbm.cpp for the design and docs/native-kbm-phaseA.md for the recon
// every address in it comes from. CZ_NO_NATIVE_KBM=1 is the whole-feature arm.

#include <cstdint>

struct PPCContext;

// --- guest-thread side (called from the XamInput HLE in kernel/imports.cpp) ---

// Whether the feature is switched on at all (env + build). Cheap, cached.
bool NativeKbm_Enabled();

// Whether the keyboard controller is connected and being fed — the flag
// window.cpp uses to retire the v1 keyboard->pad merge.
bool NativeKbm_Active();

// Called from the XamInputGetState hook (any user): runs the one-time
// verify + connect state machine until the keyboard controller is live.
void NativeKbm_Pump(PPCContext& ctx, uint8_t* base);

// The real XamInputGetKeystrokeEx. Reads r3 (ptr to user index), r4 (flags),
// r5 (keystroke out) from ctx, writes the result to r3. Also performs the
// per-tick analog/button source feed (it is called once per controller-update
// tick by the keyboard controller's own Update — the right cadence and thread).
void NativeKbm_HandleKeystroke(PPCContext& ctx, uint8_t* base);

// --- window-thread side (called from host/window.cpp's event loop) ---

// A key event, already translated to a Windows VK code. mods carries the
// XINPUT keystroke modifier bits (0x8 shift, 0x10 ctrl, 0x20 alt).
void NativeKbm_PushKey(uint16_t vk, uint16_t unicode, bool down, bool repeat,
                       uint16_t mods);

// Accumulated relative mouse motion (pixels) since the last event.
void NativeKbm_MouseDelta(int dx, int dy);

// Current mouse button state: bit0 = left, bit1 = right, bit2 = middle.
void NativeKbm_MouseButtons(uint32_t mask);

// Wheel steps (positive = up/away). Synthesized into KEY_1/KEY_3 keystrokes to
// match the DR2 PC mousemap's own wheel/key pairing (see tools/gen_kbm_map.py).
void NativeKbm_MouseWheel(int steps);

// Movement key state from the window thread: bit0=W bit1=S bit2=A bit3=D.
// (Second iteration: movement rides the XInput merge — window.cpp — so this is
// currently informational only; kept because the event plumbing feeds it.)
void NativeKbm_MoveKeys(uint32_t wasdMask);

// Device-follow (part 92): any REAL input names its device — pad input flips
// the prompt art to the Xbox glyphs, keyboard/mouse input flips it to the key
// chips, by swapping the decoded glyph texels in guest memory (the renderer's
// content guard re-uploads). Cheap when the device is unchanged.
void NativeKbm_NoteDeviceInput(bool pad);

// Synthetic pad-button bits for the PC-options panel pump (imports.cpp): the
// guest Visuals screen is driven by pad-0 BUTTON bits, which the reduced merge
// no longer produces from keys — this returns the keyboard equivalents
// (arrows=dpad, Enter=A, Esc=B, X=X) from live key state, consumed ONLY by the
// panel pump so nothing reaches the game twice.
uint32_t NativeKbm_PanelButtons();

// The camera's unclamped remainder, in stick units (±1.0 = full deflection),
// XInput sign convention. window.cpp feeds the clamped part through the XInput
// right stick; this surplus is added into the title's EFFECTIVE source cells
// after its own per-frame publish — the race-free spelling of the DR2-PC
// no-turn-rate-ceiling camera.
void NativeKbm_CameraSurplus(float sx, float sy);
