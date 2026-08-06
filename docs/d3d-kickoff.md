# D3D translation layer kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically and is current. This file adds what a fresh context
needs to start the **D3D translation pivot** without re-deriving anything.
`docs/d3d-translation-plan.md` is the decision record (why, licensing, phase list);
this is the working hand-off, and its recon tables are ahead of that document.

## The task

Replace PM4-level GPU emulation with a D3D-level translation layer in
UnleashedRecomp's architecture: hook the title's statically-linked XDK D3D functions
via our existing alias seam, service them with a host renderer, and let the entire
ring/fence/EDRAM layer below them become dead code — as it provably is in
UnleashedRecomp, where `VdSwap` and `VdInitializeRingBuffer` are empty stubs.

## Read before writing code

1. `docs/d3d-translation-plan.md` — the decision, the licensing record, phases A–D.
2. `~/GithubRepo/UnleashedRecomp/UnleashedRecomp/gpu/video.cpp` — the reference
   implementation (7,882 lines). Its hook registrations are at the bottom (~line
   7798). GPLv3; the operator has authorized taking code, not just structure —
   provenance headers on every adapted file.
3. `docs/phase5-notes.md` §6q (the Y flip), §6r (texture component swizzle) — decode
   knowledge that transfers into any backend.
4. `docs/phase3-notes.md` — the window and present seam the new layer feeds.

## What ALREADY EXISTS — do not rewrite these

**1. The hook mechanism is gotcha 6, already proven.** `PPC_FUNC(sub_X)` strong
definitions override the recompiled `__imp__sub_X` weak aliases — the same seam every
kernel hook and probe uses. UnleashedRecomp's `GUEST_FUNCTION_HOOK` is the same idea;
no new machinery is needed. A hook that calls `__imp__sub_X(ctx, base)` passes
through to the original.

**2. plume is fetched and its license is VERIFIED: MIT** (clone at the session
scratchpad from `renderbag/plume`; re-clone if gone). It can be vendored into
`runtime/thirdparty/` with no GPLv3 implications. Only `video.cpp`-derived code
carries GPLv3.

**3. The shader pipeline transfers by construction.** 336 SPIR-V shaders in
`assets/shader_spv` (gitignored — CLAUDE.md's Commands section rebuilds it in three
lines), keyed FNV-1a over big-endian microcode. At D3D level,
`CreateVertexShader`/`CreatePixelShader` receive the shader **container**; hash the
embedded microcode with the same FNV and the cache hits. The container also carries
the real vertex declarations — better input than the synthesized ones the cache was
built from, and a chance to rebuild the cache from real containers later
(`CZ_SHADER_DUMP` can be moved to the hook).

**4. The decode guts in `runtime/gpu/vk_renderer.cpp` are backend-independent.** The
Xenos texture-format table (full 62-entry enum in `gpu/xenos.h`), the
`XGAddress2DTiledOffset` untiler, the fetch-constant component swizzle (§6r — without
it all text is solid blocks), the vertex-format table with USCALED/SSCALED (§6e), the
bindless heap layout matching XenosRecomp's five register spaces, and
`Host_PresentPixels` into the phase 3 window.

**5. The PM4 executor stays, as boot engine and as the control arm.** `CZ_VKDRAW=1`
is the old renderer, untouched. The new layer gets its own switch (suggest `CZ_D3D`)
so gotcha 86's same-binary A/B holds for every claim.

**6. Every instrument and gate.** `frame_signature.py` (verified able to name a
vertical flip against capture E2, with a negative control), `frame_matched_diff.py`
(noise floor from the same runs), `frame_compare.py`, the kernel gates, both `.xtr`
oracles. Gate commands are in CLAUDE.md; run the A1 gate with an empty save root
(gotcha 106).

## Recon state — measured 2026-08-06, evidence per row

**The map.** XDK D3D occupies ~`0x8282xxxx–0x8284xxxx` (TUs 159/160/175/176 plus
neighbours). Above it, Blue Castle's engine render layer at `0x827Cxxxx–0x827Dxxxx`
works through a **global renderer singleton at `0x82AC48A4`** — singleton-style code
is engine, `this`-based code is D3D; that is the boundary test. `sub_8280FEE0` and
friends are CRT save/restore helpers, not evidence of either.

| role | address | evidence |
|---|---|---|
| **`D3DDevice_Swap` — the Present hook** | `sub_82841F00` | `VdSwap`'s only call site; finding 39's own function (`addi r11,r29,256`); called directly from the engine present path — 360-style manual swap |
| engine present path (do NOT hook) | `sub_827D3B40 → sub_827D3898 → Swap`, `sub_827D3B68 → sub_827CDE48 → Swap` | loads the `0x82AC48A4` singleton; `sub_827D3B40` is reached indirectly and stores its arg to `0x82AC4880` |
| device init, stage 2 | `sub_8284CF88`, `sub_8284D270` | the two callers of `VdInitializeRingBuffer`'s wrapper `sub_828462E0`; `sub_8284D270` also registers the graphics interrupt callback |
| device init, stage 1 | `sub_8283CCE8` (→ CF88), `sub_8283C7F0` (→ D270) | one caller each — **CreateDevice is at most one or two levels above; walk these first** |
| **the 7 emit primitives** | `sub_82837788`, `sub_828379A8`, `sub_82841F00`, `sub_82845230`, `sub_82845488`, `sub_82845F68`, `sub_82846210` | the complete distinct-caller set of the ring-reserve `sub_82845160` — every packet the D3D lib emits goes through one of these. Classify each by the PM4 opcode immediates in its body (`gdis.py`): draws build `0x2200`/`0x3600` headers, shader loads `0x2700`/`0x2B00`, fences `0x4600`/`0x5800` |
| token interpreter (finding 40) | `sub_8284B568` | possibly D3D **command-buffer playback** (Begin/Run/EndCommandBuffer family — 360 D3D has it); `sub_8284B828` has zero direct callers (indirect) |
| ring-progress spin | `sub_8283C6C8` under `sub_82845160` | finding 38; dead code once hooks replace the layer |

**The anchor set for the rest.** All 20 Vd imports in the IAT, each one's call sites
naming another D3D function (`tools/import_call_sites.py`): `VdQueryVideoMode` /
`VdGetCurrentDisplayInformation` → display-mode/CreateDevice path; `VdRetrainEDRAM` →
init; `VdPersistDisplay` → suspend/swap path; `VdGetSystemCommandBuffer` +
`VdSetSystemCommandBufferGpuIdentifierAddress` → the system command buffer the
missed-hook detector should watch too. Full list:
`CallGraphicsNotificationRoutines, EnableDisableClockGating,
EnableRingBufferRPtrWriteBack, GetCurrentDisplayGamma, GetCurrentDisplayInformation,
GetSystemCommandBuffer, InitializeEngines, InitializeRingBuffer,
InitializeScalerCommandBuffer, IsHSIOTrainingSucceeded, PersistDisplay,
QueryVideoFlags, QueryVideoMode, RetrainEDRAM, RetrainEDRAMWorker, SetDisplayMode,
SetGraphicsInterruptCallback, SetSystemCommandBufferGpuIdentifierAddress,
ShutdownEngines, Swap`.

**The checklist** of what a complete layer hooks is the UnleashedRecomp inventory in
`docs/d3d-translation-plan.md` (42 hooks + 13 stubs; its three engine-side hooks have
no Case Zero equivalent). Expect Case Zero's hook set to differ in the same way its
engine differs — find the functions by what they DO, never by address analogy.

## The recommended first move: OBSERVE mode

Before replacing anything, hook the identified functions in a mode that only LOGS and
calls through (`__imp__sub_X`), with the PM4 arm still consuming the ring. Zero
behavioural change, and it validates every identification in one run: a title-screen
frame should show a plausible stream (state sets, N draws, exactly one Swap), and a
function that never fires or fires implausibly was misidentified. This is the
measurement arm the whole recon needs, and it costs an afternoon.

Then REPLACE mode (`CZ_D3D=1`): hooks service the calls, never call through, and the
ring must stay silent. **The missed-hook detector is `Pm4_PacketCount() == 0` on the
hooked arm** — any nonzero means an unhooked entry submitted packets, the ring trace
names the packet, and the packet's opcode names the function class to go find. An
unhooked emit in replace mode otherwise presents as a hang in `sub_82845160`'s
free-space wait, with no consumer to release it.

## Traps

1. **360 D3D is not desktop D3D9.** Draws are `DrawVertices`/`DrawIndexedVertices`,
   `SetTexture` writes fetch constants, command buffers exist, and the engine calls
   `Swap` directly with its own front buffer. Verify every signature against OUR
   disassembly (`gdis.py`), never against UnleashedRecomp's hook bodies — theirs are
   Unleashed-tuned and their addresses mean nothing here.
2. **The A1/A5 kernel gates must hold on both arms.** The D3D layer replaces guest
   code, not kernel calls — but device init consumes many Vd imports, and a hook that
   swallows an init step changes the kernel-call order the gate diffs. Position 71
   permutes 1-in-3 on every arm; that is documented jitter, not a regression.
3. **The title screen is animated: one frame is one sample** (gotcha 133). Every
   picture claim goes through `frame_signature.py` vs capture E, or the operator's
   eyes — the two defects a whole metric suite missed were found by the operator in
   one minute each (gotchas 135/136).
4. **The boot crash is orthogonal and parked** by the operator's explicit call:
   pressing START eventually hits the title's own assert
   (`XexGetModuleSection('Digest')` → `deadrising/asserts.php`). Do not chase it as a
   renderer symptom.
5. **`sub_8284B568` runs on its own guest threads** (finding 40's 0xF2C/0xF30). If it
   is D3D command-buffer playback, hooking the Run/Execute entry may be needed before
   the ring goes fully silent; if it is engine code, it stays. Decide from
   disassembly, not from the address.
6. **Keep the un-pivoted binary buildable at every commit.** The PM4 arm is the
   fallback if Case Zero's engine turns out to call a D3D entry that resists
   identification.

## Gates for the first session

```
./runtime/build/cz_runtime --smoke                       # unchanged, must pass
kernel_call_diff --xenia A1  (empty save root)           # 84-deep prefix, both arms
kernel_call_diff --xenia A5 --include-high-frequency     # exit 0, both arms
OBSERVE mode: per-frame call stream logged, 1 Swap/frame at the title screen
REPLACE mode (when reached): Pm4_PacketCount() == 0, boot reaches the title screen
```

## Standing constraints

Commit proactively with the Co-Authored-By trailer; document for an outside reader —
**Case West is next and lifts these documents**; retract in place; measurement
discipline (same-binary arms, rates not single runs, the control is the old binary
run now). New adapted files: provenance header naming the UnleashedRecomp source and
the GPLv3 consequence, per the licensing section of the plan.
