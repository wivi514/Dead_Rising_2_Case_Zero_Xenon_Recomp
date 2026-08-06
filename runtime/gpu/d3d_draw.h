// Phase C of the D3D pivot: draws serviced at the API line, by REDIRECTED EMISSION.
//
// THE STRATEGY, AND WHY IT IS NEITHER OF THE KICKOFF'S TWO
// --------------------------------------------------------
// docs/d3d-phase-c-kickoff.md offered a choice: read the device struct's register
// shadow lazily at draw time, or hook every state setter and keep host-side state.
// Both require re-deriving what the XDK D3D library encodes — the shadow layout, the
// constant staging scheme, the shader-variant selection, the copy-state setup — and
// every mistake in that derivation is a silent wrong picture (the gotcha family this
// project keeps paying).
//
// This module does a third thing: it lets the title's OWN code do the encoding.
// Each content API call (the four draws, Clear/ClearF, Resolve, PreSwapResolve) has
// its command-buffer cursor (dev+0x30, end dev+0x38) redirected to a private guest
// scratch buffer for the duration of the call-through. The guest body runs
// unmodified — its draw flush reads the register shadow, selects shader variants,
// stages UP vertices, emits SET_CONSTANT / LOAD_ALU_CONSTANT / IM_LOAD / DRAW_INDX —
// but the packets land in our scratch instead of the ring. A private walker (a
// faithful subset of gpu/pm4.cpp's decode) folds them into a private register file
// and shader bindings and hands every draw to the phase-5 renderer's decode guts.
//
// What this buys, concretely:
//   * The shadow -> register encoding is the title's, by construction. There is no
//     layout to reverse-engineer and nothing to keep in sync per-title.
//   * Return values are the real ones: every serviced entry still calls through, so
//     the GpuBusyTrack class of trap (phase B: a "fire-and-forget" that returned a
//     CPU pointer) cannot recur here.
//   * The RING never carries draw content. The lifecycle (Swap, fences, init) still
//     calls through and the PM4 executor keeps consuming it as the boot engine —
//     kickoff trap 3's blessed shape.
//
// The cost, stated: the guest flush runs per draw (it would on console too), and the
// scratch parse adds one linear walk over a few hundred dwords per call. Measured
// against the phase-5 renderer's synchronous submit, this is noise.
//
// THE REDIRECT'S SAFETY ARGUMENT
// ------------------------------
// The reserve function (sub_82845F68) is only called when cursor > end. The scratch
// is 4 MB with the published end 64 KB before the real end, so a single call's
// emission (worst observed case: a UP draw staging 0x4000 bytes of vertices inline)
// cannot reach it. If the guest ever DOES call reserve while a redirect is active,
// that is a real defect in this argument — so the reserve is hooked and counts it
// loudly rather than letting segment bookkeeping run against a cursor it does not
// own (see D3dDraw_NoteReserve).
//
// Nesting is real and handled by parsing at call boundaries: Resolve and Clear call
// the draw entry internally, so an inner hook that fires while a redirect is active
// does not re-redirect — it first parses the scratch up to the current cursor (so
// state staged by the previous inner call is read before the next one overwrites
// it), then calls through into the already-redirected stream.
#pragma once

#include <cstdint>

// NOT ppc_context.h's PPCFunc typedef: that one carries __restrict__, which is not
// part of the function type, so this plain spelling accepts the same functions
// without dragging the recompiled image's headers into every includer.
struct PPCContext;
using CzGuestFunc = void (*)(PPCContext& ctx, uint8_t* base);

// True when CZ_D3D_DRAW=1 and the renderer could be brought up. First call does the
// lazy init (scratch allocation, register-file seed, Vulkan bring-up); the result is
// sticky. Loud on every failure path.
bool D3dDraw_Enabled();

// Service one content API call: redirect the device's command-buffer cursor into the
// scratch, call `through`, parse what was emitted, and dispatch draws/resolves to
// the renderer. Returns true — the call-through already happened inside.
// The guest device is r3 of every content entry.
bool D3dDraw_ServiceContent(PPCContext& ctx, uint8_t* base, CzGuestFunc through);

// The Swap hook's present: submit and publish the frame the walker has accumulated,
// BEFORE the guest Swap runs (same ordering as the PM4 arm: renderer first, then the
// frame descriptor from the stream). The Swap itself still calls through — the
// completion protocol is the guest's (kickoff trap 2).
void D3dDraw_OnSwap(uint8_t* base);

// The ring-reserve interposer. The original safety argument ("reserve only fires
// when cursor > end") was WRONG, and the first run proved it: emitters call the
// reserve unconditionally to guarantee themselves a block, and its body closes the
// current segment, kicks it, and can PARK the calling thread in the free-space wait
// (sub_82845160 — finding 38's function) — all against a cursor that, mid-redirect,
// points at our scratch. The first run stalled the boot exactly there.
//
// The correct service follows from the reserve's own contract (disassembled: it
// takes only the device and returns [dev+0x30]): while a redirect is active on the
// calling thread, "you have space" is ALWAYS true — so answer r3 = the scratch
// cursor and never run the guest's segment machinery. Outside a redirect it calls
// through untouched. Returns true when it serviced the call.
bool D3dDraw_ServiceReserve(PPCContext& ctx, uint8_t* base);

// The walker's counters, printed at exit alongside the renderer's.
void D3dDraw_DumpStats();
