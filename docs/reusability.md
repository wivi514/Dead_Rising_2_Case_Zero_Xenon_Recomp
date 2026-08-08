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

