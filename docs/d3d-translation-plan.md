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

### Phase A: the hook table (delivered 2026-08-06, session 12)

Method: `tools/guest_callers.py` (written for this — a `bl`/tail-`b` call-graph
scanner attributing every site to its enclosing function via the recompiler's own
function map). Two structural results came first, and they bound the whole phase:

1. **The externally-called surface of the D3D cluster is 117 functions, and only 27
   of them can reach the ring.** Computed mechanically: every function in
   `[0x82834000, 0x82856000)` called from outside it, intersected with reverse
   reachability from the ring-reserve `sub_82845160`. The other 90 are state
   setters/getters that only write bitfields into the device struct's register
   shadow (e.g. `dev+0x2934` is the RB_DEPTHCONTROL shadow) plus dirty flags at
   `dev+0x10` — **they can stay guest code even in replace mode**, because the
   replace-mode draw hook can read the accumulated shadow out of the device struct.
   The caveat: the scan sees direct calls only; indirect entries (`sub_8284B828`,
   `sub_828494A0`) and any engine vtable call are what `Pm4_PacketCount()==0` gates.
2. **The Present ambiguity is resolved.** `sub_8284B828` loads the device from a
   GLOBAL (`0x82000758`, or `0x8200075C` for a system-process caller — two device
   slots) and conditionally swaps: it is the D3D-internal worker/ISR path, not the
   API. The three `0x827D` callers pass the device in `r3` and are the engine's
   present path. `sub_82841F00` stays the Swap hook. In a 100 s OBSERVE run the
   worker fired twice (boot only) and the indirect draw path `sub_828494A0` never —
   the engine submits directly, so finding 40's threads are idle at boot/title.

The table. Every row verified by disassembly; rows marked `?` are tentative labels
whose *firing pattern* OBSERVE has validated but whose exact XDK name has not been
pinned. Per-frame rates are from the title screen of the OBSERVE run
(`CZ_D3D_OBSERVE=1`, 2026-08-06).

| hook | address | evidence | rate/frame |
|---|---|---|---|
| CreateDevice | `sub_8283CCE8` | allocates 0x5F00-byte device, E_OUTOFMEMORY on fail, behavior flags → `dev+0x5E80` (defaults in 0xC00), device out-param via r8; only caller is engine init `sub_827D3B68` | 1 total |
| Swap | `sub_82841F00` | `VdSwap`'s only call site, finding 39's `addi r11,r29,256` | 1 |
| PreSwapResolve | `sub_82841AD0` | `sub_82841EF8` = `li r4,0; b` here; called immediately before every Swap | 1 |
| DrawIndexedVertices | `sub_82843A98` | decodes an index-buffer object (`+0x24/+0x28/+0x30`, 16- vs 32-bit via 0x400 flag); the IB is a direct argument — there is no separate SetIndices | ~60 |
| DrawVerticesUP | `sub_82842A88` | stages user data through the 0x4000-byte scratch at `dev+0x780` | ~40 |
| DrawIndexedVerticesUP | `sub_82842E78` | same scratch, one more argument | ~1,330 |
| DrawUP variant | `sub_82842570`? | count*stride math, 16-bit index halving; called only from engine mesh path `827D5180` | 0 at title |
| Resolve | `sub_82838858` | takes float ClearZ (Resolve's only float) and internally DRAWS via `sub_82843A98` — matches the PM4-side fact that a resolve is a rect draw + RB_COPY state | 1 |
| Clear | `sub_82841630` | rects + float Z, wraps worker `sub_82841508` (ClearF); Clear's private shaders load via `sub_82840E58/sub_828408E0` under it | ~20 |
| SetRenderTarget | `sub_8283FD28` | `stwx r5,(r4+0xC52)<<2,r3` → `dev+0x3148+index*4`; `sub_82840350` is a `b` thunk to it | ~290 |
| SetDepthStencilSurface | `sub_82840078` | stores directly to `dev+0x3158` = slot 4 of the same array; draw internal reads it | ~70 |
| SetViewport | `sub_8283FBF8` | converts int viewport struct (+0x10/+0x14 float minZ/maxZ) and tails into float worker `sub_8283F990` (SetViewportF) | ~215 |
| SetTexture | `sub_82839830` | copies 16-byte fetch constants into `dev+(sampler+0x78)*16`, `dcbt`-prefetched | ~1,850 |
| SetStreamSource | `sub_82836958` | VB pointer → `dev+0x31B0+stream*4`, builds vfetch constant in the 0x18-stride shadow | ~3,500 |
| SetVertexShader | `sub_82839D38`? | stores `dev+0x3248` — the slot the draw flush `sub_8284F300` BAILS on if null (a draw without VS is impossible); retires the old shader with the current fence first | ~330 |
| SetPixelShader | `sub_82839B78`? | stores `dev+0x3244` — nullable at the flush (depth-only); same fence-retire head | ~550 |
| SetVertexDeclaration | `sub_82839F08`? | 5-instr passive setter of `dev+0x2ED8`, the third slot the flush reads (decl+VS combine at draw, D3D9-style) | ~410 |
| SetShaderConstantF (pair) | `sub_8283E950` / `sub_8283EAF8`? | identical heads indexing the 0x18-stride constant shadow at `(reg+0x30)*0x18`; which is VS vs PS unpinned | ~600 each |
| SetClipPlane | `sub_8283F848`? | float4 into a small dirty-masked 16-byte-stride table at `dev+(idx+0x282)*16` | — |
| GetRenderTarget / GetDepthStencilSurface | `sub_8283F620` / `sub_8283F668` | read the same slots back, AddRef via `sub_82836DE0` | — |
| DestructResource | `sub_82837CF0` | wraps `sub_82837788`: switch on resource-type nibble (obj dword0 low 4 bits, 1–9), frees physical pages, emits per-type fence | ~60 |
| InsertFence | `sub_82846068`? | flush + returns `dev+0x2A9C` (monotonic fence value) | 1 |
| InsertCallback | `sub_82845230`? | fixed 6-dword emit carrying its r3 argument — BUT observed args are small ints (r3=0xD), so the label is doubtful; revisit | 1 |
| Fence/throttle | `sub_82846288`? | compares `dev+0x30` vs `dev+0x38`, conditional flush | ~3 |
| SuspendNotify | `sub_8283C898`? | walks the notification-callback list at global `0x82000764`, then emits | 1 |
| FlushGpuCache | `sub_8283A110`? | builds a type-3 header `oris 0xC001` from a mask ANDed with `dev+0x325C` | ~6 |
| Flush-state | `sub_82838088`? | clears the `dev+0x325C` flush mask; on the pre-swap path every frame | ~20 |
| GPU busy-track pair | `sub_82837D70` / `sub_82837DC0`? | wrap `sub_828379A8` (atomic +0x100 GPU refcount on obj dword0 + emit), addr/size from `res+0x18/+0x1C` | ~1 |
| CreateResource family | `sub_82836630/640/648` → worker `sub_82836038`? | thunk family; `sub_82836668` allocates 0x34-byte objects | ~1–14 |
| Unknowns | `sub_82838568`, `sub_82838D10`, `sub_82837E08` | called from `827A00B8` (movie player?) and `827CFxxx`; not yet decoded | 0 at title |
| Worker/ISR swap path | `sub_8284B828` | indirect-only; takes lock, calls token interpreter `sub_8284B568` then Swap | 2 total |

Key device-struct offsets recovered on the way (all per-title, gotcha 49):
`+0x10` 64-bit dirty flags · `+0x780` UP-draw scratch (0x4000 bytes) · `+0x2934`
RB_DEPTHCONTROL shadow · `+0x28FD..+0x2903` blend bytes · `+0x2948` cull-mode
shadow · `+0x293C` stencil shadow · `+0x2A9C` current fence value · `+0x2ED8`
vertex declaration · `+0x3148+i*4` render targets (slot 4 = `+0x3158` = depth) ·
`+0x31B0+i*4` stream sources · `+0x3244`/`+0x3248` pixel/vertex shader ·
`+0x325C` gpu-cache flush mask · `+0x5E80` behavior flags. Device globals:
`0x82000758` (title), `0x8200075C` (system), notification list `0x82000764`.

**OBSERVE mode is built and the gate PASSED** (`runtime/gpu/d3d_hooks.cpp`,
`CZ_D3D_OBSERVE=1`, off = one predictable branch per hook). Title-screen stream:
exactly 1 Swap/frame, ~1,450 draws across four draw entries, 20 Clears — which
matches phase 5's ~20 resolves/frame, because a 360 Clear IS a resolve with clear
bits — 1 API Resolve, and the full state traffic above. `pm4_packets` keeps
counting on the observe arm (call-through verified). Gates on the hooked binary:
`--smoke` OK, A5 exit 0 (3 windows, all permutations), A1 clean to its usual depth
with only the documented position-71 jitter.

Open before Phase C: pin VS/PS order of the constant-set pair (one instrumented
run comparing against microcode hashes settles it); name `sub_82845230`'s real
role (its 6-dword payload against B1's packet vocabulary will); decode the
CreateResource family properly; decode `sub_82838568`/`sub_82838D10` (both
called from the suspected movie-player module `sub_827A00B8`).

**B — skeleton.** Vendor plume (license check first), port the video.cpp scaffold,
hook `CreateDevice` + `Present` only. Gate: the boot reaches the title screen with the
ring **never touched** (`Pm4_PacketCount() == 0` on the hooked arm) and A1/A5 hold.

### Phase B: DELIVERED (2026-08-06, session 12) — the ring goes silent by itself

`CZ_D3D=1` in `runtime/gpu/d3d_hooks.cpp`. What is serviced is CONTENT only — the
four draw entries, Clear/ClearF, Resolve and PreSwapResolve return S_OK and do
nothing. Everything belonging to the frame LIFECYCLE — Swap, fences, flushes,
busy-tracking, creation, destruction, init — still calls through, with the PM4
executor consuming whatever skeleton stream that produces. **Measured over 100 s:
the boot reaches the title screen (`prologue_z01.big`, full state traffic, ~1,450
serviced draws/frame), 33,984 frames, zero faults, and `pm4_packets` FROZEN —
+0 per frame in steady state.** With the content gone, the title's own Swap takes
its empty-frame branch and completes synchronously, so the ring falls silent
without the completion protocol being replaced at all. (~340 fps: the empty-swap
path never throttles; expected, not a defect.)

Two stricter variants failed first, and the reasons are load-bearing:

* **Servicing Swap hung the boot at frame 1.** The D3D worker thread
  (`sub_8284B828`) waits on an event embedded in the device struct that the real
  swap-completion protocol signals; the engine render thread (entry
  `sub_82769D58`) waits on the worker; the main thread polls the render queue's
  ticket (predicate `sub_82766760`, poll utility `sub_8276D590`) forever.
  Replacing Swap outright means implementing that protocol at the API line —
  a phase C/D task with the worker's disassembly in hand, not a skeleton task.
* **Servicing `sub_82837D70` (busy-track A) crashed the boot**: it is a
  Lock-style entry that RETURNS A CPU POINTER, and the engine dereferenced our
  serviced 0 while filling a resource from `boot.bct`. Its Phase A label was
  wrong in the way OBSERVE cannot see (rates fine, semantics not) — only
  REPLACE distinguishes "fires plausibly" from "return value consumed".

Gates on the replace arm: `--smoke` OK; A5 exit with 1 permutation + 1 real
window; A1 clean to position 81 with one REAL window — `KeResetEvent`,
`KfAcquireSpinLock`, `KfReleaseSpinLock` never occur in our run. All three are
verified downstream of ring consumption (the spinlocks are the graphics ISR's,
which only runs when the CP raises interrupts; KeResetEvent sits between two
Resolves in the completion path). An arm whose GPU work is empty legitimately
never reaches them — gotcha 106's class: a configuration difference of the arm,
recorded, not a regression. The control arm (same binary, `CZ_D3D` unset) gates
clean as before.

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
