# D3D phase C kickoff. Paste this into a fresh conversation.

`CLAUDE.md` loads automatically and is current through phases A and B of the D3D
pivot (2026-08-06, session 12). `docs/d3d-translation-plan.md` carries the Phase A
hook table (with evidence per row) and the Phase B results; this file adds what a
fresh context needs to build **Phase C: draws serviced by a host renderer**.

## Where the pivot stands

- **Phase A delivered.** The hook table lives in the plan doc. 43 hooks in
  `runtime/gpu/d3d_hooks.cpp`; `CZ_D3D_OBSERVE=1` logs and calls through, gates
  clean. `tools/guest_callers.py` answers "who calls X" from the image — use it
  before hand-walking anything.
- **Phase B delivered.** `CZ_D3D=1` no-ops the content APIs (4 draws, Clear/ClearF,
  Resolve, PreSwapResolve) while the lifecycle (Swap, fences, flushes,
  busy-tracking, init) calls through. Result: boot to title screen, 33,984 frames
  in 100 s, zero faults, **pm4_packets +0 per frame** — the title's Swap takes its
  empty-frame branch when nothing was drawn, so the ring is silent without the
  completion protocol being touched.
- The PM4 arm (`CZ_D3D` unset) is untouched and is the control for everything.

## The Phase C task

Make the serviced draws draw. The renderer state at each draw call is entirely in
guest memory, split between the arguments and the device struct's register shadow —
the offsets are in the Phase A table's tail and were each verified by disassembly.
The decode machinery already exists in `runtime/gpu/vk_renderer.cpp` (texture
formats + untiler, vertex formats with USCALED/SSCALED, component swizzle, shader
cache keyed FNV-1a over big-endian microcode, `Host_PresentPixels`). Phase C is
plumbing those guts to a new front-end that reads the D3D device struct instead of
PM4 register writes.

Key sources at draw time (device = r3 of every hook):

| what | where |
|---|---|
| render targets | `dev+0x3148+i*4` (slot 4 = `+0x3158` = depth surface) |
| viewport | float worker's writes (hook `sub_8283F990` args, or shadow) |
| depth/stencil/blend/cull state | `dev+0x2934`, `+0x28FD..+0x2903`, `+0x2948`, `+0x293C` shadows |
| vertex shader / pixel shader | `dev+0x3248` / `dev+0x3244` (objects; microcode via `obj+0x380/+0x368` — see `sub_8284F300`) |
| vertex declaration | `dev+0x2ED8` |
| stream sources | `dev+0x31B0+stream*4` (VB objects; addr/size in `+0x18/+0x1C`-style fields, verify per type) |
| texture fetch constants | `dev+(sampler+0x78)*16`, 16 bytes each, already in Xenos fetch-constant format |
| shader float constants | the `(reg+0x30)*0x18`-stride shadow (SetShaderConstantF pair) AND/OR hook the setters directly |
| index buffer | a DIRECT argument to `sub_82843A98` — there is no SetIndices |
| UP draw data | staged at `dev+0x780` (0x4000 bytes) — or intercept the API args before staging |

Strategy choice to make early: read state lazily from the shadow at draw time
(closest to the PM4 renderer's model, fewest hooks), or maintain host-side state by
hooking every setter (UnleashedRecomp's model, more hooks but no shadow-layout
dependency). The shadow layout is per-title knowledge we already hold; the lazy
read is the smaller step and keeps working even for setters we never identified.

## Traps, from this session's own failures

1. **OBSERVE validates firing patterns; only REPLACE validates return values.**
   `sub_82837D70` looked like a fire-and-forget busy-tracker for a whole phase and
   is actually a Lock-style entry returning a CPU pointer — found only when
   servicing it crashed the boot. Before servicing ANY new function, read what the
   caller does with r3 afterward.
2. **Do not service Swap.** The completion protocol spans the D3D worker
   (`sub_8284B828`), an event inside the device struct, the engine render thread,
   and a main-thread ticket poll (predicate `sub_82766760`). Phase C does not need
   it — Swap calling through with real draws no-op'd already presents the correct
   lifecycle. Replacing it is phase D, with the worker's disassembly in hand.
3. **The empty-swap branch will close when draws produce content.** Phase B's
   silent ring depends on the title believing nothing was drawn. If Phase C
   services draws but leaves Swap calling through, VdSwap will emit real swap
   packets again (fine — PM4 consumes them; the present seam then fires from
   case 0x64 as it always has, and `VkRenderer_OnSwap` must NOT also fire — keep
   `CZ_VKDRAW` off). Watch for double-present logic early.
4. **The A1 gate on the replace arm has one KNOWN real window**: `KeResetEvent` +
   the ISR spinlocks, downstream of ring consumption. If draws re-open ring
   traffic, that window should CLOSE again — its state is a free indicator of how
   much of the real pipeline is running.
5. **VS/PS assignment of the constant-set pair (`sub_8283E950`/`sub_8283EAF8`) is
   unpinned**, as is which of `dev+0x3244/0x3248` is which — the flush's null-check
   argues `0x3248`=VS. One instrumented run comparing bound-shader microcode hashes
   against the 336-shader cache settles both at once.
6. **The shader cache keys match by construction** — hash the microcode embedded in
   the shader objects with the same FNV over big-endian dwords (`CZ_SHADER_DUMP`
   flow, gotcha 115). The container also carries the REAL vertex declarations.

## Open questions carried from Phase A/B

- `sub_82845230` ("InsertCallback?"): 6-dword emit, args are small ints — real role
  unknown. It calls through today and emits nothing at a silent title screen... it
  fires 1/frame and the ring stays +0, so its emit must be conditional. Identify
  when touched next.
- The CreateResource thunk family (`sub_82836630/640/648` → `sub_82836038`) is
  unnamed per-type (texture vs VB vs IB). `CreateResource_B` fires ~12/frame at the
  title screen — that is creation-per-frame of something (probably the UP scratch
  ring or dynamic surfaces); worth understanding before Phase C leans on resource
  identity.
- `sub_82838568`/`sub_82838D10`/`sub_82837E08`: unknown, engine callers include the
  suspected movie player (`sub_827A00B8`). Zero calls at the title screen; they will
  fire during cinematics.

## Gates for Phase C

```
./runtime/build/cz_runtime --smoke                     # unchanged
A1/A5 kernel gates, empty save root, BOTH arms         # replace arm: the known window only
CZ_D3D=1 + renderer: frame dumps via the existing CZ_VK_FRAME_DUMP-style flow
frame_signature.py vs capture E2                       # exit 0, no transform
tools/frame_matched_diff.py PM4 arm vs D3D arm         # the two renderers against each other
```

The last one is new leverage this architecture buys: two INDEPENDENT renderers of
the same title in one binary. Their agreement is a gate neither had alone.

## Standing constraints

Commit proactively with the Co-Authored-By trailer; document for Case West;
retract in place; measurement discipline (same-binary arms, rates not single runs,
the animated title screen is one sample per frame — gotcha 133). UnleashedRecomp
code may be taken with provenance headers (GPLv3 consequence recorded in the plan
doc); plume is MIT-verified if an RHI is wanted, but `vk_renderer.cpp`'s existing
Vulkan machinery is the shorter path for a first frame.
