# Part 102 plan — no pop-in and no stutter on session ONE (the hedge-dev question)

**Written 2026-09-07 from a census, to be executed fresh.** The operator asked whether
the UnleashedRecomp approach — shaders recompiled ahead of time, pipelines discovered at
asset load and created on background workers, so no draw ever waits — could be applied
here so there is *no pop-in and no stutter from the beginning*. This document is the
answer: what of that design we already have, what the one real gap is, why the obvious
hook does not exist in this title, and the concrete route that closes it.

Read `phase5-notes.md` §6er first: it is the measurement this plan starts from.

---

## §0 Where we already are, mapped onto hedge-dev's three layers

| hedge-dev layer | this port, as of part 101 |
|---|---|
| **Build time:** every Xenos shader → HLSL → DXIL/SPIR-V, embedded | **First run:** the disc's own banks → SPIR-V, 1,265 of 1,265 PIXEL shaders in ~9 s under the progress window (`shader_prebuild.cpp`). Identical outcome for the pixel half; nothing is embedded because the microcode is Capcom's and the player's disc supplies it. |
| **Load time:** traverse the asset's render structures, derive pipeline keys, create them on the streaming workers before the asset is "ready" | **Boot time:** `PrewarmPipelines` reads a shipped 1,365-key seed (`tools/release/prewarm.keys`, recorded from a full playthrough) unioned with the per-user file, and creates every key whose two shaders exist. Keys whose shader is missing park in `prewarmWaiting`; `OnShaderArrived` builds them the moment the shader lands. This is hedge-dev's "predetermined list" method at whole-game scale — no asset traversal, same effect. |
| **Draw time:** map lookup, never create | Map lookup; on a miss, ASYNC create on a worker and SKIP the draw until it exists (part 98). Skipping is what the player sees as pop-in. |

**Stutter is already solved.** Session one on a fresh machine (every store parked,
§6er): 4 frames >100 ms of 19,506, one of them outdoors. Session two: zero.

**Pop-in is the one open half, and it is entirely the VERTEX shaders.** Session one
skipped 234,849 draws. Every one of them was waiting on a pipeline whose vertex shader
did not exist until the draw that first bound it, because:

- the disc holds pixel microcode verbatim (343 of 345 byte-for-byte) but **0 of 104
  vertex shaders**: the title patches the vertex FETCH instructions at bind time from
  the vertex declaration (`release-plan.md` §1.4's retraction), so
- the vertex half is built by *first-sight translation* — 45 vertex + 2 pixel shaders
  translated at 13-19 ms each on session one (32-52 ms on czamd), all off-thread — and
- every seed key naming that vertex shader is parked until then, and every draw wanting
  one of those pipelines is skipped from the first bind until translation + creation
  finish. That is the pop-in.

So the question "can we do what hedge-dev does" reduces to: **can the vertex shaders
exist before the draw that first binds them?**

---

## §1 The hook hedge-dev used does not exist here — measured, not assumed

Hedge-dev's lead time comes from the asset loader: the shader is known when the model
streams in, seconds before it is drawn. The 360 D3D runtime offers the equivalent
(`IDirect3DVertexShader9::Bind`, pre-binding a shader to a declaration at load) and I
checked whether this title uses it.

It does not. The bind is **lazy, inside the draw flush** (`sub_8284F300`, the function
the d3d-translation recon table calls the draw flush):

```
8284F34C  lwz r30, 0x3248(r31)        ; dev->vertexShader
8284F350  lwz r19, 0x2ed8(r31)        ; dev->vertexDeclaration
8284F354  lwz r29, 0x3244(r31)        ; dev->pixelShader
...
8284F764  bl  sub_8284F1C0            ; (dev, vs, decl, flag) — is vs bound for decl?
8284F798  bl  sub_8284EF28            ; (dev, 0, vs, &out, decl, binding, …) — bind: patch
```

`tools/guest_callers.py --callers` on both: **their only caller is the draw flush.** The
patch routine underneath (`sub_8284EDD8`, ~85 instructions plus two helpers
`sub_8284EC50`/`sub_8284ED20`) is reached only through them. Nothing in the engine's
`cVertexShaderManager` / `cVertexDeclManager` pre-binds. So a guest-side hook at the
bind would fire microseconds before the `IM_LOAD` we already see; **there is no earlier
signal to hook in this title.** Hedge-dev's load-time discovery is not available to us
as a hook, and reproducing it by traversing DR2's mesh/material assets would be a
reverse-engineering project with no gate.

---

## §2 The route that works: regenerate the vertex half from the disc at first run

**The runtime vertex shader IS the disc template with a handful of dwords rewritten.**
Census (this session; `tools/big_list.py --extract .vo` on the vs bank,
`vo_extract_microcode.py`, then a same-length dword diff of each of the 104 dumped
runtime vertex shaders against all 142 disc templates):

```
runtime VS:  104     with a same-length disc template:  102     no template:  2
patched dwords per shader:  2 .. 32   (median 8;  978 dwords in total = 7.8 KB as (index,value) pairs)
distinct templates used:  84 of 142   (12 templates bound under >1 declaration, max 5)
```

The patch has exactly the shape §1.4 described — vfetch triples with the fetch-constant
slot, offset, stride and format fields filled in from the declaration, zero on disc:

```
vs_070809eb11d26e55  <-  disc vs_b2a80524a46f3c6b
  dw13  disc 00000A88  runtime 00393A88
  dw14  disc 00000000  runtime 00000003
  dw15  disc 05F85000  runtime 03F85000
  dw16  disc 00000FC8  runtime 00253FC8   …
```

**The 2 without a template** (`vs_539ea9e08aa83f0c`, 108 B; `vs_a4ae7c2b7c1818c4`,
60 B) are engine-synthesised and are the 2nd and 4th vertex shaders the title ever binds
(run_A.log lines 234/242: boot, before any frame the player sees). First-sight
translation at boot covers them and always did; they are not a pop-in source.

Every runtime vertex shader the seed's 1,365 keys name is therefore reproducible from
the player's own disc plus a small table, hash-gated exactly like the pixel prebuild
(FNV-1a must equal the name the seed already carries). With that, **session one has the
whole cache before the first frame, the seed pre-warm creates every pipeline at boot,
first-sight reads 0 and skipped draws read 0** — the same numbers §6er measured for
session two, on session one.

### 2.1 Two ways to ship the table — the operator's call

**(A) Recipe file — one session.** `tools/release/vs_recipes.bin`: per runtime VS,
`{template hash, runtime hash, N, (dwordIndex, value) × N}`. 7.8 KB. The prebuild pass
applies each recipe to the extracted template, hashes, keeps only exact matches,
translates. Simple, gated, and the census above is already the generator.
*Caveat:* the 978 values are patched Xenos instruction words — Capcom-derived bytes,
which `main.cpp`'s overlay comment says this project does not ship. `prewarm.keys`
already ships the title's register state; instruction words are one step further.

**(B) Declarations + our own patcher — two to three sessions, policy-clean.** Ship
`{template hash, runtime hash, vertex declaration}` where the declaration is the
D3DVERTEXELEMENT9-shaped table (stream, offset, type, usage, usage index) captured by a
one-off hook on `sub_8284EF28` during the seed playthrough, and reimplement the ~85
instruction patch routine host-side (fill each vfetch's slot/offset/stride/format from
the element matching its usage). The microcode is then produced entirely from the
player's disc; what ships is a data description, like the keys. (A)'s recipe file is the
exact oracle for (B): the patcher must reproduce all 978 dwords, and the hash gate
catches anything it does not. *Checked and NOT usable as a shortcut:* the disc's
`deadrisingprologue-vd.big` holds 143 `.vdo` objects, but they are the engine's named
shader-definition records (`gEyeSpaceDepth`, `gHalfPixelOffset` …), not element tables —
the declaration has to be captured from the running title.

**Recommendation:** build (A) first, because it proves the whole chain in one session
with a free gate, then convert its table to (B) before it ships if the operator wants
the stricter line. The runtime side is identical for both: a prebuild pass that yields
vertex `.spv` + sidecars, and everything downstream already exists.

### 2.2 The residual, and what still says "pop-in"

After this, a draw can still be skipped for exactly two reasons, and both are counted
in the exit dump (`[vk]   draw: skipped, pipeline creating in background` and
`draw: shader translating`):

- a vertex shader no run has recorded (an era outside the seed playthrough) — first
  sight, as today, off-thread, once per machine;
- a pipeline key outside the 1,365 — async create, momentary.

Both are the shelf-life class (gotcha 13) and are hedge-dev's "special list" residual
in another dress. **Optional item 3 below makes the second one invisible.**

---

## §3 The work, in order

0. **Confirm the pop-in the operator sees is OURS before building anything.** On the
   binary with the union fix (95611b9), session two+ should read
   `draw: skipped, pipeline creating in background  0` and `draw: shader translating  0`
   in the exit dump. If those are zero and they still see pop-in, it is the title's own
   streaming/LOD (DR2 has it on hardware) and no shader work moves it. Ask for the two
   lines from a session-two log. Session ONE is expected to show the 234,849-class number
   until item 1 ships.

1. **`tools/vs_recipes.py`** — the census above made permanent: extract templates,
   diff the ucode dumps, write `tools/release/vs_recipes.bin`, and **gate two-sided**:
   applying every recipe to its template must reproduce the dump byte-for-byte (exit 1
   otherwise), and every `vsHash` in `prewarm.keys` must be either a recipe or one of the
   two synthesised shaders (name any that is neither — that is a seed key no first run
   can ever satisfy).

2. **`shader_prebuild.cpp`: the vertex pass.** After the pixel pass, open the vs bank,
   extract each template named by a recipe, apply, hash, refuse on mismatch (loudly, by
   name — never translate a blob that failed its hash), translate through the same
   in-process path (`--translate-shaders` identity gate applies), persist with sidecars.
   Progress line `PREPARING SHADERS - … OF …` widens to include them. Off switch
   `CZ_NO_VS_RECIPES=1` = today's behaviour, the control arm. Then **check that
   `PrewarmPipelines` actually sees the prebuilt modules**: §6er recorded `0 of 1365`
   on a fresh machine because the pixel half landed AFTER the pre-warm ran; if the cache
   load is what is late, the pre-warm must run after it (the chain already tolerates
   either order, but the boot-time synchronous create is the cheaper place for 1,365).

3. **(Optional) Budgeted synchronous creation for residual misses.** Pre-warm measured
   0.095 ms per pipeline here; a miss that is one or two pipelines is cheaper to create
   inline than to skip for a frame. Create synchronously while the frame's budget
   (say 1.5 ms, `CZ_VK_SYNC_BUDGET_US`) is unspent, skip past it. Measure on czamd
   before defaulting it on — AMD's creation time is the unknown.

4. **Gate = the §6er fresh-start recipe, unchanged**, so the numbers are comparable:
   every store parked, seed present, DebugJump outdoor route. Predictions, stated now:
   `[prebuild]` reports 1,265 pixel + 102 vertex translated; `pipeline pre-warm: 1365
   of 1365 created` (or chain-built before the first outdoor frame); `first-sight
   translation` lines: the two synthesised shaders only; `draw: skipped, pipeline
   creating in background` **0** on the outdoor route; >100 ms frames no worse than 4.
   Then czamd, session one on a wiped cache dir, the operator's eye on pop-in.

5. Release packaging: `release_package_linux.sh` / `.ps1` stage `vs_recipes.bin` beside
   `prewarm.keys` with the same header check; `release_gate_clean_container.sh` runs the
   vertex pass in-container. Regenerate both files whenever the seed playthrough is
   redone — they are one dataset.

---

## §4 What this does NOT claim

- It does not remove first-sight translation or async creation; those stay as the
  safety net for unrecorded eras, with their counters.
- It does not touch the stutter half, which §6er already measured as closed.
- It does not make the picture different in any frame where nothing was skipped, and
  the control arm (`CZ_NO_VS_RECIPES=1`) is a same-binary A/B for the skip counters.
- Case West: the same three functions (bind check, bind, patch) will exist at other
  addresses in the same D3D runtime, and the same census answers the same question in
  an hour — record it in `reusability.md` when it lands.

---

## Execution record (2026-09-07, same day — DONE on the dev box; czamd owed)

**`phase5-notes.md` §6es is the record; gotchas 514 and 515 are the transferable
findings.** Item 0 was answered by the operator before it was asked (*"it's ours ... it's
just the first time we see it"*) and confirmed by the baseline: 850,417 skipped draws on
the fresh-start route, 47 first-sight translations. Items 1, 2 and 5 shipped as planned;
item 3 (budgeted synchronous creation) was NOT needed and was not built — the residual
after the async warm is 173 skipped draws in a 46,851-frame run, and 0 on session two.

What the plan did not predict: **the synchronous boot warm was the next wall.** Fully
populated on session one, and on an EMPTY driver cache, it cost 37.8 s before the first
frame (28.5 ms a pipeline on NVIDIA; part 101's 118 ms on czamd). It is async now, on four
workers, drained by ~18 s under the logos; `CZ_VK_SYNC_PREWARM=1` and
`CZ_VK_PIPELINE_WORKERS=N` are the arms. The plan's §4 predictions held everywhere else:
1,265 + 102 translated at first run, first-sight = the two synthesised shaders (plus the
two disc-less pixel shaders), `draw: skipped` 173 (not 0 — 8 seed keys were promoted by
draws before the four workers reached them), >100 ms frames 0.

Owed: the czamd session (copy `vs_recipes.bin` beside the exe — the runtime says
`no .../vs_recipes.bin — vertex shaders will translate at first sight instead` when it is
missing, which is how the first dev run here fell back); the licensing decision in §2.1;
recovering the seed's 3 orphan vertex shaders.

**2026-09-08, operator-verified on the dev box:** *"Perfect, did not get any stutter"*, *"did not get any pop in of assets"* —
session one 1,920 skipped draws in 4 minutes, session two 10; seed grown to 1,378 keys
from their route. czamd remains owed.

**czamd, 2026-09-08:** release bundle built on czwin, gated, installed; session one on
parked stores (shader cache, key file, AMD VkCache contents). Shader half clean (4
boot-only first-sights). The async warm ran into gameplay at 155 ms a pipeline on four
workers (211 s of worker time; 97 seed keys promoted, 158,706 skips) and the operator
felt it as stutter — first-run-only (warm: 0.2 ms a pipeline). §6es carries the full
table. **Owed, in order:** (1) `cz_play.bat` steady-state run for the stutter verdict
(counter only); (2) if session-one stutter matters, budget-aware sizing or lower priority
for the pipeline workers, re-measured with VkCache parked; (3) the black-square arms
`cz_arm_fif1` -> `noclear` -> `norecord`.
