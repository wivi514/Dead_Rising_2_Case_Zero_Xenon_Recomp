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

### Phase C: the strategy is REDIRECTED EMISSION, and it is neither of the kickoff's two

Decided 2026-08-06 (session 13), from the recon this session ran. The kickoff offered
"read the register shadow lazily at draw time" vs "hook every setter". Both require
re-deriving what the XDK D3D library encodes, and the recon showed how much that is:

* The draw flush (`sub_8284F300`, disassembled in full) emits **type-0 PM4 packets
  whose register index names each shadow slot** — `dev+0x2884` = RB_COLOR_INFO,
  `dev+0x2920/0x2924` = SQ_PROGRAM_CNTL/SQ_CONTEXT_MISC, `dev+0x2954` =
  RB_MODECONTROL, `dev+0x3254/0x3258` = the current bin masks — but the blend state is
  BYTES the flush packs, the constants live in a separate staging scheme, and the
  flush also selects between **two vertex-shader variants under complementary bin
  masks** (`0x15555555`/`0x2AAAAAAA`), which a re-implementation would have to get
  right silently.
* The texture-sampler shadow is real and was mislabelled: **the Phase A table's
  "SetShaderConstantF pair" (`sub_8283E950`/`sub_8283EAF8`) is actually a pair of
  sampler filter/anisotropy setters** packing bitfields into a fetch-constant shadow
  at `dev + (group+0x30)*0x18` — one 6-dword entry per GPU fetch group, mirroring
  registers `0x4800+group*6`. (Retraction recorded in the Phase A table's terms: the
  row's firing pattern was right, its name and its VS/PS question were not — there was
  never a VS/PS distinction to pin.)
* Shader objects: VS microcode = `obj + [obj+(variant+0x70)*8]` inner struct
  (`+0x368` offset, `+0x36C` size) rebased by `obj+0x20`; PS = `obj+[obj+0x40]` inner
  (`+0x28`/`+0x2C`) rebased by `obj+0x18`.

Rather than re-implement that encoder, phase C **runs it and reads its output**: each
content API call (4 draws, Clear/ClearF, Resolve, PreSwapResolve) has the device's
command-buffer cursor (`dev+0x30`, end `dev+0x38`) redirected into a private 4 MB
guest scratch for the duration of the call-through. The guest body runs unmodified —
flush, variant selection, UP staging and all — and a private walker
(`runtime/gpu/d3d_draw.cpp`, a faithful subset of `gpu/pm4.cpp`'s decode over a
private register file) folds the emitted packets into state + shader hashes and hands
every draw/resolve to the phase-5 renderer's decode guts
(`VkRenderer_D3DDraw`/`VkRenderer_D3DSwap`, the same `DoDraw`/`DoResolve`/swap body
with the register file and shader bindings as parameters).

What this buys, and what it costs:

* **No shadow layout to reverse-engineer** — the title's own flush is the encoder, so
  the state semantics are correct by construction, per-title forever.
* **Return values are real** — every serviced entry still calls through, so phase B's
  `GpuBusyTrack` trap class (OBSERVE cannot see a consumed return value) cannot recur.
* **The ring still never carries draw content.** The lifecycle (Swap, fences, init)
  calls through as in phase B; with content drawn, the title's Swap takes its
  full-frame branch again, so the ring carries the swap/fence skeleton the PM4
  executor has consumed since phase 1 (kickoff trap 3's blessed shape). The present
  fires from the Swap hook, before the guest Swap runs — renderer first, then the
  stream's frame descriptor, the same order as the PM4 arm.
* Safety argument for the redirect: the reserve (`sub_82845F68`) fires only when
  cursor > end; the published end sits 64 KB inside a 4 MB scratch, and the reserve is
  hooked to count LOUDLY if it ever fires during a redirect. Nested content calls
  (Resolve/Clear draw internally) parse the scratch up to the cursor at every hook
  boundary, so state staged by one inner call is consumed before the next overwrites
  it.
* Cost: the guest flush runs per draw (it would on console too) plus one linear parse
  of a few hundred dwords per call — noise next to the renderer's synchronous submit.

Arms: `CZ_D3D_DRAW=1` is phase C (implies the replace harness); `CZ_D3D=1` alone
remains phase B's no-op skeleton; `CZ_VKDRAW=1` remains the PM4-fed renderer and is
mutually exclusive with `CZ_D3D_DRAW` (enforced loudly at init — two feeds into one
EDRAM image is a collision, not an arm). `pm4.cpp`'s behavior is untouched (one
passive env-gated instrument added, `CZ_PM4_MEM_WATCH`), so the control arm is
behaviorally identical to phase B's.

What the bring-up itself found (2026-08-06, session 13; each was measured, not
predicted, and each is a comment in the code where it bit):

1. **The reserve (`sub_82845F68`) is called unconditionally by emitters**, not only
   when the cursor passes the end — and its body closes/kicks segments and can PARK
   the thread in `sub_82845160`. Run 1 stalled the boot exactly there. Mid-redirect
   it is serviced: return the scratch cursor, never run the segment machinery.
2. **The cursor block is THREE fields**: `+0x30` cursor, `+0x34` hard segment end,
   `+0x38` soft reserve threshold. The chunked bulk emitter (`sub_8284DAF8`) computes
   per-chunk capacity from `+0x34`; redirecting only `+0x38` left it computing
   negative capacity and looping on the reserve forever.
3. **The content stream carries ~1 INTERRUPT packet per frame, and it is the token
   worker's kick.** The movie player (boot cinematics) submits through the D3D
   worker's token queue (finding 40's machinery); the worker is woken by the callback
   the stream ARMS in the scratch mirror right before the INTERRUPT. Three failures
   in sequence here: delivering synchronously from the engine thread risks deadlock
   (so interrupts are PENDED to the vblank pump); a deferred delivery that reads the
   LIVE mirror arrives after the guest re-poisons it and gets skipped (so each pend
   carries the mirror SNAPSHOT from its packet position, replayed before delivery);
   and the mirror CONFIG (SCRATCH_UMSK/ADDR) was set through the real ring at device
   init, before our register-file seed, so the walker falls back to pm4's registers
   for it. Without all three the boot deadlocks at `cinematics.big` with ~12 fences
   emitted-but-unretired and the CP fully caught up.
4. **The two-thread mirror is a race the one-stream world never had**: a walker
   mirror-write landing between the pump's poison check and the guest ISR's own
   mirror load hands the ISR the poison as its callback (measured: `ctr=0BADF00D`,
   `lr` inside the ISR). `Vd_MirrorMutex` serializes walker mirror writes against
   replay+delivery.
5. **The engine's per-frame GPU sync starves without content in the segments.** The
   wait's own head closes/kicks only when waiting for the NEWEST fence; redirected
   emission removes the content bytes that used to fill segments, so the fences the
   target names can sit in a never-closed segment. The sync-wait hook force-closes
   with the guest's own reserve before the wait runs.
6. `sub_828459D0` is the fence-block emitter: fence += 2 per block, an
   EVENT_WRITE_SHD pair into a caller-supplied cursor, plus a CPU fast path that
   writes the writeback word directly when the GPU is idle.

Measured state at this point: the boot renders the legal screen and the CAPCOM logo
pixel-correct from the D3D arm (frames dumped headless), clears the movie era, and
loads through `prologue_z01.big` (file #63) — the title screen's own scene. Boot
wall-time is ~4-5x the PM4 arm's (the load era paces on lifecycle round-trips); a
number to re-measure after the picture gate, phase 5 precedent.

### Phase C part 2: the movie deadlock, and the rule the redirect was missing

Session 14 (2026-08-06). The blocker `docs/d3d-phase-c2-kickoff.md` handed over — the
boot parking mid-cinematics with the engine thread in its per-frame GPU sync and
`emitted - completed` pinned at 16 — is **fixed**, and the diagnosis names a rule the
redirect had no statement of.

**The measurement that ended the argument.** The kickoff ranked three hypotheses. What
settled it was neither of them directly: it was running the SAME probe on both arms.
`CZ_FENCE_PROBE=1` (`runtime/cpu/guest_probe.cpp`) hooks the whole producer side of
the fence number — the fence-block emitter `sub_828459D0`, the segment submit
`sub_82845AC0`, the close/kick `sub_82845DE0`, the callback armer `sub_82845BA0` and
the graphics ISR `sub_82844D38` itself. Over one boot each:

| | PM4 control arm | phase C draw arm (before) |
|---|---|---|
| ISR delivers `sub_82841878` (frame tick) | 1,799 | 5 |
| ISR delivers `sub_827CC628`/`827CC640` (job ticks) | ~1,200 | 49 each |
| **ISR delivers `sub_8284AAD0`** | **138** | **1** |
| walker delivers interrupts (draw arm only) | — | 200, ALL `82841878` |

(Read the control column as a floor, not a total: `CZ_FENCE_PROBE` shares a
40,000-line budget across its five hooks and the control run saturates it. The draw
column does not — that run produced 17,385 probe lines in 240 s, so **one** is the
true whole-boot count. Gotcha 109 in our own new instrument, caught before it was
quoted as a ratio.)

`sub_8284AAD0` is the one that matters, and it had never been named: it pushes a job
onto the per-CPU D3D worker's ring (`[[0x82000758]] + cpu*0x6C + 0x2C40`, slot
`0x5C + (head&3)*4`) and `KeSetEvent`s the worker's event at `+0x3C`. The engine waits
on fences that only the worker retires. Delivered once and never again, the worker
sleeps forever and the boot parks — exactly the picture `gdb` showed: every thread
parked, the two worker threads (`sub_8284B828`) asleep in `KeWaitForSingleObject`.

**Why the redirect ate it.** `sub_82846288` — the Phase A table's "fence/throttle-shaped"
row, retracted here: it is the **callback armer**. It forwards to `sub_82845BA0`, which
lays down one block at the command cursor:

```
type-0 write reg 0x05C8 = 0x20000
type-0 write regs 0x057C,0x057D = (callback, argument)     <- the arm
WAIT_REG_MEM x3   hold the GPU until that mirror is visible in memory
INTERRUPT
WAIT_REG_MEM + type-0 write reg 0x057C = 0x0BADF00D        <- the re-poison
```

The ISR reads its callback back out of GUEST MEMORY (`[[user+0x2A94]] + 0x10/+0x14`),
so the whole block is a hand-off whose correctness IS its ordering against the CP and
against the guest's own poison store. Redirected emission put it in our private
scratch, where the walker had to emulate it — four designs, all racing the poison, and
the one that shipped read the arming out of the walker's own register file with a
fallback to pm4's. That fallback is why every delivery was the frame tick: the
`8284AAD0` arms rode transports it did not look at.

**The fix is to stop emulating it.** `D3dDraw_ServiceRealRing` runs `sub_82846288`'s
body with the REAL cursor block restored, so the arm/WAIT/INTERRUPT/poison block lands
in the ring and the title's own ISR delivers it, in the title's own order. The walker's
`0x54` handler is now dead code on this path and its counter says so. Measured, same
binary, one boot each: ISR deliveries of `sub_8284AAD0` went from **one in an entire
240 s boot** to continuous (enough that they saturate the probe's whole 40,000-line
budget), walker interrupt deliveries **200 -> 0**, and the boot moved from
`cinematics.big` (file #56) to `prologue_menu\zonelist.big` + `models\zombies.big`
(files #57-60 of 64).

A second, smaller fix was needed for the first to work at all. **The reserve
(`sub_82845F68`) is not "give me space" — it is CLOSE-AND-KICK.** It ignores the cursor
entirely: it hands the pending segment to the worker, closes and kicks it
(`sub_82845DE0`, which measures the segment as `[dev+0x3B20] .. [dev+0x30]+4`), and only
then returns `[dev+0x30]`. Resolve's MULTI-TILE path (`sub_82838858 + 0x250`, taken once
`dev+0x327C` — the bin/tile count — exceeds 1, i.e. from the first tiled frame) calls it
and DISCARDS the return value: the call is there purely for the kick, immediately before
the `0x88000000` token that queues that segment. Phase C's first service suppressed the
whole thing because running it mid-redirect would have measured the segment out to our
scratch cursor. That reasoning was right and the conclusion was too strong. The service
now restores the real cursor block, runs the guest's own reserve against it, adopts the
fresh segment, re-installs the scratch and answers with the scratch cursor —
`CZ_D3D_NO_RESERVE_KICK=1` is the same-binary arm for the old behaviour.

**The general rule, and it is the one phase C should have started with:** redirected
emission is right for CONTENT, whose only consumer is our renderer. It is wrong for any
packet whose consumer is the title itself. Those must be emitted where their reader
lives. The test for a hooked entry is not "does it emit?" but "who reads what it
emits?".

**THE NEW BLOCKER, localised but not fixed.** The boot now parks a little later, at
`models\zombies.big`, with the engine thread at 99% CPU in `sub_82846210`'s
`while ([dev+0x2B04] != 0)` spin — the "wait for every outstanding async segment to
drain" loop. `dev+0x2B04` is incremented only in `sub_82845AC0` (`+= r7`, and only
`sub_8284B9C0` at `8284BB44` ever passes `r7 = 1`); a whole-image scan finds no
instruction that decrements it. A hardware watchpoint on the control arm named the
decrementer in one hit: **`sub_8284A960`, called from the token interpreter
`sub_8284B568` on the D3D worker thread** — it is the worker draining the queue, not a
GPU write. On the control arm the counter oscillates 0->1->2->1->0 continuously; on the
draw arm it never returns to 0.

Two counts from the same probe window sharpen the question. The draw arm ARMS
`sub_8284AAD0` exactly **4 times** in a boot and the ISR then delivers it thousands of
times: the mirror stays armed, so every later source-1 interrupt re-enqueues the same
job. And the draw arm submits only **28** segments to the worker token queue where the
control arm submits **13,498** — because redirected emission is exactly what empties
those segments. So the worker is being woken constantly with almost nothing to drain,
which is at least consistent with the counter never reaching zero.
`docs/d3d-phase-c3-kickoff.md` carries the ranked follow-ups.

### Phase C part 3: the counter is NEGATIVE, and the ring is replaying the hand-off

Session 15 (2026-08-06). The blocker `docs/d3d-phase-c3-kickoff.md` handed over — the
engine thread at 99% CPU in `sub_82846210`'s `while ([dev+0x2B04] != 0)` spin — is
**localised precisely and not yet fixed**. What the hand-off did not have is the value
in that word, and it changes the whole question.

**The counter is negative.** `[fence] spin- counter=4294966744`, i.e. **-552**, and
falling. The loop tests `!= 0`, so once the word goes below zero it can never exit —
this is not a wait that is taking a long time, it is a wait that has been made
impossible. Every earlier framing ("the worker is woken constantly with nothing to
drain", "reconciling that is probably the fix") is retracted: the worker drains far
MORE than was ever submitted.

**The arithmetic, from the image and then from a run.** The shared object is
`dev + 0x2AC4` — proven, not assumed, because `sub_82845AC0` locks `dev+0x2B08` and
`sub_8284A960` locks `obj+0x44`. So `obj+0x3C` = `dev+0x2B00` (the token interpreter's
nesting depth) and `obj+0x40` = `dev+0x2B04` (the counter). `sub_8284B568` does
`++[obj+0x3C]` per queue pop; `sub_8284A960` does `--[obj+0x3C]` at each `0xC0000000`
sentinel and, only when that reaches zero **and `[obj+0x48]` is clear**, `--[obj+0x40]`.
The only `+1` in the image is `sub_8284B9C0`'s middle call to `sub_82845AC0`
(`r7 = 1`); the reserve and close paths both pass `r7 = 0`.

Over one 240 s boot: **6 increments, 18,900 decrements.**

**Why the worker has so much to drain: the command processor is replaying the
hand-off block.** `CZ_PM4_MEM_WATCH` pointed at the ISR mirror's callback slot
(`BBF39470` = `[[dev+0x2A94]] + 0x10`) counts **8,152,069 writes in 200 s**:

| value written to the callback mirror | count |
|---|---|
| `0BADF00D` (the re-poison) | 4,076,035 |
| `8284AAD0` (the worker kick) | 2,717,263 |
| `82841878` (the frame tick) | 1,358,673 |
| `827CC628` / `827CC640` (job ticks) | 49 each |

The guest calls the armer `sub_82845BA0` **405 times** in that run. Everything above
405 is the ring executing the same packets again. Each replay re-arms the mirror,
raises its `INTERRUPT`, the ISR pushes another job onto the D3D worker's ring and
`KeSetEvent`s it, the worker walks the same token buffer from the start, resubmits the
same segments — and the loop closes with gain one.

Three independent measurements of the same runaway, all from the ring trace:

* through the boot movie the ring carries ~390 packets and ~48 draws per frame; from
  around frame 384 it goes to **1.25 million packets and 135,000 draws per second**
  with the `XE_SWAP` count frozen and every guest thread parked;
* `sub_828455C0`, the ring submitter, is called **106,160,000+ times** in one run,
  always from the D3D worker thread, always with `count=1`, and always cycling the
  **same three segment descriptors** — 93, 11 and 23 dwords. The 93-dword one is the
  segment that contains the `sub_8284AAD0` arm block;
* the fence-completion word freezes at a constant (`[wb+0]=00000795`) at exactly the
  frame the runaway starts, because the replayed stream keeps writing one stale
  `EVENT_WRITE` value. A fence word pinned to a constant while the emitted counter
  climbs is the signature of replay, not of a slow GPU.

`truncated=0` and the indirect-buffer verify stays clean throughout, so the parser is
right and the bytes are wrong — gotcha 88 for the third time in this project.

**What was fixed, and it is not enough.** `sub_82841AD0` — the Phase A table's
"PreSwapResolve" — **resolves nothing**. Read end to end it emits a type-0 write of
register `0x0579`, a `WAIT_REG_MEM` on the ISR mirror, the `sub_82845BA0`
arm/`INTERRUPT`/re-poison block, and a second `WAIT_REG_MEM` on the same word. No draw,
no clear, no copy, no state. It was in phase C's Redirect group because it was grouped
with the content it was NAMED after rather than with the packets it emits, and the cost
was measurable: with it redirected, **all 405 of a boot's callback armings landed at
cursor `BFBEB024`, inside the private scratch**, so the walker had to deliver them and
every walker delivery was the frame tick. That is the picture part 2 diagnosed and
fixed for `sub_82846288` alone. Four functions in the image emit that block
(`sub_82841AD0`, `sub_82846288`, `sub_82849F00`, `sub_8284B9C0`); part 2 moved one.
`CZ_D3D_REDIRECT_PRESWAP=1` is the same-binary pre-fix arm.

`sub_8284B9C0` is the second one moved this session, on the same evidence: the probe
caught all six of its calls running with `cursor=BFBEB014`, the scratch, because its
only redirected caller is Resolve. It is also the function that arms `sub_8284AAD0` and
the only `+1` the counter ever gets, so it had every reason to be on the ring.

Neither change stops the replay. Both arms — fixed and `CZ_D3D_REDIRECT_PRESWAP=1` —
run away; the pre-fix arm simply runs away harder (3.5 billion ring packets against
352 million in the same wall time).

**A hypothesis retired for the second time, on a premise that had changed.**
`CZ_PM4_STOP_ON_WAIT=1` — make the command processor stall at the arm block's trailing
`WAIT_REG_MEM` as hardware does — was retired by part 2's hand-off. That measurement
was taken while the arm blocks were in the SCRATCH, where the walker's own `0x3C`
handler never stalls and the flag could not apply to them. With the blocks now in the
ring the flag genuinely gates them, so it was re-run: **still runaway** (2.9 billion
packets, 504 million draws). It stays retired, now for a reason that survives the
change of premise. Gotcha 13 and gotcha 79, in our own notes.

**`[obj+0x48]` is not the bug — it is the tile loop, and it is behaving.** The resume
cursor `sub_8284B568` reads at depth 1 (`8284B650`) is written at `8284B544`, inside
`sub_8284B228`, when a token's second dword ANDs nonzero against the current bin mask at
`[obj+0x164]`: `[obj+0x48] = cursor` and the walk jumps to an inline stub at `obj+0x54`.
That is gotcha 118's two-tile rendering — **the worker walks one token stream once per
tile, resuming where the previous tile stopped** — and it is exactly why the counter's
decrement is guarded by `[obj+0x48] == 0`: only the LAST tile's walk retires the
segment. Our drains alternate nonzero/zero in pairs, one decrement per pair, which is
the design working. The decrement RATE per walk is right; the number of walks is not.

**Where the next session starts.** The loop has gain one and needs no seed: a segment
that both contains an arm block and reaches the worker's token stream regenerates its
own wake-up forever, because `sub_8284AAD0` pushes the same token-buffer pointer again
and a walk with `[obj+0x48] == 0` RESTARTS at `buffer+4` instead of advancing. Hardware
runs the same code and does not loop, so exactly one link differs. The two candidates,
and both are cheap to measure:

1. **The re-poison is landing but the mirror is being re-armed by the replay** — in
   which case the first duplicate ISR delivery is the seed, and finding it is a matter
   of counting `[fence] arm` against `[fence] isr` per callback through the movie era,
   where the counts are still small and the log still readable.
2. **The queued segment should not contain the arm block at all.** On hardware the arm
   is what WAKES the worker; the segments the worker then submits should be the ones
   AFTER it. If our reserve service's close/kick draws the segment boundary in the wrong
   place — it restores the real cursor and runs the guest's own `sub_82845F68`, but the
   content that would normally have separated the arm from the boundary was redirected
   away — then the arm ends up inside the very segment its own wake-up resubmits. The
   `[fence] close` line already prints `seg` and `cursor` with SCRATCH labels; what it
   needs is the arm cursor compared against the segment extent, which is one more field.

Hypothesis 2 is the one that fits phase C's own rule: redirected emission does not just
move packets, it moves BOUNDARIES, and a boundary is read by the title too.

**D — retire or keep.** Measure both arms; the PM4 path stays as the control until the
D3D arm is strictly better on every gate.

### Phase C part 4: the replay is a CONSEQUENCE, and the one thing that never stalls

Session 16 (2026-08-06). Part 3 handed over two ranked candidates for the seed of the
ring runaway. **Candidate 2 — "the queued segment should not contain the arm block at
all" — is retired by measurement, and it was not a close call.** Candidate 1 is
retired too, in the sense that the duplicate ISR deliveries are real but are an effect
rather than a cause. What replaced them is a chain that runs the other way round.

**The instrument that settled it.** Three fields, all on both arms, all cheap:

* `[fence] submit` now prints the fork's actual inputs — `incr` (r7, the counter
  delta) and `queue` (r8, which token stream) alongside the cursor. Part 3 reasoned
  about which submission carried the `+1` without ever printing it.
* `[fence] kick` is a new hook on **`sub_8284AAD0` itself** — the ISR callback that
  pushes a token-buffer pointer onto the D3D worker's job ring. Its argument is the
  identity of the stream about to be walked, and it flags a kick carrying the same
  pointer as the previous one.
* `[fence] ringsub` now prints **every entry** of the first `CZ_FENCE_RINGSUB` (default
  4000) submissions, not just the first two dwords of the first eight calls. A
  submission list is the only place a replayed segment states its own address.

**Candidate 2 is wrong: the control arm queues the arm block every single frame.**
`sub_8284B9C0` writes the callback arm at `r28 - 4` and then submits `[r28, armEnd+4)`
as the segment — the arm block is inside its own segment *by construction*, and both
arms show it. What the fork decides is only whether that segment goes straight to the
ring or into a token stream, and the control arm queues it (`2B04=1 -> WORKER TOKEN
QUEUE`) on 5,696 of its 5,698 frames without ever looping. No boundary is being drawn
in the wrong place.

**What the two arms actually differ in, in one table.** Same probe, same era, one boot
each (150 s), counted over the window in which `ringsub` is verbose:

| | PM4 control arm | phase C draw arm |
|---|---|---|
| `[fence] arm cb=8284AAD0` | 768 | 12 |
| `[fence] isr cb=8284AAD0` | 766 | 856 |
| ratio | **1.003** | **71** |
| arm-carrying segments submitted to the ring | 853 | 1,149 |
| whole boot: `incr=1` submits / drains | 3,958 / 7,913 | 6 / 132,545 |

The control arm's whole-boot figures are the shape of a healthy design: **one `+1` per
frame, exactly two worker walks per frame, one decrement per pair.** `dev+0x2B04`
oscillates 0/1/2 forever. The draw arm takes six increments and a hundred and thirty
thousand decrements.

**The chain, stated once and in the right direction.** Every kick walks the token
buffer it was handed from `buffer + 4`, and the walk resubmits that frame's segments —
including the arm-carrying ones — so *both* arms produce roughly one further interrupt
per arm segment walked. The loop therefore exists on hardware too, and it is not a
loop: it is a **pipeline**. Each frame the guest arms with a NEW token buffer, so the
kicks generated by frame N's segments arrive carrying frame N+1's pointer and drive
frame N+1's walks. Gain one, and it converges because the pointer keeps moving.

It only becomes a runaway when the pointer stops moving. On the draw arm the guest
stalls, the kicks keep arriving carrying the same two buffers (`DC645600` and
`DC68D500` — the whole boot has four armings and they cycle between two arguments),
each walk regenerates its own wake-up, and the drains that walk produces have no
matching increments. So:

> the guest stalling makes the counter go negative, and the negative counter is what
> keeps the guest stalled. The replay is the flywheel, not the fault.

Part 3 quoted the replay's own numbers correctly and read them as the cause. They are
the symptom of a pipeline whose producer stopped.

**The one link in that pipeline our runtime does not have.** On hardware the command
processor **stalls** at the `WAIT_REG_MEM` packets the hand-off block is built out of;
that stall is what stops the CP from getting more than one frame ahead of the CPU, and
it is the only brake in the design. `gpu/pm4.cpp` has never had it:

```c
if (g_stopOnWait && depth == 0)   // <-- the whole of it
    return 0;
```

**`CZ_PM4_STOP_ON_WAIT` was gated on `depth == 0`, and every one of this title's
hand-off waits is at depth ≥ 1**, because command segments reach the ring as
`INDIRECT_BUFFER` packets. The flag has been measured and retired twice — part 2 on
the grounds that the blocks were in the scratch, part 3 on the grounds that they had
moved to the ring — and on both occasions it was a no-op for the packets in question.
Gotcha 148 in its sharper form: a hypothesis retired against a code path that
structurally excluded it.

Making it work below the ring is not free, because unwinding to the ring and retrying
re-walks the indirect buffer from the start, re-executing the arm and its `INTERRUPT` —
exactly the duplication the stall exists to prevent. So the stall now records, per
depth, the buffer it stopped in and the dword it stopped at (`StallPlan` in
`gpu/pm4.cpp`), the `INDIRECT_BUFFER` handler propagates the stop up to the ring
cursor, and the next tick's walk of that same buffer resumes at the recorded position.
A deliberate stall is explicitly not counted as a truncation, or `truncated=` — the one
live gate finding 39 left behind — would sit permanently nonzero.

**And with it working, both arms deadlock at the same packet.** That is a result, not a
disappointment, and it is the first thing this port has learned about that wait:

```
[pm4] WAIT_REG_MEM #1 not satisfied, STOPPING: mem 1BF39466 value=00000001
      mask=FFFFFFFF ref=00000000 func=3
```

`1BF39466` is `mirror + 4` with the endian code `2` in its low bits — i.e.
**SCRATCH_REG1, register `0x0579`, the one `sub_82841AD0` sets to 1 at the head of its
hand-off block** — and the wait holds until it reads back zero. Nothing in our runtime
ever writes that zero, on either arm, so the ring parks at frame 1 and the boot dies at
`boot.bct` (file #5). The control arm behaves identically, which is what makes this a
statement about the runtime rather than about phase C.

So the flag stays off by default, for the reason its own comment predicted in phase 1 —
but it is now a working instrument instead of a dead one, and it has named the missing
piece precisely: **who clears SCRATCH_REG1?** Either a later packet in the same stream
(in which case our `WAIT_REG_MEM` decode of that packet is wrong, because the design
would deadlock on hardware too) or the CPU, on a path we do not currently reach. That
is one question with two arms, and it is the first thing part 5 should answer.

**Where part 5 starts.** In order, cheapest first:

1. **Find the writer of `0x0579`/`mirror+4`.** `CZ_PM4_MEM_WATCH` pointed at
   `BBF39464` on the CONTROL arm, in a healthy boot, answers it in one run — the same
   trick that named the counter's decrementer (gotcha 143). If the writer is a packet,
   read our decode of it; if it is the CPU, find the guest function and ask what
   condition reaches it.
2. Only then re-run `CZ_PM4_STOP_ON_WAIT=1`. With the brake actually able to engage,
   the question "does a CP that cannot run ahead stop the runaway?" becomes askable for
   the first time.
3. The fallback, if the brake cannot be made to work: the guest stalls because
   `sub_82846210` spins on `!= 0` against a counter that is **negative**, and the
   decrements are over-counted rather than the increments under-counted. Every
   decrement comes from a walk of a stream that had already been retired. A walk that
   finds its stream's segments already submitted is doing no work, and the count of
   such walks is measurable directly from `[fence] kick`'s SAME-AS-PREVIOUS flag.

## Standing discipline, unchanged

Both arms in the same binary; a rate, never a single run; the animated title screen is
one sample per frame (gotcha 133); every picture claim through `frame_signature.py` or
an operator's eyes; the kernel gates run, not assumed. The known boot crash
(`XexGetModuleSection('Digest')` → the title's own assert, `deadrising/asserts.php`) is
orthogonal to the pivot and stays parked by the operator's explicit call.
