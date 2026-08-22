# Reusability: what gets extracted, and when

**Split out of `CLAUDE.md` on 2026-08-08.** Forward-looking; relevant when Case West
starts, not before. The two rules at the top are the whole document — the tier list is
how they are applied.

## Reusability: what gets extracted, and when

Case West is the next port and this is the **second** implementation in the workspace
(Fable 2 is the first). That is what makes extraction justified now and would have made
it premature before. Two rules govern it:

> **Extract only what is proven in BOTH ports. Never extract from Case Zero alone.**

> **Extract after the second implementation forces the seam — not in anticipation of a
> third.**

**Tier 1 — hardware-defined, identical wherever you cut the layer.** Xenos microcode →
HLSL/SPIR-V (vfetch lives *in* the shader, so input layouts are reconstructed from
shader code either way); fetch-constant decode; vertex and texture format tables;
texture detiling (the Xenos address swizzle is an algorithm, not a per-title thing);
endian utilities; 7e3, D24FS8, DXN/DXT conversion; PPC/VMX helpers; the guest memory
model; XEX/STFS loading.

**The XMA audio path joins tier 1 as of phase A/V, and it is now PROVEN IN BOTH PORTS**
— which is this document's own bar for extraction, met for the first time by something
non-graphical. The 64-byte `XMA_CONTEXT_DATA` layout, the `0x7FEA____` little-endian
register file, the input/output ring protocol (whole 512-sample decode frames, 256-byte
block offsets, 16-bit big-endian PCM, valid-bit handshake) and the ffmpeg
`AV_CODEC_ID_XMA2` wrapper are hardware facts, and Fable 2's and Case Zero's
implementations differ only in where the context array is owned. **Do not extract it
yet** — that is the rule at the bottom of this file and Case West has not started — but
this is the first item that would survive the second-implementation test today.

**Carry ONE warning with it, because it is the whole cost of the item here** (gotcha 267):
the context's three buffer pointers are PHYSICAL addresses, since the APU is a DMA device.
In a flat recompiler map they are not the addresses the CPU uses, and reading them
literally yields a page of zeros that decodes to SILENCE rather than to an error — which
is indistinguishable from the "no audio" symptom you are there to fix. Case Zero's window
is `virt = 0xA0000000 | phys`; Case West will have the same convention and possibly a
different base, and the kernel's own `MmGetPhysicalAddress` states it in the opposite
direction. The generalisation is broader than audio: **any structure the guest fills in
for HARDWARE rather than for itself** — GPU ring buffers, DMA descriptors, command
lists — deserves the question "which address space are these pointers in?" before the
first dereference.

**Tier 2 — XDK-defined, NOT per-game.** The 360 D3D9 surface is defined by the XDK, so
build the function-signature database **keyed by XDK version** (OOVPA-style patterns)
and never hardcode per-title addresses. State vector → PSO, render and sampler state
mapping, vertex declaration handling, resolve and tiling semantics all transfer
wholesale. Case Zero (2010) and Fable 2 (2008) probably differ; **Case West almost
certainly matches Case Zero**, which is exactly what makes it the cheapest next target.
`docs/d3d-translation-plan.md`'s Phase A table is this port's contribution to it.

**Tier 3 — platform.** Kernel and XAM imports (**grown lazily — implement what a title
actually needs, never speculatively**, which is gotcha 5's rule stated as a roadmap);
XBLA entitlement handling; input abstraction; XMA/audio plumbing; save containers
mapped onto the native filesystem; achievements behind a provider interface.

**Tier 4 — host.** Graphics backend; shader hash → translation → pipeline cache.

**Never shared.** Renderer translation specifics, engine reverse engineering, hook
addresses, timing/framerate/FOV/UI patches, shader hacks.

Rules on top of the tiers:

- **Static-link shared code into each port.** No shared runtime DLL, no ABI versioning,
  no launcher dependency — each port stays independently buildable and preservable, and
  one update cannot break another.
- **Upstream universal fixes** to XenonRecomp/XenosRecomp (new instructions, jump-table
  patterns, generic shader features); title-specific corrections stay in this repo's
  config and patch tree. `docs/xenonrecomp-upstream-bugs.md` is the ledger.
- **Record the license of every borrowed component in `THIRD_PARTY.md` before the first
  line is copied** — UnleashedRecomp, ReXGlue, Xenia included. (plume is
  licence-verified MIT; only video.cpp-derived code carries GPLv3.)
- Structure the renderer so a sibling title *could* reuse it, but **do not build the
  sibling abstraction until Case West actually starts.**

**Precompile everything you can.** n = 1 per port, which a general emulator can never
assume: scan assets at install, translate shaders ahead of time, and ship a populated
pipeline database so the title starts without PSO-compilation stutter. The shader cache
(`assets/shader_spv/`, 336 blobs) already does half of this; the **125 pipelines are
still created at runtime** and are the obvious next candidate.


## For the next recompilation, whichever title it is: two memory decisions, both measured

Written at the operator's request — *"Save this in documentation so we know how to properly
do it in future recomp"* — because both were arrived at by measurement here and both are
cheap to get wrong from first principles.

### 1. Container choice on the critical thread is a first-order performance decision

`std::unordered_map` and `std::map` were roughly **a quarter of this port's graphics pump
thread** — 89% of `UploadStream`, ~72% of `UploadTexture`, 17.9% of `DoDraw` — and none of
it was visible in any profiler PHASE, because a phase names a scope and the scope contained
other things too. Replacing five of them with a flat open-addressed table took the frame
**−13.2%** at the load the operator plays.

A recompiled renderer looks up a cache tens of thousands of times a frame: per draw for
shaders and pipelines, per stream for vertices and indices, per fetch for textures. At that
rate a chained hash table's two dependent cache misses and its mandated prime-modulo
division dominate the work being cached. Start with a flat table; do not arrive at one after
fifty parts. And take the two details with it — mix the key (with a power-of-two mask the
low bits ARE the bucket) and clear by generation bump rather than by memset — plus a verify
arm, because a lookup returning the wrong entry is a wrong ANSWER and not a crash.

`tools/part55_srcline.py` is how this was found and is title-agnostic: it folds a `perf`
profile by SOURCE LINE inside one symbol using the DWARF line table a RelWithDebInfo build
already carries, at zero cost to the subject. Reach for it whenever a hot path is too hot
to instrument with a scope.

### 2. Do NOT put geometry in VRAM. Do put images there.

Full reasoning in gotcha 363; the short form is that **a recompiler is not a normal engine**.
It re-uploads shader constants every draw, because the guest writes its register file every
draw — 8 KB per draw here, ~57 MB/frame at 7,000 draws — so the CPU write cost of
write-combined video memory outweighs the GPU fetch it saves. Measured on an RTX 3070 with
Resizable BAR: **~14% slower**.

  * vertex / index / constant buffers the CPU writes each frame -> `HOST_VISIBLE |
    HOST_COHERENT` in system RAM.
  * images — textures, render targets, shadow maps, resolve snapshots — -> `DEVICE_LOCAL`.
    They are uploaded through staging, never CPU-mapped, and read many times a frame.
  * re-ask the first bullet only after the per-draw constant upload is gone, which flips
    the arithmetic.
  * and unconditionally: **CPU-visible device memory is WRITE-ONLY.** A read of it is an
    uncached fetch across the bus. Audit every read before switching a buffer over.

### 3. Retrofitting ray tracing onto a recompiled title: what part 64 proved transfers

Case Zero's RT stage 2 (`phase5-notes.md` §6cv) is unfinished as a picture, but
several of its findings are about the SHAPE of the problem rather than about this
title, and they should save Case West most of a part:

* **A recompiled renderer is already most of a BLAS pipeline.** Guest vertex
  streams are uploaded dword-swapped and GPU-resident with a content guard that
  says per frame whether the bytes changed. That guard is the BLAS-freshness test
  and the buffers are the build input — one usage bit
  (`ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`) and a device address,
  zero geometry copies. Include the content stamp in the BLAS identity, never the
  address alone: streaming reuses addresses.
* **Pool the acceleration structures.** A crowd is thousands of BLASes and drivers
  cap total allocations around 4,096; one `VkDeviceMemory` per BLAS exhausts the
  allocator with textures still to serve. Sub-allocate from ~64 MB chunks.
* **Injecting through the title's OWN shadow atlas works and needs no shader
  patch** — write depths into the resolve snapshot the title samples and its own
  comparison applies them. Prove the injection point with a constant FILL at both
  polarities BEFORE building anything: two runs, and it also becomes the standing
  positive control. But know the ceiling: tracing into the atlas cannot beat the
  atlas's resolution, so soft or per-pixel shadows need the shader-patch route.
* **The shadow pass is NOT a single-matrix pass.** Do not take "the light's
  view-projection" from the most recent cascade draw — count the distinct
  matrices per slice first. In Case Zero every slice carried several, and half of
  them were per-object composites. Select by dataflow (a stream the scene pass
  draws world-space vouches for the matrix its cascade draw carries), and verify
  the inverse against the identity while you are there.
* **Budget the measurement, not just the build.** Every statistic here — median
  luma, coverage, cumulative counters — drifts with how far a run got, so an
  arm needs a same-binary control run back to back and a frame count quoted
  beside every number (gotchas 384-385). Part 64 spent an hour on three
  "improvements" that were partial reads.
