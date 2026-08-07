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

### Phase C part 5: the CP's brake was never the missing piece — the DISPLAY CONTROLLER was

Session 17 (2026-08-06). Part 4 left one question: **who writes the zero the hand-off
block's `WAIT_REG_MEM` waits for?** It has a clean answer, it is not in the ring, and
finding it turned up a whole subsystem this runtime has never had.

**The measurement that settled it, and what it did NOT say.** `CZ_PM4_MEM_WATCH=BBF39464`
on the healthy control arm, one 100 s boot: **3,089 writes, every one of them the value
`00000001`, every one from the PM4 stream.** No packet ever writes zero. Since that watch
only sees GPU stores, the answer had to be the CPU — and the CPU's store is four
instructions away from where part 4 stopped reading:

```
82841760  sub_82841760:                       ; the swap-queue walker
828417E0      lwz  r11,0x2a94(r31)            ;   the ISR mirror
828417E4      stw  r9,4(r11)                  ;   r9 = 0        <-- the zero
...
828419FC      lwz  r11,0x2a94(r31)            ; sub_82841878, the same store
82841A04      stw  r10,4(r11)                 ;   r10 = 0
```

**`sub_82841760` has exactly one caller, and it is behind an MMIO read we never
satisfy.** The graphics ISR's *vblank* path (`sub_82844D38`, source 0):

```
82844DAC  lis     r11,0x7FC8
82844DB0  lwz     r11,0x6544(r11)     ; the display controller's gate
82844DB4  clrlwi. r11,r11,31          ; bit 0
82844DB8  beq     <skip>
82844DC0  bl      sub_82841760
```

A scan of every `lis rX,0x7FC8` in the image and the load/store that follows it gives
this title's **entire** GPU MMIO surface — four registers, and only one of them read:

| address | R/W | what | site |
|---|---|---|---|
| `0x7FC80714` | W | `CP_RB_WPTR`, the ring kick | `sub_82845698` |
| `0x7FC83214` / `0x7FC83408` | W | engine enable (7 / 0x800) | `sub_8284C770`, VdInitializeEngines' own callback |
| `0x7FC86110` | W | `D1GRPH_PRIMARY_SURFACE_ADDRESS`, the scanout flip | `sub_82841760`, `sub_82841878` |
| `0x7FC86544` | **R** | the display controller's gate, bit 0 | `sub_82844D38` **only** |

So the runtime's whole MMIO *read* surface is one bit, and that bit decides whether an
entire subsystem runs. `kernel/heap.cpp` withholds the aperture from the allocator but
nothing writes into it, so it read zero for the life of every process this port has ever
started. **Fable 2 hit the identical gate at the identical address** (its findings 48 and
57) and ended up asserting it by default; this is the same XDK driver, and the offsets
below are the same structure at Case Zero's addresses.

**What was behind the gate: a swap queue, read out of the title's own two functions.**

| field | meaning |
|---|---|
| `dev+0x4174` | the vblank tick, `++` per walker run — the clock every due-time is measured against |
| `dev+0x417C` | the running "next due tick" the scheduler hands out |
| `dev+0x4188` | records retired |
| `dev+0x418C` | 16 records of `{surface address, due tick}` |
| `dev+0x420C` / `dev+0x4210` | head / tail, free-running counters masked `& 15` |

`sub_82841878` — which our ISR trace has printed as `SCRATCH_REG4=82841878` since phase
1, i.e. **the callback the hand-off blocks arm** — schedules one record per present. Due
now, or the queue empty: act immediately. A record's surface is either an address, which
goes to `D1GRPH_PRIMARY_SURFACE_ADDRESS` (the flip), **or zero, which means "nothing to
scan out, just release the GPU" and IS the `[mirror+4] = 0` store.** The walker
`sub_82841760` does the same for records that come due later. Neither can run without the
gate.

**Measured, same binary, arms alternated, `CZ_NO_VBLANK_GATE=1` as the control**
(`CZ_SWAPQ_TRACE=1`, one 100 s control-arm boot each):

| | gate OFF (the whole port to date) | gate ON |
|---|---|---|
| vblank tick `dev+0x4174` after 30 s | **0** | 1,860 (62/s, the pump's cadence) |
| records retired `dev+0x4188` | 1 | 27 |
| head / tail | 0 / 1,540 | 26 / 1,074 |
| `[mirror+4]` in steady state | 1 | 1 |

The control arm is Fable 2's finding 57 verbatim: **a queue that only ever grows, with
nothing on the other end.** The gate demonstrably starts the walker. What it does not do
on its own is drain the queue, because head still stops early and tail still runs away —
and the reason for that is the other half of the same design.

**The brake and the release are ONE mechanism, and we had neither.** On hardware the
command processor stalls at the hand-off block's `WAIT_REG_MEM` until the CPU's vblank
handler releases it; that is the whole of this title's frame pacing. Part 4 built the
stall (`CZ_PM4_STOP_ON_WAIT` at any depth, with a resume plan) and found it parked at
frame 1 forever. With the gate, the same flag behaves completely differently:

| `CZ_PM4_STOP_ON_WAIT=1`, gate ON | PM4 control arm | phase C draw arm |
|---|---|---|
| stalls on `mirror+4 == 0` | 5, **every one released** | 4, every one released |
| swap queue at the end | head = tail = 5, `[mirror+4] = 0` | head = tail = 4, `[mirror+4] = 0` |
| then parks permanently on | `mem 1BF39462 value=00000010 ref=0` | the same packet, the same value |

The queue is *healthy* under the brake — head equals tail, nothing accumulates, the
rendezvous word sits at zero — which is the first time this port has seen that. The
title is being paced instead of free-running. And then both arms stop at the same place,
which is what makes the next finding a statement about the runtime rather than about
phase C.

**The new blocker, named precisely: the interrupt is addressed to a SET of hardware
threads.** `1BF39462` is `mirror+0`, and that word is a **six-bit per-CPU acknowledge
bitmap**. The arm block's first packet is `SCRATCH_REG0 = mask`; the ISR clears
`1 << PCR[0x10C]` out of it under the `dev+0x2A98` lock (82844D88-82844D98); the arm
block's trailing `WAIT_REG_MEM` holds the CP until the whole word reads zero. One
delivery acknowledges one bit.

`sub_82845BA0` takes the mask as `(flags >> 8) & 0x3F` and **defaults to 4** — bit 2,
CPU 2, which is exactly where `vd.cpp` has always run the graphics pump, so the default
case has been right by accident since phase 1. But `sub_827D2FC0` arms with flags
`0x1000`, i.e. **mask `0x10`, CPU 4**, and our one pump thread can never clear that bit.
The ISR body is per-CPU too: `sub_8284AAD0` pushes the token buffer onto the job ring at
`dev + cpu*0x6C + 0x2C40`, so *which* CPU takes the interrupt also decides which D3D
worker sees the kick — a fact phase C has been reasoning around without knowing it
existed.

**The fix, and it is the honest one rather than a fudge.** We have one guest thread for
the graphics interrupt and are not going to grow six, so that thread takes the interrupt
**once per CPU named in the mask**, reporting each CPU in `PCR+0x10C` for the duration
and restoring its own afterwards (`DeliverCommandProcessorInterrupt` in `gpu/vd.cpp`).
Every CPU in the mask runs the ISR on hardware; this is that, serialised.
`CZ_ISR_SINGLE_CPU=1` is the same-binary control arm.

**Measured, same binary, one 100 s control-arm boot each, brake ON:**

| `CZ_PM4_STOP_ON_WAIT=1`, gate ON | `CZ_ISR_SINGLE_CPU=1` (pre-fix) | per-CPU (default) |
|---|---|---|
| unsatisfied waits, by kind | 6 × `mirror+4`, then **stuck** on `mirror+0` | 8 × `mirror+4`, **all released** |
| `ack[mirror+0]` at the end | `00000010` | `00000000` |
| per-CPU deliveries | 0 | 387 |
| `XE_SWAP` frames | **7, frozen** | 217 and climbing, ~21/s |
| swap queue | head = tail = 6, dead | head = tail = 216, advancing one record per frame |
| deepest file | — | `prologue_z01.big`, the title screen |
| `truncated=` | — | **0** |

So with all three pieces in place — the display-controller gate, the per-CPU
acknowledge, and part 4's stall-with-resume — **the command processor runs this title's
real GPU/CPU hand-off protocol end to end for the first time in this port, paced by the
guest rather than free-running, and boots to the title screen.** That is one run, not a
rate (gotchas 50-51), which is why `CZ_PM4_STOP_ON_WAIT` stays off by default; promoting
it is a measurement, and it is the first thing part 6 should make.

**And the phase C draw arm, with the brake on, stops running away.** Part 3's ring
runaway was 1.25 M packets and ~135,000 draws a second with `XE_SWAP` frozen. The same
arm, same 100 s, brake on, per-CPU acknowledge on: **306,288 packets, 46,560 draws, 725
`XE_SWAP` frames, head 724 against tail 725, `ack[mirror+0] = 0`, `truncated = 0`,
deepest file `models\zombies.big`** — the draw arm's long-standing depth, reached with a
sane packet rate instead of a spinning one. Part 4 predicted the flywheel would collapse
once the CP could not outrun the CPU; this is the first measurement consistent with that,
and it is still one run per arm.

**The caveat, stated because it is a real cost and not a footnote: with the brake OFF,
the per-CPU acknowledge makes the draw arm's runaway spin HARDER.** Same 100 s, gate on,
default flags, draw arm:

| | single-CPU delivery | per-CPU delivery |
|---|---|---|
| `XE_SWAP` frames | 1,745 | **2,856,448** |
| packets | 749.6 M | 1,191 M |
| interrupts | 6.75 M | 11.4 M |

That is not a new fault, it is the SAME flywheel with more gain: each interrupt now runs
`sub_8284AAD0` once per named CPU, so each turn of the loop generates several worker
kicks instead of one. The control arm is untouched by the change (3,091 vs 3,088 frames,
94.7 M vs 95.5 M packets over 100 s — noise), and with the brake on the draw arm is
healthy. The per-CPU acknowledge stays on by default because it is *correct* — an arm
naming CPU 4 is unacknowledgeable without it — but the pairing is now explicit: it is
the brake that makes the draw arm's default configuration liveable, and part 6 should
promote the brake before anyone reads a default-flags draw-arm number again.

**Gates, this binary, default flags, both arms:** `--smoke` OK; A1 = **exact 84-prefix**
on the control arm / position-71 divergence on the draw arm (the long-known
scheduling-sensitive window, gotcha 86); A5 = **exit 0, 0 real windows, on both**.
Unchanged from the phase C best.

**Two method notes.**

* *The gate is a READ, and a read has no write to grep for.* Every previous hunt in this
  project has started from a store — the `CP_RB_WPTR` kick, the counter's decrementer,
  the callback armer. `0x7FC86544` is the one address in this title's whole MMIO surface
  that is only ever **loaded**, so no writer-scan could find it and no capture logs it.
  What found it was scanning for the *aperture base* (`lis rX,0x7FC8`) and enumerating
  everything the title does with it — five instructions in the entire 8.8 MB image.
* *The previous port had already solved it.* Fable 2's `docs/runtime.md` names
  `0x7FC86544` and `0x7FC86110` by their AMD Avivo identities and records the same
  "queue grows, nothing retires" measurement. Two sessions of phase C reasoned about the
  hand-off's *packets* without either port's notes being consulted for the *register*.
  The transferable form: when a hand-off has a CPU side you cannot find, check what the
  other ports did about the display controller.

### Phase C part 6: the brake is PROMOTED — and three of the numbers it was to be judged on could not have judged it

Session 18 (2026-08-06). Part 5 ran the brake once per arm, said so, and left the
promotion to part 6 as a measurement. It is made, the brake is on by default, and the
more transferable half of the session is what building the harness for it found.

**Three of the four columns the decision would have been scored on were broken, and
each in a different way.**

* **The deepest file was a CAP, not a depth.** `NtCreateFile` printed only the first
  64 successful opens and a boot-to-title opens exactly 64, so the log fell silent AT
  the depth every claim in this project is scored on. "The boot opens 64 files" and
  "deepest file `prologue_z01.big`" — quoted since finding 37 and in the part 5 tables
  above — were statements about the printer. With the cap at 512 the same boot opens
  **84** and ends at `#83 game:\data\skeleton\cinezombie.big`, having passed through
  `cinematics.big` and `700_prologue_intro.big`. Gotcha 109 named this trap and the
  emitter it was written about went unraised for four sessions.
* **The stall counts were the running index of a capped print.** Both stall sites print
  the first few and then one in 65,536 / 1,048,576, so "#4" means "at least 4, fewer
  than a million". A healthy paced ring and a ring parked forever both report `#4` and
  then fall silent.
* **The number that actually decides the promotion did not exist.** The only real risk
  of making the brake default is a wait nothing ever satisfies, and that state is
  invisible: `truncated=0`, a plausible frame count, `head == tail`. Gotcha 81 — the
  missing instrument is the one whose silence reads as health.

**And the replacement was wrong twice, which is the part worth copying.** The first
version counted a RELEASE whenever the next tick's stall had a different `(buffer,
dword)` identity. Two defects, both caught by a control rather than by reading:

1. It compared against `g_stallPlan` at end of tick — but the resume path CONSUMES its
   entry (zeroes `va[depth]` once it has re-entered that buffer), so a parked ring
   scored a release every tick. Running the known-parked configuration as a negative
   control (`CZ_ISR_SINGLE_CPU=1` with the brake on, which part 5 measured parking at
   frame 7) reported `released=4568` for a run that managed 6 frames.
2. Fixed, it then reported the control arm at 100% released and the phase C draw arm at
   **4.9%** — and that produced a confident finding ("the draw arm's ring is chronically
   parked") that was **retracted the same session**. The draw arm's swap queue was
   retiring one record per frame throughout. The discriminator is not stable across the
   arms: phase C re-emits its hand-off block at the SAME private-scratch address every
   frame, so a healthy re-stall is indistinguishable from being stuck, while the PM4
   arm's blocks rotate through ring addresses and every re-stall looks like a fresh
   release. Same behaviour, opposite readings.

What has no such dependency is **how many ticks IN A ROW the ring sits on one wait**.
`ring: waits unmet=N held=N streak=N max=N`, and the three states are three orders of
magnitude apart:

| | held | max streak | frames | swap queue |
|---|---|---|---|---|
| PM4 control, brake on | 1,846 | **1** | 1,846 | head == tail |
| phase C draw, brake on | 5,249 | **2** | 2,684 | head == tail |
| deliberately parked (`CZ_ISR_SINGLE_CPU=1`) | 5,498 | **5,491** | 8 | dead |
| brake off | 0 | 0 | 2,779 | 26 / 2,778 |

**The measurement: 40 runs, 10 per configuration, 120 s each, arms alternated WITHIN
each round so a busy patch of the afternoon hits all four equally** (there was one —
another port's runtime was running on the same machine for part of it).

| | frames (median) | spread | max streak | queue head==tail | deepest | crashes | truncated |
|---|---|---|---|---|---|---|---|
| PM4, brake off | 3,680 | 1x | 0 | **0 of 10** | #83 | 0 | 0 |
| **PM4, brake on** | 2,446 | **1x** | 1 | **10 of 10** | #83 | 0 | 0 |
| draw, brake off | 290,874 | **10,397x** | 0 | 3 of 10 | #60 | 0 | 0 |
| **draw, brake on** | 3,616 | **1x** | 2 | **10 of 10** | #60 | 0 | 0 |

Two things in that table are new facts about the title rather than about the brake.
**The control arm free-running overflows its flip queue in 10 of 10 runs** (head 25-29
against tail ~3,679) — the unpaced state was never healthy, it merely had no instrument
pointed at it. And **the draw arm's default configuration is BIMODAL**: 332 to 3,451,841
frames, three near-stalled runs and seven runaway. Part 5's "1,745 frames" and
"2,856,448 frames" are two modes of one distribution, not two measurements, and any
single-run claim about that arm has been sampling a coin flip.

So the brake is promoted: `CZ_PM4_NO_STOP_ON_WAIT=1` is now the control arm, and
`CZ_PM4_STOP_ON_WAIT=1` still works so older recipes keep meaning what they said. The
cost is stated rather than buried — 2,446 frames against 3,680 — and it is not a loss:
it is the title paced at its own frame timing instead of the command processor
outrunning it.

**Part 6's second question is answered, and the answer is NO.** Part 4 predicted the
draw arm's replay would collapse once the CP could not run ahead. Part 3's instruments
re-run on the current binary, 200 s per arm, none of them saturating the probe budget:

| | guest armings | ISR deliveries of `8284AAD0` | kicks per FRAME | vs control |
|---|---|---|---|---|
| PM4 control, brake on | 14,794 | 7,743 | 1.9 | 1x (gain < 1) |
| draw, brake off | 437 | 143,191 | 430 | **226x** |
| draw, brake on | 230 | 68,381 | 11.1 | **6x** |

The brake cuts the per-frame amplification by ~39x, which is large and real, and the
raw ratio of deliveries to armings is **unchanged at ~300x**. It contains the symptom
and does not touch the cause. The prediction is retired, not confirmed; whatever makes
one guest arming produce three hundred ISR deliveries on this arm is still there, and
it is where part 7 starts.

> **RETRACTED in part 7: the ~300x is not an amplification.** The draw arm's `arms`
> column stops moving seven seconds into the boot and the deliveries column does not,
> so that ratio is a stopwatch — it reads 1.8x at 8 s and 30x at 78 s on the same
> binary. Measured link by link, no step in the chain multiplies by anything: the CP
> executes each arm block exactly once (`ints/arms` = 0.9997 on the control arm) and
> one interrupt is one ISR entry. The two numbers in the row above are a frozen
> denominator, and the freeze is the finding. See part 7.

**`MirrorIsPoisoned()` is NOT retired by this, and the reason matters.** The kickoff
expected the promotion to make that poison-skip path deletable. It records **zero skips
across all 40 campaign runs — including the brake-OFF arms** — so it is already inert
independent of the brake, and crediting the promotion for it would be crediting the
wrong change. It stays until something explains why the case it was written for no
longer occurs.

**Gates, this binary, new default, both arms:** `--smoke` OK; A5 **exit 0, 0 real
windows, on both arms**; A1 an **exact 82-prefix on the draw arm** (the phase C best);
both capture oracles clean (`pm4_packet_lengths.py` 0 disagreeing over 24.5 M packets,
`pm4_indirect_walks.py` OK over 28,726 buffers); `truncated=0` in all 40 runs; zero
crashes in all 40.

**The one number that is not flat, reported because it is not flat.** A1's
position-71 window (`XamUserCheckPrivilege` vs `XexGetModuleHandle`) has been known
since phase 1 to be scheduling-sensitive on both binaries. The campaign left 20 saved
control-arm logs, so the rate is free rather than a single run (gotcha 95): it
permutes in **4 of 10 brake-on runs against 1 of 10 brake-off**. That is not
distinguishable at this sample size (Fisher two-tailed p ~= 0.30) and the mechanism
is plausible either way, since pacing the ring changes thread interleaving. It costs
nothing measurable: all 10 brake-on runs reach `#83 cinezombie.big`, A5 is exit 0
with zero real windows, and the window is a permutation of one name set rather than
a missing or extra call. Recorded so that a future session reading "4 of 10" does not
mistake a known window for a new regression — and so that if it ever climbs, there is
a number to climb from.

### Phase C part 7: the ~300x was a STOPWATCH, and the draw arm's stall is ONE event at the first tiled frame

Session 19 (2026-08-06). Part 6 handed over one open question — "what does one
delivery do that makes the next one happen, to the tune of three hundred?" — and the
answer is that nothing does. There is no 300x link anywhere in the chain. The figure
was a ratio between a counter that keeps running and a counter that stopped.

**The instrument, and why the old numbers could not have settled it.** Part 6's two
amplification figures were grepped out of `CZ_FENCE_PROBE`, which is a LINE budget: a
count taken from a budgeted printer is a floor unless someone goes back and checks it
did not saturate (gotcha 109), and the two halves of the ratio came from two different
hooks' prints. `cpu/chain_stats.h` replaces that with unconditional relaxed atomics on
hooks that already existed, so the whole chain is on **every** run, free, and on one
line:

```
ring: chain arms=N ints=N isr=N kicks=N (distinct=N repeat=N) walks=N drains=N
           segsub=N/queued=N ringsub=N/ents=N
ring: engine counter[dev+2B04]=%d depth[dev+2B00]=%d
```

Six links, each of which could multiply: the guest ARMS a callback (`sub_82845BA0`);
the command processor reaches the arm block's INTERRUPT packet (`ints`); the runtime
runs the guest ISR, once per CPU named in the arm's mask (`isr`); the ISR's callback
KICKS the D3D worker with a token-buffer pointer (`sub_8284AAD0`); the worker WALKS
that buffer (`sub_8284B568`); the walk RESUBMITS that stream's segments to the ring
(`sub_828455C0`), which is where the next arm block comes from. `distinct` is how many
different token-buffer pointers the run has ever kicked with — part 4 established that
this loop converges only because that pointer advances, so it is the number that
separates a pipeline from a runaway (gotcha 150).

**Measured, 173 s, same binary, both arms, default flags.**

| | arms | ints | isr | kicks | distinct | walks | drains |
|---|---|---|---|---|---|---|---|
| PM4 control | 13,676 | 13,672 | 13,672 | 7,151 | 545 | 7,151 | 7,151 |
| phase C draw | **227, frozen at t = 7 s** | 6,903 | 6,903 | 4,486 | **2** | 4,486 | 4,486 |

The control arm's every ratio is one or a constant: `ints/arms` = **0.9997** (the CP
executes each arm block exactly once), `isr/ints` = **1.000** (this title's masks name
one CPU, so part 5's per-CPU acknowledge is not a multiplier here), `kicks/isr` =
**0.523** (about half the deliveries have `sub_8284AAD0` armed and the rest the
swap-queue scheduler `sub_82841878`), and `walks == kicks == drains` to the unit.

**So part 6's "~300x, unchanged by the brake" is retired.** On the draw arm the
denominator STOPS and the numerator does not, so the ratio is a function of how long
you ran: 1.8x at 8 s, 10x at 35 s, 30x at 78 s, and part 6's 300x was a 200 s run of a
boot that had frozen in the first ten seconds of it. Part 6 also read the same shape
without seeing it — its own table has the draw arm at 437 and 230 armings against a
control arm's 14,794, which is the freeze stated in the numerator's absence.

**Which makes part 7's item 2 the only question, and it is a much sharper one than
"the draw arm stops at #60".** The chain line's first seven ticks say the stall is a
single event, not a decay:

```
arms=217 ints=217 isr=217 kicks=0 (distinct=0) walks=0 drains=0 segsub=986/queued=0
arms=227 ints=309 isr=309 kicks=62 (distinct=2) walks=62 drains=62 segsub=1044/queued=28
```

For the whole healthy era the D3D worker is **never used at all** — zero kicks, zero
walks, every segment submitted straight to the ring because `dev+0x2B04` is zero at
every one of 986 submits. Then within one tick the worker engages for the first time
and the guest stops arming, permanently. The control arm has the identical transition
at the identical era (tick 7: kicks 0 -> 10, queued 0 -> 44) and survives it.

**What that transition IS: the first tiled frame.** The era is `models\zombies.big`,
and the code is Resolve's MULTI-TILE path, taken from the moment `dev+0x327C` exceeds
1 — i.e. from the moment the title starts rendering its scene as two 640-wide tiles
(gotcha 118). That path is the only site in the image that does all three things at
once, at `sub_82838858 + 0x23C..+0x284`:

```
82838A50  lwz  r11,0x3500(r31)      ; the head of the D3D worker's token stream
82838A94  bl   sub_82846288         ; ARM sub_8284AAD0 with that stream as argument
82838AA8  bl   sub_82845F68         ; the reserve = CLOSE AND KICK
82838AD0  stw  r11,0(r3)            ; 0x88000000 — queue that segment to the worker
```

**Where the engine is stuck.** The last thing it does is the per-frame GPU sync
(`sub_82845230` -> `sub_82845160`), and it never returns:

```
[d3d] sync-wait #166 target=1035 emitted=1039 completed=1019
[d3d] sync-wait #167 target=1039 emitted=1043 completed=1019     <-- never returns
```

Reproduced on a second run at `target=1037 completed=1017`, and that run had printed
`completed=1023` at the call before it. **The completion word goes BACKWARDS.**

**And it is not lagging, it is on a carousel.** `CZ_PM4_MEM_WATCH` on the fence
writeback word, draw arm, 100 s — 26,017 GPU stores, and the last 4,000 of them are:

```
440 <- 000002EF   440 <- 000002ED   440 <- 000002EB   440 <- 000002E9
440 <- 000002E7   440 <- 000002E5   440 <- 000002E3   440 <- 000002E1
440 <- 000002DF
```

Nine values, 440 laps, forever. The completion word climbs 735 -> 751 and resets to
735 on every lap, so a wait for anything past 751 is not slow, it is **unsatisfiable** —
and a wait for something inside the range is satisfied and then unsatisfied again.
Gotcha 146 said a fence word pinned to a constant is replay rather than latency; a
fence word that REGRESSES is the same statement with no ambiguity left in it, and it
is visible in two consecutive lines of an existing trace.

**What the command processor is replaying.** `CZ_FENCE_PROBE`'s `ringsub` entry list,
which is the only place a resubmitted segment states its own address:

| | most-resubmitted segment | its size | how many times |
|---|---|---|---|
| PM4 control | `1BF794C0` | 11 dwords | 11 |
| phase C draw | `1C65BA60` | **93 dwords** | **132** |
| | `1C613C00` | **93** | 126 |
| | `1C5CBC20` | **93** | 86 |

Ninety-three dwords is the size of the segment the multi-tile Resolve closes around its
own arm block (`[fence] close ... seg=BC5843A0 dwords=93` sits between the `arm
cb=8284AAD0` and the reserve, on every cycle). So the top three replayed segments on
the draw arm are **the arm blocks themselves**: the arm-carrying segment is inside the
token stream that its own interrupt tells the worker to walk. That is gotcha 147's
shape — a block ending up inside the segment its own wake-up resubmits — and the
control arm's worst case is an order of magnitude smaller.

**The two-cycle sequence, out of one log, in order.** This is the whole mechanism and
it takes two turns to close:

1. `arm cb=8284AAD0 arg=DC5C3B00` (real ring), reserve, `fsubmit tiles=2`, then
   `submit ... incr=1 ... 2B04=0 -> ring direct`. The counter was zero, so the
   arm-carrying segment went straight to the ring. Nothing is wrong yet.
2. The ISR delivers `cb=8284AAD0 arg=DC5C3B00`, the worker walks it, and
   `ringsub #724 ents: 1C5843A0/93` **resubmits the 93-dword arm segment**.
3. Its drain reads `depth=1 counter=1 48=DC5C3C14`. `sub_8284A960` decrements the
   counter only when the nesting depth reaches zero AND `[obj+0x48]` is clear, and it
   is not — so the `+1` from step 1 stays.
4. Next cycle: `arm cb=8284AAD0 arg=DC60BA00`, and now `submit ... incr=1 ... 2B04=1
   -> WORKER TOKEN QUEUE`. **The arm block is now inside a token stream**, which is
   exactly the state part 4 named ("it is only when the counter is ALREADY nonzero at
   the incr=1 submit that the arm lands in a token stream at all, and from there the
   loop closes"). The pointer advances once more and then never again.

**Two things that LOOK like the divergence and are not, both killed by running the
same probe on the control arm.** Writing them down because each had a paragraph
drafted against it before the comparison was run:

* *"Half the draw arm's drains decline to decrement, so `[obj+0x48]` is the bug."* The
  control arm's split is **1,732 non-null against 1,731 null** and the draw arm's is
  **3,576 against 3,575**. It is 50/50 on both arms; a resume-pointer set at every
  other drain is simply how this interpreter works.
* *"The draw arm gets six increments where the control arm gets 1,873, so the increment
  side is starved."* Over 100 s that is 1.0 per frame on the control arm and **3.0 per
  TILED frame** on the draw arm, which got two of them. Same trap as the ~300x this
  section opens with, one screen further down: a per-run total compared against a run
  that stopped.

What survives both is that **nothing on the guest side is abnormal until the freeze**.
The counter reaching -2,731 and part 3's negative-counter finding are downstream: 3,575
decrements against 6 increments is what a boot that stopped producing frames looks like
after ninety seconds of a loop that keeps consuming.

**So the mechanism, stated once and in the direction the evidence runs:**

> The first multi-tile Resolve puts an arm block inside a segment that reaches the D3D
> worker's token stream. Every walk of that stream resubmits the segment; every
> resubmission makes the command processor execute the arm block's INTERRUPT again;
> every interrupt makes the ISR kick the worker with the same token buffer. Gain
> exactly one, and the same on the control arm — but the ring is now saturated with
> that one segment, so the segments the engine emits AFTER it are never executed. Its
> next fence wait is for a number no packet the CP reaches will ever write. (The
> replayed segment's own `EVENT_WRITE`s rewriting the word with stale values is the
> visible symptom, and item 2 below shows it is not the blocker.) The engine stops, the
> token-buffer pointer stops advancing with it, and the loop loses its only exit.

The control arm runs the same loop and leaves it: its worst-replayed segment is
resubmitted **11** times against the draw arm's **132 and climbing**, because a guest
that keeps producing frames supersedes each token stream before the replay matters.

**Where part 8 starts.** The target is now one property rather than a subsystem:

1. **The arm block and its `INTERRUPT` must not be inside a segment the worker
   resubmits.** That is gotcha 147 restated as a requirement, and it is the one link
   in the chain above that our runtime has any say over — parts 2, 3 and 5 moved four
   emitters between streams on exactly this reasoning, and this is the fifth and the
   one that matters. Whether the segment boundary can be drawn to exclude it, and
   whether the guest's own `sub_82845F68` can be made to close it before Resolve
   appends the `0x88000000` token, is the design question.
2. ~~Or the replay must not be able to move the fence word backwards.~~ **Run, and it
   is a NEGATIVE result — which sharpens the diagnosis rather than costing a session.**
   `CZ_PM4_FENCE_MONOTONIC=1` refuses any GPU store that moves the completion word
   backwards, so the word latches at the top of its carousel instead of resetting. It
   engages hard — **5,711 refusals in 90 s**, so this is not gotcha 151's dead flag —
   and the boot freezes in exactly the same place, `arms` pinned at 190, `distinct=2`,
   `#60 models\zombies.big`. So the regression is NOT what blocks the wait. The wait
   is for a fence beyond the top of the carousel, and the segments carrying those
   `EVENT_WRITE`s are never executed at all, because the ring is saturated with the
   replayed arm segment. **The missing execution is the fault; the regressing word was
   only the most visible symptom of it.** The flag stays as an arm, off by default and
   announcing itself, because it is the cheapest way to re-ask this question after any
   change to the segment routing.
3. Only then re-ask the depth question. **`#60 models\zombies.big` is not a
   file-loading depth at all** — it is the frame at which the title first renders in
   two tiles, and every draw-arm run stops there because that is where the mechanism
   above engages. Any future note quoting it as "how far the draw arm loads" is
   quoting a symptom.

### Phase C part 9: the picture — four defects between the scene and the screen

Session 21 (2026-08-06/07). `docs/d3d-phase-c9-kickoff.md` handed over one item at the
top of its list: *the 3D background and the DEAD RISING 2 wordmark are black on BOTH
arms, so that is a phase-5 renderer gap the pivot neither caused nor fixed.* It was, and
the kickoff's advice to work it on the **PM4 control arm** was right — everything below
was found there in a binary that is also the draw arm.

Full technical record in `docs/phase5-notes.md` §§6s-6u. The short form, in the order
each one hid the next:

**0. The title screen is TWO screens, and every claim about it for three phases was a
single sample of whichever one the dump caught.** Measuring all 32 frames of a 170 s
boot instead of looking at one: 49 frames in ~1000 carry the DEAD RISING 2 CASE ZERO
logo (a near-exact match for capture E2, fading in and out over a 49-frame pulse) and
the rest carry PRESS START on black. Capture **E3** says the second era is the animated
**Still Creek** 3D background. So the logo era was already correct — E2's background is
black anyway — and it was the Still Creek era that rendered as nothing.

**1. One stale texture-cache entry composed the scene away.** `UploadTexture` consulted
the fetch-constant cache BEFORE the resolve-snapshot check, so the rule the code already
stated — *a snapshot is deliberately not cached, its contents change every frame while
its fetch constant does not* — only held for a surface whose FIRST fetch already had a
snapshot. This title's colour-grading LUT is resolved late in a frame and sampled early
in the next, so its first fetch during the boot fell through to guest memory, uploaded
whatever the allocator had left, and cached it under a fetch constant that never changed
again. The tone map's last instructions are two LUT lookups blended: a black LUT is a
black frame. Checking the snapshot first took the tone map's output from 0.00% non-black
to 95.3%, and the presented frame from 2.31% to 99.4%.

**2. The exploded geometry was DRAW_INDX read one dword off.** The index swizzle lives
in the TOP two bits of the SIZE dword, not the low two of the ADDRESS — and reading it
off the address also masked away address bit 1, which is real for a 2-byte-aligned
16-bit index buffer. Every draw in this title is `8-in-16`; ~40% were also being read
one index early. Fixed, the scene is a recognisable Still Creek.

**3 and 4. Half of every clear rectangle was missing, twice over.** A rectangle list's
fourth corner (`v0 + v2 - v1`) was never drawn — the expansion emitted the same triangle
twice — and these draws are the guest's per-pass CLEAR, half of them depth-only, so half
of every rect kept the previous pass's depth and rejected the scene behind it. Then, with
whole rects, the scene tile's clear still covered 320 of its 640 columns, because that
clear runs with `RB_SURFACE_INFO` declaring 4x MSAA and on Xenos 4x doubles a surface's
width: window coordinates are in PIXELS and our EDRAM is at sample resolution.

**What the arms are.** Every one of the four has a same-binary control:
`CZ_VK_TEX_CACHE_FIRST`, `CZ_PM4_INDEX_ADDR_SWIZZLE`, `CZ_VK_RECT_HALF`,
`CZ_VK_NO_MSAA_WINDOW_SCALE`. The instrument that made 3 and 4 diagnosable at all is
`CZ_VK_NO_DEPTH_TEST=1`, which separates "never submitted" from "submitted and rejected
by stale depth" — the same picture, completely different investigations.

**Gates, PM4 arm, fixed binary:** `--smoke` OK; A1 **exact 84-prefix**; A5 **exit 0, 0
real windows**; `truncated=0`; deepest file **#83 `cinezombie.big`**; no change in frames
presented (1,188 against 1,189).

**What is still wrong, named and measured (`phase5-notes` §6v).** The title screen's
LEFT half renders as a complete, bright Still Creek; the RIGHT half is nearly empty, and
it is **ME bin predication**. `gpu/pm4.cpp` has implemented
`(header & 1) && (binMask & binSelect) == 0` since phase 1 and had never counted it: a
THIRD of this title's draw packets are discarded by it, and one frame's two scene passes
execute 931 draws and 23. The tiles select bins `{0,1,31}` and `{2,3}`, and in the
`{2,3}` tile 100,000 draws a boot carry masks that can never overlap it — so either the
bins are not the left/right split we assume, or the comparison is wrong in one of three
specific places. B1 carries the same mask/select/draw stream, so replaying the rule over
the capture answers it without an emulator run. That is where part 10 starts.

A number was WITHDRAWN on the way there, because it would have sent the next session
hunting the wrong thing: "hardware issues ~2,540 draws a frame and we issue ~1,620"
compares draw PACKETS in a capture against draws the RENDERER ACCEPTED. Counted at one
instant of one run, the command processor parses 1,971 packets a frame and hands the
renderer 1,313. A mechanism with no counter cannot be subtracted from a comparison.

**Method notes worth more than the fixes.**

* The counter that could have named defect 1 read **7** on the broken binary and
  **70,681** on the fixed one, because it sits downstream of the early return that WAS
  the defect. A counter behind an early return counts the times the early return did not
  happen, and its silence is unfalsifiable from inside.
* Defect 2 retracts `phase5-notes` §6c's retirement of index endianness. That A/B was
  honest and was scored against a black frame, because defect 1 was upstream of the
  metric. **A retirement is only as good as the oracle it was measured on** — gotcha 13's
  shelf life, applied to our own earlier measurements rather than to a capture's.
* `CZ_VK_FORCE_COLORMASK=1` produced a visibly better picture and is not a fix; it was
  the animated title screen resampled (gotcha 133), and this phase has now nearly lost
  four claims to that one cause. What settled it was a counter, not a picture: splitting
  "empty colour mask" by `RB_MODECONTROL` shows the empty masks are a real Z prepass plus
  the degenerate point draws.

## Standing discipline, unchanged

Both arms in the same binary; a rate, never a single run; the animated title screen is
one sample per frame (gotcha 133); every picture claim through `frame_signature.py` or
an operator's eyes; the kernel gates run, not assumed. The known boot crash
(`XexGetModuleSection('Digest')` → the title's own assert, `deadrising/asserts.php`) is
orthogonal to the pivot and stays parked by the operator's explicit call.

---

## Phase C part 10 (2026-08-07, session 22): the right tile is not a command-processor defect

Part 9 handed over one open item — the scene's right tile executes 23 draws against the
left tile's 931, because ME bin predication discards a third of this title's draw
packets — with an explicit instruction: **replay the rule over capture B1 before
theorising, because it needs no emulator.** That was the right instruction and it
retired every hypothesis on the list.

### 1. The rule is right. Hardware discards 0.3%; we discard 33%.

`tools/xtr_bin_predication.py` replays SET_BIN_MASK/SELECT and the ME's
`(header & 1) && (mask & select) == 0` over a `.xtr`. It exists because our command
processor is the suspect in this question and therefore cannot be its own oracle — and
because Xenia writes a `PacketStart` for every packet *before* it evaluates predication,
so the capture contains the skipped packets and the rule can simply be replayed.

Over all **24,527,474** packets of B1:

| | draw packets | discarded |
|---|---|---|
| B1 (hardware) | 1,643,219 | **5,240 — 0.3%** |
| ours, one 170 s boot | 3,644,332 | **1,195,021 — 33%** |

and in the capture both tiles are offered **exactly 575,744** draws and each keeps
**573,124 — 99.5%**. Perfectly symmetric. So part 9's three named suspects — the 64-bit
LO/HI assembly, the meaning of bit 31, and a mask read one draw late — are all dead, and
so is "this title's geometry really is almost all in bins 0/1".

Any port with a GPU capture has this oracle. It is thirty minutes of work and it is the
difference between measuring the title and measuring yourself.

### 2. Where it IS wrong: one number, and it is a placeholder

Our runtime now prints the same pair table (`CZ_PM4_BIN_CENSUS=1`, on the ring trace
beside `predicated out=`):

| bin select | hardware | ours |
|---|---|---|
| `80000003` (left tile) | mask `FFFFFFFF` x570,504, all kept | `FFFFFFFF` x1,290,325, all kept |
| `0000000C` (right tile) | mask **`8000000F`** x570,504, all kept | **`80000000`** x893,687 SKIPPED |
| | | **`80000003`** x291,336 SKIPPED |
| | | `8000000F` x100,325, kept |

The left tile matches hardware's shape exactly; the right tile carries two mask values
hardware never has standing at a draw.

`CZ_PM4_BIN_TRACE` now logs the mask WRITES in stream order, not just the draws
(`CZ_PM4_BIN_TRACE_ARMMASK=8000000F` holds the budget until the mature tiled era; the
prologue is packet-identical on both sides). Same bracket, one different number:

```
hardware   MASK_LO 8000000F ; DRAW -> run ; MASK_LO 80000000 ; ... ; MASK_LO 8000000F ; DRAW -> run
ours       MASK_LO 80000000 ; MASK_LO 80000000 ; DRAW -> SKIP
```

`0x80000000` is a **literal** — the three UP-draw emitters build it with
`lis rX, -0x8000` and store it straight after the `0xC0006000` header (82842A18,
82842DE0, 8284322C). The real mask is patched in afterwards, in place, by the D3D
worker:

```
sub_8284B568 (token interpreter) -> sub_8284B228 -> sub_8284A900 -> sub_8284A7F8
```

`sub_8284A7F8` is the only rect-to-bin-mask computation in the 8.8 MB image: a list of
16-byte `{record*, x0, x1-1, y0, y1-1}` entries (the rect is in units of 8 pixels, hence
its `<<3`), intersected against the tile rects at `tileInfo+8`, accumulating
`(acc << 2) | 3` per overlapping tile, ORing in bit 31, stored at `record+8` — exactly
the dword the emitter left as `0x80000000`.

And the patch is gated on **bit 31 of `[obj+0x164]`**, with the token consumed either
way, so a clear bit is silent. Measured on the PM4 control arm, one 170 s boot,
`CZ_BINMASK_PROBE=1`: the dispatcher ran **once** with that word at `00000000`, the
patch pass ran **once** and patched **zero** records, while 1,040,207 draws executed
carrying the placeholder. Consistent with part 7's independent finding that the D3D
worker is never used at all for the whole healthy era.

**Open, and stated as open:** the same boot has 97,115 draws carrying `8000000F` and
239,063 carrying `80000003` at the right tile, and with the patch pass having touched
zero records those came from somewhere this session did not find. The placeholder story
explains the 1,040,207 and is not yet shown to explain the rest.

### 3. Item 2: the draw arm inherited parts 8 and 9, and it is in the healthy shape

Part 9 could not re-gate the phase C draw arm and said so. Re-gated now, over **six
serial 170 s boots** — serial because part 6 recorded this arm as BIMODAL (gotcha 159)
and one run of a bimodal arm is a coin flip:

| | part 8 | **now, 6 of 6** |
|---|---|---|
| deepest file | #60 `models\zombies.big` | **#83 `skeleton\cinezombie.big`** — 6 of 6 |
| `arms` : `ints` | 12 : 856 | **12,790 : 12,787 = 0.9998** |
| `isr` / `ints` | — | 1.000 |
| `walks == kicks == drains` | — | equal in all six |
| `distinct` token buffers | 2 | **816 - 911** |
| engine counter `dev+0x2B04` | -552 | **0** |
| frames (XE_SWAP) | — | 3,360 - 3,654 (**1.09x spread**) |
| A1 | 82-prefix | **exact 84-prefix** |
| A5 | exit 0, 0 real | exit 0, 0 real |
| `truncated` | 0 | 0 in all six |

`distinct` is the load-bearing column (gotcha 162): 816-911 different token buffers is a
pipeline; part 3's `distinct=2` was a replay. Part 7's stall at the first tiled frame,
part 3's negative counter and part 6's 10,397x bimodal spread are all absent — 1.09x
across six runs is not a bimodal distribution.

Nothing in this session caused that. The only runtime change here is the predication
refactor (no behavioural change) and probes that are off by default. It is parts 8 and 9
arriving on the arm that was never re-measured, which is exactly why the kickoff listed
it as item 2 and why gotcha 67 exists.

**A retraction inside this section, because the wrong version was believed for an hour.**
Mid-session this was written up as "healthy on most runs, still bimodal", on the strength
of a run that reported `#60`, `arms=241 ints=207,599`, `distinct=6` — the exact part-7
failure. That run was one of two `cz_runtime` processes started from two overlapping
background loops, i.e. two 170 s boots competing for the machine. The clean serial set is
6 of 6. Two things follow, and the second is the useful one:

* **A rate measured with another copy of the same binary running is not a rate.** This
  arm's health is decided by multi-threaded scheduling, so halving its effective CPU is
  not a neutral background load — it is an intervention on the variable under test.
  Gotcha 7 is usually quoted about probes; it applies to anything else sharing the box.
* **The old failure mode is still REACHABLE, under CPU contention.** That is worth more
  than a clean six: it means part 7's stall was never fixed, only made rare, and there is
  a cheap reproducer for it — run two draw-arm boots at once. Anyone attacking the stall
  should start there rather than waiting for it to happen.

### 4. Item 3: NOT done, deliberately

The kickoff proposed deleting the walker's dead `case 0x54:` INTERRUPT block and
`MirrorIsPoisoned()`, on the grounds that both record zero across every arm. That
evidence was gathered on a draw arm that stalled at `#60`. The arm now runs to `#83`
with a completely different chain shape, so the regime those zeros were measured in no
longer exists, and both are guards against a crash that was real. Re-measure on the
current arm before deleting; the kickoff's own condition — "a session that is not also
changing the routing" — is not met by a session in which the routing's behaviour changed
this much, even though it changed for free.

## Phase C part 11 (2026-08-07, session 23): the empty right tile was a packet we never implemented

Part 10 handed over "the mask standing at the right tile's draws is `80000000`, which is
the placeholder the draw emitters write and a fix-up pass in the D3D worker is supposed
to overwrite — and on our runtime that pass runs once, behind a closed gate, and patches
zero records", plus one explicitly open item: the 97,115 draws carrying `8000000F` and
239,063 carrying `80000003` that the placeholder story could not explain.

**The open item dissolved because the premise was wrong, and the retraction is the whole
finding.** Full record in `docs/phase5-notes.md` §6x; §6w carries the retraction in
place.

### 1. The probe was printing its first call and nothing else

`sub_8284A7F8`, the fix-up pass, runs **1,751 times a boot and patches 388,451
records**. `sub_8284B228`, the dispatcher above it, runs **3,497 times with its gate
OPEN on 3,496 of them**. Part 10's "once, closed, zero" was `if (n == 1 || n % 20000)`
against a subsystem that runs a few thousand times: the only line any run ever emitted
was the one at call #1, whose counts are all 1 by construction. That is gotcha 109 — a
capped emitter is not a count — inside our own instruments for the second time in three
sessions. **Both probes now report on a 15-second clock, and the schedule is the thing
to check first when a probe reports "1".**

`[obj+0x164]` is not a flags word with a gate bit either. It is the current **bin
select**, published by `sub_8284A6D0` from `sub_8284A668`, which sets bit 31 exactly
when the tile index is 0 — so the "gate" reads "are we recording the first tile", and
the patch running once per multi-tile recording is the design. (That also explains a
number this port has stared at for three phases: the left tile keeps every draw
regardless of its mask, because every patched mask carries bit 31 too, so
`mask & 80000003` is nonzero by construction. Only the right tile ever consults the
real bin bits.)

### 2. What the pass computed, and which of its two inputs was rubbish

| the fix-up pass wrote | records | share |
|---|---|---|
| `80000000` — touches NO tile | 294,787 | **75.9%** |
| `80000003` — tile 0 only | 58,624 | 15.1% |
| `8000000C` — tile 1 only | 2,450 | 0.6% |
| `8000000F` — both tiles | 32,590 | 8.4% |

The tile rects it intersects against are **perfect** — `tiles=2 tile0=0,0..640,720
tile1=640,0..1280,720`, printed by the probe. The per-record rects are uninitialised
memory (`50176,17408..0,0`, `0,0..8978,65535`, a dominant degenerate `0,0..0,0`).

### 3. Whose job it was to fill them: EVENT_WRITE_EXT, event 0x1A

The record is sixteen bytes — `{stream cursor, u16 minX, maxX, minY, maxY, minZ,
maxZ}`, the extent in units of 8 pixels — and **the GPU writes the extent**. Each draw
block ends `EVENT_WRITE_EXT {0x1A, &record+4} ; EVENT_WRITE {0x19}`: the Xenos screen
extent query, dumping what was rasterized since the matching `0x19` into the record so
the worker can turn it into next frame's bin mask.

Our command processor decoded that packet and did nothing, because the fence family's
handler stores only when a packet carries a value dword and this form carries an address
and none. The capture is unambiguous about the form: over B1's first 6,000,001 packets,
`EVENT_WRITE_EXT` appears **818,507 times, every one `body=2, event=0x1A`**, against
819,953 `EVENT_WRITE body=1 event=0x19`. One pair per draw block, 818,507 no-ops a boot.

Also retracted with it: `0x80000000` was never the placeholder. The placeholder is the
**leading** `SET_BIN_MASK_LO FFFFFFFF` at `record[0] + 8`; the `0x80000000` part 10
found is a deliberate trailing reset that confines the two bookkeeping packets after the
draw to the first tile's pass.

### 4. The fix, and the numbers

`WriteScreenExtent` in `gpu/pm4.cpp`, duplicated into `gpu/d3d_draw.cpp` under one
shared env arm so the two streams cannot decode a packet differently (the same rule
`CZ_PM4_INDEX_ADDR_SWIZZLE` follows). We cannot know what a draw covered, so we write
the conservative extent — larger than any tile, "this draw may have touched anything" —
which makes bin predication a no-op rather than a wrong filter. Too large costs work;
too small silently deletes geometry.

Measured, same binary, `CZ_PM4_NO_SCREEN_EXTENT=1` as the control arm:

| | control (pre-fix) | **fixed** | B1 (hardware) |
|---|---|---|---|
| fix-up pass output | 76% `80000000` | **100% `8000000F`** | — |
| draw packets discarded by the bin rule | **32.7%** | **0.28%** | **0.3%** |
| right tile (select `0000000C`) | `80000000` x1,125,183 SKIPPED | `8000000F` x974,779, kept 100% | `8000000F` x570,504, kept 100% |
| the two tiles' offered counts | 1,472,725 vs 1,466,725 | 975,698 vs 974,779 | 575,744 vs 575,744 |

0.28% against hardware's 0.3% is the first time this port's predication has agreed with
the capture. The residue is symmetric and legitimate — the per-tile clear loop at
82844180 emits `SET_BIN_MASK_LO 3 << 2i`, so each tile skips the other's clears.

### 5. Gates, both arms

| | PM4 control (`CZ_VKDRAW=1`) | phase C draw (`CZ_D3D_DRAW=1`) |
|---|---|---|
| `--smoke` | OK | OK |
| A1 | **exact 84-prefix** | **exact 84-prefix** |
| A5 (`--include-high-frequency`) | exit 0, 2 permutation windows, **0 real** | exit 0, 2 permutation, **0 real** |
| `truncated` | 0 | 0 |
| deepest file | **#83 `skeleton\cinezombie.big`** | **#83 `skeleton\cinezombie.big`** |
| predication | 10,076 of 3,731,554 = **0.27%** | 0 of 30,552 on the ring |
| chain | — | `arms=1115 ints=1114 isr=1114`, `walks==kicks==drains=476`, `distinct=191`, engine counter **0** |
| max wait hold streak | 1 | 1 |

Both capture oracles clean: `pm4_packet_lengths.py` 24,527,474 packets, 0 disagreeing;
`pm4_indirect_walks.py` 28,727 buffers, 0 disagreeing. The draw arm is in the healthy
shape part 7 defined, unchanged by this session's work.

### 6. Item 2 (the walker's dead code): still zero, and still not deleted

Part 10 declined to delete the walker's `case 0x54:` INTERRUPT block and
`MirrorIsPoisoned()` because the zeros behind that recommendation were measured on an
arm that stalled at `#60`. Re-measured on the current draw arm at `#83`: the walker's
in-position INTERRUPT delivery prints **zero** lines and the poisoned-skip counter is
**zero**. So the recommendation's evidence now holds in the current regime.

It is still not done here, for the reason gotcha 182 gives from the other side: this
session changed what executes inside both streams (818,507 previously-dead packets a
boot now write memory), which is exactly the kind of regime change that invalidates a
"this has always been zero" argument. One session that changes nothing else should
confirm the zeros and delete.

### 7. The general lesson, for Case West

**A packet we implement and a packet we implement for every FORM it takes are not the
same claim.** `EVENT_WRITE_EXT` has had a name in the opcode table since phase 1,
appears in the census, and passed both capture oracles — because
`pm4_packet_lengths.py` and `pm4_indirect_walks.py` check our arithmetic, and our
arithmetic was right (gotcha 88, third time). The cheap standing check that would have
caught it: census the capture by `(opcode, body length, event type)` rather than by
opcode, and read any row your handler falls through as a hole.
