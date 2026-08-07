# Phase 5 notes — the renderer

What the renderer work found that neither `docs/runtime-plan.md` nor
`docs/phase5-kickoff.md` predicted. The kickoff is the hand-off; this is the record.

Read `docs/xenia-capture-analysis.md` first — it remains the authority on measured
numbers. Where this file states a number it was measured in this phase and says how.

**Status at the end of the first session: the renderer exists, runs on a real GPU,
draws real game content, and holds every pre-existing gate.** It does not yet produce a
correct title screen. The gap is enumerated in §7 with a measurement for each part of
it, which is the deliverable that matters more than the picture: nothing in the list is
a mystery, and every item has a counter attached.

---

## 1. The shader pipeline: 336 of 336, and not one recompiler change

`tools/build_shader_spv.sh <ucode_dir> [out_dir]` takes a directory of microcode
through Fable 2's container synthesizer (`tools/synth_shader_container.py`, copied
verbatim), XenosRecomp and DXC, and writes one `.spv` + `.meta.json` per shader.

**Every shader this title uses translates, with zero failures.** XenosRecomp's Fable 2
patches — dependent vertex fetches, the full bool-constant file, param-gen, the
extended vertex-input location table — carry over completely. That is worth stating
plainly because it was the phase's biggest open risk and it cost nothing.

### The input is our own dump, not Xenia's

The cache is keyed on **FNV-1a of the microcode as the guest holds it**, computed by
`runtime/gpu/pm4.cpp`'s `IM_LOAD` handler. `CZ_SHADER_DUMP=<dir>` writes one file per
distinct blob under exactly that name, so a cache entry and a bound shader agree by
construction.

Why not build it from Xenia's `dump_shaders` output directly: Xenia writes the same
shaders with *Xenia's* idea of where the microcode ends. The runtime looks a shader up
by hashing the bytes **its own** `IM_LOAD` read, so any disagreement about the length is
not a slightly wrong picture — it is a total, silent cache miss.

### That comparison is also a free oracle for our IM_LOAD arithmetic

A boot-to-title run dumps **121 distinct blobs. 120 of them are byte-identical to the
ones Xenia dumped from the same era in A1**, modulo dword order (Xenia writes
host-endian, the guest holds big-endian). Nothing had ever checked the size field in the
`IM_LOAD` packet; it is checked now, against hardware, on every shader the era loads.

The one blob with no A1 counterpart is not a discrepancy to chase — our run and the
capture are different drives through the same era and neither is a subset of the other
(the same relation gotcha 45 records between A1 and A5).

### CORRECTION: "455 raw microcode blobs" is a file count, not a shader count

`CLAUDE.md` and `docs/phase5-kickoff.md` both quote 455. That is the number of
`*.ucode.bin.*` **files** across A1 and A2. Measured:

* A1 holds **120** distinct shaders, A2 holds **335**, and **A1 is a strict subset of
  A2** — zero A1-only shaders.
* So the captures hold **335 distinct** shaders. Our own boot dump adds exactly one
  more that neither capture saw: **336 in the cache**.

Corrected in place in both documents.

### tools/xenia_ucode_to_cache.py

The gameplay shaders are needed before the runtime can reach gameplay, and the byte
equivalence above is what makes them usable: swap the dwords, hash with the runtime's
own function, and a Xenia dump becomes a cache entry under exactly the name the runtime
will ask for. The cross-check is exact — **120 of our 121 own names reappear** in the
converted set, which validates the conversion end to end without reference to the bytes.

---

## 2. The renderer's shape

`runtime/gpu/vk_renderer.cpp`, `runtime/gpu/xenos.h` (the register and format
constants), and a new draw seam in `pm4.cpp`.

**Off unless `CZ_VKDRAW=1`.** With it off, `Pm4_SetDrawSink` is never called and DRAW
packets take exactly the phase 4 path, so the phase 3 binary is available in the same
build for every A/B (gotcha 86).

The interface the translated shaders present is transcribed at the top of
`vk_renderer.cpp` from the generated HLSL rather than designed: three uint64 device
addresses in a 24-byte push constant block, five descriptor sets matching the HLSL
register spaces, and a shared-constants block whose every offset appears verbatim in
the shaders.

The renderer runs **on the vblank pump thread**, the same thread that walks the ring,
and its Vulkan objects are used from nowhere else. The frame is submitted synchronously
at `XE_SWAP` and read back into phase 3's present seam via a new
`Host_PresentPixels`. A Vulkan swapchain on the SDL window would avoid the readback and
would put Vulkan on the window's thread, which is the coupling phase 3 deliberately
avoided; at the guest's own frame rate the readback is not what limits anything.

---

## 3. A field's width is part of the field

The first version decoded a vertex fetch constant's size as `dword1 >> 2`. The field is
`endian:2, size:24, unused:6` — **the 24-bit mask was missing**, so the six unused high
bits rode along and an 85-dword vertex stream read as 67,108,885 dwords.

The symptom was not a wrong picture. It was **2,225,992 draws reported as "vertex
stream outside the physical arena"**, which reads as an addressing or memory-map
problem, in a different subsystem, with the actual bug three layers away. With the mask
in place the same run executes 1,087,826 indexed draws.

---

## 4. The register file settles what neither tool can

A vertex fetch instruction's constant index is `const_index * 3 + const_index_sel`.
Applying that to Case Zero's shaders gives 95/94/93 for the exact shaders **Xenia's
disassembly prints as `vf0`/`vf1`/`vf2`** — and one of those two is a display
convention while the other is the hardware's index into the fetch-constant file.

Reading either tool harder cannot say which. Dumping the *populated* slots at a draw
can, and does in one run (`CZ_VK_FETCH_PROBE=1`): the guest writes slots 0 and 95, and
the shader that asks for slot 0 by our reading finds slot 0 populated. **Ours is the
hardware index; Xenia's disassembler prints `95 - index`.**

The general form is worth keeping: when two sources disagree about an index, the
authority is neither of them — it is the data structure being indexed.

---

## 5. The frame is a resolve destination, not the EDRAM

This was the phase's most load-bearing finding and it presented as a scaling bug.

Rendering every pass into one persistent colour target and reading the whole thing back
at `XE_SWAP` produces a picture with all the content crammed into the top-left corner
at assorted sizes. That looks exactly like a viewport transform off by some factor. It
is not: every viewport in the stream is correct (`CZ_VK_VIEWPORT_TRACE=1` prints them,
and the main pass is a clean `1280x720` from `xs=640, xo=640, ys=-360, yo=360`).

`CZ_VK_RESOLVE_TRACE=1` says what is really happening. **One title-screen frame issues
about twenty resolves**, all into the same EDRAM:

```
dest=06837000 destPitch=64   destHeight=64   surfacePitch=80    ctl=00100300
dest=06F7D000 destPitch=512  destHeight=256  surfacePitch=560   ctl=00100300
dest=147C0000 destPitch=640  destHeight=360  surfacePitch=640   ctl=00100300
   ... a 640x360 / 320x180 / 160x90 / ... / 32x1 downsample pyramid ...
dest=00E48000 destPitch=1280 destHeight=720  surfacePitch=1280  ctl=00100000
                                                        front=A0E48000
```

Two things fall straight out:

* **The last resolve's destination is exactly the address `VdSwap` named as the front
  buffer.** That is the frame, and nothing else is.
* **Every intermediate has `RB_COPY_CONTROL` bits 8 and 9 set (clear colour + clear
  depth) and the front-buffer one does not.** So the passes clear the EDRAM behind
  themselves and communicate *only* through guest memory.

### Resolve snapshots

So each resolve copies the EDRAM region into a host image keyed by its **destination
address**, and a texture fetch that names that address is served the host image
directly. The resolved pixels are never written back into guest memory: that would mean
tiling them so the consumer could untile them again, a round trip whose only product is
lost precision.

Measured over a 120 s boot: 67 snapshots created, **450,488 texture fetches served from
one**, and 1,187 of 1,195 frames presented from the front-buffer resolve.

The snapshot is deliberately **not** cached in the ordinary texture cache: a snapshot's
contents change every frame while its fetch constant does not, so caching it on the
fetch constant would freeze the first frame's version of that surface forever.

---

## 6. Two arms run, one of which retired a theory

**`CZ_VK_FORCE_COLORMASK=1`.** 303,866 of 787,785 draws (38.6%) arrive with an empty
`RB_COLOR_MASK`, which is either a legitimate depth-only pass or a register read at the
wrong index — and those two are indistinguishable from the picture. Forcing every draw
to write all four channels produces a **byte-identical frame**. The masks are real
depth-only passes; the register index is right; the theory is retired in one run rather
than argued about.

**`CZ_VK_FRAME_DUMP` / `CZ_VK_SNAP_DUMP`.** Every other gate this project owns is a log
diff, and "the picture is right" is the one claim that needs an image. Dumping frames
and snapshots from a **headless** run makes the renderer checkable without a window,
which is what turns the E-series comparison from an operator task into a self-servable
one.

`CZ_VK_SNAP_DUMP` is what localised §7's remaining gap: dumping *every* snapshot of one
frame is the only instrument that can distinguish "the last pass is wrong" from "the
first pass is wrong", because the frame is the last link and a wrong frame is
consistent with both.

---

## 6b. The dependent vertex fetch table, and what it turned on

A Xenos `vfetch` addresses its stream with a **register**. Only while that register still
holds the auto-loaded vertex index is the fetch expressible as a Vulkan vertex
attribute; a shader that *computes* an address — a bone palette, a per-instance record
index, particle state — is fetching from somewhere no vertex input can describe.
XenosRecomp emits those as in-shader raw loads (`XeVfetchDep`) and reads the stream's
address and size out of a table **the runtime publishes** at `SharedConstants + 544`,
sixteen bytes per fetch slot.

The runtime was not publishing it. **22 of this title's 67 vertex shaders take that
path**, and the failure mode is the quiet one: the shader's own bounds check sees size
0, returns `float4(0,0,0,0)`, and every vertex of the mesh collapses to the origin. The
draw executes, the pipeline is fine, no counter fires, and the mesh is simply absent.

Publishing it (55,702 streams a run) took two 1280x720 surfaces from **0.0% to 69.8%
non-black**. What they now contain is *scrambled* geometry — large clean triangles
radiating from a vanishing point — so the table is being read and its contents are
still wrong somewhere. That is a much better position than absent: it is a live wire.

## 6c. Four assumptions checked against the guest, and one retired by an arm

`CZ_VK_STATE_PROBE=1` prints the distinct values of the state registers the renderer
*assumes* rather than reads. Every one of these is a place where a wrong assumption
gives a plausible wrong picture instead of an error, so they are worth the one run:

| register | what the guest writes | verdict |
|---|---|---|
| `SQ_VS_CONST` (0x2307) | base 0, size 255 | our VS window (ALU 0..255) is right |
| `SQ_PS_CONST` (0x2308) | base **256**, size 255 | our PS window (ALU 256..479) is right |
| `RB_COLOR_INFO` format | 0 = `k_8_8_8_8` | our `R8G8B8A8_UNORM` target matches |
| `PA_SU_SC_MODE_CNTL` (0x2280) | `00080008` — both cull bits **clear** | disabling culling is FAITHFUL here, not a shortcut |

That last one is worth stating loudly, because "no culling" was written into the
renderer as a deliberate simplification to be revisited. It turns out not to be a
simplification at all for this era: the title does not cull. It comes off the candidate
list rather than staying on it as an unknown.

**Index endianness is retired.** Scrambled triangles are the classic symptom of an
index buffer read with the wrong swizzle, and the stream carries two codes (0 on
229,449 draws, 2 on 150,614). `CZ_VK_INDEX_ENDIAN=N` forces one for all draws; the
faithful reading gives the 69.8% surface above, while forcing 1 gives **0.3%** and
forcing 2 gives **0.2%**. Applying the packet's own code beats both overrides by two
orders of magnitude, so the decode is right and the scrambling is upstream of it.

## 6d. `RB_MODECONTROL` 5 is depth-only, and we resolve the wrong buffer for it

The state probe also showed `RB_MODECONTROL` taking the value **5** — `kDepth`, a
depth-only pass — alongside 4 (`kColorDepth`) and 6 (`kCopy`, the resolve). That
explains the 38.6% of draws with an empty colour mask (§6), and it names a real gap:
those passes resolve a **depth** surface, and our resolve unconditionally snapshots the
**colour** target. Four of the black 1280x720 surfaces in §7's table are depth resolves
being served an empty colour buffer, so their blackness is our bug rather than evidence
about the scene.

## 6e. `numFormat=integer` is not a shader-side detail

`XenosVertexFormat` mapped Xenos format 6 (`8_8_8_8`) to `R8G8B8A8_UNORM` regardless of
the fetch's `numFormat` flag. **Normalized divides by the type's range**, so an integer
32 arrives in the shader as `32/255 = 0.125`, and a shader that `floor()`s it to index
something reads element 0 every time. Case Zero has **15 such attributes**.

Vulkan's `USCALED`/`SSCALED` are exactly the missing concept — an integer in memory
delivered as its own value into a *float* input, which a `*_UINT` format would not be —
so the shader needs no change. The scrambled geometry of §6b collapsed from
exploded-to-a-vanishing-point to compact on this change alone.

`GetPipeline` now also asks the device whether it can use each vertex format at all and
names any it cannot, because a pipeline built with an unsupported vertex format is
undefined behaviour that presents as wrong geometry rather than as an error. This
device supports all of them.

## 6f. THE SCENE IS RENDERED IN TWO TILES, AND THAT IS WHAT `SET_BIN_MASK_LO` MEANT

The single most valuable finding of the phase so far, and every earlier symptom was
downstream of it.

Counting draws per pass (`drawsThisPass` in the resolve trace) reframed the whole
investigation. It is the number that separates *"the pass rendered nothing because it
had no draws"* from *"the pass had 900 draws and they produced black"* — two completely
different investigations that look identical in a snapshot. At the title screen:

```
dest=1439B000 destPitch=4096 destHeight=1024 win=0,0..1024,1024      draws=111 verts=63801
dest=143DB000 destPitch=4096 destHeight=1024 win=0,0..1024,1024      draws=224 verts=156206
dest=06BE4000 destPitch=1280 destHeight=720  win=0,0..640,720        draws=930 verts=494667
dest=06BF8000 destPitch=1280 destHeight=720  win=640,0..1280,720     draws=108 verts=49061
                                             winoff=00007D80 (= -640)
```

The scene was never missing. **930 draws and 494,667 vertices were being rendered every
frame and thrown away**, because:

* **The Xbox 360's EDRAM is 10 MB and a 1280x720 colour+depth target does not fit.** The
  title splits the screen into two 640-wide tiles, renders each into a **640-pitch**
  EDRAM surface (`surfacePitch=640` against `destPitch=1280`), and resolves each into
  its half of one 1280x720 destination. That is what makes `SET_BIN_MASK_LO` the most
  frequent opcode in the entire stream — 2,353,460 of B1's 8,283,322 type-3 packets —
  a number this project has been quoting since phase 1 without knowing what it meant.
* **`PA_SC_WINDOW_SCISSOR` is the tile, in screen coordinates**, and
  `PA_SC_WINDOW_OFFSET` is the −640 hardware adds to bring the second tile's geometry
  down into the 640-wide surface. Our EDRAM image is full-screen-sized, so the offset is
  deliberately NOT applied — the geometry is already where we want it — but the scissor
  must be, or every tile paints the whole screen and the last one wins.
* **`RB_COPY_DEST_PITCH` is the SURFACE; the window scissor is the REGION.** Conflating
  them copied a 640x720 tile as if it were a full 1280x720 destination, which is what
  put every pass's content in the top-left corner at assorted sizes — the symptom §5
  originally diagnosed as a missing surface identity. §5's finding was right and
  incomplete: the destination address identifies the surface, and the window scissor
  identifies the part of it.
* **The two tiles of one surface share a key.** The second tile's `RB_COPY_DEST_BASE` is
  pre-offset *into the same allocation*: `06BF8000 − 06BE4000 = 0x14000`, and 0x14000 is
  exactly the 20 macro-tiles that 640 pixels of a 4-byte **tiled** surface occupy
  (20 × 4096). Keying on the raw base makes one surface look like two, so a consumer
  fetching the surface's real base gets a snapshot holding only the left half.
  Subtracting the macro-tile offset puts both halves in one image, which is what the
  guest's own memory layout does.

Measured: surface `06BE4000` went **4.8% non-black → exactly 50.0%**, the separate
`06BF8000` snapshot disappeared into it, and the surface now contains **recognisable
Still Creek 3D geometry** — buildings and structures — where it held three flat colours.

The lesson generalises past this title, and it is the one to carry to Case West: **an
opcode's frequency is a statement about the renderer's architecture.** `SET_BIN_MASK_LO`
was recorded as "the most frequent type-3 opcode" in phase 1 and treated as a
predication detail to get right; it was in fact telling us the title tiles its main
render target, which is the single most structurally important fact about how it draws.

## 6g. The pixel-shader constant buffer was 32 registers short

The single biggest fix of the phase, and it was a buffer size.

XenosRecomp's README documents the pixel shader constant window as **224 float4**
(3584 bytes) and this renderer believed it. The shaders it GENERATES do not:

```
#define pc(INDEX) select((INDEX) < 256,
    vk::RawBufferLoad<float4>(PixelShaderConstants + min(INDEX,255)*16, 0x10), 0.0)
```

So a shader reading `c255` loads from offset 4080 — 512 bytes past a 224-register
buffer, into whatever the frame arena allocated next.

Case Zero's scene pixel shaders read `c255` in their **final** instructions, as the
tone map's scale and bias (`shader_8C094EA15720629A`, 106,256 draws a run):

```
mul  r0.xyz, r0.xyz, c255.wwww
mad  r0.xyz, r0.xyz, c14.wwww, c255.xxxx
max  r0.xyz, r0.xyz, c255.zzzz
mul  r0.xyz, r0.xyz, c255.yyyy
```

A wrong `c255` there does not tint the scene. It **collapses every pixel to a
constant** — which is exactly what "930 draws producing three distinct colours" was.

And the guest states the true size itself, so this never needed the README at all:
`SQ_PS_CONST` reads base=256 size=255, i.e. ALU float4 registers 256..511 — **256
registers**. Sizing a constant buffer from a tool's documentation rather than from the
register the guest writes is the whole mistake, and it is the general lesson.

Measured, one run either side:

| surface | before | after |
|---|---|---|
| the scene `06BE4000` | 50.0% non-black, **3** colours | 63.8%, **848** colours |
| the 640x360 -> 32x1 pyramid | every level 0.0% | 12.5% – 64.6% |
| the 64x64 luminance chain | every level 0.0% | 53.2% – 100% |

**What found it** was a per-(vs, ps) draw census (`CZ_VK_SHADER_CENSUS=1`) naming the
shader doing the work, plus Xenia's disassembly of that exact shader — free, beside
every blob in the capture. Neither half is useful alone: the census says which shader
matters, the disassembly says what it was supposed to compute.

## 6h. g_SwappedTexcoords is a correction for something WE do

`g_SwappedTexcoords` is a bit per TEXCOORD semantic that the generated
`tfetchTexcoord` reads, and we were leaving it at zero.

It is not a guest concept. Vertex data is copied out of guest memory by dword-swapping
the whole stream, which for 32-bit components is exactly right and for **16-bit**
components also transposes the two halves of every dword — so a `16_16` attribute
arrives as YX and a `16_16_16_16` as YXWZ. The mask is how the shader un-transposes it.
Every 16-bit vertex attribute in the title had its components swapped, silently.

The semantic index comes from the Vulkan location, because that is what the container
synthesizer keyed both sides on (TEXCOORD0..3 are locations 4..7, TEXCOORD4..23 are
12..31). Measured: the scene went **63.8% -> 81.3% non-black, 848 -> 1,089 colours**.
`CZ_VK_NO_TEXCOORD_SWAP=1` is the same-binary arm.

## 6i. The fetch-slot convention, settled properly this time

§4 concluded that our fetch-slot reading is the hardware's and Xenia's disassembler
displays `95 - index`. That conclusion was right and the evidence for it was **weaker
than it looked**: the shader the probe happened to catch asked for slot 0 twice, and
both slot 0 and slot 95 were populated, so the observation was consistent with either
reading.

`CZ_VK_FETCH_SLOT_INVERT=1` is the version that cannot be ambiguous — read every fetch
constant at `95 - slot` and look at the geometry. Inverted, the scene surface is
**0.0% non-black**; upright it is 81.3%. Settled.

The method note is the one worth keeping: an experiment that is consistent with both
hypotheses has not tested either, and it is easy to mistake for a result because it
does produce a confident-sounding answer.

## 6j. An instrument that silently disables the thing it instruments

`CZ_VK_VALIDATION=1` on a machine without the Khronos layer made `vkCreateInstance`
return `VK_ERROR_LAYER_NOT_PRESENT`, which `VkRenderer_Init` treated as fatal — so the
run had **no renderer at all**, while the log said "validation layer requested". It now
retries without the layer and says so by name. Gotcha 7 in our own tooling again.

## 6k. RETRACTION: the scene-coverage percentage is not a stable metric

Sections 6b, 6f, 6g and 6h all quote "the scene surface went from X% non-black to Y%",
and those numbers are real **only where the change was large and structural** (0.0% to
69.8%; 3 distinct colours to 848). For anything smaller they are not usable, and this
was found the expensive way.

**The title screen renders an ANIMATED 3D background** (capture E's notes say so
outright: E3 is "the title's ANIMATED 3D BACKGROUND … this is why A4's idle log is ~69%
GPU"). A snapshot taken at frame 600 is therefore a different camera angle every run.

The primitive-restart experiment is the case in point. Xenos can pack many strips into
one draw separated by a reset index, and welded strips are an excellent fit for the
remaining defect — long thin triangles between unrelated parts of a mesh look exactly
like a broken vertex transform. One run each said enabling it made the scene worse
(81.3% → 67.6%) and I wrote that down as a result. Alternated three against three on the
same binary:

```
restart OFF   100.0%   64.1%   97.5%
restart ON     64.4%   94.8%   79.6%
```

Ranges that overlap completely. **The A/B is inconclusive, not negative**, and the
single-run version of it was noise wearing the shape of a finding — gotchas 50, 51 and
86 in a new place, and the fact that they are quoted all over this project did not stop
it.

Off stays the default because it is the pre-existing behaviour, and
`CZ_VK_PRIM_RESTART=1` is the arm. Deciding it needs a frame-aligned comparison rather
than a coverage percentage — which is the renderer's-side version of gotcha 38, the rule
that the GPU gate must never be frame-indexed.

**What a usable metric would be:** the same scene rendered from a pinned camera, or a
comparison against B1's own per-era draw aggregates, which is what the kickoff prescribed
for the GPU gate and which this phase has not yet built.

## 6l. RETRACTION: `abs(x) >= 0.0` was not the problem, and the test that says so

`oPos.w` comes from `r1.w`, which 21 of the boot era's 30 vertex shaders set with the
Xenos `sges` idiom — `ps = abs(r0.x) >= 0.0`, the compiler's "set w = 1.0". With the
draw probe's real numbers, that register decides between `w = +23.3` and `w = −108.2`:
vertices behind the camera, radiating from a point, which is precisely the symptom.

It was worth testing and it is not the cause. Rebuilding the whole boot-era cache with
that line replaced by a literal `ps = 1.0` — semantically identical unless `r0.x` is NaN
— gives **81.2% against 81.1%**: no change. The idiom evaluates to 1.0.

Kept because the technique transfers: `CZ_SHADER_SPV=<dir>` lets a hand-patched shader
cache be run against the same binary, which makes "is this generated line doing what it
looks like?" a five-minute experiment instead of an argument.

## 6m. THE METRIC — per-era aggregates, and the two designs that failed first

`docs/phase5-kickoff.md` prescribed "gate on per-era aggregates, never on frame index"
from the start. This section is what it took to actually build one, and the failures are
the valuable part because both looked like they were working.

### Design 1: compare the presented frame. Cannot see the defect.

`CZ_VK_FRAME_STATS=<file>` writes one line per presented frame — draws, vertices, a
draw-stream fingerprint, a camera fingerprint, and the output's coverage, mean luminance,
distinct-colour count and pixel hash.

Comparing that across arms is blind to the thing under investigation. At the title screen
the presented front buffer is the **logo era**: mostly UI, 2–36% covered. Disabling the
16-bit texcoord unswizzle — a change touching 476,858 draws a run — moved it by **0.1
percentage points**. The defect lives on the SCENE surface and the presented frame is the
overlay in front of it. Hence `CZ_VK_FRAME_STATS_SURFACE=<hex>`, which measures a named
resolve surface as well.

### Design 2: align frames by content. Passes perfectly, tests nothing.

The project already aligns the capture pair by content rather than frame index
(`tools/xtr_determinism.py`, finding 10). Applying the same idea — match frames on
(draw fingerprint, camera fingerprint), then compare pixels — produced a beautiful
result:

```
surface aligned       : 257
surface identical     : 257/257 (100.0%)
surface coverage delta: max 0.0000 pp, mean 0.0000 pp
```

**It is worthless.** All 257 aligned frames had coverage 0.00% and a single distinct
pixel hash: 257 copies of a black image. The only frames whose exact camera constants
recur across two runs are the ones where the scene is EMPTY — and the same 100.0% came
back for arms that visibly change the picture.

The general point, and it is the one to carry: **for a scene animated off wall-clock
time, no post-hoc alignment can work.** Exact alignment selects for stasis, which is
exactly the content least able to reveal a rendering difference. And a metric that looks
authoritative while testing nothing is worse than a noisy one — the noisy version at
least did not invite belief.

### What works: the median over the era

Aggregate instead of aligning. The **median** across every frame that has scene content
is stable because several hundred frames sample the whole animation cycle; the mean is
not, because it is pulled about by how long a run happened to spend in each part of the
cycle. Five runs of one binary:

| run | frames | median coverage | mean coverage |
|---|---|---|---|
| 1 | 620 | 64.44% | 70.81% |
| 2 | 582 | 64.34% | 64.33% |
| 3 | 617 | 65.70% | — |
| 4 | 630 | 64.45% | — |
| 5 | 564 | 64.56% | — |

**Median band: 1.36 pp.** The means over the same runs spread 64.3–70.8.

### And it has been shown capable of failing

Gotcha 30 is the whole point of the exercise, so the metric was tested against arms with
a known answer rather than declared working:

| arm | median coverage | verdict |
|---|---|---|
| baseline × 5 | 64.34 – 65.70 | the band |
| `CZ_VK_NO_TEXCOORD_SWAP=1` | 64.56 | inside the band — **no detectable effect** |
| `CZ_VK_PRIM_RESTART=1` | 81.44 | 17 pp outside — **detected** |

`tools/frame_compare.py` is the reader. It quotes the 1.5 pp threshold as a constant
rather than deriving it from the runs being compared, because a band computed from its
own inputs widens to accommodate whatever difference is present — which is exactly how a
metric stops being able to fail.

Median draws per frame is a second stable aggregate, and tighter: 1,612–1,632 across all
five baselines.

## 6n. RETRACTION: the texcoord unswizzle has no measurable effect on the picture

§6h reports the 16-bit texcoord unswizzle taking the scene from 63.8% to 81.3%
non-black. **That was the animated-background noise, exactly like the primitive-restart
claim retracted in §6k.** The metric built above puts the arm at 64.56 against a
64.34–65.70 baseline band: inside it, i.e. no detectable effect.

The change stays, because it is correct on its own terms — it implements the contract
XenosRecomp's generated `tfetchTexcoord` documents, and dword-swapping a vertex stream
provably does transpose 16-bit pairs. What is retracted is the claim that it improved
the picture, which was never measured.

Two of this phase's three "measured improvement" claims turned out to be noise from the
same source, and both were single runs of a metric nobody had validated. The structural
numbers are unaffected — 0.0% → 69.8%, and 3 distinct colours → 848, are an order of
magnitude outside the band — but they were lucky rather than rigorous, and the metric
now exists so the next one does not have to be.

## 6o. The exploded geometry: NOT FIXED, and the attempt is instructive

I set out to fix it with the metric in hand and did not. What the session established is
worth more than the attempt, because it invalidates the way the defect had been
characterised all along.

### The bisection, and why its result does not stand

`CZ_VK_ONLY_VS=<hex>` / `CZ_VK_SKIP_VS=<hex>` render only, or all but, one vertex
shader's draws. Rendering the top shaders one at a time appeared to localise the defect
immediately: `fa161b0fde7aa4d5` (118k draws) drew a clean wall and floor,
`70bb3e795f69d30c` (36k draws) drew the radiating spikes.

`70bb3e795f69d30c` is a SKINNED mesh shader, and it looked like a perfect suspect —
XenosRecomp's own README warns that "issues might happen when instructions perform
dynamic constant indexing on multiple operands", and this shader does
`vc(8 + a0)` / `vc(9 + a0)` / `vc(10 + a0)` nine times against a bone palette.

Every input to it checks out, and each was measured rather than assumed:

* bone indices `[0,0,9,6]`, **range 0..9 over all 479 vertices** of the mesh — nowhere
  near the 255 at which the generated `vc()` macro clamps to zero
* bone weights `[0,0,31,224]` → 0.122 / 0.878, summing to 1.0
* the palette is real: **73 distinct rows** across `vc(8..127)`, each an orthonormal
  rotation with a sane translation
* `vc(255)` is all zeros, so this shader's `sge r4.w, abs(r0.x), c255.x` idiom does
  evaluate to 1.0 and the translation column survives
* the odd asymmetric swizzle in the third bone block (`c[9+a0].wxzy, r3.wxyz`, where the
  other two blocks use `.xzyw`) is **in the guest's own microcode** — the translation is
  faithful
* the `maxas` co-issue ordering is right: the scalar that sets `a0` is emitted after the
  vector op of the same instruction slot, so each `mul`/`mad` uses the previous slot's
  `a0`, exactly as hardware pairs them
* all indices in the title are 16-bit — there is no index-width mismatch anywhere

Patching that one shader's cache entry to bypass the `vc()` macro (a direct
`RawBufferLoad` at an index clamped into range) made the picture look clean. **So did the
control** — recompiling the same shader UNMODIFIED, whose SPIR-V is byte-identical to the
cache entry.

### What was actually wrong: the observation, not the shader

Three runs of the identical configuration, same binary, same filter, produce three
completely unrelated pictures — one nearly black, two showing different large surfaces,
none showing spikes. **A single snapshot of this scene is a random sample of the
animation**, so every visual judgement in this investigation, including the bisection
that started it, was drawn from noise.

Run through the metric instead:

| cache | median surface coverage | verdict |
|---|---|---|
| baseline × 3 | 64.44 / 64.45 / 64.56 | the band |
| `vc()` macro bypassed in `70bb…` | 65.81 | **1.37 pp — at the band's edge, not a fix** |
| skinning disabled (`a0 = 0`) in `70bb…` | 100.00 | a huge change, as expected of breaking skinning |

So the macro-bypass "fix" is not supported, and the attribution of the defect to
`70bb3e795f69d30c` is **retracted**.

### The lesson, which is the third instance of one thing

§6k retracted a numeric claim for being a single sample of an animated scene. §6n
retracted a second. This retracts a *visual* one — and the visual case is worse, because
a picture feels like direct evidence in a way a percentage does not. The discipline has
to extend to looking at things: **for this title's title screen, one frame is one sample,
and three of them disagree completely.**

Which means the honest next step is not a better hypothesis but a better *scene*: make
the animation deterministic (a pinned camera, or a guest clock advanced per frame rather
than from the host TSC) so that a picture — and a bisection built on pictures — means
something. Everything above stays true and is worth keeping; none of it could be acted on
without that.

## 6p. CZ_DETERMINISTIC_CLOCK — built, honest about what it does and does not do

§6o's conclusion was that the next step is a deterministic SCENE rather than a better
hypothesis. This is the attempt, and it is a partial result reported as one.

`CZ_DETERMINISTIC_CLOCK=1` makes the guest clock advance a fixed quantum per PRESENTED
FRAME instead of tracking the host TSC. It covers **both** of the guest's elapsed-time
sources — `mftb` (via `cz_timebase::guest_ticks`) and `KeQueryPerformanceCounter`'s
interrupt time — because two clocks that are meant to agree have to come from one
source, and leaving the second on wall time would let the animation read it through the
back door while every other symptom said the mode was working. It steps at the PM4
executor's `XE_SWAP`, the same signal that drives the present seam, so the clock and the
picture advance together by construction.

It is off by default, announces itself loudly at startup, and must never be on for a
gate run: it changes what the guest observes about time, which is the subject of
findings 38-41. Same class of instrument as `CZ_FAKE_START_MS`.

**What it measurably does.** The clock is genuinely driving the animation: distinct
camera fingerprints per run drop from ~600 to ~270, i.e. the scene now advances in
coarser, frame-locked steps.

**What it does not do — yet.** It does not make the scene reproducible. Aligning each
run's camera sequence from its first scene-content frame:

| pair | identical cameras in sequence |
|---|---|
| deterministic, det1 vs det2 | **248 / 500 = 49.6%** |
| deterministic, det1 vs det3 | 1 / 500 = 0.2% |
| baseline, run A vs run B | 1 / 582 = 0.2% |

So one pair of runs agrees half the time — an enormous improvement over the 0.2% floor —
and the third run diverges completely. A second source of nondeterminism remains, and
the most likely candidate is the boot itself: how many frames pass before the scene
starts, and in what order the title's own load work completes, still vary with host
scheduling.

Three snapshots taken at frame 600 under the deterministic clock still differ
(coverage 45.68% / 100.00% / 63.19%), so **visual debugging of this scene is still not
sound** and §6o's retraction stands unchanged.

The honest state: the instrument is correct as far as it goes, it is measured rather
than assumed, and it is not yet sufficient. Finishing it means finding what else the
animation phase depends on — the obvious next probe is to log the guest's own frame
counter and the scene's start frame alongside the camera fingerprint, and see whether
the divergent run simply started the scene at a different point in its own logic.

## 6q. THE FRAME WAS UPSIDE DOWN, AND NO INSTRUMENT HERE COULD SEE IT

The operator ran the game, looked at the Blue Castle Games logo, and said "it is upside
down". That one observation fixed the title screen.

A Xenos vertex shader emits clip coordinates in **D3D convention, where +y is UP in
NDC**. Vulkan's NDC has **+y DOWN**. Passing the guest's clip position straight into a
positive-height viewport renders every frame vertically mirrored. The fix is a
negative-height viewport (core since Vulkan 1.1) on the viewport-transform path only —
expressed there rather than folded into a matrix so it cannot double up with the
window-coordinate path's `g_PosScale`/`g_PosOffset`, which does NOT need a flip because
the runtime builds that mapping itself.

**Why it survived the entire phase is the finding.** A vertical flip preserves coverage,
mean luminance, distinct-colour count and the full histogram — *exactly*. Every number
`tools/frame_compare.py` computes is invariant under it, so the metric built two sections
ago scores a flipped frame as **identical** to a correct one. It is not a weak
measurement of this defect; it is a blind one.

That is a sharper statement of §6m's lesson than §6m managed. The metric was built to
stop conclusions being drawn from noise, and it does that. It says nothing about
transforms of the picture — flips, rotations, mirrorings, channel swaps — and an aggregate
over pixel values never will. Catching those needs a *reference*, not a statistic: the E
screenshots, or an operator's eyes.

It also explains a symptom this document has recorded three times and never diagnosed:
"the content is in the upper-left corner". A vertically flipped frame puts a
bottom-anchored HUD at the top, and §5's tiling investigation, §6f's window-scissor work
and §7's table all describe the same picture partly through this flip.

**Result:** the title screen renders — the DEAD RISING 2 wordmark, the CASE ZERO stamp,
the blood streak — recognisably capture E2. `CZ_VK_NO_FLIP_Y=1` is the arm.

Still wrong on that screen: "PRESS START" and the copyright line render as solid blocks
rather than glyphs, which is the next thread and is a *text* problem, not a geometry one.

## 6r. Text rendered as solid blocks: the fetch constant's component SWIZZLE

Reported the same way as §6q, by an operator looking at the screen: "anything written is
weird squares".

The Xenos texture fetch constant carries a 12-bit **component swizzle** in dword3
(bits 1..12), four 3-bit fields saying which fetched component each of x, y, z, w takes
(0..3 = XYZW, 4 = constant 0, 5 = constant 1). The renderer ignored it entirely.

It is **runtime data**, which is why it has to be the runtime's job: a shader compiled
without the fetch constant cannot bake it in, so XenosRecomp emits a plain `Sample()`
and the mapping has to come from the image view.

Where it shows first is TEXT. A font atlas is a single-channel image, and the guest
routes that one channel to the component its shader reads — commonly alpha. Presented as
`R8_UNORM` with an identity mapping, Vulkan reads alpha as a constant **1.0**, so every
glyph samples fully opaque and the text renders as solid blocks of the right size in the
right place. The quad is correct and the sample is not, which is exactly why it reads as
a font problem rather than a texture-decode one.

The fix is free: decode the swizzle into a `VkComponentMapping` on the image view. No
data conversion, no shader change. `CZ_VK_NO_TEX_SWIZZLE=1` is the arm.

**Result:** "© CAPCOM CO., LTD. 2010 ALL RIGHTS RESERVED" is legible, and with §6q's flip
the title screen is now recognisably capture E2 — wordmark, CASE ZERO stamp, blood drips,
the trademark mark and the 2.

Same lesson as §6q, and worth stating once more because two consecutive defects shared
it: **neither of these was visible to any number this project computes.** A swizzle
changes which channel is sampled, so a solid white block and a correct glyph have
different coverage — but the metric was never pointed at the presented frame's text, and
no aggregate would have named the cause. Both were found in one minute of a human
looking at the game.

## 6s. THE SCENE WAS NEVER MISSING — ONE STALE CACHE ENTRY COMPOSED IT AWAY

Session 21 (2026-08-06), on the PM4 control arm as `docs/d3d-phase-c9-kickoff.md`
prescribed. This is the "3D background and DEAD RISING 2 wordmark are black on both
arms" item, and it is closed.

### The observation that reframed it: the title screen is TWO screens

Every phase-5 and phase-C claim about "the title screen" has been made from a single
dumped frame, and the frames dumped happened to be one of the two. Dumping every 64th
frame of a 170 s boot and measuring all 32 says so at a glance:

| frames | non-black | distinct colours | what it is |
|---|---|---|---|
| 448..1377, 1427..2048 | **2.31%** | ~880 | PRESS START and the copyright line, on black |
| 1378..1426 | 34–37.7% | up to 81,014 | **the DEAD RISING 2 CASE ZERO logo, fading in and out over 49 frames** |

The logo frame is a near-exact match for capture E2. So the renderer could already
produce a correct title screen and had been doing it for one frame in twenty, unseen.

And capture **E3** says what the other era is supposed to be: the title screen's
animated **Still Creek** 3D background, with the same PRESS START bar over it. The
title alternates the two. Our runtime rendered the logo era correctly — because E2's
background is black anyway — and the Still Creek era as pure black.

### The dependency graph, which is what actually localised it

`CZ_VK_RESOLVE_TRACE` already prints each pass's sampled snapshots (gotcha 140). Two
instrument defects had to be fixed before it could answer, both of them the same shape
as gotcha 109:

* the trace's 60-line cap guarded only the pass HEADER, so the two follow-up lines kept
  printing uncapped for the rest of the run — and the budget bought a different number
  of passes depending on the resolve order, so the frame's LAST pass (the front buffer,
  the one every "why is it black" question ends at) fell off the end. The budget is now
  in PASSES (`CZ_VK_RESOLVE_TRACE_PASSES`, default 20).
* the per-pass draw list was capped at 4, which says what KIND of pass this is and
  nothing about what a 57-draw compose did. `CZ_VK_PASS_DRAWS=N`.

With those, one frame of the title screen is 56 passes and reads as a clean chain:

```
scene colour 0684B000 (two 640 tiles, one key — gotcha 121)
   -> 14733000 bright pass -> 1476F000 -> 147AB000 -> 147BA000        (bloom)
   -> 1439B000  TONE MAP + COLOUR GRADE  <- 0684B000 + 3 bloom levels + LUT 14338000
   -> 147C0000 (DOF blur, <- 1439B000 + depth 06BE4000)
   -> 149A0000 (circle of confusion, <- depth)
   -> 00E48000 FRONT BUFFER  <- 1439B000 + 06BE4000 + 147C0000 + 149A0000, then 57 UI draws
```

Every link measured non-black **except one**: the tone map's own output at `1439B000`
was **0.00% non-black**, from a `0684B000` input that was 61.8% covered. One pass, one
fullscreen quad, live inputs, and black out.

`1439B000` is also a shadow-cascade destination earlier in the same frame — an address
this title reuses, which cost some time before the ordered pass list made it obvious
that the LATE resolve is the tone map's.

### The cause: the texture cache was consulted BEFORE the resolve snapshot

`UploadTexture` had, in this order:

1. look the fetch constant's six dwords up in `R->textures`; on a hit, return it;
2. if the fetch's address names a resolve snapshot from this frame or the last, return
   the snapshot — *"deliberately NOT cached, because a snapshot's contents change every
   frame while its fetch constant does not"* (gotcha 114, correctly stated in the code);
3. otherwise untile it out of guest memory and cache it.

Step 2's rule only holds for a surface whose **first** fetch already had a snapshot.
This title's colour-grading LUT is resolved LATE in a frame (pass 55 of 56) and sampled
EARLY in the next one (pass 46), so during the boot — before any pass had resolved it —
its first fetch fell through to guest memory, uploaded whatever the allocator had left
at `14338000`, and cached that under the fetch constant. **The fetch constant never
changed again**, so every subsequent frame took the cache-hit path, and the tone map
sampled a dead first-frame upload for the rest of the process.

The tone map's final instructions are two LUT lookups blended (`tfetch2D r4, r3.xy, tf4`
/ `tfetch2D r1, r3.wy, tf4`, then `mad_sat` / `max oC0`), so a black LUT is a black
frame. Nothing else had to be wrong.

**The fix is to check the snapshot before the cache**, which is what the comment already
said the rule was. `CZ_VK_TEX_CACHE_FIRST=1` is the same-binary control arm.

Measured, same binary, PM4 control arm, 170 s headless boots:

| | fixed | `CZ_VK_TEX_CACHE_FIRST=1` |
|---|---|---|
| tone map output `1439B000` | **95.3% non-black, 56,658 colours** | 0.00%, 1 colour |
| front buffer `00E48000` | **96.4%, 133,114 colours** | 2.31%, 880 |
| presented frame 1024 / 1920 | **99.4% / 99.5%** | 2.31% / 2.31% |
| texture fetches served from a snapshot | 544,818 | 492,238 |
| frames presented | 1,188 | 1,189 |

That is a structural change, not a metric shift (gotcha 127) — the scene now reaches the
screen at all — and it costs nothing measurable in frame rate.

### What this says about the instruments, which is the transferable part

Every instrument this project owns reported a healthy chain while the picture was black:
the LUT's own resolve snapshot was 99.9% non-black; the tone map's four other inputs
were live snapshots; its colour mask was `F`; its blend was `00010001`; its constants
were sane; no decline counter moved; `texture: cache hit` was 2.2 M and looked like
health. The one number that could have named it — `texture: resolve snapshot too old,
falling back to guest memory` — read **7** on the broken binary and **70,681** on the
fixed one, because on the broken binary the cache hit short-circuited before the
snapshot was ever consulted. **A counter downstream of an early return counts the times
the early return did not happen**, and its silence is unfalsifiable from inside.

What broke the deadlock was the per-pass dependency graph: not "is this pass healthy"
but "which link of the chain is the first black one". That question needs the passes in
ORDER, with their inputs, which is exactly what gotcha 140 was written for and what two
capped prints were preventing.

## 6t. THE EXPLODED GEOMETRY WAS DRAW_INDX READ ONE DWORD OFF

Session 21, immediately after §6s put the scene on the screen for the first time.

**The vanishing point named the cause.** Measuring the scene surface rather than
looking at it: the triangles converge at (640, 360) — the exact centre of a 1280x720
frame, i.e. NDC (0,0), which is where a triangle goes when its indices are garbage
rather than when its transform is wrong. Every input to that transform had already been
verified individually (§6i, §6c, §7), and all of those verifications stand.

**The packet.** `CZ_PM4_DRAW_TRACE=1` prints the raw DRAW_INDX body:

```
[pm4draw] init=21870006 prim=6 count=8583 i32=0 addr=15BF75E0 size=40002187  addr&3=0  size>>30=1  size&0xFFFFFF=8583
[pm4draw] init=00390006 prim=6 count=57   i32=0 addr=15B7E726 size=40000039  addr&3=2  size>>30=1  size&0xFFFFFF=57
```

`dword[n+1]` is a bare physical address; `dword[n+2]` is `endian:2 | 0 | count:24`. The
parser read the swizzle from the low two bits of the ADDRESS, which is how every OTHER
address in a PM4 stream carries it, and that one line held two defects:

* the swizzle came out 0 or 2 depending on nothing but alignment — 680,964 draws at 0
  and 468,683 at 2 over a boot — when the packet says **1 (8-in-16) on every draw**,
  the only swizzle that makes sense for a big-endian 16-bit index buffer;
* masking the low two bits off the address **moved it**. A 16-bit index buffer is
  2-byte aligned, so bit 1 is a real address bit (`15B7E726` above), and the ~40% of
  draws that have it set were read one index early.

The count appears in both dwords and they agree on every draw of a boot, which is the
packet's own proof that this reading of the size dword is right; the parser now checks
it every draw and says so if it ever stops holding.

**Result:** a recognisable Still Creek. `CZ_PM4_INDEX_ADDR_SWIZZLE=1` is the control arm.

**This RETRACTS §6c's retirement of index endianness** ("the packet's own code beats
both overrides by two orders of magnitude"). That A/B was scored on the scene-coverage
metric while the compose chain of §6s was dead, so all three arms were being measured
against a black frame. The arm was right to exist and was read against a broken oracle —
which is the same shape as gotcha 13, applied to our own earlier measurement.

## 6u. HALF OF EVERY CLEAR RECT WAS MISSING, TWICE OVER

With the geometry fixed, the title screen's background rendered inside **one triangle**
and nothing outside it. Two independent defects, both in how a *screen-space rectangle*
reaches the screen, and the second only became visible once the first was fixed.

### The fourth corner of a rectangle list

A Xenos rectangle list stores three corners and the hardware generates the fourth. The
expansion emitted `(v0,v1,v2)` and `(v0,v2,v1)` — the same triangle twice — and the
comment beside it claimed that was "correct for an axis-aligned screen rect". It is
correct for nothing: it covers exactly half of every rect.

That is not cosmetic, because these draws are the guest's per-pass **clear** — one at
the head of nearly every pass, 46,404 a boot — and 233,155 draws a boot clear
DEPTH ONLY (`RB_MODECONTROL` 5, empty colour mask). With half a rect uncleared, the
previous pass's depth survives there and rejects the whole scene behind it. The symptom
is a picture with a diagonal edge, which reads as broken geometry rather than a missing
clear.

The corners are measured, not assumed — the stream holds TL, TR, BR:

```
v0 = (0,0)    v1 = (64,0)    v2 = (64,64)
v0 = (0,0)    v1 = (480,0)   v2 = (480,512)
v0 = (960,0)  v1 = (1024,0)  v2 = (1024,1024)      <- a 64-wide strip of a 1024 surface
```

so the missing corner is `v0 + v2 - v1`. An index rewrite cannot name a vertex that does
not exist, so the fix builds a real four-record vertex stream per rect draw and
extrapolates record 3 per dword as a float — exactly what the hardware does. Non-float
attributes copy record 0 and count themselves; a multi-rect draw (none in this title)
falls back and counts itself. `CZ_VK_RECT_HALF=1` is the control arm.

### Window coordinates are in PIXELS; our EDRAM is at SAMPLE resolution

Even with whole rects, the scene tile is 640 columns wide and its clear covers 320.
The clear rect really is `(0,0)-(320,720)` — and the pass that issues it has
`RB_SURFACE_INFO = 0x0A020280`, i.e. **MSAA bits 16..17 = 2 = 4x**, while the scene's
own draws run at `0x0A010280` = 2x. On Xenos it is 4x that doubles a surface's WIDTH,
so 320 pixels *is* the full 640-sample tile. Mapped one-to-one it clears half of it.

`posScale.x` is now doubled for a 4x surface on the window-coordinate path; 42,749 draws
a boot take it. `CZ_VK_NO_MSAA_WINDOW_SCALE=1` is the control arm.

**The instrument that made this diagnosable is `CZ_VK_NO_DEPTH_TEST=1`** — an ARM, never
a fix. "This geometry was never submitted" and "this geometry was submitted and rejected
by depth left over from another pass" are the same picture and completely different
investigations, and one run separates them: with the depth test off the tile's coverage
goes from columns 0..320 to columns 0..640, which says the draws were there the whole
time.

### One correction that is honest about having done nothing

Window-coordinate draws are also moved to their tile's screen origin by undoing
`PA_SC_WINDOW_OFFSET`, because a window coordinate is relative to the EDRAM surface
while our EDRAM holds every tile at its true screen position. It is correct and its
counter reads **zero** over a whole boot — every window-coordinate draw this title
issues runs with the offset at 0. It is credited with nothing (gotcha 151).

### And one thing that looked like a fix and was a camera angle

`CZ_VK_FORCE_COLORMASK=1` produced a visibly better-textured scene, which was the
animated title screen resampled (gotcha 133) — very nearly this phase's fourth
retraction to the same cause. Settled by a counter instead of a picture: splitting
"empty colour mask" by `RB_MODECONTROL` gives **233,155** draws in depth-only mode (a
real Z prepass, needs no explanation) and **148,150** in colour mode — and that second
number is, to within noise, the count of the degenerate 1-index point draws that emit
`oPos = (0,0,0,1)` regardless. `RB_COLOR_MASK` is read from the right register.

## 6v. THE EMPTY RIGHT TILE IS BIN PREDICATION, AND THE NUMBERS ARE ON THE TABLE

The remaining defect, characterised but NOT fixed. Everything here is measured.

**First, a number that had to be withdrawn before it did damage.** The obvious framing
was "hardware issues ~2,540 draws a frame at this screen and we issue ~1,620, so we are
losing a tile's worth". Those are not the same quantity: the capture's figure counts
draw PACKETS in the stream and ours counted draws the renderer ACCEPTED. Counted at the
same instant of the same run, the command processor parses **1,971 draw packets a
frame** and hands the renderer **1,313** — because `gpu/pm4.cpp` implements ME
predication and **a third of this title's draw packets are discarded by the bin mask**
(1,039,423 of 3,113,236 over 1,579 frames). That mechanism was in the code and had never
been counted, so the missing draws had no line to appear on. `ring: ... draws=N
(predicated out=M)` is now always on.

**Second, where the discards land.** One frame's two scene passes:

| pass | window scissor | draws EXECUTED | vertices |
|---|---|---|---|
| left tile | 0,0..640,720 | **931** | 496,130 |
| right tile | 640,0..1280,720 | **23** | 9,219 |

**Third, the predication inputs themselves** (`CZ_PM4_BIN_TRACE=N`, 300,000 draw packets
of one boot, grouped by the pair the hardware compares):

| draw's `SET_BIN_MASK` | tile's `SET_BIN_SELECT` | overlap | count | outcome |
|---|---|---|---|---|
| `00000000FFFFFFFF` | `0000000080000003` | nonzero | 108,328 | run |
| `00000000FFFFFFFF` | `00000000FFFFFFFF` | nonzero | 81,079 | run |
| `0000000080000003` | `000000000000000C` | **0** | **74,773** | SKIPPED |
| `0000000080000000` | `000000000000000C` | **0** | **25,770** | SKIPPED |
| `000000008000000F` | `000000000000000C` | nonzero | 8,385 | run |
| four rarer pairs | | | ~1,300 | mixed |

So the two tiles select bins `{0,1,31}` and `{2,3}`, and in the `{2,3}` tile roughly
**8%** of the draws offered to it survive. That is the empty right half, exactly.

**What it does not yet say is whether the discards are right.** A title does not emit
74,773 draws per boot that can never execute, so either this title's geometry really is
almost all in bins 0/1 — possible, if the bins are not a left/right split at all — or
the mask/select comparison is wrong somewhere: the 64-bit assembly from the LO/HI
packets, the meaning of bit 31 (set in almost every mask and in one select but not the
other), or the ORDER (a mask read one draw late would produce exactly this shape, and
`SET_BIN_MASK_LO` outnumbers draw packets ~1.6:1 in B1, so the pairing is not obvious).

**The check to run first, and it is self-servable:** capture B1 contains the same
`SET_BIN_MASK`/`SET_BIN_SELECT`/`DRAW_INDX` stream, so replaying this predication rule
over B1 with `tools/xtr.py` and counting how many draws survive under each bin select
answers "is 8% what hardware does" without an emulator run. If hardware also keeps 8%,
the tiling is not what we think it is and the right half's content comes from elsewhere.

`CZ_PM4_NO_PREDICATION=1` exists as the arm and is **not** a candidate fix — it is
destructive (a boot with it on renders nothing and only reaches 257,395 draws in 2,905
frames), because running a packet the hardware skipped puts one tile's geometry in
another tile's pass and corrupts the state stream with it. It is there so that "this
pass had 23 draws" and "this pass had 900 and 877 were predicated away" stop being the
same picture.

## 6w. THE RIGHT TILE: the RULE is right, the MASK is an UNPATCHED PLACEHOLDER

> **PARTLY RETRACTED BY §6x (session 23).** The first half of this section stands and is
> load-bearing: the ME rule is right, the capture replay is right, and the wrong number
> is the MASK VALUE standing at the right tile's draws. The second half does not. The
> patch pass is NOT gated shut and does NOT patch zero records — it runs 1,751 times a
> boot over 388,451 records, and "ran once, patched zero" was the probe's schedule
> (call #1, then every 20,000) rather than a count. `0x80000000` is not the placeholder
> either; it is a deliberate trailing reset, and the placeholder is the LEADING
> `FFFFFFFF`. The real defect is one packet upstream of everything below — see §6x.


Session 22 (phase C part 10). §6v ended with a check to run and no emulator needed:
replay the predication rule over capture B1 and count how many draws survive under each
bin select. Done — `tools/xtr_bin_predication.py` — and the answer retires every
hypothesis §6v listed.

### The capture's answer: hardware discards 0.3%, we discard 33%

Over all **24,527,474** packets of B1, replaying SET_BIN_MASK/SELECT and the ME's
`(header & 1) && (mask & select) == 0` exactly as `gpu/pm4.cpp` performs it:

| | draw packets | discarded by the bin rule |
|---|---|---|
| **B1 (hardware)** | 1,643,219 | **5,240 — 0.3%** |
| ours, one 170 s boot | 3,644,332 | **1,195,021 — 33%** |

and in the capture the two tiles are offered **exactly 575,744 draws each** and each
keeps **573,124 — 99.5%**, perfectly symmetric. So the rule is not wrong, the 64-bit
LO/HI assembly is not wrong, bit 31 is not a special always-pass, and the ordering is
not off by a draw. All four of §6v's candidates are dead.

The tool is worth keeping for the general reason: our command processor is the suspect,
so it cannot be its own oracle, and a Xenia capture records a `PacketStart` for every
packet *before* predication is evaluated — so the skipped packets are in there and the
rule can simply be replayed. Any title with a capture has this oracle available.

### Where it is wrong: the pair table

Our runtime now prints the same table (`CZ_PM4_BIN_CENSUS=1`, on the ring trace next to
`predicated out=`). Side by side — hardware is the whole of B1, ours is one 170 s boot,
and the absolute counts differ between our own runs because how far a boot gets in fixed
wall time is a distribution (gotcha 75); it is the SHAPE that is being compared:

| bin select | hardware (all of B1) | ours (run A) | ours (run B) |
|---|---|---|---|
| `80000003` (left tile) | mask `FFFFFFFF` x570,504, all kept | `FFFFFFFF` x1,290,325, all kept | `FFFFFFFF` x1,382,604, all kept |
| `0000000C` (right tile) | mask **`8000000F`** x570,504, all kept | **`80000000`** x893,687 **SKIPPED** | **`80000000`** x1,040,207 **SKIPPED** |
| | | **`80000003`** x291,336 **SKIPPED** | **`80000003`** x239,063 **SKIPPED** |
| | | `8000000F` x100,325, kept | `8000000F` x97,115, kept |

Run B is the one the probe numbers below come from, so the two are directly comparable.

The left tile matches hardware in shape exactly. The right tile does not, and the whole
difference is two mask values hardware never has standing at a draw.

### `80000000` is not a computed answer — it is the placeholder

`CZ_PM4_BIN_TRACE` now logs the mask WRITES in stream order, not just the draws, because
"the wrong value" is a question about which writes landed. `CZ_PM4_BIN_TRACE_ARMMASK=8000000F`
holds the budget until the mature tiled era — the prologue is packet-identical on both
sides and says nothing, which is itself worth knowing. Over the same era:

```
hardware   MASK_LO 8000000F ; DRAW -> run ; MASK_LO 80000000 ; ... ; MASK_LO 8000000F ; DRAW -> run
ours       MASK_LO 80000000 ; MASK_LO 80000000 ; DRAW -> SKIP
```

Same bracket, same structure, one different number. And `0x80000000` is a **literal**:
the three UP-draw emitters build it with `lis rX, -0x8000` and store it straight after
the `0xC0006000` header (82842A18, 82842DE0, 8284322C).

The real mask is patched in afterwards, **in place**, by the D3D worker:

```
sub_8284B568 (token interpreter) -> sub_8284B228 (this stream's dispatcher)
    -> sub_8284A900 -> sub_8284A7F8
```

`sub_8284A7F8` is the only rect-to-bin-mask computation in the 8.8 MB image. It walks a
list of 16-byte `{record*, x0, x1-1, y0, y1-1}` entries — the rect is in units of 8
pixels, hence its `<<3` — intersects each against the tile rects at `tileInfo+8`,
accumulates `(acc << 2) | 3` per overlapping tile, ORs in bit 31, and stores the result
at `record+8`: exactly the dword the emitter left as `0x80000000`. `8000000F` is what
that computation returns for "this draw touches all four bins".

### And the patch is behind a gate that is closed

> **RETRACTED (§6x).** The gate is OPEN — 3,496 of 3,497 dispatcher entries — and
> `[obj+0x164]` is not a flags word at all: it is the CURRENT BIN SELECT, published by
> `sub_8284A6D0` out of `sub_8284A668`, which sets bit 31 exactly when the tile index
> is 0. So the test below reads "are we recording the first tile", and the patch runs
> once per multi-tile recording, as designed. The reading below came from a probe that
> printed only its first call.

`sub_8284B228`'s case for that token is

```
lwz     r11, 0x164(r30)
rlwinm. r11, r11, 0, 0, 0     ; bit 31
beq     skip                  ; clear -> the patch never runs
bl      sub_8284A900
```

with the token consumed either way, so a clear bit is completely silent — the same shape
as part 5's display-controller gate (gotcha 154), and one token handler in the same
dispatcher writes `0x7FFFFFFF` there (8284B3C4), i.e. bit 31 clear.

Measured, PM4 control arm, one 170 s boot, `CZ_BINMASK_PROBE=1`:

| | count |
|---|---|
| `sub_8284B228`, the dispatcher | **1**, `[obj+0x164]=00000000`, bit 31 clear |
| `sub_8284A7F8`, the patch pass | **1**, patched **0** records |
| `sub_82838088`, the other mask setter | **1**, mask 0, from `sub_82841AD0` |
| draws executed with `80000000` against the right tile | 1,040,207 |

That is consistent with part 7's independent observation that the D3D worker is never
used at all for the whole healthy era (`kicks=0`, `queued=0` at all 986 segment submits).

### The draw arm, re-gated on the way past

Not a renderer finding, but it belongs next to these numbers: part 9 could not re-gate
the phase C draw arm and part 10 did. Six serial 170 s boots, all six reaching **#83
`skeleton\cinezombie.big`** with `arms:ints = 0.9998`, `walks == kicks == drains`,
`distinct=816-911`, engine counter `dev+0x2B04` = 0, `truncated=0`, an exact 84-prefix on
A1 and A5 exit 0 — inherited free from parts 8 and 9. Details and the retraction that
goes with it in `docs/d3d-translation-plan.md` "Phase C part 10" §3.

### Stated as open

The placeholder story explains the 1,040,207 and is **not yet shown to explain the
rest**: the same boot has 97,115 draws carrying `8000000F` and 239,063 carrying
`80000003` at the right tile, and with the patch pass having touched zero records those
values came from somewhere this session did not identify. Recorded rather than smoothed
over, because a partial explanation quoted as a whole one is how §6c's index-endianness
retirement survived a phase (gotcha 172).

The probe reads the masks BACK out of the records the pass patched rather than
recomputing the arithmetic, so it cannot merely agree with itself.

## 6x. THE RIGHT TILE WAS A PACKET WE NEVER IMPLEMENTED: the SCREEN EXTENT query

Session 23 (phase C part 11). §6w's open item — where the `8000000F` and `80000003`
masks came from "given the patch pass touched zero records" — dissolved, because the
premise was wrong. **The patch pass runs 1,751 times a boot and patches 388,451
records.** §6w's "ran once, patched zero" was the probe's *first* line and nothing
else: it printed at call #1 and then every 20,000, and the pass runs a few thousand
times, so the only line any run ever emitted was the one whose counts are all 1 by
construction. Gotcha 109 — a capped emitter is not a count — for the third time in this
project, and the second time inside our own instruments. Both probes now report on a
15-second CLOCK.

### What the pass actually computed

With a real census (`CZ_BINMASK_PROBE=1`, PM4 control arm, one 170 s boot):

| what the fix-up pass wrote | records | share |
|---|---|---|
| `80000000` — touches NO tile | 294,787 | **75.9%** |
| `80000003` — tile 0 only | 58,624 | 15.1% |
| `8000000C` — tile 1 only | 2,450 | 0.6% |
| `8000000F` — both tiles | 32,590 | 8.4% |

which is the census's right-tile row-for-row (1,125,183 skipped on `80000000`, 216,412
on `80000003`, 124,602 kept on `8000000F`). So the pass ran, and computed *empty*.

Its two inputs are the tile rects and each record's rect. **The tile rects are
perfect** — the probe prints them: `tiles=2  tile0=0,0..640,720  tile1=640,0..1280,720`,
the exact left/right split. The record rects are garbage: `50176,17408..0,0`,
`0,0..8978,65535`, and a dominant degenerate `0,0..0,0` — uninitialised memory.

### Whose job was it to fill them

The draw block the three UP emitters lay down is

```
SET_BIN_MASK_LO  FFFFFFFF        <- the patch TARGET, at record[0] + 8
DRAW_INDX (predicated)
SET_BIN_MASK_LO  80000000        <- a trailing reset, "tile 0 only"
EVENT_WRITE_EXT  {0x1A, &record+4}
EVENT_WRITE      {0x19}
```

`0x80000000` was never the placeholder (§6w's reading, retracted): the placeholder is
the LEADING `FFFFFFFF`, and `0x80000000` is a deliberate trailing reset that confines
the two bookkeeping packets after it to the first tile's pass. The record is sixteen
bytes: `{stream cursor, u16 minX, maxX, minY, maxY, minZ, maxZ}` — four bytes of
pointer and twelve of extent, in units of 8 pixels, and **the GPU writes the extent**.
That is what `EVENT_WRITE_EXT` event `0x1A` is: the Xenos screen-extent query, dumping
what was rasterized since the matching `EVENT_WRITE 0x19` into the record.

Our command processor decoded that packet and did nothing with it. The fence family's
handler stores only when a packet carries a value dword (`bodyCount >= 3`), and this
form carries an address and no value. The capture says there is exactly one form of it
in this title: over B1's first 6,000,001 packets, `EVENT_WRITE_EXT` is **818,507**
packets, every one `body=2, event=0x1A`, against 819,953 `EVENT_WRITE body=1
event=0x19`. One pair per draw block.

So: extent never written -> fix-up pass intersects uninitialised memory against the
tile rects -> 76% of records come back "touches no tile" -> the right tile's pass
discards them. Four layers between the missing packet and the symptom, and every layer
in between was working correctly.

### The fix, and what it writes

`WriteScreenExtent` in `gpu/pm4.cpp` (and its twin in `gpu/d3d_draw.cpp`, sharing one
env arm so the two streams cannot decode this differently). We do not rasterize on the
command-processor thread and cannot know what a draw covered, so we write the
conservative answer — an extent larger than any tile, i.e. "this draw may have touched
anything". That makes bin predication a no-op rather than a wrong filter, and it can
only ever cost work: a draw offered to a tile it does not touch is rejected by the
scissor. A too-SMALL extent silently deletes geometry, which is the failure being
fixed. The layout is fixed by the pass's own four comparisons, and the halfword order
by the address's endian code (1 = 8-in-16), which `StoreGpu` already applies — passing
the pair the other way round transposes minX with maxX and reads as an empty rect.

### Measured, same binary, `CZ_PM4_NO_SCREEN_EXTENT=1` as the control arm

| | control (pre-fix) | **fixed** | B1 (hardware) |
|---|---|---|---|
| fix-up pass output | 76% `80000000` | **100% `8000000F`** (259,844 of 259,844) | — |
| draw packets discarded by the bin rule | 1,352,721 of 4,141,388 = **32.7%** | **7,853 of 2,761,594 = 0.28%** | 5,240 of 1,643,219 = **0.3%** |
| right tile (select `0000000C`) | `80000000` x1,125,183 SKIPPED | `8000000F` x974,779, **kept 100%** | `8000000F` x570,504, kept 100% |
| left tile (select `80000003`) | `FFFFFFFF` x1,472,725, kept | `FFFFFFFF` x975,698, kept | `FFFFFFFF` x570,504, kept |
| the two tiles' offered counts | 1,472,725 vs 1,466,725 | 975,698 vs 974,779 | 575,744 vs 575,744 |

The residue is symmetric and legitimate: the per-tile clear loop at 82844180 emits
`SET_BIN_MASK_LO 3 << 2i` per tile, so each tile skips the other's clears — 3,928 and
3,925 in the run above. **0.28% against hardware's 0.3%** is the first time this
port's predication has agreed with the capture.

Zero `GPU store outside the physical arena DROPPED` lines, which had to be checked:
the guest builds a PHYSICAL address for that write, and a record outside our physical
arena would have had the store dropped and the pass would still be reading rubbish.

### The picture, and a metric that cannot be fooled by a transform

`CZ_VK_FRAME_DUMP` on both arms, coverage measured PER HALF of the presented frame —
which is the right unit here, because the defect is a screen-space split at x=640 and a
whole-frame aggregate would blur it into a single number (and, per gotcha 135, an
aggregate cannot see a transform at all; a per-half one at least localises):

| frame | control: left / right | **fixed: left / right** |
|---|---|---|
| 448 | 100.0% / **32.3%** | 100.0% / **98.1%** |
| 576 | 100.0% / **32.6%** | 99.5% / **99.5%** |
| 768 | 98.9% / **59.9%** | 99.7% / **99.5%** |
| 1024 | 96.3% / **60.4%** | 99.8% / **96.5%** |

The left half is unchanged between the arms and only the right half moves, which is
exactly the shape the fix predicts and is not something a global change could produce.
The control arm's frame is the Still Creek title screen with a black rectangle from
x=640 to x=1280; the fixed arm's is the whole scene — the gas station, the power lines,
the zombie, the grass, and on the right the "Still Creek ELEV 2" road sign that had
never been rendered by this port.

Against capture E3 with `tools/frame_signature.py`, **every dumped title-screen frame's
best orientation is `identity`** (+0.42 to +0.55, beating the runner-up by 0.25), so
nothing is mirrored or rotated. None reaches the tool's +0.70 match floor, which is
expected and is part 12's first item: E3 is one moment of an animated camera and our
frames are others (gotchas 127, 133).

### Two method notes

* **The capture's stream window is a packet-level oracle, and it was one command
  away.** `tools/xtr_bin_predication.py --trace-window N --trace-arm-mask 8000000F`
  prints the same window `CZ_PM4_BIN_TRACE` prints for our runtime. Hardware's reads
  `MASK_LO 8000000F ; DRAW -> run ; MASK_LO 80000000 ; MASK_LO 8000000F ; DRAW -> run`
  — the block structure above, with the leading mask PATCHED — while ours read
  `MASK_LO 80000000 ; MASK_LO 80000000 ; DRAW -> SKIP`. Two consecutive `80000000`
  writes is not "the mask is a placeholder", it is "the leading mask was computed and
  came out empty", and the difference is visible in four lines.
* **A packet we implement and a packet we implement for every FORM it takes are not
  the same claim.** `EVENT_WRITE_EXT` has been in the opcode table since phase 1, has
  a name, appears in the census, and passed both capture oracles — because
  `pm4_packet_lengths.py` and `pm4_indirect_walks.py` check our *arithmetic*, and our
  arithmetic was right. It executed 818,507 times a boot and did nothing. Gotcha 88
  again: when every check of a computation passes and the result is wrong, the inputs
  are wrong — and here we were the ones failing to produce them.

## 6y. THE LINE DOWN THE MIDDLE: one constant, one column, and no aggregate could see it

Reported by the operator on a live run, immediately after §6x made the right tile
render. A faint full-height line down the centre of the picture.

**It is the half-pixel offset.** The translated shaders add `g_HalfPixelOffset` to
every vertex's clip position and the runtime set it to -0.5 px, for the D3D9-vs-modern
pixel-centre convention. That shift is not what the convention difference needs:

* on Xenos a screen-space rect `[0, W]` samples pixel centres at the **integers**
  `0..W-1` and covers W pixels;
* on Vulkan the same rect samples centres at `0.5..W-0.5` and covers W pixels.

The coverage already agrees. Shifting half a pixel on top moves the rect to
`[-0.5, W-0.5)`, and the last pixel's centre then lands **exactly** on the exclusive
right edge, where the top-left fill rule drops it.

The scene's left tile clears screen 0..640, so its column 639 was covered by nothing at
all. Measured on the resolved scene surface (`CZ_VK_SNAP_DUMP`), sky band, same binary:

| | all-black columns | sky[639] |
|---|---|---|
| `CZ_VK_HALF_PIXEL=1` (the old value) | **[639]** | 0.0 |
| default since part 11 | **[]** | 128.3, flat across the join |

`frame_compare.py` over the era, two runs per arm alternated: 99.48 / 99.48 against
99.61 / 99.62 median scene coverage — a 0.15 pp spread, **inside** the 1.5 pp band, so
no regression. The 0.13 pp is in the right direction and about the right size for one
column in 1280, and it is inside the band, so it is not claimed.

### Why this one is worth a gotcha

**One column in 1280 is 0.08% of the frame, and every aggregate this project owns is
blind to it — but a human sees it instantly, because the frame's blur AMPLIFIES it.**
A single black column, convolved with the scene's depth-of-field kernel, becomes a
~19 px darkening: measured in the presented frame's sky band it runs x=629..648 with a
minimum of 134.8 against a flat 150.1 either side. So the defect is 0.08% of the pixels
and 1.5% of the width by the time it reaches the screen.

That is the mirror image of gotcha 135 (the frame was upside down and no statistic could
see it). There the aggregate was blind to a transform; here it is blind to a defect too
small to move a median — and in both cases the operator's eye was the instrument that
worked. The lesson is not "trust eyes over numbers": it is that a metric aggregating
over pixels cannot see a defect whose extent is a single column, and the metric that
CAN is a within-surface structural one. `all-black columns in the resolved surface` is
that metric, it is deterministic, it needs no era alignment, and it reads 1 -> 0.

## 6z. Still open, and now measurable: the menu panels

**MEASURED IN PART 12 — read §6aa.** Both halves are localised to a named object there,
and the third bullet below (§6s's shape) is refuted: the panel samples no surface our
renderer failed to produce. The rest of this section stands as written.

Also from the operator's run, and **not fixed**: on the new-game screen, three panels
render as solid black rectangles and some label text is malformed, while the body text
on the loading screen next to it renders perfectly.

The reason it is only recorded here is gotcha 103 in a new place — that screen is two
menu levels past the title, and `CZ_FAKE_START_MS` presses only START, so no headless
run could reach it. `CZ_FAKE_PRESS_SEQ=START,A,A` fixes that: a headless run now walks
title -> logo -> menu -> loading screen, and part 12 can dump and measure those frames
without an operator.

What is already known, and it narrows the hunt:

* **the glyph pipeline is not broken.** The loading screen's tip text renders crisply
  and correctly in the same run, so this is not §6r's swizzle defect returning.
* the black rectangles are large filled areas, not thin ones, so they are not the
  §6y class of coverage defect.
* the shape to check first is §6s's: a panel that samples a surface which our renderer
  never wrote is served whatever the guest's allocator left there, and the per-pass
  dependency graph (`CZ_VK_RESOLVE_TRACE` + `CZ_VK_SNAP_DUMP`, gotcha 140) is what
  turns "this pass is black" into "this pass's input was never produced".

## 6aa. The menu panels, measured: two defects, both localised, neither yet fixed

Session 24 (phase C part 12). §6z handed over "three black panels and some malformed
label text on the new-game screen", first reachable headless via
`CZ_FAKE_PRESS_SEQ=START,A,A`. Both are now localised to a named object, both by arms
rather than by reading, and the two are unrelated to each other.

The screen: `CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A` on the PM4 arm walks
title -> logo -> menu -> **save-slot panel** (about ten frames around frame 570 of a
70 s boot) -> loading screen. The panel is drawn in the frame's LAST pass — a
115-draw, 835-vertex compose straight into the front buffer `00E48000` — which is why
no snapshot dump could ever show it in isolation.

### The three black rectangles are ONE texture, and they are drawn OVER correct content

The per-pass draw list with texture addresses (`CZ_VK_PASS_DRAWS=140`, which had to be
implemented first — see below) breaks that compose into three families:

| family | draws | textures |
|---|---|---|
| `vs=d6aceb89.. ps=27af67a3..` prim6, 4 idx | ~40 | one UI quad each: `037xxxxx`/`038xxxxx`/`039xxxxx` |
| `vs=2067afd5.. ps=e93897ec..` prim13, 4..60 idx | ~20 | **two** glyph atlases, `007BB000` and `007C6000` |
| `vs=8bb7e189.. ps=438c2af8..` prim1, 1 idx | 50 | none |

Three of the first family bind `0364B000` — a 16x16 DXT1 whose every texel reads zero.
`CZ_VK_SKIP_TEX=0364B000` (767 draws filtered over a boot) removes exactly the three
black rectangles, **and what appears underneath is three correct thumbnails**. So the
draws behind them are right, the compose order is right, and the defect is one opaque
black quad painted on top.

What it is not: those draws carry `mask=F blend=07060706`, i.e. `SRC_ALPHA` /
`ONE_MINUS_SRC_ALPHA` with an ADD combine, which our pipeline does honour — so a
transparent texture would be invisible. An all-zero DXT1 block is opaque black under
BC1 (both colour endpoints zero selects the punch-through mode, and index 0 still picks
colour0, alpha 1), so it is opaque here and would be opaque on hardware given the same
bytes. **Therefore hardware's bytes differ from ours**, and the open question is who was
supposed to write them. `CZ_PM4_MEM_WATCH` is a GPU-store instrument and will not see a
CPU write; the tool for this is gotcha 143's — a hardware watchpoint on that guest word
under `gdb -p` in a healthy run, which names the writer in one hit.

### The obvious repair was built, measured and REFUTED — and its first version lied

A texture uploaded before the guest streamed its pixels in is black, and the cache key
is the fetch constant, which does not change when the data arrives. That is a real
defect class, it is §6s's shape one level down, and it is wrong here.

The first version of the test asked whether guest memory at the texture's address is
still zero, and reported **39 textures uploaded black whose memory reads NON-ZERO by
the end of the run**. That number is an artifact. A 16x16 DXT1 occupies **128 bytes**
inside an **8 KB** tiled footprint (`srcPitchUnits` 32 x `srcRows` 32 x 8), so scanning
the footprint mostly asks about the NEIGHBOURING textures. Asking the same question of
the same texels the untile actually reads gives **zero** — none of a boot's 58 all-black
uploads ever becomes non-zero.

The repair built on the bad number is worth recording too, because it looked like it
was working: it fired 3,258 times in a boot and turned the save-slot panel **entirely
white**. Two independent faults, one in each half:

* the predicate and the re-check tested DIFFERENT byte ranges, so an entry whose padding
  was non-zero flipped between "arrived" and "not arrived" every frame, forever;
* each re-upload allocated a fresh bindless slot, so the 4096-entry heap ran out —
  **62,619 fetches served the 1x1 dummy**, which is what the white panel was.

Both are gotcha 30 in the same shape: the arm had never been shown capable of failing.
The machinery is not in the tree; what remains is one counter, `texture: uploaded
entirely BLACK (the guest has not written it)` (58 a boot), because that is what named
`0364B000`.

### The malformed text is one of two glyph atlases, and it is NOT the atlas

The two text atlases differ in exactly one field:

```
s0=007C6000 376x376 fmt=2 swz=124 tiled=1 pitchBlk=12 end=0    <- garbled, cyan
s0=007BB000 184x184 fmt=2 swz=124 tiled=1 pitchBlk=6  end=0    <- crisp, white
```

Same shader pair, same format, same swizzle, same tiling, same endian. `CZ_VK_SKIP_TEX`
attributes them: skipping `007BB000` removes the crisp white run of "TIP: Items with
the wrench icon..." and leaves the cyan garbage, so the two runs of one line are drawn
from two different atlases and only the 376 one is wrong.

Two hypotheses tested, both refuted, both with an arm that demonstrably engaged:

* **stale dynamic atlas.** These are runtime-rasterized glyph caches, so the cache could
  be holding the sheet as it stood at its first fetch. `CZ_VK_TEX_REFRESH=007C6000`
  re-reads the pixels on every fetch into the same image and slot — 2,250 refreshes in a
  boot — and **the picture does not change by one pixel**. Guest memory there is static.
* **our untiling.** `CZ_VK_TEX_DUMP` writes the untiled bytes as a PGM, and the
  376x376 sheet is a **clean, correctly laid out page of glyphs** — as is the 184x184
  one. Nothing is scrambled on the way in.

So the atlas is right and the draw samples it wrong, which leaves the texture
COORDINATES. The suggestive number is that **376/184 = 2.04** and the garbled glyphs
read as magnified fragments; the next instrument is a draw probe over the vertex data of
a draw isolated with `CZ_VK_ONLY_TEX=007C6000`. Recorded as a lead, not a conclusion.

### The instruments, and one that was documented for a phase without existing

`CZ_VK_PASS_DRAWS=N` has been in `CLAUDE.md`'s instrument list since part 9 with a
default of 4 — and **was never implemented**; the 4 was a hardcoded literal and the
environment variable was read nowhere. That is gotcha 25's shape in the documentation
rather than in a grep: an instrument nobody could have used, described as if they had.
It works now, and each entry carries the draw's texture address, because a UI compose is
a hundred quads sharing two shaders and the shader is not what distinguishes them.

Also added: `CZ_VK_SKIP_TEX` / `CZ_VK_ONLY_TEX` (the bisection arm one level below
`CZ_VK_ONLY_VS`), `CZ_VK_TEX_CENSUS` (per address: uploads, all-black uploads, snapshot
hits, too-old fallbacks), `CZ_VK_SNAP_FRAME` (the snapshot dump's frame was a hardcoded
600), and `CZ_VK_FRAME_DUMP_EVERY` (the panel appeared in exactly one frame of a 180 s
boot at the built-in 64).

## 6ab. The UI's whole text layer was ONE run repeated: VGT_INDX_OFFSET

Session 25 (phase C part 13). §6aa handed over two localised menu defects and one
conclusion about the second of them: "the atlas is right and the draw samples it wrong,
which leaves the texture COORDINATES". The coordinates are right too. So is the atlas,
so is the shader, so is every field of the fetch constant. What was wrong is which
VERTICES the draw read, and the register that says so had been read by an instrument
and applied by nothing.

### The observation the probe was not built to make

`CZ_VK_ONLY_TEX=007C6000` isolates the garbled atlas's draws, so the obvious next step
was `CZ_VK_DRAW_PROBE` over them. The probe printed one dword per vertex for a
non-position attribute — which for a `32_32_FLOAT` texture coordinate is `u` and not
`v` — and the two atlases' draws agreed on every printed value. That looked like a
coincidence worth checking rather than a finding, so the probe grew: every COMPONENT
decoded as a float, `CZ_VK_DRAW_PROBE_VERTS` to cover more than one glyph, and the
bound texture and its dimensions on the header line.

With that, three draws of one frame — different index counts, different atlases, drawn
at different places on screen — read **bit-identical vertex data**:

```
[vkprobe] frame=560 ... indexCount=16 tex=007C6000 376x376
[vkprobe]   loc4 fmt=37 off=3: v0(0.34309,0.45479) v1(0.34309,0.55319) v2(0.41755,0.55319) ...
[vkprobe] frame=560 ... indexCount=48 tex=007C6000 376x376
[vkprobe]   loc4 fmt=37 off=3: v0(0.34309,0.45479) v1(0.34309,0.55319) v2(0.41755,0.55319) ...
[vkprobe] frame=560 ... indexCount=24 tex=007BB000 184x184
[vkprobe]   loc4 fmt=37 off=3: v0(0.34309,0.45479) v1(0.34309,0.55319) v2(0.41755,0.55319) ...
```

Those numbers are not noise and they are not wrong: `0.34309 x 376 = 129.0`,
`0.41755 x 376 = 157.0`, `0.45479 x 376 = 171.0`, `0.55319 x 376 = 208.0`. Every UV in
the buffer is an exact texel of the 376 grid, and the cells are 28x37, 26x37, 32x37,
30x37 — variable-width glyphs of one font sheet. Perfect data, read by every draw.

### The register

The vertex fetch constant's ADDRESS is the same for every one of the 115 draws in that
pass and alternates between two values across frames — one dynamic vertex buffer, double
buffered. A Xenos draw packet has no base-vertex field, so the only thing left that can
sub-allocate it is **`VGT_INDX_OFFSET`** (register `0x2102`), which the hardware adds to
every index, generated or fetched, before the vertex fetch. Printed per draw it is:

```
0, 16, 20, 68, 84, 88, 136, 152, 156, 204, 228, 240, 276, 324, 336, 348, 360, 412, ...
```

— each entry the previous one plus the previous draw's index count, exactly.

`gpu/xenos.h` has had `kVgtIndxOffset` since phase 5 and `CZ_VK_STATE_PROBE` has been
printing it since then. All three submission paths passed 0.

### Why five phases of scene work never saw it

Because nothing drops and nothing errors. Every draw renders the FIRST run's vertices,
so exactly one text run comes out correct — the one that happens to sit at offset 0 —
and every other draw paints that same run's glyphs at that same place, sampled through
whichever atlas it bound. On a 376-glyph sheet that is legible text drawn many times
over; on the 184 sheet the same normalized coordinates land between cells and the result
is the "magnified fragments" §6aa recorded. Part 12 attributed that split to the
atlases, which is the visible difference between the two families of draw and not the
cause of either.

It engages **211 times in a plain title-screen boot** and on essentially every draw of
the menu, which is the other half of the answer: the scene does not use it.

### Measured

`CZ_VK_NO_INDX_OFFSET=1` is the same-binary control arm. On the save-slot screen
(`CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A`, frame 564):

| | control | fixed |
|---|---|---|
| the panel | one overlapped garbled run, nothing else | `SLOT 1/2/3`, `- NEW GAME -`, `GAMER PROFILE`, `Player`, `LV. N/A`, `Total PP:`, `Total Money:`, and the `A SELECT / B BACK / Y DEVICE SELECTOR` legend |

Applied to all three submission paths — `firstVertex` for auto-index, `vertexOffset` for
indexed and for expanded — and folded into `rectSynth`'s corners instead of passed to
the draw, because that path resolves the three real corners into a private four-vertex
stream and offsetting again would apply it twice. Bounds-checked against the uploaded
stream, with the draw DROPPED and counted if it does not fit rather than clamped back to
zero: clamping is the defect itself wearing a safety check's name. Zero drops in a boot.

Gates: `--smoke` OK; A1 exact 84-prefix; A5 exit 0, 2 windows, 0 real; `truncated=0`;
deepest file #83.

## 6ac. The black panels: nothing writes them, and that is a measurement now

Part 12 left "who was supposed to write `0364B000`" open and inferred that "hardware's
bytes differ from ours". The inference is retracted; the measurement is a negative.

Three hardware watchpoints under `gdb -p`, one on each of the three physical views the
Xbox 360 memory model aliases (`0xA0000000`, `0xC0000000`, `0xE0000000` — a watchpoint on
one alias cannot see a write through another, which is the trap this instrument has),
attached four seconds into the boot and left running through the entire save-slot era:
**zero hits**. The texture is not bound at all on a plain title-screen boot
(`CZ_VK_TEX_CENSUS` never names it), so it is created when the menu loads, ~30 s after
the watchpoints were armed. No resolve targets that address either — a boot's resolve
destinations are `06BE4000/06BF8000`, the `14xxxxxx` pyramid and `00E48000`, nowhere
near it.

So this is not a lost write. The title binds a 16x16 DXT1 it never fills, on three draws
— one per save slot, all three slots empty. What remains open is only whether hardware
fills it on a path we do not execute; the cheapest test is a run with a REAL save
present, because the obvious candidate is a thumbnail the slot does not have. The
practical note for anyone looking at the screen meanwhile: `CZ_VK_SKIP_TEX=0364B000`
removes the three rectangles and reveals three correct thumbnails underneath.

The joining detail that made the whole session cheaper: the runtime already prints
`runtime: guest memory at 0x...` unconditionally, so `gdb` needs no symbol lookup to
compute a host address for a guest one — which matters, because `g_memory` did not
resolve as a minimal symbol from every frame.

## 6ad. The picture against capture E, at last: four named differences

Item 3 of part 13's kickoff, and the first time it can be asked cleanly — both tiles
render, the scene surface is 99.5% non-black, and the frame is not transformed.

`tools/frame_signature.py` over a whole dumped title-screen era against `E2` and `E3`
says what it said before: every frame's best orientation is **`identity`**, at +0.36 to
+0.55 against E2 and +0.49 to +0.55 against E3, none reaching the tool's +0.70 floor and
every runner-up 0.14-0.35 behind. That is the signature of a matching scene photographed
at a different moment, not of a transform, and it is all a correlation can say about an
animated camera (gotcha 127). The rest needs eyes, so here is the list, confirmed on two
frames of different camera angles rather than one (gotcha 133):

1. **The whole frame is out of focus.** E3 is sharp from the foreground survivor to the
   Still Creek sign, with genuine depth of field only in the far distance. Ours is
   uniformly blurred at every depth including the near sign and the character. A
   depth-of-field or bloom pass is being composited at full strength everywhere, which
   is what a wrong depth input to the blend looks like — everything reads as "far".
   This is also the defect that made gotcha 188's single black column visible as a 19 px
   band, so the blur is real, live and load-bearing, not an absence.
2. **Colour is flat, desaturated and green-shifted.** E3's sky is a deep blue and its
   ground is warm; ours is a pale grey-blue over a green-grey street. The tone map's LUT
   is the thing §6s already proved this frame depends on completely.
3. **Some text is missing.** `PRESS START` renders; the
   `(C) CAPCOM CO., LTD. 2010 ALL RIGHTS RESERVED` line under it does not, and neither
   does the small community-watch sign's lettering.
4. **Some signage is blank.** The `GAS` balloon is a white blob, and the bunting strung
   across the street between the poles is absent entirely.

None of these is a missing pass in the §6s sense — the chain composes and the scene
arrives. They are four separate questions, and (1) is the one that changes the picture
most.

**THEY WERE NOT FOUR QUESTIONS.** §6ae below closes 1, 3 and 4 with one fix and moves 2
a long way: they are all the depth-of-field pass compositing at full strength over the
whole frame, and the fine detail it erased.

## 6ae. Three of those four were ONE defect: a resolve has a SOURCE

Phase C part 14. `CZ_VK_NO_DEPTH_RESOLVE=1` is the same-binary control arm.

### What was wrong

`RB_COPY_CONTROL`'s low three bits are `copy_src_select`: 0..3 name a colour target and
**4 names the DEPTH buffer**. `DoResolve` read that field nowhere. Every resolve, for
five phases, snapshotted the colour target.

§6d named this in the session it appeared — "RB_MODECONTROL 5 is depth-only and we
resolve the wrong buffer for it" — and quantified it as "four of the black 1280x720
surfaces in §7's table are depth resolves being served an empty colour buffer", i.e. as
a cosmetic gap in a table of surfaces nobody was looking at. Nothing since asked how
much of a FRAME depends on it, because our own command processor is the suspect in any
such question and cannot be its own oracle (gotcha 178).

The capture answers it with no emulator involved. `tools/xtr_resolve_census.py` replays
`RB_COPY_CONTROL` / `RB_COPY_DEST_BASE` / `RB_MODECONTROL` over B1's 24,527,474 packets:

| | resolves | share |
|---|---|---|
| `colour0` | 46,477 | 81.6% |
| **`DEPTH`** | **10,448** | **18.4%** |

and the destinations say what they are: three 4096x1024 depth surfaces (the shadow
cascades), one 1280x720 depth surface over the scene's tile region, and one address
(`1812F000`) that is the destination of both 890 depth resolves and 852 colour ones.

### What it did to the picture

The frame's last pass composes `tone map + scene depth + DOF blur + circle of
confusion`, and both the DOF blur and the CoC are computed FROM the scene depth. Handed
the colour buffer instead, the circle of confusion comes out saturated everywhere: every
pixel reads as maximally out of focus, so the sharp input is thrown away and the blurred
one is kept, at every depth. That is §6ad item 1 exactly — "uniformly blurred including
the near sign and the character, which is what a wrong depth input to the blend looks
like" — and items 3 and 4 are its consequences rather than separate defects, because
lettering on a sign and bunting against a sky are precisely the fine detail a
full-strength blur removes first.

### The measurement, and why coverage could not make it

Two runs per arm, arms alternated, 85 s each, no input, measured over the era:

| | control (`CZ_VK_NO_DEPTH_RESOLVE=1`) | fixed |
|---|---|---|
| median mean-\|gradient\| (`tools/frame_sharpness.py`) | **1.185 / 1.204** | **7.640 / 7.666** |
| median distinct colours, scene colour surface | 72,740 / 72,711 | **85,555 / 85,752** |
| median coverage, scene colour surface | 99.61 / 99.62 | 99.62 / 99.62 |
| frames per 85 s | 859 / 848 | 803 / 811 |

**Coverage moves 0.01 pp** — inside `frame_compare.py`'s own 1.5 pp band, i.e. this
project's purpose-built renderer A/B metric reports "no detectable difference" between a
frame that is uniformly out of focus and one that is sharp. That is gotcha 135's lesson
in a second disguise: a blur, like a flip, preserves coverage, mean luminance, distinct
colours and the whole histogram. A blur is a statement about the spatial DERIVATIVE, so
`tools/frame_sharpness.py` measures that, and it separates the arms by 6.47x with the
within-arm spread at 1.6% and 0.3%.

The picture: the title screen is now sharp from the foreground survivor to the Still
Creek sign, `POP 753` is readable, the community-watch sign reads "THE AREA IS OBSERVED
BY COMMUNITY WATCH CITIZENS", the bunting is strung across the street and the gas
station's signage is legible — all of which E3 has and none of which we had. Colour is
warmer and much closer to E3, though §6ad item 2 is not fully closed.

The cost is ~6% of the frame rate (four 1280x720 shadow-cascade depth copies plus the
scene depth's two tiles, per frame). Stated rather than optimised: nothing has yet shown
it matters, and the obvious refinement — snapshot a depth resolve only if some fetch
ever names that address with a depth format — should be done on evidence, not on a hunch.

### Two details the fix had to get right beyond reading the field

* **A depth snapshot keeps the EDRAM depth format.** `vkCmdCopyImage` is defined only
  between identical depth formats — there is no copy from a depth image into a colour
  one — so the snapshot is `D24_UNORM_S8_UINT`, viewed through the DEPTH aspect with
  `.gba` mapped to `.r`. A Xenos `tfetch` of a `k_24_8` surface returns the 24-bit depth
  in the first component, and mapping the rest keeps Vulkan's undefined non-red
  components of a depth view out of the picture.
* **The snapshot key needs the SOURCE in it, not a rebuild on change.** `1439B000` is a
  shadow cascade's depth destination early in a frame and the tone map's colour output
  late in the SAME frame — the resolve trace shows the final compose sampling it, and
  §6s recorded both facts about that address a phase apart without joining them. Keyed on
  the address alone, the two evict each other twice a frame: a `vkDeviceWaitIdle` and a
  fresh bindless slot each time, which is gotcha 192's descriptor-heap exhaustion with a
  different cause. The map key carries a depth bit and a fetch picks the one it meant by
  its own format field.

### THE RETRACTION: `06BE4000` is the scene DEPTH

This is the part to carry to Case West. `CZ_VK_FRAME_STATS_SURFACE=06BE4000` has been
documented as "the scene" since phase 5 §6 and used as the surface for every renderer
A/B in this port. The resolve trace, once its budget was fixed (below), reads:

```
dest=06BE4000 src=DEPTH   1280x720  win=0,0..640,720     draws=928 verts=494615
dest=0684B000 src=colour  1280x720  win=0,0..640,720     draws=0
dest=06BF8000 src=DEPTH   1280x720  win=640,0..1280,720  draws=927 verts=494612
dest=0685F000 src=colour  1280x720  win=640,0..1280,720  draws=0
```

**`06BE4000`/`06BF8000` are the scene depth's two tiles; `0684B000`/`0685F000` are the
colour's.** The address held colour pixels for five phases only BECAUSE of this defect —
our depth resolve copied the colour buffer, so a surface labelled "the scene" happened to
contain the scene. Every earlier A/B measured real colour content and is not invalidated;
the label was wrong, and CLAUDE.md's recipe now names `0684B000`. Gotcha 121's
`06BF8000 - 06BE4000 = 0x14000` tile arithmetic is unaffected — it is true of the depth
surface, and equally true of the colour one.

The depth snapshots themselves read as depth should: `CZ_VK_SNAP_DUMP` writes them
stretched between the surface's own min and max with a `_depth` suffix and prints the
24-bit range, and the scene's is `0.943370..1.000000` — a perspective depth buffer, with
the survivor, the sign, the power lines and the debris all legible in it.

### And an instrument that did not exist

`CZ_VK_RESOLVE_TRACE_PASSES` has been in CLAUDE.md since part 12 with a stated default
and was read **nowhere in the tree**; the hardcoded budget it names counted 60 HEADER
lines while the two follow-up lines printed uncapped for the rest of the run. That is the
identical defect §6s's own note says it fixed by "putting the budget in PASSES" — the
note was written and the code was not. So a trace ran out of headers after seven passes
and then emitted 37,000 orphan input lines, which is how the frame-1000 chain above was
nearly read with the destinations missing. Gotcha 193, for the second time in three
sessions, and the check is one grep.

## 7. What is NOT right yet, with the measurement for each

**SUPERSEDED IN PART BY §§6s-6u (session 21).** The table below is the state before the
compose chain, the index decode and the two rectangle defects were fixed. The current
state is: the title screen's LEFT HALF renders as a complete, bright Still Creek —
sky, power lines, the gas station, zombies, the road, the grass — and the RIGHT HALF
(screen 640..1280, the scene's second tile) is nearly empty. **§6v names why.**

The original table, for the record:

The picture at the title screen is the blood streak from the DEAD RISING 2 wordmark,
some UI text and two untextured bars — against E2's full logo. What the snapshot dump
says about where the rest went:

| surface | extent | non-black | reading |
|---|---|---|---|
| `06BE4000` (the scene) | 1280x720 | **81.3%, 1,089 colours** | Still Creek in its own colours, with a class of triangles exploded from a vanishing point |
| the 640x360 -> 32x1 pyramid | various | 12.5% – 64.6% | the post chain, fed from the scene — working |
| the 64x64 luminance chain | 64x64 | 53.2% – 100% | working |
| `14338000`, `14359000`, `1437A000` | 1024x32 | 99.9%, ~19k colours | a 32-cubed colour-grading LUT unrolled into a strip |
| `00E48000` (the frame) | 1280x720 | 3.1%, 144 colours | what we present — the logo era, and still missing the logo |
| `1439B000` .. `143FB000` | 4096x1024 | 0.0% | the shadow cascades: DEPTH resolves being served our colour buffer (§6d), and clipped by a 1280x720 EDRAM image |

**THE ONE REMAINING GEOMETRY DEFECT** is a class of triangles exploding from a vanishing
point. It is well bounded, and the bound is now made of retirements rather than
speculation. Reading the dominant vertex shader's generated HLSL,
`oPos` depends on exactly two things:

```
r4.xyz = iPosition0.xyz;                       // fetch slot 95, format 57 = 32_32_32_FLOAT
r1.xyz = dot(vc(8..10), r4);                   // world matrix
oPos   = dot(vc(0..3), r1);                    // view-projection
```

and nothing else — the format-16 attribute that looked suspicious feeds `r0.yzw`, the
NORMAL, so it is eliminated. Individually verified already: the position format
(`32_32_32_FLOAT` needs no conversion), the fetch slot (§6i), the vertex-shader constant
window (§6c), and the index decode (§6c). So the next step is not another hypothesis —
it is to instrument ONE draw: print `vc(0..10)` and the first few `iPosition0` values it
actually reads, and compare them against a matrix and a mesh that make sense.

That probe now exists (`CZ_VK_DRAW_PROBE=<vsHash>`) and **everything it prints is
healthy**: the world matrix is a clean translation, the view-projection is a plausible
near-plane-at-w=0 projection, the positions are sensible local coordinates, and the
stride matches the format. Working the numbers by hand gives `w = +23.3` for a real
vertex — a point comfortably in front of the camera.

So the inputs are right and the output is wrong. Everything on the candidate list has
since been checked, and two of the three are eliminated:

* **`VGT_INDX_OFFSET` (0x2102) is 0** in every state the probe saw, so ignoring it costs
  nothing. The same probe gave a much better result for free: **`VGT_MAX_VTX_INDX` is
  65535**, i.e. the guest declares 0xFFFF a LEGAL vertex index. That settles §6k's
  inconclusive primitive-restart question on principle rather than on a noisy metric —
  Vulkan's fixed restart index would consume a legitimate vertex, so restart must stay
  off.
* **`8bb7e189d92e3def`, 100,392 draws a run with zero vertex attributes**, emits
  `oPos = (0,0,0,1)` unconditionally — a degenerate point. It contributes nothing and
  cannot be the exploding geometry.
* **The vertex-stream cache key was a hash, not an identity**, and that is a genuine
  defect found while looking: `(uint64_t(va) << 24) ^ (bytes << 2) ^ endian` puts the
  address in bits 24..55 and the size in bits 2..31, so the fields OVERLAP and two
  different (address, size) pairs can collide. A collision hands a draw **another
  mesh's vertex stream**, which draws triangles between unrelated vertices — precisely
  this symptom. Fixed by packing the fields into disjoint bits.

  Honest about the evidence: this is a correctness fix, and §6k's variance means the
  scene-coverage metric **cannot resolve whether it changed the picture**. The frames
  after it look cleaner — large coherent surfaces instead of a dense radiating fan —
  but that is an impression from one frame, not a measurement, and it is recorded as
  such.

What is left for the next session is therefore to build the metric before chasing the
defect: a pinned-camera comparison, or B1's own per-era draw aggregates, which is what
the kickoff prescribed for the GPU gate and which this phase still has not built. Every
cheap hypothesis has been spent; the next one needs an instrument that can tell whether
it worked.

Known simplifications in the renderer that are candidates, each stated at its site:

* **A depth-only pass resolves the colour target.** §6d — a bug with a known location,
  not a simplification, and it accounts for the four black shadow cascades above.
* **The shadow cascades are 4096x1024** and our EDRAM image is 1280x720, so their
  window (1024x1024) is clipped to the target. They need a target sized from the
  surface, not a fixed one.
* **One global sampler.** The fetch constant's per-texture filter and address modes are
  decoded and ignored.
* ~~**No culling.**~~ **Retired by §6c: the title does not cull in this era**
  (`PA_SU_SC_MODE_CNTL` has both cull bits clear), so drawing both faces is faithful.
* **One EDRAM format.** The colour target is always `R8G8B8A8_UNORM`; a pass rendering
  to an HDR surface (`16_16_16_16`, `2_10_10_10`) is clamped.
* **No mip levels.** Only level 0 of each texture is uploaded.
* **Rectangle lists reuse their three real corners** for the second triangle rather
  than synthesising hardware's fourth, which is correct only for an axis-aligned screen
  rect. 27,762 draws a run take this path.
* **`RB_COLOR_CLEAR` is read as 8888** regardless of the target's format.

The 3-colour bars are the most specific lead: a pass drawing geometry whose texture
came back as the dummy, which given that no texture failure is counted means it sampled
a **snapshot that was empty** — i.e. the same upstream gap, one link later.

---

## 8. Gates

All pre-existing gates hold, with the renderer **on**:

```
cz_runtime --smoke                                          OK
kernel_call_diff --xenia A1                                 84-deep exact prefix
kernel_call_diff --xenia A5 --include-high-frequency        exit 0, 2 windows, both permutations
tools/pm4_packet_lengths.py   (24,527,474 packets)          0 disagreeing
tools/pm4_indirect_walks.py   (28,726 buffers)              OK
ring: indirect buffers truncated=                           0
[pm4] STALLED                                               0 occurrences
```

Two notes on reading these:

* The A1 gate reached its full 84-deep prefix on the renderer-**on** arm, and the
  renderer-**off** arm permuted at position 71. That is the scheduling-sensitive window
  finding 41 and gotcha 86 already record, and it landed on the control arm this time —
  which is the cleanest possible demonstration of why a single run of one arm proves
  nothing about the other.
* Both arms were run with an **empty save root** (`CZ_SAVE_DIR` pointed at a fresh
  directory), because A1 was captured with no save present (gotcha 106).

**Cost.** Same binary, arms alternated, 100 s headless runs: **1,488 frames with the
renderer on against 3,090 with it off** — the renderer roughly halves the frame rate.
That is a real cost and it is where it is expected to be: a synchronous submit and a
full readback per frame, plus per-draw constant uploads at ~900 draws a frame. It is
not a defect to fix before the picture is right; it is a number to re-measure after.

---

## 9. Instruments this phase added

All off by default and free when off.

```
CZ_VKDRAW=1              enable the renderer at all — and the control arm for every
                         claim in this document
CZ_SHADER_DUMP=<dir>     one file per distinct microcode blob, named by the hash the
                         renderer looks up. The input to tools/build_shader_spv.sh
CZ_SHADER_SPV=<dir>      override the shader cache location
CZ_VK_STATS=N            the named-counter block every N frames (and every "we could
                         not draw this" path has a counter)
CZ_VK_FRAME_DUMP=<dir>   every 64th presented frame as a PPM — the renderer checked
                         without a window
CZ_VK_SNAP_DUMP=<dir>    EVERY resolve snapshot of one frame, which is the only way to
                         tell an early wrong pass from a late one
CZ_VK_RESOLVE_TRACE=1    each resolve's destination, extent and clear bits, against the
                         front buffer VdSwap named
CZ_VK_VIEWPORT_TRACE=1   every DISTINCT viewport setup, once each
CZ_VK_FETCH_PROBE=1      which vertex fetch slots the guest has actually populated
CZ_VK_FORCE_COLORMASK=1  treat every draw as writing all four channels (the arm in §6)
CZ_VK_STATE_PROBE=1      the distinct values of the state registers this renderer
                         ASSUMES rather than reads (§6c)
CZ_VK_INDEX_ENDIAN=N     force one index swizzle code for every draw — the arm that
                         retired index endianness as the cause of scrambled geometry
CZ_VK_VALIDATION=1       the Khronos validation layer
```

---

## 10. For Case West

Everything in §1 transfers with no change: same engine, same container, same shader
pipeline, and the container synthesizer is already game-independent. The two things to
carry deliberately are §4 (the fetch-slot index convention, which is a property of the
hardware and not of this title) and §5 (the resolve-destination identity, which is a
property of this *engine's* renderer and is very likely identical in Case West, right
down to the downsample pyramid).
