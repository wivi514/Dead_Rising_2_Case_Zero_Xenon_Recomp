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

## 6af. The prologue's black screen: TWO of our defects, and then the guest's own fade

Phase C part 15. Part 14 handed this over as the frontier — "a live pass with live
inputs produces black, the same SHAPE as §6s" — with the tone map named as the first
black link. It is not one defect and the last of the three is not ours.

Repro throughout: `CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A`, 300 s, no
window. The presented frame is 37% non-black through the menus and loading card and
**goes to 0.00% at frame ~1000 and stays there for the remaining ~2,400 frames.**

### (i) A shader the cache did not have — real, and NOT the black

The log says it in one line: `[vk] no translated shader for VS 24e60d91249e6d04 —
draws skipped`, printed once per missing hash, with a counter (`draw: shader not in the
cache`) reading **28,718 a run**. The blob is 351 dwords, loaded by the prologue, and it
is in neither capture — A1 stops at the title screen, A2 is gameplay — nor in our own
boot dump, which the cache recipe built from a plain boot that also stops at the title
screen. Three sources and a hole in the middle of all of them.

Fixed by dumping from a run that reaches the prologue (the recipe in CLAUDE.md now
does), and the cache is 337 shaders. **It did not change the picture**: the missing
count went to 0 and the frame stayed 0.00%. Recorded because a negative result on a
real defect is worth as much as a positive one — and because the reporting is the
lesson. A missing shader is one log line, no fallback, no picture alarm and no failure;
`grep -c "no translated shader"` is the gate and it is now written down.

### (ii) The colour-grading LUT's snapshot EXPIRED — ours, and a second full-frame black

The dependency graph at the black frame, from `CZ_VK_SNAP_DUMP` + the resolve trace's
`sampled snapshots` line, differs from the title screen's by exactly one entry:

```
frame  448 (works)  tone map inputs: 0684B000 1476F000 147AB000 147BA000 14338000
frame 1100 (black)  tone map inputs: 0684B000 1476F000 147AB000 147BA000
```

`14338000` is the colour-grading LUT, and §6s already established that a black LUT is a
black frame. The reason it dropped out is `frameSeen + 1 >= frame`: a snapshot had to
have been taken this frame or last. **At the title screen the title re-renders all three
LUTs every frame** (three resolves in frame 448's list) so the window never bound; at
the prologue the grade is static, the LUT stops being resolved, and from the next frame
the fetch fell out of the snapshot path into guest memory — which is zero, because
resolved pixels are never written back there.

The window is now gone (`CZ_VK_SNAPSHOT_MAX_AGE=1` restores it as the control arm). For
an address the GPU has resolved to, the snapshot is the ONLY copy of what that surface
holds, at any age; the risk the window was implicitly guarding — the guest freeing a
resolve destination and putting a CPU texture there — is real, different, and does not
happen in this title, and the census now tracks `maxAge` on the served path so it would
be visible.

This too was invisible until (iii) was arm'd out, and that is how it was measured:

| prologue frames 1088..1664, with the fade patched out | non-black | colours |
|---|---|---|
| `CZ_VK_SNAPSHOT_MAX_AGE=1` (pre-part-15) | **0.00%** | 1 |
| default (no age limit) | **99.99%** | ~89,450 |

### (iii) The rest is the GUEST asking for black, and the compose is faithful

With both of ours fixed the frame was still 0.00%, so the next move was to print the
tone map's constants rather than reason about them (gotcha 145). `CZ_VK_DRAW_PROBE`
with `CZ_VK_DRAW_PROBE_PC=105,106,110,111,112,254,255`, on the tone map draw
(`tex=0684B000 1280x720`) in both eras:

| | title screen (frame 440) | prologue (frame 1000) |
|---|---|---|
| `pc(110)` | 0, 0, 0, **0.4** | 0, 0, 0, **1.0** |
| `pc(111)` | **1.0**, 1.414, 0, 0 | **0**, 0, 0, 0 |
| `pc(254)` | .0156, 1.4142, .0312, 0 | .0156, 1.4142, .0312, 0 |
| `pc(255)` | .9688, 31, 2, -1 | .9688, 31, 2, -1 |

The shader's last four instructions are a vignette/fade:

```
r0.x = pow(r1.w / pc(111).y, pc(111).x) * pc(110).w;   // weight
r0.xyw = saturate(r0.x * (pc(110).xyz - r1.xyz) + r1.xyz);
```

`pc(111).x` is the vignette POWER and `pc(110).w` its strength. With the power at
**zero**, `pow(anything, 0) == 1` at every pixel, so the weight is `pc(110).w` = 1.0
everywhere and the compose lerps 100% of the way to `pc(110).xyz`, which is black. The
LUT arithmetic constants (254/255) are bit-identical between the eras, so nothing about
the grade is wrong — the frame is being deliberately, uniformly faded out.

**Proved with an arm rather than argued.** `CZ_SHADER_SPV=<dir>` and a one-line patch to
`ps_114c4965eaabd54c`'s HLSL forcing the fade weight to zero (gotcha 128, a five-minute
experiment):

| prologue frames 1024..1664 | non-black | colours |
|---|---|---|
| stock shader | 0.00% | 1 |
| fade weight forced to 0 | **99.99%** | **~89,450** |

and the picture behind it is the prologue's opening highway — the road into Still Creek,
power lines, rocks, the tree, tone-mapped and graded. **The renderer draws the prologue
correctly.**

### What IS wrong, then, and it is not the renderer

`CZ_VK_FRAME_STATS` over the whole black era says the game is not advancing:

* the **camera fingerprint is one constant value, `00d7a3a4aaed62c6`, for 1,700+
  frames**, and the scene surface's mean luminance is pinned at 104.484 with coverage
  100.0000 on every sampled frame;
* the draw stream still moves a little — 1,225..1,247 draws and 848,653..883,067
  vertices, cycling through a handful of fingerprints — so the title is alive and
  submitting work, it is the scene STATE that is frozen;
* everything underneath is healthy: `ring: chain arms=11489 ints=11483 isr=11483`,
  `kicks == walks == drains = 6659`, `distinct=764`, `truncated=0`, zero parser stalls,
  and every `[wait]` is an idle worker or one of the two threads the title blocks by
  design (finding 41).

So the prologue is stuck in a faded-out state with a frozen camera, and the black screen
is the game's own fade drawn faithfully over it. The leading hypothesis for what it is
waiting on is **audio**: the loading ends with `XMACreateContext` (six contexts), the
pump runs — 55,808 driver frames in 300 s — and **every sampled frame has peak
amplitude exactly 0.0000**, because there is no XMA decoder (phase 6). An in-engine
cinematic cued off a voice or music stream that never plays would look exactly like
this. That is a hypothesis with an obvious next probe, not a finding.

The general lesson is the one this port keeps re-learning from the other side: **a
symptom can be several defects deep, and the last layer can belong to the guest.** Three
things had to be true for the prologue to be black, and only two of them were ours. The
instrument that separated them was an arm on the shader itself — the only way to ask
"is this black because we computed it wrong, or because we were told to".

## 6ag. The shadow cascade: one clipping defect, and the guest's clear rects named

Phase C part 15, item 3. Part 14 handed this over as "every plumbing hypothesis is
retired, the map is 48.7% pure zero, so ask about its DRAWS". Asking about the draws
found one of ours and then turned the rest into a measured fact about the title.

### The exact shape of the empty half

Counted off `CZ_VK_SNAP_DUMP`'s depth PPM rather than looked at (the boundary is what
matters and it is axis-aligned, so a number is better than an eye here):

```
rows    0.. 511   all 1024 columns populated
rows  512.. 719   only columns 960..1023  (a 64-wide strip)
rows  720..1023   nothing
nothing at all beyond x = 1024 on the 4096-wide surface
```

Three numbers, and two of them are explained by one defect and one by the title.

### Ours: window coordinates were mapped through the PRESENTED FRAME

The `vte == 0` path built `posScale = 2 / targetWidth, 2 / targetHeight` and then set a
fallback viewport of `targetWidth x targetHeight`, i.e. the front buffer's 1280x720. The
two divide out, so window (X, Y) lands on framebuffer (X, Y) and nothing looks wrong —
but the CLIP happens at NDC ±1, which is window y = 720, on an EDRAM stand-in that has
been 1024 rows tall since part 14. Every window-coordinate draw taller than the screen
was being cut off at row 719.

Both are the EDRAM's extent now. `CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1` is the control arm,
and the delta is exactly what the mechanism predicts and nothing more:

| cascade snapshot at frame 448, non-black over 4096x1024 | |
|---|---|
| `CZ_VK_WINDOW_COORDS_FRONT_BUFFER=1` (pre-part-15) | 12.82% |
| default | **13.28%** |

0.46 pp of 4,194,304 pixels is 19,300; the clipped strip is 64 x 304 = 19,456. It is the
strip, to the pixel, and nothing else moved.

### The title's: the clear rects do not cover the map

`CZ_VK_DRAW_PROBE` on the clear shader (`vs=539ea9e08aa83f0c`, prim 8, 3 indices — a
rectangle list whose fourth corner part 9 synthesises) over 60 draws, deduplicated:

| clear rect | count |
|---|---|
| `(0,0)-(480,512)` | 16 |
| `(960,0)-(1024,1024)` | 17 |
| `(0,0)-(320,720)` — the scene tile, part 9's | 2 |
| `(0,0)-(640,360)`, `(0,0)-(64,64)`, `(0,0)-(1,1)` | the small passes |

All at z = 1.0, with `depthCtl=76 (test=1 write=1 func=7)` — func 7 is ALWAYS, so these
are unconditional depth writes, a clear in every sense but the name.

So for a 1024x1024 cascade the guest issues a 480x512 rect and a 64x1024 one. That does
not cover the map, and it is not a consequence of anything this renderer does — it is
what the title's own vertex data says. Whether 480x512 is a pixel extent that should be
doubled somewhere (the pass reports `msaa=0`, so our 4x scaling does not apply to it),
whether the cascade is really four smaller maps packed into one surface, or whether the
uncleared region is simply never sampled, is the open question part 16 inherits — and it
is now a question with all four numbers on the table instead of a suspicion.

**Honest about the picture: shadows still do not appear.** This is committed on
mechanism plus a matching structural delta, which is the half-answer part 14's own rule
says to declare (gotcha: "a picture fix is not a picture fix until it is measured").

## 6ah. The prologue freeze: three readings retired, and part 15's confirmed

Phase C part 16. Part 15 handed this over as "the prologue is stuck in a faded-out
state with a frozen camera; the leading hypothesis is AUDIO". This section is mostly
**negative results**, which is what it is for: each one cost a build and a run, each
one had a mechanism behind it, and each one is now off the list with a same-binary arm
to show for it. The blocker is still open and is now much better bounded.

### The timeline, which nobody had written down

`CZ_VK_FRAME_STATS` over one 300 s
`CZ_FAKE_START_MS=8000 CZ_FAKE_PRESS_SEQ=START,A,A` run, collapsed on the camera
fingerprint. Reading it as *eras* rather than as "the run freezes" is what made the
rest of the session possible:

| frames | draws | presented coverage | camera | what it is |
|---|---|---|---|---|
| 1..591 | 2,514 | ~96% | a new value every frame | the title screen, animating |
| 596..962 | ~150 | ~36% | 4–5 values cycling | the **loading screen** |
| 974 | 948 | 0.00% | changes once | the world's first frame |
| 1002..end | 1,225–1,247 | **0.00%** | **`00d7a3a4aaed62c6`, constant** | frozen |

So the loading COMPLETED, the world is being submitted at ~1,230 draws and ~849,000
vertices a frame, and the scene surface's own mean luminance is pinned at **104.484 to
three decimals** — the world is not merely hidden, it is not being simulated. And the
last assets the boot opens name what the title was about to do: `#146
cinematics\cinematics.big`, `#147 anim\cinematic\701_chuck_arrives_in_town.big`, `#148
skeleton\cineplayer.big`, `#149 skeleton\cinechild.big`. **It is sitting at the start
of the first cinematic.**

### (i) It is not audio — refuted across all three configurations

> **THIS SECTION IS WRONG AND PHASE A/V PROVED IT. IT IS AUDIO.** Kept verbatim below
> because *how* a three-configuration same-binary refutation retired a true hypothesis is
> more valuable than the conclusion was. With a REAL XMA decoder — ffmpeg, wired in phase
> A/V — the prologue's longest frozen camera run goes from **10,513 frames to 159** and
> presented coverage from **15.00% to 99.94%**, measured with `CZ_NO_XMA_DECODE=1` as the
> same-binary control on the same recipe (`docs/phase-av-notes.md` §4).
>
> The error is not in the runs below; every number in them is real. It is in what the arm
> could reach. `CZ_XMA_NULL_DECODER` moves the **predicate the title polls** — the
> input-buffer-valid bits sub_8285EFE0 reads — and nothing downstream of PCM actually
> existing. Both polarities of a predicate is not the whole mechanism, and a null
> implementation only reaches the states its author thought were load-bearing. The arm's
> own header comment said as much at the time: it "fabricates playback progress the real
> hardware would only make after actually decoding the audio". Gotcha 268.
>
> The other three negative results in §6ah — no deadlock, not our synthetic input, the
> guest really is asking for black — are untouched by this and still stand.

Part 15's evidence was 55,808 driver frames of peak amplitude exactly 0.0000. That is
a fact about our OUTPUT, which no guest code can observe, so it could not have been
more than a suspicion. The image states the real mechanism, and it is a good one:

```
sub_8285EFE0(pool, i)  -> ((ctx[i].dword0 >> 20) & 3) == 0
                          bits 20/21 are the two input-buffer-VALID flags
sub_82862A90(voice)    -> OR over the voice's contexts = IsPlaying()
sub_82864808(voice)    -> caches it at voice+0x120, branches on the transition
```

The guest SETS those bits when it hands the decoder packets; the DECODER clears them
as it consumes them. We have no decoder, so nothing on this machine ever clears one
and **every voice the title has ever started is still playing, for the life of the
process** — measured, not argued: 284,373 polls, 284,354 "playing", **0 stop edges**.

`CZ_XMA_NULL_DECODER=1` is the arm: a decoder that consumes its input and produces
nothing. Three configurations of one binary, all reaching `#154`:

| arm | what the guest sees | camera | presented frame |
|---|---|---|---|
| stock | always playing | `00d7a3a4aaed62c6` | 0.00% |
| `MS_PER_PKT=0` (instant) | **never** playing, `playing=0/318,631` | `00d7a3a4aaed62c6` | 0.00% |
| `MS_PER_PKT=40` (real rate) | plays then ends: 19 start / 18 **stop** edges | `00d7a3a4aaed62c6` | 0.00% |

Both polarities of the predicate AND the transition between them. The prologue does
not move. **Refuted, not merely unconfirmed** — and the rate is why the third row
exists at all: at instant consumption a voice is dry before anything can poll it, so
that arm tests "nothing ever plays", which is the opposite end of the same axis rather
than the middle of it.

### (ii) Nothing is deadlocked — it is a logic state

`gdb -p` over all 31 host threads, joined to guest thread ids by the always-on
`guest thread tid=... host tid=...` line (gotcha 82). **Exactly one thread is in guest
code**, and it is the Draw Thread doing its ordinary per-frame GPU sync
(`sub_827D2FC0 -> sub_82845230 -> sub_82845160 -> sub_8283C6C8`). The MAIN guest
thread (`tid=00000F00`) is in `NtWaitForSingleObjectEx` on an event with an infinite
timeout — and `CZ_WAIT_TRACE` never reports it, which means that wait is being
SIGNALLED and re-entered, i.e. the main loop is turning. Every other thread is an idle
worker or one of the two the title blocks by design (finding 41).

Corroborating from the other side: `[kcall]`'s first-occurrence list ends at
`XeCryptShaFinal`. **The prologue era reaches no new kernel import at all**, so the
blocker is not a stub we have yet to write.

### (iii) It is not our synthetic input either

`CZ_FAKE_PRESS_SEQ` holds its last entry forever, so every run that reached the
prologue had been pressing A every 8 s at it for minutes. `NONE` now exists so the arm
can stop. `START,A,A,NONE` stops at the TITLE screen — several A presses are
load-bearing, so NONE alone proves nothing — but ten A presses then NONE, i.e. no
input for the last ~170 s, reaches `#154` and **the identical frozen state**.

### (iv) Part 15's "the guest is asking for black" is CONFIRMED

This one was worth re-asking (gotcha 172), because a shader constant that is WRONG and
one the guest never wrote in this era look identical from inside the shader. The
answer is a clean confirmation. Over the black era the guest writes, **5,662 times
each and with exactly one distinct value per register**:

```
pc(110) = (0, 0, 0, 1.0)      the vignette colour and its strength
pc(111) = (0, 0, 0, 0)        the power and its normaliser
```

Not stale, not ours, not a missed packet form. The tone map's `pow(x, 0) == 1`
therefore lerps every pixel to black exactly as part 15 said, and the renderer is
drawing it faithfully.

Getting there cost two wrong readings off my own instrument in one afternoon, both
worth recording because they are the same mistake wearing different hats.
`CZ_PM4_CONST_WATCH` printed the first 32 writes and then every 4096th. Watching a
RANGE, the 32-line head was eaten by the lowest registers, so `pc(111)` printed nothing
and read as *"the guest never writes the vignette parameters in this era"* — a
finding, and false. Watching `45BC-45BF` alone, the thinned tail sampled the `.w` lane
every time, which invited *"every write is zero"* off four identical lines. It is a
per-register value **histogram** now. A capped print is not a count, and a thinned
print is not a distribution.

### (v) The engine has its own log, and it is switched off

The most reusable thing this session found. `sub_827877C8` is the engine's global
debug printf — a vsnprintf into an 0x800 stack buffer with **640 distinct callers** —
handing the formatted string to `sub_828223A0`, which twelve sites share. `CZ_GUEST_LOG=1`
hooks that one function and the title narrates itself.

It currently prints nothing, and the zero is checked rather than believed: the strong
`PPC_FUNC(sub_828223A0)` is present in the object file, so the seam is wired and it is
the CALL SITES that are silent — each gated on a debug byte
(`lbz rX,<flag>; cmplwi rX,0; bne <skip>`) that a shipped build leaves at zero.
Raising those flags is the follow-up and it is worth real effort, because the strings
say exactly what a stuck state machine would want to tell us: `[FE] Showing tutorial
%d`, `[FE] NOT showing tutorial %d because we're not in the HUD`, `cinematics are
playing`, `For cinematic props... check to make sure the prop names match up in the
data files`.

`game:\cl.txt` — which the boot probes and does not find — is **not** the switch, and
that is worth writing down so nobody else spends an hour on it: `sub_82482E50` reads it
as a CHANGELIST NUMBER, `atoi`s it into `0x82A5742C`, and if it lands between 10,000
and 305,000 spins forever printing `your CL is bad, error code: 0xID 10T`.

### (vi) One real defect found on the way, not yet shown to matter

`VfsTranslate` returns empty for any path with no `:` in it, so **a guest path with no
device prefix can never resolve**. A boot makes 29 such opens, all
`data\anim\weapon\<Weapon>.big`, and every one fails. It is not currently costing
anything — the package ships `allweapons.big` and none of the 29 names exist under any
prefix — so this is recorded rather than fixed, with the note that on console a
relative path resolves against the title's own directory. CLAUDE.md already warns that
at least one path here is built at runtime (`anm_%s.big`), so the gap will bite
eventually.

## 6ai. GAMEPLAY IS REACHABLE — and the prologue freeze is "cinematics never end"

> **PHASE A/V CLOSED THE PROLOGUE HALF OF THIS: the cinematic was waiting on AUDIO**, and
> wiring a real XMA decoder unfreezes it (10,513 frozen frames -> 159; coverage 15.00% ->
> 99.94%, same-binary arm). The reading below — that the teardown is fine and what is
> missing is whatever *triggers* it — was exactly right, and the trigger turns out to be
> downstream of the audio stream actually producing PCM. `docs/phase-av-notes.md` §4.
> The section's own title was already retracted separately in `docs/open-items.md` 1:
> most cinematics never failed.

Phase C part 16, operator session on the part-16 binary (real controller, windowed,
`CZ_VKDRAW=1`, no arms). This is the single most informative hour this port has had,
and it did not come from an instrument.

### The finding that unifies §6ah

The operator **skipped both prologue cinematics and reached live gameplay** — the
Zombrex tutorial card, the watch/MESSAGES screen, Still Creek, combo weapons. Then a
combo-weapon cutscene left the camera parked on the workbench looking at Chuck with no
HUD, while Chuck still responded to input. **Skipping that cutscene restored the
camera.**

So this is not "the prologue is stuck". It is:

> **Every cinematic in this title starts and never ends. Skipping is the only exit,
> and the skip path works perfectly.**

The prologue's black screen is the same defect wearing §6af's fade: the title fades
out for a cinematic that never completes, so the fade never comes back. §6ah's four
negative results all stand and are now *explained* rather than merely true — a
cinematic that never advances asks nothing of the kernel, blocks no thread, and is
indifferent to whether audio finishes.

**The skip path being clean is the strongest clue available.** Skipping runs the same
"end this cinematic and give control back" code that a natural end would run, and it
demonstrably restores the camera, the HUD and control. So the teardown is fine. What
is missing is whatever *triggers* it — the cinematic's own notion of "I have reached
my end". That is a much smaller target than "the prologue is stuck", and it is where
part 17 should start.

Two supporting details worth keeping: A alone does not skip (every part-16 headless
run pressed A every 8 s at the prologue and stayed black), and the combo cutscene's
camera is frozen at the cutscene's OPENING framing, not somewhere in the middle.

### The picture in gameplay, with a hardware reference for the first time

Capture **E4 is the first gameplay frame** and the operator's indoor screenshot is very
nearly the same scene, which makes this the first like-for-like gameplay comparison
this port has been able to make.

**The HUD is NOT a defect — recorded because it was written up as one an hour before
it was retracted.** The indoor frame showed only `Find Katey Zombrex` and `0 KILLED`
against E4's full HUD, which read as "one class of HUD widget does not render". It
appears in full the moment Chuck steps outside: LV/PP bar, LIFE pips, `$2,000`,
`ZOMBREX 0`, the item wheel, the watch dial. The safehouse simply has not raised the
HUD yet. E4 is a *later* first-gameplay frame than the one it was being compared to —
gotcha 127's rule about single samples of a moving thing, applied to a whole SCREEN
rather than to a metric.

**The colour is a real defect and now has its sharpest symptom.** Two eras, and the
outdoor one is far more diagnostic than the interior:

| | hardware | ours |
|---|---|---|
| safehouse interior (vs E4) | warm red/brown wood, bright orange shirt | green-shifted, blacks crushed |
| Still Creek exterior, daylight | pale hazy blue sky | **the sky is PINK/MAGENTA** |

In the exterior frame Chuck's orange shirt, the red car body and the yellow LIFE pips
are all correct, and the ground and fence are green-tinted, and the sky is magenta.
That is not a global tint or an exposure error — those would move the shirt too. A hue
error that spares saturated reds and yellows while turning a pale blue sky magenta and
the mid greys green is the signature of a **wrong colour-grading LUT**, which is
exactly the mechanism §6s proved this frame depends on completely and §6af caught
silently expiring. It is the same "flat and green-shifted" as §6ad item 2, finally
visible somewhere a hue error cannot hide.

The exterior is also much brighter and closer to right overall, which is itself
evidence: whatever is wrong is *graded* rather than *lit*, and it varies with the
grade the title selects per area.

### THE HEADLESS GAMEPLAY RECIPE — the thing that unblocks everything else

**START skips a cinematic** (operator). That one fact, plus two more from the operator
about the Zombrex tutorial, turns gameplay from an operator task into a headless run:

```
CZ_FAKE_START_MS=8000 \
CZ_FAKE_PRESS_SEQ=START,A,A,A,A,A,A,A,A,A,A,START,START,START,START,START,START,START,START,A,A,LEFT,B,NONE
```

* `START` then ten `A` — title, logo, menu, new game, confirmations (ten is empirical:
  with fewer, the run parks on the title screen);
* eight `START` — the two prologue cinematics, with a loading screen between them;
* `A`, `A` — the two pages of the Zombrex tutorial card;
* **`LEFT`, `B`** — page two of that card requires opening the watch with D-pad LEFT and
  backing out of it with B. Without those two the run sits on the card forever, which
  is exactly what the first two attempts did (`#177` and `#178`, a frame frozen at
  55.0% and 56.1% coverage);
* `NONE` — go quiet, so the run is not being poked while it is measured.

Measured: **`#184`, ~1,860 draws a frame, and 200 DISTINCT CAMERAS over the last 200
frames.** The frame is Chuck in the safehouse with the bat and pipe pickups, the guide
arrow, `Find Katey Zombrex` and `0 KILLED` — the same screen the operator reached with
a controller. Roughly 185 s to arrive, so give it a 300 s+ timeout.

Read it as a distribution, not a fact: every step is a fixed 8 s interval against a
boot whose depth in fixed wall time has always varied (gotcha 75), so the number of
`A`s and `START`s that lands correctly will drift with load and with any change to
frame rate. Run it serially (gotcha 183) and check the camera-distinctness number
before trusting a gameplay measurement taken from it.

### A consequence of skipping, from the operator

**Skipping the combo-weapon cutscene does not award the combo weapon.** That is not a
second bug — it is the same one, seen from the other end: the cinematic's completion is
what grants the reward, so a skip that bypasses the completion bypasses the grant too.
It also means the skip is a workaround for *observing* the game and not for *playing*
it, and it puts a floor under how much of the game is reachable until cinematics end
properly.

### What this changes about method

Everything above was unreachable headless, and part 16 spent a session measuring the
one screen `CZ_FAKE_PRESS_SEQ` could get to. Gotcha 190 said to extend the arm until a
gate no longer needs a human; the arm was extended to the menus and stopped there,
because nothing had established that a cinematic could be skipped at all. **The
cheapest next instrument is not a probe — it is the button sequence that skips a
cinematic**, because it turns gameplay into something a headless run can reach, and
every renderer question above then becomes self-servable in the session that asks it.

## 6aj. WHERE A GAMEPLAY FRAME ACTUALLY GOES — and 57% of it is a `sleep_for`

`docs/perf-plan-overnight.md` §1 says: attribute the 58% that `CZ_VK_PROFILE` calls
`outside` before optimising a single thing, because that one number contains at least
four different investigations. This is that attribution. **It is not what the plan
expected, and it is not work at all.**

The starting picture, re-measured on the current binary (steady gameplay, headless,
~1,890 draws a frame, five consecutive `[vkprof]` windows):

```
11.8 fps (85.0 ms/frame) | draw 8.8% [constants 0.5 streams 2.3 textures 1.0
                            record 5.1 other 0.0] submit 28.5% readback 0.4%
                            outside 62.2%
```

### (i) `perf` says the CPU is a guest BUSY-WAIT, and the renderer is a rounding error

`perf` is installed on this machine now (it was not as of part 17). Attached to the
running process during the gameplay era — 60 s, 18,201 samples, 332.9 G cycles, i.e.
**1.33 cores busy**:

| symbol | share of all cycles |
|---|---|
| `__imp__sub_8283C6C8` | **38.1%** |
| `__imp__sub_82845160` | **21.3%** |
| `__imp____restgprlr_29` / `__savegprlr_29` | 9.9% / 3.9% |
| `__imp__sub_82821FF0` | 2.9% |
| `UploadStream` / `DoSwapImpl` / `DoDraw` / `BindShader` | 2.0 / 1.5 / 0.6 / 0.4 |
| `memcmp_avx2` / `strncmp_avx2` / `getenv` / `std::map::operator[]` | 0.8 / 0.8 / 0.4 / 0.4 |

That top pair is **finding 38's ring-progress spin** — `sub_82845230` -> `sub_82845160`
-> `sub_8283C6C8`, the engine's per-frame GPU sync — and with the save/restore ladders
it accounts for roughly **73% of every cycle the process burns**. Per THREAD the same
data reads 77.6% in one thread, 10.8% in the next, then 5.1 / 2.9 / 2.1 and nothing.

So the picture from the CPU's side is: one guest thread burns a full core waiting for a
fence, and the thread that runs the command processor AND the entire renderer uses
0.14 of a core. Gotcha 97's "a yield loop is a busy loop wearing a polite name",
arriving from the guest's side this time — except that here it is not even a defect we
can fix, because it is the title's own code and on console it spins for microseconds.

**What it is NOT is an explanation of the frame time.** A cycles profile can only see
threads that are on a CPU, and the thread that decides when a frame ends is not.

### (ii) The pump is ASLEEP for most of a frame, and nothing could see it

The renderer runs on the graphics interrupt pump's thread, so everything that thread
does between two presents lands in `outside` — including the `sleep_for(vblankMs)` at
the top of its own loop. `runtime/gpu/pump_stats.h` is the instrument: four counters,
always on, printed on a second `[vkprof]` line. Same run, same windows:

```
[vkprof] pump 180 ticks (3.00/frame) | sleep 57.3% walk 42.7% vblank-isr 0.0%
[vkprof] pump 177 ticks (3.00/frame) | sleep 56.5% walk 43.5% vblank-isr 0.0%
[vkprof] pump 180 ticks (3.00/frame) | sleep 57.3% walk 42.6% vblank-isr 0.0%
[vkprof] pump 177 ticks (3.00/frame) | sleep 56.6% walk 43.5% vblank-isr 0.0%
```

**3.00 ticks per frame, every window, to two decimals.** The frame is quantised by the
pump's cadence. Ranked attribution of the 85 ms, which is the deliverable §1 asked for:

| term | ms | share | what it is |
|---|---|---|---|
| **`sleep_for(16 ms)` x 3** | **48.5** | **57%** | not work, not GPU wait — the pump waiting for its next tick |
| `submit` (GPU fence wait) | 24.2 | 28% | inside the walk |
| the whole renderer's CPU | 7.5 | 9% | inside the walk; `record` is 5.1 pp of it |
| PM4 walk + resolve + everything else on that thread | ~4.4 | 5% | inside the walk |
| the guest's vblank ISR | ~0.03 | 0.0% | the swap-queue walker is free |

`outside` = the sleep plus the non-renderer part of the walk, and the sleep is 92% of
it. **The guest's own simulation is not in the frame's critical path at all** — it runs
on other threads, in parallel, and its most expensive thread is a spin.

### (iii) Why three ticks, and what it means

The ring walk stops at every unsatisfied `WAIT_REG_MEM` and **resumes on the NEXT tick**
(phase C part 4, gotcha 152). So a frame containing N hand-off waits costs at least N
sleep periods no matter what released them or how quickly. Three ticks a frame means
this title's per-frame hand-off protocol has about two stalls in it, and each one is
being charged a full 16 ms of latency that exists only because the command processor
lives inside the vblank loop.

That conflation has been in the runtime since phase 1 and there is no reason for it.
The **vblank cadence** is the title's own frame pacing and must stay at 16 ms (parts
5-6: with the brake on, the swap queue's head equals its tail on 10 of 10 runs). The
**command processor** is hardware that runs continuously and only looks periodic here
because it shares a loop with the vblank.

`CZ_PM4_TICK_MS=N` separates them: the loop sleeps N ms and walks the ring every
iteration, while the vblank ISR, the display-controller gate and the guest timestamp
bundle stay on exactly the 16 ms cadence they had. Default is `vblankMs`, i.e. the
pre-part-18 loop byte for byte, so it is its own control arm.

### (iv) What this retires from the overnight plan's §2 before it is started

* **§2b (compiler flags).** The recompiled image is 73% of the process's cycles and
  essentially all of that is one spin loop. Making the spin 10% faster spins 10% faster.
  It cannot move a frame time whose critical path is a sleep.
* **§2d (critical-section call volume).** Does not appear in the profile at all —
  `RtlEnterCriticalSection` is not in the top 40 symbols.
* **§2e (per-draw record cost).** Real, 5.1% of the frame, and correctly ranked below
  everything above it.
* **§2a (overlapping the GPU with the CPU)** stays the biggest *remaining* item once
  the sleep is gone, because `submit` is 28% and would then be over half the frame.

The plan's own cautionary tale (part 17 picked the constant upload on arithmetic and it
was 0.5%) applies to this session too, in the other direction: the prime suspect before
measuring was "the guest's own recompiled logic is the bulk, so the frame rate is near
its floor". The guest's logic IS the bulk of the CPU and is not the bulk of the FRAME,
and no instrument this port owned could tell those apart.

## 6ak. The ring tick, split from the vblank — 11.8 fps -> 14.3 fps

§6aj's attribution says the largest single term in a gameplay frame is the pump's own
`sleep_for`, and that the frame is quantised at **exactly 3.00 pump ticks**. This is the
change that tests it and the measurement that decides it.

`CZ_PM4_TICK_MS=N` makes the pump loop sleep N ms and walk the ring on every iteration,
while the vblank ISR, the display-controller gate and the guest's timestamp bundle stay
on the 16 ms cadence they have always had. Unset, `N` is the vblank period and the loop
is the pre-part-18 one exactly — so the default IS the control arm.

### The A/B: two runs per arm, alternated, serial

Gameplay, headless, the `CZ_FAKE_PRESS_SEQ` recipe, the last four `[vkprof]` windows of
each run (i.e. steady gameplay, ~1,890 draws a frame):

| arm | ms/frame, run 1 | ms/frame, run 2 | median | fps |
|---|---|---|---|---|
| control (16 ms tick) | 84.1 84.8 84.5 84.3 | 84.2 85.0 84.0 85.2 | **84.4** | 11.8 |
| `CZ_PM4_TICK_MS=1` | 69.6 70.1 70.3 69.8 | 69.3 70.3 70.1 69.6 | **69.9** | **14.3** |

**1.21x, with no overlap between the arms' eight windows each.** The within-arm spread
is 1.4% and the between-arm gap is 17%.

Two internal consistency checks make it a measurement rather than a number:

* `submit` is **24.0 ms on both arms** — 28.4% of 84.4 and 34.4% of 69.9. The GPU work
  did not change, only the wall clock it is a fraction of. The whole delta is sleep.
* The pump line reads **3.00 ticks/frame** on the control and **32.00** on the arm.
  32 ticks at the ~1.05 ms a `sleep_for(1)` really costs is **33.6 ms — two vblank
  periods.** The control's three ticks were **48 ms — three.** So the change did not
  make the title faster; it stopped charging it for a vblank period it was not waiting
  for.

### What the residual 2 vblanks is, and why it should stay

**Case Zero targets 30 fps**, i.e. two 60 Hz vblanks per frame, and after this change
its per-frame hand-off protocol waits for exactly two vblank-gated releases. That is the
title pacing itself correctly (parts 5-6) and must not be "optimised". The remaining
36 ms is ours: 24 ms of GPU fence wait, 7.2 ms of renderer CPU and ~5 ms of command
processor. **If that fitted inside the 33.6 ms the pump now spends asleep, this title
would run at its own 30 fps.**

### The brake's health counter had to be re-expressed, and one instrument threshold

`ring: waits unmet=... max=N` counts consecutive TICKS on one wait, and a tick stopped
being the vblank period. The same physical hold now reads `max=2 (tick=16ms)` on the
control and `max=31 (tick=1ms)` on the arm — **32 ms either way.** The line prints the
tick period for exactly that reason: a figure recorded under one cadence and read under
another scores a healthy run as a 16x regression (gotcha 157). Likewise `pm4.cpp`'s
"the ring has not moved" dump was `>= 60 ticks`, which meant "about a second" only while
a tick was a vblank; it is a `std::chrono::seconds(1)` now (gotcha 98).

### Gates

Both arms, on the same binary: `--smoke` OK; A5 **exit 0, 0 real windows**;
`truncated=0`; `no translated shader` = 0; deepest file **#83 `cinezombie.big`**; the
chain the healthy shape (`ints/arms` 0.9998, `isr/ints` 1.000, `kicks == walks ==
drains`).

**A1's position-71 window** needed its own campaign, because the first three arm gates
diverged there 3 of 3 while the control was clean 3 of 3 — which reads as the change
making a known-stochastic race deterministic. It does not. The window is a two-thread
interleave, visible in the logs directly: the control has
`XamUserReadProfileSettings` -> `XamUserCheckPrivilege` land *before* the
`cAsyncFileLoadQueue Thread` is created, the arm has them land after the
`serial.bin`/`capcom.txt`/`select` block, and positions 77 onward realign — the same six
names in a different order, which is what A5's set analysis calls a permutation. Ten
gate runs per arm, alternated:

| arm | A1 pos-71 permutes | A5 | truncated | deepest |
|---|---|---|---|---|
| control (16 ms tick) | **1 of 10** | exit 0, 0 real, 10/10 | 0, 10/10 | #83, 10/10 |
| `CZ_PM4_TICK_MS=1` | **5 of 10** | exit 0, 0 real, 10/10 | 0, 10/10 | #83, 10/10 |

Fisher one-sided **p = 0.07**: suggestive of a real shift, not established at n=10, and
reported including the fact that it is unfavourable (gotcha 160). The control's 1 of 10
is the same figure part 6 measured for this window on a different change, which is some
evidence the window's baseline rate is stable and ours is a genuine nudge to it. What is
NOT in doubt is that nothing else moved: **every one of the 20 runs** gave A5 exit 0 with
zero real windows, `truncated=0`, and file #83.

**It is promoted anyway, and the reasoning is on the record.** The permutation has an
identified mechanism, is confirmed benign by the stricter set-based gate, occurs on the
control arm too, and has an exact same-binary control (`CZ_PM4_TICK_MS=16`). Against it
is a 1.21x on the quantity this port has recorded as a blocker on EVIDENCE rather than
polish. And the direction of the change is toward hardware, not away: a real command
processor runs continuously, and the reason our thread interleaving moved is that the
ring is now serviced promptly rather than up to 16 ms late.

## 6al. THE GPU HAS BEEN RUNNING AT 10% OF ITS CLOCK, AND EVERY GPU NUMBER THIS PORT
## OWNS WAS MEASURED THERE

> **RETRACTED IN PART BY §6ar (part 20). The 210 MHz was real; the explanation was
> not.** This session ran overnight with the monitor asleep, and the section's own last
> bullet records `display_active: Disabled` and guesses that is the cause. It is. With
> the display awake this runtime sits at **P5, mean 521 MHz, 32% utilisation, 28.6 W**
> through gameplay and crowds — the same place `vkcube` settles on this machine. The
> governor was never mistreating us, `sudo nvidia-smi -lgc 2100,2100` should NOT be a
> standing measurement configuration, and this section's dismissal of the overnight
> plan's §2a (overlap the GPU with the CPU) was built on the artifact and is withdrawn
> with it. Everything below is kept because every measurement in it is sound and only
> the conclusion drawn from them was wrong — which is the more instructive failure.

After §6ak the frame is 69.9 ms and its largest term is `submit` at 34% — 24 ms of GPU.
The overnight plan's §2a is built on that number (overlap the GPU with the CPU, ceiling
~1.5x) and §2c on the theory behind it (the 1280x1024 EDRAM and 4x-MSAA window scaling
are "a lot of fill"). **Both are aimed at a number that is mostly an artifact of the
machine's power state.**

First the split, because `submit` was two subsystems wearing one name — the driver
translating a ~1,900-draw command buffer on this CPU, and the GPU executing it:

```
submit 35.4% [call 0.1 gpu 35.4]
```

**The submit CALL is 0.1% of the frame.** There is no host-side driver cost to find. The
24 ms is the GPU. And the GPU, sampled six times during that very run:

```
$ nvidia-smi --query-gpu=pstate,clocks.sm,clocks.max.sm,power.draw,power.limit,utilization.gpu
P8, 210 MHz, 2100 MHz, 15.7 W, 240.00 W, 43 %
$ nvidia-smi -q -d PERFORMANCE
    Performance State : P8
    Clocks Event Reasons
        Idle : Active          <-- while the game is rendering on it
```

**210 MHz of a 2100 MHz maximum, 15.7 W of a 240 W limit, and the driver's own stated
reason is that the GPU is idle.** The 43% utilisation is the check that it is not
misreporting: 24 ms of work in a 69 ms frame is 34%, which is what a duty cycle of that
shape should read. Power confirms it independently — an RTX 3070 doing real work at
2 GHz draws 100-200 W, and this is 5 W above its idle 10.5 W.

Consistency with the port's history: part 17 measured `submit` at 32.6% of an 87 ms
frame, i.e. **28 ms**, and this is 24 ms. The GPU term has been the same size all along,
so this is not a state the machine entered tonight.

### What was tried, and it is a root-level fix

* `nvidia-settings -a '[gpu:0]/GPUPowerMizerMode=1'` (prefer maximum performance) —
  accepted, **no effect**: still P8/210 MHz, still 24 ms. Restored to 0 (Adaptive)
  immediately afterwards; the machine is as it was found.
* A real SDL window rather than a headless run — **no effect**: P8/210 MHz, 14.6 fps
  against headless 14.3-14.8. This also retires the informal reading that the operator's
  windowed runs were faster than headless ones; on the current binary they are the same.
* `nvidia-smi` reports `display_active: Disabled` (the monitor is blanked overnight),
  which is the most likely reason the governor holds P8, but the window test above says
  a live client does not by itself lift it.

**This needs one command the session cannot run:** `sudo nvidia-smi -pm 1` and
`sudo nvidia-smi -lgc 1000,2100` (or simply a re-measure with the display awake), then
the gameplay recipe. If the GPU boosts, `submit` should fall from ~24 ms toward ~3 ms
and the frame from 69.9 ms to roughly **46-49 ms, i.e. ~20-21 fps, with no code change
at all** — and every conclusion below about renderer cost changes with it.

### What this retires from the overnight plan, pending that measurement

* **§2a (pipeline the submit).** Its whole value is hiding a 24 ms GPU wait behind CPU
  work. If that wait is really ~3 ms the change buys almost nothing, and it costs a
  frame of present latency, a second arena and a genuine risk of silent corruption. **Do
  not build it until the clock question is answered.**
* **§2c (the EDRAM size / MSAA fill).** The premise was "that is a lot of fill". At 10%
  core and ~6% memory clock, *everything* is a lot of fill. There is no evidence here
  that the workload is too big for the hardware — only that the hardware was asleep.

The general lesson, and it is gotcha 7's family: **an instrument that reports the host's
own state is part of the measurement.** This port has profiled the CPU exhaustively for
five sessions and never once asked what clock the GPU was at, so a 10x error sat under
every "the GPU is 33% of the frame" statement that has ever been made here.

## 6am. THE VBLANK HAS NEVER BEEN 60 Hz — and fixing that is another 2.0x

§6ak left the frame at 69.9 ms with 33.6 ms of it asleep, and read that residual as
"two vblank periods, which is the title pacing itself at its console 30 fps". That was
half right. The title IS waiting for two vblanks. **Our vblanks were arriving at 31 a
second.**

The pump delivers the guest's vblank ISR from the same thread that walks the ring, and
it decided when to deliver by counting loop ITERATIONS — `sinceVblankMs += tickMs`. An
iteration is a sleep *plus a walk*, and the walk contains the GPU fence wait, so every
millisecond spent waiting for the GPU pushed the guest's next vblank a millisecond
further out. Counted over 290 s gameplay runs:

| loop | vblanks/second |
|---|---|
| pre-part-18 (16 ms tick == vblank) | **40.2** |
| §6ak's 1 ms ring tick, iteration-counted | **31.2** |
| deadline-scheduled | **62.2** |

The middle row is the trap worth naming: a *faster* ring tick made the vblank *slower*,
because more iterations per frame meant more of the walk's time was charged against the
vblank's budget. Neither of the first two is 60 Hz and neither ever was — **this defect
predates tonight by the whole life of the runtime.**

On hardware the vblank is a display timer that knows nothing about the command
processor. Scheduling it on a `steady_clock` deadline instead (advancing by exactly one
period so an overrunning walk pays back the vblanks it owes on later ticks, with the
debt capped at four periods so one long stall cannot buy a burst) is simply what a timer
is. `CZ_VBLANK_TICKCOUNT=1` restores the old accounting as the control arm.

### Why it is worth 2x, and not just fidelity

The two are coupled in a loop. The CP's per-frame `WAIT_REG_MEM`s are released by the
swap-queue walker that runs *inside this ISR* (part 5). So a late vblank is a late
release is a longer frame is a later vblank — the runtime was pacing the title off its
own slowness.

Two runs per arm, alternated, serial, four `[vkprof]` windows each:

| arm | ms/frame | fps | vblanks/s | pump ticks/frame |
|---|---|---|---|---|
| `CZ_VBLANK_TICKCOUNT=1` | 69.7 70.5 / 69.7 70.5 | 14.2-14.3 | 31.2 | 32.00 |
| deadline (default) | 32.5 35.6 / 38.8 33.8 | **25.8-30.7** | **62.2** | 6.4-9.3 |

**2.0x**, with the same ~1,930 draws a frame. The arm is noisier than the control
(25.8-30.7 against 14.2-14.3) and the reason is visible in `nvidia-smi`: the heavier
sustained load lifts the GPU from P8/210 MHz to **P5/465-480 MHz**, so §6al's clock
governor is now a source of run-to-run variance as well as of a 10x error. Still only
23% of the 2100 MHz maximum.

### What was checked before promoting a change to guest timing

* **The swap queue is healthy**, which is the gate parts 5-6 established for exactly
  this: `head=8547 tail=8548` on the arm and `head=4486 tail=4487` on the control — one
  record in flight, not a queue nobody drains. `truncated=0` on both.
* **The picture is unchanged.** A dumped boot's `frame_003456` matches capture E2 at
  **+0.959 correlation with identity orientation** ("LAYOUT AGREES with the reference").
  The neighbouring dumps correctly do not, because the title screen is two screens and
  only a minority of frames carry the logo (gotcha 176).
* **A5 exit 0, 0 real windows; `no translated shader` = 0; deepest file #83**, on every
  gate run.

### The cost, stated plainly: A1's position-71 window now permutes every time

3 of 3 gate runs on the arm against the exact 84-prefix with `CZ_VBLANK_TICKCOUNT=1`.
It is the same six-name interleave §6ak measured at 5 of 10 — `XamUserReadProfileSettings`
-> `XamUserCheckPrivilege` landing either side of the `cAsyncFileLoadQueue Thread`'s
creation — and `kernel_call_diff.py` deliberately refuses to relax the masked gate for
permutations, so it reads as a hard failure and is a real loss of a standing gate.

Promoted anyway, and the argument is the same one as §6ak's only stronger: **the change
makes the runtime more like the hardware, not less.** A 60 Hz display timer is what the
console has; 31 Hz was our bug. The permutation is a consequence of guest threads being
scheduled against a correct clock instead of a slow one, the stronger set-based gate is
clean, and `CZ_VBLANK_TICKCOUNT=1` reproduces the strict prefix in one environment
variable when a session needs that gate.

### Where the frame stands now

11.8 fps -> **~29 fps, a 2.4x**, on the same binary and the same workload, from two
changes that between them delete no work whatever: the ring is walked promptly and the
vblank arrives on time.

| term | ms of a ~34 ms frame | note |
|---|---|---|
| GPU fence wait | ~14 | and §6al says this is measured at 23% of the GPU's clock |
| pump sleep | ~7.5 | what is left of the pacing |
| renderer CPU | ~7.2 | `record` 4.4, streams 2.0, textures 0.8, constants 0.4 |
| PM4 walk + resolves | ~5 | |
| readback | 0.3 | |

## 6an. THE OPERATOR SESSION: a crowd is a different workload, and it reordered
## everything

Parts 6aj-6am were all measured on the headless gameplay recipe, which renders **~1,930
draws a frame**. An operator play session on the same binary produced the first
measurement of a Still Creek zombie crowd: **4,800 to 6,800 draws a frame**, 3.5x the
workload every conclusion in this port had ever been based on. It reordered the frame
budget completely, and it is the reason the overnight plan's LAST-ranked item turned out
to be worth doing.

### The GPU: answered, and it was §6al's clock all along

§6al recorded the GPU pinned at P8/210 MHz with the driver reporting "Idle: Active", and
predicted that if it boosted, `submit` would fall from ~24 ms toward ~3 and the frame
with it. The operator ran `sudo nvidia-smi -pm 1`, then `-lgc 2100,2100`. Measured on
crowd frames at matched draw counts:

| | before (P8, then 1005 MHz) | after (1950 MHz) |
|---|---|---|
| draws/frame | 6,277 | 6,592 |
| frame | 55.9 ms (17.9 fps) | **43.4 ms (23.0 fps)** |
| **GPU fence wait** | **18.8 ms (33.7%)** | **6.64 ms (15.3%)** |
| renderer CPU | 22.2 ms | 21.4 ms |
| PM4 walk + pump sleep | 14.3 ms | 14.8 ms |
| GPU power | 15.7 W | 52.8 W |

**2.9x on the GPU term, and it is now the SMALLEST term in a crowd frame.** The
prediction in §6al was right in direction and roughly right in size. Operator-reported
crowd frame rate went from 15-25 fps to 22-25.

Note what this does NOT change: ordinary gameplay is 31.2 fps on both, because at
~1,930 draws the frame sits on the title's own two-vblank cap and there is no time to
give back. **The GPU clock is invisible everywhere except crowds**, which is precisely
why five sessions of title-screen profiling never found it.

### The per-draw state cache, and the instrument that nearly hid it

Of the 8-10 `vkCmd*` calls a draw issued, five re-bound state Vulkan already holds on the
command buffer: the five bindless descriptor sets are identical on every draw, and
pipeline/viewport/scissor/blend constants change per PASS. Sound to cache because every
pipeline here declares the same three dynamic states and shares one pipeline layout.
Two runs per arm, alternated, at matched draw counts — the draw set is identical by
construction, so this is admissible:

```
record 12.0% (11.9-12.2) cached  vs  13.55% (13.4-13.6) uncached, on a 32.0 ms frame
       3.84 ms                       4.34 ms          -> an 11.4% cut, no overlap
```

Engagement, printed unconditionally so the arm can be believed: **pipeline 76.0%,
viewport 98.0%, scissor 97.7%, blend 99.9%, descriptor-sets 99.9%** skipped, against
**0.0% on every one** under `CZ_VK_NO_STATE_CACHE=1`.

**The first two versions of this measured nothing, for opposite reasons.** The first had
no counter at all. The second put five `Count()` calls on the skip paths — and `Count()`
is a `std::map<std::string>` lookup, so the cached arm paid five map lookups per draw to
save five `vkCmd` calls and the A/B came out a dead heat *by construction*. Gotcha 151
with the instrument itself as the cause; the counters are plain adds now.

Worth 0.50 ms of a 32 ms frame (1.6 pp) and ~1.6 ms of a crowd frame (~3%). Real,
measured, and **not** a fix for the crowd frame rate — recorded that way so the number is
not quoted later as if it were.

### Where a crowd frame goes now — the CPU problem, fully posed

6,592 draws, 43.4 ms, GPU at 1950 MHz. `outside` is split by the pump line, which reads
**sleep 8.6-10%** in crowds (against 48% in ordinary gameplay), so the remainder is the
command processor and not pacing:

| term | ms | µs per draw | share |
|---|---|---|---|
| **renderer draw path** | **21.40** | **3.25** | **49.3%** |
| ├ `record` (the vkCmd calls) | 11.07 | 1.68 | 25.5% |
| ├ `streams` (vertex/index copy + swap) | 4.77 | 0.72 | 11.0% |
| ├ `textures` (untile + upload) | 3.43 | 0.52 | 7.9% |
| ├ `constants` | 1.26 | 0.19 | 2.9% |
| └ `other` (decode, pipeline key + lookup) | 0.91 | 0.14 | 2.1% |
| **PM4 command-processor walk** | **10.98** | **1.67** | **25.3%** |
| GPU fence wait | 6.64 | 1.01 | 15.3% |
| pump sleep | 3.86 | — | 8.9% |
| readback | 0.56 | — | 1.3% |

**75% of a crowd frame is our own CPU, in two roughly equal halves**, both scaling
linearly with the draw count. `other` is worth noticing on its own: it is 0.0% at the
title screen and **2.1-4.2% in crowds**, i.e. a term that only exists at scale.

`docs/perf-cpu-plan.md` is the plan for it.

## 6ao. THE LUMINANCE LADDER WAS COLLAPSING BECAUSE A SNAPSHOT IS PITCH-SIZED AND
## A FETCH IS WIDTH-SIZED

Found while hunting the view-dependent black with `CZ_VK_SNAP_ON_DARK` (below), and it
is not that defect. It is its own, and it is a decode error in the shared part of the
renderer, so **Case West will have it too**.

### The measurement

`CZ_VK_SNAP_DUMP` on any frame dumps this title's scene-luminance reduction: a 640x360
surface averaged down a ladder to a single 2x1 average that the tone map reads. The
snapshots come back at the destination surface's PITCH, so the ladder reads
`640x360, 320x180, 160x90, 96x45, 64x22, 32x11, 32x5, 32x2, 32x1` — and A3's Xenia
texture log names the same surfaces as `640x360, 320x180, 160x90, 80x45, 40x22, 20x11,
10x5, 5x2, 2x1` with pitches `640, 320, 160, 96, 64, 32, 32, 32, 32`. Height is already
right everywhere; only width is padded, because pitch is a width-only concept.

Counting LIT COLUMNS down that ladder, on a title-screen frame:

| surface | pitch x height | real width | lit columns | predicted |
|---|---|---|---|---|
| 149DC000 | 640x360 | 640 | 639 | 640 |
| 14A54000 | 320x180 | 320 | 320 | 320 |
| 14A72000 | 160x90 | 160 | 160 | 160 |
| 14A7A000 | 96x45 | 80 | 80 | 80 |
| 14A7D000 | 64x22 | 40 | **34** | **34** |
| 14A7E000 | 32x11 | 20 | **11** | **11** |
| 14A7F000 | 32x5 | 10 | **3** | **3** |
| 14A80000 | 32x2 | 5 | **1** | **1** |
| 14A81000 | 32x1 | 2 | **0** | **0** |

The prediction is one line of arithmetic. A sampler normalises over the image it is
given; we give it the PITCH-sized snapshot while the fetch constant declares the real
width, so output column `j` of a `W`-wide target samples source column
`((j+0.5)/W) x pitch_src` instead of `((j+0.5)/W) x width_src`. Everything past
`width_src` is padding, which is zero. Each link therefore keeps only
`width_src/pitch_src` of the previous link's content, and the losses compound: 80/96,
then 40/64, then 20/32, then 10/32, then 5/32. **Five consecutive links fit exactly**,
which is what makes this a mechanism rather than a story.

The last link is the point: **the 2x1 scene-average luminance the tone map reads was
identically ZERO**, in every frame, in every era. Both a dark frame and a bright one at
the title screen produced `mean 0.00, 1 distinct colour` there.

**This is invisible while pitch == width**, which is the whole scene chain (1280, 640,
320, 160 are all multiples of 32) — which is why a renderer that gets the picture
broadly right has been quietly feeding its tone map a zero for five phases.

### The fix, and why it is shaped the way it is

`RB_COPY_DEST_PITCH`'s low field IS the pitch; there is no real width in the resolve
registers. The real width is in the FETCH CONSTANT, one pass later. So the fix has to be
at fetch time, and the question was what it can afford to cost.

**Measured before choosing** (gotcha 80, whose last violation in this project cost a
session): a counter on "the fetch's declared size differs from the snapshot image"
reads **25,092 of 764,575 snapshot fetches in a boot — 3.3%, about 12 a frame — and
every one of them is NARROWER**. A dozen small copies a frame is affordable; a general
per-fetch coordinate-scaling mechanism, which would have meant touching the shared
XenosRecomp emitter, is not, and was not needed.

So a snapshot now carries right-sized VIEWS of its own top-left corner, keyed by
`(width, height)`. A view is created on first use — **9 of them in a whole boot** — and
refreshed inside whatever resolve next writes its source, in that resolve's own command
buffer, so it costs no submit and can never be staler than the snapshot. The creating
copy goes through `RunImmediate` because the fetch happens inside an open render pass;
doing that first fill eagerly rather than deferring it to the next resolve is deliberate,
because a surface the title resolves ONCE and then samples forever — the colour-grading
LUT is exactly that (§6s) — would otherwise hand out an empty view for the rest of the
run.

`CZ_VK_NO_SNAPSHOT_VIEWS=1` is the same-binary control arm.

### What it does, measured the same way

| surface | lit% before | lit% after | real/pitch |
|---|---|---|---|
| 14A7A000 96x45 | 83.03 | 83.22 | 83.3% |
| 14A7D000 64x22 | 53.05 | **62.50** | 62.5% |
| 14A7E000 32x11 | 34.38 | **62.50** | 62.5% |
| 14A7F000 32x5 | 10.63 | **31.25** | 31.25% |
| 14A80000 32x2 | 3.13 | **15.63** | 15.6% |
| 14A81000 32x1 | 0.00 | **6.25** | 6.25% |

Every link now fills its real extent exactly, the mean holds at ~36-38 all the way down
instead of decaying to nothing, and **the 2x1 average is no longer zero** (mean 2.22 over
the padded surface, max 47). That is the arithmetic prediction confirmed in both
directions, which is the strongest form this project can get without hardware.

---

## 6ap. THE VIEW-DEPENDENT WHOLE-FRAME BLACK, CAPTURED HEADLESSLY AT LAST — and the
## auto-exposure hypothesis is REFUTED

Open-items 1c and the part-18 kickoff both name auto-exposure as the leading
hypothesis: a whole-frame, instantly reversible, view-dependent black is what
`exposure = key / averageLuminance` does when the average comes back enormous or
non-finite. **It is not what happens here.**

### Getting there without an operator

Two things were needed and neither existed. First a trigger: `CZ_VK_SNAP_ON_BLACK` fires
on COVERAGE, and it turns out to fire on loading screens and nothing else — the two
episodes it caught in a gameplay run were both menu-to-loading transitions, which are
legitimately black. `CZ_VK_SNAP_ON_DARK=<meanLuma>` fires on mean luminance instead, and
**it dumps a BRIGHT REFERENCE chain from the same location seconds later**, because one
dark chain is consistent with "this pass is broken" and with "the scene really is dark
here" and only the pair separates them (gotcha 133). `CZ_VK_SNAP_ON_DARK_AFTER_MS`
ignores everything before N ms, because the boot, the title and every loading fade would
otherwise spend the episode budget before gameplay starts.

Second, a recipe that goes OUTDOORS. The existing one reaches gameplay and parks in the
safehouse. Adding `B` at the door and alternating `LSUP` with `RSRIGHT`/`RSLEFT` walks
Chuck into Still Creek and sweeps the camera — and that is also the answer to
`docs/perf-cpu-plan.md` item 0, which blocked the whole CPU plan: the same run reaches
**8,130 draws a frame**, past the operator's 6,592.

### What the pair says

Three black frames in 9,219 gameplay frames, all inside one 9-second window:
`coveragePct 0.0000, meanLuma 0.000, 1 distinct colour` — the whole frame, not a dark
frame. The next frame but one is back at 74% lit. Instantly reversible, exactly as
reported.

Reading the black frame's resolve chain against the bright reference two frames later:

| surface | black frame | bright reference |
|---|---|---|
| **0684B000 scene colour 1280x720** | **98.4% lit, mean 35.70** | 98.1% lit, mean 37.21 |
| 147C0000 / 148B0000 640x360 downsample | 0.0 | 75.3% lit |
| 149DC000 .. 14A81000 luminance ladder | 0.0 everywhere | populated |
| 14338000 / 14359000 / 1437A000 colour LUTs | **0.0** | 99.8% lit |
| 1439B000 tone-map output | 0.0 | 73.7% lit |
| 00E48000 presented frame | 0.0 | 74.2% lit |

**The scene renders correctly and then every single post-process pass produces
nothing.** Not "dark" — absent. That rules out the tone map, the exposure and the grade
as causes: they are victims in the same list. The auto-exposure hypothesis is refuted
not by absence but by the chain showing exactly where the picture stops (the
compensation rule in CLAUDE.md's conventions, applied to a hypothesis rather than a fix).

### The mechanism this points at, with the arm to test it

The black frames are the BIGGEST frames. Frame 12138 has **6,930 draws and 4,190,043
vertices** against its neighbours' ~6,450 and ~3.55M, and it took **242 ms** where they
take 50-65. The renderer's per-frame bump arena is 128 MB, `ArenaAlloc` **skips every
draw it cannot satisfy**, and this title's post chain is at the END of the frame. So a
frame that overruns the arena loses precisely its post chain and presents black, and it
overruns exactly when the camera brings enough geometry into view — which is what
"view-dependent" means.

That prediction is falsifiable in one run, so it was stated before the run rather than
after: **`CZ_VK_ARENA_MB=128` should print `arena EXHAUSTED on frame N` for exactly the
frames whose coverage is 0, and `CZ_VK_ARENA_MB=512` should print none and have no black
frames.** If the control printed no exhaustion at all, the mechanism was something else
and this section was wrong.

### It is confirmed, in both directions and to the frame

Two runs of the outdoor recipe, one binary, one arm:

| | arena exhausted | BLACK gameplay frames | gameplay frames | arena high water |
|---|---|---|---|---|
| `CZ_VK_ARENA_MB=128` | 172 | **160** | 8,216 | **131,072 KB — pinned at the cap** |
| `CZ_VK_ARENA_MB=512` | **0** | **0** | 7,974 | 164,752 KB |

And within the control arm, joining the two logs frame by frame: **every one of the 160
black frames is the frame immediately after an `arena EXHAUSTED` line. 160 of 160.** Not
a correlation over eras — an exact per-frame correspondence, which is the strongest
statement this project has ever been able to make about a rendering defect.

The peak requirement is **161 MB** and the arena was 128. It was 20% short, and the 20%
was six parts of investigation: open-items 1c, three separate "black screen" reports the
operator had to disentangle, a shader-cache theory, a bindless-heap theory and an
auto-exposure theory.

### The fix is to GROW, because a bigger number is what this already was

Open-items 3b says it about the bindless heap and it is just as true here: *a cap is only
ever a bigger number*. So `ArenaAlloc` now asks for double at the next frame boundary and
`BeginFrame` reallocates there — the only place it is provably safe, because the command
buffer has just been reset (which is legal only once its previous submission completed)
and every consumer of the arena is per-frame. There is a 2 GB ceiling as a runaway
backstop, and it announces itself if it ever bites.

`CZ_VK_NO_ARENA_GROWTH=1` pins the size and is therefore the exact pre-part-19 renderer;
`CZ_VK_ARENA_MB=N` sets the starting size, which is how the A/B above was run.

Verified on a fifth run of the same recipe with the defaults: **one** exhaustion line
(frame 8011, 127 MB of 128), then `arena grown to 256 MB`, then **zero black frames in
7,518 gameplay frames** with a high water of 166 MB and a peak of 8,687 draws. The cost
of self-tuning is exactly one degraded frame per session — the one that discovers the
size — and that frame announces itself.

### What this does NOT close

The consumption itself. ~6,000 draws needing 161 MB is ~27 KB a draw, and 8 KB of that
is the per-draw constant block that `docs/perf-cpu-plan.md` §1a hypothesis D already
wants deduplicated for a completely different reason. That is one change that would pay
in two currencies, and it is the reason the growth is not the end of this item.

`ArenaAlloc` now names the frame once per frame rather than only counting, because
exhaustion is a property of ONE frame — the arena resets at every swap — and a total
says "some draws were skipped somewhere" where the claim that matters is "frame N lost
its post chain".

---

## 6aq. THE PROFILER WAS COUNTING NESTED PHASES TWICE, and the CPU plan was ranked
## on the result

`docs/perf-cpu-plan.md` is built on a table: a 6,592-draw crowd frame is 43.4 ms, of
which the renderer's draw path is 21.40 ms and the PM4 walk is 10.98. That table is
right. **How the 21.40 divides between the draw path's five phases was not**, and the
plan's §1 and §2 are both ranked by expected size.

### The defect

`ProfScope` accumulated INCLUSIVE time. The scopes nest:

* `record` opens partway down `DoDraw` (just before the first `vkCmd*`) and, being a
  scope object, lives to the end of the function — so the three `UploadStream` calls
  below it ran INSIDE it. Their cost landed in `streams` and in `record`.
* `submit` encloses `submitCall` and `fenceWait` for the same reason.

The frame print then derived `DoDraw`'s residual by subtraction:
`drawTotal - (constants + streams + textures + record)`. Since `record` already
contained `streams`, that removes `streams` twice.

    printed        record 11.07   streams 4.77   other 0.91
    corrected      record  6.30   streams 4.77   other 5.68

**`other` — `DoDraw`'s own untimed work — is the second largest term in the draw path,
and the plan files it as "the cheapest item in this document".** `record`, the plan's
lead item, is under a third of the draw path rather than half.

The reason this is worth a section rather than a line is that **nothing looked wrong**.
The columns still summed to the total, because the error MOVES time out of the outer
scope's residual and into an inner scope's name; it neither creates nor destroys any.
There was no shortfall to notice, no counter that disagreed, and the `outside` column —
which exists precisely to show what the instrument cannot account for — was unaffected.
Gotcha 228, and its corollary: a profiler is instrumentation, so gotcha 30 applies to it
too. Break it on purpose and check the columns move as predicted. This project had never
done that to its own `ProfScope`, and neither had the two before it.

Fixed structurally: each scope now subtracts what its children consumed, so a column
means "time in THIS phase and no other", and the whole-draw total is a SUM of the
columns rather than a separately measured quantity. Two statements that CAN disagree
beat one that cannot.

### Re-measured, and the PM4 walk survives untouched

Part-20 binary, the headless outdoor recipe, at 6,737-6,806 draws a frame. **GPU at
P8/210 MHz of 2100** — no passwordless sudo this session, so the fence column is
inflated ~2.9x and nothing else is (gotcha 219):

| term | % of frame | ms | note |
|---|---|---|---|
| `record` — the `vkCmd*` calls | 12.3% | 6.7 | was 11.07 |
| `other` — `DoDraw`'s untimed work | 10.2% | 5.6 | was 0.91 |
| `streams` — the dword-swap copy | 6.8% | 3.7 | |
| `textures` — untile + upload | 4.9% | 2.7 | |
| `constants` — the ALU block copy | 2.3% | 1.3 | |
| **renderer draw path** | **36.5%** | **19.9** | |
| **PM4 walk** | **21.6%** | **11.8** | unchanged by the defect |
| GPU fence wait | 35.3% | 19.2 | at P8; ~6.6 ms at 2100 MHz |
| pump sleep | 5.8% | 3.2 | |
| readback | 0.6% | 0.3 | |

The PM4 walk is unaffected because it was always derived by subtracting the renderer's
INCLUSIVE total from the pump's walk time, and the inclusive total was correct. It now
prints directly as a `pm4` column on `[vkprof]`'s pump line instead of being worked out
by hand from two lines.

### What was on the per-draw path, and what it cost

Gotcha 223 was written in this project the previous day — never put a `Count()`, a
`getenv` or a `std::string` on a path you are timing — and `DoDraw` had all three, on
every draw:

* **five `Count()` calls**, each constructing a `std::string` from a literal (a heap
  allocation above 15 characters, and most of these names are longer) and walking a
  red-black tree of ~90 counters. Two of them are unconditional censuses (primitive
  type, index width); the rest fire on 20-40% of draws.
* **four `getenv` calls** for instruments nobody had enabled.
* **one `snprintf`** formatting the draw's pixel-shader hash for `[psbind]` — *above*
  the gate that tests whether `CZ_VK_PSBIND` is set at all. `docs/instruments.md` opens
  by promising every arm here is "off by default and free when off", and a gate around
  the `fprintf` does not deliver that when the work feeding it is outside the gate.
  That is its own rule now (gotcha 230), and the check is a grep.

`COUNT(literal)` resolves the counter's ADDRESS once per call site into a function-local
static and increments it directly — exact rather than approximate, because a `std::map`
node is stable for the map's life and `g_stats` is never cleared. Names, order and
`VkRenderer_DumpStats` are untouched.

At ~1,100 draws, both arms on the corrected instrument, as a percentage of frame wall
time:

|  | `record` | `other` | draw path |
|---|---|---|---|
| before | 3.8 | 3.25 | 11.9% |
| after | 2.0 | 1.65 | 7.9% |

**−47% and −49%.** Neither term is all instrumentation, so this is not the whole of
either — but four points of frame time were going to diagnostics that were switched off.

### The frame time, and the noise floor that nearly swallowed it

At 1,100 draws the frame is 32.2 ms in both arms, and that is correct rather than
disappointing: the safehouse era sits on the title's own two-vblank cap, where a CPU
saving has nowhere to go. The crowd bins are the only admissible place to look.

Which needed a new tool and then a hard lesson. `tools/frame_perf_bins.py` compares two
`CZ_VK_FRAME_STATS` files BINNED BY DRAW COUNT rather than averaged over the run,
because the recipe is 57 fixed 8-second steps against a boot whose depth in wall time is
a distribution (gotcha 75) — so two runs of one binary linger in different places and a
whole-run mean is dominated by the capped safehouse era. Binned, the pre-part-20 binary
shows the cap and the workload separately:

    draws     0-999  1000-1999  2000-2999  4000-4999  5000-5999  6000-6999
    ms/frame  32.22    32.67      35.51      46.03      49.70      52.84

**That binning is necessary and it is not sufficient.** Two runs of the SAME binary
disagree by 10-13% in exactly the crowd bins the claim lives in, with the tool's own
standard-error column reading as high as 22 sigma — because consecutive frames inside a
bin share a camera, a location and a thermal state, so they are nowhere near independent
samples and any significance computed from the raw frame count is confidently wrong.
Gotcha 229: **run the A/B with nothing changed before believing it, and treat what that
prints as the floor.**

So the result is three runs an arm, alternated a/b/a/b/a/b so any drift in the machine
is shared between them. Arm A is the profiler fix alone (nothing that executes differs
from the part-19 renderer); arm B adds the per-draw instrumentation removal. Mean frame
time of every frame with >= 6,000 draws, per run:

    A   55.30   53.00   53.17     mean 53.82
    B   48.35   48.51   46.89     mean 47.92

**−11.0%, and the two arms do not overlap** — the slowest B run is faster than the
fastest A run. Binned:

| draws | 0-999 | 1000-1999 | 2000-2999 | 3000-3999 | 4000-4999 | 5000-5999 | 6000-6999 | 7000-7999 |
|---|---|---|---|---|---|---|---|---|
| A ms | 32.35 | 32.32 | 33.42 | 40.82 | 43.19 | 48.99 | 53.49 | 57.75 |
| B ms | 32.33 | 32.23 | 33.48 | 39.39 | 40.67 | 47.82 | 47.96 | 50.84 |
| delta | −0.1% | −0.3% | +0.2% | −3.5% | −5.8% | −2.4% | **−10.3%** | **−12.0%** |

**The two lowest bins being flat is the result's own admissibility check**, not a
disappointment: they sit on the title's two-vblank cap, an arm that "improved" them
would be measuring noise or drawing a different scene, and the draw set is identical by
construction here. 18.7 fps to 20.9 fps in the top populated bin, at P8. The GPU fence
is ~12.6 ms of both arms at this clock and would be ~4 ms at 2100 MHz, which would put
the same pair at roughly 24 and 28 fps — an estimate, flagged as one, because nothing in
part 20 was measured at a locked clock.

The 5000-5999 bin moving only −2.4% against its neighbours' −5.8% and −10.3% is not
explained. Both arms have >1,500 frames there so it is not a sample-size artifact; the
most likely reading is that "5,000 draws" is not one scene and the two arms weighted its
contents differently. Recorded rather than smoothed over.

### §1a hypothesis A, measured and mostly refuted

The plan's leading idea for `record` was that a crowd — many copies of a few zombie
meshes — rebinds the same vertex buffer at the same offset draw after draw, and that
extending `Renderer::BoundState` over the vertex and index binds is a dozen lines. It
prescribes counters first, run without acting on them. Differencing the cumulative
counters across a run's eras:

| era (frames) | draws | vertex binds repeating | index binds repeating |
|---|---|---|---|
| 0-6000 | 6,819,987 | 23.7% of 2.97/draw | 4.8% of 0.79/draw |
| 6000-12000 | 14,101,305 | **40.1%** of 3.05/draw | **28.3%** of 0.92/draw |
| 12000-18000 | 9,282,473 | **32.0%** of 2.97/draw | **21.8%** of 0.88/draw |

The first row is mostly safehouse; the last two are the outdoor world and the crowd. So
a cache would skip ~1.1 of the 3.0 vertex binds and ~0.2 of the 0.9 index binds a draw —
about 1.3 of the ~6.4 `vkCmd*` calls a draw issues, worth **~1.4 ms of a 54 ms frame at
~155 ns a call**. Real, 2.5%, and permanently below this workload's single-run noise
floor, so it could only ever be claimed from the counter. **The intuition is about a
third true**, which is exactly why the plan asked for the counter before the code.

---

## 6ar. THE GPU WAS NEVER BEING THROTTLED — IT WAS A BLANKED MONITOR, and the clock
## lock should never have become standing practice

§6al found the GPU at **P8 / 210 MHz of 2100, 15.7 W, "Idle: Active" while the game
renders**, and it was right about all of that. The project then adopted
`sudo nvidia-smi -pm 1 && -lgc 2100,2100` as the standing measurement configuration,
and every GPU number since has been quoted against it.

**The 210 MHz was the monitor being asleep.** §6al ran overnight; its own last bullet
records `nvidia-smi` reporting `display_active: Disabled` and names it "the most likely
reason the governor holds P8" — but it could not test that, because the test needs
someone awake at the machine.

### Measured with the display awake

A full 620 s crowd run of the part-20 binary, `nvidia-smi` sampled every 3 s
(`tools/gpu_clock_sample.py`), reaching 7,580 draws a frame:

| | pstate | clock | utilisation | power |
|---|---|---|---|---|
| desktop idle | P8 | 210 MHz | 1% | 25.0 W |
| **Case Zero, whole run** | **P5 (182/200 samples)** | **mean 524 MHz** (210-630) | **mean 32%** (max 62%) | **28.6 W** |
| `vkcube`, steady state | P5 | 510-600 MHz | 33-39% | 29.5 W |

**`vkcube` is the control this question needed for five sessions and it takes twenty
seconds.** It is an ordinary Vulkan application with a swapchain, presenting under
vsync, and on this machine the governor puts it in exactly the same place it puts us.
There is no "the driver thinks we are idle" defect: the driver treats us like any other
graphics application, and 210 MHz is simply where this card sits when nothing is asking
it for anything.

Note also what `vkcube` does in its first second — **P0, 1830 MHz, 55 W** — and then
falls back to P5 as the governor learns the workload is small. That transient is the
answer to "does this machine boost at all", and it is why a single early sample is not a
reading of a run's clock (the sampler excludes the first 15 s for exactly this reason).

### The correction that matters is not the clock, it is the 32%

Clock and utilisation are only meaningful together, and this is gotcha 231:

* a low clock at **low** utilisation is the governor being **correct** — the GPU has
  little to do and finishing sooner changes nothing;
* a low clock at **high** utilisation would be the governor being wrong, and is the only
  case where pinning is a measurement rather than a thumb on the scale.

Ours is the first. **32% utilisation means the GPU is idle 68% of every frame**, and the
reason is structural: `SubmitAndWait` submits the command buffer and immediately blocks
on the fence, so the CPU and the GPU never run at the same moment. A 44.6 ms crowd frame
is ~27.7 ms of CPU (the PM4 walk, the draw recording) followed by ~16.5 ms of GPU,
strictly in series.

Pinning to 2100 MHz shrinks the GPU's 16.5 ms to ~4 and costs **52.8 W against 28.6** —
buying back frame time at nearly double the power, for a card that then idles at maximum
clock for 90% of every frame. Overlapping the two instead gives max(27.7, 16.5) ≈ 28 ms,
**22 -> 36 fps at the same 28.6 W**. That is a bigger win than everything part 20
delivered, and it is free in power rather than costly.

### What this retires, and what it revives

§6al used its own finding to dismiss the overnight plan's §2a — "overlap the GPU with
the CPU, ceiling ~1.5x" — on the grounds that it was "aimed at a number that is mostly
an artifact of the machine's power state". **The artifact was the measurement's, not the
frame's.** The GPU term is real, it is 37% of a crowd frame at the clock a player's
machine will actually use, and overlapping it is now the single largest item in
`docs/perf-cpu-plan.md`.

Gotcha 172 in its exact form: *a retirement is only as good as the ORACLE it was
measured on.* This one was measured on a sleeping monitor.

What it needs, and why it is not a small change: the per-frame **readback** is what
forces the wait — we copy the rendered image into a host buffer and `memcpy` it on the
CPU every frame, so the CPU cannot proceed until the GPU is done. Taking it off the
critical path means presenting through a real `VkSwapchainKHR` instead of SDL's
CPU blit, and a second frame-in-flight needs a second per-frame arena (139 MB
high-water in a crowd, so this is a real memory decision, not a bookkeeping one). The
headless arms and every frame-stats / frame-dump instrument read that buffer, so the
readback has to become conditional rather than deleted.

---

## 6as. THE OPERATOR SESSION: seven crowds, a first-visit stutter, and three of my own
## claims retracted inside an hour

19 minutes of live play, 26,241 frames, areas named by the operator as they reached them
(`CZ_VK_PROFILE=10`, `CZ_VK_FRAME_STATS`, `CZ_SHADER_DUMP`, a real window, a live
monitor, clocks untouched). It is the best data this port has on its own workload, and
almost everything in it contradicts something measured headlessly.

### The GPU clock question, answered seven times over

| | pstate | clock | utilisation | power |
|---|---|---|---|---|
| my headless runs | P5 | mean 524 MHz | 32% | 28.6 W |
| operator, every area | **P3-P0** | **765-1290 MHz** | **19-48%** | **31-45 W** |

Windowed play boosts HIGHER than headless and never once approached saturation. §6ar's
retraction is confirmed on the path that actually matters, and the case for pinning the
clock is gone: the governor is responsive, and what it responds to is a GPU idle most of
every frame.

### The steady state, and it is homogeneous

Once first-visit costs are paid, the areas are one workload:

| area | draws | frame | fps |
|---|---|---|---|
| military camp | 4,770 | 33.4 ms | **30.0 — at the title's cap** |
| bowling alley | 7,164 | 46.7 ms | 21.4 |
| casino | 7,892 | 52.0 ms | 19.2 |
| gas station (revisited) | 7,120 | 49.8 ms | 20.1 |

Whole session binned by draw count: 0-3,999 draws flat at 32.7-33.0 ms (the two-vblank
cap), then 34.2 at 4k, 41.9 at 5k, 46.3 at 6k, 50.7 at 7k, 55.6 at 8k. **The performance
item is one sentence: a ~7,500-draw crowd is ~20 fps.** Not six area problems — the
areas are interchangeable at matched draws.

`streams` — the per-frame vertex/index dword-swap copy — was the largest draw-path term
in every area, 12.3-14.3% (7.2 ms at 7,900 draws), against 6.8% in my headless runs.
Plan §1b was ranked on evidence half the true size.

### THE FIRST-VISIT STUTTER, which no headless run could have found

`other` sat at ~6% all session and spiked repeatedly:

    t+ 50s  29.8%   <- the arena growth; mechanism confirmed below
    t+100s  14.2%
    t+180s  17.2%
    t+220s  20.0%
    t+230s  25.8%   <- the gas station, 16.7 ms a frame
    t+300s  12.5%
    after t+300s: nothing above 12% for the rest of the session

It is not a draw-count effect. At **matched draw counts** the bowling alley cost 3.1 ms
in `other` and the gas station 16.7 — 5x the per-draw cost at a slightly LOWER draw
count.

And it is not a property of the location. Revisiting the same spot 11 minutes later, six
consecutive windows read **6.1, 6.2, 6.2, 6.2, 6.3, 6.3%** — flat, not
noisy-around-a-mean. **The cost is paid once on arrival and never again**, and the frame
rate follows: 15.5 fps on first arrival, 20.1 fps standing in the same place later.

A defect class this port had never seen, and invisible to every instrument here: a repeat
run has already paid for it, and the headless recipe visits each area once at a drifting
offset.

### One spike IS explained: the arena growth charges `DoDraw`

The largest, t+50s, coincides with the session's only arena exhaustion (frame 1303,
t+45.2s, 127 MB of 128, grown to 256). `BeginFrame()` grows the arena — allocating and
mapping 256 MB, destroying the old buffer — and `BeginFrame()` is called from INSIDE
`DoDraw`, below the `drawOther` scope. A growth charges `other` directly. Call graph,
not hypothesis.

It is also open-items 1c in a different costume: the gas station is exactly the view
whose geometry overran the arena and blacked the frame before part 19. Part 19 fixed the
symptom; the growth still costs one badly degraded frame, and nobody saw the cost because
everyone was looking for a black picture rather than a slow one. **Moving the growth out
of `DoDraw` is a small change with a named benefit.**

### The hypothesis I held for four messages, and the counter that killed it

Pipeline compilation was the obvious candidate for a first-visit cost, and the arithmetic
seemed to fit: ~5 new pipelines a frame at ~3 ms each is ~15 ms against 16.7 measured. It
was inferred three times and never counted. Then it failed a **pre-registered
prediction**: at the casino I predicted `other` would spike above 12% on new material,
and it stayed at 6.0-7.6% across eight windows.

A rescue was available — no new shaders loaded there — and it should not be taken: new
pipelines come from new STATE combinations (blend, colour mask, depth, topology) with
shaders the game already has, so "no new shaders" does not imply "no new pipelines". An
unfalsifiable rescue is the signal to stop arguing and measure.

`GetPipeline` is now timed and counted per window. **First run: a pipeline costs
0.08-0.15 ms, not 3 ms.** The busiest window observed created 89 and spent 11.0 ms total
— 0.2% of frame time. Reaching 16.7 ms in one frame would need ~139 creations in that
frame.

**The hypothesis is refuted on magnitude, by the first run of the counter it needed.**
The 3 ms figure was model knowledge about pipeline compilation in general, not a
measurement of this renderer, and it was 20-40x wrong — which is gotcha 232. What the
counter buys is a narrowed `other`: what remains is the two shader-map lookups, the
`std::map<PipelineKey>` lookup, the fetch-constant decode loop and the vertex-attribute
loop. A per-draw census of sampler slots and vertex attributes is the next cheap step.

### Method, and two corrections the operator forced

* **A periodic instrument read on a human's cue is not synchronised with the human.** I
  was reading the last 10-second profile window whenever a message arrived — up to 10 s
  stale, averaged over wherever they were during it. My "first crowd / second area"
  labels were mine, not measurements, and the operator corrected them. Ask for the area
  name, or run `CZ_FILE_TRACE=1` so the log names the zone itself.
* **Three claims made and retracted inside one hour**, each by the next measurement:
  "crowds hit the 31 fps cap" (my headless ceiling — real crowds are 7,000-9,000 draws
  where the CPU alone exceeds it); "the gas station is the worst case because it has 1.7x
  the draws" (matched-draw comparison says `other`, not draws); "the gas station is the
  worst case" (revisiting says it is an ordinary crowd). Each was stated with evidence
  and each was wrong at the next scale. Gotcha 222, three times, in the session that
  cited it.

---

## 6at. THE STREAM CACHE IS 94% HITS AND STILL COPIES 74 MB A FRAME — §1b's ambiguity
## is resolved, and it resolves the other way

`streams` is the largest draw-path term in a real crowd (§6as: 12.3-14.3% of a frame,
against 6.8% in the headless recipe). `docs/perf-cpu-plan.md` §1b called it **"genuinely
ambiguous"** and refused to guess, because two opposite readings fit the same millisecond
count and need opposite fixes:

* nearly all HITS — then 0.72 µs/draw is the *lookup*, and the fix is a cheaper key;
* mostly MISSES — then it is real copying, and the fix is a different cache **lifetime**.

The plan's instruction was to count hits, misses and bytes before writing anything.
`CZ_VK_STREAM_CENSUS=1|2` counts them.

### What the code already settled, and nobody had read

`ProfScope(&g_prof.streams)` wraps **only the `CopySwapped`** — not the map lookup, not
the `ArenaAlloc`. So a cache HIT costs the `streams` column exactly nothing, and its
lookup is charged to `other`. The whole `streams` column is copying, and half the
ambiguity was answerable by reading nine lines. Recorded because the plan said "no
reading of the code can tell you which you have", and on this half it could.

### The numbers, outdoor crowd recipe, ~6,400 draws in a ~50 ms frame

| | |
|---|---|
| lookups per frame | ~33,000 (≈5.2 per draw) |
| hit rate WITHIN a frame | **93.6-94.0%** |
| misses per frame | ~2,000, average 37 KB each |
| **bytes copied per frame** | **74-77 MB** |
| `streams` | 11.3-11.7% of the frame = **5.6-5.9 ms** |
| **of the copied bytes, share repeating LAST frame's key** | **95-97%** |

Split by what the stream is: **vertex bindings 61-63 MB, shader-side dependent fetches
(`XeVfetchDep`) 11 MB, index buffers 1.8 MB.** The split matters because the three have
different answers to "could this be cached across frames", and one total cannot be read
that way.

The census discriminates strongly across eras of a single run — hit rate 50% → 71% → 80%
→ 82% → 94%, cross-frame repeat 6% → 18% → 65% → 98% — so it is demonstrably capable of
reporting something other than what it reports in a crowd (gotcha 30).

**So: it is real copying, and it is almost entirely the SAME buffer as last frame.** The
cache is doing its job perfectly within a frame and is thrown away at every frame
boundary. 74 MB a frame of dword-swapped copying into HOST_COHERENT memory, ~13 GB/s,
95% of which reproduces bytes that are already there.

### The content check, and the control that makes it believable

"The key repeats" is not "the content repeats" — a persistent cache keyed on
(address, size, endian) is *wrong* if the guest rewrites the buffer in place. Level 2
hashes every stream's guest bytes and compares against last frame's hash for the same
key. It read **100.0% unchanged**, in every window, in every era.

**A number that only ever reads 100.0% has not been shown capable of reading anything
else**, and a real fact about this title's geometry is indistinguishable from a
comparison that cannot fail. `CZ_VK_STREAM_CENSUS_POISON=1` is the control: it salts the
hash with the frame number, so identical bytes must hash differently and the line must
read 0.0%. On the same binary:

```
poison off   CONTENT UNCHANGED: 75492 of 75492 repeated keys (100.0%)
poison on    CONTENT UNCHANGED:     0 of 96048 repeated keys (  0.0%)
```

The control also exposed what the rounding was hiding. Over two full runs,
**164 of 10,154,820 repeated keys DID change content — 0.0016%** — and the changing set
recurs as a fixed 26. So in-place rewriting is real, rare, and **not zero**: a persistent
cache must **invalidate**, not assume. Without the poison arm the honest reading would
have been "100%, safe to cache blindly", which is wrong.

### What this makes the fix

A cross-frame stream cache is worth **~5.5 ms of a ~50 ms crowd frame (≈11%)** — the same
order as the whole of part 20's instrumentation removal. It needs three things the
per-frame cache does not:

1. **Storage that outlives the frame.** The arena is a bump allocator reset at every swap;
   persistent streams need their own allocation and an eviction policy.
2. **Invalidation.** 0.0016% of repeats change in place, and a stale vertex buffer draws
   the wrong mesh. Hashing to detect it costs what the copy costs, so the candidate is
   guest-page write tracking (`mprotect` on the guest map plus a `SIGSEGV` handler that
   coexists with `cpu/crash_report.cpp`'s). Static geometry then faults never; dynamic
   buffers fault once a page a frame.
3. **A counter for both**, since a cache that silently serves stale data looks exactly
   like a rendering bug twenty frames later.

That is a session's work and it should not be started inside another item.

## 6au. The arena growth ran inside `DoDraw`, and that is why a growth cost 29.8% of
## `other`

`BeginFrame()` is called from inside `DoDraw`, and the arena growth lived at the top of
it — so a growth charged its `vkDeviceWaitIdle`, its buffer allocation and its map to the
draw path's `other` column. §6as measured one such frame at 29.8%, the largest single
spike of the operator session, and the mechanism was the **call graph**, not anything
about drawing. Moved to the end of `DoSwapImpl`, after `SubmitAndWait`.

**This is a measurement fix before it is a performance one**, and saying which matters:
the work still happens, it is charged where it belongs. The one real saving is the
`vkDeviceWaitIdle`, which at the new site follows a fence wait that has already idled the
device.

The old comment claimed *"here is safe and nowhere else is: the command buffer has just
been reset, which is only legal once its previous submission has completed"*. The end of
`DoSwapImpl` meets that same condition more directly — the fence has been **waited on**,
so the submission is not merely complete but observed to be.

Verified against the stated prediction (the growth still fires, and the black-frame count
per growth is unchanged at one):

```
pre-move   exhausted frame 1817, black frame 1818, 6 black of 13,410
post-move  exhausted frame 1258, black frame 1259, 7 black of  5,387
```

In each, the black frame **is** the exhausting frame — the frame counter increments at
the top of `DoSwapImpl`, so the message and the stats line are one apart by construction.
The frame that overruns is lost either way; only the frame after it is rescued, and it
already was. The remaining black frames are the same four early-boot frames in both runs,
at identical draw counts. Closes the last live piece of open-items 1c.

## 6av. THE CROSS-FRAME STREAM STORE — and the invalidation question turned out to be
## the whole of the work

open-items 0a, built on §6at's measurement. The short version: the per-frame stream cache
was 94% hits and was thrown away at every swap, so a crowd frame copied 61-77 MB it had
copied last frame to the same guest address. This gives the cache a lifetime.

### First, the measurement was re-run, because it was one afternoon's

The hand-off said so and it was worth the ten minutes (gotchas 50/51/86). On the part-22
binary, outdoor crowd recipe, 6,872 draws at peak: **93.5-94.1% hits within a frame,
61-66 MB copied a frame, 93.5-94.7% of those bytes repeating last frame's key.** Part 21's
shares reproduce exactly; its 74-77 MB does not, because level-2 hashing slows the frame
and the fixed-interval recipe drifts with it. The shares are the claim; the MB is a
property of where that particular run got to.

### The census was made to name the changing streams, and that decided the design

Part 21 knew 164 of 10,154,820 repeated keys really changed content and called the
changing set "a recurring ~26". It did not know **which**, and that is the question the
design hinges on: a named set is an exclusion rule and costs nothing, a scattered set is
guest-page write tracking — `mprotect` plus a `SIGSEGV` handler sharing the process with
`cpu/crash_report.cpp`'s, which uses `SA_NODEFER`, plus a hazard nobody had noticed
(`kernel/vfs.cpp` reads file data into guest memory with `fread`, and a `read(2)` into a
`PROT_READ` page returns **EFAULT** rather than faulting, so a level reload into a
protected page would fail silently instead of trapping).

Keeping the identity cost about fifteen lines. The answer:

```
streams REWRITTEN IN PLACE: 30 distinct keys, 112 occurrences,
                            guest range A0162328..A03C5AE8
streams   va=A0162448 size=80 endian=2 vertex  x5  frames 1654..2288
...
```

**Every one is exactly 80 bytes, endian 2, a declared vertex binding**, in two narrow
runs. Not one index buffer, not one dependent fetch, and nothing larger than 80 bytes.
`mprotect` is not needed and was not built.

### The guard: bounded cost, and it still CHECKS the streams that have never changed

`StreamGuard` hashes a stream in full up to 512 bytes and, above that, at eight spread
64-byte windows including the first and the last. The threshold puts the entire observed
rewritten population on the exact branch with a 6x margin.

The sampled branch exists because **"we have never seen a big one change" is a zero, and a
zero is a detection failure until something could have detected it** (gotcha 3). A size
cutoff that simply declined to check large streams would have been cheaper and would have
had no way to ever discover it was wrong.

Cost: **under 0.8 MB a frame read, against 61-77 MB of copying avoided** — about 1%.

### What it does

Gameplay recipe, ~1,900 first-touch streams a frame:

```
store 1878 first-touch/frame: 97.3% served across the frame boundary,
      68.73 MB/frame NOT copied | fills 8 stale 5985 overflow 0
      | guard read 0.78 MB/frame
store 23005 entries, 141 MB of 256 MB used, 0 flushes this window
streams 28360 lookups/frame: 93.4% hit | copied 0.23 MB/frame
```

**Copied bytes fall from 61-66 MB a frame to 0.23**, and `fills` in the steady state is
eight — the store is warm and stays warm.

### THE STALE COUNT IS THE FINDING, and it says part 21's number was too small

`stale 5985` per fifteen-second window is roughly **20 streams a frame** served from an
address the store already held whose contents had changed. That is two orders of magnitude
more than the 30-per-run the census reports, and the gap is structural rather than a
disagreement: **the census compares against LAST FRAME and the store compares against the
LAST COPY.** An address the guest reuses for a different mesh after a gap of frames is
invisible to the first and is exactly what the second exists to catch. Part 21's 0.0016%
was an honest measurement of a smaller question than the one a persistent cache asks.

Every one of those 20 a frame is a wrong mesh in a cache that assumed instead of checking.
The hand-off's "invalidation is not optional" was right for a stronger reason than it knew.

### The correctness counter, and the control that makes it worth reading

`CZ_VK_STREAM_CENSUS=2` now also computes the full hash and counts every real content
change the guard let through as a hit — a stale buffer handed to a draw, the only defect
this design can cause, and one that would otherwise appear as a rendering bug frames later
in the class this project has spent whole parts chasing.

It reads **0**. And, because a check that only ever passes has not been shown capable of
failing (gotcha 234), under `CZ_VK_STREAM_CENSUS_POISON=1` — where the salted full hash
calls every repeat a change while the unsalted guard correctly does not — the same line
reads **240,652 of 240,652**. The counter fires when it should.

### Storage: a second buffer, not a region of the arena

The arena's exhaustion path is load-bearing — it is what turned a fixed 128 MB into six
parts of "view-dependent whole-frame black" (§6ap) — and carving a persistent region out
of its bottom would put a moving floor under that machinery and under
`GrowArenaIfNeeded`'s buffer swap. A separate buffer leaves every line of that alone. The
price is that `UploadStream` must say which buffer it used, which is `StreamLoc` and three
call sites; the synthesised rectangle stream still goes in the per-frame arena, because it
is built from one draw's corner indices and is not shared.

Maintenance runs where the arena's growth runs — the end of `DoSwapImpl`, after the fence
has been **waited on**. That is load-bearing here in a way it is not for the arena: the
store's offsets are recorded into command buffers, so reusing its memory a moment early
hands an in-flight draw somebody else's vertices.

**Eviction is a whole drop, on purpose and with a counter.** An LRU with compaction has to
MOVE live streams to close its gaps, which is copying, which is the cost this removes. A
drop pays one frame at the old cost and runs warm again. `flushes` is on the profile line
so the evidence for building the harder thing would be visible; in these runs it is zero
after the one growth from 128 to 256 MB.

### THE FRAME-TIME RESULT, and why the headline number is small and the finding is not

Three runs an arm, alternated a/b/a/b/a/b, 620 s each, arm A the store and arm B
`CZ_VK_NO_PERSIST_STREAMS=1`. The null comparison first, within arm A: **+1.3%** in the
6000-6999 bin. Then the arms, `tools/frame_perf_bins.py`, which reports **means**:

```
per-run mean ms of frames with >= 6000 draws:
  A: 47.48 47.67 48.22      B: 48.56 48.35 48.63
  6000-6999   A 47.63 ms    B 48.44 ms    +1.7%
```

Every A run beats every B run with no overlap, so the direction is not in doubt — but
**+1.7% against a +1.3% null is not what removing 5.5 ms of measured copying should buy**,
and publishing it without explaining the gap would have been publishing a number I could
not account for.

**The explanation is that this title's frame time is a PACING FLOOR, and a mean hides it.**
Binned finer and read as medians, with the share of frames sitting within 1 ms of a 16 ms
multiple:

| draws | A median | A on a vblank | B median | B on a vblank |
|---|---|---|---|---|
| 3000-3499 | 32 ms | 93% | 34 ms | 42% |
| **3500-3999** | **32 ms** | **97%** | **44 ms** | **10%** |
| 5500-5999 | 48 ms | 80% | 48 ms | 67% |
| 6000-6999 | 48 ms | 80% | 48 ms | 63% |

At **~3,700 draws the store takes the frame from 44 ms to 32 ms — a 27% reduction, ~23 fps
to 31** — and the giveaway is the pinning column: arm B is free-running at 10% pinned,
arm A is *pacing-limited* at 97%. The workload stops being ours. At ~6,500 draws both arms
are already mostly parked on the 48 ms three-vblank floor, the saving is ~5 ms, and 5 ms
does not reach the next floor 16 ms away — so it appears as idle time and the mean barely
moves.

This is `perf-cpu-plan.md` item 0's rule ("do not optimise anything measured at ~1,930
draws, the title's own two-vblank pacing makes a CPU saving measure as exactly zero")
turning out to apply at **three** vblanks as well, and the honest statement of the win is
therefore conditional: **the store removes the copying everywhere and converts it to frame
rate only in the band where the frame is above one vblank floor and within reach of the
next.**

### Where the 5.5 ms went, measured rather than argued

The profiler settles it. Matched draw counts, arm A at 5,670 draws and arm B at 5,727:

| | store ON (46.5 ms) | store OFF (48.2 ms) |
|---|---|---|
| `streams` | **0.0% — 0.00 ms** | 11.1% — 5.35 ms |
| `record` | 9.1% — 4.23 ms | 4.9% — 2.36 ms |
| `textures` | 6.1% — 2.84 | 5.7% — 2.75 |
| `other` | 4.5% — 2.09 | 4.3% — 2.07 |
| `constants` | 2.3% — 1.07 | 2.2% — 1.06 |
| **draw total** | **10.3 ms** | **13.6 ms** |
| `submit` (GPU) | 17.5 | 19.0 |
| `outside` (idle) | **18.4** | **15.3** |

`streams` goes to **zero** — the predicted saving is fully realised — and the frame does
not get 5.5 ms shorter because 3.1 ms of it reappears in `outside`, which is the CPU
waiting on the pacing floor.

**`record` nearly doubles, and that is not noise — it is the guard, charged to the wrong
column.** `record`'s scope opens partway down `DoDraw` and encloses the `UploadStream`
calls; the scopes are exclusive as of part 20, and `ProfScope(streams)` wraps only the
`CopySwapped`. So the guard hash, which runs inside `UploadStream` and outside the
`streams` scope, lands in `record`. Net CPU saving is 3.3 ms, not 5.5.

Worth stating as a rule, because it is the same shape as §6at's own lesson: **when you
remove work from a timed scope, check where the replacement work is charged.** Reading
`streams: 0.0%` alone would have claimed 5.5 ms and been wrong by 40%.

### Gates

`--smoke` OK; A5 exit 0 with 3 permutation windows and 0 real; `truncated=0`;
`no translated shader` = 0; both PM4 capture oracles clean. The picture against capture E2
at frame 576 is **+0.9590 identity with the store on and +0.9596 with it off**, against
the project's standing +0.9597, and the two arms' own frames correlate +0.9998, +0.9934,
+0.9929 and +0.9921 at matched indices.

---

## 6aw. THE CPU AND THE GPU NOW OVERLAP — a ring of frame slots, and the swapchain the
## plan insisted on turned out not to be needed

`docs/perf-cpu-plan.md`'s largest item, and the one part 22's hand-off ranked first. A
crowd frame was ~27.7 ms of CPU followed by ~16.5 ms of GPU **strictly in series**,
because `SubmitAndWait` submitted a command buffer and blocked on its fence. The card sat
idle 68% of every frame and the driver correctly governed it to a mid clock (§6ar,
gotcha 231). `CZ_VK_NO_SUBMIT=1` had measured the ceiling on removing that at ~1.45x
without building it.

`CZ_VK_FRAMES_IN_FLIGHT=N`, default 2, and **`=1` is the pre-part-23 renderer exactly**,
so one binary is both arms.

### What actually had to be duplicated, which is less than the plan assumed

Only what the CPU writes and the GPU reads, or the reverse: the command buffer, the bump
arena, and the buffer the presented image is read back into. Everything else — the EDRAM
stand-in, the resolve snapshots, the textures — is touched only by the device, and a
single queue executes submissions in order, so the barriers already in the recorder cover
every cross-frame GPU hazard. The bindless heap needed nothing either: it already carries
`UPDATE_AFTER_BIND | PARTIALLY_BOUND` and slots are only ever handed out fresh.

**The arena is cut into N regions of ONE buffer rather than becoming N buffers.** That
keeps `GrowArenaIfNeeded`'s buffer swap, the exhaustion path and every device address a
draw records exactly as they were; the only lines that change are where the cursor starts
and stops. The buffer is allocated N times larger at startup so a frame's own capacity —
the number that decided the whole-frame black (§6ap) — is identical between the arms.

**The kickoff's claim that this needs a real `VkSwapchainKHR` was wrong.** A per-slot
readback buffer presented one frame later keeps phase 3's renderer/window separation and
costs one frame of latency, and nothing else. That claim had been carried in the
`SubmitAndWait` comment since §6ar; it is retracted in place there.

### The two hazards part 22 flagged in advance, both at the line that breaks

* **The stream store's stale path overwrote a slot in place**, safe only while the submit
  was synchronous. It now ping-pongs to a twin slot allocated lazily on the first rewrite.
  Two slots is exactly enough: when frame N+1 is recording, frame N-1 has provably retired.
  When no twin can be allocated the entry is DROPPED to the per-frame arena and counted
  (`evicted, no twin`) rather than overwritten — trading a copy for correctness, never the
  reverse. It read 0 in every run.
* **The present-side instruments labelled `presentPixels` with `R->frame` and the
  fingerprints**, which after this describe the frame being RECORDED. Each slot carries its
  own metadata, captured at submit and read at present, so `frame_compare.py` still aligns
  two runs by the right frame. Three instruments read the LIVE resolve chain next to the
  presented pixels and cannot be fixed that way — `CZ_VK_SNAP_ON_BLACK`,
  `CZ_VK_SNAP_ON_DARK`, `CZ_VK_FRAME_STATS_SURFACE` — so they force N=1 and say so.

### The result

The fence wait is the counter that says it engaged (gotcha 151), and it collapsed:

| | N=1 (old renderer) | N=2 |
|---|---|---|
| `submit` | **31.5%** `[call 0.2 gpu 31.4]` | **0.2%** `[call 0.2 gpu 0.0]` |
| store `evicted, no twin` | 0 | 0 |
| store flushes | 0 | 0 |

The CPU no longer blocks on the GPU at all. The picture is unchanged: capture E2 reads
**+0.9595 identity at N=1 and +0.9594 at N=2** over a 120 s boot each, against part 22's
+0.9596/+0.9590.

**The two profile windows above are NOT a frame-time comparison** — they sat at 6,433 and
4,968 draws a frame, because the synthetic-input recipe drifts (gotcha 75). The binned
three-runs-an-arm A/B is **still owed** and is part 24's first chore; it was stopped
deliberately when an operator report arrived, because rebuilding the renderer underneath a
running timing sweep contaminates it.

**What an operator saw, which is better evidence than the bins would have been:** "the game
is way more stable than before, staying almost always at around 30 fps", with 31-32 fps in
every screenshot's title bar. That is exactly the statistic gotcha 237 says to quote — the
share of frames pinned to a vblank multiple — arriving as a report rather than a number.

### A method note, and a mistake worth keeping

`pkill -f cz_runtime` killed its own parent shell before the second call ran, so the run
being stopped kept going for ten more minutes alongside the next experiment. Kill by PID
when the pattern can match the killer.


## 6ax. AN OPERATOR REPORTED WRONG TEXTURES EVERYWHERE, AND THE ANSWER WAS A CENSUS OF THE
## SHADER BANK RATHER THAN ANY OF THE THREE THEORIES THAT PRECEDED IT

The report: "almost all the textures in the game are wrong and got the texture of something
else, like a building getting the repeated texture of a moose head item", then a wrong
floor (a different atlas each run), an iridescent "unicorn colour" filing cabinet, a wrong
dumpster colour and a blank white wall.

**The finding is `docs/open-items.md` item 00: 91 of 395 shaders sample a CUBE MAP and
every one of them reads descriptor index 0 — the 1x1 white dummy — on every draw, and has
since phase 5.** `bindTextures` writes only the `Texture2D` index array; `kSharedTex3D`,
`kSharedTexCube` and `kSharedTex1D` appear at their definitions and nowhere else; and it
cannot do better because `t.dimension` is hardcoded to 2D with a comment saying the
dimension comes from the shader, while the shader metadata is a flat list of slot numbers
with no dimension in it. Cube maps here are the environment/reflection maps, so every
reflective surface multiplies its specular by pure white.

### The three theories that came first, and why recording their deaths is the point

1. **The texture cache serves a previous occupant's image**, because it is keyed on the
   fetch constant (a descriptor) and never invalidated, while this title streams textures
   by distance into recycled heap addresses. Plausible enough to build an instrument for —
   and **refuted by the operator's own log at 0.00%** (1,968 stale of 139,775,032 hits,
   with the wrong textures on screen throughout). The full table is in open-items 00b.
2. **The colour-grading LUT is a `Texture3D` and therefore also unbound.** Killed by the
   same census: **zero** modules reference set 1.
3. **`if (constIdx >= 16) continue;` silently drops fetch slots 16-31.** Real, uncountered,
   and it **never fires**: 0 of 1,076 declared fetch slots in the bank are >= 16.

Three hypotheses, three refutations, and the survivor was found by parsing 395 SPIR-V
modules for their `OpDecorate ... DescriptorSet` words — a census that needed no run, no
operator and about a minute. The lesson is the project's own first evidence rule arriving
in a new place: **the shader bank is a population that can be counted, and counting it
beat three rounds of reasoning about pictures.**

### What the pictures were actually good for

One thing no counter here produced: the giant translucent overlay had a **hard vertical
seam at the exact middle of the frame** — this title's tiles are left/right 640-wide halves
— which is a tiling or predication question and not a texture one.

**And one thing the pictures were NOT good for, recorded because it was written up as
evidence and then corrected by the operator.** I read a large flat cream wall in one shot
as "the signature of the 1x1 white dummy" and used it to corroborate the binding-path
theory. **The operator says that wall is a normal texture rendering correctly.** It is a
plaster wall; it looks like that. The corroboration is withdrawn — the cube-map finding
stands entirely on the shader-bank census and never needed it.

The general lesson is the sharper half of gotcha 190. An operator's screenshot is the only
evidence channel for "does it look right", and this project keeps learning that what they
SEE names a subsystem — but **only the operator can say which parts of their own screen
are wrong.** Reading a defect out of an unremarkable region of someone else's screenshot
is inventing evidence, and it is easy to do because a wrong-looking frame makes every
surface in it suspect. Ask; do not infer. The same shot's seam at x=640 was a real
observation precisely because it is a discontinuity no correct render can have.

## 6ay. CUBE MAPS ARE BOUND — and the whole of it was found and checked WITHOUT a run,
## except for the one field that had to be measured

§6ax established the defect: 91 of 395 shaders sample a cube map, `bindTextures` wrote
only the `Texture2D` descriptor-index array, and every cube fetch therefore read index 0 —
the 1x1 white dummy — on every draw since phase 5. Part 25 built the fix. The interesting
part is not the code; it is that almost every step had an independent oracle, and the one
step that did not was the one that turned out to be wrong.

### The dimension has TWO independent derivations, so it is a gate rather than a claim

The shader's fetch instruction carries the dimension in word 2, bits 14..15 — XenosRecomp's
own `TextureFetchInstruction::dimension`, the field the translator switches on when it
picks `tfetch2D` versus `tfetchCube`. `tools/synth_shader_container.py` now writes it into
the sidecar as `tfetchDims`, positionally against the sorted `tfetchConsts`, **per slot**:
one pixel shader here samples slot 3 as a cube and slots 0 and 2 as 2D in three consecutive
instructions, so a per-module flag would be wrong for two thirds of it.

That same fact is derivable a second time, through a path containing no code of ours: DXC's
`OpDecorate <id> DescriptorSet n` words in the translated SPIR-V, because the generated
HLSL binds `Texture2D[]` to space0 and `TextureCube[]` to space2. `tools/shader_dim_census.py`
compares the two over the whole cache and exits 1 on any disagreement.

| | modules | declared fetch slots |
|---|---|---|
| 2D | 298 | 973 |
| cube | **92** | **92** |
| 1D, 3D | 0 | 0 |

The two agree on every shader. That also reconfirms §6ax's SPIR-V census from the other
end — sets 1 and 4 are unused, so the colour-grading LUT is still not a `Texture3D` — and
it was **shown capable of failing**: moving the parse one bit to 15..16 and rebuilding a
15-shader subset makes the census flag all 15 (gotcha 30).

Eleven cache entries predate this and have no microcode left on disk, so their sidecars
carry no `tfetchDims`; the runtime treats them as 2D and counts it, and the census names
them. Exactly one of the eleven samples a cube map. **/tmp is a tmpfs here, which is why
those dumps are gone** — the persistent copy is `~/DR2CZ-troubleshooting/ucode-dumps`.

### The one field with no oracle was the one my recollection got wrong

The renderer also needs the *guest's* view of the dimension, because a cube map is six
faces and reading one is reading a sixth of it. `DecodeTextureFetch` had
`t.dimension = 1;` with a comment saying the dimension is taken from the shader, and the
shader metadata had no dimension in it — so it was taken from nowhere.

I could have written down a bit position from memory. It would have been **bits 7..8, and
it would have been wrong**, and a wrong dimension does not fail: it produces a plausible
wrong image. So `CZ_VK_DIM_CENSUS=1` measures it instead, using the shader's answer as the
partition. For each class it accumulates the AND and the OR of all six fetch-constant
dwords; a bit set in every fetch of a class has AND=1, one clear in every fetch has OR=0,
and the field must live where the two classes' patterns disagree. Over **842,556 2D and
47,574 cube fetches**, exactly two dwords separate them:

* **dword5 bits 9..10** — reads **1** for every 2D fetch and **3** for every cube one,
  which is the `TextureDimension` encoding itself;
* **dword2 bits 26..31** — reads **5** for every cube fetch and **0** for every 2D one.

The second was a **prediction stated before the run** (Xenia's layout says those bits are
the stack depth, stored minus one, so a cube must read 5 = six faces). It could have
refuted the whole reading and it did not. Two dwords, one measured and one predicted,
agreeing — that is what makes this a measurement rather than a fit.

**Both sources are now cross-checked on every fetch,** and they do disagree: a constant
saying 2D under a shader saying cube. Those are served the dummy and counted, because
reading six faces out of a surface the guest describes as one would build a cube from five
slabs of whatever follows it — and declining is exactly the picture those draws already got.

**The SHARE, however, is not yet known, and the first number this document carried was
misleading enough to be worth keeping as an example.** The boot-to-gameplay recipe gave
114 disagreements against 337,602 agreements — 0.03%, which reads as negligible. The same
binary on the deeper outdoor recipe declined **90,984**, and there was no total to divide
by, because nothing counted cube fetches at all. A denominator counter went in afterwards.
That is gotcha 242 for the third time in this project: **a statistic is fitted to the one
population the instrument happened to reach**, and "0.03%" was a fact about the safehouse.

### Two of the seven cube maps upload entirely BLACK, for two different reasons

The aggregate "uploaded entirely BLACK" counter sits around 250 in a long run, so a cube
joining it moves a number nobody would look at twice — and a black cube map is a whole
surface class losing its reflection, not one 16x16 icon. `CZ_VK_TEX_CENSUS` names them:

| address | extent | census row | reading |
|---|---|---|---|
| `06805000` | 64x64 `k_8_8_8_8` | `up 1 (zero 1) <- uploaded BLACK, guest memory STILL zero` | a **resolve destination**: the title renders this environment map itself, so guest memory there is nothing and always was |
| `01330000` | 4x4 `k_8_8_8_8` | `up 1 (zero 1) <- uploaded BLACK, guest memory is NON-ZERO NOW` | the texture arrived AFTER our one and only upload; the fetch-constant cache froze it black (§6aa's shape) |

The first is now **declined to the dummy** rather than uploaded. Guest memory at a resolve
destination is known not to hold the texture — that is the Snapshot doctrine, gotcha 113 —
so uploading the zeros would present "I had nothing" as a black reflection, which is a lie
dressed as data; and the dummy is also exactly the picture that surface had before part 25,
so declining cannot make it worse. We cannot serve the snapshot either, because it is a 2D
image in set 0 whose slot number is meaningless in set 2. **The real fix is a cube snapshot
path — six resolves into six layers — and it is open item 00's remaining half.**
`CZ_VK_CUBE_FROM_GUEST=1` keeps the zeros, as the arm for asking an operator which of white
and black is closer for that surface, which is not a question to settle by argument.

### What the six faces cost, and the one thing that is a model rather than a quotation

The face stride is `faceBytes` — one face's tiled footprint, the pitch rounded to 32 units
by the rows rounded to 32 — which is the 2D path's own arithmetic applied six times. **That
is the one part of this with no oracle behind it**, and it is called out in the code so a
sheared or offset sky is traced to that line rather than to the sampler. The uploads are
small and plausible: 128x128 DXT1 faces at 8,192 bytes each, one 64x64 `k_8_8_8_8` at
16,384, a 32x32 and a 4x4 padded up to the 32x32-unit tile minimum.

### A latent barrier defect, found by reading rather than by symptom

`Barrier` had `layerCount = 1` hardcoded in its subresource range. That was correct for
every image this renderer had ever created and became silently wrong the moment one had six
layers: faces 1..5 would never have left `TRANSFER_DST`, and the most likely presentation —
one correct face and five wrong — reads as a decode bug, not a barrier one. Fixed by giving
`Image` its own `layers` and using it.

**And it was never a future problem — it was already live, in the DUMMY.** `R->dummyCube`
is a six-layer image and has been since phase 5, so five of its faces were written and
sampled in `VK_IMAGE_LAYOUT_UNDEFINED` for the whole of it. That means §6ax's "every cube
fetch reliably reads a defined 1x1 white texel" is **too generous a description of the old
behaviour**: only the +X face was defined, and the other five were undefined by
specification, however white they may have looked on this driver. The defect was therefore
slightly worse than it was written up as, and — more to the point — the bug was sitting in
the renderer the entire time with no image large enough to expose it.

**And note what did NOT catch it: the validation layer is not installed on this machine.**
Every log in this session says `VK_LAYER_KHRONOS_validation is NOT INSTALLED`, so a grep for
`VUID` over any of them returns zero for the reason gotcha 25 exists. Installing it
(`sudo dnf install vulkan-validation-layers`) is the cheapest outstanding safety net this
renderer has.

### THE PICTURE A/B CAME BACK PIXEL-IDENTICAL, and that is a result about the HARNESS

Four runs, two arms, same binary: cube maps bound versus `CZ_VK_NO_CUBE=1`, on the outdoor
recipe with `CZ_VK_FRAME_DUMP`. 301 frames dumped per long arm, arm A peaking at 8,823
draws and arm B at 9,223.

**The admissibility rule did almost all the work here, and it is worth copying.** The
naive comparison — every matched present index — reports a mean |RGB| difference of 13-34
in the gameplay era and 0.000 in the boot era, which looks like a large effect. It is not:
the two runs DRIFT, so at a matched index they are in different places, and this project's
own A/B rule says two arms are comparable only if they are two states of one renderer
producing the SAME draw set. The frame-stats file carries a `drawFingerprint` and a
`cameraFingerprint` per frame, so the rule is enforceable rather than aspirational:

| matched dumped indices | 301 |
|---|---|
| ...with the same **camera** fingerprint | 70 |
| ...with the same camera **and the same draw set** | **44** |
| median mean \|RGB\| over those 44 | **0.000 — byte-identical** |

And the 44 admissible frames top out at **1,799 draws**. Every frame above 4,000 draws in
either run has a different draw fingerprint, so **the outdoor era — where an environment
map matters most — contributes exactly zero admissible pairs.** Of the whole run, 3,094 of
19,279 frames share a draw fingerprint across the arms and **none of them is above 4,000
draws.**

So the honest reading is not "the cube maps changed nothing". It is: **inside the
safehouse, at ≤1,799 draws, binding real cube maps changes no pixel; and the harness cannot
currently produce an admissible outdoor comparison at all.** That is gotcha 242's shape for
the third time — a statistic about the one population the instrument could reach.

**`tools/frame_matched_diff.py` printed the opposite headline, and reading its per-pair
lines is what saved it.** Its aggregate over four runs says `late: cross 4.02 vs noise
floor 1.18 -> ARMS DIFFER (3.41x the floor)`. The pairs underneath say otherwise:

| pair | late median \|RGB\| | n |
|---|---|---|
| within-A (`cubeA` vs `cubeA2`) | 1.18 | 135 |
| within-B (`cubeB` vs `cubeB2`) | 0.77 | 136 |
| cross `cubeA2`–`cubeB` | **0.61** | 135 |
| cross `cubeA2`–`cubeB2` | **0.67** | 135 |
| cross `cubeA`–`cubeB2` | **1.00** | 136 |
| cross `cubeA`–`cubeB` | **13.00** | **292** |

Three of the four cross pairs sit AT or BELOW both within-arm floors — indistinguishable.
The fourth is the only pair of two 620 s runs, which drift furthest from each other, and it
carries more than twice the sample count of any other pair, so the pooled median inherits
it. **A median over pooled pairs of unequal n is not a summary of those pairs**, and the
tool's own detailed lines are the check on its verdict. Quote the per-pair table.

The null is still surprising, because cube fetches are not rare: arm B's control counter
read **3,521,910 forced-back cube fetches against 53,882,535 draws, i.e. 6.5% of every
draw in the run**. Two readings fit, and no amount of looking separates them — the cube
sample never reaches the output, or it reaches it and is indistinguishable from white. So
the next instrument is a POSITIVE CONTROL rather than another picture: `CZ_VK_CUBE_POISON=1`
makes the cube DUMMY opaque magenta, which is what the pre-part-25 renderer's cube fetches
read. A poisoned run that is still identical means the cube sample is discarded downstream
and the item is mis-scoped; a frame full of magenta means the path is live and the null is
a statement about the CONTENT of this title's cube maps.

Two counters went in at the same time for the same reason — `draw: bound a REAL cube map`
and `draw: cube fetch got the dummy` — because there was no way to tell a frame in which
every cube bound correctly from one in which no draw asked for a cube (gotcha 151).

### The poison control is POSITIVE, and it took three attempts to build one that could say so

The final form is one variable: **magenta cube dummy versus white cube dummy**, same
binary, same recipe, `CZ_VK_NO_CUBE=1` on both sides so every cube fetch reads the dummy.

| frames compared | 110 |
|---|---|
| frames that differ at all | **80** |
| worst frame | mean \|RGB\| **53.7**, **72.1% of pixels changed**, max delta 255 |
| draws that asked for a cube, this recipe | **747,097** |

**So the cube sample is not discarded anywhere downstream — it reaches the presented image
hard.** That kills the reading that item 00 is mis-scoped, and it makes the byte-identical
A/B above harder rather than easier to explain: if repainting the dummy changes 72% of a
frame, then replacing that dummy with a real cube map should change something too.

**Three things had to be fixed before the control could report anything at all**, and each
of them would have produced a confident wrong answer:

1. The two cube reach counters sat AFTER the `CZ_VK_NO_CUBE` forcing, so on the arm the
   control runs under they could not fire. `draw: shader asked for a CUBE map` is now
   counted from the shader's own answer before any arm can rewrite it.
2. The arm did not announce itself. A poisoned run showing no magenta was indistinguishable
   from a run where the flag never took (gotcha 151).
3. The dummy upload wrote **four bytes for a six-layer copy**, so faces 1..5 of every 1x1
   dummy came from whatever the staging buffer last held. Invisible while the only
   multi-layer image was a dummy nobody could see; it would have poisoned one face out of
   six and left five reading garbage.

**And the first readout was the wrong statistic.** I measured "how many pixels are
magenta", requiring saturated R and B — but a cube sample arrives multiplied by a specular
term, so it TINTS rather than saturating. That detector read 0.24% and looked like a clean
negative. The right question was never "is it magenta"; it is "did the frame change at
all", which is 80 of 110. A positive control has to be read with a statistic that can see
the effect it is controlling for (gotcha 248).

### The split that explains it: 55% of cube fetches are ONE cube map we decline

The counters on the DEFAULT arm, same safehouse recipe:

| | |
|---|---|
| draws that asked for a cube map | **746,355** |
| ...bound a **REAL** cube map | **336,044 (45.0%)** |
| ...got the dummy | **410,311 (55.0%)** |
| of which: the **resolve-destination** decline (`06805000`) | **409,911** |
| of which: the shader/constant disagreement | 400 |
| distinct cube maps uploaded | 6 |

So the single dynamically-rendered environment map at `06805000` accounts for **99.9% of
every dummy a cube fetch still receives**, and for more than half of all cube sampling in
the game's opening hour. **That makes the cube snapshot path — six resolves into six
layers — not a loose end but the larger half of this item by volume.** Binding the other
45% is real, and it is what the operator's verdict is about; the remaining 55% cannot be
fixed by any amount of decode work, because the pixels are not in guest memory to read.

### The four-run picture A/B is SUPERSEDED and must not be quoted for the shipped renderer

It was run before two behaviour changes landed — `06805000` is declined now, where that
binary uploaded it black — so it describes a renderer that no longer exists. Its method
survives (the fingerprint admissibility filter, and the per-pair reading of
`frame_matched_diff.py`); its numbers do not. The same-recipe replacement is default arm
versus `CZ_VK_NO_CUBE=1` on the current binary.

### THE FINAL ANSWER, AND A MISTAKE I MADE TWICE ON THE WAY TO IT

The unfiltered comparison on the current binary looked like a result: **82 of 109 frames
differ** between real cube maps and the white dummy, and the poison control differed on
80 of 110. Two counts that close look like two effects of the same size, and I said so.

**It is worthless, because two runs of the SAME configuration differ on 82 of 109 frames
too.** The whole count is drift (gotcha 75). What separates them is the MAGNITUDE against
that floor. All four configurations, one serial job, idle GPU, same recipe, same binary:

| comparison | frames differing | median | **p90** | p99 | max |
|---|---|---|---|---|---|
| **NULL** — default vs default | 81/111 | 0.0000 | **2.972** | 8.418 | 39.830 |
| real cubes vs white dummy | 81/111 | 0.0000 | **3.101** | 8.424 | 39.599 |
| real cubes vs white dummy, 2nd pairing | 81/111 | 0.0000 | **2.393** | 6.956 | 8.539 |
| **POSITIVE CONTROL** — magenta vs white dummy | 77/105 | 0.4085 | **37.877** | 53.567 | 57.592 |

**Read the p90 column. The instrument is not blind — it separates the positive control from
the null by 12.7x, cleanly, with no overlap.** And against that sensitivity, binding real
cube maps produces a p90 of 3.101 where the null is 2.972, with the second pairing at 2.393,
i.e. *below* the null. Identical frame counts (81/111) across all three non-control rows
are the same point again: the count is drift and nothing else.

**So the earlier framing of "the harness is blind" was wrong and is retracted here.** The
harness detects a 12.7x effect without difficulty. The correct statement is narrower and
more useful:

* **the cube path is live** — the control repaints up to 72% of a frame, so the sample
  reaches the presented image with force;
* **in the safehouse and prologue, replacing the white dummy with this title's real cube
  maps changes nothing measurable.** Not "too small to see against noise" — nothing, at a
  sensitivity that would have shown a twelfth of the control;
* **and more than half the potential effect is not in play at all**, because 55% of cube
  sampling is `06805000`, which is white in BOTH arms until the cube snapshot path exists.

Which leaves exactly two live explanations, and they are the next questions rather than
this part's conclusions: either this era's cube maps are themselves near-white (plausible —
they are small DXT1 environment maps and the safehouse is an interior), or the surfaces
that sample them are not on screen indoors. **Both predict that the outdoor era is where
the effect lives, and that is precisely the era no admissible comparison can reach.**

**The mistake, twice, in two different disguises.** First I read a positive control with a
statistic that could not see its own effect ("how much of the frame is magenta" — 0.24%,
against 80-of-110 by per-pixel diff; gotcha 248). Then I read an experiment with no null
comparison at all and quoted a frame COUNT as if it were an effect size — in the same
session in which I had just written gotcha 246 about shipping the denominator. **Both are
the same error: a number with nothing to divide by or compare against.** The habit that
fixes both is mechanical and cheap — run the null arm FIRST, in the same block, and quote
every effect as a multiple of it. `docs/measurement.md` already says this for frame time;
it now says it for pictures.

### What this item still needs, in order

1. **The operator's verdict.** Reflective surfaces, same spot, `CZ_VK_NO_CUBE=1` versus
   default. It is the only channel that can answer "is it right", and the headless harness
   has now been shown, quantitatively, to be blind to a change of this size in the era it
   can reach.
2. **The cube snapshot path** — six resolves into six layers for `06805000`. By volume it
   is the larger half of the item.
3. **A harness that can reach an admissible outdoor frame.** Every filter that is honest
   about drift discards every frame above ~1,800 draws, which is exactly where an
   environment map matters. Until that is fixed, no headless picture claim about this
   title's reflections is possible.

### One thread left open, with the measurement named

`06805000` (64x64, `k_8_8_8_8`) is a cube map at an address this renderer holds a **resolve
snapshot** for, and it is the only one of them that is. A resolve's pixels are never written
back into guest memory, so if the title renders that cube dynamically we are feeding it
zeros. Serving the snapshot is refused — a snapshot is a 2D image in set 0, so its slot
number is meaningless in set 2 — and counted. The row of `CZ_VK_TEX_CENSUS` for that address
settles it: all-zero uploads mean a cube snapshot path (six resolves into six layers) is
owed, and nothing in the decode above is at fault.

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

## 6az. THE RENDERED CUBE MAP, and a filter that could never have reported anything

Part 26. Two things, and the second is the one that changes how this project measures.

### The cube snapshot path — six resolves into six layers, and the layout was measured

§6ay bound the cube maps and left half the volume behind: `06805000` (64x64, `k_8_8_8_8`)
is an environment map the TITLE RENDERS ITSELF, so its address is a resolve destination and
guest memory there is zeros. Part 25 declined it to the 1x1 white dummy, which is exactly
the picture it had had since phase 5.

The fix is a `CubeSnapshot`: one six-layer `VK_IMAGE_VIEW_TYPE_CUBE` image in descriptor
set 2, filled by copying each face's resolve snapshot into its layer. It is deliberately a
SECOND view of six snapshots rather than a change to them — other passes sample those same
addresses as ordinary 2D surfaces in the same frame, and a resolve does not know it is
writing a cube face. Only a later fetch constant, naming the base address under a shader
that declares a cube, says so.

**The face layout is the one fact the design turns on, and nothing in this repo had ever
asked it.** The code derives face *i* as `base + i * faceBytes`, reusing the stride the
guest-memory cube path already computes — a MODEL — and then prints each derived address
with whether a resolve snapshot was found there. It could have refuted itself face by face:

```
CUBE SNAPSHOT 06805000 64x64 stride 00004000 -> set 2 slot 1, faces:
  face 0 at 06805000: filled from its resolve snapshot
  face 1 at 06809000 ... face 5 at 06819000: all filled
```

Six of six. So the engine lays a rendered cube out exactly as it lays a loaded one out,
which is worth carrying to Case West as a prediction rather than a discovery.

**The refresh is load-bearing, and the counter says so.** Each face's own resolve copies
into its layer in that resolve's own command buffer — no extra submit, and never staler
than its source. `resolve: refreshed a face of a rendered CUBE MAP` reads **8,850 in a
240 s run**: the title re-renders this map continuously, so a cube assembled once at its
first fetch would have frozen the world's reflection at that instant. That is §6s one
descriptor set over.

**Volume, with its denominator.** On a 240 s DebugJump run to the military camp,
`texture: CUBE served from resolve snapshots` is **358,767 of 999,508 cube fetches
(35.9%)**, every one of which read white before. Part 25's census put this map at 55% of
cube sampling in the opening hour; that is a different population and the two numbers are
not one claim (gotcha 242).

### The admissibility filter is unsatisfiable outdoors, and the route is not why

Part 25 handed part 26 one job before measuring anything on the new route: two runs of ONE
configuration, count the frames sharing `drawFingerprint` AND `cameraFingerprint`. The
answer, over two 420 s runs of 13,061 and 13,059 frames (`tools/frame_determinism.py`):

| matching | frames | draw counts |
|---|---|---|
| by present index, both fingerprints | 422 of 13,056 (3.2%) | 26..**141** |
| by index, both, >= 1,800 draws | **0** | — |
| by CONTENT, any index | 446 of 13,061 | max **153** |

And the route is fine — that is the part worth reading twice:

* **12,174 of 13,056 indices are above 1,800 draws in at least one arm** (93% of the run),
  so the two runs are in the same place at the same moment;
* their draw counts agree to a **median relative difference of 1.4%** (median |dA - dB| =
  89 draws against ~6,200).

The runs land together and render very nearly the same amount; they never render the same
SET. A crowd of animated actors does not produce a bit-identical draw list twice, so exact
equality selects for stasis — the frames where nothing is happening. `frame_compare.py`'s
docstring recorded the same failure from the other end in phase 5, where 257 "perfectly
aligned" frames turned out to be 257 copies of an empty scene. Gotcha 254.

**The replacement, with its noise floor from the same null pair**, over every frame above
1,800 draws:

| era median | run 1 | run 2 | null |
|---|---|---|---|
| mean luma | 56.693 | 57.229 | **0.94%** |
| distinct colours | 101,128 | 100,364 | **0.76%** |
| coverage % | 99.671 | 99.675 | 0.004% — saturated, useless |

That is a usable outdoor instrument at last, and it is what this part's cube A/B is read
with. The prescription was never new (gotcha 38: aggregate over the era, never align within
it); what was missing was a measured null for the OUTDOOR era, and one null pair supplies
it.

### The outdoor cube A/B, read with three baselines — and the third one changed the answer

Six 420 s runs of the DebugJump route, one serial block, arms alternated, era medians over
every frame above 1,800 draws (~12,170 frames each):

| arm | median mean luma | median distinct colours |
|---|---|---|
| **A** default — every cube bound, `06805000` assembled from its resolves (3 runs) | 56.593, 56.907, **56.738** | 98,566, 98,448, **103,777** |
| **B** `CZ_VK_NO_CUBE_SNAPSHOT=1` — that one map white (2 runs) | 56.291, 57.086 | 99,910, 99,093 |
| **C** `CZ_VK_NO_CUBE=1` — no cube map bound at all (1 run) | **59.469** | 106,276 |

**C is decisively separated and that is the instrument's calibration.** 59.47 against a
baseline band of 56.59-56.91 is eight times the band's whole width with no overlap, so a
whole-frame era median CAN see a cube-map change outdoors. Part 25's worry that the harness
is blind is dead twice over now — once by the magenta positive control indoors, once by
this. The direction is also the physical one: white dummy reflections ADD light, so
removing the real maps brightens the scene.

**B is not separated.** Its two runs (56.291, 57.086) straddle the baseline band from both
sides. So declining `06805000` to white moves the whole-frame median by less than the
run-to-run spread, and the honest statement is a BOUND — the rendered cube map's
contribution to the frame's median luma is below ~0.5% — not "no effect".

**AND THE THIRD BASELINE IS WHY THIS IS THE WRITE-UP AND NOT THE OTHER ONE.** On two
baselines, arm B's distinct-colour shift read **12.0x the null** and would have been
published as a result. The third baseline came in at 103,777 against 98,566 and 98,448 —
**a 5.4% spread on a statistic whose two-run null had read 0.12%** — and the 12x collapsed
into the noise. Two independent pairs had already disagreed by 6x on that statistic
(0.76% and 0.12%), which was the warning. **Median mean-luma is the usable outdoor
statistic (0.55% over three runs); median distinct-colour count is NOT, and its two-sample
nulls were flukes in both directions.**

**Fetch share is not screen area, and this A/B is the demonstration.** `06805000` is 35.9%
of all cube FETCHES and its removal is invisible in the frame's median, while removing
*every* cube map is 8x the noise floor. A fetch count measures how often the sampler is
asked; a median over pixels measures how much of the picture moved, and one 64x64
environment map on scattered reflective surfaces is a small area however often it is
sampled (gotcha 257). That is what sends this to the operator, with a specific instruction:
look at reflective SURFACES — a car bonnet, a shop window — not at the frame.

**Frame rate: unchanged, and that says less than it looks.** 13,058-13,061 presented frames
in 420 s across all six runs, a 0.02% spread. The frame is pinned at the two-vblank floor,
where a CPU cost is exactly as invisible as a saving (gotcha 243), so this bounds the cost
at "does not push the frame above the floor" and not at zero.

### Three of the validation layer's five defects, closed

`VkImageMemoryBarrier-image-03320` (20) and `VkImageViewCreateInfo-subResourceRange-01021`
(4) were both found by reading and both went to zero: a barrier on a depth/stencil format
must name both aspects, and an image's TYPE must come from its view type rather than from
its depth extent.

`vkCmdDraw-None-09600` (14) needed a run and, first, NAMES. `VK_EXT_debug_utils` now comes
in with the layer and every image we create carries one, so the message reads
`VkImage 0x235...[resolve snapshot 14A7A000 96x45 slot 32]` — and the other thirteen were
64x22, 32x11, 32x5, 32x2, 32x1 and repeats, i.e. one bloom pyramid, every snapshot created
mid-frame. The defect was the PUBLISH ORDER: the descriptor was written before the
fill-and-transition that goes into the frame's command buffer, so for that window a
descriptor claimed `SHADER_READ_ONLY` on an image still in `UNDEFINED`. Nothing indexed it
— but with a bindless heap that is an argument, not a guarantee, which is why the layer
reports it and is right to. Transition in an immediate submit, THEN publish. The snapshot
VIEW path in the same file already did exactly that, which is why views never appeared in
the messages: **when one path is quiet and its twin is not, read the quiet one.**

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

## 6ba. THE WHITE PLATEAU HAS A TONE CURVE NOW, AND THE CURVE SAYS 180 IS OUR CEILING
## AND HARDWARE'S MIDPOINT (part 30)

Part 28's hand-off left the item at one instruction: *what pins `c` at `1/pc(14).w`*. That
question presumes the constants around `c` are the ones hardware uses. Nobody had read
them. This section reads them, and the presumption does not survive.

### The emitter is exonerated for a second time, now on the ground shader specifically

Part 29's hand-off named the next step as reading our translated `ps_ad65b98593f95926`
against the capture's own disassembly (`~/DR2CZ-troubleshooting/r2-shaders/
shader_D007C18389DF0E55.ucode.frag`). Done, and they are **instruction for instruction
identical** through the whole epilogue — hardware's lines 111-126 are our `case 26/27/28`
with the same constant slots in the same operand positions:

| hardware ucode | our HLSL |
|---|---|
| `dp3 r0.x, r4.zxyy, r4.zxyy` | `r0.x = dot(r4.zxy, r4.zxy);` |
| `add r0._yzw, r6.xxyz, -c19.xxyz` | `r0.yzw = r6.xyz + -pc(19).xyz;` |
| `mad_sat r1.xyz_, -r0.xyzz, c14.wwww, c255.wwww` | `r1.xyz = saturate(-r0.xyz * pc(14).www + pc(255).www);` |
| `mul r0.xyz_, r0.xyzz, c253.yyyy` | `r0.xyz = r0.xyz * pc(253).yyy;` |
| `mad r0.xyz_, r0.xyzz, c14.wwww, c253.xxxx` | `r0.xyz = r0.xyz * pc(14).www + pc(253).xxx;` |
| `max r0.xyz_, r0.xyzz, c255.wwww` | `r0.xyz = max(r0.xyz, pc(255).www);` |
| `mad r0.xyz_, -r1.xyzz, r1.xyzz, r0.xyzz` | `r0.xyz = -r1.xyz * r1.xyz + r0.xyz;` |
| `mul r0.xyz_, r0.xyzz, c254.zzzz` | `r0.xyz = r0.xyz * pc(254).zzz;` |
| `sqrt` x3, `max oC0, r2.yzwx` | the same |

So the shading defect is not in the translation. It is in what those constants hold.

### Hardware's constants, read for the first time

`tools/xtr_draw_constants.py` (new; the replay loop of `xtr_draw_bindings.py` with a
different read-out and per-register provenance). Part 27 asked this of `w1_spawn` alone,
got `UNRECOVERABLE`, and recorded that the capture cannot answer. **It cannot answer
there.** Asked of the other six captures it answers on five — a capture is a population,
and an absence in one member is not an absence in the set (the same shape as gotcha 264).

For `ps_7d2f8f33deec1b65`, the biggest emitter (703,376 painted px), identical on all 68
recoverable draws across five captures:

    pc(252) = (0.75, 0.333333, -2.0, 0.25)
    pc(254) = (0.85, 0.15, 0.0, 1.0)
    pc(255) = (0.5, 0.0, 0.0, 0.0)

That shader's epilogue allocates the same curve into different slots than the ground
shader's, which is what part 27 meant by "differing only in which constant slots the
compiler allocated" — `A = pc(252).w`, `B = pc(252).x`, `K1 = pc(254).w`, `K2 = pc(255).x`
here; `A = pc(253).y`, `B = pc(253).x`, `K1 = pc(255).w`, `K2 = pc(254).z` on the ground.
**Read a slot number off one shader and look it up in another's constants and you get
nonsense** — the first pass of this analysis did exactly that and briefly "found" a tone
map that outputs black.

### The curve

With `x = c * pc(14).w`, the colour after exposure:

    out^2 = (max(A*x + B, K1) - saturate(K1 - x)^2) * K2

and hardware's numbers `A=0.25, B=0.75, K1=1.0, K2=0.5` give

| x | out | 8-bit |
|---|---|---|
| 0 | 0 | 0 |
| 1 | sqrt(0.5) = 0.7071 | **180** |
| 5 | 1.0 | 255 |

**180 is hardware's value at exactly full exposure, not a ceiling** — above `x = 1` the
curve keeps climbing, slowly, and only reaches 255 at five times exposure. This confirms
part 27's knee arithmetic from the constants themselves (`sqrt(K1*K2) = sqrt(0.5)`), and
it **retracts part 27's gamma explanation**: 180 is not "a literal 0.5 written into a
`k_8_8_8_8_GAMMA` surface", it is `sqrt(0.5)` written into an ordinary UNORM surface by
this `sqrt` at the end of the shader. No gamma encode is involved and none needs looking
for.

### What that does to part 28's question, and the prediction it replaces it with

Part 27's step 8 read `XE_FLOOR_PAINT`'s zero magenta as "no max takes its floor,
therefore `c' >= K1`, therefore `c = 1/pc(14).w`". Under hardware's constants that
inference is sound, and it is also **impossible as a description of the picture**: it
requires `c * E = 1` to the last bit over 141,564 contiguous pixels of varying texture and
lighting. Nothing in the epilogue clamps `x` to 1. So the constants in force in OUR
runtime are not hardware's.

**PRE-REGISTERED PREDICTION, stated before the run that tests it.** One value refutes or
confirms it, and it is the only assignment that reproduces every observation part 27
recorded:

> Our `A` term reads **0** — `pc(253).y` for `ps_ad65b98593f95926`, `pc(252).w` for
> `ps_7d2f8f33deec1b65` — with `B <= K1`, while `K1` and `K2` are correct.

Because with `A = 0` the curve becomes `out^2 = (K1 - saturate(K1-x)^2) * K2`, which is 0
at `x=0`, climbs normally, and then **pins at exactly `sqrt(K1*K2) = 180` for every `x >= 1`
and never exceeds it.** That single fact produces, with nothing else added:

* the patches being **one exact colour** rather than a bright surface (52,840 px at
  exactly 180, next colour above luma 150 has two pixels);
* the scene buffer's **max being exactly 180** in five of seven captured frames;
* the surfaces **not being modulated by lighting or time of day** — every over-exposed
  pixel lands on the same number whatever it was lit by;
* dark and mid-tone surfaces still looking **right**, because below `x = 1` the two curves
  differ by at most `A*x/2K2`;
* and the tone map faithfully **amplifying 180 to 255**, which is what part 27 measured.

If instead our `A` and `B` match hardware's, the prediction is refuted, the plateau must
come from `c` itself, and part 28's question stands unchanged.

The instrument needs no build: `CZ_VK_PSBIND=<hash>` with `CZ_VK_PSBIND_PC=252,253,254,255`
already prints exactly these registers per distinct binding, and has since part 26.

### THE PREDICTION IS REFUTED. Our constants are hardware's, to the digit

`CZ_VK_PSBIND=7d2f8f33deec1b65,ad65b98593f95926` with
`CZ_VK_PSBIND_PC=14,18,19,252,253,254,255`, outdoor DebugJump route, 64 distinct
bindings:

| | hardware (5 captures, 68 draws) | ours (55 distinct bindings) |
|---|---|---|
| `ps_7d2f8f33deec1b65` `pc(252)` | 0.75, 0.333333, -2.0, **0.25** | 0.7500, 0.3333, -2.0000, **0.2500** |
| `pc(254)` | 0.85, 0.15, 0.0, **1.0** | 0.8500, 0.1500, 0.0000, **1.0000** |
| `pc(255)` | **0.5**, 0.0, 0.0, 0.0 | **0.5000**, 0.0000, 0.0000, 0.0000 |
| `pc(18)` fog | 15.0, 0.004, 0.25, 1.0 | 15.0000, 0.0040, 0.2500, 1.0000 |
| `pc(19)` fog colour | 0.26, 0.40, 0.98, 1.0 | 0.2600, 0.4000, 0.9800, 1.0000 |

So `A = 0.25`, `B = 0.75`, `K1 = 1.0`, `K2 = 0.5` in this runtime as well. The predicted
zero is not there and the mechanism it proposed is dead.

**The run also confirms the per-shader literal pool independently, which is worth more
than the refutation.** `ps_ad65b98593f95926` reads its terms one register lower, and our
own constant file holds exactly that shifted pool:

    ps_7d2f8f33deec1b65   pc252=(0.75, 0.3333, -2, 0.25)  pc253=(2, 0.001, -1, 1.5)
                          pc254=(0.85, 0.15, 0, 1)        pc255=(0.5, 0, 0, 0)
    ps_ad65b98593f95926   pc252=(0, 0, 0, 0)              pc253=(0.75, 0.25, 0, 0)
                          pc254=(2, 0.001, 0.5, -2)       pc255=(0.85, 0.15, 0, 1)

The ground shader's `A = pc(253).y = 0.25`, `B = pc(253).x = 0.75`, `K2 = pc(254).z =
0.5`, `K1 = pc(255).w = 1.0` — the same four numbers, in the slots that shader's own
disassembly reads them from. Both pools are right, and the two disagree about which
register holds what, which is why a slot number lifted from one shader and looked up in
another's constants produces nonsense.

### What the refutation leaves, and it sharpens part 27 rather than restoring it

With the real constants the curve is quantifiably flat near full exposure. The 8-bit
value **180 is not a single value of `x`; it is the band `x` in [0.9055, 1.0080]** —
because `d(out^2)/dx = (1-x)` on the lower branch and vanishes at the join:

| x | 0 | 0.25 | 0.5 | 0.75 | 0.9 | **1.0** | 1.05 | 1.5 | 3 | 5 |
|---|---|---|---|---|---|---|---|---|---|---|
| 8-bit | 0 | 119 | 156 | 175 | 179 | **180** | 181 | 191 | 221 | 255 |

Two consequences, and they pull in opposite directions:

* **Part 27's "these surfaces are not shaded at all" is too strong.** A 10% spread in the
  shaded value is invisible at the join, so a flat 180 is equally consistent with a
  normally-shaded surface sitting at full exposure. What part 27 measured is that the
  output is constant; the input need not be.
* **The anomaly that survives is the OTHER one, and it is not the frame maximum.** The
  first draft of this section argued from "in five of seven frames nothing in the buffer
  exceeds 180, and 181 needs only `x = 1.008`" and called it a clamp. **Withdrawn before
  it was quoted anywhere: those five are the operator's NIGHT captures** (`DISABLE TIME
  OF DAY`, 90% of the slot-machine frame below luma 40). In a night scene the only thing
  at full exposure is the defect itself, so "the maximum is the defect's own value" is
  arithmetic, not evidence. Two of the seven do reach 255.

  What does survive is part 27's measurement that these surfaces are **not modulated by
  lighting or by time of day** — they sit at `x` in [0.905, 1.008] in a pitch-black room.
  That is still `c` tracking `1/pc(14).w`, i.e. part 28's statement, and it is now reached
  from the picture rather than from a paint probe: the probe asked whether
  `0.25x + 0.75` falls below its floor of 1.0, which it can only do for `x < 1`, so it
  could not have distinguished "below" from "equal" for the population it was aimed at.

### Where the clamp is NOT: the shader

Six `_sat` modifiers in hardware's disassembly of the ground shader, six `saturate()` in
our translation of it, on the same six instructions (`mul_sat r0.x, r1.z, r0.x`;
`muls_prev_sat`; `mul_sat r3.w, r3.w, c44.w`; `mul_sat r5.w, r9.y, r5.w`; the fog factor;
the tone map's `mad_sat`). There is no clamp in our translation that hardware does not
have, and the epilogue is instruction-identical. The clamp is on an INPUT to the shader,
not inside it.

### The next measurement, named

Our own exposure `pc(14).w` reads **1.0** on a large share of draws where hardware reads
**0.331** (w1_spawn) and **0.298** (w7_slotmachine). This title's exposure is scene
adaptive, so those are not matched comparisons and cannot be quoted as a defect — but the
ratio is the one the clamp predicts. If our lit colour is capped near 1.0 where hardware's
reaches ~3, an auto-exposure that targets a fixed mean would settle at ~1.0 for us and
~0.33 for hardware, and the two disagreements would be one disagreement.

So the question for part 31 is **what caps the lit colour at 1.0**, and it is a question
about the shader's inputs. The ground draw's vertex data and its three DXT1 textures are
already known to match hardware bit for bit, and DXT1 cannot carry a value above 1 on
either side — so the HDR range on hardware has to arrive through something else, and
finding which input carries it is the whole of the next step. Two ways in, both cheap:

1. **An exposure arm.** Overriding `pc(14).w` at bind time distinguishes the two worlds in
   one run: if `x` is a genuine product, halving the exposure moves the patches off 180
   and reveals detail in them; if `c` is pinned at `1/E` by something upstream, they do
   not move at all.
2. **The matched-location comparison the R2 captures can still answer.** `pc(14).w` at a
   draw of a named shader is one more column for `tools/xtr_draw_constants.py`, and the
   operator's route reaches w1_spawn.

### The comparison that is actually owed: 32 constants, of which four have been checked

The ground shader reads **32 distinct pixel constants**, counted straight out of
hardware's own disassembly of it:

    c1 c14 c15 c16 c18 c19 c20 c21 c22 c23 c24 c27 c28..c39 c40 c41 c42
    c44 c45 c46 c47 c67 c253 c254 c255

Part 27 compared **four** of them (`c1`, `c22`, `c45`, `c46`) and part 30 added five
(`c14`, `c18`, `c19`, plus the two literal pools). `c28..c39` is a twelve-register block,
which is the shape of a matrix palette or a light array; `c40/c41/c42` are three rows
used with `dp4` against a position, i.e. a shadow-map projection; `c23`, `c27` and `c67`
are scalar/colour multipliers on the lit term — and `c67.w` in particular multiplies the
value that becomes `r7`, which is the term the fog LERP and then the tone map consume.

**That block is where an HDR range could live and where nobody has looked.** The ground
draw's vertex data and its three DXT1 textures already match hardware bit for bit, and
DXT1 cannot carry a value above 1 on either side, so whatever puts hardware's colour
above `1/exposure` arrives through a constant. Comparing the remaining 23 is one run of
`CZ_VK_PSBIND_PC` against one invocation of `tools/xtr_draw_constants.py`, and the only
care needed is that many of them are camera- and light-dependent, so the comparison has
to be made at a matched location — the operator's route reaches `w1_spawn`, which is the
capture that carries the ground draw.

## 6bb. THE GROUND SHADER'S OTHER 23 CONSTANTS: hardware's values, the dataflow that
## uses them, and a prediction registered before the run (part 31)

§6ba ended by naming the owed comparison: the ground shader `ps_ad65b98593f95926` reads
**32 distinct pixel constants** and nine had been compared. This section reads the other
23 off hardware, works out from the shader's own disassembly which of them can carry a
value above 1.0, and states what our runtime is expected to disagree about **before the
run that tests it**.

### All seven captures answer, not five

Part 27 recorded that `w1_spawn` cannot supply the ground draw's constants. It cannot
supply `c253..c255` — those three arrive by a `LOAD_ALU_CONSTANT` whose source memory the
trace does not carry, and `tools/xtr_draw_constants.py` prints them as `UNRECOVERABLE`
rather than as zeros (gotcha 263). Every other register it wants is `set`, and all seven
captures carry the draw. `w1_spawn` is the one to read, because it is the capture whose
screenshot shows the defect at its worst ("white ground, turned around").

Hardware, `w1_spawn`, draw #0, `ps_ad65b98593f95926`, 25,234 indices:

    pc(  1)    1.000000    1.000000    1.000000    1.000000
    pc( 14) -120.441513    5.201186  -77.814049    0.331368     camera xyz + EXPOSURE
    pc( 15)   -0.030648   -0.173998    0.984269    0.000000     a unit direction
    pc( 16)    1.000000    0.000000    0.000000    0.000000
    pc( 18)   15.000000    0.004000    0.250000    1.000000     fog: start, 1/range, min
    pc( 19)    0.260000    0.400000    0.980000    1.000000     fog colour
    pc( 20)    0.000000    0.000000    0.000000    0.000000     SHADING-PATH SELECTOR
    pc( 21) -149.408096    6.234319 -106.297379    0.740000     a point light's position
    pc( 22)    0.388236    1.043922    0.741961    6.866384     that light's COLOUR + range
    pc( 23)   -0.371391    0.557086    0.742781    0.400000     a unit direction (sun)
    pc( 24)    3.300000    1.500000    1.000000    0.000000     SUN COLOUR — all above 1
    pc( 27)   -0.009998    0.999750    0.019995    1.000000     a unit direction
    pc( 28)   -0.015843   -0.008015   -0.001910   -1.889862  \
    pc( 29)    0.022816   -0.058226    0.055078    7.405762   |
    pc( 30)    0.005855   -0.008782   -0.011710    0.840724   |  three 4x4 shadow-cascade
    pc( 31)    0.000000    0.000000    0.000000    1.000000   |  matrices, `dp4`d against
    pc( 32)   -0.010082   -0.005100   -0.001216   -0.907196   |  the world position
    pc( 33)    0.019557   -0.049908    0.047210    6.165527   |
    pc( 34)    0.005533   -0.008299   -0.011066    0.863456   |  (c28..c31, c32..c35,
    pc( 35)    0.000000    0.000000    0.000000    1.000000   |   c36..c39 — the guess
    pc( 36)   -0.004107   -0.002078   -0.000495    0.102814   |   that c28..c39 was "a
    pc( 37)    0.007605   -0.019409    0.018359    2.353027   |   light array" was wrong)
    pc( 38)    0.004139   -0.006208   -0.008278    0.903830   |
    pc( 39)    0.000000    0.000000    0.000000    1.000000  /
    pc( 40)   -0.020821   -0.000195   -0.000683   -1.684784  \  a fourth projection, used
    pc( 41)   -0.004072   -0.002539    0.124908    9.221191   |  with tf5 — the far/static
    pc( 42)    0.000003   -0.000279   -0.000006    1.000202  /   shadow term
    pc( 44)    0.000244    0.000977   18.000000    0.071429
    pc( 45)    3.000000    6.000000    0.350000    1.000000     cascade split distances
    pc( 46)    8.000000   12.000000   32.000000    7.000000     cascade split distances
    pc( 47)    0.001000    0.002000    0.003000 3583.531006     per-cascade depth bias
    pc( 67)   10.000000    0.000000    1.000000    4.000000     .w = 4.0 — see below
    pc(253)  UNRECOVERABLE     (the literal pool; §6ba already showed ours matches)
    pc(254)  UNRECOVERABLE
    pc(255)  UNRECOVERABLE

### Where a value above 1.0 can enter, read off the disassembly

The colour the epilogue consumes is `r6.xyz`, and there are exactly three ways it is
written. Everything below is a line number in
`~/DR2CZ-troubleshooting/r2-shaders/shader_D007C18389DF0E55.ucode.frag`.

* Lines 18-20 seed `r6.xyz` with the **albedo** from `tf0`, and lines 101-103 replace it
  with the lit colour:

        102:  mad r1.xyz_, r6.xyzz, r0.wzyy, r7.xyzz     // albedo * light + r7
        103:  mul r1.xyz_, r1.zyxx, c1.zyxx              // * c1.rgb   (= 1,1,1)

* **`r7` is an ADDITIVE term and it is scaled by `c67.w = 4.0`** (line 21,
  `mul r7.xyz_, r0.wzyy, c67.wwww`, where `r0.wzy` is the `tf1` fetch squared at line 20).
  A DXT1 texel cannot exceed 1, and squaring it cannot either — but `x^2 * 4` reaches
  **4.0**. This is the largest single source of range in the shader.
* **The diffuse light is scaled by `c24 = (3.3, 1.5, 1.0)`** (line 75,
  `mul r0._yzw, r0.wwww, c24.zzyx`), then shadowed and added to the `tf2` ambient at line
  88. A warm sun whose red channel is 3.3.
* **`c22.xyz = (0.388, 1.044, 0.742)`** is a point light's colour, added at line 100, and
  its green channel is above 1.
* **`c20` chooses the path.** Lines 103/104/107 are `setp_gt r0._, c20.x` / `c20.y` /
  `c20.z`, and with hardware's `c20 = (0,0,0,0)` all three fail, which lands on the
  ordinary lit path at line 109. **A non-zero `c20.x` jumps straight to `L26` and leaves
  `r6` holding the raw albedo** — unlit, unshadowed, and unmodulated by the sun colour or
  by anything the time of day changes.

So the three constants that can carry hardware's extra range are `c67.w`, `c24.xyz` and
`c22.xyz`, and one constant — `c20` — can remove the shading entirely.

### PRE-REGISTERED PREDICTION (written and committed before the run)

> At least one of `c20`, `c24` and `c67` disagrees with hardware in our runtime, and
> `c20.x > 0` is the single assignment that reproduces part 27's measurement that these
> surfaces are not modulated by lighting or by time of day.

If instead all 32 registers match to the printed digit, the constants are exonerated as a
class, and what is left as an input to this draw is the **interpolated vertex registers**
and the **contents of `tf3`/`tf5`, which are the shadow maps WE render** — the only inputs
to this draw that part 27 did not compare, because they are not loaded from memory.

**One caveat is registered with the prediction**: `c14`, `c15`, `c21`, `c23`, `c27` and
`c28..c42` are camera-, light- and time-of-day-dependent, so a disagreement in those is
not by itself a defect. `c1`, `c16`, `c18`, `c19`, `c20`, `c22.w`, `c24`, `c44`, `c45`,
`c46`, `c47` and `c67` look like tuning values and should match wherever the run stands.

### THE PREDICTION IS REFUTED, AND THE ONE CONSTANT LEFT STANDING IS THE EXPOSURE

Outdoor DebugJump route, `CZ_VK_PSBIND=ad65b98593f95926` with all 35 registers on one
line. `c20`, `c24` and `c67` are hardware's exactly, so the mechanism the prediction
proposed is dead: the shader takes the lit path, the sun colour carries its full
`(3.3, 1.5, 1.0)` and the additive term keeps its `x4`.

**The comparison found a matched lighting state without being asked to, and the match is
better evidence than the route was.** At frame 2037 of the run our `pc(21)` — the world
position of a point light — reads `(-149.4081, 6.2343, -106.2974, 0.7400)` against
hardware's `(-149.408096, 6.234319, -106.297379, 0.740000)`. A light's world position is
a fingerprint of the scene's lighting state, and ours is hardware's to every printed
digit. `pc(22)`, `pc(23)`, `pc(24)`, `pc(44)`, `pc(45)`, `pc(46)` and `pc(47)` agree with
it: same zone, same time of day, same light set. The camera is 40 m away, which is why
`pc(14).xyz`, `pc(15)` and the twelve shadow-cascade registers differ — those follow the
view.

| register | hardware `w1_spawn` | ours, frame 2037 | |
|---|---|---|---|
| `pc(1)` | 1, 1, 1, 1 | 1, 1, 1, 1 | = |
| `pc(14).xyz` | -120.44, 5.20, -77.81 | -106.39, 8.40, -114.75 | camera |
| **`pc(14).w`** | **0.331368** | **0.2000** | **DIFFERS** |
| `pc(15)` | -0.031, -0.174, 0.984 | 0.508, -0.150, -0.849 | view direction |
| `pc(16)` | 1, 0, 0, 0 | 1, 0, 0, 0 | = |
| `pc(18)` | 15, 0.004, 0.25, 1 | 15, 0.0040, 0.2500, 1 | = |
| `pc(19)` | 0.26, 0.40, 0.98, 1 | 0.2600, 0.4000, 0.9800, 1 | = |
| `pc(20)` | 0, 0, 0, 0 | 0, 0, 0, 0 | = |
| `pc(21)` | -149.408096, 6.234319, -106.297379, 0.74 | -149.4081, 6.2343, -106.2974, 0.7400 | **=** |
| `pc(22)` | 0.388236, 1.043922, 0.741961, 6.866384 | 0.3882, 1.0439, 0.7420, 6.8664 | = |
| `pc(23)` | -0.371391, 0.557086, 0.742781, 0.4 | -0.3714, 0.5571, 0.7428, 0.4000 | = |
| `pc(24)` | 3.3, 1.5, 1.0, 0 | 3.3000, 1.5000, 1.0000, 0.0000 | = |
| `pc(27)` | -0.009998, 0.999750, 0.019995, 1 | -0.0100, 0.9998, 0.0200, 1.0000 | = |
| `pc(28..42)` | cascade matrices | different | camera |
| `pc(44)` | 0.000244, 0.000977, 18, 0.071429 | 0.0002, 0.0010, 18.0000, 0.0714 | = |
| `pc(45)` | 3, 6, 0.35, 1 | 3.0000, 6.0000, 0.3500, 1.0000 | = |
| `pc(46)` | 8, 12, 32, 7 | 8.0000, 12.0000, 32.0000, 7.0000 | = |
| `pc(47)` | 0.001, 0.002, 0.003, 3583.531 | 0.0010, 0.0020, 0.0030, 3583.4331 | = |
| `pc(67)` | 10, 0, 1, 4 | 10.0000, 0.0000, 1.0000, 4.0000 | = |
| `pc(253..255)` | UNRECOVERABLE here | §6ba compared them elsewhere | = |

**Every constant this draw reads that is not a function of the camera is hardware's, and
the only one that is neither camera-dependent nor equal is the exposure.** The item's
"23 unchecked constants" are checked; the constants are exonerated as a class.

### What the exposure number does and does not say

Our `pc(14).w` moves — 1.0000 at the menu, 0.4000, then 0.2000 with a monotone creep to
0.2040 over the ~450 frames the instrument's 64-line cap covers. So the title's
auto-exposure is running in our runtime; it is not stuck at a compiled-in default.

**And the sign is the wrong way round for exposure to be the cause.** `x = c * E`, so a
LOWER exposure moves a surface DOWN the tone curve, not up. Ours being 0.200 where
hardware's is 0.331 means the controller is pulling harder than hardware's is — which is
what a controller does when the scene it measures is too bright. Read as arithmetic: our
plateau sits at `x ≈ 1`, so our pre-exposure colour is `c ≈ 1/0.200 = 5.0`, while the
same surface on hardware, correctly shaded at the same lighting state, must be below
`1/0.331 = 3.0`. **Our colour into the epilogue is at least 1.7x hardware's, and the
exposure disagreement is the symptom of that, not its cause.**

That leaves exactly one class of input to this draw uncompared, and it is not a constant:
`tf3` and `tf5`, the **shadow atlas** — `1812F000` on hardware and `1439B000` in ours,
both 4096x1024 `k_24_8`, bound at three slots (`s3`, `s5`, `s7`) by the same draw. It is
the only input the R2 captures cannot settle by comparison, because we render it rather
than load it. And it is the term that gates the sun: line 85 `dp4 r0.x, r9.xywz, r8.wzxy`
reduces four depth comparisons to a lit factor in [0,1], line 88 multiplies the
`(3.3, 1.5, 1.0)` sun by it. **A shadow atlas that reads "nothing occludes" delivers full
sunlight to every surface, including one in a pitch-black room** — which is the
observation part 27 recorded and could not explain.

`docs/open-items.md` item 3 has been carrying an independent report of exactly that since
part 15: *"no shadows anywhere"*, and a cascade map measured half empty. The two items
are now one item.

### DECOMPOSING THE COLOUR: which of the three terms carries the range

The epilogue's input `r6.xyz` has exactly three sources, and `CZ_VK_PS_CONST_SCALE` can
zero each one independently without a rebuild:

| term | line | constant | arm |
|---|---|---|---|
| additive, `tf1^2 * 4` | 21 | `c67.w` | `67.w=0` |
| diffuse sun, `(3.3, 1.5, 1.0)` | 75 | `c24.xyz` | `24.x=0,24.y=0,24.z=0` |
| everything multiplicative | 103 | `c1.xyz` | `1.x=0,1.y=0,1.z=0` |

**PRE-REGISTERED PREDICTION, written before the runs exist.**

> `67.w=0` collapses the plateau — the count of scene-buffer pixels at exactly
> `rgb(180,180,180)` falls by more than half — and `24.x=0,24.y=0,24.z=0` does not.

The reasoning is the plateau's own neutrality. `(180,180,180)` is three EQUAL channels,
and §6ba's table shows the 8-bit output still varies with `x` around the join (179 at
0.9, 181 at 1.05, 191 at 1.5). An asymmetric multiplier like the `(3.3, 1.5, 1.0)` sun
cannot put three channels on the same 8-bit value unless it is scaled by something that
is itself near zero — so the sun is not what is holding these pixels at 180. `r7` is the
only term in the shader that is additive, neutral by construction (`tf1` squared) and
able to exceed 1.0 (`x4`). If `tf1` were serving white in our runtime, `r7` would be
exactly `4.0` in all three channels, `x = 4.0 * 0.2 = 0.8` at the exposure we measure,
and the surface would sit right at the flat part of the curve regardless of lighting or
time of day — which is the whole of part 27's observation.

**A caveat registered with the prediction:** `CZ_VK_PS_CONST_SCALE` is global, and the
literal pools are per shader (§6ba), so `c67.w` and `c24` in the other 47 emitting
shaders are scaled too. A FALL in the count is evidence; its exact magnitude is not
attributable to the ground shader alone.

## 6bc. THREE QUARTERS OF THE SHADOW ATLAS IS EMPTY, AND THE REASON IS THAT THIS TITLE
## PACKS ITS CASCADES BY OFFSETTING THE ADDRESS AND NOT THE SCISSOR (part 31)

Found while closing out §6bb: the shadow atlas is the one input to the ground draw that
the R2 captures cannot settle by comparison, because we render it rather than load it.
It can be settled by comparison after all — the capture carries hardware's copy of it as
a `MemoryRead`, and hardware's own resolve census says how it was built.

### The measurement, both sides

Ours, from `CZ_VK_SNAP_DUMP` on frame 3000 of the outdoor DebugJump route. Four
4096x1024 depth snapshots, at `1439B000`, `143BB000`, `143DB000`, `143FB000` —
0x20000 apart:

    nonzero 13.281% in every one of the four
    columns with any content: 1024 of 4096 (columns 0..1023)
    rows with any content:    1024 of 1024

Hardware's, `tools/xtr_draw_bindings.py --dump-texture 1812F000` out of `w1_spawn`,
which is the surface the ground draw binds at `s3`, `s5` AND `s7`:

    16,777,216 bytes = 4096 x 1024 x 4, i.e. D24S8
    depth == 0        3.522%
    depth == 0xFFFFFF 0.444%
    the dominant bucket is high byte 255 (41%), with a broad spread over 249..254

**Ours is 86.7% zero; hardware's is 3.5% zero.** One quarter of our atlas carries a
shadow map and three quarters carry nothing.

### Why, and it is not what the renderer was accused of

`tools/xtr_resolve_census.py` on `w1_spawn` prints the title's own resolves:

      dest      surface        region              sources
      1812F000  1280x720   0,0 .. 1280,720   DEPTH=1 colour0=1
      1814F000  4096x1024  0,0 .. 1024,1024  DEPTH=1
      1816F000  4096x1024  0,0 .. 1024,1024  DEPTH=1
      1818F000  4096x1024  0,0 .. 1024,1024  DEPTH=1

**Our renderer resolves exactly what hardware resolves, to exactly the same addresses,
with exactly the same regions.** The command processor is right. What differs is what
happens next.

Each cascade declares a **4096-wide destination surface** and copies a **1024x1024
region at the origin**, and the four are told apart by their destination ADDRESS being
pre-offset by 0x20000. That offset is an X offset in Xenos tiled address space, and the
arithmetic is exact: a 32bpp macro tile is 32x32 texels = 4096 bytes, a 4096-wide
surface is 128 tiles per tile row, so +1024 texels in X is +32 tiles is
**32 x 4096 = 131,072 = 0x20000**. The four cascades sit side by side in ONE
4096x1024 atlas at x = 0, 1024, 2048, 3072, and the shader fetches the base address and
samples across the whole width.

This renderer already knows that idiom. `DoResolve` computes

    baseKey = dest - macroTileOffset(wx, wy, surfW)

precisely so that the two 640-wide halves of a tiled frame land in one image — the
comment above it works that case out in full. **But it derives the offset from the
WINDOW SCISSOR, and the cascade resolves leave the scissor at (0,0).** So
`macroTileOffset(0, 0, 4096)` is 0, the subtraction is a no-op, and the four cascades
become four disjoint 4096x1024 snapshots each holding its own quarter. A fetch of
`1439B000` finds the first snapshot and reads zero everywhere past column 1023.

The title has two ways of saying "this copy goes to a sub-region of a larger surface" —
move the scissor, or pre-offset the address — and this renderer only understands the
first. The frame tiles use both, which is why that case worked and hid this one.

### What this does and does not explain

It is the mechanism behind `docs/open-items.md` item 3, which has carried the operator's
*"no shadows anywhere"* since part 15 and part 15's measurement of an atlas that is
partly empty. That item's three candidate readings can now be retired: it is not a
pixel-extent factor of two, the cascade is not "several smaller maps packed into one
surface" in any way the renderer needed to guess at, and the uncleared region is
certainly sampled.

**It does not explain the white surfaces, and the sign says so.** An empty region of a
shadow map reads depth 0, and the consumer's test is `sgt r8, r8, r0.x` — lit where the
sample exceeds the reference — so a zero sample reads as OCCLUDED. Three quarters of the
atlas returning zero makes the picture darker, not brighter. Recorded here rather than
folded into item 00f: it is a real defect with a hardware-verified mechanism, and it
belongs to item 3.

## 6bd. THE TERM DECOMPOSITION REFUTES ITS OWN PREDICTION, AND WHAT SURVIVES IS BETTER
## THAN WHAT IT WAS LOOKING FOR (part 31)

Four runs of the outdoor DebugJump route, `CZ_VK_SNAP_DUMP` on frame 3000, read with the
new `tools/snap_plateau.py` against the 1280x720 scene colour `0684B000`.

| arm | px at exactly rgb(180,180,180) | px at 181/182/183 grey | frame at rgb(0,0,0) |
|---|---|---|---|
| null | 1,348 (0.146%) | **0 / 0 / 0** | 2.28% |
| `67.w=0` — the additive `tf1^2 * 4` | 961 | **0 / 0 / 0** | 2.24% |
| `24.x=0,24.y=0,24.z=0` — the sun | 721 | **0 / 0 / 0** | 1.82% |
| `1.x=0,1.y=0,1.z=0` — everything multiplicative | 893 | **0 / 0 / 0** | **61.54%** |

**Read the counts as inadmissible and the invariant as the finding.** Frame 3000 of one
run is not frame 3000 of another — that is gotcha 254, and it applies to a pixel count as
much as to a fingerprint — so 1,348 against 961 is a difference between two cameras and
no null was measured for it. The prediction registered above ("`67.w=0` collapses the
plateau, `24.*=0` does not") is therefore **not supported**, and it is not refuted in the
strong sense either: the experiment as designed could not have supported it.

What IS admissible is what every arm agrees on, because it is a within-frame property:

* **The peak sits at exactly 180 in all four arms**, and
* **not one pixel of 921,600 lands on grey 181, 182 or 183, in any arm.**

The last arm makes that a real result rather than a null one. `c1.xyz = 0` zeroes the
multiplicative path of every shader that reads `pc(1)` and takes the frame from 2.28%
black to **61.54% black** — the single most destructive thing this project has done to a
frame on purpose — and the plateau at 180 comes through it untouched, still with nothing
above it.

**So the exactly-180 pixels are not the ground shader's colour terms.** They survive
zeroing its sun, its additive term and its entire multiplicative path. Either they are
written by a different shader, or by a path in which `pc(1)`, `pc(24)` and `pc(67)` are
not what they are in `ps_ad65b98593f95926` — which the per-shader literal pool (§6ba)
makes entirely ordinary.

### What the invariant is worth on its own

§6ba's curve is invertible, and it says `out = 180` if and only if `x = 1` **exactly**:

    out^2 = (max(0.25x + 0.75, 1) - saturate(1 - x)^2) * 0.5 = 0.5
      x >= 1 kills the saturate, so the max must be 1, so 0.25x + 0.75 <= 1, so x <= 1
      x <  1 makes the max 1, so 1 - (1-x)^2 = 1, so x = 1 — a contradiction

and "nothing at 181" says **no pixel in the frame has `x > 1`**. That is a ceiling on
`colour * pc(14).w` across the whole scene, holding through an arm that removed two
thirds of the picture. It also retires the doubt §6ba raised about part 27's frame
maximum: that argument was withdrawn because the seven captures behind it were NIGHT
captures where the only thing at full exposure is the defect itself. This frame is not a
night capture — it is a daylight street with mean luma 36.3 and 63,398 distinct colours —
and it still has a hard ceiling at exactly `x = 1`.

The next question is therefore whether `x` is a product at all on those pixels, and that
is a question about the peak's LOCATION rather than its population — which is immune to
the frame-matching problem, because it is read inside one frame. `CZ_VK_PS_CONST_SCALE`
with `14.w=0.25` predicts the peak moves to exactly **119** if `x = c * pc(14).w` holds
there, and stays at 180 if the register being scaled is not that shader's exposure.

### THE FIX, AND ITS CONTROL ARM

`DoResolve` now derives the destination offset from the ADDRESS as well as from the
scissor. When a copy does not cover its destination surface's width and the scissor sits
at the origin, it looks for an existing snapshot of the same kind and extent at a lower
address whose byte delta decodes to a whole number of macro tiles inside the surface, and
folds this copy into that snapshot at the decoded `(x, y)`. Source and destination offsets
are now separate: the scissor still says where in the EDRAM the pass rendered.

The delta bound (`delta < surfW * surfH * 4`) is not decoration. `0684B000` and
`1439B000` are both 1280x720 in this title and 0xD150000 apart, and the frame's first
tile — scissor at the origin, 640 of 1280 wide — asks this question of that pair every
frame. Without the bound the pair is rejected only by the `ty + copyH > surfH` test, i.e.
by arithmetic accident rather than by saying what it means.

Measured on the outdoor DebugJump route, frame 3000, same binary both ways:

| | `1439B000` 4096x1024 depth | separate 4096x1024 depth snapshots | folds |
|---|---|---|---|
| **fold on (default)** | **53.125% non-zero, all 4,096 columns** | **1** | 17,355 |
| `CZ_VK_NO_ADDR_TILE_FOLD=1` | 13.281% non-zero, columns 0..1023 | 4 | 0 |

**The arithmetic is exact and that is what makes it a mechanism**: `13.281% x 4 =
53.125%`, so each cascade is individually 53.1% populated and the fix puts all four where
one was. Every band is filled — the same per-band check that found hardware's four
cascades finds ours now at 53.12% in each of the four.

It costs nothing measurable: 6,206 presented frames and 7,339 peak draws with the fold
against 6,103 and 7,403 without, and no new Vulkan validation messages.

**And it does not touch the white surfaces, which was the prediction.** Frame 3000 of the
fixed run still carries 2,074 pixels at exactly `rgb(180,180,180)` and still not one at
grey 181. The sign argument said an empty shadow region reads as OCCLUDED and therefore
darkens; filling it in leaves the plateau exactly where it was.

### One number this run establishes that nothing else could

`CZ_VK_EXPOSURE_TRACE` (new) reports, at frame 3000, **6,116 draws with `pc(14).w` between
0.214622 and 0.214647**. So a single exposure is in force across the whole frame, to five
digits — which is what makes a whole-frame histogram invertible at all, and it had been
assumed rather than measured. At `E = 0.2146`, a pixel at the plateau has a pre-exposure
colour of `c = 1/E = 4.66`.

## 6be. THE PLATEAU IS NOT THE TONE CURVE AT x=1. THE EXPOSURE ARM SAYS SO, AND IT
## RETIRES THE MODEL PARTS 27-31 WERE ALL BUILT ON (part 31)

This was the hand-off's step 1a and it was expected to be a refinement. It is a
refutation, and of the framework rather than of a detail.

`CZ_VK_PS_CONST_SCALE="14.w=0.25"`, outdoor DebugJump route, snapshot at frame 3000.

**The arm engaged, hard, and it is not in doubt.** 11,835,619 draws had a pixel constant
scaled. The scene buffer's mean luma falls from **35.07 to 18.30** and its distinct
colour count from **62,704 to 19,841**. Whatever else is true, quartering `pc(14).w`
darkened most of this frame.

**The plateau did not move.**

| | scene mean | px at exactly rgb(180,180,180) | px at rgb(119,119,119) |
|---|---|---|---|
| default | 35.07 | 2,074 | — |
| `14.w=0.25` | **18.30** | **1,093** | **0** |

119 is not an arbitrary target. §6ba's curve maps `x = 0.25` to exactly 119, so pixels
sitting at `x = 1` whose exposure register is quartered must land there. **Zero of them
do**, and only one pixel in the whole 921,600 lands on 118. They stayed on 180.

### What that kills

Every part since 27 has read the plateau as *the shared tone curve evaluated at `x = 1`*
— part 27's paint probe, part 28's "what pins `c` at `1/pc(14).w`", part 30's constant
comparison, and §6bb/§6bd above. The arithmetic behind it is still correct: `out = 180`
if and only if `x = 1` exactly, and `255 * sqrt(K1*K2) = 180.3`. **What is refuted is
that these pixels are an output of that curve at all.** They are immune to:

* the sun colour `c24` (§6bd),
* the additive `tf1^2 * c67.w` term (§6bd),
* the entire multiplicative path `c1.xyz`, an arm that blacks out 61.5% of the frame
  (§6bd),
* and now the exposure `pc(14).w` itself, an arm that halves the scene's mean luma.

A value produced by `sqrt((max(A*c*E + B, K1) - saturate(K1 - c*E)^2) * K2)` cannot be
invariant under scaling `E`. So either these pixels come from a shader that does not
read `pc(14).w` as its exposure, or — far more economically — **they do not go through
the tone curve at all**, and 180 is arriving from somewhere else that happens to sit at
the same number.

That the number matches `255 * sqrt(0.5)` so exactly is now the thing to explain rather
than the explanation. It has been treated as a derivation for four parts; it is a
coincidence until a draw is named.

### What the next step is, and it is a different KIND of measurement

Every instrument this item has used so far perturbs an INPUT and reads the whole frame.
Four of them in a row have now returned "the plateau is unmoved", which is as much as
that class of instrument can say. The question left is **which draw paints those
pixels**, and that needs a per-draw instrument rather than a per-frame one.

`CZ_VK_DRAW_CENSUS` already lists every draw of one frame with its shader, its bindings
and its constants, and `CZ_VK_ONLY_TEX`/`CZ_VK_SKIP_TEX` can remove one texture's draws
from a frame and diff the picture. Between them the population is enumerable: dump the
census for a frame with a large plateau, then skip candidates until the 180 pixels go.
**Do not build another whole-frame arm for this** — the four above are the argument that
it cannot answer.

One caution for whoever does it, from §6bd: a plateau count compared across two runs is
not a measurement, because frame N of one run is not frame N of another (gotcha 254).
Within ONE frame, "these pixels vanished when that draw did" is sound.

### THE OPERATOR'S VERDICT: THE FIX REACHES THE PICTURE, AND A SECOND DEFECT REMAINS

Four operator screenshots at one crowd spot in Case 0-2, two arms of one binary, taken
within half an hour of each other. Files and the full index in
`~/DR2CZ-troubleshooting/part31/operator-shadow-ab/`.

**Fold ON**: *"you can see much wider — the spot where shadows are is actually in front of
the camera"*, and from the other side of the truck *"also much better, still way far from
intended behaviour, but much better."* **Fold OFF**: a smaller lit patch that jumps around
the frame — right edge in one shot, the lower-left crowd in the next.

So this is the third of the three outcomes the A/B was set up to distinguish: the resolve
fix works, and a separate defect remains beyond it.

**The improvement fits the mechanism quantitatively**, which is what makes it a
measurement rather than an impression. The title's split distances are
`pc(46) = (8, 12, 32, 7)`. With only cascade 0 populated, a pixel past roughly **8 m**
samples an empty region, reads depth 0, and `sgt r8, r8, r0.x` calls it occluded. With all
four populated the shadow term is real out to the third split at roughly **32 m**. A lit
footprint growing from ~8 m to ~32 m is exactly "much wider". What is left — "still way
far from intended" — is the geometry past the last split, where the distance fade
`c44.w` (line 81, `mul_sat r3.w, r3.w, c44.w`) and the `tf5` term are supposed to force
the surface back to fully lit. **That is the next question, and it is a different one from
the atlas.**

### A CORRECTION, in place

On the strength of the two fold-OFF shots ALONE, before any fold-ON file existed on disk,
this session wrote that the camera-dependent patch was "present on both arms, so the
resolve fix is neither the cause of this symptom nor its cure, and the defect is
downstream in the lookup". **That was wrong.** The two arms differ in the EXTENT of the
lit region, not in whether a camera-dependent region exists at all, and the comparison was
being made against a remembered pair of images rather than against files. Two shots at two
different cameras were enough to establish "a moving lit patch exists here" and not enough
to establish "the arms are the same"; the wrong property was being compared, and the
conclusion was stated more strongly than the evidence carried. Gotcha 50/51/86 covers the
remembered-control half of this. The half it does not cover is the new one:

**A symptom that survives an arm is not the same as a symptom the arm does not affect.**
Both arms show a camera-dependent lit region, so "the symptom is present in both" is true
and useless; what separates them is how far it extends. Before concluding an arm changed
nothing, name the property that would have changed if it had worked — here, the RADIUS of
the shadowed footprint, which follows directly from the split distances — and check that
one.

## 6bf. THE OTHER HALF OF EVERY SHADOW CASCADE IS REJECTED BY A DEPTH TEST AGAINST A
## BUFFER CLEARED TO ZERO — and §6bc's hardware oracle is retracted (part 32)

Part 31 fixed the shadow atlas's ADDRESS FOLD and the operator confirmed the improvement
("much wider... still way far from intended behaviour"). This section is what "still way
far" is. It is not the distance fade past the last cascade split, which was part 31's
guess and is what the hand-off recommended looking at: our translation of that fade is
instruction-for-instruction the guest's, and the arithmetic works out. It is that **half
of every cascade band holds zero, and a zero depth sample reads as OCCLUDED.**

### The measurement, and it is structural rather than scene-dependent

`CZ_VK_SNAP_DUMP` on the outdoor DebugJump route, frame 3000, and on a plain boot at
frame 600 — the same numbers both times, and the same in all four bands:

    atlas 1439B000, 4096x1024 depth:  46.8750% zero
      band 0 (x    0.. 1023):  46.8750% zero
      band 1 (x 1024.. 2047):  46.8750% zero
      band 2 (x 2048.. 3071):  46.8750% zero
      band 3 (x 3072.. 4095):  46.8750% zero
      rows   0.. 511: fully populated, every column
      rows 512..1023: populated ONLY in the last 64 columns of each band

**46.875% is 15/32 exactly, and it is identical in four bands rendered from four
different light frusta by 108, 87, 221 and 35 draws.** Scene content cannot do that. The
covered region is `[0,960]x[0,512] ∪ [960,1024]x[0,1024]` per band = 557,056 of 1,048,576
texels = 53.125%, which is the number part 31 recorded as the fix's result and read as
success.

Looked at as an image, band 2 is a perfectly good light-space depth render — street
lamps, trees, buildings, the power line — cut off dead at row 511 with no taper. A clean
horizontal edge mid-scene is a clip, not the end of the geometry.

### It is not missing geometry: the arm that says so, and the arm that could not

`CZ_VK_DEPTH_ALWAYS=1` keeps the depth test enabled and forces the comparison to ALWAYS:

| arm | atlas zero | fully covered rows |
|---|---|---|
| null | 46.8750% | 512 / 1024 |
| `CZ_VK_DEPTH_ALWAYS=1` | **1.8645%** | 533 / 1024 |

So the cascade's geometry is submitted for the whole 1024x1024 and the bottom half is
**rejected by the depth test**. Since nothing ever writes there, the value it is tested
against is the zero the EDRAM depth image was created with, and `LESS` against 0 can
never pass. (The cascade draws are `RB_DEPTHCONTROL = 0x16`: z_enable, z_write,
zfunc = 1 = LESS.)

**`CZ_VK_NO_DEPTH_TEST=1`, the arm that exists for exactly this question, cannot answer
it and answers the opposite.** Vulkan ties depth WRITES to the depth TEST: with
`depthTestEnable` false the attachment is not written at all, whatever `depthWriteEnable`
says. On a depth-only pass that turns "draw everything regardless of depth" into "write
nothing", and the atlas came back **100% zero** — which is the symptom under
investigation, reported by the instrument meant to rule it out. Gotcha 279.

### What the zero is: the clear VALUE, established by a positive control

`CZ_VK_DEPTH_CLEAR_FAR=1` ignores `RB_DEPTH_CLEAR` and clears depth to 1.0:

| arm | atlas zero |
|---|---|
| null | 46.8750% |
| `CZ_VK_DEPTH_CLEAR_FAR=1` | **0.0113%** |

The cascade fills completely. So the input is the depth CLEAR VALUE, and this title
leaves `RB_DEPTH_CLEAR` at `00000000` for nearly every pass — only the two scene-tile
resolves carry `FFFFFF00`. Our renderer applies each clear to the WHOLE 1280x1024 EDRAM
stand-in; hardware's copy block clears the tiles of the surface that pass was rendering
into.

`CZ_VK_DEPTH_CLEAR_FAR` is a diagnostic arm, not the fix — it ignores a register the
guest writes. What it establishes is that the remaining shadow defect is entirely
upstream of the lookup, in the state the cascade pass starts from.

### A registered prediction, refuted

> Scoping each resolve's clear to the region that pass rendered (`CZ_VK_SCOPED_CLEAR=1`)
> will stop small post-chain passes from wiping the cascade's region, and the atlas will
> fill well beyond 53.125%.

    null    46.8750% zero, 512/1024 rows, frame mean luma 29.10
    scoped  46.8750% zero, 512/1024 rows, frame mean luma 29.12

**Unmoved to four decimal places.** So the zeros in the cascade's bottom half are not put
there by an over-broad clear from another pass; that region is simply never cleared to
anything by anybody, and it holds the zero the image was created with. The arm is kept
because scoping is still closer to what hardware does, but it is off by default and it
is not this defect.

### What the title's own clear actually asks for

`CZ_VK_RECT_TRACE=<surfacePitch>` prints the corners of every rect-list clear on one
EDRAM surface. On the cascade surface (`RB_SURFACE_INFO` pitch 1040 — 1024 rounded up to
a multiple of 80) there is exactly **one distinct rect, issued three times a frame**:

    (960.0,0.0) (1024.0,0.0) (1024.0,1024.0)  -> BL (960.0,1024.0)  depthControl=76 vte=00

`depthControl = 0x76` is z_enable + z_write + zfunc 7 (ALWAYS) — a clear draw — and it
covers **x in [960,1024] over the full height**, which is exactly the 64-column sliver
that is the only populated part of our bottom half. The four cascade PASSES issue no
clear rect of their own at all.

So the sliver is accounted for. What is not yet accounted for is why rows 0..511 are
populated across every column when the frame's last depth clear before the cascades
writes 0 to the whole image — on that reading the atlas should be 6.25% populated, not
53.125%. **That is the one open link in the chain, and it needs an instrument this
runtime does not have: what the EDRAM depth image holds AT the cascade pass, over its
full 1280x1024 extent, rather than the 1024x1024 window the resolve copies.** The
resolve trace now prints its derived copy geometry and both clear values, which is where
that instrument should grow.

### THE RETRACTION: §6bc's hardware-side atlas measurement is an artifact

§6bc reported "ours is 86.7% zero; hardware's copy of the same surface, dumped from the
capture, is 3.5% zero with all four bands populated", from
`tools/xtr_draw_bindings.py --dump-texture 1812F000` on `w1_spawn`.

**That 16 MB is not hardware's shadow atlas. It is the previous frame's composited
scene.** Detile it as a 1280x720 8888 surface and the game's own HUD is legible in it —
"8 KILLED" across the middle, "Find Katey Zombrex" at the left. Of course it is 96.5%
non-zero: it is a photograph.

The mechanism is simple and it generalises. A `.xtr`'s memory records are SNAPSHOTS with
a time: Xenia dumps the bytes behind a resource the first time the GPU reads it. There is
exactly **one** chunk covering `1812F000` in `w1_spawn`, taken at walk position 39, and
the first resolve INTO `1812F000` is at walk position 3522. The snapshot predates the
surface. A capture therefore **cannot** supply hardware's copy of any surface the GPU
produces inside the traced frame, and the title reuses `1812F000` for both the cascade
atlas and the scene, so the one snapshot it does carry is the scene.

Gotcha 275's second half — *"a surface you RENDER is still comparable, because the
capture carries the consumer's copy of it"* — is **wrong as stated** and is corrected
here: it holds only when the address is not itself a resolve destination in the same
trace. Gotcha 280.

**What survives, and it is most of §6bc.** The address-fold fix rests on register values,
not on that dump: the resolves declare a 4096-wide destination surface while copying a
1024x1024 region at the origin, at addresses 0x20000 apart; 0x20000 is exactly +1024
texels in X for a 4096-wide 32bpp tiled surface; and the fetch declares one 4096x1024
surface at the base. The operator's verdict is independent of it too. What does not
survive is the yardstick — "hardware's is 96.5% full" was never measured, so 53.125% was
never a shortfall against a known target. **It is a shortfall against 100%, which is what
this section measures instead.**

`tools/xtr_draw_bindings.py --dump-texture` now gates itself: it reports how many memory
snapshots cover the range and when they arrived, says whether the address is a resolve
destination in the same trace, and **exits 2** when every covering snapshot predates the
first resolve to it. Run against the three DXT1 textures part 27 compared for the ground
draw it prints *"the trace issues no RESOLVE to this address, so the bytes are guest
memory the title uploaded — a sound oracle"* and exits 0, so **part 27's texture
comparison is unaffected by this retraction**; the scope is surfaces the GPU produces.

### THE MECHANISM, AND IT IS ONE LINE: a 4x MSAA surface is twice as tall too

The open link above closed the same session. Part 15 recorded the title's clear rects as
`(0,0)-(480,512)` and `(960,0)-(1024,1024)` and observed that "those do not cover a
1024x1024 map and nothing this renderer does causes it". Both halves of that are wrong,
and `CZ_VK_RECT_TRACE=0` — every rect on every surface, with the surface's pitch and MSAA
mode printed beside it — says why:

    pitch=1040 msaa=0  (960,0) (1024,0) (1024,1024)  -> BL (960,1024)   depthControl=76
    pitch= 520 msaa=2  (  0,0) ( 480,0) ( 480, 512)  -> BL (  0, 512)   depthControl=76
    pitch= 640 msaa=2  (  0,0) ( 640,0) ( 640, 360)  -> BL (  0, 360)   depthControl=76
    pitch= 320 msaa=2  (  0,0) ( 320,0) ( 320, 720)  -> BL (  0, 720)   depthControl=77

The `(0,0)-(480,512)` rect is on a **520-pitch, 4x MSAA** surface — and 520 x 2 = 1040,
which is the cascade's own sample pitch. Xenos 4x MSAA is a **2x2** sample grid, so a 4x
surface is twice as wide AND twice as tall in samples; our EDRAM stand-in is at sample
resolution. Scale both axes and the rect becomes `(0,0)-(960,1024)`, which with
`(960,0)-(1024,1024)` tiles the 1024x1024 cascade **exactly**. Scale only X — which is all
this renderer has ever done — and the union is `[0,960]x[0,512] ∪ [960,1024]x[0,1024]`,
557,056 of 1,048,576 texels, **53.125%**. That is not a number that had to be fitted: it
is the observed coverage, to four decimal places, derived from the guest's own two
rectangles.

`CZ_VK_MSAA_WINDOW_SCALE_Y=1`, on a plain boot at frame 600:

| arm | atlas zero | covered rows | frame mean luma | black | distinct colours |
|---|---|---|---|---|---|
| null | 46.8750% | 512 / 1024 | 29.08 | 61.58% | 19,172 |
| `CZ_VK_MSAA_WINDOW_SCALE_Y=1` | **0.0038%** | 1000 / 1024 | 29.04 | 61.58% | 19,175 |
| `CZ_VK_DEPTH_CLEAR_FAR=1` (diagnostic) | 0.0113% | 1000 / 1024 | 29.05 | 61.58% | — |

**The one-line change reproduces the diagnostic arm's result by honouring the guest's own
clear rects instead of ignoring a register**, and the title-screen picture does not move.

**It is an ARM and not the default yet, and the reason is in the table above.** The scene
tile's 4x clear is `(0,0)-(320,720)` on a tile that is 640 samples wide and 720 rows tall:
X wants the factor and Y appears not to. Two 4x surfaces asking for different things is
two data points, not a model. The most likely reconciliation is that the scene tile's
720 is already a SAMPLE count while the cascade's 512 is a pixel count — which would mean
the discriminator is not the MSAA mode alone — and that has to be established before this
becomes the behaviour. What is not in doubt is the cascade: two rectangles that tile a
1024x1024 map exactly under one rule and cover 53.125% of it under the other.

### And the empty half IS sampled — hardware's own cascade matrix says so

Part 15's third reading of the half-empty cascade was *"the uncleared region is never
sampled and the shadows fail elsewhere"*, and it was the one it said to test first because
it is free. It is now testable from the capture rather than from our runtime, because
§6bb read hardware's `c28..c31` — cascade 0's world-to-atlas projection — off `w1_spawn`.
The shader's fetch coordinate for that cascade is `(dot(c28,(p,1)), dot(c29,(p,1)))` and
`c31 = (0,0,0,1)`, so it is orthographic and there is no divide:

    c28 = (-0.015843, -0.008015, -0.001910, -1.889862)     ->  |grad u| = 0.017857
    c29 = ( 0.022816, -0.058226,  0.055078,  7.405762)     ->  |grad v| = 0.083333

So `u` spans 0..1 over **56.0 m** of world and `v` over **12.0 m**. A shadow map's texel
density is near-isotropic by construction, and only one reading of the atlas makes it so:

| what u and v are normalised over | texels/m in u | in v | anisotropy |
|---|---|---|---|
| u over 4096, v over 1024 | 73.1 | 85.3 | **1.17x** |
| u over 4096, v over **512** | 73.1 | 42.7 | 1.71x |
| u over 1024, v over 1024 | 18.3 | 85.3 | 4.67x |

**`v` is normalised over the full 1024 rows**, so the shader samples the half we leave
empty; and `u` is normalised over the full 4096, which is part 31's fold arrived at from
the consumer's side rather than the producer's. Under that reading one cascade's footprint
is **14.0 m x 12.0 m** — square, and the right size for a first split at 8 m and a second
at 12 m (`pc(46) = (8, 12, 32, 7)`). Under the 1024-wide reading it would be 56 m x 12 m,
which no cascade scheme produces.

That retires part 15's third reading with hardware's own constants, and it is what makes
the empty half a defect rather than dead space: a pixel whose `v` exceeds 0.5 samples zero
and `sgt r8, r8, r0.x` calls it occluded. **A camera-dependent boundary running across the
world at a fixed distance, moving as the camera moves — which is the operator's report on
the fold-ON build, word for word.**

### The reconciliation candidate for the scene tile, and the counter-example that stops it

The blocker on making `CZ_VK_MSAA_WINDOW_SCALE_Y` the default is that two 4x clear rects
want different things in Y. The four surfaces, from `CZ_VK_RECT_TRACE=0` beside
`CZ_VK_VIEWPORT_TRACE=1`, with the CLEAR's declaration and the RENDER's:

| surface | clear draw declares | clear rect | render draws declare | our EDRAM holds it as |
|---|---|---|---|---|
| shadow cascade | pitch 520, **4x** | `(0,0)-(480,512)` | pitch 1040, **1x** | 1024 x 1024 |
| scene tile | pitch 320, **4x** | `(0,0)-(320,720)` | pitch 640, **2x** | 640 x 720 |
| 640x360 post | pitch 640, **4x** | `(0,0)-(640,360)` | pitch 640, 1x and 4x | 640 x 360 |
| 320x180 post | pitch 320, **4x** | `(0,0)-(320,176)` | pitch 320, 1x and 4x | 320 x 180 |

**The title re-declares the same EDRAM as 4x purely to clear it** — a 4x clear writes four
samples per pixel, so the rect is half-size in both axes — and then renders with a
different declaration. That is why a clear rect is never the surface's own extent.

The candidate rule follows: scale a window coordinate by *(the clear declaration's sample
factor) / (the render declaration's sample factor)*, per axis, with Xenos sample factors
1x = (1,1), 2x = (1,2), 4x = (2,2).

* cascade: X 2/1 = 2, Y 2/1 = **2**. Gives `(0,0)-(960,1024)`. Correct.
* scene tile: X 2/1 = 2, Y 2/2 = **1**. Gives `(0,0)-(640,720)`. Correct.

Two for two, and it explains why X has always needed the factor unconditionally.

**The counter-example is the 640x360 post surface**, whose clear rect is already
`(0,0)-(640,360)` — the full extent — while its declaration is 4x. Under any rule that
doubles X for a 4x clear, that becomes 1280x360 on a 640-wide surface. It is harmless
today (our EDRAM is 1280 wide and shared, and the resolve copies only 640x360), which is
exactly why it has never been noticed — but it means the rule above is not yet the rule.

**And the deeper statement, which is what part 33 should decide first: our EDRAM stand-in
is at SAMPLE resolution in X and PIXEL resolution in Y.** That asymmetry is not a model,
it is the accumulated consequence of adding the X factor in part 9 and never the Y one.
Every case above is a symptom of it. The two ways out are to make the stand-in
consistently sample-resolution in both axes — which means a taller image and a resolve
that downsamples — or to carry the per-axis factor explicitly at every window-coordinate
site. The first is correct and larger; the second is what
`CZ_VK_MSAA_WINDOW_SCALE_Y` is a one-surface probe of.

### The outdoor picture, as an era aggregate

The atlas number is a within-run property and is decisive; the PICTURE needs
`tools/frame_era_medians.py`, because matched frames are unsatisfiable outdoors
(gotcha 254). Two null runs of the outdoor DebugJump route as the baseline pair, the arm
as the third, every frame above 1,800 draws:

| statistic | base1 | base2 | null | arm | vs base |
|---|---|---|---|---|---|
| coveragePct | 98.3988 | 98.3691 | 0.03% | 98.4016 | 0.02% — inside the null |
| meanLuma | 62.97 | 55.94 | **11.83%** | 56.13 | 5.59% — inside the null |
| distinctColours | 68,614 | 68,371 | 0.35% | **78,201** | **+14.17%, 40.0x the null** |

**Distinct colours is the statistic that resolves, and it moves in the direction a working
shadow term predicts**: a graded shadow replaces flat-lit surfaces with a spread of values,
where a broken one clamps whole regions to one. 40x a 0.35% floor is well past the tool's
own "under ~3x is unresolved" guidance and past the 6x by which independent null pairs have
been seen to differ.

`meanLuma` cannot resolve it here and the reason is in the table: this null pair's own
floor is **11.83%**, against the 0.94% part 26 measured. The two baseline runs did not
reach the same places (7,833 and 9,303 frames above 1,800 draws, and the arm only 4,043),
so the era each one aggregates is different. That is a fact about this pair of runs, not
about the arm — and it is why the reading quoted is the one whose null is tight.

**What is still owed is the operator**, and it is now a well-posed three-way question:
null, `CZ_VK_MSAA_WINDOW_SCALE_Y=1`, and `CZ_VK_NO_ADDR_TILE_FOLD=1` (the pre-part-31
renderer), at one crowd spot with the camera not moved between shots. Per gotcha 278 the
property to name first is the EXTENT and the CONTINUITY of the shadowed region: with the
fold alone, the shadow term is real to the third split at ~32 m and half of every cascade
still reads as occluded, so the boundary should be a hard camera-locked line across the
world; with both, it should not exist.

## 6bg. THE WHITE PLATEAU IS SOLVED. IT WAS NaN, THE NaN WAS OURS, AND THE ENTRY POINT
## WAS A VERTEX-INPUT TYPE MISMATCH ON PACKED NORMALS (part 33)

Open item 00f, reported part 26, prosecuted under a wrong model for parts 27-31,
correctly re-posed by part 31's exposure arm, closed here. The whole chain is
same-binary or same-cache arms on the outdoor DebugJump route, scene-buffer snapshot
(`0684B000`) at frame 3000, ~5,800-6,300 draws a frame.

### The hypothesis §6be asked for, and the instrument class it needed

§6be ended with: the plateau pixels are immune to four whole-frame constant arms, so
either they come from a shader that does not read `pc(14).w`, or they do not go through
the tone curve at all — and the next instrument must be per-draw, not per-frame. The
third possibility, the one that is actually true: **they go through the tone curve at
`x = NaN`.** On the host GPU `max(NaN, K1)` returns `K1` and `saturate(K1 - NaN)`
returns 0, so the shared epilogue

    out^2 = (max(A*x + B, K1) - saturate(K1 - x)^2) * K2

evaluates to exactly `sqrt(K1*K2)` = `sqrt(0.5)` = **180/255 for any NaN input** — and a
NaN is invariant under scaling ANY upstream constant, which is precisely the four
"unmoved" results. The `255 * sqrt(0.5)` coincidence §6be demoted is restored as a
derivation, with `x = NaN` in place of `x = 1`.

**Why every earlier NaN probe read clean, and this is gotcha 281:** part 27's
`XE_NAN_PAINT` tests `isnan(oC0)` at the END of the shader — downstream of the very
`max`/`saturate` pair that launders the NaN into a finite 0.7071. Its zero-magenta
result was read as "the value never was a NaN"; it could not have said that. The
detector has to sit at the OPERANDS of the laundering instruction, which is what the
`xe_max` hook already instrumented for `XE_FLOOR_PAINT` — one predicate swap away.

### The measurement chain, in order, one run each

| arm | cache defines | plateau 180 | magenta | green |
|---|---|---|---|---|
| baseline | (default) | **1,092** | 0 | 0 |
| NaN-at-max | `XE_VALUE_PAINT=0.70711 XE_FLOOR_PAINT XE_FLOOR_IS_NAN` | **0** | **1,187** | 0 |
| floor-predicate control | `XE_VALUE_PAINT=0.70711 XE_FLOOR_PAINT` | 0 | 0 | **976** |
| NaN-at-input | `XE_NAN_IN_PAINT` | 0 | **20,563** | 0 |
| input + VS-kill | `XE_NAN_IN_PAINT XE_NAN_VS_KILL_IN` | **0** | **0** | 0 |

Readings, each one sentence:

* **Every plateau pixel is NaN-fed and none is honest**: the plateau vanishes entirely
  into magenta, zero pixels legitimately compute the 0.707 band (green 0), zero remain
  at 180 (so none are stale either — three worlds separated in one frame).
* **The flag responds to its predicate, not to its plumbing**: the same hook with the
  original `(b > a)` predicate paints the same population GREEN — a NaN operand fails
  `<` and `>` alike, so a stuck flag would have shown magenta both times (gotcha 279's
  check, passed).
* **The NaN ARRIVES at the pixel shader**: `XE_NAN_IN_PAINT` samples the
  interpolator-fed GPRs at entry, before any PS arithmetic — and its footprint is
  **17x the visible plateau** (2.23% of the frame), hard-edged quads and mesh chunks on
  barriers, zombies, props and buildings. The 180 plateau is only the slice of NaN-fed
  pixels the epilogue happens to pin; the rest were wrong in unremarkable colours.
* **The NaN is in the vertex data the VS consumes**: culling every triangle whose
  declared float inputs arrive NaN (`XE_NAN_VS_KILL_IN`) takes magenta 20,563 -> 0 and
  the plateau to 0, scene otherwise normal.

### Two null results that pointed the right way, and one broken arm caught in time

* **`CZ_VK_RANGE_CENSUS`** (new): per draw, walk the index VALUES (the existing guard
  bounds `indxOffset + indexCount`, which is the number of indices, not the vertices
  they name) against each stream's declared size, and scan every float-format
  attribute's in-range bytes — FP32 and FP16 — for NaN patterns. **786,861 draws:
  zero overruns, zero NaN bytes.** The streams were clean; the NaN was being minted at
  the fetch.
* **`CZ_VK_ROBUST=1`** (new) enables `robustBufferAccess` — and was recognised as a
  NO-TEST before its null was recorded as evidence: every stream is sub-allocated from
  ONE arena VkBuffer and `vkCmdBindVertexBuffers` carries no size, so the robust bound
  is the whole arena and a per-stream overrun is invisible to it (gotcha 279's shape
  again — "defect absent" and "arm cannot see" print the same string).
* The VS-kill force control (`XE_NAN_VS_KILL_IN_FORCE`) was never run; the kill arm's
  reading is instead corroborated by the fix landing exactly where it pointed.

### The validation layer names it in one run

`CZ_VK_VALIDATION=1`, outdoor route: **10 pipelines fail
`VUID-VkGraphicsPipelineCreateInfo-Input-08733`** — a `VK_FORMAT_R32_UINT` attribute
feeding a shader input typed `vec4 of float32`, at locations 4..15 (the TEXCOORD
range). Format census over the 416 sidecars: the only R32_UINT source in this title is
**fmt16, `k_10_11_11` — the packed normal — declared in 37 vertex shaders**, always
under a TEXCOORD usage.

**The mechanism, end to end.** Fable 2 wraps packed normals in NORMAL usage, which the
emitter declares `uint4` and decodes via `tfetchR11G11B10` — types match, everything
works, and that is the design the `case 16: return VK_FORMAT_R32_UINT` mapping was
written for. **This title wraps them as TEXCOORD, whose input is `float4`.** A
`R32_UINT` attribute against a `float4` input is undefined per the spec; what the
driver actually delivered was the packed dword's BITS as a float. Any normal whose
dword has bits 30..23 all ones reads as NaN — a fraction of ordinary normal directions
— and the rest read as huge finite garbage. Per-vertex, so triangle-hard edges; across
37 vertex shaders, so every material class; laundered to one exact colour by the
epilogue all 48 pixel shaders share, so one plateau value everywhere. Hardware decodes
the same bytes with its fetch hardware and never sees any of it.

### The fix, and what it measured

Emitter (`XeUnpack_10_11_11` + the read-site format branch, XenosRecomp 4621beb): a
declared fmt16 element under a float-typed usage is unpacked in-shader from
`asuint(input.x)`, by the fetch instruction's own static format field — no spec
constant. Runtime (7889e99): fmt16 binds `R32_SFLOAT`, a plain 32-bit load that
delivers the bits intact and type-matched. 35 of 416 modules change.

| | baseline | fix |
|---|---|---|
| px at exactly rgb(180,180,180) | 1,092 | **0** |
| scene mean luma | 35.49 | **44.70** |
| distinct colours | ~80,558 | **111,956** |
| scene max | 255 | 189 |

And the picture: the crowd's blotchy flat-lit patches are gone, the pale-pink cast on
distant zombies is gone, barriers and signs shade like their neighbours. **The plateau
was only the tip: every fmt16 mesh has had garbage normals since phase 5.** Gates
re-run on the fixed cache: `--smoke` OK, `shader_dim_census` exit 0 (the ucode parse
and the SPIR-V agree on every shader), `no translated shader` = 0.

### What this dissolves elsewhere, recorded here so the next reader stops paying for it

* §6ba's closing question — "what caps the lit colour at 1.0" — is dissolved, not
  answered: nothing did. The plateau was never on the curve, so `c = 1/E` was never a
  constraint the shading had to satisfy. The exposure discrepancy (ours 1.0 where
  hardware reads 0.298-0.331) should be RE-MEASURED now that the scene it adapts to is
  lit with real normals; it may simply close.
* Part 27's "these surfaces are not shaded at all", §6ba's "`x` in [0.9055, 1.0080]"
  band, and part 28's "one shared idiom hitting its floor at `x = 1`" all described the
  laundering accurately and attributed it to the wrong input.
* The open items whose evidence was contaminated by whole-frame whiteness — LOD/00i,
  NPC part meshes, the operator's three-way shadow verdict — should be re-asked on this
  renderer.

### THE OPERATOR'S VERDICT, same day: seven for seven, closed

The operator toured all seven part-27 locations on the fixed renderer and F9-captured
each one (`~/DR2CZ-troubleshooting/part33-operator/`, assigned and indexed). Scene-buffer
plateau — pixels at exactly `rgb(180,180,180)` — is **ZERO in all seven** frames, against
15,822-141,564 at the same places in part 27; presented-frame saturated white is a flat
~1,920 px in every frame, which is the HUD text. (Part 27's captures were night runs
under `DISABLE TIME OF DAY`, so the comparison is presence/absence of the pin, not
matched luminance — and the pin is binary.) The reported objects read correct in the
pictures: the cactus is green, the slot cabinets are textured with lit screens in an
ordinarily-lit room, the register is dark metal, the pawnshop sign and ground shade like
their surroundings. **Item 00f is closed with the operator's confirmation, not just the
headless number.**

| frame | location | part 27 px@180 | part 33 |
|---|---|---|---|
| 3344 | w1_spawn | 141,564 (15.36%) | **0** |
| 5075 | w2_gasstation | 53,256 (5.78%) | **0** |
| 4271 | w3_pawnshop | 114,381 (12.41%) | **0** |
| 6738 | w4_bathroom | 15,822 (1.72%) | **0** |
| 4738 | w5_newsboxes | 16,692 (1.81%) | **0** |
| 6017 | w6_register_door | 63,562 (6.90%) | **0** |
| 6312 | w7_slotmachine | 52,840 (5.73%) | **0** |

## 6bh. THE 4x MSAA Y FACTOR IS THE DEFAULT — part 32's item 0 shipped, and the exposure
## question rode along (part 34)

Part 32 derived the mechanism (§6bf: a Xenos 4x surface is a 2x2 sample grid, twice as
tall in samples as well as twice as wide) and measured the arm; part 34 makes it the
behaviour. `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` is the same-binary control arm — the
part-33-and-earlier renderer — and the part-32 arm variable `CZ_VK_MSAA_WINDOW_SCALE_Y`
is retired rather than left as a second spelling of the default.

### The reconciliation that unblocked it, stated once

§6bf's blocker was that the SCENE tile's 4x clear `(0,0)-(320,720)` appears to want X
scaled and Y not, and its counter-example killed the candidate rule "scale by (clear
declaration's factor) / (render declaration's factor)" — a rule that would anyway need
the render declaration at clear time, which is unknowable. The rule that ships is
simpler and needs no lookahead: **a draw's window coordinates are in its OWN
declaration's pixel space, so they scale by its own declared sample factors, both
axes** (1x = (1,1), 2x = (1,2), 4x = (2,2), and our stand-in is at sample resolution
in X). Under it:

* cascade clear, 4x: `(480,512) -> (960,1024)` — tiles the map exactly with the 1x
  sliver rect. The defect this ships to fix.
* scene tile clear, 4x: `(320,720) -> (640,1440)`, clipped at the stand-in's 1024 rows.
  X covers the full tile (part 9's fix, unchanged); Y OVER-clears past the tile's 720
  rows into the shared stand-in — **which is exactly what the X factor has always done
  to the 640x360 post surface** (its 4x clear becomes 1280 wide on a 640-wide surface,
  §6bf's own counter-example, harmless for the whole life of the renderer). The
  over-clear is one approximation applied consistently to both axes now, instead of
  X-only; every gate below is the measurement that it stays harmless.
* the exact form remains a stand-in at SAMPLE resolution in BOTH axes with
  downsampling resolves — larger, still open, and nothing here forecloses it.

### The gates, all on one binary, arms differing by the env var alone

**Title boot, frame-600 snap dump.** The atlas reproduces §6bf's arm numbers to four
decimal places, which is the engagement proof (the counter dump does not survive a
`timeout` kill, so the differential IS the counter):

| arm | atlas 1439B000 zero | covered rows |
|---|---|---|
| `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` | 46.8750% (every band) | 512 / 1024 |
| default | **0.0038%** | **1024 / 1024** |

**Every other surface in the dump, compared across arms**: no surface lost coverage,
none went flat. The scene buffers and the whole post-reduction ladder gain mean luma
(+7 to +9) and distinct colours (+12-18%) — the direction a shadow term that stops
calling half of every cascade "occluded" predicts. NB part 32 recorded the arm's
title picture as "unmoved"; that was measured on the pre-part-33 renderer, whose fmt16
normals were garbage. On the fixed renderer the shadow term visibly contributes, so
"unmoved" is superseded, not contradicted.

**Outdoor era medians** (DebugJump route, three 420 s runs alternated null/arm/null,
`frame_era_medians.py`, every frame >= 1,800 draws; the three runs reached matched
depth — 12,138 / 12,130 / 12,114 era frames — unlike part 32's block):

| statistic | base1 | base2 | null floor | arm | vs base |
|---|---|---|---|---|---|
| coveragePct | 99.6750 | 99.6721 | 0.00% | 99.6801 | inside the null |
| meanLuma | 67.76 | 70.08 | 3.37% | 74.50 | +8.10%, 2.4x — unresolved |
| distinctColours | 148,717 | 146,355 | 1.60% | **159,784** | **+8.30%, 5.2x the null** |

Distinct colours is again the statistic that resolves (part 32: +14.17% at 40x its own
0.35% null, on the pre-fmt16 renderer), and it moves the way a graded shadow term
predicts. The registered prediction in commit e10df05 named that direction before the
block ran. Note the baselines themselves say what part 33 did to this scene: era-median
distinct colours was ~68k in part 26's baselines and is ~147k now.

### §6ba's owed exposure re-measure: the discrepancy no longer describes this renderer

`CZ_VK_EXPOSURE_TRACE` rode on all three outdoor runs. All three arms read identically —
the shadow change does not move the controller:

    frame ~3000:      0.211 in every arm   (part 31 recorded 0.2146 on the same route)
    era 2000..12000:  mean 0.2755, range 0.200 .. 0.354   (~10,000 frames per arm)
    within-frame spread: ~2e-5 — one exposure per frame, re-confirmed

Hardware's two point measurements — **0.331368** (`w1_spawn`) and **0.298**
(`w7_slotmachine`) — sit INSIDE our adaptive range. So "ours reads 1.0 / pinned low
where hardware reads 0.33" is not a property of the fixed renderer: the controller
adapts across the same regime hardware occupies. What would settle it entirely is a
MATCHED-LOCATION comparison (the trace beside an operator F9 at `w1_spawn`), which can
ride along with the next operator session for free. Until then §6ba's question is
downgraded from "a discrepancy to explain" to "no remaining evidence of disagreement".

### What is owed after this

* **The operator's shadow verdict**, now a cleaner three-way on one binary at one crowd
  spot, camera unmoved: default / `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` (half of every
  cascade occluded) / `CZ_VK_NO_ADDR_TILE_FOLD=1` (the pre-part-31 renderer). Name the
  property first (gotcha 278): the EXTENT and CONTINUITY of the shadowed region — with
  the control arms there is a hard camera-locked boundary across the world; with the
  default there should be none.
* The sample-resolution stand-in, as the eventual exact form of all of this.
