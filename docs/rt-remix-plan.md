# The RT geometry plan, taken from RTX Remix — items, order, arms and gates

Written at the end of part 68 for a fresh conversation to execute. The prior-art reading
it comes from is `docs/rtx-remix-prior-art.md`; the licence is recorded there (DXVK base
zlib/libpng, NVIDIA's `rtx_render/*` per-file MIT — both permissive, so code *could* be
lifted with attribution, and almost none of it would drop in).

**Read `docs/rtx-remix-prior-art.md` first, then this. Do not re-derive either.**

## 0. What is already true, so it is not re-measured

* **The placement is correct.** `tools/rt_placement_render.py` projects our placed
  geometry through the camera hardware itself used and lands it on Chuck, on every zombie,
  on the lamp posts and the kerb of hardware's own frame. `config/rt_world_xform.json` is
  validated: 46,820 accepted draws over twenty traces, 11.7% -> 98.6% of boxes intersecting
  the frustum they were drawn into (§6cy).
* **The placement fix is measured, in two same-binary pairs**: the AO probe 0.987 -> 0.650
  and the shipped path 0.8% -> 14.6% shadowed, with `CZ_VK_RT_OBJ_XFORM=0` reproducing
  part 66 exactly.
* **The remaining picture defect is the actor population**, measured on the factor image
  rather than through the frame: **100.5 edge pixels per 1000 in the crowd region against
  10.6 on open road**, isolated-pixel rate 0.35%, intermediate share 0.00%. Localised to
  the actors; not acne, not noise.
* **`dyn` is 17-41% of what the collector sees**, depending heavily on where the camera
  is. It is a real loss and a smaller one than part 67's single reading implied.

## 1. THE ORDER, and why it is this order

Each item is the enabler for the next. Doing them out of order means building something
that has nothing to attach to.

| # | item | why it must come first |
|---|---|---|
| 0 | bind the BLAS to the persist store | removes the staging copy, and is what makes refit nearly free |
| 1 | an identity that survives a content change | refit has nothing to refit *into* without it |
| 2 | BLAS refit | the enabler for both the `dyn` and the skinned populations |
| 3 | do the palette blend | retires the actor artifact and `CZ_VK_RT_NO_PALETTE` together |
| 4 | retire the workarounds | `CZ_VK_RT_DYN_SETTLE` becomes a cost knob, not a patch |

---

## ITEM 0 — bind the BLAS vertex source to the persist store

**This is a one-flag finding and it should be checked first because it changes the cost of
everything after it.**

`R->persist` is already created with `deviceAddress = true`
(`vk_renderer.cpp:17284`), holds the guest vertex bytes **already dword-swapped exactly as
the BLAS staging swaps them**, and is re-uploaded by the raster path whenever the guest
rewrites a stream. It lacks exactly one usage flag:

```
VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
```

With it, `VkAccelerationStructureGeometryTrianglesDataKHR::vertexData` can point at
`R->persist.address + entry.at + offsetDw * 4` and the RT path stops copying vertices
altogether. Remix has the same idea as `rtx.useBuffersDirectly`.

**The split that makes this work:** indices cannot come from the persist store, because
the build expands triangle strips into lists and drops degenerates — that expansion is
*derived* data. But topology does not change when a mesh animates, so the expanded index
buffer is built **once per mesh and kept**, while the vertex pointer is re-read every
frame. Static index storage, live vertex pointer.

**Watch the ping-pong twin.** A stream caught changing alternates between `e.at` and
`e.alt`, so the device address differs between frames. That is legal for an AS update (the
geometry *description* must match, addresses need not) but it means the geometry info has
to be rewritten each frame rather than cached wholesale.

**Prediction:** `streams`-class copying disappears from the RT path and per-frame BLAS
staging bytes go to ~0 for meshes already in the persist store. **Gate:** the picture is
unchanged with `CZ_VK_RT_SHADOWS=1` (this is a data-source change, not a semantic one), and
`[rt] blas=` memory is unchanged.
**Arm:** `CZ_VK_RT_NO_DIRECT_BUFFERS=1` restores the staging copy.

**Risk, stated:** the persist store is `HOST_VISIBLE | HOST_COHERENT`, not device-local, so
AS builds read over PCIe. That may be slower per build than staging into device memory. It
is measurable directly (`CZ_VK_PROFILE`, and `[rt] blas=` build counts) and the arm above is
the control. If it loses, item 0 becomes "keep staging, but only for the meshes being
rebuilt" and items 1-3 are unaffected.

---

## ITEM 1 — an identity that survives a content change

Today:

```
key = Mix(0x52545348, streamKey) ⊕ vGuard ⊕ idxKey ⊕ iGuard ⊕ (indexCount|prim|offset|stride)
```

`vGuard` and `iGuard` are **content hashes**, so a mesh whose bytes change every frame gets
a new key, a new BLAS and unbounded growth — which is exactly why `CZ_VK_RT_DYN_SETTLE=0`
had to be documented as a diagnostic rather than shipped.

Remix's answer is that a content hash cannot identify a thing whose content changes, and it
tracks those instances geometrically instead (`rtx.enableAlwaysCalculateAABB`:
*"may improve instance tracking across frames for skinned and vertex shaded calls"*).

**Change:** drop the guards from the key. Identity becomes
`f(streamKey, idxKey, indexCount, prim, offsetDwords, strideDwords)` — address, size,
topology and layout. Keep `vGuard`/`iGuard` in the `Blas` record as *state*, not identity:
they are what says whether this frame's bytes differ from the ones the BLAS was built from.

**The collision question changes shape and gets easier.** Today a recycled address is a
correctness hazard (`g_keyCollisions`: "treat as absent; never trace another mesh's BLAS").
Once refit exists it is not: if the address now holds a different mesh with the same
topology, refitting from the new bytes produces the geometry that is actually there. The
only genuine incompatibility is a *different index count or layout*, and that is already
part of the key.

**Prediction:** `collide=` stops being a correctness counter and `blas=` stops growing on
the dynamic population. **Gate:** with refit not yet implemented (item 2), this item alone
must leave the picture and `tlasInst` unchanged on a static scene — it is a re-keying, not
a behaviour change. **Arm:** `CZ_VK_RT_STABLE_KEY=0`.

---

## ITEM 2 — BLAS refit

Every Remix BLAS is created

```
VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR |
VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
```

and `AccelManager::validateUpdateMode(oldInfo, newInfo)` checks that the previous build's
geometry description is compatible before using
`VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR`.

**Ours builds with `PREFER_FAST_TRACE` and no `ALLOW_UPDATE`** (`vk_renderer.cpp`, the
`bd.info.flags` line). That is the whole reason a changing mesh costs a new allocation.

**Change:**
1. add `ALLOW_UPDATE` to the BLAS build flags (note: this *costs* trace performance — that
   is the documented trade, and Remix pairs it with `PREFER_FAST_BUILD` rather than
   `PREFER_FAST_TRACE`; measure before choosing);
2. each frame, walk the live set for BLASes whose stored `vGuard` differs from the persist
   entry's current guard — the **dirty list**;
3. issue those as `MODE_UPDATE` with `srcAccelerationStructure = dstAccelerationStructure`
   (in-place is permitted), reusing the same AS allocation;
4. **use `updateScratchSize`, not `buildScratchSize`** — a real gotcha: they differ, and
   sizing update scratch from the build figure wastes memory while sizing it from nothing
   is undefined behaviour the validation layer will name.

Refit is only valid when the topology is unchanged. So: `iGuard` differs ⇒ full rebuild;
only `vGuard` differs ⇒ refit.

**Quality caveat, stated:** a refitted BLAS keeps the original build's tree topology, so
after large motion its trace quality degrades. Remix rebuilds periodically. Add a
`framesSinceBuild` counter per BLAS and force a rebuild past a threshold
(`CZ_VK_RT_REFIT_MAX=N`), so the degradation is bounded and named rather than discovered.

**Prediction:** with `CZ_VK_RT_DYN_SETTLE=0` — the configuration that today would climb to
the 1 GB cap and flush — `blas=` stabilises and `flushes=0` holds over a long roam.
**That is the gate, and it is checkable headlessly from the log alone.**
**Arm:** `CZ_VK_RT_NO_REFIT=1`.

---

## ITEM 3 — do the palette blend

Remix skins on the GPU: `RtxGeometryUtils::dispatchSkinning(drawCallState, geo)`, with
`rtx.limitedBonesPerVertex` (default 4) capping influences.

Ours is harder to reach than D3D9's fixed-function blend, and the difficulty is *reading
the inputs*, not doing the arithmetic. From the translated microcode of
`vs_b677dc3457f5b41a`:

```
r2.xyz = XeVfetchDep(95u, r0.x, ...).yzw    <-- three WEIGHTS
r6.xyw = XeVfetchDep(95u, r0.x, ...).ywz    <-- three INDICES
a0 = (int)clamp(floor(r6.x + 0.5), -256, 255)
r5 = r2.xxxx * vc(8 + a0).xzyw + ...        <-- the palette, entries 3 rows apart
world = dot(r5, pos4), dot(r3, pos4), dot(r4, pos4)
then vc(4..6), then the camera composite at vc(0..3)
```

So the work is:
1. **extend `tools/rt_world_xform_census.py`** to record, per palette shader, the dependent
   fetch slot and the component swizzles that carry the weights and the indices. It already
   walks this dataflow; it currently stops at "palette@8". This is offline and it is where
   the item should start — **census first, build nothing** (the instruction that made parts
   65 and 67 cheap and whose absence made part 66 expensive);
2. resolve the dependent fetch's own vertex buffer at collect time (`XeVfetchDep(95, ...)`
   is an ordinary fetch constant read with a per-vertex index);
3. blend and write positions into the BLAS vertex source. **Start on the CPU inside the
   existing staging copy** — it is a few lines and it is correct-or-not, no compute plumbing
   — and only move it to a compute pass if the profile says so. This project has repeatedly
   found that the expensive-looking thing was not the cost (parts 47, 55).

### What part 68 measured about this item, before anything was built

**The palette path is not the actor path — it is the engine's MAIN WORLD SHADER.**
`CZ_VK_RT_NO_PALETTE=1` in an operator session took `tlasInst` from **2994 to 1197**, a
60% loss of occluders, and the offline census agrees to the point: `vs_b677dc3457f5b41a`
alone carries 2,658 of the gas-station frame's 4,512 accepted draws. So exclusion cannot
ship as anything but a diagnostic, and this item is REQUIRED rather than optional.

**And the cheap shortcut is already dead.** The obvious saving would be to detect draws
whose palette does not actually vary — a static building using entry 0 with unit weight is
already handled exactly — by comparing palette entry 0 against entry 1 in the constant
window. Measured over two traces:

| | gas_station_sign | tanker |
|---|---|---|
| palette, entry 1 DIFFERS from entry 0 | 63.4% | 69.8% |
| palette, entry 1 same as entry 0 | 9.9% | 9.9% |
| `direct` (no palette) | 26.6% | 20.2% |

Two thirds of accepted draws have a distinct second entry, so the test separates nothing.
**It has two readings and they need different work**: either most of this world really is
batched under a matrix palette (several props per buffer, each vertex indexed to its own
matrix), or vc(base+3..base+5) simply holds unrelated constants for shaders that only ever
use entry 0, making "differs" meaningless. **Settle that first, offline**: read the
dependent fetch's actual per-vertex indices for one known draw and count how many distinct
palette entries it references. One draw answers it.

If it is the first reading, the shape of the fix changes: the placement is PER VERTEX, so
the answer is to **bake world positions into the BLAS vertex data at staging time** and
leave the instance transform at identity — which is what Remix's skinning compute pass
does, and which costs us nothing extra because we already keep one BLAS per draw key.

**Prediction:** the factor image's crowd-region edge density falls from 100.5/1000 toward
the open-road figure of 10.6, with no change on open road. **That is the gate and it needs
no eye** — `tools/` already has the measurement, it was run on part 68's capture, and the
comparison is the same statistic on the same route.
**Arm:** `CZ_VK_RT_NO_PALETTE=1` (already shipped) stays as the control.

---

## ITEM 4 — retire the workarounds

* `CZ_VK_RT_DYN_SETTLE` stops being a patch for an architectural gap and becomes a **cost**
  knob: how much churn to accept. Re-measure its default with refit in place; it may want
  to be 0.
* `CZ_VK_RT_NO_PALETTE` retires when item 3 lands, or stays as a tier knob if the blend is
  too expensive for RT LOW.
* Re-ask part 67's exonerations **against the new structure** (gotcha 172: a retirement is
  only as good as the oracle it was measured on). The sun, the ray length and the bias were
  all cleared at 0.9% shadowed against a pile at the origin, which is no test at all. Mode
  20 in particular said "no direction is occluded" *because the structure was empty*.

## 2. Deliberately out of scope

* **Remix's texture categories** (sky, terrain, decal, particle, world matte). They are
  human-authored per game in their toolkit; ours should be derived from the microcode
  census, the same source as `config/rt_world_xform.json` and `config/rt_shadow_slots.json`.
  Derivable and checkable beats authored, for a recompiler.
* **Path tracing, materials, denoising.** We inject a shadow factor into the title's own
  126 shaders. Their material and light conversion has no analogue here.
* **Per-BLAS eviction.** The AS pool is a bump allocator, so evicting an entry reclaims
  nothing. Refit is what removes the pressure; eviction would be a separate, larger change
  and item 2 is expected to make it unnecessary.

## 3. How each item gets measured

Every item above names its own prediction, arm and gate. Two rules on top:

* **Prefer the factor readback to the picture.** `CZ_VK_RT_FACTOR_READBACK` plus the
  edge-density statistic answered in ninety seconds what eleven ladder modes could not in
  three sessions (gotcha 397). Items 2 and 3 both have log-only or offline gates; only the
  final look needs the operator.
* **The operator's eye answers SHAPE.** Three headless statistics missed both the 427 px
  misalignment and the "nothing is world-locked" observation. Book their time for "which
  surfaces", not for "how much".

## 4. Risks, with kill thresholds stated in advance

| risk | how it shows | threshold |
|---|---|---|
| `ALLOW_UPDATE` costs trace performance | `CZ_VK_PROFILE`'s RT pass time rises with RT on | if the RT pass costs more than 15% over the FAST_TRACE build at the operator's load, keep two BLAS classes — static built FAST_TRACE without update, dynamic built FAST_BUILD with it |
| host-visible AS input is slow (item 0) | build time per MB rises against the staging path | revert item 0 only; items 1-3 do not depend on it |
| refit quality degrades under motion | shadows soften or detach on moving meshes | `CZ_VK_RT_REFIT_MAX` forces a rebuild; if the needed N is below ~10 frames, refit is not buying anything and item 2 fails honestly |
| the palette blend is expensive | pump-thread time rises (part 55's lesson: this thread is the frame rate) | move to compute; if still expensive, make it a HIGH-tier-only feature and keep `NO_PALETTE` for LOW |

## 5. What to read, in order, before writing any code

1. `docs/rtx-remix-prior-art.md` — the source of all of this, and the licence.
2. `docs/phase5-notes.md` §6cy — part 67, the object-space finding and what shipped.
3. This file.
4. `docs/open-items.md` 0v — the backlog entry.
5. The collector itself: `runtime/gpu/vk_renderer.cpp`, `namespace rtshadow`, from
   `Collect()` to `BuildFrameStructures()`.
