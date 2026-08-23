# RTX Remix as prior art for our RT shadows — what it does, and what transfers

The operator's suggestion, closing part 68's first session: *"RTX Remix is made to put all
this rtx stuff in old games that doesn't have this in their engine, we should look at how
they do it."*

It is the closest prior art that exists, it solves the two problems this port is stuck
on, and — the part that matters most — **it independently confirms that part 67's defect
is a known, named failure class in this space, with an option dedicated to it.**

This document is a reading of NVIDIA's own documentation and source. Nothing has been
copied. It records the licence first, because that is this project's rule.

## 0. The licence, recorded before anything else

* The DXVK base is **zlib/libpng** (Philip Rebohle, Joshua Ashton).
* NVIDIA's `src/dxvk/rtx_render/*` files carry a per-file **MIT** notice
  (`Copyright (c) 2021-2026, NVIDIA CORPORATION`).

Both are permissive, so unlike UnleashedRecomp — GPLv3, and therefore structural
reference only — code from dxvk-remix *could* be lifted with attribution. That does not
mean it should be: their runtime is a full path tracer that replaces the game's lighting,
where ours injects a shadow factor into the title's own 126 shaders, so almost nothing
would drop in. **Take the technique, and record where it came from.**

## 1. Our part-67 defect is in their options list, by name

> **`rtx.capture.correctBakedTransforms`** — *"Some games bake world transforms into mesh
> vertices. If individually captured meshes appear to be way off in the middle of nowhere
> OR **instanced meshes appear to all have identity xform matrices**, enabling will
> attempt to correct this..."*

The second symptom is exactly what part 66 measured and part 67 diagnosed: every TLAS
instance carrying an identity transform because the position streams were object-space
and the placement lived in a shader constant. The class has two symmetrical failure modes
— transforms baked into the vertices, or transforms left in a constant nobody read — and
we walked into the second. **Anyone doing this on a new title should check both on day
one**, and the check is one line (project the stream by the matrix you believe in and ask
what fraction lands in the frustum the draw was issued into).

## 2. They read transforms out of shader constants too

For fixed-function D3D9 the world matrix is explicit state, but for shader-based engines
Remix does what we did: for Unreal Engine 3 it reads *"camera and object transforms from
UE3's reserved shader constants through CTAB parsing"*, and it carries an option for
engines that hand it a **fused world-view matrix** rather than separable ones.

So `tools/rt_world_xform_census.py` — reading `vc(8..10)` out of the translated microcode
per shader — is the standard move for this problem, not a workaround. Ours has to parse
the microcode because a Xenos shader has no CTAB; the principle is identical.

## 3. Skinned meshes: they SKIN them, they do not exclude them

`RtxGeometryUtils::dispatchSkinning(const DrawCallState&, const RaytraceGeometry&)` is a
**compute shader that skins the geometry into the acceleration structure's input buffer**,
with `rtx.limitedBonesPerVertex` (default 4) capping influences per vertex for
performance.

That is the principled answer to our palette problem. Part 67 collapses a palette-blended
draw onto entry 0 with unit weight, which is right for the static world (99.5% of vertices
on screen for the busiest world shader) and wrong for an actor — the mesh entering our
BLAS is the raw object-space geometry under one bone, so the primary ray hits a wrong
surface and part 68's capture shows the shadow chopped into actor-shaped holes.

`CZ_VK_RT_NO_PALETTE=1` (exclude them) is the cheap arm and it is worth running, because
it prices what the static world loses. But **exclusion is not the destination**; doing the
blend is. Our blend inputs are harder to reach than D3D9's — the weights and indices come
through `XeVfetchDep` on fetch slot 95, indexed per vertex — so the work is to decode that
dependent fetch, not to write the blend.

Remix also states the expectation we should hold for our own results:

> animated meshes may flicker depending on how the game does skinning — **software
> animation (skinning on the CPU) will flicker; hardware animation (on the GPU) should be
> stable.**

Case Zero's actors are the CPU-deformed class our persist store latches as `dynamic`, so
they are the hard half by construction.

## 4. Dynamic geometry: they REFIT the BLAS, they do not rebuild it

Every BLAS in `AccelManager` is created with

```
VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR |
VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
```

and `AccelManager::validateUpdateMode(oldInfo, newInfo)` checks that the previous build's
geometry description is compatible so the next one can use
`VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR` instead of a full build.

**This is the single most important thing to take.** Our BLAS key is a function of the
stream key AND its content guard, so a mesh whose bytes change gets a *new key*, a *new
BLAS*, and — since there is no per-BLAS eviction — unbounded growth until the
`CZ_VK_RT_BLAS_MB` cap flushes everything. That is precisely why
`CZ_VK_RT_DYN_SETTLE=0` had to be documented as a diagnostic rather than shipped. With
refit, a mesh that changes every frame costs one update against a stable allocation, and
both the `dyn` population and the skinned population stop being architecturally excluded.

## 5. Instance identity when the content changes every frame: the AABB

> **`rtx.enableAlwaysCalculateAABB`** — *"Calculate an Axis Aligned Bounding Box for every
> draw call. This may improve instance tracking across frames for skinned and vertex
> shaded calls."*
>
> **`rtx.antiCulling.object.hashInstanceWithBoundingBoxHash`** — *"Hash instances with
> bounding box hash for object duplication check."*

The insight is that **a content hash cannot be the identity of a thing whose content
changes**. For those draws Remix identifies the instance geometrically instead. Ours has
the same hole: `Mix(streamKey, vGuard, idxKey, iGuard, ...)` re-keys a skinned mesh every
frame, so even with refit available we would have nothing to refit *into* until the
identity stops depending on the bytes.

Also worth noting for our own change detector:

> **`rtx.enablePreservePath`** — *"identify draw calls whose state has not changed since
> last frame and re-use the previous frame's translation, rather than retranslating."*

which is what our persist store's guard already does for the raster path.

## 6. What does NOT transfer

* Their **texture-hash categories** (sky, terrain, decal, particle, world matte) are
  authored per game by a human in the toolkit. Ours would come from the microcode census
  instead — the same source that gave us `config/rt_world_xform.json` and part 65's
  `config/rt_shadow_slots.json`. That is a better fit for a recompiler: it is derivable
  and it is checkable.
* Remix **replaces** the game's lighting with a path tracer. We inject a factor into the
  title's own shaders, so their material/light-conversion machinery has no analogue here.
* They target D3D8/9 fixed-function and shader-model-2/3 era titles through a translation
  layer; we sit on a recompiled PowerPC binary driving our own PM4 command processor.

## 7. What it changes about the plan, in order

1. **BLAS refit** (`ALLOW_UPDATE` + a `validateUpdateMode` equivalent). It is the enabler
   for everything below and it is a bounded, self-contained change.
2. **An identity that survives a content change** — stream address + shader + primitive
   count, with the AABB as the tie-breaker, rather than the content guard. Without it
   refit has nothing to refit.
3. **Decode the `XeVfetchDep` weights/indices** and do the palette blend properly, which
   retires `CZ_VK_RT_NO_PALETTE` and the actor artifact together.
4. `CZ_VK_RT_DYN_SETTLE` then stops being a workaround for an architectural gap and
   becomes what it should be: a cost knob.

Sources: NVIDIA's `RtxOptions.md`, `src/dxvk/rtx_render/rtx_geometry_utils.h` and
`rtx_accel_manager.cpp` in `NVIDIAGameWorks/dxvk-remix`, and the `rtx-remix` compatibility
wiki. Read 2026-08-22.
