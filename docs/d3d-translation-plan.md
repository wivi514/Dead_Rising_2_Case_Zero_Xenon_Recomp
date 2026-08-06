# The D3D translation pivot — plan and recon record

Decided by the operator 2026-08-06: **restructure the renderer as a D3D translation
layer in UnleashedRecomp's architecture** — hook the title's statically-linked XDK D3D
functions and translate at the API level — instead of emulating the Xenos command
processor underneath them. Keep everything still useful; take the maximum from
UnleashedRecomp.

This document is the plan, the day-one recon results, and the licensing record. It
supersedes `docs/phase5-3d-plan.md` (whose Step 0 instrument and Step 1 findings both
survive — see "What is kept").

## Why the pivot is the right call, in this project's own evidence

Every hard renderer defect this phase hit lived **below the D3D line**, in machinery the
title's own D3D library drives: `VdSwap`'s 52-dword padding (finding 39), the
ring-progress fences that parked the Draw Thread (finding 38), the EDRAM tiling +
window-scissor + resolve-destination complex (§6f), the depth-resolve gap (§6d), and a
bright-pass fed by an EDRAM emulation whose scene never crosses its threshold (step 1's
close-out: the post chain was *correct* and its input was wrong).

UnleashedRecomp proves the alternative removes the class rather than the instances: in
its runtime **`VdSwap` and `VdInitializeRingBuffer` are empty stubs**. The hooked D3D
functions never call down, so there is no ring, no PM4, no EDRAM emulation, no fence
protocol — the entire layer findings 38–41 were spent on is dead code.

The trade is honest: in exchange we must find and hook the title's D3D functions
(UnleashedRecomp hooks **42** in `gpu/video.cpp`, hardcoded to Sonic Unleashed's
addresses — nothing transfers by address), and any D3D entry the engine calls that we
miss will fall through into ring-submitting code that no longer has a consumer. Hook
completeness is the risk, and the un-pivoted arm is the safety net.

## Licensing, recorded before any code moves

* **UnleashedRecomp is GPLv3** (COPYING in its root). This repo's standing rule was
  "structural reference only"; the operator has widened it to "get the maximum". Taking
  *code* (not just structure) makes the runtime a GPLv3 derivative — acceptable to the
  operator, recorded here so the decision has a date. Every adapted file carries a
  provenance header naming what it derives from.
* **plume** (the RHI UnleashedRecomp renders through — D3D12/Vulkan/Metal) is a
  **separate project**: submodule `renderbag/plume`, NOT vendored in the local checkout
  (the directory is empty). Fetch it and **check its own license at vendor time** before
  assuming anything about it.
* XenonRecomp/XenosRecomp remain MIT; our own code is ours.

## What is KEPT — the section that saves the next session

Everything below the GPU is untouched: phases 0–3, the kernel HLE, the save layer, the
window/input seam, and **every gate** (`--smoke`, A1's 84-prefix, A5 exit 0,
`truncated=0`, both `.xtr` oracles).

From the phase-5 renderer, transferring almost whole:

* **The shader pipeline.** At D3D level the hooks receive the shader **container**
  (`CreateVertexShader`/`CreatePixelShader`) rather than bare microcode from `IM_LOAD`.
  Hash the embedded ucode with the same FNV → the existing 336-shader SPIR-V cache keys
  match by construction. Better than before, in fact: real containers carry the real
  vertex declarations, replacing `synth_shader_container.py`'s positional guesses.
* **`vk_renderer.cpp`'s decode guts**: the Xenos texture-format table, the
  `XGAddress2DTiledOffset` untiler, the component swizzle (§6r), the vertex-format
  table with USCALED/SSCALED, the bindless heaps, `Host_PresentPixels`.
* **All measurement instruments**: `frame_signature.py` (the transform detector),
  `frame_matched_diff.py`, `frame_compare.py`, the per-pass dependency-graph trace,
  `CZ_VK_PSBIND`, the draw probe with its MINFRAME/MINVERTS bounds.
* **The PM4 executor stays**, twice over: it is the boot-progress engine until the hook
  set is complete, and afterwards it is the same-binary control arm (gotcha 86) — plus
  the un-hooked fallback if Case Zero's engine calls a D3D entry we have not found.
* **The PM4 knowledge is the signature database for the recon.** Every D3D function we
  must hook is recognisable by the PM4 it builds, and we know that vocabulary to the
  packet (225 headers, every opcode named).

From UnleashedRecomp, to take:

* The **42-hook inventory** in `gpu/video.cpp` — the checklist of what a working layer
  needs (device, resources, locks, state, draws, present, vertex declarations).
* `video.cpp`'s semantics: GuestDevice layout, resource lifecycle, XXH3 pipeline
  hashing, the lazily-filled sampler heap (independently reinvented by Fable 2 and by
  us), MSAA/resolve handling.
* **plume** as the RHI, subject to the license check above.
* The embedded zstd shader-cache scheme (optional — our on-disk `.spv` cache works).

## Day-one recon: Case Zero's D3D cluster is already bounded

Method: a kernel import used only by the D3D library marks its internals; callers of
those internals, walked upward, are the API surface to hook. Measured today with
`tools/import_call_sites.py` and a `ppc/` grep:

| anchor | function | role |
|---|---|---|
| `VdSwap`'s only call site | `sub_82841F00` | the swap internal — finding 39's own function (`addi r11,r29,256`) |
| `VdInitializeRingBuffer` + `VdEnableRingBufferRPtrWriteBack` | `sub_828462E0` | device/ring init |
| `VdSetGraphicsInterruptCallback` (2 sites) | `sub_8284CE80`, `sub_8284D270` | interrupt registration |
| callers of `sub_82841F00` | in TUs 159/160/176 | the Present path above the swap |

All in `ppc_recomp.{159,160,175,176}.cpp` → **the XDK D3D library occupies roughly
`0x8283xxxx–0x8284xxxx`**, exactly where findings 38–41 already placed "the driver"
(`sub_8283C6C8`, `sub_82845160`, `sub_8284B568`). Nothing learned there is wasted; it
was the map.

## Phases

### The checklist: UnleashedRecomp's hook inventory

42 hooks + 13 stubs in its `gpu/video.cpp`. The XDK-D3D subset is the checklist for
Case Zero; the last three named ones (`MakePictureData`, `SetResolution`,
`ScreenShaderInit`) are Sonic Unleashed *engine* functions and have no Case Zero
equivalent to look for — Case Zero will have its own engine-side additions instead.

| group | hooks to find in Case Zero's image |
|---|---|
| device | CreateDevice, Present, GetBackBuffer |
| resources | CreateTexture, CreateVertexBuffer, CreateIndexBuffer, CreateSurface, DestructResource, GetSurfaceDesc, GetVertexBufferDesc, GetIndexBufferDesc |
| CPU access | LockTextureRect/Unlock, LockVertexBuffer/Unlock, LockIndexBuffer/Unlock |
| shaders | CreateVertexShader, SetVertexShader, CreatePixelShader, SetPixelShader |
| declarations | CreateVertexDeclaration, SetVertexDeclaration, GetVertexDeclaration, HashVertexDeclaration |
| state | SetRenderTarget, SetDepthStencilSurface, SetViewport, SetScissorRect, SetTexture, SetStreamSource, SetIndices, Clear, StretchRect |
| draws | DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP |
| plus | render-state/sampler-state setters (hooked via SetRenderState in video.cpp), a GammaRamp-class stub set |

### Phase A findings so far

The swap internal `sub_82841F00` has exactly four callers:
`sub_827D2FC0`, `sub_827D3898`, `sub_827CDE48` (all `0x827Cxxxx–0x827Dxxxx`) and
`sub_8284B828` (inside the cluster). Three callers sitting BELOW the bounded range
means one of two things — the D3D library is wider than `0x8283–0x8284`, or those
three are engine-side and the D3D `Present` is `sub_8284B828`'s neighbourhood — and
which one is the first disassembly question of Phase A, because Present is the first
hook Phase B needs.

**A — identify the hook set.** Produce the table `sub_XXXXXXXX ↔ hook name` for
UnleashedRecomp's 42, each verified by disassembly (`gdis.py`), found via (a) import
call sites as above, (b) PM4-building constants (`--find-uses` on draw/IM_LOAD header
immediates), (c) the engine's calls into the cluster from above. Deliverable is the
table in this document, with evidence per row.

**B — skeleton.** Vendor plume (license check first), port the video.cpp scaffold,
hook `CreateDevice` + `Present` only. Gate: the boot reaches the title screen with the
ring **never touched** (`Pm4_PacketCount() == 0` on the hooked arm) and A1/A5 hold.

**C — resources and draws.** Textures/buffers/shaders/declarations/draw hooks, reusing
our decode guts. Gate: `frame_signature.py` vs E2 says `identity`, and the kernel gates
still hold.

**D — retire or keep.** Measure both arms; the PM4 path stays as the control until the
D3D arm is strictly better on every gate.

## Standing discipline, unchanged

Both arms in the same binary; a rate, never a single run; the animated title screen is
one sample per frame (gotcha 133); every picture claim through `frame_signature.py` or
an operator's eyes; the kernel gates run, not assumed. The known boot crash
(`XexGetModuleSection('Digest')` → the title's own assert, `deadrising/asserts.php`) is
orthogonal to the pivot and stays parked by the operator's explicit call.
