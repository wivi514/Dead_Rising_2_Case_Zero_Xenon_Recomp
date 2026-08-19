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

### THE OPERATOR'S VERDICT, same day: the three-way closes

Three F9 captures at one Case 0-2 crowd spot, same session state ("39 KILLED" in every
frame), same framing, one launch per arm, arms differing by one env var
(`~/DR2CZ-troubleshooting/part34-operator/`, indexed). The operator on arm 1: **"First
one looked perfect."** The atlas at each capture confirms the arms were what they
claimed, and the pictures carry the contrast the verdict rests on:

| arm | atlas at capture | the picture |
|---|---|---|
| default | **0.0006% zero, 1024/1024 rows** | clean shading, soft contact shadows, no false occlusion |
| `CZ_VK_NO_MSAA_WINDOW_SCALE_Y=1` | 46.8750%, 512/1024 | hard-edged black false-occlusion blotches across the truck and ground |
| `CZ_VK_NO_ADDR_TILE_FOLD=1` | 75.0000%, 0/1024 covered | the same class of blotches, differently placed |

One refinement against the registered property: at this close-range camera the control
arms present as BLOTCHY FALSE OCCLUSION on nearby geometry rather than as the
camera-locked boundary line (which is the same defect seen at distance). The decisive
part held: both controls show the artifact, the default shows none, and the operator
called it without prompting. **Open item 3's picture question is closed both ways.**
The exposure trace at the capture read 0.376 at 2,521 draws — a regime check only, since
this spot is not one of hardware's two captured places.

## 6bi. THE STRIPED-MATERIAL CLASS: five theories dead in one session, and the reader
## chain is exonerated — the writer is part 36's target (part 35)

The operator re-asked the parked picture items on the part-34 renderer and found the
real open defect class: **one streamed quality level of an asset renders as black/white
banded garbage** — the tanker up close, Dick the survivor at distance, the pawnshop's
boarded windows, the green building close up. Stable, stuck to the surface (operator
strafe test: the patches do not slide), painted by the scene pass, one level per asset
per boot (which level varies between boots).

### What was closed on the way

* **Item 3d (NPC part meshes) is CLOSED**: Dick renders whole in every capture on two
  binaries. The missing-parts symptom was the shader-cache gap, as the item's own
  "re-test before investigating" note predicted. What he has instead is this class.
* **Item 00i's picture is captured**: the same shop as flat colour panels at
  street-across distance and full siding up close (reload_test 30631/30807). The one
  look owed is Xenia's promotion distance.
* A garbled-subtitle-text sighting (f24288) is noted as its own thread.

### The five refutations, each by its own instrument

1. **The shadow term** — the atlas at the blotch frame is a healthy light-space render
   (0.0006% zero), and the patches stick to the surface under a strafe.
2. **A positional-IO race in our VFS** (the session's first theory, commit d65874d):
   NT makes a positional read atomic and ours was seek-then-read. The fix is correct
   and stays, but the overlap counter read **0 across two sessions** while the class
   showed — the title serializes its own IO. The lock fixed nothing visible.
   The commit's registered prediction failed and is retracted here.
3. **"The tanker wears a pickup's atlas"** — RETRACTED as misattribution: the 5,486-vert
   draw sampling the pickup atlas is an actual pickup parked in frame, on both binaries.
4. **Snapshot-too-old fallback** — cannot fire; `SnapshotMaxAge` defaults to no limit.
5. **The texture cache freezing content that later changed** — the part-23 content
   guard, re-run with revalidation on the outdoor route: **92,730,622 cache hits
   checked, 4 stale (0.00%), 4 re-uploaded.** The cache serves what memory holds.

### What is measured and stands

* At blotch time, **guest memory at the sampled address genuinely holds the banded
  content** — dumped live (`process_vm_readv` seconds after the operator's F9, standing
  still; a dump minutes later reads recycled memory and means nothing). The renderer is
  faithful; the bytes were wrong before any GPU work touched them.
* The affected textures include **runtime-composed impostor/billboard sheets**: odd
  extents (360x160, 400x240, 1024x64), DXT5, drawn as 4-vertex quads. No impostor
  entries exist on disc (`big_list --find imposter`: 0) and **no impostor-sized surface
  is ever a resolve destination** (the 61-entry resolve census is the known
  scene/post/LUT/cube set) — so the sheets are composed by the CPU, from sources not
  yet named, and the junk is composed INTO them.
* The texture census's enumerated sub-defects on this route: **245 addresses uploaded
  while guest memory was all-zero** (the part-25 `01330000` class — mostly benign in
  practice, since the guard shows their entries are not re-hit with changed bytes),
  **231 colour fetches served by a DEPTH resolve snapshot**, and the
  single-repeated-block flat-colour uploads (the far-LOD placeholder look).

### The next step, named

**Trace the WRITER of one junk sheet.** Everything that reads the bytes is exonerated;
the question is who composes them and from what. Two cheap routes, in order:
`CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1` on the outdoor route — the engine narrates its
streaming decisions and may name the impostor/composite system outright — and a write
watch on one sheet's page when its fetch first appears (the `guest_probe` machinery is
the worked example), which names the composing function's address for `gdis`.

## 6bj. Part 36: the striped-material "junk sheets" are correct to the byte, and the
## writer hunt is closed before it started

Part 35 ended with item 0s framed as "the CPU composes junk into runtime impostor
sheets; every reader is exonerated; trace the writer" — and the same night's R3
captures carried the oracle to test that framing against. Part 36 ran the comparison
FIRST, as the kickoff ordered, and the framing did not survive it.

### The measurements, in the order they killed premises

1. **The tanker.xtr fetch census** (`xtr_draw_bindings --csv`, 53,870 fetch rows):
   fmt 20 (DXT5) = 3,514 fetches / 112 distinct textures; fmt 49 (DXN) = 3,040 / 53.
   The kickoff's "only 3 DXT5 and 0 DXN, hardware may not draw impostor sheets" was a
   filtered first pass. Hardware binds the whole odd-extent DXT5 sheet class at the
   tanker site — 512x240, 400x240, 360x160, 1024x64, 88x88 — on 4..8-vertex quads,
   exactly the class part 35 called ours-only. The "LOD tier our runtime wrongly
   lingers in" branch is dead.

2. **Byte comparison against the oracle.** Our live-dumped sheets at blotch time
   (part 35's `tanker_blotch_f43675/`, seconds after the F9) versus the bytes
   hardware's GPU sampled in the R3 trace (`--dump-texture`):

   | material | ours (guest addr) | hardware (guest addr) | verdict |
   |---|---|---|---|
   | 400x240 DXT5 sheet | 036DA000 | 0746E000 | **md5 IDENTICAL** |
   | 1024x64 DXT5 sheet | 036FB000 | 0748F000 | **md5 IDENTICAL** |

   Different addresses (streaming allocation differs per boot), identical content.
   **The memory the blotch-frame fetches named holds hardware's own bytes.** For these
   textures there is no bad writer — and no writer hunt.

3. **Decode and LOOK** (`tools/tex_decode.py`, written for this; gotcha 287). Under
   the correct interpretation — tiled, pitch from the fetch constant, DXT5 — the
   400x240 sheet is a coherent foliage-billboard ALPHA CUTOUT: colour endpoints white,
   the tree silhouette in the alpha channel. The junk-scorer flagged it because
   greyscale-with-extremes is its trigger, and a cutout sheet is exactly that. The
   part-35 kickoff carried this warning verbatim and the item was scoped off the score
   anyway. Also decoded: part 35's `texdump_clean_10017000` (the GAS-station atlas,
   confirming the decoder against a known-good) and `texdump_weird_110AD000` — which
   is NOT noise either: a structured white-slat/boards texture with clean edges.

4. **Where the weird texture stands.** Its bytes appear nowhere in hardware's tanker
   frame (all 728 byte-carrying fetches searched by 4K prefix), and our own captures
   bind the same address as a 4x4 in reload_test f2601 and a 512x512 in f24288 —
   streaming reuse of the page. So it is either a real asset legitimately absent from
   hardware's frame, or real-asset content sitting at a wrongly-assigned quality level.
   **Wrong-BINDING (a real asset at the wrong streamed quality slot), not composed
   junk, now fits every recorded observation**: stable, stuck to the surface
   (UV-mapped), painted by the scene pass, one quality level per asset, which level
   varies per boot with streaming order.

5. **The content-match census** (gotcha 288): of the 459 textures our blotch frame
   dumped, **226 are byte-identical to a texture hardware's frame carries**. The 233
   others are unadjudicated, not suspect — the two frames differ in camera, time and
   streaming state, so render targets and differently-streamed assets can never match.

6. **The engine does not narrate a compositor.** The outdoor DebugJump route with
   `CZ_GUEST_DIAG=1 CZ_GUEST_LOG=1`: 1,209 `[guest]` lines, zero matches for
   impostor/billboard/composite. Route 1 of the part-35 plan is exhausted.

### What survives of item 0s

The blotch is real (operator, two renderers, R3 four-for-four says hardware is clean).
But the mechanism is upstream question marks around WHICH texture the blotched surface
samples, not around byte corruption: the one texture class we can pair cross-platform
is correct to the byte. The reframed next moves are in the item. Hardware's 16 small
colour resolves in the frame (64x64 x9, 128x64 x4, 128x128, 512x256 — none in our
61-entry resolve census) remain the standing lead for the OTHER sub-defects, and
resolve write-back to guest memory remains never-implemented on our side.

## 6bk. Part 36, operator session: BOTH quality levels of the tanker captured in one
## boot, and the texture bytes are correct in both

An operator session on the part-36 binary (13 F9 captures, 9 with live texture dumps,
`~/DR2CZ-troubleshooting/part36-operator/`) landed the pair the reframed item needs.

### The pair

| capture | what the picture shows | draws | dump |
|---|---|---|---|
| `capture_002048` | tanker CLOSE — **dark olive-green with hard-edged blotches** (the defect) | 782 textures | `texdump_f2048` |
| `capture_005614` | same tanker at STREET DISTANCE — **correct cream/tan skin**, matching hardware's R3 screenshot | 698 textures | `texdump_f5614` |

Two states of one asset, one boot, both with a full per-draw census and a live
texture dump taken seconds after the press. This is the first time the item has had
its correct and incorrect states side by side from the SAME process.

### What the pair already rules out

* **The texture bytes are hardware's, at the defect site, up close.** Pairing every
  large draw in `capture_f2536` (an earlier close-up at the same spot) against
  `tanker.xtr` by (vertex count, pixel shader) and md5-ing every bound texture:
  **~45 of ~50 pairable draws match hardware on every slot**, including the 25,234-vert
  and 18,193-vert body/cab draws. The 512x512 albedo those draws sample decodes to a
  legible atlas (rusted door, planks, cable runs) that is **md5-identical to hardware's
  `11995000`**. §6bj's byte-level exoneration was not specific to the impostor sheets.
* **The shadow atlas is healthy in the blotched frame**: 0.0003% zero, all four
  cascade bands full. Part 35's refutation of the shadow term reproduces on this boot.
* **Nothing sizable is missing.** Of hardware's 730 fetched textures, only 22 above
  32 KB are absent from our close-up frame, and decoding the DXT1 candidates shows
  they are decal/AO atlases and a blob-shadow mask — no tanker skin among them.

### The method that did NOT work, recorded so it is not re-run

Pairing draws by (verts, vs, ps) ACROSS the near and far frames and flagging slots
whose content differs yields **115 "differences"** that are almost entirely
legitimate: the crowd's zombies share meshes and vertex counts while wearing
different clothing textures, so a vertex count is not an object identity across two
frames of a live scene (the same class of error as gotcha 254's fingerprint matching).
This list cannot be triaged by eye and should not be treated as a suspect set.

### The instrument part 37 needs, and it already exists

**`CZ_VK_ONLY_TEX=<addr>` renders only the draws that bind a given texture** (and
`CZ_VK_SKIP_TEX` its complement) — that is the pixel-attribution the identification
needs: pick a candidate address out of the blotch frame's census, and the picture says
whether it is the blotched surface. Two caveats to settle first: both are read once
per process (`static ... Env(...)`), so the arm needs a fresh launch, and streaming
addresses may differ between boots — so check whether two runs of the headless
DebugJump recipe produce the same census addresses before relying on one boot's.
If they do not, the cheap fix is to make the filter re-readable at runtime (a file or
a key) so an operator can toggle it in the boot that shows the defect.

## 6bl. The game can already move Chuck: `setplayerpos` is in the shipped image, and
## this is its call sequence (part 36)

The pose work started by hunting the player's position in memory and got nowhere useful
— the object `CZ_AUTOCHUCK` steers is a controller (0 of 512 dwords move across the
map), and scanning for coordinates near the camera finds crowds and navmesh arrays but
cannot single out the player. The operator asked the right question: *doesn't the game
already have something to grab Chuck's position?* It does. This is the retail image
with its debug layer intact (the same fact gotcha 266 records for the log kill switch),
and it ships a debug CONSOLE with a command table:

```
setcamorient  setcampos  getremoteplayid  setallremoteplaypos  setplayerpos
playerinfo  getplayerinfo  zombieinfo  getactivezombiei...
```

`setplayerpos` is dispatched by string compare at **`0x825C049C`** (the table's names
live at `0x8208C3D8..`), and its body at **`0x825C04D4`** reads as follows — this is the
whole primitive, so it can be replicated without a console:

| step | code | meaning |
|---|---|---|
| `argc >= 5` | `cmplwi r27,5` | `setplayerpos <player> <x> <y> <z>` |
| `atoi(argv[1])` | `bl 0x82810BC8` | player index -> r31 |
| `atof(argv[2..4])` | `bl 0x82810210` x3 | X, Y, Z (note the handler reads them in the order argv[4], argv[3], argv[2]) |
| stack vec3 | `stfs` to `0x158/0x15C/0x160(r1)` | the position, three floats |
| `lwz r3, 0x7428(0x82A50000)` | global **`0x82A57428`** | the manager the player hangs off |
| `bl 0x82483230` with r4=1 | | resolve session/manager |
| `vtable[0x10]()` | `bctrl` | |
| `lwz r3, 0x7C(r3)` then `bl 0x8247B020` with r4 = index | | **get the player object** |
| `vtable[0x28]()` | `bctrl` | |
| `vtable[0x84](obj, &vec3)` | `bctrl` | **SET POSITION** |

Two things follow. **The teleport should call the title's own path** rather than poking
a field: the virtual setter is what handles collision, streaming and whatever else the
engine attaches to a move, and every one of those would be a separate defect to
discover by writing memory directly. And **`getplayerinfo`/`playerinfo` is the matching
READ**, which is the cheaper half of the pose problem — it will name the position's
offset for free, and it is the next thing to disassemble.

### `getplayerinfo` is the matching READ, and it shares the whole lookup

Dispatched at **`0x825C033C`**, body at `0x825C0374`. It needs only `argc >= 2`
(`getplayerinfo <player>`) and reaches the object by exactly the same chain, which is
what makes the chain trustworthy — two independent commands walk it identically:

```
mgr    = *(u32*)0x82A57428
sess   = call 0x82483230(mgr, 1)
t      = sess->vtable[0x10]()
list   = *(u32*)(t + 0x7C)
obj    = call 0x8247B020(list, index)      // null -> "error:player not found"
```

Then the two halves diverge, and both are single virtual calls on the player object:

| | call | note |
|---|---|---|
| READ | `obj->vtable[0x18](&out)` | PPC struct return: the hidden out-pointer is r3 and `this` is r4. The vec3 lands at `0x50/0x54/0x58(r1)` and the handler formats those three floats — so **position is x,y,z at +0x00,+0x04,+0x08 of the returned struct** |
| WRITE | `obj->vtable[0x28]()` then `obj->vtable[0x84](obj, &vec3)` | the `0x28` call runs first in the title's own path and is kept for that reason: it is exactly the kind of step a hand-written memory poke omits and then spends a session rediscovering |

So the pose problem is solved in the guest's own terms and needs no struct-offset
archaeology: read with `vtable[0x18]`, write with `vtable[0x84]`, get the object with
the five-step lookup above. `setcampos` / `setcamorient` are the camera equivalents and
are the remaining piece for restoring a SHOT rather than a place.

This supersedes the memory hunt as the primary route. `tools/live_findpos.py` stays
because it is the general instrument (and it is what proved the controller object
innocent), but a shipped debug command beats a heuristic scan.

## 6bm. Wiring the console's two player primitives into the runtime: the read works
## without calling anything, and the write is not safe from our hook (part 36)

§6bl read `setplayerpos` and `getplayerinfo` out of the image. This is what happened
when both were executed rather than parsed. The five-step lookup is shared and is
**safe**; what differs is what each does with the object.

### The read needs no guest call at all

The virtual `getplayerinfo` dispatches (vtable+0x18 -> `0x82483718`) is seven
instructions — `lwz r11,0x1C(r4)` / `0x20` / `0x24`, stored to the out-pointer. **The
player's world position IS `obj + 0x1C`.** So the runtime reads the three floats
directly: no call, no scratch buffer, no borrowed thread, and nothing that can fault.

That is not an optimisation, it is the fix. CALLING the virtual crashed:

| arm (same recipe, 420 s) | faults | log lines |
|---|---|---|
| control, pumps disabled | 0 | 31,994 |
| armed, read by direct field access | **0** | **32,295** |
| armed, with a teleport requested | 2 | 1,593 (died) |

The first armed version, which called the virtual and also clobbered the *live* guest
context, faulted where the control did not — two separate defects found by the same
control arm. The context one is real and general: **these pumps run inside the
`XamInputSetState` hook, so `ctx` is a live thread's register file, and setting
r3/r4/ctr on it to make a call corrupts whatever the hooked function held there.**
Copy the context. AutoChuck gets away with not copying because it fires once per state
change; anything that runs every poll will not.

**A vtable index is only meaningful for the class it was read from.** The chain hands
back more than one kind of actor, so the runtime checks the object's vtable is
`0x8205D440` — the class the disassembly came from — and DECLINES otherwise instead of
dispatching into whatever `+0x18` means for some other class. The lookup also validates
that a vtable pointer lands inside the image before dispatching through it.

Measured, gameplay, via the DebugJump route: **player at (-106.08, 6.57, -115.89)**,
stable across 144 samples. `CZ_POSE_TRACE=1` prints it once a second;
`CZ_POSE_TRACE=lookup` stops after the lookup (the bisection knob that separated the
chain from the call). The F9 pose now carries `player_pos` with the age of the reading.

### The write is a real function and our hook is the wrong place to call it

`setplayerpos`'s setter (vtable+0x84 -> `0x8243A1F0`) calls `0x82439F90` and then writes
the position at `this+0x620` — note that is a DIFFERENT field from the `+0x1C` the
getter reads, so the two are not a matched pair and poking `+0x1C` would not teleport
anything. Called from the input hook it faults inside guest code (the table above).
The console runs that command from the game's own safe point and we do not have that
point yet.

`CZ_TELEPORT_FILE=<path>` (one line, `x y z`) is built and refuses to fire without
`CZ_TELEPORT_UNSAFE=1`, printing why. **Finding the safe call site is the next task**,
and the candidates are: a hook on a per-frame guest function that runs on the thread
owning actor state, or the title's own console/command queue, which would let the game
schedule it exactly as a typed command.

## 6bn. The teleport: the crash is a THREAD-LOCAL, and the position fields are
## downstream of something else (part 36)

§6bm shipped the read and fenced off the write. This is the write, chased to the end.
It does not work yet, and every step of why is a measurement.

### The crash was never the marshalling, the state, or a race

Three candidates were killed in order:

* **Marshalling.** The vector is read back out of guest memory through the guest's own
  accessors before any call: `vec@88475060 reads back (-86.1500, 6.5700, -115.8500)`,
  the object is `B925ABE0`, its vtable `8205D440` (the class the disassembly came from),
  and the two call targets are the addresses from the image. Asked and answered.
* **A race.** The fault is byte-identical on every attempt — same host pc, same guest
  chain, same `r3 = 0`. A race varies; this does not.
* **Game state.** It faults at gameplay, standing still, with a level running.

**What it actually is:** `addr2line` on the raw host pc lands on guest `0x82639288`, at
`lwz r11, 0(r3)` immediately after `bl 0x825F9CF0` — and that callee is three
instructions:

```
lwz r11, 0(r13)   ;  li r10, 8  ;  lwzx r3, r10, r11  ;  blr
```

r13 is the PCR, so it reads **thread-local slot 8**, and its caller dereferences the
result unchecked. That slot is the engine's per-thread context: **4,581 call sites read
it and exactly 4 write it**, all in the CRT thread-startup region. On the threads our
pumps run on it is zero, so the getter returns NULL and the next load faults.

**The pumps live inside the input imports, and no input-polling thread has that
context** — censused, both threads reaching the hook report slot 8 = 0 at first sight.
So no amount of choosing a better moment inside that hook can work; it is the wrong
THREAD, permanently.

### The fix for the crash: hook the accessor itself

`PPC_FUNC(sub_825F9CF0)` — the getter — is where a queued teleport is applied. Any
thread executing it is by definition a thread with the context, so the qualification
test and the call site are the same thing, which is the only version that cannot be
wrong about the thread. The request is queued by the file pump and drained there; the
hooked function's own r3 is saved and restored around the detour, or its 4,581 callers
would receive our last callee's return value.

**Result: the calls now run to completion on an engine thread with no fault.** The
crash is solved.

### But the player does not move, and the reason is upstream

Dumping the four fields immediately after the setter returns, at real gameplay:

```
obj+001C = (-106.08, 6.57, -115.89)      <- the getter's field
obj+0250 = (-106.08, 6.57, -115.89)      <- written by 0x82439F90
obj+0620 = (-106.09, 6.57, -115.89)      <- written by the setter
obj+0638 = (-106.08, 6.57, -115.89)      <- written by the setter
```

All four hold the ORIGINAL position, and `+0x620` differs from the others in the last
digit — so the engine is rewriting these fields continuously from somewhere else. They
are outputs, not inputs. The same conclusion arrived at from the other direction:
writing all four directly (FIELD mode, no calls at all) makes the read-back report the
new position for one instant and the next sample reports the old one.

**So the authoritative position is upstream of this actor's fields** — the physics body
(this engine is Havok) or a controller that drives them.

### Where to look next, and the strong lead

**The game teleports the player successfully every time DebugJump spawns a level**, so
the working code exists in the image and is reachable: follow `DebugJump` /
`cSpawnPoint` / `LevelSpawnPoint` to whatever it calls to place the actor, and use that.
`cMissionTeleportPlayer` (`missionteleportplayer.cpp` is named in the image) is the
second candidate and is literally a mission asset for teleporting the player.

Also worth knowing before the next attempt: `setplayerpos` may itself be intended for a
paused or specific state — it was never verified to work on hardware, and "the console
has a command" is not "the command does what its name says from any state".

## 6bo. Part 37: THE STRIPED-MATERIAL CLASS IS SOLVED — the blotch was our own
## texcoord unswizzle correcting a correction the shader already makes

Item 0s, the top picture item since part 35, closed by a same-binary A/B at the
reproduced defect site. The whole investigation ran headlessly; no operator was needed.

### The chase, in order, because two of its steps retract part-36 attributions

1. **The part-36 "body/cab draws" were never the tanker.** §6bk paired the 25,234- and
   18,193-vert draws with hardware by (verts, ps) and called them the truck. Decoding
   their s0 atlas (rusted GAZ door, planks, POWER CABLES, a headlight lens) says street
   props; the same 25,234-vert mesh renders at `w1_spawn` — it is the zone's street
   clutter chunk, present in both frames because it is the same STREET. Everything §6bk
   measured about those draws (byte-identical textures, healthy atlas) is true — of the
   street, not the truck. Cross-platform pairing by vertex count then landed my first
   candidate on a 7,938-vert draw whose s0 decodes to a MAN'S HEAD (an NPC standing in
   both frames, byte-identical on both platforms for the honest reason). Gotcha 291.

2. **The truck was found by CONTENT, not by pairing.** Decode hardware's large textures
   from `tanker.xtr` and LOOK: `14790000` 1024x1024 DXT1 is unmistakably the tanker
   (cream cab, grille, wheels). Its bytes exist in our blotch-frame dump at `109FC000`
   — md5-identical — and exactly one draw binds it: **verts=5896,
   vs_fa161b0fde7aa4d5 / ps_c3ae0ec7855c4a18** (hardware draw 1163/3699, our f2048 draw
   1202/3759). Every input the shader declares (sidecar: slots 0,1,2,4) is
   byte-identical to hardware: s0 the skin, s1 `109CC000` = hardware's `14760000`
   (md5), s2/s4 the shadow atlas (healthy). Constants pc255 match the material class.
   Hardware's extra register-file slots (s3 4x4 BLACK `050C4000`, s5, s6) are unread by
   this shader — leftovers, not the difference.

3. **The shader says what the layers ARE.** Xenia's own disassembly of the same
   microcode (`r3_shaders.zip` `shader_135E5B34D16CF41E`, matched to our
   `ps_c3ae0ec7855c4a18` by byteswapped-md5 of the ucode): s1 is sampled at
   INTERPOLATOR 1 — a second UV channel — and its (squared) value joins the lighting
   sum next to the sun term, gated by a spotlight cone/attenuation block whose light
   POSITION constant (`pc(14)` = -104.4, 4.8, -121.3) is the tanker's own floodlight.
   **s1 is a baked LIGHTMAP** — decoded, it is white (lit) with hard black shapes
   (props' baked shadows). The shader also does a fully manual 4-tap PCF on the shadow
   atlas (getCompTexLOD2D / setTexLOD / point taps at ±0.5 texel / getWeights2D),
   which our translation reproduces faithfully — audited on the way, not the defect.

4. **The vertex shader names the defective input.** `vs_fa161b0fde7aa4d5` reads UV0 as
   32_32_FLOAT (untouched) and the lightmap UV as **fmt 25 = 16_16 at TEXCOORD2**,
   through `tfetchTexcoord(g_SwappedTexcoords, iTexCoord2, 2).yx` — the `.yx` being the
   microcode's OWN destination swizzle, the compensation the Fable 2 census found on
   ~87% of 16-bit fetches. Chain accounting: CopySwapped's 8-in-32 dword reverse leaves
   the 16-bit pair per-component little-endian and pair-TRANSPOSED — byte-for-byte the
   state the real fetch pipe hands the shader, whose `.yx` then corrects it. Our mask
   corrected it a SECOND time: `(V,U) -> .yxwz -> .yx -> (V,U)`. **The lightmap was
   sampled with U and V exchanged**, painting its baked prop shadows as giant
   hard-edged black patches wherever a lightmapped material was on screen.

5. **Reproduction and the A/B.** The blotch site is AT the Case 0-2 DebugJump spawn
   (operator pose `-102.2, 3.2, -123.8`; spawn `-106.1, 6.6, -115.9`), so the standard
   headless recipe + F9 (which works headlessly, by design) reproduces it: run 2's
   sweep shows the tanker cylinder in full camo-blotch, census confirming the same
   material and the same streaming addresses as the operator boot (stability holding
   across a third boot). Run 3, same binary, `CZ_VK_NO_TEXCOORD_SWAP=1`: **same frame
   index, same camera, blotches GONE** — the cylinder shades like hardware's. The two
   crops are the item's closing evidence; era medians are NOT quoted because the two
   runs' camera paths diverge (gotcha 254 applies; the matched-index F9 crop is the
   admissible comparison).

### Why the mask existed, and the two prior findings this corrects in place

* §6h added the mask and measured "63.8% -> 81.3% non-black" — on the animated
  title-camera coverage metric that §6k RETRACTED for exactly this kind of claim. The
  justification was noise wearing the shape of a finding.
* §6n then measured the mask's removal as "no effect" frame-wide and could not explain
  it; CLAUDE.md carried the destination-swizzle compensation as the "live lead". Both
  halves resolve at once: the mask's effect is real, harmful, and LOCALIZED to
  lightmapped props — invisible to any whole-frame statistic, decisive at the F9 crop.

### What changed

`vk_renderer.cpp`: the published mask is now **zero** (hardware semantics; the shader's
own swizzles are the whole correction). `CZ_VK_TEXCOORD_SWAP=1` republishes the old
mask as the same-binary control arm (repaints the blotches on demand);
`CZ_VK_NO_TEXCOORD_SWAP` remains accepted as a no-op so recorded recipes keep meaning
what they meant.

### What this closes and what it re-opens

Closes the striped-material mechanism for every surface whose defect is the lightmap
layer: the tanker up close, and by the same mechanism the class members recorded in
part 35 (Dick's far LOD, the pawnshop boards) — each still owed one look on the fixed
renderer. The part-35/36 sub-defects that were never this mechanism stay open in item
0s: hardware's 16 small colour resolves (resolve write-back still unimplemented), the
231 colour fetches served by a depth snapshot, and the billboard-sheet quality-level
question (the sheets themselves are correct to the byte, §6bj).

## 6bp. Part 38 (same day): the operator evening — the random-texture class was the
## texture cache never refreshing, two new defects cornered, and R4 delivered

One two-arm operator session (part-37 default binary, then +revalidate/+alpha-test),
13 F9s with live texdumps in arm 1, ~14 in the retest arm; evidence in
`~/DR2CZ-troubleshooting/part38-operator/`, hardware ground truth in
`Xenia logs/R4_world/`.

### The class-closure tour confirmed the part-37 fix — and found the next defect

Dick at distance and the pawnshop boards render CLEAN on the fixed renderer (both
predicted by the lightmap-UV mechanism). The tanker cab was clean too — but the TANK
CYLINDER wore, in capture_016970, an unmistakable BRICK WALL texture, and the operator
reported the general form: "almost everything up close wears a random texture", worse
the longer the session ran.

### The random-texture mechanism, pinned by one live dump

The cylinder is its own draw (5,941 verts, same material family). Guest memory at its
s0 address held a coherent PICKUP TRUCK atlas at dump time — a real asset, NOT brick.
The screen showed content the address did not hold: OUR texture cache uploads once per
(address, extent, format) and never refreshes, so any streaming-recycled address
serves its FIRST occupant forever. Part 35's justification for leaving the repair off
("4 stale of 92,730,622 hits") was measured on a 400 s headless run at one location —
a fact about that route (gotcha 293). A long play session recycles addresses
constantly, and "which prop is wrong" depends on streaming order — which is also the
last unexplained residue of item 0s ("one quality level per asset, varies per boot"):
the wrongly-bound level was a stale cache entry, and the "weird 110AD000" white-slat
dump of §6bj is retro-explained as an address mid-recycle.

**The field test:** the retest arm ran `CZ_VK_TEX_GUARD=1 CZ_VK_TEX_REVALIDATE=1` for
a full evening across the map: every prop correct up close (tanker cylinder olive and
proper at point-blank — capture_006155), boarded storefronts crisp, no reported
slowdown. **Guard+revalidate are now the DEFAULT; `CZ_VK_NO_TEX_REVALIDATE=1` is the
same-binary control arm that brings the random textures back.** A headless frame-time
A/B of the guard's cost is owed but was not allowed to block a correctness default
(the operator session is the field evidence; the pacing floor absorbed it).

### The shard trees: alpha test built, and it is NOT what the foliage uses

The operator's tree F9 (capture_f28446) shows leaf cards as solid angular shards —
the cutout never happens. The translated shaders have carried an alpha-test path
forever (SPEC_CONSTANT_ALPHA_TEST -> clip(oC0.w - g_AlphaThreshold)) with nothing
driving it: part 38 wired RB_COLORCONTROL (enable bit 3, funcs GREATER/GEQUAL -> the
clip; other enabled funcs counted BY NAME, never guessed) into a new pipeline-key bit
+ fragment-stage specialization constant, and RB_ALPHA_REF into shared+272 per draw.
It engages without regression — and the trees are UNCHANGED, so the foliage does not
use the RB alpha test. The suspect is ALPHA-TO-MASK (Xenos alpha-to-coverage,
RB_COLORCONTROL bit 4, natural on a 4x MSAA surface); the arm-1b exit counters that
would have said so were LOST because the window-close path skipped the stats dump
(fixed: `Shutdown` now calls `VkRenderer_DumpStats()` before `_Exit` — gotcha 294).
The loss does not matter much: R4's traces carry hardware's full register state at
the foliage draws, which answers the mode question offline.

### R4 closes the 00i question: the flat-panel distance look is OURS

The operator walked the Big Buck approach in OUR renderer (9 F9s, flat-color building
panels at range snapping to full texture up close, "almost everything in the game
behaves like this") and then delivered `R4_world/` the same night: eight frame-locked
single-frame traces of the same approach on hardware. **Hardware shows fully textured
buildings at every distance** — the HARDWARE sign legible from far down the street.
Item 00i is no longer "possibly the game's own streaming": it is our defect, with
eight paired oracles to chase it against, and it is now the top picture item.

## 6bq. Part 39: the mip chain, which the renderer had declared and discarded since
##      phase 5 — and the refutation of item 0t's suspect

Part 38 handed over two picture items cornered with hardware ground truth: item 00i
(the flat-panel LOD look, eight paired R4 traces) and item 0t (the shard trees,
ALPHA-TO-MASK suspected). Part 39 worked both against `Xenia logs/R4_world/`.

### The content pairing item 00i asked for, and what it exonerated

The kickoff's instruction was to pair one far building's draw between our F9 census
(`part38-operator/arm1b_revalidate/`) and the matching R4 trace **by texture CONTENT,
never by vertex count** (gotcha 291). The pair that carried across is the Big Buck
shopfront's 153-vertex sign draw, `vs_2f13eecec64e508e` + `ps_aa7b6dcff7437b20`, whose
four bound textures have the same extents on both sides (256x64, 128x32, 128x32, 16x4)
— and the bytes settle it:

| | hardware (`Big_buck_hardware_store_01`, addr `15689000`) | ours (`texdump_f10986`, addr `0E04D000`) |
|---|---|---|
| md5 of the 8 KB the sampler read | `b06f8bdd8957a7a1f2cf3edf27e0a2de` | `b06f8bdd8957a7a1f2cf3edf27e0a2de` |

Different boot, different platform, different streamed address, **identical bytes**. So
the level-0 input to the far-building draws is not the defect. (Both dumps sized themselves `w*h*bpp`, which for a TILED surface is short of the
pitch-and-rows-rounded footprint — here 8 KB of 16 KB. The comparison holds because both
sides truncated identically, but that is luck rather than a property of the method, so
**both tools were fixed in this part**: at the full 16 KB footprint the sign decodes with
zero missing blocks and mean **170.0**, which is its own level 1's **170.1** — the
same-mean invariant now holds between a base level and its chain, and it could not
before.)

Two candidate mechanisms from the kickoff died in the same pass:

* **"a fetch slot we read as unset"** — the 1x1 white dummy is bound **once** in six of
  the eight F9 frames and 39-61 times in the other two, out of 3,260-6,523 draws. Not a
  building-scale path.
* **"the streaming system raises the LOD clamp and we ignore it"** — a plausible reading
  of `ForceLODTexForStreamingWorld` and `wait_for_tex_lod`, and **refuted**:
  `mip_min_level` is **0 on all 328,164 texture fetches across all eight R4 traces**. The
  guest never disclaims level 0 here.

### What the same census found instead: a whole declared input the renderer discards

`mip_max_level` is not zero. Across **all eight R4 traces, 88,689 of 328,164 fetches
(27.0%) carry a separate MIP-CHAIN ADDRESS** — dword5's `mip_address`, a field this
project had never decoded — declaring chains up to **nine levels** deep. Our own
census of an outdoor frame says the same from our side: **8,688 of 12,491 fetches
(69.6%) declare `mipMax >= 1`.**

`xenos::DecodeTextureFetch` had parsed `mipMin`/`mipMax` since phase 5 and **no line of
the renderer read either**; `CreateImage` hardcoded `mipLevels = 1`. So every minified
surface in this game has been sampling full-resolution texels at whatever rate the
rasteriser landed on, for the whole of phase 5 (gotcha 295: a parsed-and-unread field
reads as support).

### The mip layout, measured against hardware's own bytes rather than reasoned about

dword5's layout is fixed by the dimension field at bits 9..10 that part 25 located by
census: that puts `packed_mips` at 11 and the 20-bit page-aligned mip address at 12..31.
Decoding hardware's chains out of the trace and LOOKING at them (`tools/tex_decode.py`,
the part-36 habit) confirms each clause:

| texture | level | where | what it decodes to |
|---|---|---|---|
| `15689000` 256x64 DXT1, `mip=0..4`, chain at `1568D000` | 1 | chain + 0 | a clean half-size copy: same plank, same knots |
| `115D1000` 512x512 DXT1, `mip=0..7`, chain at `115F1000` | 0/1/2/3/4 | base / +0 / +32768 / +40960 / +49152 | mean **247.4 / 247.3 / 247.3 / 247.3 / 247.1**, distinct colours **179 / 123 / 85 / 53 / 31** |

Same mean, steadily fewer distinct colours, is what a mip chain looks like and what a
wrong offset does not. So: **level 1 begins at `mipAddress` exactly; each later level
begins at the accumulated TILED footprint of the levels before it (its own pitch rounded
to 32 units by its own rows rounded to 32); and a level's pitch comes from ITS OWN width,
not from the base level's `pitchBlocks`.**

**Where it stops, and it stops loudly.** The 256x64 chain does *not* continue at +8192 —
its tail is packed into one tile at sub-tile offsets this code does not know how to
compute. The rule taken is therefore: accumulate while every preceding level is a full
tile, take the first sub-tile level (verified correct on both textures above), and
**decline everything below it, counted by name** rather than guessed (gotcha 5). A
guessed low mip is a wrong colour on a distant surface — indistinguishable from the
defect being fixed.

Built in `UploadTexture`: per-level untile into the same staging image, one
`VkBufferImageCopy` region per level, `Image::levels` so `Barrier` covers the whole
range (the trap cube maps set in part 25, one subresource axis over). The sampler
already had `mipmapMode = LINEAR` and `maxLod = VK_LOD_CLAMP_NONE`, so nothing there
changed. **`CZ_VK_NO_MIPS=1` is the same-binary control arm — the pre-part-39 renderer.**

On the outdoor DebugJump route: **1,815 textures take a chain** (of ~2,267 uploaded),
1,815 hit the packed-tail decline, 6 cube chains declined (a cube's chain is six chains
and the mip face stride is a second model on top of the base level's — not attempted).

**REGISTERED PREDICTION, before the A/B:** distant textured surfaces gain filtered
detail rather than aliased level-0 texels, and the era-median distinct-colour count
moves measurably against the `CZ_VK_NO_MIPS=1` arm at its own null. **If the picture is
unmoved, missing mips is NOT item 00i's mechanism** and this stands only as a
correctness fix — which it is either way, because the guest declared the data and we
were throwing it away.

### Item 0t: ALPHA-TO-MASK is REFUTED, and so is the alpha test as foliage's mechanism

The kickoff said to read RB_COLORCONTROL at hardware's foliage draws before building
anything. Done, across **all eight R4 traces, 40,703 draws**:

| trace | draws | distinct RB_COLORCONTROL | alpha test enabled (bit 3) | ALPHA_TO_MASK (bit 4) |
|---|---|---|---|---|
| hw01..hw08 | 5806 / 6092 / 6205 / 6657 / 4675 / 4640 / 3664 / 2964 | 5-6 values each | **0** | **0** |

Hardware sets neither bit anywhere in this area. So part 38's alpha-test wiring is
correct and inert here — which is exactly why the trees were unchanged — and **the
suspect the item named is dead.** The values present are `00018004/5/6`, `00010000` and
`00019804`: a compare function is programmed but the enable bit never is.

Nor is it a shader `kill`: of R4's **208 dumped pixel shaders exactly one** contains a
`kill` instruction (`shader_D6198686761E8FF5`, our `ps_dc4cb6371ac4d3f8`, which our
cache already has). Asking the same question of our own bank returns **324 of 324**,
which is a fact about XenosRecomp's unconditional `SPEC_CONSTANT_ALPHA_TEST` clip and
not about materials (gotcha 296).

Hardware's blend modes at these draws: `00010001` (opaque) 5,109 draws, `07060706`
(SRC_ALPHA / INV_SRC_ALPHA) 666, `01000100` 28, `01060106` 3. Our shard-tree draws
(`ps_c9ca4f73ba93d023`, capture_f28446) are `blend=00010001` — opaque.

**What is owed, and it is a capture request, not an inference:** `ps_c9ca4f73ba93d023`
is **absent from R4's 261-shader bank**, so R4 cannot say how hardware draws *that*
material — R4 is the Big Buck area and the operator's shard trees are elsewhere. The
next round-5 ask is a single-frame trace **standing at a shard tree**, which turns this
from a hunt into the same read that just closed the alpha-to-mask branch.

### The mip A/B, three runs an arm — and the registered prediction was WRONG IN SIGN

Alternated block, `CZ_VK_NO_MIPS=1` the control, era medians over every frame above
1,800 draws, read with the new `tools/frame_arm_spread.py` (which prints every run and
compares the between-arm difference against the WORST within-arm spread, instead of
letting the choice of null pair decide the answer — gotcha 300):

| statistic | mips ON (3 runs) | mips OFF (3 runs) | between | worst within-arm spread | verdict |
|---|---|---|---|---|---|
| meanLuma | 74.881 | 75.908 | **−1.35%** | 0.65% | **RESOLVED** |
| distinctColours | 143,819 | 149,030 | −3.50% | 7.91% | unresolved |
| coveragePct | 99.677 | 99.679 | 0.00% | — | saturated, reports nothing |

**The prediction registered in the commit — that distinct colours would RISE — is
retracted, and its sign was the interesting part.** Sampling an unfiltered level 0 at
high minification manufactures colour variety out of ALIASING; a correctly filtered mip
removes it. So the statistic was scoring the defect as though it were signal, and
"gains detail" was the wrong verbal model for what a mip chain does to an aggregate
(gotcha 298).

**AND THE TABLE ABOVE IS RETRACTED TOO. It measured a BUG, not the feature** — see the
next section: those three runs bound 254 mip levels that are not the textures they were
attached to, and rejecting them removes the whole effect. The block is kept here because
the retraction is the finding.

### The guard caught the rule, and the A/B above is QUALIFIED because of it

The divergence counter built alongside the chain (endpoint luma of a level against the
level above it, the same invariant the layout was confirmed with) was expected to read
zero and turn "two textures by hand" into a census. **It read 254 of 1,818 chained
textures — 14% — and the eight lines it PRINTS are a capped sample, not the count**
(gotcha 109, which this very nearly got recorded as: the first write-up of this section
said "eight"). The sample:

```
mip 0D49D000  32x128 fmt=20 level 1: endpoint luma 42.8 vs 138.0   (0.310)
mip 0E69E000 256x32  fmt=18 level 1: endpoint luma 37.6 vs 111.9   (0.336)
mip 0E698000 256x32  fmt=18 level 1: endpoint luma 33.4 vs 101.4   (0.329)
mip 0D8F4000  64x32  fmt=18 level 1: endpoint luma 27.9 vs  89.2   (0.313)
mip 0D7E7000  64x32  fmt=18 level 1: endpoint luma 24.5 vs  81.1   (0.302)
mip 0D8F8000  32x32  fmt=18 level 1: endpoint luma 38.5 vs 118.0   (0.326)
mip 0D877000  32x32  fmt=18 level 1: endpoint luma 44.5 vs 137.7   (0.323)
mip 0D89A000  32x64  fmt=18 level 1: endpoint luma 48.6 vs 145.4   (0.334)
```

Every ratio is **≈ 1/3**, which is a signature and not a scatter: we are reading a
sparse sample of a tightly packed level at a pitch it does not have, so most of what
lands in the level is unwritten. All eight printed have a level 1 narrower than a macro
tile —
and the two chains verified by hand both have a level 1 exactly 32 blocks wide, so
neither could have shown this. That is the whole argument for building the guard
(gotcha 30: a test that has never failed has not been shown capable of failing; this one
failed the first time it ran, on data its author believed was fine).

**The guard therefore REJECTS rather than counts**: the level is dropped, the chain
stops, the texture keeps the levels that passed.

**Which qualifies the A/B in the section above, and not slightly.** Those three runs
were taken on the binary that BOUND all 254 of those levels — one chained texture in
seven — so a substantial part of the −1.35% mean luma is wrongly-dark mips rather than
correct filtering — a change that darkens the scene for
the wrong reason, moving toward hardware's darker frames for the wrong reason too. Arm A
is re-run on the rejecting binary; the control arm is unaffected, since
`CZ_VK_NO_MIPS=1` never enters the block. Read the re-run's numbers, not the first
block's.


### The A/B RE-RUN on the rejecting binary — and the first block's result was the bug

Arm A re-run three times on the binary that rejects divergent levels; arm B is the same
three control runs, untouched, because `CZ_VK_NO_MIPS=1` never enters the block.

| statistic | mips ON, rejecting (3 runs) | mips OFF (3 runs) | between | worst within-arm spread | verdict |
|---|---|---|---|---|---|
| meanLuma | 76.179 | 75.908 | **+0.36%** | 1.28% | unresolved |
| distinctColours | 140,907 | 149,030 | −5.45% | 7.91% | unresolved |

**Mean luma went from −1.35% (resolved, 2.1x its floor) to +0.36% (unresolved) — the
sign flipped and the magnitude collapsed.** So the darkening the first block measured
was not correct filtering at all: it was 254 wrongly-dark levels being bound, one
chained texture in seven. The oracle agreement that made it look right — "it moves
toward hardware's darker frames" — was moving toward the right answer for the wrong
reason, which is the most dangerous shape a measurement can take (gotcha 301).

**The honest verdict on the mip chain, as implemented:**

* It is **correct**: the guest declares levels 1..n at a second address, hardware samples
  them, and this renderer was discarding them. That does not need an A/B to justify.
* Its effect on outdoor era statistics is **not resolvable at three runs an arm**. Both
  statistics are inside their arms' own noise.
* **Item 00i is untouched by it.** Nothing here says a distant building panel regained
  its siding.
* The "visibly less speckle" crop pair above was taken on the PRE-REJECTION binary and is
  therefore also suspect; a fresh look is owed. 1,560 textures still carry levels, so the
  observation may well survive — but it has not been re-taken.

Evidence kept outside the repo: `~/DR2CZ-troubleshooting/part39-mip-ab/` — the six
frame-stats files the verdict rests on, the divergence census, the crop pair, and an
INDEX naming what was discarded and why (one run overlapped the census; three predate the
rejection fix).

Why so little effect from something so structural: we upload only down to the first
sub-tile level, and reject the chain entirely on 254 textures. **The packed tail — where
the deepest minification lives, and therefore where a distant building actually needs a
level — is still declined.** That is the next thing to build, and the guard's 254 (a
deterministic count, identical in every run) is the regression test for it.

---

## §6br — the shard trees, part 40: the material is named by content, and its texture is innocent

Part 39 built the draw-ID pass and the operator delivered 23 frame-locked ID maps of a
walk down the main road, each with its census, its pose and its resolve snapshots. This
is what reading all 23 of them established, plus the headless work that followed.

### The map is its own picture, and the first read of it was wrong twice

The raw ID map is near-black (draw 4,043 is `rgb(204,15,0)`), so the obvious way to look
at it — stretch the brightness — makes neighbouring indices collide and invents flat
regions. Read that way, one map appeared to show a single draw covering the right half of
the screen; the numbers said **633 distinct draws** in the same rectangle.
`tools/drawid_read.py --palette` hashes the index to a colour instead, and the result is
legible enough to pick a canopy out of by eye. On a `CZ_VK_DRAW_ID` run there is no
photograph, so the map has to serve as one.

The second wrong read was mine and is worth recording next to it: part 39 found an
untextured shader owning 57% of a canopy in ONE tree frame and called it "the first
mechanism-shaped fact". Across the 22 walk frames that shader owns **0.00% in nineteen of
them** (mean 0.83%). One frame is one sample — gotcha 133 again, on the very finding that
was supposed to be the breakthrough.

### The foliage material, identified by the shape it covers

`ps_8452bb656149204e` + `vs_716ff2d14e06fa52`, alpha-blended (`07060706`), 12 draws and
4.78% of frame 018379. It is identified by **masking its screen footprint and looking at
the shape**: a tree canopy of overlapping quads at top-left, plus a second, distant tree
as a grid of quads. Not by a texture-format signature — that method picked a HAIR
material last session (gotcha 302). A second foliage material, `ps_e2c3ca8c13351984`,
was found the same way.

The zombie material `0A2E4000` (512x512 DXT1, 717 draws, 0.17% visible) is what the
format signature had selected. It is the crowd, and it is fine.

### What the defect actually is

Hardware (`R4_world/Big_buck_hardware_store_0{1,6}.png`, frame-locked to their traces)
renders these trees as dense, individually cut-out golden leaves on a textured trunk.
Ours renders **large flat facets with hard polygon edges**, dark brown to black, carrying
almost no leaf detail. Whatever the mechanism, it is not subtle and it is on every tree in
all 28 frames of the operator's walk.

**It reproduces headlessly.** The Case 0-2 DebugJump spawn looks out through the camp
fence at trees showing exactly the same shards, so the tree question is no longer an
operator-only experiment. Recipe: the documented DebugJump sequence with no AutoChuck.

### Four explanations refuted

* **The leaf texture is broken.** Refuted. `CZ_VK_TEX_DUMP_PS` (new, see below) pulled the
  foliage material's own textures out of a headless run: a 256x256 DXT5 **leaf sheet**, a
  128x256 DXT1 **bark** strip and a 256x512 DXT5 **branch card**. All decode cleanly
  through our own untiler, and the DXT5 **alpha planes are perfect** — the leaf sheet's
  alpha is a per-leaf cutout mask, the branch card's is a clean branch silhouette. The
  bytes the sampler sees are right.
* **The mip chain.** Refuted. The operator's three mip arms — `arm1_mips`,
  `arm2_nomips`, `arm3_fullmips` — show the same shard tree at the same place. With
  `CZ_VK_NO_MIPS` there is only level 0, so a deep-mip average cannot be the flattener.
* **`exp_adjust` on the texcoord fetch.** Refuted, and this one is worth the detail. The
  field is parsed by XenosRecomp and read by **nothing** — one occurrence in the whole
  source tree, its own declaration — which is gotcha 295's pattern exactly, and
  CLAUDE.md's "exp_adjust is zero everywhere" was measured on the FABLE 2 bank and never
  repeated here. Re-run through `tools/synth_shader_container.py`'s own control-flow
  walk: **345 vertex fetches across 99 vertex shaders, exp_adjust ZERO on every one.**
  The do-not-chase entry now holds for this title too. (A first, structural scan said
  234 non-zero of 607 and was junk — gotcha 307.)
* **The 16-bit texcoord swap (part 37's `g_SwappedTexcoords`).** Not applicable, by
  construction rather than by A/B: the foliage vertex shader fetches its UV as
  **fmt 37 = `k_32_32_FLOAT`**, a plain float2. The swap mask only touches 16-bit pairs.
  (An A/B on `CZ_VK_TEXCOORD_SWAP` was run first and was **inadmissible** — neither arm
  had a tree in frame. Recorded so it is not mistaken for a null.)

The foliage vertex shader's three inputs are the whole attribute set: `POSITION0`
(fmt 57, float3), `TEXCOORD0` (fmt 37, float2 UV), `TEXCOORD1` (fmt 16, `k_10_11_11`
packed normal — part 33's path). No normal semantic, no colour, no second UV.

### What is still open

Texture right, alpha right, UV format trivial, mips irrelevant, blend state translated
correctly (`blendEnable` is on for anything that is not ONE/ZERO). The remaining suspects
are the pixel shader's own arithmetic and its constants: `oC0.w = pc(1).w * s0.a`, and
`oC0.rgb` runs through a branch chain on `pc(20).xyz` before the shared tone epilogue. A
`pc(1).w` above 1 saturates the alpha and turns every leaf card into an opaque quad,
which is the observed symptom exactly — and this material's constants have never been
compared against hardware the way the ground shader's were in part 31.

**The next measurement is named**: get hardware's `pc(1)` and `pc(20)` for a foliage draw
out of an R4 trace with `tools/xtr_draw_constants.py` and put them beside ours. Note that
`ps_8452bb656149204e` appears in **none** of the eight R4 traces, so this needs either a
foliage shader the two sides share or a round-5 trace standing at one of the main-road
trees.

---

## §6bs — part 40, the answer: RB_COLORCONTROL was read at the wrong register index, and the shard trees fall out of one constant

§6br ended with "the next measurement is the pixel constants". It never got there —
the mechanism fell out of a three-arm A/B first, and the cause sits above every
constant: **`xenos.h` had `kRbColorControl = 0x2205`, and RB_COLORCONTROL is 0x2202.**

### The chain that found it

1. **A headless viewpoint facing the trees.** The Case 0-2 spawn + two RSLEFT camera
   entries points the camera down the street with three shard trees in the top of
   frame. No operator needed, F9 pressed synthetically.
2. **Three arms:** base / `CZ_VK_NO_DEPTH_FETCH=1` / `CZ_VK_PS_CONST_SCALE=1.w=0.25`.
   The no-depth-fetch arm transformed the trees — golden, leafy, textured — so the
   DARKNESS was the shadow term: the foliage shader four-taps the shadow atlas
   (a manual PCF in the microcode) and the canopy read occluded. But the same arm
   still showed **opaque cards** — no cutout between leaves — where hardware shows sky
   through the canopy. Two defects.
3. **The atlas snapshot** (healthy, 0.00-0.01% zero per cascade) shows the trees as
   **solid diamond cards** in cascades 1-2 — the caster stamps un-cutout quads, and
   those quads' shadows are exactly the shard shapes.
4. **The caster pixel shader is the tell.** The 1,048 mask=0 caster draws bind
   `ps_34524bb64374d20e`, whose entire body is: sample the material's ALPHA, write it
   out, `clip(oC0.w - g_AlphaThreshold)` under `SPEC_CONSTANT_ALPHA_TEST`. A caster
   samples alpha for one reason only — an alpha-tested shadow map. Yet part 39 had
   "measured" that hardware never enables the alpha test, and our own counter for it
   read zero in every log.
5. **The prepass draws carry `RB_ALPHA_REF = 0.502`** — a meaningful cutout threshold
   on a supposedly disabled test. That smell broke the deadlock: the Fable 2 port
   (whose alpha test is picture-validated) reads RB_COLORCONTROL at **0x2202**; ours
   read **0x2205**.
6. **Settled by histogram, not by authority:** per-draw values of 0x2202 over R4
   trace 01 all carry the 0xAA alpha-to-mask sample-offset signature in the top byte,
   with enable+GREATER (0x0C) on 316 draws, GEQUAL, EQUAL, and GREATER+A2M (0x1C);
   0x2205's values never set bit 3 anywhere. 0x2205 is RB_BLENDCONTROL1 — the per-RT
   blend controls interleave at 0x2201/0x2205/0x2209/0x220D, which is the layout
   mistake the original map made.

### What the fix does

Across all eight R4 traces, hardware runs **4,975 of 40,703 draws (12.2%) with the
alpha test enabled** — the foliage, the fences, the hair sheets, the horizon
backdrops, and 1,787 shadow-caster draws. On our side the test now fires on ~3.07M
draws over a five-minute run. The same-binary A/B at the treecam viewpoint
(`CZ_VK_NO_ALPHA_TEST=1` = the old renderer): the dark shard plates are GONE — lit,
textured, cutout canopies. Both defects close at once, because the caster now clips
leaf holes into the shadow map instead of stamping solid quads.

Gates: title screen +0.947 against capture E2 (its usual level for an animated
background); `no translated shader` = 0.

### Left open, counted rather than guessed

* **Func EQUAL, 1.15M draws/run** (hardware: 176 in trace 01, `ps_34524bb64374d20e`
  ref=1.0 blended — the two-pass cutout's core-redraw pass). Un-emulated;
  XenosRecomp's clip is GEQUAL-shaped and cannot express EQUAL. Visual cost should be
  limited to slightly softer cutout edges.
* **GREATER at ref=0.0** (321 draws, blended): our `clip(w - ref)` is >= where
  hardware is >, which differs only at alpha exactly 0 — invisible on a blended draw,
  and the casters use ref=0.502 where it matters.
* **A2M without the test**: counted, zero observed so far.
* The §6br program (foliage pixel-constant comparison) is superseded — no constant was
  ever wrong.

## §6bt — part 41, the distance part: global aniso refuted by the shadow sampler, per-fetch samplers built, and the packed mip tail decoded

Part 41 executes `docs/part41-kickoff.md` (the distance plan). Three results in the
first session, two of them landed as defaults and one landed as a retraction worth
recording in full.

### The global-aniso experiment, refuted the same hour it ran (item 1a -> 1b)

The plan's step 1a — `anisotropyEnable` on the one linear sampler, 16x, global —
was committed with a registered prediction (4f3a0e3) and refuted by its very first
matched F9 capture: the ON arm renders dense dark red/black speckle across walls,
fences, characters and props at the treecam viewpoint. RETRACTED IN PLACE; the
commit is reverted by d5b8fdc.

The mechanism was named by hardware before any code moved: histogramming dword3
bits 25..27 over all 621 distinct fetch constants in R4 trace 01:

* the field reads ONLY 0, 3, 4 — valid ANISO_FILTER enum values, so the bit
  position (after mag:2 min:2 mip:2 at 19..24) is confirmed against an independent
  decode;
* the world's albedo textures ask for 4:1 (179 textures) and 8:1 (321); 121 ask
  for none; NOTHING asks for 16:1;
* **the 4096x1024 shadow atlas fetch asks for aniso=0 AND mag/min/mip all POINT.**

So a global aniso sampler anisotropically filters the shadow-map depth lookups —
averaging depth values across a long grazing footprint before the shader's manual
comparison — and the speckle is that comparison flickering per pixel. Hardware
never sees it because the aniso degree is per fetch constant. (A second divergence
fell out for free: we had been LINEAR-filtering the shadow atlas since phase 5
where hardware asks for POINT.)

Two measurement lessons, both now in the record:

* The registered metric read the regression WRONG-WAY-ROUND: the prediction said
  "sharpness rises", and the speckled arm read **-19%** (4.351 vs 5.313/5.389,
  within-null spread 1.4%, outdoor era, 287-349 frames/run). Without the matched
  F9 eyeball that drop reads as "aniso did nothing useful", not "aniso broke the
  shadow term". A metric can flag a defect while mislabeling it; the picture named
  the mechanism (the speckle is visibly the shadow term) in one look.
* The failed arm was still the right experiment to run first: one 7-minute run
  bought the mechanism, the field position, and the correct design.

### Per-fetch samplers (d5b8fdc) — the plan's 1b, now the default

One `VkSampler` per distinct (mag, min, mip, aniso) spec decoded from the fetch
constant, created on first sight, written into the set-3 update-after-bind heap,
and published per slot where the index had been hardcoded 0 since phase 5. The
first boot creates exactly the census's four specs — trilinear/no-aniso,
trilinear/8:1, trilinear/4:1, and point/point/point/no-aniso (the shadow atlas) —
which is the engagement evidence and the two-sided check in one log line each.
Address modes stay REPEAT deliberately: the clamp fields are kickoff item 5's
experiment (the cyan edge fringes) with their own prediction.

Arms: `CZ_VK_NO_FETCH_SAMPLERS=1` = the part-40 renderer (everything reads sampler
0, plain trilinear REPEAT — same binary). `CZ_VK_ANISO=N` caps the degree; `=0`
keeps per-fetch filters while disabling aniso, separating the change's two halves.
`CZ_VK_NO_ANISO` never existed in a lasting binary and is NOT an arm.

### The packed mip tail (409777d) — item 2, decoded from 7,515 hardware votes

`tools/packed_mip_derive.py` (9f84cfe) carries no remembered table. It walks every
packed-mips DXT texture all eight R4 traces hold bytes for, validates the unpacked
chain with part 39's accumulation rule, decodes the shared tail tile to TEXELS
once, and brute-forces every block-aligned offset per tail level against the 2x
box downsample of the level above, scored on luma AND alpha. (The first draft
scored DXT endpoint luma only and went blind exactly where the tail matters — a
4x4 level is one block, and one block's two endpoints have no variance. Gotcha
287's shape, caught in the first shakeout.)

The answer, at 7,466 of 7,515 informative votes across DXT1 and DXT5: **a square
packed level of width W blocks sits at block (W, 0) in the shared tile** — the
16-texel level at (4,0), the 8 at (2,0), the 4 at (1,0). Equivalently: texel
offset = the level's own texel width. Non-square tails are too rare in this
title's traces to derive (9 votes, inconsistent) and stay declined-and-counted,
as do non-DXT formats and sub-block levels.

The runtime walk now shifts tail reads by the derived offset and stops advancing
`chainOff` across the shared tile — advancing per level is exactly what made the
old read land on empty blocks and end every chain at "PACKED TAIL REACHED". Both
part-39 guards still run per tail level. `mip: packed tail level TAKEN` reads
1,877 on the boot route where it was structurally zero for parts 39-40;
`CZ_VK_NO_MIP_TAIL=1` reproduces the part-39/40 walk byte-for-byte, same binary.

### The state of the kickoff's own items

* Item 1a: refuted as shipped-default; superseded by 1b (done, default).
* Item 1b filters+aniso: DONE; clamps deferred to their own experiment.
* Item 2: DONE pending the era-median A/B (running as this is written).
* `verify/capture_002863` adjudicated: part-40's fixes hold on the big canopies;
  the small orange tree's hard triangular shards at range are item 4's signature.
* Items 3, 4, 5: untouched.

### §6bt addendum — the three-arm A/B verdict (same binary d5b8fdc, 9 runs, alternated)

Treecam route, arms def (samplers+tail) / `CZ_VK_NO_FETCH_SAMPLERS` /
`CZ_VK_NO_MIP_TAIL`, three runs each, null from the def pair. All nine runs
landed outdoors (6,618-7,404 max draws); both arms proved their engagement in the
log (nfs creates no samplers; ntl takes zero tail levels).

* **Per-fetch samplers: CONFIRMED.** Outdoor-era median sharpness def
  5.492/5.539/5.551 vs nfs 5.341/5.398/5.437 — **no overlap at three runs an
  arm**, +2.6% at the medians, the registered direction. The matched F9s agree:
  no speckle anywhere on def, and the far grass slope and fence mesh hold texture
  where nfs washes out. Era medians: meanLuma inside/near the null on every arm
  (no away-from-hardware move), distinctColours unresolved (def3 alone sits 5.6x
  the null from its own siblings — the within-arm spread swamps the statistic,
  §6bq's shape).
* **The packed tail: correctness-justified, era-unresolved, direction right.**
  ntl (no tail) reads slightly SHARPER (5.537-5.693) than def — which is the
  aliasing-scores-as-gradient effect: without deep mips the far field samples
  too-detailed levels and shimmers. Counters, identical across all three def
  runs: **4,463-4,475 tail levels TAKEN, 73 REJECTED by the divergence guard,
  302 chains still ending mostly-empty** — the rule uploads what it fits and the
  guards still refuse what it does not. One registered clause was MIS-SPECIFIED
  and is retracted as written: "the REJECTED class shrinks" assumed part 39's
  254 rejections happened at tail levels; the old walk never scored a tail level
  (ntl REJECTED = 0 on this route), so the def arm's 73 is a NEW count of
  square-rule misfits, not a residue of an old one.
* The failed global-aniso arm's numbers, for the record: sharpness 4.351 vs
  5.313/5.389 nulls (-19% — the metric flagged the speckle but as a DROP, which
  without the eyeball reads as "ineffective" rather than "broke the shadow
  term").

**Owed next**: the operator's far-field look (their mandate started this part),
kickoff items 3 (00i pairing on the 81-capture walk), 4 (A2M dither at
distance), 5 (clamp modes / edge fringes), and 1b's clamp half.

## §6bu — part 41, second session: the far-field softness is the DoF COMPOSITE, the scene underneath is SHARP, and the hardware contradiction that stops a fix tonight

The operator's verdict on the part-41 defaults: aniso works, but "everything that
isn't ground" still degrades hard with distance. Twenty F9 captures with poses and
censuses landed in `~/DR2CZ-troubleshooting/part41-operator/default/`. This section
is the anatomy of what they showed.

### The pipeline bisection: the softness enters at the post chain

The F9 capture's own resolve snapshots bracket it exactly (frame 2904, mid-street):

* `1439B000` (the resolved scene) and `0684B000` (scene-era, pre-exposure): **SHARP
  AT EVERY DISTANCE** — the CASINO sign legible, distant poles crisp. The part-41
  samplers and mip work are vindicated; nothing upstream of post is losing detail.
* `00E48000` (the post composite, = the presented frame minus HUD): the mid/far
  field is uniformly soft. The blur enters between those two surfaces.

### The DoF chain, read from our own census + the translated microcode

Census draws 6783/6784/6813 of frame 2904 (`capture_f2904.census`), shaders
translated from the ucode bank:

* draw 6783 `ps_e7066eaabb79a885` (scene + depth-as-fmt22):
  `CoC = saturate(pc49.z * (rcp(z*pc86.z + pc86.w) * (z*pc85.z + pc85.w) - pc49.x))`
  written to the 640x360 downsample's ALPHA.
* draw 6784 `ps_166bbb9722e9c3ca` (downsample + blurred + depth-as-FMT6): a
  poisson-disc gather, depth-edge-aware via EIGHT reads of the depth surface
  DECLARED AS 8_8_8_8 — the 360 byte-split trick, `.z` channel = one byte of the
  packed 24-bit depth. **Our runtime serves that fetch the float-depth image**
  (the "231 colour fetches served by a DEPTH resolve snapshot" class part 36
  counted and left unclaimed — it has a victim now).
* draw 6813 `ps_8375f611aba84bc4`: `oC0 = lerp(sceneFullRes, blur640, blur.a)` —
  the presented image. The whole question is the alpha.

Hardware's constants for the prepass, stable across R4 traces 01/04/08
(`xtr_draw_constants.py`): pc49=(0,50,0.02,0), pc85=(0,0,0,1),
pc86=(0,0,-9.999001,10) — i.e. `viewZ = 1/(10 - 9.999*z)` (a near=0.1/far~1001
linearization) and `CoC = saturate(viewZ/50)`.

**Our depth input to that formula is measured**: the depth snapshot dumps print raw
24-bit ranges 0.83..1.00 (operator log), and the snapshot IS scene depth (the
street silhouettes are visible in the dump). Perspective-bunched z near 1.0 makes
`viewZ` explode: z=0.998 (about 50 m) gives CoC=1.0, z=0.99 (about 10 m) gives 0.2.
**That curve is exactly the operator's complaint** — near sharp, everything else
increasingly mush, ground rescued separately by aniso.

### A retraction, in place, before it cost anything

An earlier read of "hardware's depth bytes" from R4 trace 01 (memory record at
`0A978000`) measured values 0.0-0.35 and spawned three encoding theories. Rendered
as a PICTURE (the two-minute check, gotcha 287), that record is the PREVIOUS
FRAME'S COMPOSITED SCENE in greyscale — "20 KILLED" is legible. Gotcha 280 in a
second disguise: the DoF reads depth resolved INSIDE the frame, which a trace
cannot carry, and the pre-frame record at that address was a colour aliasing. All
conclusions drawn from those bytes are void. What survives from hardware: the
CONSTANTS (above), `RB_DEPTH_INFO` bit16=0 on all 5,831 draws (the depth surfaces
are **D24S8 UNORM** — no 20e4-float domain gap; also `xenos.h`'s comment for that
bit is wrong and says "16-bit vs 24_8"), `PA_CL_VPORT_ZSCALE/ZOFFSET = 1/0 identity
on every draw, and the depth resolve running every frame (8 per frame to the
right-tile-offset address, the §6be idiom).

### THE OPEN CONTRADICTION, stated so nobody ships a half-derived fix

Same shader, same constants, same unorm depth domain, identity viewport on both
sides — the naive math says HARDWARE should also blur at 40-60 m (CoC 0.8-1.0),
and hardware's R4 PNGs show a legible store sign at that range. So a compensating
term exists on hardware that we have not located. The candidates, in checking
order, all with tools already proven:

1. **The gather pass's OTHER constants** (pc48.w, pc82.x, pc96..101, pc252..255)
   — the final alpha is `saturate(max(saturate(reconstructedZ*pc82.x), sqrt(...)*pc48.w))`,
   so a zero in pc82.x/pc48.w kills the blur REGARDLESS of the prepass CoC.
   Read hardware's via `xtr_draw_constants.py --ps 166bbb9722e9c3ca`; read OURS at
   the same draw (instrument needed, or CZ_VK_PSBIND extension).
2. **The fmt6 byte-trick depth reads** (8 of them in the gather) — on hardware
   they return packed depth BYTES; we serve float-depth-in-R, so every
   depth-edge weight in the gather is garbage on our side. This alone could be
   the whole difference if the weights normally SUPPRESS blur.
3. The alpha's journey from 6784's output to 6813's s1 (`148B0000`) — which pass
   writes it last, on both sides.

### What this is NOT

Not the mip chain, not the samplers, not the textures, not streaming, not item
00i's flat panels (a separate binding question this class was masking) — the
scene surface has full detail at every distance. One fix at the post chain
recovers the whole far field at once.

## §6bv — part 42: the flat-texture class is a PROMOTION-DENIAL defect measured four ways, and the DoF "hardware contradiction" mostly dissolves

Part 42 ran the part-42 kickoff in its stated order: item 00i first, the DoF
composite second. Both moved decisively, and each one's answer reshaped the other.

### 00i: the complaint verified on the SCENE surface, then censused two-sided

The kickoff's warning was to judge flatness on the scene snapshot, not the
presented frame. Done first, and the complaint is REAL AND PRE-POST-CHAIN:
in the operator's `capture_003053` the two-story building's walls are flat
tan/olive panels with NO surface pattern **in `1439B000` itself** — crisp
edges, legible PAWN sign, blank albedo — while the zombies and props beside it
carry full detail. So the flat class is not the DoF blur, exactly as the
operator said.

The census then named the mechanism without needing the draw-ID pointer:

* In `capture_f3053`, **53 draws of ≥200 verts (up to 3,575 verts) bind an
  8×8 or 16×16 texture as s0** on the world shader `ps_34524bb64374d20e` —
  and all 53 come from just TWO texture addresses (`0ED9A000` 8×8,
  `11DB9000`). The flat buildings are a handful of SHARED WORLD ATLASES
  stuck at thumbnail quality, not a zone-wide state and not a per-object
  lottery.
* Two-sided, at the shader level: across **all eight R4 hardware traces**
  (the same street, eight distances, 40,703 draws) the same shader binds a
  ≤16×16 s0 on a ≥200-vert draw **zero times** — its s0 population is
  512×512 (586 draws in trace 01 alone) down to 256×64. On OUR side, 44 of
  the 81-capture walk's frames and 14 of part 41's 20 carry the tiny-bound
  class (912 and 516 draws). Hardware's smallest-for-this-shader is 8×32,
  twice, on tiny meshes.
* Part 39's close-up exoneration (512×512 md5-identical to hardware) still
  stands, so promotion WORKS near. The defect is WHO GETS PROMOTED.

### The stand-still experiment: promotion never comes, so the rate class is dead

A fixed-camera, fixed-position headless run (DebugJump spawn, 20 F9s at 16 s
intervals) held the SAME three tiny-bound textures at thumbnail quality for
**13 consecutive censuses over ~2.3 minutes** — not one promotion, while the
walk evidence shows those same materials promote instantly on approach. A
rate-limited streamer (file-IO latency, a starved decompression thread — the
`KeSetBasePriorityThread` no-op that has been the named candidate since part
28) would fill in while the player stands still; a threshold or budget
decision does exactly this. **The priority no-op is refuted as the mechanism
of item 00i — do not build it for this item.** What decides promotion is a
DISTANCE/SCREEN-SIZE THRESHOLD or a POOL BUDGET whose input differs on our
runtime, and the next probe is the engine's own narration (`CZ_GUEST_DIAG`)
at the stand-still spot, plus the `cLODController` / `wait_for_tex_lod`
vocabulary in the image.

### 0u: the constants are exonerated to the digit, and the contradiction thins to two residues

Step 0/1 of the kickoff's plan, both sides:

* Hardware's gather constants (`xtr_draw_constants.py`, traces 01/04/08
  agree): pc48=(10,0.005,3,10), pc82=(2,0.2,0.001,0), pc96/97 the poisson
  weights, pc98..101 the 1/1280, 1/720 tap steps. **The blur is NOT
  constant-gated on hardware** — the kickoff's "if pc82.x or pc48.w is 0"
  branch is closed.
* OURS at the same draws — and the kickoff's "instrument needed" was wrong,
  **`CZ_VK_PSBIND_PC` has existed since part 31**: every recoverable register
  matches hardware to the printed digit (pc48/49/82/85/86/96/97). pc81 reads
  (−1,0,10.84,90.84) against hardware's (−1,0,12.89,92.89) — the same 80-wide
  far-blur band at a focus-dependent offset, not a defect.
* The alpha math, worked with real values on both sides:
  `alpha = saturate(2 × saturate((viewZ − pc81.z)/80))` — **0.98 at 50 m on
  ours, 0.93 on hardware.** The composite shows ~95% blur surface at range on
  BOTH platforms.
* The gather's tap radius collapses to ~zero on BOTH sides: it is
  `±(pc253.zw/pc48.x) × (s1.yx×pc252.w + pc252.z)` and s1 (`14A82000`)
  measures mean (0.502, 0.516) — neutral — on our side, with pc252/253/254
  carrying a static sevenths pattern. So blur640 ≈ the half-res downsample,
  which is what the surface dumps had already shown.
* **Hardware's own R4 PNGs are soft at range.** The distant HARDWARE-store
  sign and far crowd in `Big_buck_hardware_store_01.png` show exactly the
  soft far field the math predicts. §6bu's "its 40-60 m storefront is
  legible" was read against OUR flat-textured walls: high-contrast signage
  survives a 95% half-res lerp; a patternless wall has nothing to survive it.
  **The unlocated compensating term mostly does not exist — the far-field
  categorical difference was item 00i wearing item 0u's clothes.**

Two residues keep 0u open at reduced priority:

1. **The fmt6 byte-split depth serving** (the gather's 8 depth-edge taps) is
   still wrong-for-sure and still owed — it shapes the tap WEIGHTS at depth
   edges (halo suppression), not the field-wide blur.
2. **pc255.x at the gather is 0 on our side and unrecoverable from the
   trace** — the depth-compare threshold for those taps. With ours at 0
   every tap passes as "same layer". Unknown whether hardware differs.

### The 252..255 provenance question, asked and answered

`tools/xtr_draw_constants.py` now carries the LOAD SOURCE ADDRESS in every
UNRECOVERABLE marker (the natural next question of gotcha 263's honesty).
The gather's pc252..255 load from guest `032B6000` — and
`xtr_resolve_census.py` says that is **not a resolve destination** in any R4
trace, so the "the GPU computes the DoF block and we never write resolves
back" theory is REFUTED for this block: it is CPU-written, our PM4 load path
is correct (PhysToVa applied), and our register file's values are the guest's
own.

### The cache gate paid again, and an emitter bug fell out

The part-27 name-diff gate found **four dumped shaders absent from the live
cache** (`ps_57ba544a00f605a5`, `ps_73deb51969d6438d`, `vs_80b9611a08d9ae9a`,
`vs_cb7c5eb41489a916` — none ever reported missing by any run) and one dump
that FAILED translation: `vs_c8e86dffb37149dd`, eight vfetches whose
destination mask names no component (the "full" half of a full+mini pair,
present only for the slot/stride the minis inherit). XenosRecomp emitted
`r0. = XeVfetchDep(...).;` — DXC-invalid. Fixed in the emitter (skip the
assignment, keep the 0/1 lanes and the slot/stride recording); the shader
translates and the cache is **435**, dim-census gate clean. The failure had
been invisible because build_shader_spv.sh reports per-shader failures only
to whoever reads the batch output.

### §6bv addendum — the engine names the mechanism itself, and one flag theory dies in one live read

The `CZ_GUEST_DIAG` run at the stand-still spot narrates the whole thing:

```
LoadZoneCommonTextureSet : zone = 0, filename = COMMON_TEXTURE.tex
LoadZoneCommonTextureSet : zone = 1, filename = COMMON_TEXTURE_LOD.tex
LoadZoneCommonTextureSet : zone = 2, filename = COMMON_TEXTURE_LOD.tex
LoadZoneCommonTextureSet : zone = 3, filename = COMMON_TEXTURE_LOD.tex
LoadZoneCommonTextureSet : zone = 5, filename = COMMON_TEXTURE.tex
LoadZoneCommonTextureSet : zone = 7, filename = COMMON_TEXTURE_LOD.tex
LoadZoneCommonTextureSet : zone = 8, filename = COMMON_TEXTURE.tex
```

**The flat-texture class IS the per-zone COMMON_TEXTURE vs COMMON_TEXTURE_LOD
choice**: the spawn's zone reads the full set (which is why everything near
looks right), the street-building zones read the thumbnail set. It reconciles
every measurement in this part — a handful of shared atlases (the zone's
common set), no per-texture promotion while stationary (the choice is made at
zone load), promotion on approach (the zone's set upgrades when the player
gets close), and hardware fully-textured at range (ITS decision loads the
full set for the street zones). Part 28 recorded "3 full, 4 LOD" as *what
working looks like* — true locally, but nobody could compare the DECISION
against hardware then. Now the eight R4 traces do.

One candidate died the cheap way first: `ForceLODTexForStreamingWorld` is a
name-resolved config flag (the §-style registry at `sub_82773298`; the
pipelined store puts its byte at `0x82A57BD7`), and a `process_vm_readv` of
the RUNNING diag process reads the whole flag block `0x82A57BD0..DF` as
zeros. The flag is off on our side; it is not the mechanism. (Method note:
one live read beats an afternoon of theorizing about a flag — the same move
that closed part 36's AutoChuck question.)

Next (part 43): the branch that picks the filename. `gdis --find-uses` on the
`LoadZoneCommonTextureSet` format string names the caller; the inputs to its
LOD-or-full branch (zone distance? a budget? `cZone::UpdatePriorities`'s
`mForceLowLOD`?) are the item's remaining unknown, and each is one more live
read against the running process at the stand-still spot.

## §6bw — part 43: the zone texture-set decision is fully named, our inputs are live and correct, and the engine itself picks LOD at the spawn

Part 43 executed the part-43 kickoff's step 1-2 and the answer inverts the
item's framing: **the decision is not diverging on our side — the engine's own
math, run on live, verified-correct inputs, chooses `COMMON_TEXTURE_LOD.tex`
for zones 1/2/3/7 from the DebugJump spawn.** Every input was named, printed
at decision time, and re-evaluated live; the choice reproduces exactly across
three runs and a live re-computation.

### The chain, named function by function

* **`sub_82270870`** (only caller: `sub_82271550` ← `sub_82272128`, the
  streaming update; one call site in the whole image, plus the dispatch table)
  is "load zone N's common texture set". Its LOD branch at `82270C38..C70`:
  pick `COMMON_TEXTURE_LOD.tex` **iff `rec+0x90C` is set AND
  `sub_821C4F28([rec+0x910]) == 1`**; otherwise loop the full set
  (`COMMON_TEXTURE.tex` + any numbered `COMMON_TEXTURE<n>.tex`, count probed
  into `rec+0x938` by an exists-loop that patches the digit at position 14).
  `sub_8226F650` is the narrator ("<> LoadZoneCommonTextureSet…", format
  string at `0x82020910`); `sub_82269388` / request-handler `sub_8226F0E8`
  are the IO continuation, and `sub_822696D0` is the per-asset load-completion
  callback ("CallbackLoadRequest").
* **`rec+0x90C` ("LOD-capable")** is written at zone setup (`8222AF18` inside
  `sub_8222AD80`): set iff `COMMON_TEXTURE.tex`'s size satisfies
  `0 < size < 0x280000` AND `COMMON_TEXTURE_LOD.tex` exists in the zone
  archive. Zones whose full set is empty (zones 4/6/9 map to
  `prologue_z05/07/10.big`, all 0 bytes) get flag 0 — which is why part 42's
  narration had no lines for them, a cross-check that cost nothing.
* **`rec+0x910`** is the zone's VOLUME LIST (`sub_8217F848(this+0x83CC)`):
  count at `+0x120`, elements at `[+0x124]`, stride 0xD0. Per element: a
  sphere (x,y,z,r) at `+0x80`, a skip bit (u32 `+0x90` bit 0 — set forces
  "near", i.e. FULL), a threshold float at `+0xA8`.
* **`sub_821C4F28`** returns 1 (→ LOD) iff EVERY volume is far, caching the
  verdict at `volObj+0x22C`. **`sub_82175040`** is the per-volume vote:
  `dist = |cam − sphere.xyz| − 0.01 − sphere.r` (`sub_821551C0`, camera-point
  radius 0.01 from `0x821027F4`), far iff `dist > threshold`, with the
  threshold boosted `× table2[level]` when it is under `table1[level]`
  (`sub_82373DC0/82373E00`, tables at `0x82042C18`/`0x82042D68`, level from
  `[g+0x34F5C]`, debug-override byte `0x82A58623` shipped 0). The camera is
  `[0x82A46294]+0x40..0x48`; the force byte `0x82A57BD7`
  (`ForceLODTexForStreamingWorld`) short-circuits every vote to "far".
* **`COMMON_TEXTURE_LOD.tex` IS the thumbnail set by design**: zone 1's LOD
  file is 27,734 bytes against a 1,297,584-byte full set (z03: 5,060 vs
  231,788). The 8×8/16×16 atlases of part 42 are not a broken read of a good
  file; they are the file.

### The instrument pair, and what it measured

`CZ_ZONE_TEX_PROBE=1` (new, `runtime/cpu/guest_probe.cpp`) prints every input
above on each `sub_82270870` entry plus its own prediction of the branch;
`tools/zone_lod_live.py` (new) re-evaluates the decision against a RUNNING
process's camera via `process_vm_readv`. The probe's predictions matched part
42's narration line for line — menu zone 0 FULL (+ its `COMMON_TEXTURE1.tex`
second set), level zones 0/5/8 FULL, 1/2/3/7 LOD — so the model of the branch
is confirmed against the engine's own mouth, not just read from disassembly.

Measured, DebugJump stand-still route, three runs:

* The level-load burst runs at ~46.3 s with the camera **already at the spawn**
  `(-106.09, 6.57, -115.89)` — not stale, not the menu's `(2.57, 0.00, 6.88)`,
  which the menu-era decisions correctly used. Level = 14, whose boost table
  entry is 9999/×1.0 (no boost); force = 0; every skip bit = 0.
* Zones 1/2/3/7 are all-far by real margins: nearest volume 31–107 m beyond
  its threshold (zone 1: +107.4, zone 2: +41.3, zone 3: +75.7, zone 7: +31.0).
  Zones 0/5/8 have near volumes (min margins −63.5/−18.2/−55.3) → FULL.
* A live re-evaluation while standing (same spot, minutes later) returns the
  identical verdict per zone. The camera word is live (it tracked the
  menu→spawn transition) and simply does not move while standing.

### What this does to item 00i

The defect is NOT "our runtime computes the decision wrong": inputs live,
constants exact, math reproduced, choice deterministic. What remains is a
STATE question against hardware, and the R4 capture notes reframe it:
**the eight R4 traces are a standing sweep AT Big Buck taken after the
operator walked there** — a WARM session, in which every zone whose volumes
the player had come near was already promoted to full. A fresh-jump state
(ours) and a warm walked state (theirs) are not matched arms (gotcha 50's
shape, in streaming-state form).

Two sub-questions now separate the item:

1. **Does OUR runtime re-run the decision on approach** (zone reload as the
   player nears its volumes)? Measured this part with the probe on a 10-minute
   EXPLORER roam — see below.
2. **What does HARDWARE narrate at the matched state** (fresh DebugJump,
   stand still)? Not self-servable: needs one Xenia run with the part-28 diag
   bytes patched (`0x829EC974=0`, `0x82AC3EAD=1`) and the seven
   `LoadZoneCommonTextureSet` lines from its log. If hardware also says
   1/2/3/7 = LOD, item 00i's "flat at range from a fresh jump" is the engine's
   own design and the comparison collapses; if it says FULL, a dynamic input
   (the skip bits, the volume data, or a promotion path we never run) is the
   remaining suspect list, in that order.

### A gotcha paid for on the way

The probe's first version printed `rec+0x69C` — a directory OBJECT — as `%s`,
salting the log with NULs; plain `grep` then treated the whole file as binary
and reported the probe absent, and TWO runs were misread as "the hook never
fired" (an hour spent verifying link-time symbol override that was never
broken — the machine-level call target check at least settled that the alias
seam handles C++-mangled weak aliases fine). `grep -a` / `tr -d '\0'`
recovered 263 sitting probe lines. Gotcha 25's self-made form: when an
instrument you wrote reports zero through a text pipeline, check what bytes
the instrument itself put in the file before concluding anything.

### The promotion question, measured as far as it can be from this side

* A 9.5-minute EXPLORER roam with the probe live produced **ZERO re-decisions
  after the level-load burst** — but `tools/zone_lod_watch.py` (new; samples a
  live process every N s: position, the per-zone state table at
  `this+0x841C`, and the would-be verdict per zone) shows the roam stayed in a
  ~60 m pocket around the spawn and NEVER entered any LOD zone's threshold.
  So "no promotion on approach" is NOT proven — the arm never engaged
  (gotcha 151). A no-AutoChuck run does not move at all (the title's MISSION
  MASTER state exists but does not drive Chuck headlessly), so the directed
  approach test still needs either an operator or a steered stick recipe
  (zone 7's nearest volumes sit ~west of the spawn at x≈−180..−206,
  z≈−115..−127, 31–47 m beyond threshold).
* The state table semantics fell out of the writers: entry+0 is the state
  (`sub_82271550` loads the first zone found in state 0; the loader leaves
  2 or 3; the load-completion callback `sub_822696D0` writes 3), entry+4 is
  the has-texture-set flag (cleared at setup when the full file is 0 bytes —
  zones 4/6/9). **No writer of state 0 outside initialization was found**, so
  as far as static reading goes the texture-set choice is once-per-zone-load;
  per-volume GEOMETRY streaming (`sub_8226A0B8`, 0xD0-stride volume list) is
  a separate system underneath it.
* **The ordering hypothesis is dead**: recomputing the decision from the menu
  camera `(2.57, 0.00, 6.88)` and from the origin makes MORE zones LOD
  (5 and 8 join), not fewer. No camera position outside town yields
  hardware's all-full street, so "hardware decided before/after the teleport"
  explains nothing.

### Where item 00i stands after part 43

Our side is exonerated up to its inputs: the branch, its constants, its
tables, its camera and its volume data all verified live, and the engine
itself picks LOD at the spawn. Hardware's all-full R4 street is either a warm
session state (zone sets loaded/reloaded when the player was near) or a
dynamic input we cannot see from here (the skip bits are first on that list).
**The discriminating capture is R5** (`docs/xenia-capture-requests.md`): one
fresh-DebugJump stand-still F4 at the spawn — no patching, one press. Until
it lands, do NOT build a fix: every candidate (force the full set, widen
thresholds, fake the skip bits) would be faking the decision, the exact thing
gotcha 5 and the part-43 kickoff prohibit.

### §6bw addendum — the R4/R3 traces DID hold the answer, and one more jump proved the mechanism end to end

The operator pushed back on the R5 request — the existing captures should
answer it — and they were right. Two measurements, both from data already on
disk plus one five-minute run:

1. **Hardware's camera positions are recoverable from the traces**, and at
   every one of them the fresh decision math says LOD for zones 1/2/3/7.
   The ground draw (`vs_36eef2c94b4a065c`/`ps_ad65b98593f95926`, 25,234
   verts, present in every trace) carries the view state in `vc12..vc14`,
   whose .w column is the eye directly in (y, x, z) order — verified by its
   coherence: y is the constant walk height 3.2, and the eight R4 positions
   trace a ~35 m eastward approach `(-95,-113) → (-63,-129)` exactly matching
   the far-to-close walk the operator described. (The first solve attempted
   `-Rᵀt` on those rows and produced out-of-world eyes for five of eight
   traces — the .w-direct reading is the one that survives the invariant
   check. The all-eight-identical `vc8..10` column is a model matrix, not the
   camera.) R3's tanker and green-building traces solve into the same pocket.
   Per-zone margins at all ten hardware positions: zones 1/2/3/7 all-far by
   **+21 to +136 m**. Yet the traces bind zero thumbnail atlases and render
   fully textured — **hardware's zone texture state cannot be the product of
   a fresh batch decision at any captured position.**
2. **The jump-elsewhere positive control**: `DOWN,DOWN` on the DebugJump
   screen spawns at `(-271.7, 3.3, -64.0)` (level id 18, 11 zones) and the
   burst decisions reshuffle exactly as position dictates — zone 1 flips to
   FULL (75 near volumes), zones 5/8 flip to LOD. The engine's decision
   follows the load-time camera everywhere; nothing about our execution is
   position-dependent-broken.

Consequence: the divergence against hardware is REAL and is now fully
characterized as a STATE question — hardware's zones were loaded (or
re-loaded) with the player near them; ours are batch-loaded once from the
jump spot and never touched again (states pinned at 3 through every roam;
zero re-decisions in 9.5 min + a directed attempt — synthetic LSUP does not
move Chuck at the jump spawn, only a real pad does, so the approach test
remains open). The one-sentence question that replaces capture R5: **how did
the operator enter the level for the R4 session?** DebugJump-then-walk →
hardware re-decides zones on approach and our missing reload trigger is the
defect; normal play → both runtimes faithful, and the flat-at-range IS the
batch-load state.

Also reported by the operator this session, filed not chased: **ambient
occlusion only visible very close to buildings/objects** — plausibly the
same far-LOD mesh switch (LOD shells sampling the common set carry no baked
AO), so it may ride along with this item's fix. Lower priority than 00i.

### §6bw second addendum — normal play batch-loads too, the mask cannot unload main zones, and the divergence narrows to "what re-runs the decision on hardware mid-session"

The operator's second correction: their flat-building captures came from NORMAL
PLAY on our runtime (and their R4 Xenia session was normal play — no debug menu
there). Measured response, all headless:

* **Our normal play batch-loads exactly like the jump.** The stick recipe
  (prologue → safehouse → Still Creek) runs ONE 11-zone burst at ~43 s with
  the camera at the case start `(-266.35, 3.24, -31.75)` — zones 2/5/7/8
  decide LOD there — and NO re-decision for the rest of a 10-minute run that
  reached the outdoor world (8,975 draws). The operator's normal-play flat
  street is reproduced and explained.
* **Loading the save is the same state**: `START,DOWN,A,...` reads
  `DR2P000.DSF` and the burst runs at the SAME safehouse camera — Case Zero's
  save point is the safehouse, so "hardware loaded a save mid-town" is dead.
* **The zone-interest mask cannot reload the street zones.** The enqueuer
  (`sub_8226CB60`, called every streaming update before the queue processor)
  enqueues on mask-bit set when `zoneInfo+0x6C == 0` and UNLOADS
  (`state 3 → 2`, `sub_8226B818`) on bit-clear — the reload path exists —
  but zones 0-8 carry ALL-ONES membership (`zoneInfo+0x68 = 0xFFFFFFFF`),
  so no mask value can ever unload them within a level session. Only
  special zones (9: bit0, 10: bit1) toggle. Live-watched over a roam
  (`zone_lod_watch.py --mask`): mask words `0000000e 00000001 ffff0001
  00010001` static throughout, all states pinned at 3. (Word 0 of that
  block IS the "level id" the boost getters index — 14 jump / 17 menu / 18
  normal play; table entries 14 AND 18 both give ×1.0, so the boost is a
  non-factor on every route. The probe's 16-entry cap should read 24.)
* Consequence: hardware's full street CANNOT be explained by any single
  fresh batch decision (safehouse math also says street = LOD) nor by
  mask-driven reload. **Something re-ran the zone loads on hardware
  mid-session with the player elsewhere.** Remaining candidates, in order:
  (a) full level reloads at natural CASE TRANSITIONS (each case start
  re-decides everything from the case spawn — the DebugJump entries are
  exactly these spawns, and entry 2 lands at `(-271.7, -64)` where zone 1
  flips full); (b) mission-script zone loads (`immediately = 1` — every
  narration we have ever seen is `immediately = 0`); (c) an unfound path.
* **The discriminating experiment is an operator session on OUR runtime**:
  natural play from new game through the first case transition and into
  town, with `CZ_ZONE_TEX_PROBE=1` — the probe prints every re-decision
  with its camera. Same code on both platforms: if transitions re-decide
  on ours, they re-decide on hardware, and the transition position explains
  hardware's full street (and the FIX becomes making our sessions carry
  the same state, or nothing at all); if nothing re-decides even across a
  case transition, candidate (b)/(c) is next.

### §6bw third addendum — the R4 session was a LOADED SAVE at the tanker, and even that position's math says LOD: the save's carried state is the last input standing

The operator: the R4 Xenia session loaded a save made at the TANKER save
point (the one proposed after the case 0-1 → 0-2 transition), from their
R1-era normal playthrough — so the session was Case 0-2 from its first
frame. The tanker is `(-118, 3.2, -105)` (the R3 tanker trace's own camera),
and the fresh-decision math THERE still says LOD for zones 1/2/3/7
(+21..+77 m margins). An identical load on our side would produce the flat
street. Therefore the save (or the case state it restores) must change an
input of the decision. The candidate that fits the shape exactly: the
per-volume skip-bit (`elem+0x90` bit 0 — set forces "near" → FULL SET
regardless of distance; all zero in every fresh state we have ever read).
Case/mission progression setting those bits (areas activated by the story),
saved and restored, would reproduce hardware's full-everywhere and our
flat-everywhere from the same code. A static hunt for the bit's writer is
noisy (1,293 stores to +0x90-shaped offsets); the discriminating experiment
is to LOAD THE OPERATOR'S XENIA SAVE on our runtime with the probe: if the
burst flips the street zones to FULL, the carrier is proven and nothing in
the runtime is broken — then one live diff of the volume records between a
fresh and a save-loaded process names the exact field. The save is an
XContent package in Xenia's content tree for title 58410A8D;
`tools/extract_stfs.py` unpacks it and the DSF goes to
`assets/save/DR2P000.DSF/DR2P000.DSF`.

### §6bw fourth addendum — THE DECISION IS EXONERATED AS THE CAUSE: the flat class exists in the MAIN MENU, where the decision chose FULL and the full file loaded

The operator: the flat class shows even on the main menu's gas sign, on every
boot. Verified headlessly in one run (synthetic F9 at the title screen,
`CZ_CAPTURE_KEY`): the menu frame carries **56 draws of ≥200 verts binding a
≤16×16 s0**, twelve of them the world shader `ps_34524bb64374d20e` reading an
**8×8 atlas at `0D875000`** on meshes up to 2,131 verts — in the ONE state
where every link of the decision chain is known good: zone 0 decided FULL
(near giant volumes, probe-confirmed), `COMMON_TEXTURE.tex` (2.4 MB) and
`COMMON_TEXTURE1.tex` both narrated as loading, and the "Avoid loading TOO
big file(s)" path read and found benign (sizes over 0x240000 just pump the
decompressor `size/0x240000` extra times — hardware runs the same line).

**Item 00i's cause is therefore DOWNSTREAM of the texture-set choice: the
set's payload never reaches the texture descriptors.** The atlases exist at
8×8/16×16 — plausibly their creation-time minimal state — and the apply/
commit step that should promote each descriptor to its full extent after the
set file decompresses never lands. The LOD-vs-full decision work (this
part) stands as correct reverse-engineering, and the position analysis of
the R4/R3 traces stands as proof hardware holds full-size atlases — but the
decision only ever selected WHICH file fails to apply. Everything measured
(fresh/jump/normal-play/save states all flat; hardware full everywhere;
"promotion works up close" = the separate per-object texture system) is
consistent with a single broken set-apply on our side.

Part 44's hunt, in order: (1) the set-load pipeline end to end on ours —
the `NtReadFile` extents against the zone archive (does the whole 2.4 MB
arrive), the decompress pump `sub_82177358` (probe: calls, completion), the
asset-type-3 completion (`CallbackLoadRequest ... mStatus = 2` —
`sub_82269388`) and whatever walks the decompressed set and REWRITES the
material fetch descriptors; (2) find where it silently dies; (3) the fix at
that input. The B1 boot-title hardware stream can supply the menu-frame
oracle (same draws, hardware's s0 extents) if needed.

Also from the operator session (natural play, case 0-1): the probe shows the
same two bursts (menu + safehouse at 138 s) and nothing after — and the
session ended in a FREEZE (not a fault: signal-15 dump, input/audio still
pumping) right after grabbing a sledgehammer outside the safehouse. Filed:
next operator session should carry `CZ_WAIT_TRACE=1` so a freeze names its
wait. Log preserved at
`~/DR2CZ-troubleshooting/part43-operator-zone-session.log`.

## §6bx — part 44: the set-apply pipeline is fully named, and the MENU HALF OF THE REFRAME IS RETRACTED — hardware's menu binds the identical distribution, tiny draws included

Part 44 executed the part-44 kickoff's hunt ("the decompressed set payload never
rewrites the material texture descriptors") and the hunt's premise failed its
control. Two results, in order of importance:

### 1. The fourth addendum's menu evidence is RETRACTED: our menu frame equals hardware's, bind for bind

The B1 boot→title hardware stream — named by the kickoff itself as "the oracle
for the same draws if needed" and never actually read — was censused with
`tools/xtr_draw_bindings.py` (min-verts 200) and compared against a fresh
synthetic-F9 census of our own menu frame. The `ps_34524bb64374d20e` s0
distribution, title era:

| s0 extent | B1 draws (hardware) | ours |
|---|---|---|
| 256×64 | 148 | 148 |
| 512×256 | 41 | 41 |
| 128×128 | 37 | 37 |
| **8×8** | **31** | **31** |
| 512×256 (2nd tex) | 27 | 27 |
| 128×128 (2nd) | 20 | 20 |
| 256×64 (2nd) | 20 | 20 |
| 32×128 | 17 | 17 |
| 512×512 | 12 | 12 |
| (10 more rows) | = | = |

Identical, entry for entry, including the tiny ones — and the tiny-on-big
census (≥200 verts, s0 ≤16×16) reads 56 on ours against ~60 in hardware's last
3,000 draws. The addendum's "8×8 atlas at 0D875000 on meshes up to 2,131
verts" is hardware's 8×8 at 11609000 on the same 2,131-vert mesh (B1 draw
66520). The texture is `flat_color_gray_cm.bct` — 346 bytes on disc, one DXT
block plus mips: a flat gray genuinely painted on large meshes by design. The
menu was never broken, the "56 tiny-on-big menu draws" were always hardware's
own numbers, and part 43's final reframe ("the defect is the set APPLY; the
menu proves it") dissolves. The full-size atlases on our menu (512×512 at
10443000 etc.) show the set payload DOES reach the descriptors on our runtime.
Gotcha 13's shape again, in its sharpest form yet: an absolute census was read
as a defect without running the free control sitting on disk.

### 2. The texture level machine, named function by function (kept — this is the reverse-engineering the kickoff asked for, with the apply question answered under it)

The `.tex` set container: header `06 05 04 03`, entry count at +0xC (LE
words — the tool that wrote it was a PC tool; the guest parses it through its
own readers), 0x1C-stride records {name-offset, hash32, payload size, format
word, payload offset}, name table of `.bct` basenames, per-entry payloads that
begin with the record's format word big-endian. `prologue_menu/prologue_z01`:
COMMON_TEXTURE.tex = 18 entries / 2.4 MB, COMMON_TEXTURE1.tex = 17.

The machine (all guest code, verified live with `CZ_SET_APPLY_PROBE=1`):

* **Name key**: CRC-shaped hash over the extensionless basename
  (`sub_8276C768`, init 0x20225, poly 0xEDB88320, LSB-first bits per byte,
  sign-extended chars; the copy helper `sub_8279EDE0` strips path and
  extension). `concrete_dirty_rooftop_beige_cm` → 032A3240.
* **The texture DB** (`[[0x82A46294]]+0x2C`): name-hash map (`sub_8243FE40` →
  index), entries at [db+0x28], stride 0x4C: name at +4, fs asset-slot index
  at +0x38, per-level refcounts at +0x3C (level 0) / +0x40 (level 1),
  **current level at +0x44**, flag byte at +0x48. `sub_82178DD8` = desired
  level from refcounts (any level-0 ref dominates: full wins), −1 = no change.
  Level 0 = the FULL payload (zone COMMON_TEXTURE.tex), level 1 = the
  thumbnail (model-embedded containers / COMMON_TEXTURE_LOD.tex).
* **Container registry** (`fs = [0x82AC4878]`, slots at fs+0xD70, stride
  0x6C): `sub_827B8A58` registers a loaded set container and pre-creates
  per-entry ops; `sub_82268238` (vtable-dispatched, reached via the
  `sub_822692B8` thunk) is "bind container at level L": pass 1 looks up every
  entry name and refs hits at L (`sub_8222CC80` with idx), pass 2 CREATES
  misses (`sub_8222CC80` idx=−1: name, +0x38 = container-entry op, +0x44 = L).
  When any entry's current level ≠ desired, its tail enqueues a catch-up item
  (`sub_82206910` → db+0x70 queue → type-8 dispatcher request → `sub_82268A10`
  walks the container, schedules payloads, writes +0x44 = item level).
* **Set completion** (`sub_82269388`, from `CallbackLoadRequest` type 3):
  walks the container with `sub_82268840` — every entry found in the DB with
  +0x44 ≠ 0 (i.e. currently at thumbnail level) gets its payload read
  scheduled (`sub_82782480` → `sub_827D1BC0` → `sub_827CF4C0` →
  `sub_827B8F38`) and +0x44 set to 0 — then registers the container and
  tail-binds it at **level = the zone's cached LOD verdict** (`[volObj+0x22C]`,
  0 = full). `sub_8226C800` re-walks a container when a type-1 asset completes
  with `sub_821788F0(file) == 0`.
* **Payload reads verified live**: 39 walk-scheduled ops in one run, each
  followed by its archive read in the file trace. The apply path is healthy on
  our runtime, which is what the identical menu distribution independently
  proves.

The round-2 probe timeline that looked like a defect — zone-set walk misses
all 18 names, the completion resolve creates them at level 0, model resolves
then ref them at level 1 forever, no payload ever scheduled through the WALK —
is how the machine is supposed to look on that ordering: an entry created at
level 0 by the container bind takes its data from the container at object
creation (the menu's full 512×512 binds are exactly those entries), so nothing
needed promoting. The walk-promote path serves the opposite ordering (object
exists at thumbnail level when the full set arrives). Do not re-read that
timeline as a trap; part 44 did for several hours.

### Where item 00i stands after this

The flat-at-range class OUTDOORS remains the item, and the open question is
unchanged from §6bw's addenda but now has a decisive unread oracle: **B2 is a
FRESH hardware session (boot → new game → walk into Still Creek), captured at
day one and 7.95 GiB** — its timeline separates "fresh hardware binds
thumbnails at range exactly like ours (faithful; R4-warm's full street is
session state)" from "hardware promotes on approach mid-session (a reload
trigger we never run)". The tiny-on-big census over B2's draw timeline is the
measurement; part 44 runs it next (in flight as this is written).

Instruments added (all free when off): `CZ_SET_APPLY_PROBE=1` — prints every
`sub_82268840`/`sub_82268A10` walk (container, entry count), every DB lookup
inside a walk (hash → idx, +0x44/refcounts/dest), every registration
(`sub_8222CC80`: hash, level, packed request word), every container bind
(`sub_82268238`: slot, level, caller), and every walk-scheduled payload op
(`sub_827D1BC0`, with entry name). The menu-set hash list is compiled in so
those 35 names print from ANY caller.

### §6bx addendum — B2 DECIDES ITEM 00i: a fresh hardware session binds the thumbnail class at the same rate, and it never promotes. CLOSED AS FAITHFUL

The discriminator named above ran the same day. `tools/xtr_draw_bindings.py`
could not survive the 8.5 GiB B2 trace (three OOM kills: 43.9 GB RSS with
every memory record retained, then again with latest-wins-per-base, then the
accumulated draw list), so part 44 built a validated lean variant
(`~/DR2CZ-troubleshooting/part44/` carries it with the artifacts): a rolling
8,192-record memory window (register loads read records shortly after they
land and Xenia re-records changed memory, so register state survives — proven
by byte-identical CSV output against the unmodified tool on the whole of B1),
plus rows streamed to the CSV at draw time. One truncated run (ENOSPC on the
/tmp tmpfs at 7.04 GB — the CSV now writes to /home) plus one clean run.

The timeline, 24 buckets over the session (boot → New Game → skipped
cutscenes → safehouse → the Still Creek walk; ≥200-vert draws with an s0):

* Buckets 0–3 (title/menu): tiny-on-big **5.94–6.05%** — B1's title-era
  numbers reproduced from a different capture, the retraction's control
  confirmed in passing.
* Buckets 4–11 (cutscene/safehouse): 1.0–2.0%.
* Buckets 12–23 (the town): **1.96–3.03% tiny-on-big in every bucket, with
  the world shader `ps_34524bb64374d20e` itself binding an 8×8 on 1,300–3,900
  draws per bucket** — the operator's flat-buildings-at-range class, on
  HARDWARE, on a FRESH session, all the way through the walk. Our own
  fresh-session frames read 0.2–4.9% on the same census (part-41/42 operator
  captures re-censused; peak 94 tiny of 1,904 big at the Big Buck approach).
* Every long-lived tiny texture persists from first bind to the END of the
  session (a 4×4 alive from draw 67k to 7.65M): **no promotion wave exists on
  a fresh session** — outcome (b) refuted directly.

**Item 00i is closed as a state-comparison artifact.** The R4 "fully textured
street at every distance" that anchored parts 42–44 was a warm session from a
loaded Case 0-2 save; fresh-vs-fresh, hardware shows the operator's complaint
exactly as our renderer does. The flat-at-range look IS Dead Rising 2: Case
Zero on a fresh session: far zones stream their `COMMON_TEXTURE_LOD.tex`
thumbnail sets by the (part-43-verified) distance decision, and the part-44
level machine applies them correctly on both platforms. What the save carries
that makes a warm session all-full remains UNNAMED (skip bits stay the leading
candidate) — but it is a curiosity about the save format now, not a defect:
the one follow-up that could name it is loading the operator's tanker save on
our runtime with `CZ_SET_APPLY_PROBE=1`.

Operator guidance that falls out: like-for-like comparisons only — a fresh
DebugJump/New Game on ours against a fresh session on Xenia, or the same save
on both. And the part-43 addendum's AO-only-up-close observation plausibly
rides the same design (LOD shells at range), to be re-checked only if it
survives a like-for-like look.

### §6bx second addendum — REOPENED THE SAME DAY: the operator's matched capture shows the flat buildings bind FULL-SIZE textures; the flat-at-range look was never the thumbnail class

The operator rejected the closure and recreated the R4 walk on our renderer
(8 F9 captures, `~/DR2CZ-troubleshooting/part44-operator/`). Their capture 4
against R4_04's frame-locked PNG: hardware's Big Buck has legible siding,
brick and the HARDWARE sign; ours has flat single-color building faces. And
the census of that same capture kills the attribution this whole item has
carried since part 42: **8 tiny-on-big draws of 1,760** — the flat buildings
are binding normal-sized textures. The B2 "2–3% tiny-on-big" rate argument
could not place those binds on building-scale surfaces and never could have
(rate is not prominence; the closure over-reached exactly there). The menu
retraction (first half of §6bx) is unaffected — that comparison was
bind-for-bind at one matched scene and remains correct.

The mechanism now under operator A/B: **mip sampling at range.** A distant
facade samples its texture's deepest mip levels; since part 41 those levels
come from the packed-mip-tail decode, which uploads ~4,468 tail levels per
outdoor run including "302 mostly-empty" ones. A wrong or empty tail level
renders a surface as one flat color exactly at distance while it stays
detailed up close — which is the operator's ORIGINAL wording of this item
from part 28. Arms in flight: `CZ_VK_NO_MIP_TAIL=1` (tail off, chain on),
then `CZ_VK_NO_MIPS=1` (level 0 only), F9 at the matched Big Buck view.

### §6bx third addendum — the flat class is a MIP-SELECTION OVERSHOOT in the scene pass: ~2 octaves too deep everywhere, data and every LOD input verified matched

The reopened hunt ran the full elimination (all same-day, operator A/B plus
headless instruments):

* **The mechanism is mips**: `CZ_VK_NO_MIPS=1` restores building detail at the
  operator's matched Big Buck view; `CZ_VK_NO_MIP_TAIL=1` changes nothing.
  The flatness is present in the SCENE surface (pre-post-chain), confirming
  part 42's pre-post reading and clearing the DoF composite.
* **The mip DATA is correct.** Guest chains decode as clean downscales for
  every texture checked (fence 512×256 levels 1–3, foliage 256×256 levels
  1–3, the HARDWARE sign 128×512 level 2 — an eyeballed "wrong texture" there
  was refuted by a brute-force scorer: our offset wins at error 106 against
  ~2,300 for every alternative). The UPLOADED staging bytes equal the guest
  chain (CZ_VK_TEX_DUMP grew per-level dumping to prove it). An early
  live-dump "all-black level 4" was the gotcha-285 trap (dump taken after the
  operator moved on) plus my own wrong pitch flag — both retracted in place.
* **Every LOD input matches hardware.** tfetch instruction census over the
  435-shader bank: lod_bias 0 on 1,846 of 1,846, mip/aniso "use fetch
  constant" throughout. Fetch-constant census over R4_04's register state:
  lod_bias (dword4 bits 12..21) 0 on 56,111 of 56,111 bound constants;
  filters linear, aniso 3–4 on world textures — exactly what our decoder
  reads and our per-fetch samplers honor (samplerAnisotropy enabled, limit
  read; the aniso tint A/B shows an octave shift, so it ENGAGES).
* **`CZ_VK_MIP_TINT=1` (new instrument)** paints every uploaded chain level a
  solid code color (L1 red … L6 cyan): the spawn view shows truck beds at
  TWO METERS solid red (L1), vans at ten solid green (L2), and the spawn
  fence L2 where its texel density computes to LOD ≈ 0.5 — a ~2-octave
  overshoot on ordinary surfaces, which at 40–80 m compounds to L4–L6 =
  the flat-color class. E5 (round-1, FRESH hardware, case 0-1) shows the
  same street crisp: the divergence is real, ours, and now has a measured
  signature.

What is NOT yet named is the overshoot's cause: with data correct and every
bias/filter field matched, the remaining suspects are (a) the identity of the
wall textures' own fetch state at the matched view (still inferred, never
draw-ID'd — gotcha 302's warning stands), and (b) the derivative environment
of the scene pass (the MSAA window-scale interplay with UV interpolation is
the one global term that could scale every gradient; §6bf/§6bh). Part 45's
first move: CZ_VK_DRAW_ID at the matched view to name the wall draw, then
compute hardware's implied LOD for that exact draw from the R4 trace's vertex
streams and put a hard number on the octave delta.

### §6bx fourth addendum — the MENU IS THE LAB: the GAS ball is flat AT LEVEL 0, so the flat class splits into (at least) a non-mip layer/content divergence, and B1 carries the byte-level oracle

The mip tint run at the MAIN MENU (fixed camera, bind-distribution equal to
hardware, E3 as the exact hardware picture) splits the mechanism:

* **The GAS ball's body samples LEVEL 0 (untinted) and is still flat cream** —
  no rust, no red band — and `CZ_VK_NO_MIPS=1` leaves it identical, as it
  must. E3's ball is red-topped and rust-streaked with white-on-red GAS
  lettering; ours renders what looks like a different texture region or a
  missing layer. For THIS surface the defect is not selection and not the
  chain: it is level-0 content or a second material layer we never deliver.
  (The "GAS" letters are a separate decal sampling L2 with mips on; they
  sharpen under no-mips.)
* The facades at range still show the tint's deep levels (L2–L3 at the
  menu's distances), so the selection-overshoot signature of the third
  addendum stands as a separate, possibly co-occurring term.

Why this is the RIGHT next battlefield: the menu needs no operator and no
pose-matching — one fixed camera, and the B1 boot→title trace carries
hardware's ACTUAL texture bytes for the same draws (memory records; the
lean-tool census already enumerates every draw's fetch state). Part 45 step 0
is therefore: find the ball draw in B1 (shader + extent pin it), dump
hardware's s0 (and every other slot's) bytes from the trace, compare with a
live dump of ours at the menu — a pure byte-level adjudication of "wrong
region / wrong content / missing layer", with the E3 picture as the visible
gate. If ours and hardware's level-0 bytes MATCH for every slot of the ball
draw, the divergence moved into shading constants for that draw, and the same
trace supplies those too.

## 6by. Part 45: THE MENU LAB CONVICTS OUR OWN SYNTH TOOL — a PARTIAL write is not a
## write, and 217 pixel shaders had been sampling their diffuse at ONE TEXEL

The fourth addendum's plan ran exactly as written, and the byte-level adjudication
came back ALL-MATCH — which is the branch that says "the divergence is in the
shading", and this time the shading was audited to the instruction.

### The elimination, in order, one measurement each

* **The ball draw was NAMED, not inferred** (gotcha 302 honoured): a headless
  `CZ_VK_DRAW_ID` F9 at the menu, `drawid_read.py` on the scene surface at the
  ball's pixels — draw 1606, 1,229 verts, `vs_d338876a58c8c0ed` /
  `ps_eb170d16fe949e52`, four DXT1 slots + the rendered cube at s4. The 19%
  neighbour is the GAS-letter decal (a different pair, and it renders correctly —
  which becomes a clue below).
* **All four texture slots are BYTE-IDENTICAL to hardware.** B1 carries the same
  draw (66918 etc., same extents, same slot layout); `xtr_draw_bindings.py
  --dump-texture` for each of hardware's addresses vs our
  `CZ_VK_TEX_DUMP_PS=eb170d16fe949e52` tiled psdumps: four md5 matches of four
  (131,072 B / 32,768 B files). The staleness gate is moot for a dump that equals
  our own upload byte-for-byte. Decoded, s0 IS the red disc with the rust streaks
  and the white band; s1 the normal map; s2 a grayscale mask; s3 the baked-shadow
  atlas. The red was in our memory, uploaded, and never reached the screen.
* **The UVs are equal to the printed digit** — `xtr_draw_vertices.py` on B1 vs
  `CZ_VK_DRAW_PROBE` live: all 24 sampled verts of loc4 identical, and the menu
  camera is deterministic enough that vc0-vc10 match to four decimals too.
* **Every recoverable constant is equal**: c1, c14, c16-c20, c23, c24, c67 all
  match; our literal bank c253=(0.5,0.3333,0.75,2.0), c254=(1,1.5,-1,0.25),
  c255=0 is exactly the shape the microcode's normal-z reconstruction needs
  (`sqrt(c254.x - (x²+y²) + c255.x)` with scale c253.w=2, bias c254.z=-1), so
  the pc255=0 that looked alarming beside the DoF gather's open question is the
  CORRECT value here — hardware's copy is LOAD_ALU-sourced and unrecoverable
  from B1, but a literal bank this coherent is not a zero-init.
* **The dummies are refuted for this surface**: `CZ_VK_DUMMY_POISON=1` (all four
  heaps magenta, engagement printed per set) leaves the ball cream, pixel for
  pixel.

### The conviction

With every input equal and the output different, the divergence is inside the
translated module — and reading the generated HLSL against the disassembly names
it in one screen: the PS initialises `r1, r4..r7` from `iTexCoord1,4,5,6,7` and
**zero-initialises r0, r2, r3** — the DIFFUSE UV, the mask UV and the atlas UV.
`tools/synth_shader_container.py`'s liveness kept `written` as a flat set of
register NUMBERS, so instruction 10's `tfetch2D r0.__xy, r1.xy, tf1` — a fetch
whose destination swizzle KEEPS .xy and writes only .zw — removed r0 from the
interpolator inputs, sixteen instructions before `tfetch2D r1, r0.xy, tf0` reads
the components the fetch had preserved. Same story for r3 (written .zw at 14) and
r2 (written .w at 16). The translated shader sampled its diffuse at a CONSTANT
UV (0,0) — one texel of gray galvanised panel, squared, lit by the correctly
wired normal map, fogged and tone-mapped: a flat cream ball whose panel seams
come from the normal map, on a surface whose texture bytes were perfect. The
GAS-letter decal drew correctly because ITS shader's UV register is never
partially pre-written.

### The fix and its census

Per-component read-before-write, with every operand's component set transcribed
from XenosRecomp's own operand printer (`shader_recompiler.cpp`): vector source
lane sets by opcode family (dot family fixed-count, others = write mask with the
.x fallback), src3 shared between the 3-source vector opcodes and the scalar
co-issue (operand A at `((swz>>6)+3)&3`, B at `swz&3`, present iff the opcode is
not RetainPrev=50 — Adds is 0, so "nonzero opcode" would drop a real adds
feeding a `*_prev`), fetch destination swizzle 7 = Keep = not a write, scalar
co-issue DESTINATION writes tracked at all (the old analysis ignored them
entirely), predicated writes not counted as definite.

Census over the 434-microcode bank: **265 of 333 pixel shaders change; 217 gain
at least one interpolant** (1-5 each, the defect class), the rest lose only
spurious entries the old src3/scalar blindness had invented (verified on
`ps_ad65b98593f95926`: its "input 6" was three scalar co-issue writes the old
tracker could not see). Zero vertex shaders change.

### The gates, and the picture

* dim census on the new cache: 328 2D + 97 cube modules, zero disagreements.
* `no translated shader` = 0 on every run below; the lost-microcode entry
  `ps_926c15dd20571cf1` is carried over unchanged (its ucode is gone; documented
  name-diff exception since part 27).
* **The registered prediction held**: on the new cache the menu ball is RED with
  the rust streaks and the white band — E3's ball, at the same fixed camera.
  Full-frame diff vs the old cache: 7.6% of pixels moved >8 luma, localised to
  the affected materials.
* **The standing E3 correlation gate flips from fail to pass**: +0.687 (below
  the +0.70 threshold, "NO MATCH") on the old cache to **+0.710 ("LAYOUT
  AGREES")** on the new one, same frame, same reference.
* The old cache is preserved at `assets/shader_spv_pre45` and selectable with
  `CZ_SHADER_SPV` — the whole fix is a same-binary A/B.

### What this reopens, on purpose

Every shading-side measurement taken through the old cache has gotcha-172
exposure. The ones worth re-asking first: the residual white PROPS of part 26
(newspaper boxes, register, sign — a one-texel sample of an atlas corner is very
often white, so this class plausibly explains what §6bg's NaN fix left behind),
and the whole part-44 selection-overshoot signature, whose mip-tint readings
were taken on world shaders that are in the 217. Item 00i's outdoor half is
re-measured on the new cache before any further overshoot work.

### §6by addendum — the overshoot on the CLEAN bank: signature reproduced, sampler terms exonerated

Re-measured same day on the fixed cache (gotcha 172), DebugJump spawn
stand-still F9, one fixed view, arms differing by one env var:

* **Default**: part 44's signature reproduces — trucks solid L1 at ~2 m, vans
  L1/L2 at 5-10 m, ground L1 at mid-distance. The overshoot is real and
  separate from the interpolant defect. (Calibration: these fetches declare
  LINEAR mip filtering, so a SOLID code color needs LOD ≥ ~1 — the solid vans
  are a genuine ≥ +1-octave shift, not trilinear bleed.)
* **tint + `CZ_VK_ANISO=0`**: ~1 octave deeper everywhere — aniso ENGAGES in
  the default and buys its octave back. Not "aniso failing to engage".
* **tint + `CZ_VK_NO_FETCH_SAMPLERS=1`**: deeper still (Chuck's skin at L1 at
  arm's length). Neither arm SHALLOWS: the per-fetch sampler machinery is
  helping, not causing.

With the derivative environment audited as matched (viewport-path raster =
1280×720 = hardware's pixel grid; MSAA window scaling touches only
window-coordinate draws), the residual +1..2-octave global shift has no named
cause. The next discriminator is the hard number: hardware's implied LOD for
a projection-NAMED wall draw out of R4_04's own vertex streams
(`part46-kickoff.md` item 1). Captures:
`~/DR2CZ-troubleshooting/part45/{tintcap_fixed,tint_aniso0,tint_nofetch}`.

### §6by addendum 2 — THE OPERATOR'S A/B VERDICT: the FLAT-AT-RANGE CLASS was substantially the liveness defect, not the mip overshoot

Same evening, same binary, two launches differing by `CZ_SHADER_SPV` alone
(fixed cache / `assets/shader_spv_pre45`), operator driving their own route:

* **Run 1 (fixed): "everything seems fine except the tree… the game looks way
  better now, the building doesn't seem to have issues", and "run 1 is almost
  like OG game."**
* **Run 2 (pre-45): "way worse — gas station looks bad and building look FLAT
  DEPENDING ON DISTANCE."**

That second sentence is item 00i's original complaint, in the operator's own
words, reproduced ON DEMAND by switching the shader cache back — and gone on
the fixed one. **The flat-at-range class that drove parts 42, 43 and 44 was
substantially our interpolant liveness defect**, not the zone LOD decision
(part 43 exonerated that on its own evidence) and not, on the picture the
operator can see, the mip-selection overshoot.

**What this does to part 44's mip conclusion, stated carefully.** The
overshoot SIGNATURE still reproduces on the clean bank (addendum 1), so the
tint reading is not an artifact of the broken shaders. But its VISIBLE
consequence is now unclear: the surfaces whose flatness motivated the whole
mechanism look right to the operator with the mip chain untouched. Two
readings survive and part 46 must separate them rather than assume either —
(a) the overshoot is real and visually minor, its share of the complaint
having been swamped by the one-texel sampling; or (b) part 44's decisive arm
(`CZ_VK_NO_MIPS=1` "restores building detail at the matched view") was
measured through the broken cache and needs re-running before it can be
quoted at all. **Re-run that arm on the fixed cache FIRST** (gotcha 172, and
gotcha 30's shape: an arm that has never been re-asked after an upstream fix
is not evidence about the current renderer). If NO_MIPS no longer restores
anything, the overshoot loses its only picture-level support and becomes an
instrument reading in search of a symptom.

**Method note worth keeping** (this is why the fix was findable at all): the
menu lab's value was that it needed no operator and no pose matching, and it
produced a byte-level adjudication. But it was the OPERATOR who refused part
44's closure, and the operator who has now converted "the E3 correlation went
up" into "this is almost the OG game." Both halves were necessary; neither
would have closed this alone.

## 6bz. Part 45, operator session: THE UI TEXT LAYER IS STALE, IT ACCUMULATES WITH
## SESSION LENGTH, AND A FRESH HEADLESS RUN CANNOT SEE IT

The operator's second complaint on the fixed cache (the first being the tree canopy,
which is part 41's parked A2M item). **Pre-existing — they report it has been happening
for a while, and it is visible in part-44 captures.** 32 F9 captures, fixed cache, one
labelled directory: `~/DR2CZ-troubleshooting/part45-operator/ui_fixed/`.

### The symptom, characterised

Every affected screen shows the same three things, and the third is the one that names
the mechanism:

* **Colour changes mid-word**: `Whee|l` (white then green), `ATT|ACK` (green then teal),
  `Wh|eel: RETURNED` (white then red), `I've| found this part` (red then white).
* **Glyphs are missing**: `E___ne` for "Engine", with three glyphs simply absent.
* **The PREVIOUS screen's text persists**: the STATUS screen's SKILLS tab renders the
  ATTRIBUTES tab's five labels and none of its own; and in gameplay, after the pause
  menu is closed, `LEADERBOARDS / ACHIEVEMENTS / QUIT` are still painted over the world.

**Static text is perfect in the same frames** — "Chuck Greene", the stats panel, "130
KILLED", "ZOMBREX", "$21,700", the `B BACK` legend. Only text whose CONTENT changes is
wrong.

### What that combination means

§6ab established that this title batches its whole text layer into ONE dynamic vertex
buffer and sub-allocates it per run with `VGT_INDX_OFFSET`. So the draws are the current
frame's — right count, right colour constant, right screen position — while the VERTEX
DATA they read is an older copy of that buffer: this frame's colours land on the previous
layout's glyphs, runs that no longer exist keep being painted, and a run whose glyphs
moved loses the ones that fell outside the old span. One stale buffer garbles every run
at once, which is exactly what the captures show.

### Eliminated

* **Draws are not being dropped.** The `VGT_INDX_OFFSET runs past the vertex stream` and
  `vertex stream outside the physical arena` counters are BOTH ABSENT from the operator's
  54,676,315-draw session — zero. The missing glyphs are not declined draws.
* **The ALU constant window is honoured**, not assumed (0/256): 24 draws moved it in that
  session and the renderer reads `SQ_VS_CONST`/`SQ_PS_CONST` per draw. Wrong-window
  constants are not the colour splits.
* **A FRESH HEADLESS SESSION CANNOT REPRODUCE ANY OF IT.** Main menu, save-slot screen,
  Help & Options (from the title), the PAUSE MENU itself and the in-game case banner all
  render perfectly in short runs — captures beside the operator's for each. The defect is
  STATE-DEPENDENT and accumulates over a session; that is a finding about the mechanism,
  not a failure to look.

### A control that could not fail, recorded as such

The first positive control ran `CZ_VK_STREAM_GUARD_BYTES=64` — a guard EIGHT TIMES
smaller than the one item 00c was fixed from — against the Help & Options screen, and the
picture was pixel-identical to the default. That is not evidence: **the screen is static,
so a stale copy of its buffer is indistinguishable from a fresh one**, and the arm could
not have produced the symptom whatever the truth. Gotcha 30's shape, made here after
writing it down elsewhere. A positive control for a STALENESS defect has to run against
text that CHANGES.

### The leading suspect, and why it is not yet a finding

The cross-frame stream store's guard is exact only to 16 KB and SAMPLES 8x64 bytes above
it. Measured at the menu, `CZ_VK_PROFILE` reports **1,785-3,296 streams per frame
exceeding the bound and therefore only sampled**, and the store grows to **12,162 entries
in 20 seconds**. A missed edit is served forever rather than for one frame, because the
guard keeps returning the same hash — which matches "the SKILLS tab still shows
ATTRIBUTES seconds later". This is item 00c's mechanism (part 24, the ammo counter) at a
larger buffer size, and the guard's own comment predicts this failure in these words.

**But it is a suspect, not a conclusion**: no arm has yet been run against a REPRODUCING
instance, because the fresh-session repro does not exist. The next two moves, in order:
(1) the direct measurement that needs no picture — run one route with the default guard
and again with `CZ_VK_STREAM_GUARD_EXACT=1` and compare the `stale` counter, since any
excess the exact guard catches is an edit the sampled one missed; (2) an operator arm,
which is cheap because they reproduce it in about two minutes of play:
`CZ_VK_NO_PERSIST_STREAMS=1` is the blunt control (store off entirely), and
`CZ_VK_STREAM_GUARD_EXACT=1` is the one that separates "the store" from "the store's
GUARD" — the distinction that decides whether the fix costs 4.7 ms of a crowd frame or
nothing.

### §6bz addendum — the stale text is NOT FROZEN, it VARIES FRAME TO FRAME, which points at a TEAR rather than a cache

Three consecutive F9 captures of ONE screen (the CASE FILE, operator captures
004267/004298/004340) differ from each other by 3.5-4.3% of pixels, and the
difference is in the TEXT: the visible fragments are

    "…Bike Parts" / "Unknow"          then
    "…Bike Parts" / "ON: Unknown"     then
    "B" / "Part(s)" / "ON:"

— a DIFFERENT partial subset of the same strings each time. A cache serving one
stale copy would garble identically every frame; this does not.

**So the better reading is a TORN buffer**: we copy the guest's text vertex
buffer while the guest is still writing it, and get part new / part old. That
reproduces all three symptoms at once — a run whose glyph quads are half-written
loses glyphs, the surviving half sits at the previous layout's positions under
this draw's colour constant (the mid-word colour split), and runs the guest has
stopped emitting persist wherever their old bytes were not yet overwritten.

**What it predicts, and what to test next.** A tear needs the reader and the
writer to overlap, so it should scale with how far our command processor LAGS
the guest — which is exactly what grows over a session and what a fresh short
run does not have. `CZ_VK_FRAMES_IN_FLIGHT=1` (the pre-part-23 renderer, same
binary) is therefore the arm to run after the guard arm, and it has a date
attached: part 23 made 2 the default, and the operator reports this defect as
long-standing rather than new. Note this also demotes the cross-frame store: a
store that faithfully caches a TORN copy is a victim, not the cause, and its
guard arm would not fix the picture.

The two arms are cheap and mutually exclusive in what they implicate, so run
both before building anything (gotcha 5).

### §6bz addendum 2 — CONFIRMED BY A MATCHED OPERATOR A/B: it is the STORE'S GUARD, and the tear reading of addendum 1 is RETRACTED

The operator played the `CZ_VK_STREAM_GUARD_EXACT=1` arm and took one capture,
which is all it needed: **the STATUS screen's KEY ITEMS tab, same save state as
the default-guard session** (identical left panel — level 1, 13,250 PP,
$21,700, 130 kills), one environment variable apart.

| arm | the KEY ITEMS tab |
|---|---|
| default (exact to 16 KB, sampled above) | renders the ATTRIBUTES tab's labels — `ATTACK / SPEED / LIFE / ITEM STOCK / THROW…` — plus a garbled `ombrex.` fragment, and none of its own content |
| `CZ_VK_STREAM_GUARD_EXACT=1` | `Still Creek Map` (selected, yellow) / `Zombrex` / `Shed Key`, with `A detailed map to Still Creek.` — correct, complete, one colour per run |

`~/DR2CZ-troubleshooting/part45-operator/ui_fixed/capture_004665.ppm` against
`ui_guardexact/capture_002676.ppm`. **The cross-frame stream store's GUARD is
the mechanism** — item 00c (part 24, the ammo counter) recurring at a buffer
size above the 16 KB bound that fixed it, exactly as this code's own comment
predicted it would.

**RETRACTION, in place.** Addendum 1 read the frame-to-frame variation as a
TORN buffer and named `CZ_VK_FRAMES_IN_FLIGHT=1` as the next arm. That is
wrong, and the guard explains the variation better: the sampled guard reads 8
blocks of 64 bytes, so it catches an edit only when a sampled block happens to
land on changed bytes — sometimes it does and the run re-copies, sometimes it
does not and the old bytes are served. Different fragments therefore update on
different frames with no concurrency involved at all. The observation stands;
the inference from it does not.

**What the fix has to be, and the number that constrains it.** Not "always
exact": the operator's arm read **63.76 MB/frame in the guard against the
default's 9.28 MB/frame**, and that lands squarely in the performance
regression they reported the same evening (open item 00l). The fix is a guard
that is exact for the streams in this class without paying for it on the
crowd's big geometry — raise the bound and measure, or make exactness a
property of the stream KIND (the store already splits declared binding / index
buffer / dependent fetch) rather than of its size. Measure the candidates the
way `docs/measurement.md` says, because this one has a real cost on the frame.

## 6ca. Part 46: THE TREES — a HEADLESS repro with a hardware oracle, the defect
## attributed to a DRAW BY MEASUREMENT, and ALPHA-TO-MASK named as the mechanism

Part 46's first item, set by the operator: the tree canopies render hard-edged
near-black shards among otherwise-correct gold leaves
(`~/DR2CZ-troubleshooting/part45-operator/capture_006615`). It pre-dates the
part-45 liveness fix. This section records what was established, what was
refuted, and — importantly — two things the part-46 kickoff said that turned
out to be wrong.

### The repro moved from "an operator at the gas station" to the TITLE SCREEN

The defect reproduces on the **menu backdrop**, which is a near-static Still
Creek scene reached in ~40 s with no input at all:

```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 \
   CZ_CAPTURE_KEY=<dir> CZ_FAKE_START_MS=8000 \
   CZ_FAKE_PRESS_SEQ=NONE,NONE,NONE,F9,NONE timeout 120 ./cz_runtime)
```

That matters for three reasons and each was a blocker before. It is **cheap**
(120 s against 420 for the DebugJump route). It is **static**, so two arms land
on the same camera and a `CZ_VK_DRAW_ID` map can be read against a picture from
a different run — the outdoor route cannot do this (gotcha 254, and two spawn
runs here came out with view directions 0.9 apart). And it has a **hardware
oracle already in the repository**: `Xenia logs/E_screenshots/E3_title_background_stillcreek.png`
is this exact screen. E3's content box is x 0..1399, y 96..880 — crop and
resize to 1280x720 and it registers against our capture directly.

The gameplay route still reproduces it (the DebugJump spawn tree renders almost
entirely black), but nothing below needed it.

### THE KICKOFF NAMED THE WRONG SHADER PAIR — the menu tree is a different material

`docs/part46-kickoff.md` names `ps_69a5c3be9359b87c` / `ps_8602b5fd69289893` as
"the leaf materials", from the operator's gameplay capture. On the menu tree the
canopy is **`vs_716ff2d14e06fa52` / `ps_03533a74cbd5228c`**. Both pairs are real
leaf materials; neither is "the" one. This is the third time in three parts that
a shader-hash-by-appearance inference selected the wrong draw (gotchas 291, 302,
and now this), and it is why the attribution below was done with the draw-ID
pass instead.

### The attribution, and what it eliminates

`CZ_VK_DRAW_ID=1` on the menu frame, read against the same run's census, with
the dark-shard pixels and the correct-gold pixels selected separately out of the
canopy box (645,395)-(795,505) of the *picture* run:

| pixels | top draw | share |
|---|---|---|
| dark shards (628 px) | draw 2329 `vs_716ff2d14e06fa52 / ps_03533a74cbd5228c cc=AA00001C` | 267 px |
| correct gold (12,251 px) | **draw 2329, the same draw** | 2,367 px |

**The shards and the leaves they sit among are the SAME DRAW.** That eliminates
every per-draw input in one measurement: the constants, the bound textures, the
render state and the pipeline are by construction identical for both sets of
pixels. Whatever differs is per-vertex or per-texel.

`ps_03533a74cbd5228c` narrows it further: it samples **exactly one texture** and
its colour is a function of the interpolated normal alone (twelve constants
`pc(68)..pc(79)` plus `pc(23)`, each dotted against the normalised interpolator
2 — an irradiance evaluation). So the candidates are the per-vertex NORMAL and
the per-texel texture, and nothing else.

### Refuted: the render-state read, and the packed-normal decode

Both were checked against hardware rather than argued.

**The render state is byte-equal to hardware.** `tools/xtr_draw_bindings.py`
over all 19 round-2/3/4 traces: for the leaf shaders hardware uses exactly the
three `RB_COLORCONTROL` values our own census reports — `AA000007` (test off),
`AA00000C` (GREATER + enable, alpha-blended) and `AA00001C` (GREATER + enable +
**ALPHA_TO_MASK**, opaque) — with the same `RB_BLENDCONTROL` for each, and
`RB_ALPHA_REF = 0.0` on **every** leaf draw in every trace. So the part-46
kickoff's "read how `vk_renderer.cpp` treats `RB_COLORCONTROL` bit 4 before
theorising" has been read, and the register decode is not the defect.

**The `k_10_11_11` packed-normal decode is correct**, and the check is an
invariant rather than a comparison (memory: *derive the reading from an
invariant*). Hardware's own vertex streams for the leaf VS, read with
`tools/xtr_draw_vertices.py`, decode through `XeUnpack_10_11_11` to **unit
length on 512 of 512 sampled vertices** across 12 draws. A wrong field layout,
a wrong sign convention or a wrong endian could not produce that. The same
census also shows **74.8% of hardware's leaf normals have N·L > 0** (median
+0.365), which is what makes hardware's canopy lit at all — the shader's
diffuse term is `max(N·L, pc(254).x)` and `pc(254).x` reads **0.0** in our
runtime, so a back-facing card genuinely does collapse to the ambient term on
BOTH machines.

One suspect came out of that census and is **demoted, not closed**: 10.5% of
hardware's leaf packed normals are DENORMAL as float32 and 2.5% are NaN, and
those are precisely the `(0, 0.999, 0)` straight-up normals — the brightest
cards. We deliver that attribute as `VK_FORMAT_R32_SFLOAT` and recover the bits
with `asuint` (vk_renderer.cpp's fmt-16 note), which is only safe if the fetch
is a raw dword load. The `XE_NAN_VS_KILL_IN` shader arm (built with
`CZ_DXC_DEFINES="-D XE_NAN_VS_KILL_IN=1"`, which kills any vertex whose inputs
are NaN) **does fire on this tree**: 892 of the canopy box's 16,500 pixels
change, 5.41%, 71 of them by more than 32 levels. So NaN-class vertices are
present and the exponent bits reach the shader intact — which is the opposite of
what "the fetch destroyed them" predicts.

**A correction, in place**: this section first recorded that arm as leaving the
tree "pixel-identical", which was an eyeball comparison of two zoomed crops. The
canopy crop's md5 says otherwise. Looking at a picture is not a null test
(gotcha 133); hash the region.

What the arm still cannot say is whether the MANTISSA survived, since a
canonicalised NaN is still a NaN, so the denormal half of the class is
untouched by it. Given that the A2M arm below reproduces and removes the actual
symptom, this suspect is no longer the leading explanation for anything, and it
should be closed by a cheap direct check rather than by more picture arms.

### The mechanism: ALPHA-TO-MASK, on a material with FRACTIONAL alpha

The canopy draws are `cc=AA00001C` — alpha test GREATER with **`RB_ALPHA_REF =
0.0`**, plus ALPHA_TO_MASK, with blend `ONE/ZERO`. Two facts turn that into the
symptom:

1. **The leaf albedo is `fmt=20`, `k_DXT4_5`** — 8-bit interpolated alpha, not
   punch-through. (Every `AA00001C` leaf draw in the menu frame, in the
   operator's gameplay frame and in hardware's `w2_gasstation` binds a DXT4/5
   at slot 0.) An artist wanting a binary cutout would ship DXT1; DXT4/5 plus
   ALPHA_TO_MASK is a *coverage* material.
   **And the split is exact, which is the corroboration**: over the operator's
   frame 6615, all **58** `cc=AA000007` leaf draws (alpha test OFF, opaque) bind
   a **DXT1**, and all **26** draws that enable the test (10 `AA00000C`
   alpha-blended, 16 `AA00001C` A2M) bind a **DXT4/5**. The material's format
   and its render state agree perfectly about which draws want a cutout, with no
   overlap in either direction — so "the foliage cards are the fractional-alpha
   ones and the trunk/branch geometry is not" is measured rather than assumed.
2. **At ref = 0 the alpha test keeps essentially everything.** Our emulation is
   `clip(oC0.w - g_AlphaThreshold)` with the threshold published as
   `ref + 1/512` — so every texel with alpha above ~0.002 is written at FULL
   opacity into an opaque-blended target. The soft feathered fringe of each leaf
   card, which the artist authored at alpha 0.05..0.5, is painted solid.

On hardware ALPHA_TO_MASK converts that fractional alpha into a per-sample
coverage pattern inside the 4x MSAA surface, and the resolve averages it — which
is exactly the soft, feathery canopy E3 shows. `vk_renderer.cpp` declines to
emulate A2M and justifies it as "the draws hardware sets it on also set the
alpha test, so the clip covers them". **That justification does not hold when
the reference is zero**: with ref = 0 the alpha test is a no-op and A2M is doing
all of the work, which is the case for every canopy draw in this title.

The quantitative comparison against E3 supports "coverage, not brightness":
over the registered canopy box, hardware and our render have nearly the same
luminance distribution —

| | p05 | p50 | p95 | p05/p95 | share < 40 |
|---|---|---|---|---|---|
| hardware (E3) | 58.6 | 115.2 | 179.6 | 0.326 | 0.9% |
| ours | 55.1 | 126.5 | 189.4 | 0.291 | 0.3% |

— so the canopy is not globally too dark, and the lighting inputs are not
grossly wrong. What differs is spatial: hardware's dark foliage is soft-edged
and blended, ours is hard-cut. That is the signature of binary coverage
replacing fractional coverage, and it is why the operator reads it as "shards".

### Why this is emulable here, exactly, and what the fix looks like

We rasterise at `VK_SAMPLE_COUNT_1_BIT` into an EDRAM image that is **twice as
wide and twice as tall** for a 4x-MSAA surface (the part-32/33 window scale,
`vk_renderer.cpp`'s `msaa == 2` block). So one of our pixels IS one guest
sample, and the 2x2 sample grid is `(int(iPos.x) & 1, int(iPos.y) & 1)`. A2M is
therefore not an approximation problem here — the coverage pattern can be
reproduced sample for sample, and `RB_COLORCONTROL`'s top byte `0xAA` is
literally `alpha_to_mask_offset0..3 = 2,2,2,2`, hardware's own ordering.

The change is to make the alpha threshold **per-sample** for A2M draws: an
ordered 2x2 dither instead of the scalar `g_AlphaThreshold`. The emitted test
comes from XenosRecomp (`clip(oC0.w - g_AlphaThreshold)`) and `g_AlphaThreshold`
is a macro in its `shader_common.h`, so this can be built as a shader-cache arm
(`CZ_DXC_DEFINES` + `CZ_SHADER_SPV`) and A/B'd on the menu frame against E3
before anything is made default. **The property to move is named**: the
p05/p95 ratio and the hard-edge count inside the registered canopy box, both
against E3 — not "does it still look wrong" (memory: *name the property a fix
should move*).

**The alpha channel was already dumped, in part 40, and it is a RAMP.**
`CZ_VK_TEX_DUMP_PS` pulled the gameplay leaf material's own textures out of a
headless run: a 256x256 DXT5 leaf sheet, a 128x256 DXT1 bark strip and a
256x512 DXT5 branch card, all decoding cleanly through our untiler with, in
that session's words, "the DXT5 alpha planes are PERFECT — the leaf sheet's
alpha is a per-leaf cutout mask" (open item 0t). That was recorded then as
*exonerating* the texture, and it is better read now as the other half of this
diagnosis: an authored per-leaf alpha mask in an 8-bit-alpha format is exactly
the input ALPHA_TO_MASK exists to spread over coverage, and exactly what a
binary clip destroys. The same dump has not been repeated for the MENU tree's
`ps_03533a74cbd5228c`, which would cost one 120 s run
(`CZ_VK_TEX_DUMP` + `CZ_VK_TEX_DUMP_PS=03533a74cbd5228c`); the trace tools
cannot substitute, because none of the 19 captures carries that texture's bytes
(`--dump-texture` returns "dumped 0 copies").

**Not yet done, and honestly owed**: the fix itself is not built or measured.

### §6ca addendum — THE MECHANISM IS DEMONSTRATED: an A2M coverage arm removes the
### shards and lands the named property on hardware, and the remaining work is named

The dither described above was built and run, and it converts §6ca from a named
mechanism into a demonstrated one.

**What was built.** Three pieces, each separately controlled:
* `XenosRecomp/shader_common.h` gains `XeAlphaTestThreshold(float2 pos)`, which
  returns `g_AlphaThreshold` unless the shader is built with `XE_ALPHA_TO_MASK`,
  in which case an A2M draw takes `max(threshold, (bayer2x2 + 0.5) / 4)` indexed
  by `uint2(pos) & 1`.
* `shader_recompiler.cpp` emits `clip(oC0.w - XeAlphaTestThreshold(iPos.xy))` in
  place of `clip(oC0.w - g_AlphaThreshold)`.
* the runtime publishes an A2M flag at shared+284, and — separately — turns the
  clip on for A2M draws that do NOT enable the alpha test, which would otherwise
  have compiled no clip at all for the arm to modify.

**THE SURFACE IS 2x, NOT 4x, AND THE COUNTER IS WHAT SAID SO.** The first gate
only published the flag for `RB_SURFACE_INFO` msaa == 2 (4x), on the reasoning in
§6ca that our 4x window scale makes one of our pixels one guest sample. It
published **zero** flags, against 187,621 draws in the same run taking the 4x
window scale — a contradiction a nameless "declined" counter could not resolve,
so the counter was changed to NAME the sample count. It reads **msaa=1 (2x) on
69,390 A2M draws and msaa=0 on 518, and 4x on none of them**: this title draws
its foliage into a 2x surface, which our renderer does not sample-expand. So the
exact sample-for-sample emulation §6ca proposed is not available at the foliage
as the renderer stands, and `CZ_VK_A2M_ANY_SURFACE=1` was added as a diagnostic
that drops the gate — knowingly dithering at PIXEL granularity, which is a worse
picture but a decisive question.

**AND HARDWARE AGREES THAT IT IS 2x.** A number that redirects a fix should not
rest on our own decoder (an untrusted path is not an oracle), so
`tools/xtr_draw_bindings.py` now carries `RB_SURFACE_INFO` (0x2000) into its CSV
as well. Over `w1_spawn`: **msaa = 1 on every leaf draw of both leaf shaders —
all four `RB_COLORCONTROL` flavours — and on every one of the 240 A2M draws in
the trace, with none at 1x or 4x.** So the title genuinely renders this pass into
a 2x surface and our read is right. (Our own runs also show 140,715-187,621
draws taking the 4x window scale in the same frame, so both surfaces exist in one
frame; which pass is which is not chased here.)

**The result, at the menu frame, canopy box (645,395)-(795,505), against E3:**

| arm | p05 | p50 | p95 | **p05/p95** | hard-edge share |
|---|---|---|---|---|---|
| hardware E3 | 58.6 | 115.2 | 179.6 | **0.326** | 0.21% |
| default cache | 55.1 | 126.5 | 189.4 | 0.291 | 3.13% |
| null cache (new emitter, define OFF) | 55.1 | 126.5 | 189.4 | 0.291 | 3.13% |
| A2M built, 4x gate declines every draw | 55.1 | 126.5 | 189.4 | 0.291 | 3.13% |
| **A2M dither (`CZ_VK_A2M_ANY_SURFACE=1`)** | 61.6 | 129.0 | 190.0 | **0.324** | 4.92% |

**The three null arms are byte-identical over the canopy** (md5
`f4a1a593a15b3e27b40d59136aadf622` on all three crops), which is the control this
project's own rule asks for before quoting an arm: the emitter change and the
published flag are proven no-ops, so the fourth row is the dither and nothing
else. The dither changes 59.5% of canopy pixels, 12.9% by more than 32 levels.

**Read it as two separate statements.** The tonal one is that the named property
lands on the oracle — p05/p95 0.291 → 0.324 against hardware's 0.326 — and the
picture shows the hard dark plates broken up into feathered foliage with sky
visible through it. **The shards were missing coverage.** The spatial one is that
hard edges go UP, 3.13% → 4.92%, where hardware sits at 0.21%: that is the
expected artifact of a 2x2 dither on a surface with no sample grid under it, and
it is why this stays a diagnostic arm rather than becoming a default.

**What the fix now is, and it is a renderer change rather than a shader one:**
give the coverage somewhere to be resolved. Either sample-expand 2x surfaces the
way `msaa == 2` surfaces are already expanded (Xenos 2x is a vertical sample
pair, so a 1x2 dither over a Y-expanded image is exact and the existing resolve
path averages it), or rasterise A2M draws with real Vulkan MSAA plus
`alphaToCoverage`. The first fits this renderer's existing design and is the
recommendation. The shader side is already built and controlled; only the
surface expansion is missing, and the arm above is how the result will be read.

## 6cb. Part 46: THE PERFORMANCE REGRESSION — part 45's shader fix is EXONERATED by a
## three-runs-an-arm A/B, and the profiler names `textures` as where the frame went

The operator's second item: *"the performance degraded with all the fix you did
in the last few days."* Unmeasured until now. `tools/part46_perf_ab.sh` runs the
A/B and `tools/part46_perf_read.py` reads it.

### The design, and why it is the only one-variable A/B available

Three suspects were named in the part-46 kickoff, in the order they were
introduced: part 41's per-fetch samplers, part 44/45's mip-chain and tail
uploads, and part 45's interpolant-liveness fix, which added interpolants to 217
of 333 pixel shaders. Only the last has a whole preserved control arm —
`assets/shader_spv_pre45` is the entire pre-fix cache — so it is the one that can
be turned into a **one-variable, same-binary** A/B with no rebuild.

Six runs of the unattended outdoor DebugJump route, **alternated** rather than
blocked so that thermal drift over the hour cannot be read as an arm difference,
600 s each, `CZ_VK_PROFILE=30` and `CZ_VK_FRAME_STATS` on both. All six reached
the outdoor era (peak draws 7,291-10,014). GPU sampled during the block with
`tools/gpu_clock_sample.py`: **P5, mean 559 MHz, 34% utilisation, 28.1 W** —
the awake, governed state this project documents, not the blanked-monitor P8 that
gotcha 219 was retracted over.

### The result: a clean NULL, in every draw bin

Medians per run, and the share of frames within 1 ms of the 16 ms pacing period —
the two statistics gotchas 237/238 say to read, because a MEAN on this title
measures its own two-vblank floor rather than the change.

| draw bin | fixed (median ms x3) | pre45 (median ms x3) | fixed vs pre45 | within-arm floor |
|---|---|---|---|---|
| 0-500 | 32 32 32 | 32 32 32 | +0.0% | 0.0% |
| 500-1500 | 32 32 32 | 32 32 32 | +0.0% | 0.0% |
| 1500-3000 | 32 32 32 | 32 32 32 | +0.0% | 0.0% |
| 3000-5000 | 34 33 36 | 35 33 38 | −2.9% | 14.3% |
| 5000-8000 | 48 48 45 | 45 46 46 | +4.3% | 6.2% |

**Every bin is inside its own measured noise floor**, and the two bins that are
not at the pacing floor disagree in SIGN (−2.9% and +4.3%), which is what a null
looks like. Below 3,000 draws both arms sit at 32 ms with 94-99% of frames
pinned — there is nothing there to regress. **Part 45's liveness fix is not
responsible for a measurable frame-time regression on this route**, and 217
shaders gaining interpolants cost nothing readable.

The phase split agrees, which is the stronger form of the same statement because
it does not go through the pacing floor at all — outdoor samples only, ≥4,000
draws/frame, sub-phases as a share OF THE DRAW PATH:

| arm | ms/frame | draws | draw% | constants | streams | **textures** | record | other | outside% |
|---|---|---|---|---|---|---|---|---|---|
| fixed | 42.9 | 5,241 | 66.9% | 2.4 | 0.0 | **45.4** | 11.6 | 7.1 | 32.1% |
| pre45 | 41.9 | 4,783 | 64.8% | 2.4 | 0.1 | **43.0** | 11.6 | 7.2 | 34.0% |

### WHAT THE PROFILER DID SAY: `textures` is now most of the draw path

The interesting number is not the difference between the arms — it is the column
they agree on. **`textures` is 43-45% of the draw path on both**, i.e. roughly
29-30% of the whole frame.

Against part 20's corrected measurement at ~6,800 draws — draw path 19.9 ms of
which `textures` 2.7 ms, **13.6%** — that share has more than tripled. And the
two suspects the A/B could not test are exactly the two that live in the texture
path: **part 41's per-fetch samplers** (one `VkSampler` per distinct fetch spec,
chosen per fetch) and **part 44/45's mip-chain and packed-tail uploads**.

**Read that comparison with its exposure, honestly.** Part 20's figure is a
different session, a different route era and a different binary, and it is a
SHARE rather than a like-for-like millisecond count, so gotcha 13 applies to it
as much as to anything else here. What it supports is a direction to look, not a
quantity. What makes it actionable anyway is that both suspects already have
same-binary arms and need no new code: `CZ_VK_NO_FETCH_SAMPLERS=1` (the part-40
renderer: one global trilinear sampler), `CZ_VK_NO_MIPS=1` (the part-38
renderer: level 0 only) and `CZ_VK_NO_MIP_TAIL=1` (the part-40 renderer).

**And read the `textures` share rather than the frame time when running them.**
The frame-time A/B above needed three runs an arm and still landed inside its
floor in every bin; the profiler's phase share is an internal attribution rather
than a paced quantity, so it separates arms that the frame time cannot — which
is the whole lesson of gotchas 237/238 applied one level further in.

### §6cb addendum — AND THE OTHER TWO SUSPECTS ARE EXONERATED TOO, on the phase share

The section above named `textures` as where the frame goes and pointed at the two
suspects living in that path. Both were then run, plus the third mip arm, one
600 s outdoor run each against the three-run baseline, read on the `textures`
share rather than on frame time:

| arm | ms/frame | draws | draw% | **textures (% of draw path)** |
|---|---|---|---|---|
| baseline (3 runs) | 42.9 | 5,241 | 66.9% | **45.4** |
| `CZ_VK_NO_FETCH_SAMPLERS=1` (part-40 renderer) | 43.5 | 4,773 | 66.2% | **44.6** |
| `CZ_VK_NO_MIPS=1` (part-38 renderer) | 46.9 | 5,718 | 67.8% | **46.0** |
| `CZ_VK_NO_MIP_TAIL=1` (part-40 renderer) | 44.6 | 5,626 | 66.6% | **44.2** |

**Every arm engaged, and each is shown so by the counter the others carry**
(gotcha 151): the baseline uploads 2,513 mip chains and 6,452 packed-tail levels
and creates 4 distinct samplers; `NO_FETCH_SAMPLERS` creates **0** samplers while
still uploading 2,450 chains and 6,274 tails; `NO_MIPS` emits **no** chain or
tail lines while still creating 4 samplers; `NO_MIP_TAIL` uploads 2,178 chains
and **no** tails. So these are real semantic differences, not inert flags.

**And none of them moves `textures` — 44.2 to 46.0 against a baseline of 45.4.**
So all three suspects the part-46 kickoff named are exonerated: part 45's
liveness fix by the six-run A/B above, and part 41's per-fetch samplers and part
44/45's mip uploads by this table.

**Read four "unmoved" results in a row as a fact about the framing, not as four
eliminations** (that is the standing lesson from part 33's instrument-class
wall). What this table actually establishes is that **the `textures` share is
STRUCTURAL rather than recent** — it is not something the last few days
introduced. Which in turn means the part-20 comparison in §6cb is the weak half
of that argument and should be treated as such: a share measured on a different
route era, binary and session is exactly the number gotcha 13 says has a shelf
life, and the honest reading now is that 13.6% and 45% may simply not be
comparable quantities.

**What is NOT concluded here.** "No regression exists" is not what this measures.
The operator plays windowed on a real display, on their own route, with audio and
a swapchain this headless route does not exercise; the standing rule in this
project is that an operator report outranks a headless number. What is measured
is narrower and still useful: **on the outdoor DebugJump route, none of the three
changes named as suspects produces a frame-time or phase-share regression, and
three of them cannot be the cause.** The next move is to measure on the
operator's own configuration — a windowed run with `CZ_VK_PROFILE` and
`CZ_VK_FRAME_STATS` on their route, and ideally the same on a binary from before
part 41 — rather than to run a fourth arm here.

## 6cc. Part 46, OPERATOR SESSIONS: the A2M arm judged in play, a THIRD mode that keeps
## the win without the screen door, and the FIRST profile of the operator's own frame

Two chained operator sessions (`tools/part46_operator_session.sh` and
`part46_operator_session2.sh`), captures in
`~/DR2CZ-troubleshooting/part46-operator{,2}/`.

### The trees: the arm is confirmed in play, and it brought its own defect

Session 1, default vs the A2M dither. The operator: *"I think the trees look
better on second arm but they still got different kind of issue."* Both halves
of that are measurable, and the second half is the artifact §6ca predicted in
advance rather than a surprise.

The new statistic is **ISOLATED PIXELS** — a pixel differing from all four of its
neighbours by more than 50 levels. A real edge cannot produce one; a dither
produces nothing else, so it separates "cutout" from "screen door" where the
luminance quantiles cannot.

Session 2 put a third mode against the second at a **near-matched pose** (eye 5.4
units apart, view directions within 0.02 — the same tree by the DAILY
NECESSITIES building):

| arm | hard-edge | **ISOLATED** |
|---|---|---|
| hardware E3 (menu canopy) | 0.18% | **0.00%** |
| A2M mode 2 — per-sample dither | 12.92% | **4.17%** |
| A2M mode 1 — flat threshold at 0.5 | 6.93% | **0.71%** |

and on the headless menu tree, which IS pixel-matched:

| arm | p05/p95 | hard-edge | ISOLATED |
|---|---|---|---|
| hardware E3 | 0.326 | 0.18% | 0.00% |
| default | 0.291 | 3.10% | 0.18% |
| mode 2 (dither) | 0.324 | 4.95% | 1.13% |
| **mode 1 (flat 0.5)** | 0.306 | 3.37% | **0.20%** |

**Mode 1 is the one to keep.** It gives up hardware's soft edge and recovers
about half the tonal gain, but it removes the hard black plates with **no screen
door at all** — 0.20% isolated against the default's 0.18%, where the dither is
1.13%. Its justification is one line: a coverage mask averaged over a resolve
crosses 50% at alpha 0.5, so a flat 0.5 threshold puts the SILHOUETTE where
hardware's resolve puts it without needing a sample grid to average anything.
`CZ_VK_A2M_MODE=1` / `=2` selects them.

### A METHOD ERROR, recorded because it nearly became a finding

Two operator captures were compared per-pixel over a canopy box on the strength
of the pictures looking like the same view. `tools/pose_read.py` says the eyes
are **250 units apart** with view directions 0.88 and −0.03 in x — different
trees entirely, and the numbers were comparing unrelated pixels. They were
withdrawn. **Read the pose before the picture**: this port has a tool that
answers "is this the same place" in one second, and eyeballing two screenshots is
not that tool. It is the same wall gotcha 254 describes, met from the other side.

### THE FIRST PROFILE OF THE OPERATOR'S OWN FRAME, and it does not look like ours

Session 1 shipped with no `CZ_VK_PROFILE` and no `CZ_VK_FRAME_STATS`, so the
operator's *"performance is at around 20fps"* had no measurement behind it at
all. Session 2 wired both. Their frame, windowed, on their own route:

| fps | ms | draws | draw path | textures (% of draw) | outside |
|---|---|---|---|---|---|
| **14.6** | 68.6 | 5,080 | **78.6%** | 33.1 | 20.2% |
| 21.7 | 46.0 | 4,590 | 66.9% | 42.1 | 31.4% |
| 31.2 | 32.0 | 1,423 | 25.5% | 15.2 | 72.0% |

**The 20 fps is real and it is OURS.** At 1,400 draws they sit at the two-vblank
floor with 72% of the frame `outside` — the guest's pacing, nothing to win. In a
crowd the draw path is **67-79% of the frame**, and at 5,080 draws that is 53.9
ms of a 68.6 ms frame, of which `textures` is ~17.8 ms — the largest single term,
exactly as §6cb's headless profile said.

**AND THE HEADLESS ROUTE UNDERSTATES IT BY ABOUT A FACTOR OF TWO.** §6cb measured
the draw path at 28.7 ms at 5,241 draws; the operator's is **53.9 ms at 5,080**.
Same order of draws, nearly double the cost. Read with its caveat — a different
route renders different content at the same draw count, and windowed adds a
swapchain and a compositor this route never touches — but it is large enough to
matter for how §6cb's conclusions are used: **the three suspects "exonerated"
there were exonerated on a workload that is half as expensive as the real one**,
so that is an exoneration on the headless route, not on the operator's.

### THE NUMBER THAT UNBLOCKS ITEM 00c/00k, finally measured

The UI text defect has been diagnosed to the cross-frame stream store's guard
since part 45, and the fix has been parked twice with the same recommendation —
"raise the bound" — and no way to say to what, because a COUNT of exposed streams
cannot price a bound. Raising it costs in BYTES. Session 2 added the histogram:

| stream size | streams/frame | MB/frame if the bound covered it |
|---|---|---|
| 16-32K | 46 | 1.1 |
| 32-64K | 144 | 5.4 |
| 64-128K | 137 | 9.5 |
| **128-256K** | **831** | **104.7** |
| 256-512K | 411 | 103.0 |
| 512K-1M | 24 | 12.3 |
| 1-2M | 2 | 2.0 |

**A bound of 128 KB costs 16 MB/frame; 256 KB costs 121 MB/frame.** The
distribution is strongly bimodal — a few hundred small streams, then a wall of
831 buffers at 128-256K — so the question "is the UI text buffer under 128 KB"
now has a price attached to both answers, and the arm that settles it
(`CZ_VK_STREAM_GUARD_BYTES=262144`, deliberately wide so a null cannot be
ambiguous) is what the operator is playing. If it fixes the HUD, the histogram
narrows the bound afterwards; that is the order that gets a fix instead of a
trade.

### §6cc addendum — THE UI TEXT DEFECT IS FIXED, confirmed in play, and what it costs

Open items 00c/00k, first seen in part 24 and diagnosed in part 45, closed in
part 46 by an operator session. Their words, on the third build of the evening:
**"Ui stay good the whole time."**

**The route to it, because two of the three steps were wrong and both were
informative.**

1. *Raise the guard's byte bound.* This was the recommendation in two successive
   kickoffs and it is REFUTED: at 256 KB the operator's HUD still dropped out
   (`part46-operator2/ui_guard/`, captures 2240 and 2293 correct, 2528 with the
   whole top-left block gone — with the presence test shown capable of both
   readings against known-good and known-bad frames first). Since part 45's
   unlimited arm did fix it, the UI buffer is ABOVE 256 KB, and the size
   histogram prices exactness there at 121+ MB/frame. **Size is the wrong
   discriminator**, and it took a measured null to say so.
2. *Earn exactness instead.* A stream the store catches CHANGING is hashed
   exactly from then on; everything else keeps the sampled guard. Static world
   geometry never promotes; a per-frame UI buffer promotes whatever its size.
   Lazy, this cost 101-116 streams and **0.5 MB/frame**. The operator: *"UI did
   break at the start of being in game but then it seems to be good now"* — the
   hole the design has by construction, since a stream is sampled until its first
   VISIBLE change, and then self-heals. **A defect that repairs itself is a
   strong signal about its own mechanism**; it was the first evidence the policy
   was working at all.
3. *Close the window by inverting the presumption for a new entry* — exact for
   its first few observations, demoted once it proves static. Unbounded this cost
   **838 streams and 66.8 MB/frame**, i.e. as much as hashing everything, which
   is the entire cost the policy exists to avoid: a streaming world meets new
   geometry continuously, so "new entries only" is not a small population. With a
   **4 MB/frame budget** spent on unprobed entries it works and the hole closes
   over a few frames instead of instantly.

`CZ_VK_NO_DYNAMIC_GUARD=1` is the same-binary control arm, and the operator ran
it: with the fix the HUD self-healed and stayed healed; with the control it broke
at the start, recovered, and **relapsed later in the session**. One session a
side and judged by eye, so it is a pointer rather than a proof — but it is the
right pointer and it agrees with the mechanism.

**THE COST, stated rather than buried.** On the headless outdoor route the
promotion reads 290-323 streams and ~18 MB/frame and does not move the frame time
(42.0-44.5 ms at ~5,000 draws against a 42.9 ms baseline at 5,241). **On the
operator's own session it reads 451-676 streams and 33.9-48.3 MB/frame** — twice
the headless figure, the same factor by which their draw path exceeds ours
(§6cc). That is not free, and it has NOT been A/B'd for frame rate on their
configuration: their control-arm run was on an earlier build, so the two are not
comparable. **The owed measurement is `CZ_VK_NO_DYNAMIC_GUARD=1` against the
default on the SAME binary, on their route.**

Their frame, on this build: 14.2-18.9 fps at 5,394-7,148 draws, draw path
67-72%, `textures` 40-43% of it. So the guard is not what makes their frame slow
— `textures` still is — but it is a real addition to a frame that is already
over budget, and shipping it without pricing it on their machine would be
exactly the trade this project keeps refusing to make blind.

## 6cd. Part 47: PERFORMANCE — the texture revalidation guard was almost the whole
## texture phase, and its upper bound is far above what the plan estimated

Part 47's subject was set by the operator: performance, "as close as you could
run it on an Xbox 360". The plan is `docs/perf-plan-part47.md`, written against
their own profiled frame — **61.7 ms at 7,231 draws, target 33 ms** — and its
first instruction was one run, because item 1.1's upper bound is knowable in one:

> "Item 1.1's upper bound is knowable in one run (`CZ_VK_NO_TEX_REVALIDATE=1`),
> and that is the first thing to do — if it does not move the frame by ~10 ms,
> this entire plan's top item is wrong and the ranking should be rebuilt."

### The measurement: `CZ_VK_NO_TEX_REVALIDATE=1` on the outdoor route

Two runs an arm, alternated, `tools/part47_perf_ab.sh`, the unattended DebugJump
route with the title's own AI driving. Read as CZ_VK_PROFILE **phase shares**
rather than frame time, per the plan's §6, because this title paces to a
two-vblank floor that absorbs a saving the phase share still records.

| | draws/frame | frame | `textures` | frames in 600 s |
|---|---|---|---|---|
| default (guard on) | 4,155-4,626 | 36.7-38.6 ms | **39.4-40.8%** | 15,599 |
| `CZ_VK_NO_TEX_REVALIDATE=1` | 5,402-6,375 | 32.1-32.8 ms | **7.8-8.3%** | 18,225 |

**The guard is nearly the whole texture phase**, and the arm without it renders
MORE draws in LESS time — 16.8% more frames over the same wall clock. On the
operator's frame, where `textures` is 42.9% of 61.7 ms, the same proportion puts
the guard at roughly **21 ms of their 61.7**, against the plan's estimate of
8-11. The plan's ranking is right and its top item is worth about twice what it
was priced at.

That arm is NOT a shippable configuration — it is the defect part 38 fixed (a
streaming-recycled address serving its first occupant forever; the tanker wearing
a brick wall). It is an upper bound, and its whole value is that it is one run.

### What the guard was actually doing

From the same session's own census: **97,041,062 cache hits checked over 15,599
frames — 6,221 a frame — reading 1,376,540 MB, i.e. 88.2 MB a frame, to catch
575 real changes.** The operator's session reads 92.9 MB/frame to catch 986 in
26.8 M (0.0037%). `UploadTexture` runs once per texture fetch per draw, so a
texture that many draws of one frame share was re-hashed once for each of them.

Two properties of that population decide the fix, and both are printed:

* the addresses that DO change are **tiny** — 8x8 to 64x16, formats 18 and 20 —
  while the bytes are spent on large surfaces, every one of them capped at the
  16 KB the guard borrowed from the STREAM guard without anyone choosing it;
* a change is a *recycled address* — an entirely different texture written over
  the old one — not a subtle edit, which is what the stream guard hunts.

### A caveat on this A/B's own conditions

`base_2` ran with light single-threaded background work on the same machine
(two analysis scripts, `nice -n 19`, on a 16-core box while the runtime uses
three threads). `base_1` and both `norevalidate` runs were clean. The effect
being measured is a **five-fold** difference in the `textures` phase share, which
is two orders of magnitude outside anything that could explain, so the conclusion
does not depend on it — but a run's conditions are part of its record and this
project has been bitten by unrecorded ones before (gotchas 50/51/86). Quote
`base_1` where a single clean baseline is wanted.

### The four changes part 47 made, and what each one is

Each has its own same-binary control arm, and each is a separate commit, because
an item without an arm cannot be shown to have engaged (gotcha 151) and items
bundled together cannot be attributed.

| item | change | arm |
|---|---|---|
| 1.1 | the content guard runs **once per frame per cache entry**, not once per fetch | `CZ_VK_TEX_GUARD_EVERY_FETCH=1` |
| 1.2/1.3 | `Count`→`COUNT` on 21 hot sites; the per-fetch linear scan behind its readers' gate | none — mechanical, and the counters must read identically |
| 2.1/2.2 | the PM4 walk writes register RUNS in bulk, asking the scratch and const-watch range questions once per run | `CZ_PM4_NO_BULK_REGS=1` |
| 3.1 | the state cache covers the **vertex and index bindings** | `CZ_VK_NO_BUFFER_BIND_CACHE=1` |

Plus one that is too small to have an arm and is provably equivalent: the
per-fetch sampler lookup is a flat 512-entry table rather than a `std::map` over
a nine-bit key.

**Item 1.1 is a cadence change, not a mechanism change**, and that is what makes
it safe to reason about: what it costs is at most one frame of staleness for a
texture the guest rewrites mid-frame, against a picture that only updates once a
frame anyway. The falsifiable claim registered with it is that **`changed` does
not fall between the arms** — a real change is still there at the next frame's
first fetch — and the `texture guard cadence:` line reports both the skipped
share and the fetch-to-texture redundancy factor, which nothing in this runtime
had ever measured.

**Item 3.1 was measured before it was written**, which is what part 18 asked for:
it added the repeat counters and deliberately did not act on them, because a low
repeat rate kills the idea for free. The rate came back **51.0% vertex and 39.4%
index over 16.17 M draws** on the operator's session (55.1% / 43.3% headless),
i.e. ~11,900 and ~2,700 `vkCmd` calls a frame that need not happen.

### What the fixes measured, on their own instruments

**The guard cadence — the number that decided item 1.1's size, and nothing in this
runtime had ever measured it:**

```
texture guard cadence: 49,017,332 of 52,499,796 checks skipped because the entry
                       was ALREADY validated this frame (93.4%, i.e. 15.1x less hashing)
```

**15.1x**, not the 2x a first estimate off the session totals suggested — that
estimate divided per-frame fetches by addresses seen over a whole RUN, which is
the population error gotcha 242 names. Per frame the distinct-descriptor count is
far smaller than the run's, so the redundancy is large. The guard's remaining cost
is 6.6% of what it was, which PREDICTS `textures` at roughly the no-revalidate
arm's 2.3 ms plus 0.9 — i.e. recovering nearly all of the 13.6 ms upper bound
while keeping the mechanism. **That is an arithmetic prediction off a counter, not
a measurement**; the measurement is the `perf2` A/B below, and the distinction is
the one this project keeps insisting on.

**The bulk register path, verified against the code it replaced** (see
`CZ_PM4_VERIFY_BULK_REGS`, with `CZ_PM4_VERIFY_POISON` as its positive control):
**0 mismatches over 152,020,384 register dwords**, and **100.0% of dwords take the
bulk path** — the per-dword fallback for runs touching the scratch mirror or the
const-watch window never fires on this title, because its constant banks live at
0x2000 and above while the scratch registers are at 0x0578. ns-per-packet reads
93-97 against the 154 ns baseline *with the verifier still on*.

**The bind cache** reports `vertex 55,686,751 of 104,798,941 repeat the previous
offset (53.1%), index 11,935,810 of 29,871,146 (40.0%)` — the operator's 51.0% /
39.4%, reproduced.

**The sampler flat table's registered prediction held exactly**: the four
`[vk] sampler #N:` lines are identical in number, order and content to the
previous build's, which is what says a `std::map` over a nine-bit key and a
512-entry array are the same function.

### The one lever left in item 1.1, now priced rather than argued

The guard's bytes, by SOURCE size, over that run (reads are already capped at the
16 KB bound, so the MB column is true cost and not a size sum):

```
8K=6,896/26.9MB(0%)   16K=1,106,624/8,645.5MB(19%)   32K=475,001/7,421.9MB(16%)
64K=542,464/8,476.0MB(19%)   128K=357,371/5,583.9MB(12%)  256K=735,508/11,492.3MB(25%)
512K=181,147/2,830.4MB(6%)   1024K=60,997/953.1MB(2%)     2048K=16,456/257.1MB(1%)
```

Four fifths of the bytes are spent on textures whose source is 32 KB or larger,
every one of them read at the 16 KB cap. `CZ_VK_TEX_GUARD_BYTES=N` exists now and
**its default is unchanged at 16384**, because lowering it is the one option in
§1.1 that trades detection for cost and it needs its own measurement and an
operator's eyes. What the histogram makes possible is choosing it from data: the
per-address `changed` table now carries each texture's source size, so "what would
this bound stop being able to see" is a column and not an argument.

### THE MEASUREMENT: part 47's default against the three part-47 arms, same binary

`~/DR2CZ-troubleshooting/part47/perf2`, three runs an arm, alternated, the
unattended DebugJump route. `pre47` = `CZ_VK_TEX_GUARD_EVERY_FETCH=1
CZ_PM4_NO_BULK_REGS=1 CZ_VK_NO_BUFFER_BIND_CACHE=1`, i.e. the three part-47
changes off and nothing else.

**Both negative controls read exactly zero**, which is what says the arms engaged:
`texture guard cadence: 0 of 100,556,024 checks skipped (0.0%)` and `0.0% bulk`.

**Frame time by draw bin — the decisive table, and the statistic that moved is the
PINNED share (gotcha 238):**

| draw bin | part 47 | pre-47 |
|---|---|---|
| 3,000-5,000 | **32 ms, 98/98/98% pinned** | 40/32/38 ms, 7/66/15% pinned |
| 5,000-8,000 | **32 ms, 73/74/85% pinned** | 43/46/42 ms, 13/11/5% pinned |
| 8,000+ | **36-37 ms** | *the pre-47 binary never reaches this bin* |

At crowd density the frame goes from 42-46 ms to **32 ms and 73-85% pinned to a
16 ms multiple** — it stops being CPU-bound and becomes pacing-bound, which is
what "as close as you could run it on an Xbox 360" means on this title. The
part-47 binary also renders a whole draw band the pre-47 one never reaches, and
does it at 36-37 ms.

**Phase cost in milliseconds, matched 3,000-8,000 draw band:**

| phase | part 47 | pre-47 |
|---|---|---|
| `textures` | **2.47** | 17.18 |
| draw path | **13.50** | 27.90 |
| `record` | 7.15 | 6.76 |

`textures` is 2.47 ms against the no-revalidate arm's 2.3 — i.e. **the cadence fix
recovers essentially the whole of the upper bound while keeping the mechanism.**
The guard reads **5.8-7.4 MB/frame against 77.9-95.1**, a 12x cut.

**A caveat on `outside` and `record`, stated because the table above invites the
wrong reading**: those two columns come out slightly WORSE on the part-47 arm, and
the comparison is inadmissible rather than the result being bad. The two arms do
not submit the same command stream — the faster arm reaches denser content, and
**packets per frame differ by 40%** (101,258-112,270 against 53,543-73,807). A
matched DRAW band does not match a PM4 workload. The admissible statistic for the
walk is its cost per packet, which is normalised by the work:

```
part 47:  110, 111, 111, 111, 112, 112, 112, 113 ns   (100.0% bulk)
pre-47:   151, 152, 155, 156, 157, 157, 157, 158 ns   (0.0% bulk)
```

**Zero overlap across nine windows an arm, -29%.** That is item 2.1's result;
`outside` in milliseconds is not.

### The registered falsifiable claim for item 1.1, and its answer

The claim was that `changed` must not fall between the arms — a real change is
still there at the next frame's first fetch, so a drop would mean the
once-per-frame cadence is losing detections.

| | changed / frame | distinct addresses ever caught changing |
|---|---|---|
| part 47 (once per frame) | 0.0739, 0.0607, 0.0847 | 131, 141, 157 |
| pre-47 (every fetch) | 0.0668, 0.0640, 0.0508 | 156, 184, 157 |

**On the event rate the claim holds and the fix detects slightly MORE** (median
0.0739 against 0.0640) — the ranges overlap, so the honest statement is that no
detection loss is measurable there.

**On the DISTINCT-ADDRESS measure the two barely overlap and the every-fetch arm
is higher** (median 157 against 141), while the part-47 runs cover *more* ground
(18,000 frames and 9,252-9,498 peak draws against 15,000-16,800 and 7,817-8,424).
That is a small difference in the direction of a real detection loss, and this
route cannot resolve it: the arms do not visit the same places, so "more addresses
seen changing" and "more places visited" are confounded. **Recorded as an open
question rather than dismissed** — it is the kind of small honest discrepancy that
turns out to matter, and the thing that would settle it is the operator's own
session, where the same route is walked twice. It is question 2 of
`docs/part48-kickoff.md`.

### THE OPERATOR'S OWN A/B — the confirmation the headless route could not give

Their session, `~/DR2CZ-troubleshooting/part47-operator/`, two chained arms of ONE
binary (`tools/part47_operator_session.sh`), their own route, the gas-station spot
they name as the worst for frame rate. Three minutes an arm at the same place.

Their words: *"performance is way better, still far from perfect"*, *"pretty much
10 fps difference between the two"*, and — the answer to the question part 47's
fix could have failed — *"games looks pretty much the same as last time"*, with
**no stale texture reported**.

Matched on draw count (6,500-7,600 band, 15 windows against 10):

| | part 47 | pre-47 |
|---|---|---|
| frame | **42.8 ms — 23.4 fps** | 64.1 ms — 15.6 fps |
| `textures` | **4.45 ms** | 25.19 ms |
| `outside` | 16.61 | 18.24 |
| `record` | 15.19 | 14.20 |
| other + constants | 5.56 | 5.70 |

**-21.3 ms and +7.8 fps at matched draws**, and against part 46's profile of the
same machine (61.7 ms at 7,231 draws) the texture phase is **26.5 -> 4.45 ms**.
Item 1.1 is confirmed on the only workload that decides.

**`record` comes out 1 ms HIGHER on the part-47 arm and that is NOT established as
a regression** — the same sign appeared headlessly (7.15 against 6.76) and the
reader put it INSIDE a 22.8% within-arm floor. A matched draw COUNT is not a
matched draw COMPOSITION: the operator played two three-minute stretches at one
spot, not the same stretch twice, and `record` scales with per-draw vertex and
stream sizes as well as with draw count. **But the vertex/index bind cache is the
one part-47 change never isolated in an A/B of its own**, and if it is a net loss
this is where that would show. `CZ_VK_NO_BUFFER_BIND_CACHE=1` exists precisely for
that and the measurement is owed.

### What their frame is made of NOW, and the finding that is specific to it

At ~7,010 draws and 42.8 ms:

| phase | ms | |
|---|---|---|
| **`outside`** | **16.61** | 81,533 packets/frame at **144 ns each** |
| **`record`** | **15.19** | **2.17 µs per draw** |
| other | 4.19 | |
| textures | 4.45 | closed |
| constants + readback | 2.2 | |

**Their packets cost 144 ns where the headless route measures 110-113 on the SAME
binary, and the reason is in the data: their packets carry 7.8 register dwords
each against the headless 9.4.** Their frame submits a different packet MIX, with
proportionally more non-register packets, so the bulk-register path of item 2.1
buys them less than it bought the headless route and what dominates their walk is
**per-PACKET** overhead. That makes the census counters — four atomic
read-modify-writes per packet, ~326,000 `lock xadd`s a frame on their mix — the
correctly aimed next item rather than a footnote (`part48-kickoff.md` 1b).

**And `record` is 2.17 µs per draw against the headless route's ~1.3.** It is now
the largest draw-path term on their frame and it is **completely uninstrumented
inside** — exactly the state the PM4 walk was in before it got its packet census,
and the state that made "the walk is 11 ms" support no hypothesis about what to
change. Splitting it is the prerequisite to costing it.

## 6ce. Part 48: THE COUNTERS WERE THE COST, TWICE — and splitting a profiler phase
## refuted the plan that asked for the split

Part 48's subject is unchanged and set by the operator: *"For now performance is
the most important."* The plan is `docs/perf-plan-part48.md`, built on their own
part-47 frame — **42.8 ms at ~7,010 draws, target 33 ms**.

Its first instruction, and this is the part worth transferring, was **not** an
optimisation. It was to print a number that had been collected for twenty-odd
parts and read by nobody.

### §1. `Pm4_OpcodeCount` was incremented on every packet and called from NOWHERE

`Pm4_OpcodeCount(opcode)` and `Pm4_TypeCount(type)` have existed since phase 4.
Every packet the command processor executed bumped one of each. **Neither was
called anywhere in the runtime.** So "which packets cost the 16.6 ms of PM4 walk"
— the largest single term on the operator's frame — was unanswerable while the
answer sat in memory, and the walk had been optimised twice without it.

Twenty lines, differenced per profile window like every other rate on those
lines. On the outdoor route at ~6,400 draws:

```
[vkprof] pm4 types: t0(reg-run) 34.5% t1(reg-pair) 0.0% t2(filler) 28.7% t3(command) 36.8%
[vkprof] pm4 opcodes: SET_BIN_MASK_LO 9061/f (11.9%)  DRAW_INDX 5975/f (7.9%)
                      EVENT_WRITE 4557/f (6.0%)  EVENT_WRITE_EXT 4486/f (5.9%)
                      IM_LOAD 1556/f (2.0%)  LOAD_ALU_CONSTANT 1839/f (2.3%)  ...
```

Two things nobody had predicted, and neither is small:

* **`SET_BIN_MASK_LO` is the most frequent packet in the whole stream** — a third
  of all type-3 packets, and *half again as many as there are draws*. The guest
  rewrites the bin mask's low dword one and a half times per draw.
* **28.7% of every packet walked is type-2 ring FILLER**, which does no work
  whatsoever and, before this part, still paid two atomic read-modify-writes.

### §2. Those atomics were the item, and they are now per-thread

`ExecutePacket` did four `lock xadd`s on an ordinary packet — packets, types,
opcodes, regWrites — and a fifth on a draw. On the operator's 81,533 packets a
frame that is roughly **326,000 bus-locked read-modify-writes**, ~20 cycles each
even completely uncontended, for pure instrumentation. Gotcha 230's defect class,
one subsystem over from part 47's items 1.2 and 1.3.

The counters are **not** removed — they are the only description this project has
of the thing it is optimising. They become one instance per walking thread,
summed by the accessors, with the fields still `std::atomic` because the storage
class is not what costs: a relaxed `store(load() + n)` is `mov`/`add`/`mov` with
no bus lock, where `fetch_add` is `lock xadd`.

**Verified against the code it replaces**, because no gate in this repo can see
inside `ExecutePacket` (gotcha 322): `CZ_PM4_VERIFY_COUNTERS=1` drives BOTH forms
from every call site and compares all 135 counters. "Compare two runs' totals"
would not do — nothing about this title's packet stream is reproducible run to
run. Within one run the two are bumped by the same call with the same argument.

| arm | result |
|---|---|
| `CZ_PM4_VERIFY_COUNTERS_POISON=1` | **1 of 135 DISAGREE** — the check can fail |
| `CZ_PM4_VERIFY_COUNTERS=1` | **0 of 135**, 13 windows, outdoor route |

The comparison is exact because exactly one thread walks (`vd.cpp` is the only
caller of `Pm4_Execute` and says so), and the report prints the walker count so
that is checked every run rather than trusted. It read 1.

### §3. Splitting `other` REFUTED the plan section that asked for the split

`drawOther` was 4.19 ms of the operator's frame and was four things wearing one
number. `docs/perf-plan-part48.md` §5 predicted: *"expect the pipeline-key build
and its `std::map` lookup to be most of it"*, reasoning that a red-black tree
probed once per draw is the same shape as the sampler `std::map` part 47 replaced
with a flat table.

Measured, outdoor route at ~5,000 draws:

```
[vkprof] other 12.2% = key 0.7 + pipeline 2.0 + fetch 4.1 + residual 5.5
         per draw:  735 ns =  40 +        118 +       246 +          329
```

**The pipeline probe is 16% of it.** The largest term is the residual at 45% and
the fetch walk is second. The plan's tier-3 ranking was wrong, and it was wrong in
the same direction the profiler has been wrong every previous time: toward the
thing that has a name.

### §4. ...and within minutes the split found a `getenv` on the per-draw path

`otherFetch` at 246 ns/draw was worth reading, and the read was immediate. The
alpha-to-mask block ran

* `EnvOn("CZ_VK_NO_ALPHA_TEST")` — a raw `getenv`, i.e. a linear walk of the
  environment block — **once per draw**, while every other environment read in the
  same twenty lines is already a function-local static; and
* on its declined branch, an `snprintf` building a counter name followed by a
  lookup in a `std::map<std::string, uint64_t>`. **That branch is the common
  one**: 598,304 draws took it in the run that measured this.

Fixed as a static, a `COUNT(literal)`, and four literals for the four values the
formatted name can take. **`otherFetch` 246 → 119 ns/draw and `other` as a whole
735 → 571**; at the operator's ~7,000 draws that is ~1.15 ms of a 42.8 ms frame.

No control arm, because there is nothing to compare that is not strictly worse:
the change is equivalent by construction and the check is that the counters still
print the same names with real counts (`msaa=1 … dither declined 598304`), which
is the one thing a wrong transcription of four literals would break.

### What generalises

**Three items in two parts have now been found by SPLITTING a profiler phase, and
none of them by reading the code.** Part 47's `record` split found the stream
guard (81.65 MB hashed in a frame, charged to a phase whose name did not mention
it); part 48's `other` split found the per-draw `getenv`; and item 1a's census —
also a split, of the walk by opcode — found that a third of the packets are
filler. A phase name is a **scope**, not a subsystem, and what is expensive inside
it is routinely not what the name says (gotchas 325, 326).

The second half is worth saying separately: **a counter that is collected and
never printed is worse than no counter**, because it makes the question look
answered. `Pm4_OpcodeCount` was in the header, in the accessor list, and
incremented on the hottest loop in the runtime, for twenty parts.

### §6ce addendum — THE OPERATOR'S SESSION: 33.6 ms AND 29.8 fps AT THE SPOT THEY NAME
### AS WORST, i.e. THE PLAN'S TARGET IS MET — and both items are confirmed on their frame

Three arms, `tools/part48_operator_session.sh`, played by the operator on their own
machine and route. **They soaked at the gas-station spot in every arm**, which is
what makes this the cleanest comparison this project has ever had on their hardware:
long stationary windows at ~7,000 draws in all three, and a band check reading
**0.0% drift** across 6,800-7,500 draws.

| arm | frame | fps | what it isolates |
|---|---|---|---|
| **default (all of part 48)** | **33.6 ms** | **29.8** | — |
| `CZ_VK_GUARD_FOLD_SERIAL=1` | 40.5 ms | 24.7 | **part 47's four-lane guard fold: 6.9 ms** |
| `CZ_PM4_ENV_PER_PACKET=1` | 38.1 ms | 26.2 | **part 48's PM4-walk `getenv`: 4.5 ms** |

Against their part-47 session — 42.8 ms and 23.4 fps at 7,010 draws — that is
**−9.2 ms and +6.4 fps**, and **`docs/perf-plan-part48.md`'s target of a 33 ms frame
is met**: the Xbox 360 shipped this game at 30 fps and their worst spot now runs at
29.8.

**Action zero is confirmed to within 0.1 ms of its prediction.** The plan predicted
the guard fold would be worth ~6.8 ms on their 81.65 MB/frame of hashing; it measured
6.9. In per-draw terms it *halves* the vertex section of `record`:

```
                base   envpkt (null)      fold
  record        1357   1380  +1.7%     2172  +60.1%
  rec.vertex     776    788  +1.6%     1547  +99.4%
  other          721    732  +1.5%      717   -0.6%
```

**THE NULL CONTROL IS THE REASON THOSE NUMBERS CAN BE BELIEVED** (gotcha 331).
`CZ_PM4_ENV_PER_PACKET=1` changes only the command processor and therefore cannot
move any draw-path phase by any mechanism, and it reads **+1.5% to +4.9% across all
thirteen** of them. That IS the floor, measured rather than assumed — and the fold's
99.4% is 61 times it.

The walk agrees independently: **136 → 95 ns per packet**, with the packet mix
matched (7.4 against 7.5 register dwords per packet, filler 28.6% against 28.7%), so
the comparison is admissible by this reader's own test. At their ~100,000 packets a
frame, 41 ns/packet is 4.1 ms — against the 4.5 ms the frame time says. Two
statistics, different denominators, agreeing.

### The second question, which is the one that could have gone wrong

Both of part 47 and part 48's biggest wins are **content guards**, and the only
symptom either can produce is a STALE surface. Asked directly, the operator's answer
was: *"Some looked wrong but they are not new so no issue with your fix from what I
can see."* That is the second consecutive session in which the guard work comes back
clean, and it attributes the wrong-looking surfaces to the pre-existing open items
00m and 00n, which they had already filed and deferred.

### What their frame is made of NOW, and it is a different shape from part 47's

At 33.6 ms and ~7,000 draws, from their own profile:

| term | ms | note |
|---|---|---|
| PM4 walk | ~9.5 | ~100,000 packets at 95 ns |
| `record` | ~9.5 | of which **`vertex` is 5.4** |
| `other` | ~5.0 | `shader` 98 + `key` 37 + `pipeline` 122 + `begin` 107 + `fetch` 112 + `tail` 39 + **`residual` 205** ns/draw |
| `textures` | ~3.6 | closed in part 47 and staying closed |

Three things in that table are new information and none of them existed this morning:

* **`rec.vertex` at 776 ns/draw is the largest single draw-path item**, and the
  cross-frame store's content guard is still reading **63-72 MB every frame** inside
  it. Part 47 made that hash four times faster; it did not make it SMALLER, and the
  fold arm proves the hash is most of the section.
* **`oth.begin` is 107 ns/draw ≈ 0.75 ms a frame** for `BeginFrame` + `BeginRendering`
  — work that is supposed to happen once per frame. Nothing had ever measured it.
* **`oth.residual` is 205 ns/draw ≈ 1.4 ms** and is still unnamed after two splits.

## 6cf. Part 49: THE 30 fps CAP IS THE TITLE'S OWN SETTING, THE 60-OR-NOTHING WAS THE
## VBLANK LADDER, AND "ordinary gameplay is 31 fps and CLOSED" IS RETRACTED

The operator's request, and it was framed as diagnosis rather than comfort: *"we'll
implement a 60 fps cap ... so it'll help us better see what's the performance issue
without hitting 30 fps cap all the time."* That framing is correct and it is gotcha
237/238's problem stated from the other end — part 48 had put the frame ON the
two-vblank floor (at >=5,000 draws, 4,452 frames of 12,009 at exactly 32 ms and
another 3,570 at 33), so a CPU saving below 33 ms measured as exactly zero.

### §1. Where the 30 fps actually came from — the title, not us

Traced end to end through the recompiled image:

```
game config 0x82A57ACC
  -> sub_823C8D20   0->2, 1->1, 2->2, 3->3, >3->0
  -> sub_827CBB00   0->0x80000000, 2->2, 3->4, ELSE->0
  -> sub_8283E920   `stw r4,13804(r3)` — a one-instruction setter; THE FIELD
  -> sub_82841AD0   0/1->1, 2->2, 4->3;  packs (n << 8)
  -> sub_82841878   due = cursor + n
  -> sub_82841760   the vblank walker retires only when due <= tick, and retiring is
                    what clears the word our WAIT_REG_MEM polls
```

**Reading that chain backwards is the finding**: the game's own "vsync 1" setting
produces device value 0, i.e. present interval 1. **A 60 fps mode is a configuration
this title already ships with**, not a defeat of its pacing, which matters because
§6am says in terms that the two-vblank wait "must not be optimised".

### §2. The registered claim, and it could have made the whole thing a lie

If the engine took its delta time from the vblank COUNT rather than from the timebase,
presenting twice as often would run the whole game at DOUBLE SPEED. Registered before
running: locomotion p90/p95 near 3.38/3.58 units per WALL second if delta-time based,
near 6.8/7.2 if frame-locked.

| arm | loco s | p75 | p90 | p95 |
|---|---|---|---|---|
| cap30 #1 | 129 | 1.28 | 3.38 | 3.58 |
| cap30 #2 | 129 | 0.78 | 2.28 | 2.48 |
| cap60 | 193 | 1.63 | 2.80 | 3.57 |

**p90 0.99x. Nothing near 2.00x.** The null control — two runs of the same config —
is 31-39%, so this rules out a doubling and not a 10-20% difference. The operator's
verdict in play settled the rest: *"the game plays perfectly"*.

A METHOD NOTE, because the first oracle was thrown away: holding the stick forward was
tried first and gave zero signal — Chuck walked into geometry within seconds and both
arms ended at the IDENTICAL position, p90 0.00 each. **A held stick is a locomotion
oracle only where there is somewhere to walk.**

### §3. TWO more ceilings appeared the moment the first one lifted

* **The host's vsync.** The SDL renderer was created without
  `SDL_RENDERER_PRESENTVSYNC` and was never told NOT to vsync; a compositor throttles
  presentation regardless, and they are on Wayland. It hid for 48 parts because the
  guest's own 30 fps cap was always the slower clock. **The operator diagnosed it from
  the shape of the number** — *"pretty sure it's vsync"* — while headless had been
  reading 62.5 fps throughout and nobody had asked why windowed disagreed (gotcha 332).
* **The vblank LADDER, which is the one that mattered.** Presents can land only on a
  vblank boundary, so at a 16 ms period the rungs are 16/32/48 ms and a frame needing
  17 ms falls to 31 fps. Raising the ceiling with the interval left that alone — again
  the operator, within minutes: *"when it is 60 fps the game plays perfectly"* but
  *"when it drops it still goes back to 30 fps"*. The fix is to shorten the PERIOD to
  8 ms and pin the title's OWN interval of 2: same 16 ms cap, half the rung.

A four-way sweep at matched draws, which also RETRACTS an intermediate claim that a
shorter period costs ~5 ms of CPU (that was two runs compared across time):

| vblank | interval | cap | fps | frame | `outside` |
|---|---|---|---|---|---|
| 16 ms | 1 | 16 ms | 61.7 | 16.2 ms | 6.3 ms |
| 8 ms | 2 | 16 ms | 61.8 | 16.2 ms | 6.1 ms |
| 8 ms | 1 | 8 ms | 61.4 | 16.3 ms | 6.1 ms |
| 16 ms | 2 | 32 ms | 31.2 | 32.0 ms | 21.5 ms |

### §4. THE RESULT, on the operator's own whole-map lap — 16,788 frames

| draws/frame | frame | fps | before |
|---|---|---|---|
| 0-1,500 | 16.0 ms | **62.5** | 31 |
| 1,500-3,000 | 16.0 ms | **62.5** | 31 |
| 3,000-5,000 | 23.0 ms | **43.5** | 31 |
| 5,000-7,000 | 28.0 ms | **35.7** | 31 |
| 7,000+ | 35.0 ms | 28.6 | ~25 |

**24.1% of frames at 57+ fps; only 3.6% below 30.** So **"ordinary gameplay is 31 fps
and CLOSED — that is the title's own two-vblank pacing and it will not go higher" is
RETRACTED.** It was true of the shipped configuration and false of the title, which
supports a 60 fps configuration and always did.

**What this did NOT do is make anything faster.** The CPU cost per frame is unchanged;
a ceiling that was hiding it was removed. That is exactly what it was asked for.

### §5. What the frame is made of now, and it is a different shape

At 5,000-7,000 draws, 28.3 ms:

| term | ms | |
|---|---|---|
| **`outside`** | **11.3** | the PM4 walk (**81,106 packets/frame at 100 ns = 8.1 ms**) plus the guest's own simulation |
| **`record`** | **7.4** | 1,250 ns/draw = state 172 + **vertex 662** + index 220 + residual 182 |
| `other` | 4.4 | 717 ns/draw = shader 96 + key 36 + pipeline 124 + begin 101 + fetch 114 + tail 40 + **residual 206** |
| `textures` | 3.1 | closed in part 47, staying closed |
| **GPU** | **0.0** | `submit.gpu` median 0.0% over 22 windows — **the GPU is idle and this is 100% a CPU problem** |

And a cost that was free to ignore at 30 fps and is not now: **the guest simulates
twice as often per second**, so `outside` per SECOND has roughly doubled, and
`readback` — our present path copying the image back to host memory to blit it — went
2.7% -> 4.7% of the frame for the same reason.

## 6cg. Part 50: THE PLAN'S TOP TWO ITEMS WERE BOTH REPRICED BY THE MEASUREMENT THAT
## PRECEDED THEM, and one of them turned out to be the profiler measuring itself

Part 50 executed `docs/perf-plan-part50.md` tier 1 and tier 3 and instrumented tier 2.
Its transferable result is not a millisecond count. It is that **three of the plan's
estimates were built on statistics that could not support them, and in each case the
cheap measurement that could was one counter away.**

### §1. Item 1a — the filler runs. A SHARE IS NOT A SHAPE

Part 48's opcode census established that **28.7% of every PM4 packet this title walks is
type-2 ring filler**, a one-dword no-op. The plan priced "skip runs of it" at 1.5-2 ms
on that number alone.

But a 28.7% share is equally consistent with one enormous run of padding and with 23,000
isolated dwords wedged between real packets, and coalescing is worth everything in the
first case and nothing in the second. **Nobody had ever measured the run length.** So the
histogram was built before the fast path, and it repriced the item immediately:

| | |
|---|---|
| mean run length | **2.24 dwords** |
| shape | **bimodal** — 28% singletons, 72% runs of 2-3, a thin tail of ~1,100 runs of 32-63 per window, and *nothing in between* |
| at ring level | **0%** |

Two consequences, neither of which the share could have suggested. First, coalescing in
the callee would have removed only **57%** of the calls, so the test was moved to
`ExecuteLinear`'s loop — which has already fetched the header for its own trail, so there
it is free and removes **100%** of them. Second, **this is not the driver padding the ring
to a wrap boundary**, which is what the word "filler" suggests and what the ring path is
written to expect; it is the title's own indirect buffers, padded packet by packet. Two
different producers, and only one of them was where the code expected it.

**The A/B refuted the plan's prediction.** Registered: 20-30 ns off the mean per-packet
cost. Measured, three runs an arm, alternated, in the 3,000+ draw band:

| arm | ns/packet, per-run medians | pooled |
|---|---|---|
| base (the fix) | 96.0, 99.0, 105.0 | 102.0 |
| `CZ_PM4_NO_FILLER_RUNS=1` | 105.5, 101.5, 110.0 | 106.0 |

**The null control is `base` against itself and it spans 96.0-105.0 — 9.4%.** The arm
effect is 4.0-6.5 ns/packet, i.e. **inside the noise floor**. What keeps the sign is that
all three rounds order the same way and the mechanism is not in doubt (12,267 calls a
frame provably removed), so the item is real and **worth ~0.3-0.5 ms, not 1.5-2 ms**.

**Its by-product is worth more than the item.** The difference divided by the calls
removed prices one `ExecutePacket` call at **24-40 ns**, and because a filler packet
returns before the zero-header check, the `avail` test and the body decode, that is a
LOWER BOUND on the per-packet fixed cost. At 74,767 packets a frame that is **~2.2 ms of
pure dispatch overhead** — which is item 1c's ceiling, now measured instead of estimated,
and it lands inside the plan's guessed 2-3 ms.

### §2. Item 3 — `other`'s residual IS THIS PROFILER, and the plan called it the best item

The plan: *"`residual` 206 ns/draw (~1.4 ms) — SPLIT IT AGAIN. Two splits have not named
it... This is the highest-yield-per-hour item in the document."*

It could never have been named by splitting, because **it is not in any of the code being
split.** Work through where `ProfScope`'s two clock reads fall. The constructor's read
happens between the parent's `t0` and the child's, so it is inside the parent's interval
and is **not** in `childNs` — nothing subtracts it. The `Close()` read is inside the
child's own measured total, so it lands in the child's named phase. **One whole clock read
per nested scope therefore accumulates in the parent's residual and nowhere else** — and
`drawOther`, printed as `other`, is DoDraw's outermost scope.

Measured rather than argued. `CalibrateProfNow` times the call; the scopes are counted;
the print multiplies them and puts the product next to the residual, as a claim that can
fail. `CZ_VK_PROFILE_EXTRA_SCOPES=N` is the positive control, and it did not fail:

| arm | scopes/draw | `other` residual |
|---|---|---|
| null | 14.3 | **205 ns** |
| `CZ_VK_PROFILE_EXTRA_SCOPES=8` | 22.3 | **397 ns** |

**+8 scopes moved the residual +192 ns — a slope of 24.0 ns per scope against a calibrated
clock read of 21.6 ns.** DoDraw opens ~8 scopes DIRECTLY inside `_pDraw` (the rest are
grandchildren, whose reads land in their own parents), and 8 x 24.0 = **192 ns of the
205 ns residual, 94% of it**.

**The item is retired.** There is no frame time there to save: it is absent from every run
that does not set `CZ_VK_PROFILE`. Gotcha 335, and it is a nastier shape than gotcha 7 —
an expensive probe usually distorts what it reports, but this one **files its own cost
under a name that invites you to go looking for a defect there**, and two parts did.

### §3. Item 2a — the guard's promotion, and a hypothesis of mine refuted by one counter

The stream guard reads 35.6 MB a frame, of which **26 MB is "promoted to exact"** — but
the budget for speculative promotion is 4 MB a frame, so most of it had to be arriving
through some other door and nothing in the runtime said which. Split three ways:

| door | streams/frame | MB/frame |
|---|---|---|
| **proven** — `needsExact`, **unbudgeted and permanent** | **388-483** | **21.8-29.4** |
| speculative — dynamic, still accruing proof (budgeted) | 29-37 | 0.0 |
| probe — newly met (budgeted) | 10-12 | 0.0 |

**This refutes part 46's expectation of its own mechanism.** That part wrote that
`needsExact` would be "the UI text buffer's small edits inside a large buffer and almost
nothing else". It is **15,643 of 126,536 store entries — 12.4%, rising monotonically
window over window, and the latch never unlatches.** A streaming world keeps meeting new
large buffers, so the set that pays the unbudgeted exact hash grows for as long as the
player keeps playing.

**And it refutes the hypothesis I formed about what to do with it, which is the part worth
keeping.** The argument was clean: a stream proven to change is one we are about to copy
anyway, so hashing it is a whole extra read of the buffer to learn what the copy is about
to tell us free — and always-copying would be **cheaper and safer**, since a stream always
copied can never be served stale. So the change rate inside the proven set was counted
before anything was built. **11-13% of proven observations find the stream actually
changed.** The guard saves the copy on ~88% of them and is doing exactly its job.

So item 2a's 26 MB a frame is not waste, and the item is NOT "stop hashing". It is the
narrower and harder question the counters now pose: **can a large buffer's change be
detected more cheaply than by reading all of it?** The sampled guard reads 16 KB of a
128 KB buffer and genuinely misses localized edits — that is why the latch fires, and it
fires correctly. The candidates left are all about asking the operating system rather than
the bytes (soft-dirty page tracking via `/proc/self/clear_refs` + `pagemap` would turn a
128 KB read into a 256-byte one and be EXACT), and that is architectural work, not a
tightening. It is written up as the live item rather than attempted at the end of a part.

### §4. What this does to the plan's budget, and it affects every number in it

`docs/perf-plan-part50.md` §1 is built on the operator's whole-map lap: 28.3 ms and
35.7 fps at 5,000-7,000 draws, with a full per-draw table. **Every one of those numbers
was read out of a run with `CZ_VK_PROFILE` set**, because that is the only way to obtain
the phase split at all — and §2 above establishes that the profiler charges ~24 ns per
nested scope into the enclosing residual plus a further read into each named phase, at
14.3 scopes per draw.

That makes the frame the operator actually plays **faster than the frame this project has
been quoting**, and the target correspondingly closer. The A/B that prices it is
`tools/part50_profiler_cost.sh` — three runs with the profile off, compared against the
campaign's own three `base` runs on the same pinned binary, using the `msec` column of
`CZ_VK_FRAME_STATS`, which is the one statistic that exists in both arms.

### §5. Item 1c — the plan's top candidate is worth nothing, and the census already said so

The plan lists three candidates for the per-packet preamble "in order of confidence", and
the first is: *"Hoist the wrap modulo. `Source::operator()` does `% ring` per dword
fetched. A walk that knows it is not near the ring end can index directly."*

**It is worth essentially nothing, and the opcode census this project has been printing
since part 48 already contained the refutation.** `INDIRECT_BUFFER` runs at **43-46
packets a frame** — 0.1% of the stream. So ~45 indirect buffers carry all ~75,000
packets, roughly 1,660 packets each, and **every one of those packets is fetched with
`wrapDwords == 0`**, where `Source::operator()` is a predictable branch and no modulo at
all. Only the ~45 ring-level packets that dispatch them ever take the modulo path. This is
the same shape as §1's filler finding — the ring is nearly empty of work and the title's
own buffers hold all of it — and it is the second time in one part that assuming "the
ring" was where the packets are cost an estimate its credibility.

The second candidate (skip the const-watch store when the window is empty) is one store of
a constant pointer per packet. So **item 1c's ~2.2 ms is real but has no single lever**:
it is the call, the fetch, the thread-local census read, two counter updates, the switch
and the return, none of them dominant. The only structural way at it is to inline the walk
so there is no call per packet, which is a real refactor of `ExecutePacket` with genuine
desync risk, and it should be costed as one rather than picked up as a tightening.

The per-frame mix it would be attacking, for whoever does:

```
SET_BIN_MASK_LO 8,848 (11.6%)   DRAW_INDX 5,377 (7.1%)   EVENT_WRITE 4,447 (5.9%)
EVENT_WRITE_EXT 4,375 (5.8%)    IM_LOAD 1,919 (2.5%)     LOAD_ALU_CONSTANT 1,694 (2.2%)
INDIRECT_BUFFER 43 (0.1%)
```

### §6. AND THE PROFILER'S BILL, MEASURED IN FRAME TIME — 2-4 ms, 8-18% of the frame

§2 established the mechanism and priced it per scope. This prices it where it matters, by
the one statistic that exists in both arms: the `msec` column of `CZ_VK_FRAME_STATS`,
which is written whether or not `CZ_VK_PROFILE` is set. Three runs an arm, the same pinned
binary, the same route, banded by draws:

| draws/frame | profile ON | profile OFF | delta | |
|---|---|---|---|---|
| 1,500-3,000 | 18.00 ms | 16.00 ms | **−11.1%** | **outside the 6.2% floor** |
| 3,000-5,000 | 22.00 ms | 18.00 ms | −18.2% | inside a 23.5% floor |
| 5,000-8,000 | 25.00 ms | 23.00 ms | −8.0% | inside a 16.0% floor |
| 8,000+ | 31.00 ms | 27.00 ms | **−12.9%** | **outside the floor** |

**Two of four bands clear their own noise floor and all four point the same way: the
profiler costs 2-4 ms a frame, 8-18%.** It is consistent with §2's per-scope model — 14.3
scopes/draw at ~45 ns of total bill is ~3.9 ms at 6,000 draws, against 2-4 ms measured,
the shortfall being the reads that overlap surrounding work.

**So `docs/perf-plan-part50.md` §1's budget describes a frame that only exists while it is
being measured.** The operator's 28.3 ms and 35.7 fps at 5,000-7,000 draws is really about
**25-26 ms and 39-40 fps** in play. That does not make any item in the plan less worth
doing, and it does not change their ranking — every phase is inflated, not one of them.
What it changes is the distance to the target: the plan's "creditable intermediate" of
20 ms (50 fps) is roughly **three milliseconds closer than the plan believed**, and the
16 ms goal is 9 ms away rather than 12.

Two rules follow, and they are cheap:

* **Never quote a frame time from a profiled run without saying so.** This project has
  been doing it since part 30 and the error was invisible while the frame was pinned to a
  32 ms floor, because a floor absorbs an 8% inflation without moving. Part 49 removed the
  floor; this is the first part that could have seen it.
* **A frame-time A/B between a profiled and an unprofiled arm is possible ONLY because
  `CZ_VK_FRAME_STATS` is independent of `CZ_VK_PROFILE`.** Keep them independent. An
  instrument that can only be read through another instrument cannot measure that one.

### §7. HOW MANY CORES ARE WE ACTUALLY USING? 2.5 OF 16 — and the plan never asked

Every performance number this project has produced since part 18 is a wall-clock frame
time or a share of one, and both are blind to the question a reader asks first: **is this
a single-core problem or a parallel one?** A 25 ms frame with one thread saturated and
fifteen cores idle is a completely different item from a 25 ms frame with eight threads
half busy. `tools/part50_thread_cpu.py` reads it out of `/proc/PID/task/*/stat`,
differenced over a window so it describes the workload running *now* rather than the boot.

Sampled over 25 s in an outdoor crowd (6,563 draws, ~22-26 ms/frame), 16 cores:

| thread | % of one core | what it is (from its stack) |
|---|---|---|
| 630402 | **93.2%** | a GUEST thread — `GuestThread::Run` -> `sub_82829BB0` -> `sub_827D3898` |
| 630391 | **79.0%** | **OURS** — `GraphicsInterruptPump` -> `Pm4_Execute` -> `ExecuteLinear` -> `ExecutePacket` -> `DoDraw` |
| 630380 | 29.1% | a second guest thread |
| 630403 | 10.5% | |
| 17 more | < 6% each | |
| 20 more | **< 0.5%** | |

```
process total   246.2% of one core = 2.46 cores of 16 (15.4% of the machine)
busiest thread   93.2%  -- 38% of all CPU this process is using
```

**37 threads exist and two of them carry 70% of the CPU.** The capacity is there on both
sides and is not being used: the title is built to be parallel (A1 shows it naming
`JobThread0`..`JobThread5`, `cAsyncFileSystem` and `BigFile Decompress Thread`), and our
runtime gives every guest thread a real host thread with no affinity pinning — the
processor mask is only reported back through the PCR so the guest reads what it asked for.
**Twenty of those threads are below 0.5%.**

Three things follow, and none of them are in `perf-plan-part50.md`.

**1. OUR PUMP DOES THE PM4 WALK AND THE VULKAN RECORDING ON ONE THREAD.** The stack shows
it directly: `Pm4_Execute` -> `ExecutePacket` -> `DoDraw`. So `outside`'s walk (~8 ms) and
the entire draw path (`record` + `other` + `textures`, ~15 ms) are **serialised on a single
core by construction**, and every item in the plan is an attempt to make one core's work
smaller. That is a legitimate strategy and it is not the only one available.

**2. THE BUSIEST THREAD IS THE GAME'S, NOT OURS — and it is nearly saturated.** 93.2% on
recompiled title code is a thread with almost no headroom, and we cannot optimise it
directly: it is the guest simulating. Whether it is genuinely the critical path or merely
busy is NOT established here and must not be assumed — a guest thread spinning on a lock
would look identical in this measurement. **That is the first thing part 51 should
resolve**, because if the simulation thread is the limiter then several milliseconds off
our pump buys nothing at all, and the plan has no item that would find that out.

**3. `outside` IS NOT ALL WORK.** Our pump is **79% busy**, so on a 22.3 ms frame it is
blocked for ~4.7 ms. `outside` read 47.8% of that frame (10.7 ms) — and the plan reads
`outside` as "the PM4 walk 8.1 ms plus the guest's own simulation, ~3 ms". **The guest's
simulation is on a different thread and cannot be inside our pump's frame time except as
blocking**, so that ~3 ms is not the guest computing, it is our pump WAITING for it. The
walk item is therefore smaller than `outside` makes it look, for the third time in this
part that a number was read as work when part of it was something else.

**What this does NOT say.** It does not say the frame is limited by single-threading — a
79% pump has 21% slack, and a change that halves the pump's work would still help if the
pump is the limiter. It says the machine has ~13 idle cores, that we have never asked why,
and that **the parallelism question outranks several of the plan's remaining items on
expected value while costing one run to investigate**. Multithreaded command recording is
already named in the plan's §5 as deferred "for the same reasons" as the swapchain work;
this is the measurement that says how much it might be worth and it should be re-costed
rather than left at the bottom.

## 6ch. Part 51: THE BUSIEST THREAD IN THE PROCESS IS NOT WORKING, IT IS WAITING FOR US —
## and the plan's largest remaining item is refuted by two costs nobody had priced

`docs/part51-kickoff.md` opened with a question part 50 could not answer and correctly
refused to guess at: our pump is 79% busy, a GUEST thread is at 93.2%, and **a CPU
percentage cannot tell WORKING from SPINNING** (gotcha 338). If that thread is the title
simulating and it is saturated, then every item in `docs/perf-plan-part50.md` — all of
which make our pump's work smaller — buys nothing.

It is spinning, it is spinning on **us**, and the instrument that says so is one command.

### §1. Item 0 — the 93% thread is the Draw Thread, and 84% of it is a fence spin

`tools/part51_thread_probe.sh` drives the standard unattended outdoor route, waits for
the DRAW COUNT to pass 4,000 (an event, not a wall clock — gotcha 75), and then samples
the whole process with `perf record -F 999` for 30 s. No call graph: at `-O2` without
frame pointers an fp walk is fiction, and the question is which INSTRUCTIONS run, which
needs no stack. Every guest function is a real symbol (`sub_XXXXXXXX`, RelWithDebInfo),
so the profile reads the title's own code directly.

The thread table reproduces part 50 exactly (2.57 cores of 16, busiest thread 93.6%), and
83,222 samples then split that thread as follows — percentages **of the thread**:

| symbol | share of the thread | what it is |
|---|---|---|
| `sub_8283C6C8` | **41.98%** | the spin body: `mr rX,rX` priority hints in a `bdnz` loop, then a timeout check |
| `sub_82845160` | **21.49%** | the loop that calls it — the ring-progress wait |
| `__restgprlr_29` / `__savegprlr_29` | 11.23% / 5.35% | that function's own prologue and epilogue, once per spin iteration |
| `sub_82821FF0` | 4.10% | the two-load timebase read the spin polls with |
| everything else | < 2.3% each | |

**84.15% of the busiest thread in this process is one spin-wait.** And the port already
knew what that wait is: `docs/phase1-notes.md` **finding 38** traced it end to end in
phase 1, with the same two addresses and a `gdb` stack showing them under
`sub_827D3898` — **the Draw Thread body**. Its protocol, quoted there out of the title's
own code:

```
82845200  lwz  r11,0x2a90(r31)   ; r11 = the progress-word POINTER
82845204  lwz  r10,0x2a9c(r31)   ; W = what the driver has produced
8284520C  lwz  r11,0(r11)        ; R = what the GPU has consumed
82845214  cmplw cr6,r9,r11       ; (W - target) >= (W - R)  ->  R has reached target
82845218  blt  cr6,0x828451f0    ; ...else spin again
```

`R` is the ring read pointer, and **the only thing in this process that advances it is
our own pump**, which publishes its parser's real position after every walk
(`vd.cpp`, `g_rptrWriteback`). So the answer to item 0 is not "the guest is saturated
and we cannot help it". It is:

* the thread is **not simulating** — part 50's table guessed "a GUEST thread — the title
  simulating", and that guess was wrong in the direction that would have retired the
  plan;
* it is **blocked on our pump's throughput**, burning a core to do it, because on the
  360 those `mr r31,r31` hints yield SMT slots to the other hardware thread and here
  they are no-ops;
* therefore **the plan's whole strategy is correctly aimed**: milliseconds taken off our
  pump are milliseconds off the frame, and the concern that opened part 51 is answered
  in the reassuring direction.

**The transferable half is the method, and it is embarrassingly cheap.** Thirty parts of
this project have profiled the frame with a hand-written per-phase timer that can only
see inside the pump thread and reports scopes rather than functions. One `perf record`
names the top cost in every thread at once, in symbols, including the guest's — and the
port had the symbols the whole time. **Run the symbol profiler before the phase
profiler**: the phase profiler tells you which of the scopes YOU wrote is expensive, and
only the symbol profiler can tell you that the expensive thing is not in them.

### §2. The same profile, read on our pump thread — and it is mostly not the renderer

The pump (31.12% of the process, 79.3% of a core) splits by symbol like this:

| symbol | share of the pump thread |
|---|---|
| `DoSwapImpl` | **19.40%** |
| `GuardFold` | **16.79%** |
| `DoDraw` | 9.84% |
| `UploadStream` | 7.21% |
| `BindShader` | 6.48% |
| `UploadTexture` | 4.92% |
| `memcmp` / `memmove` / `memset` | 4.57 / 3.78 / 1.71% |
| `WriteRegisterRun` | 2.92% |
| `ExecutePacket` | 2.42% |
| `ExecuteLinear` | 1.14% |

Three things are worth saying about that table, and the first two contradict documents
in this repo.

**`DoDraw` is under a tenth of the pump.** The per-phase profiler charges ~15 ms of a
~22 ms frame to the draw path, which reads as "the renderer is the problem"; at symbol
level the draw recording itself is 9.84% and the two biggest entries are a **content
guard** and the **present path**. `GuardFold` at 16.79% is item 2a — the stream guard —
independently confirmed by a tool nobody in this project wrote, which is the kind of
oracle the evidence rules ask for.

**`ExecutePacket` + `ExecuteLinear` + `WriteRegisterRun` is 6.48% of the pump.** Part
50 priced the PM4 walk's dispatch overhead at ~2.2 ms of a ~22 ms frame from its own
A/B — about 10% — so this is the same order, measured a completely different way. Item
1c is real, and it is not where the frame is.

**And `DoSwapImpl` at the top is a trap that §5 unpicks**: `perf` attributes inlined
code to its container, and the frame-statistics instrument is inlined into that
function. See below before reading 19.40% as the cost of presenting.

### §3. Item 1 — soft-dirty page tracking is DEAD, and not for the reason it was expected to be

The kickoff's item 1 was to price `/proc/self/clear_refs` before building anything on
it, on the grounds that it walks the page tables of the whole process and could be
milliseconds. `tools/part51_clear_refs_cost.py` reproduces the runtime's own memory
shape (a 4 GB sparse reservation; the live outdoor process measures **1.2 GB resident**
across 7.6 GB of VMAs) and sweeps the resident set. Three measurements, and **the one
that was expected to decide it did not**:

| | measured | verdict |
|---|---|---|
| **the arm** — one `clear_refs` write | 24.4 ns per resident page → **7.5 ms** at 1.2 GB | **fatal.** It replaces ~26 MB of hashing, which at the fold's own 35.7 GB/s is ~0.7 ms. The arm alone is ~10x the cost of the thing it removes, once per frame |
| **the query** — a `pagemap` read | 2.24 us for 128 KB against 3.67 us to hash it; 14.9 us against 58.7 at 2 MB | **the idea's premise was RIGHT.** Above ~64 KB the kernel's answer is 1.6-4x cheaper than reading the bytes |
| **the aftermath** — the write-protect faults arming creates | re-touching 1.2 GB costs **+237 ms**, i.e. **773 ns per page** | **fatal, and independently so.** Those faults land on whoever writes next, which is the GUEST's threads — we would be charging the title's own simulation for our measurement |

So the item dies twice over, and neither death is the one the question was aimed at. The
part worth keeping is the shape: **the seductive part of a mechanism was the part that
was fine.** The query really is three orders of magnitude smaller than the hash, exactly
as the kickoff argued — and the mechanism is unusable anyway because arming it is
process-wide and because it converts a read-only page into a fault. When costing an
idea that replaces work with a kernel service, price the SETUP and the AFTERMATH before
the part that made the idea attractive; they are the two nobody writes down.

One honest note on measurement 2: the first version of that table read the pagemap
*and decoded every entry in Python*, and reported the query as SLOWER than the hash at
every size. That was an artifact of the decoder, not of the syscall. Re-measured with
the decode removed, the syscall floor is what the table above records. The direction of
the error is the one to notice — it flattered the conclusion the other two rows were
already reaching, and it would have been quoted as a third independent refutation.

### §4. The item item 0 found: the pump SLEEPS 1 ms before every walk, and somebody is waiting

Nothing in `docs/perf-plan-part50.md` mentions the pump's sleep, and the reason is
structural: every item in that plan makes the pump's WORK smaller, and a sleep is not
work. But the pump loop has begun with `sleep_for(tickMs)` since phase 1, the `pump` line
has reported that sleep as 10-18% of the wall clock since part 18, and **the 1 ms is not
a measured period — it is the smallest number the millisecond knob can express.**

Two readings fit "sleep 12%", and until part 51 nothing in this runtime could separate
them: the ring is empty and sleeping is correct, or packets are already waiting and every
nanosecond of it is frame time. §1 makes the second reading likely — the Draw Thread is
burning 93% of a core spinning on the read pointer only that walk advances — but likely
is not measured.

**The discriminator is what the walk does NEXT.** If the walk immediately after a sleep
advances the ring cursor, there was work to do and the sleep delayed it; if the cursor
does not move, the sleep cost nothing. That is `PumpStats::sleepBeforeProgressNs`, and it
is an UPPER BOUND by construction — work that arrived halfway through a sleep was delayed
by half of it, and nothing on this side can see when it arrived. It prints with a `<=` so
it cannot be quoted as a saving by accident.

It also has its own negative control built in, without anyone having to arrange one: in
the boot and menu windows the progress share reads **37-46%**, and in the outdoor crowd it
reads **87-100%**. An instrument that read the same everywhere would not have been shown
capable of failing (gotcha 30); this one is visibly a measurement of something.

`tools/part51_tick_campaign.sh`, one pinned binary, three arms — `base` at the shipped
1 ms, `fast` at 100 us, and `slow` at 4 ms as the positive control, because an arm that
can only make things better is a hope rather than an experiment:

| arm | tick | ticks/frame | `sleep` % of wall clock | ticks whose walk made progress | latency bound |
|---|---|---|---|---|---|
| base | 1000 us | 3.00-3.44 | 10.6-15.9% | 87-100% | **<= 3.17 ms/frame** |
| fast | 100 us | 3.01-3.35 | **1.6-1.9%** | 90-99.7% | **<= 0.47 ms/frame** |

**The most informative number in that table is the one that did NOT move.** A tick ten
times finer produced the SAME ~3.0-3.4 ticks a frame. So the pump is not waking uselessly
and the tick is not what schedules it: the walk stops three times a frame because it stops
at unsatisfied `WAIT_REG_MEM`s (part 4's brake, resumed by `StallPlan`), and the tick only
decides how long each of those three stops lasts. The saving is therefore arithmetic
rather than statistical — 3 stops x 0.9 ms = 2.7 ms — and the measured bound moves 3.17 ->
0.47, which is that number to two figures.

### §5. Does the removed latency become frame time? Yes — and the POSITIVE CONTROL is what proves it

Three arms, one pinned binary, **three unprofiled runs each**, alternated, read by draw
bin with `tools/part47_perf_read.py` (medians per run, and the within-arm spread as the
floor):

| draw bin | base (1 ms) | fast (100 us) | slow (4 ms) | fast vs base | slow vs base |
|---|---|---|---|---|---|
| 500-1,500 | 16.0 | 16.0 | 22/22/21 | +0.0% | **+37.5%**, outside its 4.5% floor |
| 1,500-3,000 | 17/16/16 | 16/16/16 | 25/25/26 | +0.0%, inside a 6.2% floor | **+56.2%**, outside |
| **3,000-5,000** | 19/19/18 | **16/16/16** | 28/29/29 | **−15.8%, OUTSIDE its 5.3% floor** | **+52.6%**, outside |
| 5,000-8,000 | 23/23/23 | 20/22/16 | 30/31/32 | −13.0%, inside a 30% floor | **+34.8%**, outside |

And the statistic gotchas 237/238 exist for — the share of frames pinned within 1 ms of a
vblank multiple — in the bin where the win lands: **24-36% (base) -> 72-95% (fast)**. At
3,000-5,000 draws the frame stops being CPU-bound and lands on the 16 ms floor.

**The `slow` arm is the reason any of this can be believed.** The direct comparison is
outside its floor in only one bin, and this route's floor is wide (part 50 measured 8-18%
on frame time). But the 4 ms arm is +34.8% to +56.2% and outside the floor in **every**
bin, at a tick 4x coarser — i.e. **3 extra sleeps of 3 ms convert to 7-11 ms of frame
time, ~1:1.** A mechanism that converts at 1:1 when pushed the expensive way converts at
1:1 when pushed the cheap way, and that is what licenses the 2.7 ms in the bins where the
direct measurement sits inside its noise. An arm that can only make things better would
have proved none of it (gotcha 331).

**100 us is now the default.** `CZ_PM4_TICK_MS` is untouched and setting it explicitly
still gives millisecond behaviour, so `CZ_PM4_TICK_MS=1` is the control arm for this
change. The costs, stated rather than buried: process CPU in a crowd goes **2.57 -> 2.75
cores of 16**, and our pump's duty cycle goes **79% -> 93%** — it is now walking where it
used to sleep, which is the point, and which also makes part 52's parallelism item
sharper. What is NOT judged here is how it FEELS: this changes WHEN work happens rather
than how much there is, and pacing is felt before it is counted. That is the operator's
question (`tools/part51_operator_session.sh`) and it is part 52's item 0.

**And the fix broke a health signature on the way through, which is worth recording
because the line's own comment warned about it.** The ring trace prints the consecutive
hold streak in TICKS and warns when it passes 60, a threshold that meant "about a second"
only while a tick was a millisecond. At 100 us it means 6 ms, and a healthy run of the new
default measures `max=104` ticks — so the warning would have fired on every run, on the
one line whose comment says "when a wait's cadence changes, re-express every gate on it in
a unit that survives the change" (gotcha 157/98). The threshold is now a duration and the
label prints the real period. **A warning that fires on healthy runs is worse than no
warning: it teaches the reader to skip the line.**

### §6. AND THE INSTRUMENT THAT RECORDS FRAME TIME COSTS 3 ms A FRAME

Part 50 priced `CZ_VK_PROFILE` at 2-4 ms/frame by reading `CZ_VK_FRAME_STATS`, and wrote
the rule that followed from it: *"an instrument that can only be read through another
instrument cannot measure that one."* It then stopped one instrument short of applying it.

`CZ_VK_FRAME_STATS` zeroes a 2 MB colour bitmap and walks all **921,600 pixels** of every
PRESENTED frame, computing lit coverage, mean luma, an exact distinct-colour count through
a random-access bit test, and a hash. Measured with its own `ProfScope` on the outdoor
route:

```
[vkprof]   CZ_VK_FRAME_STATS itself: 3.24 ms/frame (19.8% of this window)
                                     2.94 ms/frame (15.2%)
                                     2.99 ms/frame (14.9%)
```

**~3 ms a frame, 15-20% of the window** — larger than the profiler's bill, and it has been
enabled in **every performance run this project has ever recorded**, including the
operator's whole-map lap and part 50's own profiler A/B. It was invisible for the
structural reason part 50 named and did not extend: the run that would show its absence is
the run that records no frame times.

Three consequences, and the first is a correction rather than a win (gotcha 337):

1. **Every absolute frame time in this repo is inflated by ~3 ms on top of the profiler's
   2-4.** The operator's 28.3 ms at 5,000-7,000 draws was already corrected to ~25-26 by
   part 50; it is really nearer **22-23 ms, ~43-45 fps**. Nobody plays with either
   instrument set. This changes what may be CLAIMED, not what the game does.
2. **A/Bs are unaffected** where both arms carry it, which is all of them. Part 51's own
   campaign carried it in all three arms.
3. **3 ms is a lower bound on its cost.** The `ProfScope` measures the block's own
   interval; it cannot see that a 2 MB bitmap scrubbed once a frame evicts the cache the
   next frame's work then re-warms.

The rule to carry: **when quoting a frame time, say which instruments were on.** And to
price an instrument, find a counter that does not depend on it — `VkRenderer_DumpStats`
prints the frame number and is independent of both — or put a scope on the instrument
itself, which is what was done here.

### §7. THE OPERATOR'S VERDICT — the win reproduces on their machine, and the risk did not materialise

Part 51's change was the first in this campaign that could fail as PACING rather than as
throughput: it moves WHEN work happens rather than how much there is. So the operator was
asked two questions in a chained same-binary A/B (`tools/part51_operator_session.sh`, arm
A `CZ_PM4_TICK_US=100`, arm B `=1000`), and the first was not the usual one.

**"Which felt smoother?" — they could not tell the two apart.** Recorded as what it is:
that rules out a GROSS pacing regression, which is the way this change could plausibly
have failed. It does not establish that the change is smoother, and it is not written up
as if it did.

**"What does the profiler say?" — the win reproduces on their hardware and route.** Median
frame time over thousands of frames, binned by draws:

| draws/frame | tick100 | tick1000 | | frames at the vblank floor |
|---|---|---|---|---|
| 0-500 | 16.0 ms | 16.0 ms | — | 99% / 99% |
| 1,500-3,000 | 16.0 ms | 16.0 ms | — | 99% / 93% |
| **3,000-5,000** | **17.0 ms** | **21.0 ms** | **+23.5%** | **51% / 2%** |
| 5,000-8,000 | **24.0 ms** | 26.0 ms | +8.3% | 3% / 3% |

47.6 -> 58.8 fps in the mid band, 38.5 -> 41.7 in the crowd, and the 2% -> 51% pinned
share is the same signature the headless campaign showed: the frame stops being CPU-bound
and lands on the 16 ms floor.

**And the phase split shows the mechanism directly, which is what makes a one-run-an-arm
session worth quoting at all.** `outside` — the column the pump's sleep lives in — reads
**5.37 ms against 7.89 ms, a difference of 2.52 ms** against the 2.7 ms the model
predicted, while every other phase sits within a few percent (`constants` −0.5%, `submit`
+3.2%, `readback` +3.9%). The arm is also provably engaged: `sleep` 2.1-3.3% of the wall
clock against 11.6-14.0%, and the latency bound 0.47 against 3.17 ms/frame.

**What this session is NOT.** One run an arm, so `part47_perf_read.py` correctly refuses a
verdict on every frame-time bin; the statistical weight is the headless campaign's three
runs an arm plus its 4 ms positive control, and this session's job was to confirm the
direction and the mechanism on the operator's own machine. Both arms also carried
`CZ_VK_PROFILE` and `CZ_VK_FRAME_STATS` — the latter measured at **1.86-3.32 ms/frame on
their machine**, agreeing with §6's headless 3 ms — so the frame times above are ~5-6 ms
slower than what they actually play. Uninstrumented, their 3,000-5,000 band would sit on
the 16 ms floor almost entirely.

**One incidental finding worth keeping.** In the LIGHT windows of the tick100 arm (1,284
and 1,351 draws/frame) the pump ran **46.9-54.8 ticks a frame with only 5.0-6.4% of them
finding work**, against 3.3-3.4 ticks a frame and 88-90% in the crowd. That is the finer
tick doing exactly what a finer tick does when there is nothing to collect — waking,
looking and going back to sleep — and it is the honest cost of the change: a few percent
of one core in scenes that are already at the frame cap, where it buys nothing. It is
also the instrument's negative control working on the operator's route without anyone
having to arrange it.

## 6ci. Part 52: THE PLAN'S BIGGEST ITEM WAS RIGHT ABOUT THE COST AND WRONG ABOUT THE
## FIX — and its own verify arm refuted it on the first run

`docs/perf-plan-part52.md` opens with a symbol budget rather than a phase table, and its
item 1.0 is the one that budget found: **`BindShader` re-hashes the entire microcode on
every shader-load packet**, 12.47% of the pump thread with every instrument off, third
behind `GuardFold` and `DoDraw` and **in no plan this project had ever written**.

Part 52 confirmed the cost, built the fix the plan specified, and the fix was wrong. The
verify arm the plan insisted on caught it in one run.

### §1. The cost is where the plan said, and the annotation says why

`perf annotate` on the `nostats` recon profile, over `BindShader`'s ~4.3% of the whole
process:

```
   17.36 :   2579c48:  imulq %rax, %r10
    5.82 :   2579c4c:  movzbl 0x1(%rdx,%r8), %r9d
   17.41 :   2579c55:  imulq %rax, %r9
    6.51 :   2579c59:  movzbl 0x2(%rdx,%r8), %r10d
   17.75 :   2579c62:  imulq %rax, %r10
    6.51 :   ...
   18.65 :   2579c6f:  imulq %rax, %r9
```

**~71% of the function's samples sit on four `imulq`s** — the FNV-1a accumulator, one
5-cycle-latency multiply per BYTE with every multiply depending on the last. The loads
beside them take 6% each. So this is **ALU-latency bound at about a byte per cycle**, not
memory bound, and that distinction is what decides the fix. (Contrast `GuardFold`, which
§0 of the plan establishes IS memory-bound at ~10 GB/s — the same "it's a hash, make it
faster" instinct is right for one of them and wrong for the other.)

Neither `LooksLikeUcode` nor the `announced` linear scan the plan also flagged shows up
at all. The plan named three suspects inside one function; the annotation says one of
them is 95% of it.

### §2. Why the hash cannot be replaced, which is the trap that makes the item interesting

It is the **shader cache key**. `assets/shader_spv/vs_<hash>.spv`, `build_shader_spv.sh`,
the `[imload]` line and the "no translated shader" miss report all name a shader by this
exact FNV-1a value, and the offline pipeline recomputes it from the same bytes. A wider
or vectorised fold renames all 435 cache entries at once and presents as a silently
unshaded world. **The hash has to be avoided, not accelerated** — which is what makes
this a memoization item rather than a SIMD item.

### §3. THE PLAN'S KEY IS REFUTED, and the failure is silent rather than loud

The plan specifies the memo as `(ucodeVa, sizeDwords)` plus "the first and last dword of
the microcode alongside — two loads, and it catches the overwhelming majority of a
re-upload", and argues the failure mode is benign: a wrong hash names nothing in the
cache, so the standing `grep -c "no translated shader"` gate would catch it.

Both halves are false, and `CZ_PM4_VERIFY_SHADER_HASH=1` — which computes the memoized
answer AND the real fold on every load — said so on its first full outdoor run:

```
[pm4] SHADER MEMO MISMATCH #2: VS va=00000000 size=102 — memo said f2ef2d2f8de976d0,
      the microcode hashes to 8ed00911a7bc1eb1 (first=F1555004 last=A9A9C68D)
[pm4] SHADER MEMO MISMATCH #3: VS va=00000000 size=102 — memo said 8ed00911a7bc1eb1,
      the microcode hashes to f2ef2d2f8de976d0 (first=F1555004 last=A9A9C68D)
```

**Two different shaders, identical in size and in both probe dwords, alternating** — so
the probe was wrong about half the times it was consulted for that pair. It happened at a
real address too (`va=BC73D080 size=120 first=F5556005`), which is the driver recycling
one staging buffer: exactly the case the probe was specifically supposed to catch.

The reason is structural and should have been predictable: **microcode is far too regular
for two dwords to identify it.** Dword 0 is a control-flow instruction pair whose encoding
repeats across every shader a compiler emits from one template, and the last dword is as
often as not the tail of a padded block. `F5556005` and `F1555004` are shared CF headers,
not fingerprints.

And the wrong answer is **not** a cache miss. It is **another real shader's hash**, which
IS in the cache — so the renderer would bind a real, wrong, translated shader and draw
with it. That is a stale-mesh-class defect (part 46) with no gate pointing at it.

> **The transferable rule (gotcha 342): when a cache key is a PROBE rather than the
> content, ask what the WRONG ANSWER IS, not just how likely it is.** A probe that fails
> into another valid key fails invisibly. The plan reasoned about probability ("catches
> the overwhelming majority") and about loudness ("it would read as a miss"), and was
> wrong about both — one measurement settled it, and the measurement existed only because
> the plan had also insisted on a verify arm. **Write the verify arm even when the
> argument for the fix sounds complete.**

### §4. What is shipped instead: the key is the content, compared with `memcmp`

`(va, size, first dword)` survives only as a way to choose which stored copy to compare
against; the decision is `memcmp` against the microcode we hashed last time. **Exact —
there is no probability left in it** — and still a large win, because the cost being
removed is a serial multiply chain and not a memory read: FNV-1a runs at ~1 byte/cycle
here and `__memcmp_avx2_movbe` at tens of bytes per cycle over the same bytes, which are
hot (each of ~250 shaders is re-bound several times a frame).

The table is **256 sets x 4 ways** rather than direct-mapped *because of the measurement
in §3*: the alternating pair is real, and a one-entry-per-slot table would evict on every
load and hash every time.

Two by-products, both free:

* the pointer path no longer copies at all on a hit — it compares guest memory in place;
* both paths snapshot into **one reusable buffer** instead of constructing a
  `std::vector` per packet, removing ~1,300-1,900 mallocs and frees a frame. That is a
  share of the `_int_malloc` the recon found on the frame path (plan item 3.3), collected
  without going looking for it.

### §5. The gates, in the order that makes them mean anything

**Poison first.** `CZ_PM4_VERIFY_SHADER_POISON=1` corrupts one memoized hash in every
1024 hits. It produced the capped 32 mismatch reports — so the verifier's silence is
evidence rather than decoration (gotcha 30).

**And the poison run found a defect in the verifier itself**: under the verify arm,
`BindShader` ran on every load and therefore re-INSERTED on every load, filling all four
ways of a set with the same shader and reporting **2,152,161 evictions against 46,128
misses**. An instrument that destroys the statistic standing next to it has changed the
measurement (gotcha 7); the fix is one comparison — insert only when the memo did not
already answer correctly, so a disagreement still repairs the table.

**Then the clean arm**, full unattended outdoor roam:

| | |
|---|---|
| memo hit rate | **100.0%** — and the last four windows are **0 misses at all**, at 1,195-2,073 loads/frame |
| evictions | **0** |
| capacity misses | **0** — every miss is compulsory, i.e. a shader seen for the first time |
| `SHADER MEMO MISMATCH` | **0** over the whole 600 s run |
| `no translated shader` | **0**, with 564 distinct shaders bound |

A 100% hit rate is the item's own pre-registered success criterion (the plan asks for
~80%) and it is also the strongest evidence that the ~1,500 loads a frame really are the
same few hundred shaders rebound over and over.

### §5b. The A/B: `BindShader` goes from 14.16% of the pump thread to ZERO

Two arms of one binary on the same route with the same event gate, `nostats` (no profiler,
no frame stats), 40 s of `perf record -F 999` after a 100 s roam —
`tools/part52_recon.sh` with `ENVX=CZ_PM4_NO_SHADER_MEMO=1` for the control. Symbol shares
**of the pump thread**, which is the denominator that matters: process-wide shares are
confounded here because the memo makes the guest's Draw Thread spin proportionally longer
(`sub_8283C6C8` 14.26% -> 19.44% of the process), which is itself a corroboration.

| symbol | control (memo off) | memo on |
|---|---|---|
| **`BindShader`** | **14.16%** | **0.00% — not one sample** |
| `GuardFold` | 24.30% | 29.85% |
| `DoDraw` | 15.53% | 15.49% |
| `UploadStream` | 8.27% | 10.46% |
| `memcmp` | 3.36% | 4.18% |
| `_int_malloc` | 1.08% | 1.41% |

The commit predicted "under 2%". It is zero. And the memo's own `memcmp` — ~1.5 MB a
frame against the 9 MB it stopped hashing — never becomes visible: `memcmp`'s share of the
*process* actually fell (1.06% -> 0.89%), which is route noise rather than evidence, but
it bounds the new cost at well under the old one either way.

**Where the honest uncertainty is, stated rather than smoothed over.** The two arms
disagree on how much the pump's total CPU fell, because they are two different roams
driven by the title's own AI and the two instruments sampled different windows:

* `/proc` per-thread CPU over a 20 s window: the pump goes **59.9% -> 51.5% of a core**,
  −14%;
* `perf` sample share over the following 40 s: the pump goes **31.56% -> 21.31% of the
  process**, which against near-identical process totals (2.28 vs 2.24 cores) is −34%.

Both say down, by between a seventh and a third. Neither is quoted as *the* number.
`BindShader` -> 0.00% is the result that needs no denominator.

### §5c. The frame time, measured ABOVE the cap because below it the question is unaskable

Part 51's tick change plus this item took the headless outdoor route onto the frame cap
for most of its length, and **a capped frame cannot report a CPU saving**: both arms sit
on the rung and the A/B reads zero whatever the change was worth. That is not a null
result, it is an unmeasurable one. So `tools/part52_item_campaign.sh` runs every arm at
`CZ_FPS_CAP=120`, which lifts the ceiling above the work and changes nothing else. **The
number it produces is the CPU saving, not the frame rate a player sees.**

Three runs an arm, alternated, one pinned binary, plus a NULL arm (`base` against itself)
in the same block — `tools/frame_perf_bins.py`:

| draws/frame | base (memo on) | noitem (memo off) | control arm's cost | **the null, same bin** |
|---|---|---|---|---|
| 0-999 | 8.74 ms | 8.83 | +1.0% | +1.1% |
| 3,000-3,999 | 12.21 | 12.22 | +0.1% | −1.3% |
| 4,000-4,999 | 13.15 | 13.50 | +2.7% | −5.8% |
| 5,000-5,999 | 14.98 | 15.93 | +6.4% | −4.5% |
| **6,000-6,999** | **15.84** | **18.22** | **+15.0% mean, +12.5% median** | **+0.2%** |
| **7,000-7,999** | **16.98** | **19.38** | **+14.2% / +11.8%** | +7.4% (low n) |

**Read the 6,000-6,999 row against the null in the same bin**: +15.0% against +0.2%, on
19,875 frames versus 11,202. The bins below 5,000 are flat in both arms, which is what a
change to a per-packet cost should look like where the frame is under the ladder's reach —
and an arm that "improved" those would have meant a changed draw set and an inadmissible
comparison.

**The saving is ~2.4 ms at 6,000-7,000 draws, and that is a LOWER bound.** The `base` arm
is *on a pacing rung* there (96% of its frames within 1 ms of one, against 17% for the
control), so its CPU work is bounded above by the rung rather than measured by it. The
plan predicted −1.5 to −2.3 ms; the measurement is at or just past the top of that range.

One caveat on the pinned column, because it is quoted here and it was defined for a
different ladder: `frame_perf_bins.py` counts frames within 1 ms of a multiple of **16 ms**,
and this campaign ran a 8.33 ms vblank period. 16.67 is within 1 ms of 16, so the column
still reads "on the second rung" correctly — but it is not counting the ladder's other
rungs and should not be quoted as "pinned share" without saying which ladder.

### §6. Item 2.1 — the counter dump repriced it, and one site is 85% of it

The plan prices `std::map<std::string, uint64_t>::operator[]` (2.30% of the pump) as "28
`Count(` sites inside `DoDraw`'s body", counted by reading the source. **The counters'
own dump says something different.** Of ~62.5 M plain-`Count` calls in a 200 s outdoor
run:

| site | calls |
|---|---|
| `VkRenderer_Draw`, "draw: handed to the renderer" | **52,901,332 — 84.6% on its own** |
| the next nine sites together | 8.6 M |
| everything else (83 sites) | < 0.5 M |

Most of the 28 in `DoDraw` are decline paths that fire a few hundred times an hour. Ten
sites are 99.2% of the total and those are what was converted.

> **The rule: `VkRenderer_DumpStats` already prints the call count of every counter,
> which is the exact statistic that ranks these sites.** Reading the source instead ranks
> them by how alarming they look. This is the third time in three parts that a number the
> project already collects answered a question someone was about to estimate.

### §8. Item 3.2 — the pipeline lookup, and a column that moved alone

`R->pipelines` is probed once per draw — **6,613 lookups a frame** on this route — and it
was a `std::map` of ~400 entries: ~9 levels of pointer chasing with a 48-byte `memcmp` at
each, every level on its own red-black node and therefore its own cache line. Replaced
with `std::unordered_map` hashing the key's fixed 48 bytes, plus a one-entry front cache
because consecutive draws often share a pipeline.

The measurement is the `other` split, against the **pinned pre-change binary run now**
(gotcha 51) on the same route with the same instruments:

| `other`, ns/draw | shader | key | **pipeline** | begin | fetch | tail | residual | total |
|---|---|---|---|---|---|---|---|---|
| before | 79-80 | 25 | **110-112** | 54-59 | 105-108 | 34-35 | 203-204 | 609-623 |
| after | 79 | 25 | **38-43** | 55-60 | 104-106 | 34-35 | 204 | 540-547 |

**Every other column is unchanged to the digit and only the one the change touches
moved.** That is a stronger control than any arm could give here: a route difference or a
thermal drift would have moved `fetch` and `begin` too. −70 ns/draw is **~0.43 ms/frame**
at 6,000 draws, against the plan's −0.3 to −0.7 ms, and the commit's prediction of "under
50 ns/draw" is met at 38-43.

The front cache's assumption is confirmed rather than assumed: **67.7-71.6% of lookups
are served by it**, which is what makes the remaining hash probes ~30% of what they were.
A low rate here would have meant a wasted 48-byte compare per draw, and the counter is
the only thing that could have said so. `CZ_VK_NO_PIPELINE_CACHE1=1` is its arm.

One honest gap. The commit predicted "`pipelines=413` unchanged"; the two runs ended with
412 and 366 entries, because the title's own AI drove two different roams and a pipeline
is created per distinct STATE combination, so the count is a fact about where the run
went. `shaders=435` matched exactly in both. The correctness argument is therefore
structural rather than numeric — same key, same equality, and a front cache that only
ever answers on `==` — and it should have been stated that way in the prediction.

### §7. Item 4.1 — `outside` re-split, and almost none of it is blocking

`outside` is a wall-clock residual measured from inside the pump thread, and a wall-clock
interval cannot tell "we spent 4 ms doing something" from "we spent 4 ms descheduled".
Part 50 §6cg read the residual as "guest simulation ~3 ms" when at 79% duty the pump was
simply blocked, and the plan's item 4.1 asks for the split rather than another guess.

One `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` per REPORT — not per frame, so the
instrument is on no path at all (gotcha 223) — answers it exactly, because `wall - cpu`
is by definition every nanosecond the pump was off a core and the sleep counter already
accounts for the deliberate part:

```
[vkprof]   pump thread: 75.6% on CPU | off-CPU 3.96 ms/frame = sleep 3.83 + BLOCKED 0.12
```

**The blocked term is 0.12 ms of a 16 ms frame — 0.8%.** Whatever else `outside` is, it
is not the pump waiting on somebody else. Read this line together with the frame rate,
though: in a window pinned to the 60 fps cap the sleep term is the CAP and not a cost,
and the instrument is only interesting when the frame is above the cap's floor.

### §9. Gates at close — all clean, and one caught something

| gate | result |
|---|---|
| `--smoke` | OK |
| switch gate | 0 defects (2 benign frameless thunks) |
| shader dimension census | 0 disagreements; 1 sidecar without `tfetchDims`, the known lost-microcode entry |
| PM4 oracle 1/2 — packet lengths, 24.5 M packets | clean |
| PM4 oracle 2/2 — indirect-buffer walks, 28,726 buffers | clean |
| E3 picture | **best of five +0.8771**, 4 of 5 agreeing on layout |
| `no translated shader` | 0 |
| `truncated=` | 0 |
| deepest file on a plain boot | **#83 `game:\data\skeleton\cinezombie.big`** |
| A5 kernel-call diff | **exit 0, 4 permutation windows, 0 real** |
| `SHADER MEMO MISMATCH` under the verify arm | 0 over a 600 s roam |

**Re-run after the `record`/GUARD split**, because that change landed after the gate pass
above and a claim of "instrument-only" is exactly the kind this project does not take on
trust: **ALL GATES CLEAN again, E3 best of five +0.8764 with 4 of 5 agreeing** (against
+0.8771 on the previous pass — the same five sample points on an animated backdrop). The
split is safe on the shipping path by construction as well as by measurement:
`ProfScope`'s constructor and `Close()` both early-return on `!g_profileOn`, so with the
profiler off it is a global load and a branch, the same as the twenty scopes already on
the draw path.

E3 is quoted next to its neighbours because a cross-session best-of on an ANIMATED
backdrop is a weak comparison (gotcha 133): part 50 read +0.8820 of five, part 51 +0.8043
of fourteen, this part +0.8771 of five. Nothing here touches what is drawn.

**The name-diff gate earned its place again.** `tools/build_shader_spv.sh` over
`~/DR2CZ-troubleshooting/ucode-dumps` and a `diff` of the NAMES found
**`ps_bd5d8eb053e36a84` present in the dumps and absent from the cache** — microcode
captured at 23:58 the previous evening, by a recon run, and never translated. No run in
this part bound it, so `grep -c "no translated shader"` read 0 every single time, and the
COUNT could not show it either: 435 dumps, 435 modules, **different sets**. Exactly the
failure CLAUDE.md describes from part 27, reproduced. The cache is now **436**, and the
only remaining name difference is `ps_926c15dd20571cf1`, whose microcode is lost.

> Restating the rule because it has now caught two entries in two different parts: **the
> gate is the NAME diff, not the count, and it must be run in any part where
> `CZ_SHADER_DUMP` was set on any run** — which for a performance part means every part,
> because the recon scripts set it. It is two lines and it is free.

### §10. THE OPERATOR'S SESSION — the two columns that should have moved, moved, by the
### predicted amounts, and nothing else did

A whole-map lap on the operator's own machine, `tools/part52_operator_session.sh`, single
arm, `CZ_VK_PROFILE=20` and **`CZ_VK_FRAME_STATS` deliberately OFF** — part 51 measured
that instrument at 1.86-3.32 ms/frame on this machine and it was on in every performance
run this project had ever recorded, including part 51's own operator session.

Their verdict, first, because it is the thing the headless campaign cannot produce:
**"performance is better."**

#### The phase split, against part 51's operator session on the same machine and route

Milliseconds, part 51's medians over 3,000-8,000 draws (`perf-plan-part52.md` §2a) against
part 52's four crowd windows at 5,365-6,293 draws. **Both tables exclude the instruments
from their columns**, which is what makes them comparable at all:

| phase | part 51 | part 52 | delta | where the change was aimed |
|---|---|---|---|---|
| **`outside`** | **5.37** | **3.02** | **−2.35** | **`BindShader` — it is called from the PM4 WALK, not from `DoDraw`, so the memo's saving lands here.** Predicted ~2.4 |
| — `other` | 3.92 | 3.48 | **−0.44** | **the pipeline lookup.** Predicted 0.43 |
| — `textures` | 3.20 | 3.50 | +0.30 | not touched |
| — `record` | 6.21 | 6.47 | +0.26 | not touched |
| — `constants` | 1.10 | 1.17 | +0.07 | not touched |
| — `streams` | 0.03 | 0.02 | −0.01 | not touched |
| `readback` | 0.60 | 0.62 | +0.02 | not touched |
| `submit` | 0.07 | 0.06 | −0.01 | not touched |
| **draw total** | 14.45 | 14.46 | +0.01 | |

**Two columns moved and both by the amount predicted for the item that lives in them.**
`outside` −2.35 against a predicted ~2.4; `other` −0.44 against a predicted 0.43. The two
that drifted up (`textures`, `record`) are a heavier draw mix — this band is 5,365-6,293
where part 51's median spans 3,000-8,000 — and `draw total` is flat because that drift
exactly cancels the pipeline win inside it.

That the memo's saving appears in **`outside` and not in `draw`** is worth stating plainly,
because it is not where a reader would look for a "shader" change: `BindShader` runs on an
IM_LOAD packet inside the command-processor walk, and the walk is what `outside` is.

#### Frame rate, with the instrument arithmetic done out loud

| | part 51 (tick100) | part 52 |
|---|---|---|
| crowd frame, as measured | 24.0 ms median, profiler **and** frame stats | 17.5-18.7 ms mean, profiler only |
| frame stats removed from the part-51 figure | ~21.5 ms | — |
| **comparable** | **~21.5 ms, ~46.5 fps** | **~18.2 ms, ~55 fps** |
| profiler also removed (2-4 ms) | | **~14-16 ms — at or above the 60 fps cap** |

**~2.5 ms of the raw 5.8 ms difference is the instrument I turned off, not the change**
(gotcha 337, and the reason the phase table above is the evidence rather than this one).
The remaining ~3.3 ms is part 52, and it agrees with the phase columns.

**The operator's crowd frame now reaches the 60 fps cap uninstrumented.** Part 50 quoted
them at 35.7 fps at 5,000-7,000 draws; part 51 at 41.7; this is the first session where
the heaviest thing they walk through is not CPU-bound.

#### What the session says about the items themselves

* **The memo holds on their route**: 100.0% hit in every window, at up to **2,574
  loads/frame** (headless peaks at 2,073), 4-134 misses per ~2.9 M, 0-2 evictions, and
  **0 `SHADER MEMO MISMATCH`**.
* **The pipeline front cache holds too**: 62.7-71.5%, against 67.7-71.6% headless. And
  their route grows the table to **568 entries** where headless reaches 392-412 — a deeper
  tree, so the item is worth at least as much to them as to the harness.
* **`readback` is 0.62 ms with frame stats off**, which is exactly what plan item 1.3
  asked to be priced this way and settles it: it is a 0.5-0.6 ms item, not the 1.2 the old
  plan guessed.
* **The pump is 87-93% on CPU and BLOCKED 0.37-0.77 ms/frame** — larger than headless
  (0.09-0.12) but still small. `outside`'s 3.02 ms is now roughly sleep 0.8-1.7 + blocked
  0.4-0.8 + about a millisecond of actual walk.
* **`no translated shader` = 0** over 327 distinct shaders, and the name-diff gate found
  **no new microcode at all** after a whole-map lap. The cache is complete for the map
  they cover — the first lap in this project's history to add nothing.

#### What this session is NOT

**It is not an A/B.** It is one run compared with a session from the previous day, which is
the comparison this project's own rule warns against (gotchas 50/51/86: the control is the
old configuration run NOW). It is also a per-window MEAN read against part 51's MEDIAN, and
a different draw band. The phase table is quotable in spite of that because the two columns
that moved are the two the items live in and every other column held — drift, thermals or a
different route would have moved `record` and `textures` too, and would not have known to
leave `readback` and `submit` alone. **The same-binary control exists and was not run**:
`ARM=ab tools/part52_operator_session.sh` chains a second arm with `CZ_PM4_NO_SHADER_MEMO=1`.
Run it before quoting a number from this session as a measured speedup rather than as a
confirmation of direction and mechanism.

### §11. THE SAME-BINARY A/B ON THE OPERATOR'S MACHINE — one column moved, and it is the
### right one

§10's comparison was against the previous day's session, which is the comparison this
project's own rule warns about. The control was then run: `ARM=ab
tools/part52_operator_session.sh`, two arms chained in one sitting, arm B restoring the
pre-part-52 shader path with `CZ_PM4_NO_SHADER_MEMO=1` in the SAME BINARY. Both arms
carried `CZ_VK_FRAME_STATS` this time — valid because it inflates both equally, and it is
what buys the two statistics below. The operator reports the same route in both, with the
zombie spawns varying and slightly longer at the main menu in arm A.

#### Frame time by draw bin, and the arm's own null inside it

| draws/frame | A (memo on) | B (memo off) | Δ mean | Δ median | pinned A / B |
|---|---|---|---|---|---|
| 0-999 | 16.83 ms | 16.83 | **+0.0%** | **+0.0%** | 99% / 98% |
| 2,000-2,999 | 16.02 | 16.05 | **+0.2%** | **+0.0%** | 99% / 98% |
| 4,000-4,999 | 18.21 | 20.02 | **+9.9%** | **+17.6%** | **53% / 16%** |
| 5,000-5,999 | 19.71 | 20.69 | +5.0% | +0.0% | **15% / 1%** |
| **6,000-6,999** | **21.33** | **23.37** | **+9.6%** | **+9.5%** | 0% / 1% |
| 7,000-7,999 | 25.10 | 34.86 | +38.9% | +8.7% | n = 52 / 22 — ignore |

**The two light bins are this A/B's own null control and they read +0.0%.** They are at
the frame cap in both arms, where the change cannot move anything — so the experiment
contains, for free, a band in which the arm is provably unable to act. That also settles
the operator's own worry about spending longer at the main menu in arm A: the 0-999 bin
holds 1,228 frames against 1,230 and the two are identical to the digit. **Binning by
draw count is what makes an operator's route usable as an experiment** — it survives
different spawns and different dwell times, and it cannot compare a crowd with a corridor.

**The memo alone is worth ~1.8-2.0 ms** on their machine (18.21 -> 20.02 and
21.33 -> 23.37), and the pinned share collapses where it acts: **53% -> 16%** and
**15% -> 1%**.

#### The phase split — the mechanism, and the reason this is quotable at one run an arm

Median over the crowd windows of each arm:

| phase | memo ON | memo OFF | delta |
|---|---|---|---|
| **`outside`** | **3.06** | **4.42** | **+1.36** |
| `record` | 6.01 | 6.03 | +0.02 |
| `other` | 3.25 | 3.27 | +0.01 |
| `textures` | 2.95 | 3.10 | +0.14 |
| `constants` | 1.09 | 1.05 | −0.03 |
| `streams` | 0.04 | 0.04 | +0.00 |
| `readback` | 0.58 | 0.55 | −0.04 |
| `submit` | 0.06 | 0.06 | +0.00 |
| draw total | 13.34 | 13.12 | −0.21 |

**Exactly one column moved and it is the one `BindShader` lives in.** Everything else is
within ±0.21 ms. A thermal drift or a route difference — the two confounds one run an arm
cannot remove — would have moved `record` and `textures` as well, and would not have known
to leave `readback` and `submit` alone. Note also that **`other` did NOT move**, which is
the control working in the other direction: the pipeline-lookup change has no run-time
switch and rides in BOTH arms, so `other` must hold, and it does.

#### What this revises

* **§10's `−2.35 ms in `outside`` is a cross-session number and this supersedes it as a
  measurement of SIZE.** The A/B puts the memo at ~1.4 ms of `outside` and ~1.8-2.0 ms of
  frame time on their machine; §10's larger figure was a day-to-day comparison and should
  be read as agreeing on MECHANISM, not as a second measurement. The headless campaign's
  ~2.4 ms remains the headless figure.
* **This A/B under-reports part 52 by design**, because only the memo has a run-time
  switch. Adding the pipeline lookup (~0.4 ms) and the counters (~0.3) puts the part at
  **~2.5-2.7 ms of the operator's frame**.
* **Frame stats is confirmed at ~1.5-2 ms on their machine a third time**, by difference:
  the same arm reads 20.1-20.2 ms here at ~5,700 draws against 18.3-18.7 in §10's
  frame-stats-free session.
* The memo held again: **100.0% hit at 2,224 loads/frame, 16 misses in 2.2 M, 0
  mismatches, `no translated shader` = 0** in both arms.

### §12. THE SOAK — the operator's idea, and it produced the cleanest A/B in the project's
### history *and* found a place that is not at the cap

The operator proposed going somewhere heavier and standing still for three minutes in each
arm. Both halves of that matter, and the second is the one nobody here had thought of.

**Why a soak is the right shape.** Part 26 established that a matched-frame picture A/B is
unsatisfiable outdoors — a crowd of animated actors never renders the same draw list twice
(gotcha 247) — and every performance A/B since has been two different WALKS compared
through draw bins, which spreads a few hundred frames over a dozen bins. A soak does not
make frames identical; it makes them **dense**. Standing still holds the draw count in a
band, so one bin fills with thousands of comparable frames.

Measured: **the 7,000-7,499 bin holds 7,773 frames in arm A and 6,079 in arm B**, where
the previous walk A/B's best bin held 1,348 and 1,276.

#### Frame time — and a significance figure two orders of magnitude past anything before

| draws/frame | A (memo on) | B (memo off) | Δ mean | Δ median | sig |
|---|---|---|---|---|---|
| 0-499 | 16.82 | 16.82 | **+0.0%** | **+0.0%** | +0.0 |
| 2,500-2,999 | 16.12 | 16.12 | **+0.0%** | **+0.0%** | +0.0 |
| 4,500-4,999 | 16.71 | 19.75 | +18.2% | +12.5% | +3.1 |
| 5,000-5,499 | 17.93 | 20.71 | +15.5% | +17.6% | +6.6 |
| 6,500-6,999 | 22.83 | 25.59 | +12.1% | +8.7% | +9.6 |
| **7,000-7,499 (the soak)** | **23.87 / 24.0 med** | **26.63 / 27.0 med** | **+11.6%** | **+12.5%** | **+211.3** |
| 7,500-7,999 | 24.92 | 27.17 | +9.0% | +12.5% | +12.0 |
| 8,000-8,499 | 25.17 | 28.07 | +11.5% | +12.0% | +10.6 |

The light bins remain the experiment's own null at **+0.0%**, and four bins around the soak
agree with it independently. **The memo alone is worth 2.8-2.9 ms at 7,000-8,500 draws.**

#### The phase split over the soak — 18 windows an arm, and ONE column moved

| phase | memo ON | memo OFF | delta |
|---|---|---|---|
| **`outside`** | **2.77** | **5.27** | **+2.49** |
| `record` | 8.69 | 8.76 | +0.08 |
| `other` | 4.11 | 4.23 | +0.12 |
| `textures` | 3.13 | 3.13 | −0.00 |
| `constants` | 1.34 | 1.38 | +0.04 |
| `streams` | 0.02 | 0.03 | +0.00 |
| `readback` | 0.55 | 0.58 | +0.03 |
| `submit` | 0.07 | 0.08 | +0.01 |
| accounted total | 20.66 | 23.48 | +2.82 |

**2.49 of the 2.82 ms is one column**, and the other eight are within ±0.12. This is what a
same-binary A/B is supposed to look like and this project has never had one this clean.

#### The memo's value SCALES WITH DRAW COUNT — which reconciles three measurements

| where | draws | `outside` delta |
|---|---|---|
| operator, walk (§11) | ~5,500 | 1.36 ms |
| operator, soak (§12) | ~7,200 | **2.49 ms** |
| headless campaign | 6,000-7,000 | ~2.4 ms |

Of course it does: `BindShader` runs per shader-load packet and the packets scale with the
draws. The soak measured **3,010-3,047 loads/frame**, against 2,224 on the walk and 2,073
headless — 45% more. **So "the memo is worth N ms" is not a number, it is a slope**, and
§11's 1.8-2.0 ms and this section's 2.8-2.9 ms are the same finding at two loads. Quote the
draw count with it, always.

#### AND THE STRATEGIC FINDING: this place is NOT at the frame cap

§6ci §10 and the part 53 hand-off both said the open question was whether a heavier place
exists, because both routes this project could measure on had reached the cap and further
CPU items could therefore buy only headroom. **The operator found the place in one
session.**

* the soak sustains **7,162-7,529 draws with peaks to 8,562** — heavier than any place this
  project has ever measured, and it holds there for three minutes;
* **0% of its frames are on a pacing rung** in either arm. It is CPU-bound, not
  pacing-limited;
* `CZ_VK_FRAME_STATS` prints its own bill at **3.21-3.23 ms/frame** here — measured
  directly on their machine, a fourth independent confirmation — so uninstrumented the
  24.0 ms median is roughly **16.7-18.7 ms, ~53-60 fps**, at or just under the cap rather
  than pinned to it;
* the pump is **97.5-97.8% on CPU** and blocked 0.11-0.12 ms. It is saturated.

**So the remaining plan items buy frames here, not headroom, and this is the place to
measure them from.** The three options the hand-off posed are resolved in favour of the
first: find the worst place and measure there. It has been found.

#### What dominates the heaviest frame, now that the memo is gone

`record` is **8.69 ms of 20.66** — 42% of the accounted frame and the largest phase by a
factor of two. Its own split: vertex 21.0% (699 ns/draw), index 6.4% (214), state 4.6%
(154), residual 4.4% (148). Then `other` 4.11, `textures` 3.13, `outside` 2.77.

That reorders the plan. `GuardFold` (item 1.1) is charged inside `textures` and `other`,
which together are 7.24 ms; `record` is the `vkCmd*` calls themselves, which item 1.4 —
parallel command recording, explicitly deferred by the plan as "a genuine architectural
project" — is the only item that addresses. The plan's own instruction was to re-measure
first and let the numbers make that case rather than ambition. **They now make it.**

### §13. PRICING ITEM 1.4 — and 39% of its apparent size belongs to item 1.1

§12 put `record` at 8.69 ms of the operator's heaviest frame — 42%, twice the next phase —
and concluded that item 1.4 (parallel command recording) was the only item addressing it.
The operator asked for it to be priced before anything was built. **Pricing it moved a
third of it to a different item.**

#### Step 1: what `record` is made of, which nobody had asked

`ProfScope(streams)` wraps only the `CopySwapped` — deliberately, so a cross-frame guard
HIT costs the `streams` column nothing. That is the right design and it is why `streams`
reads 0.02 ms while `GuardFold` is the biggest symbol in the pump. But `UploadStream` is
called from **inside** the `recordVertex` and `recordIndex` scopes, so the hash was charged
to `record`. **Item 1.4 is priced off `record` and item 1.1 off `GuardFold`, so the same
milliseconds were in both prices.** Split (`g_prof.streamGuard`), measured on the outdoor
route at ~6,500 draws:

```
record 1,007 ns/draw = state 141 + vertex 188 + index 161 + GUARD 391 + residual 126
```

**The guard is 38.8% of `record`.** And the split makes the phase table and the symbol
profile agree for the first time:

| | |
|---|---|
| stream guard, from the phase split | 391 ns/draw x 6,508 draws = **2.54 ms**, 16.8% of the pump's work |
| `GuardFold`, from `perf` | **29.85% of the pump** = 4.52 ms |
| texture guard, by difference | **1.98 ms** — which is what `textures` should contain |

Two instruments of different classes, built years apart, now reconcile to the millisecond.
Neither could have produced this alone: the symbol profile knows `GuardFold` is huge but
not which *phase* pays for it, and the phase table knew `record` was huge but not that a
hash was inside it.

#### Step 2: item 1.4's real ceiling

Applying the 38.8% share to the operator's soak:

| | ms of their heaviest frame |
|---|---|
| `record` as measured in §12 | 8.69 |
| — the stream guard inside it → **item 1.1** | **3.37** |
| — the actual `vkCmd*` recording → **item 1.4** | **5.32** |

And the recording work is small per call, because the state cache already removes most of
it. From the counters the renderer has been keeping all along:

* **3.20 vertex-bind attempts per draw, 52.5% skipped → 1.52 real calls**
* 0.95 index-bind attempts, 40.6% skipped → 0.56 real
* pipeline 70% skipped → 0.30; viewport 99.1%, scissor 99.0%, blend and descriptor sets
  100% skipped → ~0.02 combined
* plus one push-constants and one draw

**≈ 4.4 `vkCmd*` calls per draw**, so 616 ns/draw of recording is ~140 ns a call. That is
driver-side work, and it is the kind that parallelises across independent command buffers.

| workers | ideal saving on their heaviest frame |
|---|---|
| 2 | 2.66 ms |
| 4 | **3.99 ms** |
| 8 | 4.65 ms |

**Minus** the overhead the plan already names and this pricing does not remove: a secondary
command buffer inherits no state except the render pass, so every secondary must re-bind
pipeline, sets, viewport, scissor and vertex buffers at its head — which is cheap only
because the draws must be split into CONTIGUOUS ranges anyway (order is semantic). Plus
`vkBeginCommandBuffer`/`vkEndCommandBuffer` per secondary and one `vkCmdExecuteCommands`.

#### The verdict, and it is the plan's own order

**Item 1.4 is worth ~4 ms at four workers, not the ~8.7 ms §12 implied — and item 1.1 is
worth more than the plan thought.** The guard is now measured at **3.37 ms inside `record`
plus 1.98 ms inside `textures` = ~5.3 ms of the operator's heaviest frame**, against the
plan's "20% of the pump, −2..3 ms".

So the two items are the same size, and they are not the same risk:

| | item 1.1 parallel guards | item 1.4 parallel recording |
|---|---|---|
| size | ~5.3 ms (both guards) | ~4 ms at 4 workers |
| what moves | a **pure** function: reads guest memory, returns a `uint64_t` | Vulkan command recording, which owns renderer state |
| ordering | none — each stream is independent | **semantic** — draw order must be preserved |
| oracle | the serial hash, byte-identical | none; a mis-ordered draw is a picture defect |
| failure mode | a wrong hash = a stale mesh, caught by a verify arm | wrong order or lost state = wrong picture, no gate |
| plan's own risk | medium | **high, "a genuine architectural project"** |

**Do 1.1 first.** It is the same size, it has an oracle, and it is already next in the
order. Item 1.4 stays a live candidate — the numbers do support it, which §12 could not
say — but it is now a ~4 ms item behind a ~5.3 ms one, not the 8.7 ms item that looked
like it had to be taken first.

> **The transferable half (gotcha 343): before pricing an item off a profiler phase, check
> what ELSE is inside that phase.** A scope is a region of code, not a subsystem, and two
> items priced off two instruments can silently share the same milliseconds. Here the
> overlap was 39% and it would have been spent on the riskier of the two.

---

## 6cj. Part 53: THE FIRST WORK THIS PORT HAS EVER MOVED ONTO ANOTHER CORE — and the
## first item whose BILL had to be measured as carefully as its benefit

Part 52 shipped four items, the operator confirmed them, and every one of them was
strategy **(a)**: make the pump thread's work smaller. `perf-plan-part52.md` §1 commits in
as many words to **(b)** — move work onto cores that are doing nothing — and puts it first;
its §10 order then front-loads three serial items ahead of the first parallel one, and
following the table rather than the prose cost a whole part. At the close of part 52 the
process used **2.24 of 16 cores** with the pump **97.5-97.8% on CPU** and ~13 cores idle.
There was nothing left in (a) at that load.

**Part 53 is item 1.1, and item 1.1 is (b).**

### §1. Why the guard and not something else

The plan's three questions for a movable piece of pump work — is it pure, is its input
knowable in advance, is there an oracle — and the content guard is the only large cost in
this renderer that answers yes to all three. It reads guest memory, returns a `uint64_t`,
touches no Vulkan and no renderer state; the incumbent serial hash is retained as a
same-binary arm and is byte-exact.

It is also the biggest thing there. Measured at the open of this part on the outdoor
DebugJump route with **both instruments off** (`tools/part52_recon.sh`, read with the new
`tools/part53_symbols.py`):

| symbol | % of the pump thread |
|---|---|
| **`GuardFold`** | **26.31%** |
| `DoDraw` | 16.27 |
| `[unknown]` (the nvidia driver) | 10.90 |
| `UploadStream` | 9.88 |
| `UploadTexture` | 6.80 |
| `memmove` | 6.45 |
| `WriteRegisterRun` | 4.89 |

> **A note on the tool, because it is the transferable half of the measurement.**
> `perf report --tid=N` does **not** renormalise: it shows only that thread's rows while
> printing each symbol's share of the WHOLE profile. Every hand reading of a per-thread
> profile in this project has had to correct for that, and getting it wrong turns a symbol
> at 26% of the thread that IS the frame into 8% of a process nobody is trying to speed
> up. `tools/part53_symbols.py` buckets by TID, folds `symbol+0xNNN` rows into one
> function, and divides by the THREAD. `--diff` prints two arms side by side.

### §2. The design, and the one thing it does not do

The pump discovers work by walking packets, so nothing can be handed to a worker in
advance — that is the obstacle the plan names. What makes it tractable is that the working
SET barely changes even when the contents do (part 22: 94-97% of stream bytes are identical
frame to frame). So:

* every guard the pump computes **files a job for the next frame** — a guest pointer, a
  length, a fold bound, and whether the exact variant was wanted;
* the **swap** dispatches the whole list to four workers, right after `++R->frame` and
  before `BeginFrame`, so the readback and the pre-draw packets are head start;
* the pump's existing `persistCache.find(key)` / `textures.find(key)` now also yields a
  slot index, so **the lookup costs nothing extra**;
* a miss — never seen, not finished, wrong variant — **hashes inline exactly as before.**
  Correctness never depends on the prediction; only performance does.

**Workers never touch `persistCache` or `textures`.** An `unordered_map` node's address is
stable but an erase is not, and a worker holding a pointer into a cache the pump is editing
is a use-after-free waiting for an unlucky frame. Each job is self-contained and each
result lands in an array the pool owns; the cache entry keeps only an index plus the frame
it was filed for, both written by the pump. A stale index therefore reads as a miss rather
than as another buffer's hash.

**Both guards move**: the cross-frame stream store's (the one charged to `record`, gotcha
343) and the texture cache's (charged to `textures`). They have different fold bounds and
were priced separately in §13; they are one mechanism here.

### §3. THE FAILURE MODE THAT WOULD BE SILENT, and what was built for it

Two things can go wrong and only one of them is a bug.

**A slot mix-up — one entry handed another entry's hash — is the bug**, and it is exactly
the shape of the part-52 memo defect: the wrong answer is *another real buffer's real
hash*, which compares equal or unequal for reasons that have nothing to do with this
buffer, and the symptom is a stale mesh with no error anywhere (gotcha 342). So every
result **echoes back the descriptor it was computed for**, the consumer checks it, and a
disagreement prints `[vk] PARALLEL GUARD SLOT MIX-UP` and falls back to hashing inline.
Zero in every run of this part.

**The race is not a bug, it is the item's real cost.** The guard has read guest memory
while the guest wrote it since the day it existed; a torn read produces a different hash,
reads as "changed", and is safe by construction. What pre-hashing changes is the WINDOW: up
to a frame, instead of the instant of the draw. And the new failure it admits is not a torn
read — it is a **coherent OLD state** that matches the stored guard where the inline hash
would have seen new bytes and re-copied. That is a one-frame stale mesh, the part-46 defect
class, so it was measured rather than argued.

`CZ_VK_VERIFY_PARALLEL_GUARD=1` hashes inline as well, at the draw, on every served guard:

```
[vkprof] guard prehash VERIFY: 16 of 3499109 served guards disagreed ... (0.0005%)
                               76 of 3664279                             (0.0021%)
                                8 of 3214914                             (0.0002%)
```

**8-76 per window, 0.0002-0.0021%** — and that is an **upper** bound on harm, because a
disagreement only damages anything when the pre-hash happens to equal the STORED guard
while the true value differs; most disagreements are streams that read as changed either
way.

`CZ_VK_VERIFY_PARALLEL_GUARD_POISON=1` perturbs every pre-hashed value and the same check
reads **100.0000%**. Without that arm the 0.0005% is a silent instrument rather than a
measurement (gotcha 30) — and this project has now twice shipped a check that could not
fail.

### §4. THE RESULT — one binary, arms by environment, matched on draw count

`CZ_FPS_CAP=120` in both arms, because at the shipped 60 fps cap this route sits on the
rung and the comparison reads zero whatever the change was worth (§6ci §5c). Phase
profiler, six windows an arm, quoted at the closest matched draw counts:

| | parallel (default) | control (`CZ_VK_NO_PARALLEL_GUARD=1`) |
|---|---|---|
| draws/frame | 6,549 | 6,501 |
| **frame** | **13.9 ms (72.1 fps)** | **15.9 ms (62.8 fps)** |
| `record` GUARD | **15 ns/draw** | 313 ns/draw |
| `textures` | 1.17 ms | 2.05 ms |
| `record` total | 4.34 ms | 6.09 ms |
| served by a finished pre-hash | 88-91% | — |
| pool blocked the pump | **0 times** | — |

And in symbols, from a `perf` profile of each arm:

| | parallel | control |
|---|---|---|
| **`GuardFold`, share of the pump thread** | **0.86%** | **25.87%** |
| pump thread, % of one core | **50.3%** | 63.4% |
| the guest Draw Thread (the load's own control) | 91.7% | 91.3% |

The Draw Thread is the honest check that the two runs saw comparable work: it is the
guest's own ring spin (finding 38) and neither arm touches it.

**−2.0 to −2.4 ms at 6,000-6,800 draws**, which is a **saving**, not a frame rate — at the
shipped cap this route is already at 60 and the saving buys headroom. The player-facing
number is owed from the operator's soak, which is not capped.

### §5. THE BILL, and it is the part of strategy (b) nobody had had to write down before

**Moving work onto idle cores raises the process's total CPU even when the frame gets
shorter**, and a saving reported without that is half a measurement:

| | parallel | control |
|---|---|---|
| process total | **2.68 cores of 16** | 2.53 |
| the four workers | 8.3% of a core **each** | — |
| pump thread | 50.3% | 63.4% |

Note what does **not** balance. The pump loses **13.1 points** of a core; the workers gain
**33.2**. Two and a half times as much CPU appears on the workers as leaves the pump, and
the honest reading is that a memory-latency-bound loop is *cheaper interleaved with other
work than run on its own*: on the pump the guard's misses overlapped with `DoDraw`'s
register decode and the driver's own work, and isolated on a worker there is nothing to
overlap with. **`perf`-attributed cycles for such a loop understate its isolated cost**,
which means a symbol share is a good guide to what to MOVE and a bad estimate of what the
move will cost elsewhere.

And there is a second, smaller bill charged straight back to the pump:

| phase | parallel | control |
|---|---|---|
| `recordState` | 181-186 ns/draw | 140-147 |
| `otherBegin` | 79-83 ns/draw | 56-58 |

Neither of those is code this change touched. It is the workers evicting the pump's working
set from the shared L3 — the state cache's `memcmp`s and `BeginFrame`'s walk of a few
thousand `streamCache` nodes both start missing. **~0.4 ms/frame**, against 2.8 ms of guard
removed and 2.2 ms of frame delivered; the difference is exactly this.

> **The transferable half: a parallel item's price is not its symbol share.** Budget for
> three separate things — the work that moves (measurable), the dispatch and lookup
> bookkeeping (small), and the CACHE the moved work stops warming and starts polluting
> (invisible in every instrument that names a subsystem, and it showed up here in two
> phases that have nothing to do with hashing).

### §6. THE FRAME-TIME CAMPAIGN — three runs an arm, alternated, with its own null

`tools/part53_item_campaign.sh`, one pinned binary, `CZ_FPS_CAP=120` in every arm so
neither lands on the rung, `CZ_VK_FRAME_STATS` in every arm (so its ~2-3 ms rides in both
and the ABSOLUTE times below are inflated by it — the relative saving is not). 84,540
frames in the item arm against 76,845 in the control.

**Item against control** (`CZ_VK_NO_PARALLEL_GUARD=1`):

| draws | control mean | item mean | Δ mean | Δ median | significance |
|---|---|---|---|---|---|
| 5,000-5,999 | 14.37 | 12.50 | **−13.0%** | −14.3% | −37.3 |
| 6,000-6,999 | 15.30 | **13.38** | **−12.5%** | −13.3% | **−34.9** |
| 7,000-7,999 | 17.04 | 14.82 | −13.1% | −17.6% | −11.0 |
| 8,000-8,999 | 18.58 | 16.17 | −13.0% | −11.1% | −17.3 |

**And the experiment's own null** (`base` against `base`), in the same bands:

| draws | Δ mean | Δ median | significance |
|---|---|---|---|
| 5,000-5,999 | −0.5% | +0.0% | −1.2 |
| 6,000-6,999 | **+0.1%** | **+0.0%** | **+0.2** |
| 8,000-8,999 | +1.6% | +0.0% | +5.8 |

A 12-13% effect against a 0-2% floor, holding across four adjacent draw bands, on medians
as well as means. The 3,000-3,999 band is the one to distrust: the null itself reads −5.7%
there, which is what that band does on its own.

**In milliseconds it is a SLOPE, like part 52's memo** — 1.92 ms at 6,000-6,999, 2.22 at
7,000-7,999, 2.41 at 8,000-8,999. Of course: the guard runs per first-touch stream and per
guarded texture, and both scale with the frame. **Quote the draw count with it.**

Raw throughput, which needs no binning at all: **84,540 frames against 76,845 in the same
3 x 330 seconds, +10.0%.**

> **`pinned%` is quoted here for a 16 ms ladder while the cap is 8.33 ms**, so read it as
> "landed near 16 ms", not "on the pacing floor" (the part-52 rule). It is why the control
> arm shows 79% at 6,000-6,999 and the item arm 6%: the control is sitting at ~15-16 ms
> and the item arm is not.

### §7. ITEM 1.3 — and the copy turned out to be for instruments that were not running

The plan sizes item 1.3 as "move the present readback onto a worker, ~0.55 ms". It never
needed a worker. The readback was **two** 3.5 MB copies: one from the mapped readback
buffer into `R->presentPixels`, and then `Host_PresentPixels`'s own into the window's back
buffer. The intermediate one existed so the present-side instruments — frame stats, the
PPM dumps, the black/dark triggers, the uniform-colour census — could walk a *cached*
buffer.

**But that buffer has been HOST_CACHED since the readback fix.** `ReadbackMemoryProps`
searches for a `HOST_CACHED` memory type and takes it; `CZ_VK_READBACK_UNCACHED=1` is
still the arm for the other case. So the instruments can read the mapped buffer directly
and the staging copy is 3.5 MB per frame for nothing.

| | default | `CZ_VK_PRESENT_STAGING=1` |
|---|---|---|
| `readback`, share of frame | **0.0%** | 5.5-7.3% |
| ms/frame | ~0 | ~0.78 |
| frame at a matched 6,253 / 6,255 draws | **13.0 ms** | 13.9 ms |

**The condition is the MEMORY TYPE, not which instruments are armed, and that is the
load-bearing choice.** The obvious implementation is "copy only if an instrument will read
it" — and it would have left the default configuration on a code path **no gate in this
project ever runs**, because every picture gate here (`CZ_VK_FRAME_DUMP`, the E3
correlation's `CZ_CAPTURE_KEY`, `CZ_VK_SNAP_ON_*`) sets one of them. Gating on
`g_readbackCached` instead keeps the default path the only path in an ordinary run, and
leaves the staging copy exactly where it is genuinely needed.

> **The transferable half: a fast path that only runs when no instrument is armed is a
> fast path nothing tests.** Ask what the gates set before choosing the predicate.

### §8. Gates at close — all clean

| gate | result |
|---|---|
| `--smoke` | OK |
| switch gate | 0 defects (2 benign frameless thunks) |
| shader dimension census | 0 disagreements; 1 sidecar without `tfetchDims`, the known lost-microcode entry |
| PM4 oracle 1/2 — packet lengths | 24,527,474 packets, **0 disagreeing** |
| PM4 oracle 2/2 — indirect-buffer walks | clean |
| E3 picture | **best of five +0.8808**, 4 of 5 agreeing on layout |
| `no translated shader` | 0 |
| `truncated=` | 0 |
| deepest file on a plain boot | **#83 `game:\data\skeleton\cinezombie.big`** |
| A5 kernel-call diff | **exit 0, 4 permutation windows, 0 real** |
| shader-cache NAME diff | 435 built from the dumps vs 436 in the cache, differing only by `ps_926c15dd20571cf1` (microcode lost) — **no new entry missed this part** |
| `PARALLEL GUARD SLOT MIX-UP` | **0**, over every run of this part |

E3 next to its neighbours, because a cross-session best-of on an ANIMATED backdrop is a
weak comparison (gotcha 133): part 50 +0.8820 of five, part 51 +0.8043 of fourteen, part 52
+0.8771 then +0.8764 of five, part 53 **+0.8808** of five. Nothing in this part touches
what is drawn — and item 1.3 in particular is now on the path the gate itself exercises,
which is the whole reason its predicate is the memory type.

### §9. WHAT PART 53 DID NOT DO

* **Item 1.2, parallel texture untile.** The pool now exists and is proved, which was the
  plan's precondition ("only after 1.1 proves the worker pool"). It is a harder dependency
  than the guard: the pump needs the *result* — a filled staging buffer — before it can
  record the upload, where the guard only needs a decision.
* **Item 1.4, parallel command recording**, still the largest remaining item at ~4 ms and
  still the riskiest.
* **The operator has not judged this yet.** Everything above is headless. The soak they
  found in part 52 is the place to measure it, and it is not at the cap.

### §10. THE OPERATOR'S SOAK A/B — the strongest measurement in this part, and one of its
### two items did not survive it

`ARM=ab tools/part53_operator_session.sh`, two soaks in the heaviest place they know, one
binary, both items switched off in arm B (`CZ_VK_NO_PARALLEL_GUARD=1
CZ_VK_PRESENT_STAGING=1`). Both arms carry `CZ_VK_PROFILE` and `CZ_VK_FRAME_STATS`, so
every absolute time below is inflated by ~3 ms in both.

#### Frame time, binned by draw count

| draws | control n | control mean | item n | item mean | Δ mean | Δ median | significance |
|---|---|---|---|---|---|---|---|
| 0-999 | 1,229 | 16.83 | 1,231 | 16.85 | **+0.1%** | +0.0% | **+0.0** |
| 2,000-2,999 | 1,041 | 16.04 | 1,354 | 16.12 | **+0.5%** | +0.0% | **+0.8** |
| 4,000-4,999 | 111 | 18.74 | 502 | 16.13 | −13.9% | −5.9% | −4.3 |
| 5,000-5,999 | 651 | 19.18 | 620 | 16.28 | −15.1% | −11.1% | −10.6 |
| 6,000-6,999 | 402 | 21.60 | 6,754 | 19.94 | −7.7% | −4.8% | −6.2 |
| **7,000-7,999** | **7,628** | **24.65** | **3,064** | **20.33** | **−17.5%** | **−16.7%** | **−50.2** |

The two light bins are the experiment's own null and they read **+0.1%** and **+0.5%**.
The two arms' soaks settled at slightly different draw counts — the control's in
7,000-7,999 and the item's in 6,000-6,999 — which is exactly what binning by draw count
exists to survive; both bins still hold thousands of frames on both sides.

**And the `pinned%` column says something the means cannot.** At 4,000-4,999 the control
is 55% pinned to the 16 ms ladder and the item arm **98%**; at 5,000-5,999, **29% -> 93%**.
Here the ladder IS the shipped 60 fps cap, so in those bands this is not headroom — it is
the game reaching its own cap where it previously could not.

#### The mechanism, per draw, which is the fair comparison at unequal draw counts

| | control (~7,470 draws) | item (~6,930 draws) |
|---|---|---|
| **`record`'s GUARD** | **518 ns/draw** | **13** |
| **`textures`** | **428 ns/draw** | **227** |
| `record` total | 1,244 ns/draw | 746 |
| `other` | 569 ns/draw | 614 |
| frame | 25.0 ms, **40.1 fps** | 20.1 ms, **49.8 fps** |
| guards served by a finished pre-hash | — | **97.8%**, 0 pending, 0 blocked |
| `PARALLEL GUARD SLOT MIX-UP` | — | **0** |

−505 ns/draw of stream guard and −201 of texture guard is **−4.9 ms at 7,000 draws**, and
the 97.8% hit rate is *higher* than the headless 88-91% because a soak's working set is
stable by construction.

#### THE BILL ON THEIR MACHINE, and the reading that changes how to think about the pump

| | control | item |
|---|---|---|
| process total | 2.64 cores of 16 | **3.11** |
| the four workers | — | **11.1-11.2% of a core each** (44.6 points) |
| **the pump thread** | **94.5% of a core** | **93.3%** |
| pump WORK per frame | 23.6 ms | **18.75 ms** |

**The pump did not get less busy — it stayed saturated, and that is the point.** The
headless route left it slack (63.4% -> 50.3%), so the item read there as a thread doing
less. Here the pump is pinned at ~94% in both arms and what changed is how much work each
FRAME costs it: **23.6 -> 18.75 ms, so the same saturated thread delivers 24% more
frames.** That is the shape a strategy-(b) item takes when the thread it shortens is
genuinely the critical path, and it is the more useful way to read one.

#### AND ITEM 1.3 DID NOT SURVIVE THE SESSION

`readback` went **0.58 ms (control) -> 0.66 ms (item)**. The headless measurement said
−0.78 ms. Both are right about what they measured, and the difference is that **headless,
`Host_PresentPixels` returns immediately** — so the staging copy WAS the entire readback
and removing it removed all of it. Windowed, the window's own copy runs, and one copy read
from the mapped buffer costs slightly more than a copy from the mapped buffer plus a copy
from a staging vector that is still hot in cache.

It cannot be separated from item 1.1 in this A/B — both were switched together — and the
most likely explanation is not that 1.3 is wrong but that it is being **taxed by 1.1**:
the workers stream 69.8 MB/frame at 49.8 fps, i.e. ~3.5 GB/s, and the same tax is visible
in `other` (+45 ns/draw) and `outside` (2.88 -> 3.30 ms). Item 1.1's +0.42 ms in `outside`
and +0.31 in `other` are the same phenomenon the headless run charged to `recordState` and
`otherBegin`.

**So item 1.3 is an OPEN QUESTION, not a shipped saving, and it needs its own arm** —
windowed, with the guard pool held constant in both. Recorded here rather than tidied
away, because a headless number that does not transfer is exactly what an operator session
is for, and this project has now had three of them.

#### The operator's LOOK/FEEL verdict, recorded as it came

> *"Look and feel is as usual."*

**What that does and does not establish.** The one class this part could have introduced is
a one-frame stale buffer — a HUD value lagging, a mesh snapping, a texture flicking for a
frame — and a whole soak in a crowd with none of it seen **rules out a GROSS regression**,
which is the way this change could have failed badly. It is not evidence that the widened
race never fires: the verify arm measures it at **0.0002-0.0021%** of served guards, and a
single wrong frame in a 50 fps crowd is not something a human reliably catches. The two
instruments answer different questions and both are needed.

It is also not "confirmed smoother", and is not written up as such — both arms carried
~3 ms of instrument, which is most of the difference being judged. The frame-time claim
rests on the binned statistics above, not on this line. Same shape as part 51 §6ch §7,
where the operator could not tell the tick arms apart while the profiler showed the win
plainly.

### §11. ITEM 1.3'S OWN ARM — the item is real, and §10's reading of it was a CONFOUND

§10 read `readback` 0.58 -> 0.66 ms across the operator's two arms and filed item 1.3 as
"did not survive". **That is retracted here.** Their A/B switched BOTH items together, and
the other item taxes this one.

Item 1.3's own arm: windowed, driven by synthetic input on the DebugJump route,
`CZ_FPS_CAP=120`, **the guard pool ON in both sides**, `CZ_VK_PRESENT_STAGING` the only
thing that differs. Thirteen profile windows an arm:

| | `readback` ms/frame |
|---|---|
| no staging copy (the default) | 0.525 0.576 0.707 0.687 0.697 0.684 0.691 0.686 0.693 0.677 0.673 0.693 0.700 — **mean 0.668** |
| `CZ_VK_PRESENT_STAGING=1` | 0.894 1.009 1.283 1.255 1.242 1.195 1.251 1.254 1.210 1.028 1.025 1.069 1.035 — **mean 1.135** |

**−0.467 ms, and the two sets do not overlap at all** (highest without: 0.707; lowest
with: 0.894). `readback` is a fixed 3.5 MB per frame and does not scale with draws, which
is why thirteen windows at wildly different draw counts are all the same number within an
arm — and why this is decisive at one run an arm.

#### Putting the three configurations side by side explains everything, including §10

| guard pool | present copies | `readback` | where measured |
|---|---|---|---|
| **off** | 2 (staging) | **0.56 ms** | the operator's arm B |
| **on** | 2 (staging) | **1.14 ms** | this arm, `CZ_VK_PRESENT_STAGING=1` |
| **on** | 1 | **0.67 ms** | this arm, default — and **0.66 ms** in the operator's arm A |

Read down the table: **the guard pool DOUBLES the cost of the present copies** — 0.56 ->
1.14 ms for the same two `memcpy`s — because four workers streaming ~70 MB/frame leave far
less memory bandwidth for a 3.5 MB copy. Item 1.3 then takes that 1.14 back to 0.67.

So in the operator's session item 1.3 was **saving them ~0.47 ms**, and it looked like
+0.08 only because the arm it was compared against had the workers switched off as well.
Their arm A and this arm's default agree to **0.01 ms**, which is what makes the table a
measurement rather than a story.

> **The transferable half: when one A/B switches two items together, an item can read
> NEGATIVE because the other one taxes it.** That is not noise and no number of repeats
> would have fixed it — the arms were answering a different question from the one being
> asked. The fix is the obvious one and it is cheap: give each item an arm that holds the
> other constant. It cost fifteen minutes here and it saved reverting a change that works.

**And a number worth keeping on its own: the guard pool's bandwidth footprint is large
enough to double an unrelated 3.5 MB copy.** Every remaining parallel item in the plan —
1.2 especially, which moves texture UNTILING, a pure bandwidth job — has to be priced
against that, not just against the CPU it frees.

### §12. THE OPERATOR CORRECTS §11's COVERAGE — and it moves item 1.3 the GOOD way

> *"Your session pretty much stayed at the military camp where load is light and my
> session is where load is the heaviest."*

They are right, and it invalidates one sentence of §11 outright. Item 1.3's arm ran
**1,891-4,777 draws/frame**; their soak was **7,000-7,500**. §11 said "thirteen windows at
wildly different draw counts are all the same number within an arm", and that is **wrong**
— it was read off the arm means without looking down the columns. Paired by draw count:

| draws | 1 copy (default) | 2 copies (`CZ_VK_PRESENT_STAGING=1`) | Δ |
|---|---|---|---|
| ~1,890 | 0.525 | 0.894 | **−0.369** |
| ~2,580 | 0.576 | 1.009 | −0.433 |
| ~3,400 | 0.687 | 1.210 | −0.523 |
| ~3,800/3,900 | 0.686 | 1.242 | −0.556 |
| ~4,100/4,200 | 0.693 | 1.283 | **−0.590** |
| ~4,570/4,780 | 0.700 | 1.251 | −0.551 |

**`readback` is a fixed 3.5 MB copy and its COST still rises with the frame's load** — 0.525
-> 0.700 ms across the light range with one copy — because what sets it is the memory
bandwidth left over, and the guard pool is most of what takes that. So **item 1.3's saving
is a slope like everything else in this part**: −0.37 ms at 1,900 draws, −0.55 to −0.59 at
4,100-4,800. The mean of −0.467 quoted in §11 is the mean of a slope over a range that does
not include the operator's load, and should not be quoted on its own.

**The direction is the point: the operator's load is above the whole of that range, so
item 1.3 is worth MORE there than this arm can show, not less.** Their caveat improves the
item.

#### What has to be downgraded, said plainly

§11's three-configuration table compared their arm B (pool OFF, 2 copies, **40 fps**) with
this arm (pool ON, 2 copies, **~95-120 fps**). Two things differ, not one, so
"**the guard pool DOUBLES the cost of the present copies**" is an INFERENCE across
non-comparable runs and not a matched measurement. What survives it:

* the within-arm slope above, which is direct evidence that the cost tracks memory
  pressure on ONE route with everything else held constant;
* the pool's own traffic, which dominates that pressure: **2.2-3.0 GB/s** in every
  configuration measured here (23 MB/frame x 95 fps; 30 MB/frame x 100; **69.8 MB/frame x
  49.8 on their machine**);
* their arm B, the only pool-OFF measurement in existence, at 0.56 ms for TWO copies —
  cheaper than one copy costs with the pool running anywhere.

The residual §11 could not explain — their 1-copy heavy arm at **0.66 ms** against this
arm's 4,573-draw window at **0.70** — dissolves once the quantity is read as bandwidth per
SECOND rather than per frame: at 40 fps they do 140 MB/s of present copies where this route
does 350, on a similar pool load. Per-frame load is the wrong axis; per-second pressure is
the right one.

#### And the correction to gotcha 347's magnitude, not its lesson

The confound is real and the lesson stands: their combined arm had the pool switched off
alongside item 1.3, so item 1.3 was measured against a machine with no memory pressure and
read as +0.08 ms when it was saving them time. What is downgraded is the SIZE — "doubles"
becomes "the pool is most of the memory pressure that sets this cost" — and the size was
never what the gotcha was about.

**Owed, and it is cheap: fold a `CZ_VK_PRESENT_STAGING`-only pair into the next operator
soak.** Two three-minute holds at their load, everything else constant, and item 1.3 has a
number at the load that matters. It is 0.5 ms, so it does not justify an evening on its
own — but it costs nothing next to an item that does.

### §13. THE UNCAPPED PLAY SESSION — and the shipped 60 fps default now COSTS frames

The operator asked what `CZ_FPS_CAP=120` in the measurement runs meant and whether the cap
could come off entirely. **It cannot, and that is deliberate**: the knob moves the vblank
PERIOD with the title's own present interval pinned at 2, and interval 0 (present
immediately) overflows the flip queue in 10 runs out of 10 (`gpu/vd.cpp`). Its maximum,
500, gives a **1 ms period and a 2 ms ceiling** — nothing in this game approaches that, so
it never binds. `tools/play_session.sh` is that configuration with every instrument off
except `CZ_FPS_LOG`, which is one counter and one clock read per presented frame.

Their session, ~4 minutes, `[fps]` medians:

| where | fps | ms |
|---|---|---|
| menus / title | 166 | 6.0 |
| light zones | **119-147** | 6.8-8.4 |
| ordinary gameplay | 83-114 | 8.7-12.1 |
| **their soak spot** | **69-71** | **14.05-14.34** |

**Nothing clusters at a ceiling** — 166, 147, 129, 119, 114, 111, 105... — which is the
check that the cap really is out of the way, and it is why the operator's "light zones
reach 125" is a workload number and not a rung.

#### The instrument bill, measured a fifth time and most tightly

Their instrumented soak in §10 read **49.8 fps / 20.1 ms** at the same place this reads
**69 fps / 14.44 ms**. That is **~5.7 ms of `CZ_VK_PROFILE` + `CZ_VK_FRAME_STATS`** on
their machine, against the 2-4 and 1.9-3.3 quoted separately — the first time both have
been removed at once at a known place. Every absolute frame time in §10 is inflated by it;
the A/B deltas are not.

#### AND THE FINDING THAT CHANGES A DEFAULT

**Their clean soak frame is 14.44 ms, which is UNDER the 16 ms ceiling the shipped 60 fps
cap imposes.** The title's presents are vblank-quantised, so at `CZ_FPS_CAP=60` (period 8)
the ladder is 16 / 24 / 32 ms and a 14.44 ms frame **presents at 16 — exactly 62.5 fps**.
In light zones a 6.8 ms frame presents at 16 too.

Before part 53 this did not matter: they were at ~20 ms at the soak, above the ceiling, so
the cap never bound. **Part 53 took them under it, and the default now costs them
frames** — 62.5 where the work supports 69 at the soak and 119-147 in light zones.

The period is what sets the granularity, and it has to be FINE, not merely high:

| `CZ_FPS_CAP` | period | what a 14.44 ms frame presents at |
|---|---|---|
| 60 (shipped) | 8 ms | 16.0 ms — **62.5 fps** |
| 120 | 4 ms | 16.0 ms — 62.5 fps |
| 250 | 2 ms | 16.0 ms — 62.5 fps |
| **500** | **1 ms** | **15.0 ms — 66.7 fps** |

Raising the cap without shortening the period buys nothing at this frame time, which is
not obvious and is worth saying: **the ladder's STEP is the lever, and the step is the
period.** The cost of the top setting is that the period is also the guest's vblank ISR
cadence — **1000 a second against 125** — which measured 0.0% of the pump at 4 ms and now
has ~4 minutes of evidence at 1 ms and no complaint.

#### Gates from the session

* **`no translated shader` = 1** — `vs_44a271ebee6e6354`, and `CZ_SHADER_DUMP` was set so
  the microcode was captured rather than lost.
* **The NAME diff then found a SECOND one the counter never reported**,
  `ps_22a996258bacd2c8` — no run bound it, so the miss counter read 1 and not 2. **Fourth
  part running that the name diff catches an entry the count cannot.**
* Both caches rebuilt and re-checked: **436 -> 438**, `assets/shader_spv` and
  `assets/shader_spv_a2m` identical in membership, dimension census 0 disagreements.
* **0 `PARALLEL GUARD SLOT MIX-UP` over a real play session**, which is the first time
  part 53's guard has been exercised anywhere other than the two headless routes.

### §14. INTERNAL RESOLUTION SCALING — and it is nearly FREE exactly where the frame is
### worst, which is the opposite of the intuition

The operator asked for a resolution knob "so the game looks crisp and we can better test
the GPU". `CZ_VK_RES=2560x1440` (or `CZ_VK_RES_SCALE=2`). Their verdict on the picture:
**"Perfect looks all good."**

#### What it does, in one line

The title still renders at 1280x720 in its own coordinates — its vertex positions, its
viewports, its scissors and its resolve extents are its numbers and none of them changed.
What scales is the **rasterisation target** they land in, so the same triangles are sampled
at four times as many points.

**The invariant the whole change is an instance of: a surface whose pixels come from the
RENDER PIPELINE scales; a surface whose pixels come from GUEST MEMORY does not.** The EDRAM
stand-in, the resolve snapshots, their right-sized views and the cube map the title renders
itself are the first kind. An uploaded texture is the second — there is no more data in
guest memory than the guest put there, and inventing some would be a different feature (an
upscaler) wearing this one's name.

The consequence is that every guest coordinate is multiplied **once, on its way into
Vulkan, and nowhere else**. `edramWidth`/`edramHeight` therefore stay in guest pixels: they
are the denominator of the window-coordinate-to-NDC mapping, which is
resolution-independent by construction. Those two used to equal `R->color.width`, so every
site that compared a guest coordinate against the image now compares against `edramWidth`
— **substitutions that are identities at scale 1**, which is what makes the default arm
provably the old code rather than a rewrite that happens to agree.

`Snapshot` gained `guestW`/`guestH` for the same reason: a fetch declaring 640x360, a
sub-region fold looking for "a surface of the same extent", and the resize check that
rebuilds a reused address are all asking about the surface the TITLE resolved, not about
how many host pixels we chose to keep it in.

Integer multiples only, and an unsupported value is refused loudly (gotcha 5): a fractional
scale would put a fraction into the tile scissors, and this title renders in two 640-wide
halves where half a pixel of scissor error is a **seam down the middle of the screen that
no counter would report.**

#### THE MEASUREMENT, and it reorders how to think about the GPU

Their play session, same route, same build, `CZ_FPS_LOG` medians:

| where | 1280x720 | **2560x1440** | cost |
|---|---|---|---|
| menus / title | 166 fps | 183 | — (both trivial) |
| light zones | **119-147** | **96-97** | **−30 to −35%** |
| ordinary gameplay | 83-114 | 74-92 | −10 to −20% |
| **the heavy end** | **69-71** | **66** | **−4 to −7%** |

**Four times the pixels costs almost nothing where the frame is worst.** That is not a
surprise once it is written down — gotcha 231 measured the GPU idle 68% of every frame, and
part 51 established that our pump thread is the critical path — but it inverts the usual
expectation about a resolution setting:

* **in a crowd we are CPU-bound, so 1440p is nearly free**: 69-71 -> 66 fps for four times
  the sampling;
* **in a light zone we were NOT CPU-bound, so 1440p costs what it should**: 147 -> 96.

So this knob converts idle GPU into picture quality precisely where the frame has none to
spare, and costs frames only where there were frames to spare. **It is the first change in
this port that makes the GPU the limiter anywhere**, which is what the operator asked for
in the second half of their sentence, and it gives every future GPU-side item a place to be
measured that did not exist this morning.

#### The bill, and where to look first if a frame time surprises

The present readback is the scale SQUARED in bytes: **3.5 MB/frame at 1x, 14.1 at 2x.**
That is four times the copy part 53 spent the afternoon shrinking (§11), and at 2x it is
the single largest fixed per-frame cost in the renderer. `readback` in `CZ_VK_PROFILE` is
where it lands, the startup line says the number out loud, and **it is now the strongest
argument yet for a real swapchain** — the plan's §7 item, which has been deferred as "a
different and much larger job" since part 52 and would remove the copy entirely rather than
shrinking it.

The window is not resized: the bigger image is filtered down into whatever the window is,
which is supersampling. Dragging the window out is what puts the resolution on screen
rather than only in the sampling.

#### What this does NOT fix, said out loud because it will be noticed

The title's post chain computes its blur taps from texel offsets IT supplies, in units of a
1280-wide surface. Those are NORMALISED offsets, so a tap still lands the same fraction of
the screen away and a blur keeps its screen-space size — it is simply sampled at fewer taps
per pixel than the artist intended. That is the ordinary, accepted outcome of resolution
scaling and not a defect to chase.

### §15. GATES RE-RUN AT THE CLOSE — including the resolution change, at BOTH scales

§8's pass predates the resolution knob, the frame-cap default change and `CZ_FPS_LOG`, so
it was re-run whole. **ALL CLEAN.**

| gate | result |
|---|---|
| `--smoke` | OK |
| switch gate | 0 defects (2 benign frameless thunks) |
| shader dimension census | 0 disagreements; 1 sidecar without `tfetchDims`, the known lost-microcode entry |
| PM4 oracle 1/2 — packet lengths | 24.5 M packets, 0 disagreeing |
| PM4 oracle 2/2 — indirect-buffer walks | clean |
| **E3 picture at 1x** | **best of five +0.8396**, 4 of 5 agreeing on layout |
| **E3 picture at 2x (`CZ_VK_RES=2560x1440`)** | **best of five +0.8562**, 4 of 5 agreeing, captures 2560x1440 |
| `no translated shader` | 0 at both scales |
| `truncated=` | 0 |
| deepest file on a plain boot | **#83 `game:\data\skeleton\cinezombie.big`** |
| A5 kernel-call diff | **exit 0, 4 permutation windows, 0 real** |
| `PARALLEL GUARD SLOT MIX-UP` | **0** |

**The 2x row is the one that matters most, because until it ran the resolution change had
exactly one oracle and it was the operator's eye.** It now has a second that is not ours:
the picture at 2560x1440 correlates with hardware's own screenshot of the same screen as
well as the 1x picture does, and agrees on layout on the same 4 of 5 samples.
`frame_signature.py` compares across sizes already (E3 is 1401x1006), so this needed no new
tooling — which is worth noting as a reason the gate was affordable at all.

#### And a small instance of gotcha 133 caused by the cap change

E3 read **+0.8808** earlier in the part and **+0.8396** here, on the same code path at
scale 1. The reason is in the capture filenames: the earlier pass sampled frames
**1,896-3,853** and this one **5,890-13,874**. The gate presses F9 on a fixed schedule and
the frame cap moved from 60 to 500, so far more frames elapse in the same 120 seconds and
the five presses land on completely different moments of an ANIMATED backdrop. Nothing about
what is drawn changed. **A frame-index-addressed sample of a moving scene is re-aimed by
anything that changes the frame rate** — which is a fact worth having written down before
someone reads a future E3 drop as a regression.

---

## 6ck. Part 54: THE PRESENT WAS COPYING THE FRAME THREE TIMES, AND ONE OF THOSE COPIES
## HAD NEVER BEEN MEASURED — plus the route to the outdoor world was a coin flip

Part 53 closed by promoting the plan's §7 swapchain item on the strength of an
**arithmetic** claim: "the present readback is the scale squared in bytes, 3.5 MB/frame at
1x and 14.1 at 2x, and at 2x it is the single largest fixed per-frame cost in the
renderer." That is a multiplication. Nobody had read it off a running game, and every part
of this project that built before it measured has repriced or killed the item afterwards.
So part 54 measured it first — and then, before it could measure anything at all, had to
fix the route.

### §1. THE ROUTE TO THE OUTDOOR WORLD WAS WINNING A 150 ms RACE, AND IT LOST ONE

The part opened by re-taking the symbol budget, which the hand-off asks for and which is
six minutes. It took **seven** and sampled the wrong place: the run parked at the
`WAITJUMP` barrier for its whole length and profiled the prologue.

The cause is in `CZ_FAKE_PRESS_SEQ`. A host debug edge — `F2`, `F3`, `F4`, `F9` — fired
only if the guest happened to poll `XamInputGetState` inside a **150 ms window at a fixed
wall-clock offset**, 8.000-8.150 s for the first entry. Whether the guest polls input at
all in those 150 ms is not something this runtime controls: during a load it may not poll
for seconds. On a miss the edge was **lost** — `idx` walked on, the DebugJump screen was
never requested, and the entire recipe degraded to "press START a lot" while the run
continued for its full seven minutes.

**It fails intermittently, which is the worst version**, and the diagnosis went wrong
first in a way worth recording. One run an arm said the frame-cap default change (60 → 500
at the close of part 53) had broken the route: cap 60 delivered the edge, cap 500 did not.
Three runs an arm, six minutes later, read **3/3 and 3/3** — the cap has nothing to do with
it. That is gotcha 159 exactly, committed on the first suspect that came to hand.

The fix fires the lowest un-fired host edge whose interval has been reached, on the first
poll after it is reached, one edge per poll, at most once each. Ordering is preserved and
an already-delivered edge cannot re-pulse — the two properties the old single-index latch
existed to give. While `WAITJUMP` is parked the sequence clock is frozen at the barrier, so
a lost `F2` is **still eligible there**, which is the case that matters: the barrier is
waiting for exactly the screen request that the lost `F2` was supposed to cause.

**`CZ_FAKE_PRESS_EDGE_MISS=1` is its positive control and it is the half that makes this a
measurement.** When the window IS hit, the new code is indistinguishable from the old
latch — the first eligible poll is inside the window — so the recovery path is exactly the
part no ordinary run exercises (gotcha 30). The arm makes the window unhittable, so the
edge can only be delivered late. Same recipe, same binary:

| arm | edge | DebugJump |
|---|---|---|
| normal | `F2` at 8s | serviced at 27s |
| `CZ_FAKE_PRESS_EDGE_MISS=1` | `F2` at 8s, **LATE — the recovery** | serviced at 27s |

### §2. THE SYMBOL BUDGET, RE-TAKEN — and the headless route is a different machine now

With the route working, `tools/part52_recon.sh` + `tools/part53_symbols.py`, outdoors in a
crowd, instruments off:

| pump thread (26.5% of the process's cycles) | share |
|---|---|
| `DoDraw` | **24.43%** |
| the NVIDIA driver, unsymbolised | **15.13%** |
| `UploadStream` | **12.84%** |
| `WriteRegisterRun` | 9.10% |
| `UploadTexture` | 8.69% |
| `ExecutePacket` | 5.77% |
| `SynthRectStream` / `ExecuteLinear` | 2.76 / 2.36% |
| `_int_malloc` | 2.00% |

`GuardFold` — a quarter of this thread at the start of part 53 — does not appear. Two
readings matter more than the ranking:

* **The pump is at 93.7% of a core headlessly**, where it was 50.3% at the close of part
  53. The frame-cap default moving 60 → 500 took the headless route off the pacing rung,
  so **it now behaves like the operator's machine** — which retires §6ci §5c's warning that
  a headless A/B here reads zero whatever the change was worth. The process uses **3.75 of
  16 cores**, up from 2.68, and four guard workers sit at 14.3% each.
* **The 15.13% `[unknown]` is the driver executing our `vkCmd*` calls** (`libnvidia-glcore`
  is 3.95% of the whole process and the pump is 26.5% of it — the arithmetic closes). It is
  the second-largest cost on that thread and nothing can make it faster except making
  fewer calls, which is what item 1.4 would do.

### §3. THE MEASUREMENT THE PROMOTION NEEDED — and it is bigger than the arithmetic said

`Host_PresentPixels` returns immediately when there is no window, so `readback` reads
**0.0% on every headless run this project has ever taken**, including the ones part 53 used
to declare item 1.3 done. The cost exists only with a window, and it is three copies of the
frame, not one:

1. **GPU** — `vkCmdCopyImageToBuffer`, colour image → host-visible buffer;
2. **pump** — `memcpy` into the window's back buffer, under `g_frameMutex`;
3. **window thread** — `SDL_UpdateTexture` from that buffer, under the *same* mutex.

Only (2) is charged to `readback`. `tools/part54_present_cost.sh` is the windowed harness —
the recon's route and event gate, `CZ_VK_PROFILE` and per-thread CPU, one variable between
arms:

| | 1280x720 | 2560x1440 |
|---|---|---|
| `readback` | **8.1-8.7%** of the frame | **16.4-22.6%** |
| in ms, at ~2,900-3,700 draws | ~0.65 ms | **~1.7-2.2 ms** |
| `submit gpu` (blocked on the GPU) | 0.0-2.8% | **2.1-14.7%** |
| the window thread (main) | 8.8% of a core | **15.0%** |

Three things fall out of that table:

* **At 2x the readback is the largest single non-draw phase**, and it is the only cost in
  this renderer that grows when the operator raises the resolution. The arithmetic
  under-stated it: the claim was 4x the bytes, the measurement is ~3x the milliseconds at
  4x the bytes (a larger `memcpy` gets better bandwidth) — but it is measured against a
  frame that is itself longer, so the SHARE roughly doubles either way.
* **`submit gpu` at 14.7% is the GPU being the limiter**, which is what the resolution knob
  was supposed to produce and had not yet been seen doing.
* **The window thread's cost doubles too** — 8.8% → 15.0% of a core — which is copy (3),
  and no instrument in this project reports it because none of them reads that thread.

### §4. THE SWAPCHAIN — what was built, and the three things it deliberately does not do

`CZ_VK_SWAPCHAIN=1`. The window is created with `SDL_WINDOW_VULKAN` and no `SDL_Renderer`;
the renderer creates a surface from it, acquires an image at present time, blits the
finished frame into it and presents on a **semaphore** rather than a fence. `readback`
reads 0.0%.

**The decision has to be made before the window exists**, and that is why the arm is an
env-var read in `Host_WindowInit` rather than a renderer option: `SDL_WINDOW_VULKAN` is a
creation flag, and a window carrying it cannot also carry an `SDL_Renderer` (SDL2 has no
Vulkan renderer backend). So the two present paths are mutually exclusive by construction,
which is the honest shape — two arms, not a fallback chain. The renderer asks the WINDOW
whether the flag took, not the environment, so a window that failed to create with the flag
takes the renderer down the readback path with it and says so.

Three deliberate non-goals:

* **Nothing renders INTO a swapchain image.** The images are `TRANSFER_DST` only and the
  frame arrives by `vkCmdBlitImage`. The renderer's dynamic rendering, its pipeline colour
  formats and its whole resolve chain are therefore completely unaware a swapchain exists
  — which is what makes this an arm rather than a rewrite.
* **Vulkan does not move onto the window's thread.** The surface is created from the window
  (SDL documents that as thread-safe) and every other call runs on the pump, where the rest
  of the renderer runs. Phase 3's separation was about not depending on the WINDOWING
  SYSTEM'S thread, and that still holds.
* **BLIT, not copy, and LINEAR.** The swapchain is the window's size and the frame is the
  internal resolution, and those stopped being the same number when `CZ_VK_RES` existed. A
  copy would require them equal and would present a crop; `NEAREST` would alias a 2x image
  down into a 720p window and make the resolution knob look like a downgrade.

**The present MODE is a choice the SDL path never had.** Part 49 spent a session
discovering that a compositor throttles `SDL_RenderPresent` to the display refresh whatever
SDL was asked for, with a sharp failure: a frame just over 16.67 ms snaps 60 → 30. MAILBOX
is stated here rather than requested; FIFO is the fallback and is **named in the log** when
it is all the surface offers, because a run silently on FIFO is a run whose frame rate is
the monitor's. `CZ_VK_SWAPCHAIN_FIFO=1` is the arm for that question.

Two hazards were closed by inspection rather than by a symptom, and both are worth naming
because their symptom would have appeared nowhere near their cause:

* **An acquired image with no submit.** An acquired image comes with a semaphore the
  presentation engine will signal, and something must wait on it; abandoning the frame
  after acquiring leaves it signalled with no waiter and the next acquire reuses it
  illegally. Both ways of abandoning a frame here — `CZ_VK_NO_SUBMIT`, and a command buffer
  that is not recording — are knowable BEFORE the acquire, so the acquire is simply not
  made.
* **A rebuild destroying pending semaphores.** A resize or an out-of-date surface rebuilds
  the swapchain, and the old objects can still be pending on a submit or a present. The
  device is idled first; rebuilds are rare, so it costs nothing anyone measures, and
  without it the failure lands several frames later as a device lost with no visible
  connection to a window resize.

### §5. THE PICTURE GATE HAD TO BE NEW, AND THE FIRST ONE READ BLACK BECAUSE THE MONITOR
### WAS ASLEEP

**Not one of this project's picture gates can see this arm.** `CZ_CAPTURE_KEY`,
`CZ_VK_FRAME_DUMP`, `CZ_VK_FRAME_STATS` and the E3 correlation all walk the present
READBACK — the exact copy the swapchain removes. Run them against this arm and they pass
**with the old path still doing all the work**; take the readback genuinely away and they
see nothing. Either way the gate looks healthy while measuring something the change never
touched. That is gotcha 350, and it is gotcha 345's shape one step further out.

The right oracle is a grab of the actual display, because the compositor is the one thing
in this pipeline that neither our renderer nor our instruments produced.
`tools/part54_swapchain_picture.sh` is that gate — and every grab it took at 01:35 came
back **uniformly black**, RGB extrema `(0,0)` on every channel in both arms, scoring
`+0.0000` against every orientation. Nothing was wrong with the renderer, the grabber or
the gate: **the display was asleep.** Gotcha 231's trap, one subsystem over.

**That is a test, not an inference**, and it is worth saying which: "the monitor was
asleep" is exactly the kind of comfortable explanation this project files under
"measure it" (gotcha 3's shape — a black image is a capture failure, not a fact about the
window). The same command, run later in the day with the screen in use, returns a
**6452x1694 grab with extrema (0,255) on every channel**. The grabber and the gate are
fine; what the earlier run measured was the display's power state. (It also
found that the tool is compositor-specific: KWin does not implement the `wlr-screencopy`
protocol `grim` needs and says so clearly, which is the better of the two failures;
`spectacle -b -n -f` goes through KWin's own interface.)

So the night-time gate reads back **the image actually handed to the presentation engine**
— `CZ_VK_SWAPCHAIN_DUMP=<dir>`, in the channel order the surface format declares — and
correlates it against capture E3, Xenia's own screenshot of that screen. The blit's scale,
filter, orientation and channel order are all inside that number:

| | |
|---|---|
| best identity correlation, 38 dumps | **+0.8831** |
| frames reporting LAYOUT AGREES | **21 of 38** |
| the E3 gate's own standing figure | +0.8396 … +0.8808 |
| Vulkan validation, swapchain/present/blit/layout messages | **0** |

The dump arm requests `TRANSFER_SRC` on the swapchain images, which the default arm does
not, so it is strictly a different swapchain: read it as a picture gate and never as a
frame time. And what it proves is bounded, said out loud — the pixels handed to the
presentation engine are the right pixels; **that they are displayed is something only a
screen can show**, and that is the operator's judgement to make.

### §6. WHAT IT IS WORTH — three rounds an arm, both resolutions, with the campaign's own
### null

`tools/part54_present_cost.sh` × 3 rounds × 2 resolutions × 2 arms, alternated, one
variable between arms, **one frozen binary** (`md5 abeca2f4…`; a mid-campaign rebuild is
why the first, mixed-binary campaign was discarded rather than reported). Read with
`tools/part54_swap_bins.py`, which bins the profiler's own windows by DRAW COUNT — the two
arms never see the same draw list, and one round put them at 1,806 and 4,039 draws in the
same window.

**1280x720**, median ms/frame per band, 21 profiler windows an arm:

| draws | base | swapchain | delta | `readback` base → arm |
|---|---|---|---|---|
| 500-999 | 3.70 | 3.60 | −2.7% | 8.5 → 0.0 |
| 2,000-2,499 | 7.10 | 6.50 | −8.5% | 7.8 → 0.0 |
| **2,500-2,999** (n=7/4) | **7.20** | **6.60** | **−8.3%** | 7.5 → 0.0 |
| 3,500-3,999 | 9.30 | 8.60 | −7.5% | 7.3 → 0.0 |
| 4,000-4,499 | 9.40 | 9.50 | +1.1% | 7.0 → 0.0 |
| 4,500-4,999 (n=1/2) | 10.40 | 10.00 | −3.8% | 6.5 → 0.0 |

**2560x1440**, 21 windows an arm:

| draws | base | swapchain | delta | `readback` base → arm |
|---|---|---|---|---|
| 500-999 | 7.60 | 3.80 | **−50.0%** | 36.2 → 0.0 |
| 2,000-2,499 | 10.20 | 7.40 | −27.5% | 24.6 → 0.0 |
| **2,500-2,999** (n=9/9) | **10.20** | **7.00** | **−31.4%** | 24.5 → 0.0 |
| 4,000-4,499 | 12.30 | 9.20 | −25.2% | 21.7 → 0.0 |
| 5,000-5,499 (n=1/1) | 13.50 | 11.10 | −17.8% | 16.0 → 0.0 |

**And the campaign's own null**, rounds a+b against round c *within* an arm — the
comparison that cannot move, run on the same data:

| null | 500-999 | 2,500-2,999 | 4,000-4,499 |
|---|---|---|---|
| base vs base, 1x | +2.7% | −0.7% | −2.6% |
| base vs base, 2x | +5.5% | **+0.0%** | — |
| swapchain vs swapchain, 2x | +0.0% | +0.7% | −1.1% |

In the best-populated band the null is **−0.7%, +0.0%, +0.7%** against signals of **−8.3%
and −31.4%**.

#### Three readings worth carrying

* **It removes MORE than the phase it zeroes.** At 2x, 2,500-2,999 draws: `readback` is
  24.5% of a 10.20 ms frame, i.e. 2.50 ms, so zeroing it predicts 7.70 ms. Measured
  **7.00 ms.** The extra 0.7 ms is the two copies no instrument here charges to anything —
  the GPU's image-to-buffer copy (visible only as `submit gpu`) and the pump's wait on
  `g_frameMutex` while the window thread runs `SDL_UpdateTexture` under it.
* **The saving is a SLOPE the other way round from every previous item.** Parts 52 and 53
  each shipped a saving that grew with the draw count, because their work ran per draw or
  per packet. This one is a FIXED cost per frame, so its share is largest where the frame
  is otherwise lightest: −50% at 500-999 draws and −17.8% at 5,000-5,499. **Quote the draw
  count with it, and expect the opposite trend.**
* **`readback`'s share at 2x is much bigger than §3 measured** — 36.2% at 500-999 draws
  against the 16.4-22.6% seen at 2,900-3,700. Same reason. §3's numbers are not wrong; they
  are the value at the draw counts §3 happened to sample, which is the whole of the point
  above.

### §7. GATES AT CLOSE — ALL CLEAN, and the picture gate ran at BOTH resolutions

| gate | result |
|---|---|
| `--smoke` | OK |
| switch-lowering gate | 0 defects |
| shader dimension census | 0 disagreements |
| PM4 oracle, packet lengths (24.5 M packets) | clean |
| PM4 oracle, indirect-buffer walks (28,726 buffers) | clean |
| E3 picture gate, best of five | **+0.8399**, 4 of 5 agreeing on layout |
| `no translated shader` | **0** |
| `truncated=` | **0** |
| `PARALLEL GUARD SLOT MIX-UP` | **0** |
| deepest file on a no-input boot | **#83 `cinezombie.big`** |
| A5 kernel-call diff | **exit 0, 4 permutation windows, 0 real** |
| shader-cache NAME diff | only `ps_926c15dd20571cf1`, the known lost-microcode entry |
| shader cache | **438**, unchanged |
| **swapchain image vs E3, 1280x720** | **+0.8831**, 21 of 38 |
| **swapchain image vs E3, 2560x1440** | **+0.8741**, 16 of 32 |

The last two rows are the ones this part had to invent, and they are why §5 exists: the E3
row above them was produced by the READBACK path and says nothing about the swapchain arm,
because the arm is off in a headless gate run.

### §8. TWO CORRECTIONS THE OPERATOR FOUND IN TEN MINUTES, AND THE SECOND IS OPEN

The arm was handed over as a judging session, `SWAP=1 RES=2560x1440
tools/play_session.sh`. Their first sentence was *"Feels good but it is blurry — you are
sure it's at 2560x1440?"*

#### §8a. THE PICTURE WAS BLURRY AND IT WAS OURS — the swapchain did not follow the window

It was at 2560x1440. It was also being blitted into a **1280x720 swapchain** and stretched
back out to a 1440p monitor by the compositor. A screen grab of the live session showed the
window nearly filling the display while the log carried **exactly one** `[vk] swapchain`
line, at 1280x720.

The first version rebuilt only on `VK_ERROR_OUT_OF_DATE_KHR` and merely COUNTED
`VK_SUBOPTIMAL_KHR`. A Wayland compositor commonly reports a resize as suboptimal — or
tolerates the mismatch silently and upscales the smaller image itself — so a window
enlarged after the first present kept being presented from the original swapchain forever.

**The readback path never had this, and that is the transferable half.** SDL's
`SDL_RenderCopy` always scales the full-size texture into whatever the window currently is,
so a resize needed no code of ours at all. **Replacing a library's present meant inheriting
a job it had been doing invisibly** — and the inherited job is invisible precisely because
nothing in our code ever mentioned it.

The fix asks the SIZE rather than trusting a return code, so it does not depend on which of
three plausible compositor behaviours this one picks: the window's event loop publishes the
drawable size into two atomics on every event that can change it, and the blit rebuilds when
it differs. `CZ_WINDOW_RESIZE_AT=SECS:WxH` is the positive control, because **no headless
gate can resize a window** and without it this would have shipped on an argument (gotcha 30):

| | before | after |
|---|---|---|
| window maximised | 1088x612 | **rebuilt to 2560x1417** |
| window shrunk | 1280x720 | **rebuilt to 900x600** |

Their verdict after the fix: *"Looks a lot nicer now."*

#### §8b. THE FRAME-TIME NUMBER WAS MEASURED INTO A SMALL WINDOW — **RESOLVED IN §9, AND
#### THE WORRY WAS WRONG.** Read §9 before acting on anything below

Their second sentence is the one that is not resolved: *"still feels pretty much the same
framerate wise"*, at 69-96 fps on `CZ_FPS_LOG`.

**§6's campaign left the window at its default and measured a 1088x612 or 1280x720
drawable. They play at 2560x1417.** Those are not the same experiment, and the two arms do
not scale the same way with it:

* the **readback** path's CPU cost is a function of the INTERNAL resolution — 14.1 MB
  read back and 14.1 MB copied at 2x — and is **independent of the window size**;
* the **swapchain** arm's blit DESTINATION *is* the window, so its GPU cost grows with it,
  and at 2560x1417 it is roughly four times the destination §6 measured.

So the honest statement is that **§6's −31.4% is a number for a small window, and the
item's value at the operator's window size is UNMEASURED.** Their report is the first
evidence that it may be materially smaller there — and it is only evidence, not a
measurement: comparing their 69-96 fps against part 53's remembered 66-97 fps for the
readback path at 1440p is a cross-session comparison against numbers from another day,
which is the exact thing gotcha 51 exists to forbid.

**What settles it** is `WINSIZE=2560x1417 tools/part54_present_cost.sh`, three rounds an
arm, which now forces the same window size into both arms. It is owed, and it is the first
thing part 55 should run. **Until it has, quote §6's numbers with the window size attached**
— the same discipline part 53 imposed for the internal resolution, one variable over.

> **The general rule this adds: a present-path measurement has TWO resolutions, and naming
> only one of them is naming none.** The internal render and the window are independent
> knobs, they load the two arms differently, and no instrument in this project records
> either alongside a frame time.

### §9. THE OWED CAMPAIGN RAN, AND §8b's WORRY IS REFUTED: the saving survives at the
### operator's window

`MAXIMIZED=1 tools/part54_present_cost.sh`, 1440p internal, **both arms at a stable
2560x1417 drawable** — the operator's own configuration — three rounds an arm, alternated,
one frozen binary.

| draws | base | swapchain | delta | null (base a+b vs c) |
|---|---|---|---|---|
| 500-999 | 7.10 | 3.90 | **−45.1%** | +3.5% |
| **2,500-2,999** (n=7/9) | **10.70** | **7.60** | **−29.0%** | **+0.0%** |
| 3,000-3,499 | 11.30 | 8.40 | −25.7% | — |
| 3,500-3,999 (n=4/4) | 11.60 | 8.90 | −23.3% | −1.7% |
| 4,500-4,999 | 12.60 | 9.90 | −21.4% | — |

The swapchain arm's own null reads **+0.0%, +0.0%, +2.3%**. So **−29.0% at the operator's
window against −31.4% at a 1088x612 one**: the worry was reasonable and the answer is that
it barely matters.

#### Why it barely matters — the 2x2 that falls out of the two campaigns

The same internal resolution measured at two window sizes, per arm:

| arm | small window → maximised | reading |
|---|---|---|
| **readback** | +2.7 … +4.9% | as predicted: its CPU cost is a function of the INTERNAL resolution, not the window |
| **swapchain** | +2.6 … +8.6% | the blit destination is four times larger and it costs a few percent, not the item |

**§8b's mechanism was real and its magnitude was wrong.** The swapchain arm *does* pay for
a larger window and the readback arm *does* not, exactly as argued — the term is simply
small next to the three copies being removed. **Recording it as OPEN rather than as
"probably fine" was still right**: the argument for it was sound, and the only thing that
could separate "sound argument, small effect" from "sound argument, large effect" was the
campaign.

#### One number in here is not explained, and it is flagged rather than smoothed

The readback arm's `readback` phase costs **2.50 ms at the small window and 1.86 ms at the
maximised one** (24.5% of 10.20 against 17.4% of 10.70) — it got *cheaper* as the window
grew, while the frame got longer. The copy itself cannot have changed: it is sized by the
internal resolution. The plausible mechanism is contention — `Host_PresentPixels` and the
window thread's `SDL_UpdateTexture` share `g_frameMutex`, and a bigger window makes each
loop iteration slower, so the loop takes that mutex *less often* and the pump blocks on it
less. **That is a hypothesis and it is written down as one**; it would be settled by
timing the lock, which no instrument here does.

#### WHAT IS STILL UNEXPLAINED, AND IT IS THE OPERATOR'S REPORT

The measurement says this item is worth **21-29% of the frame at their internal resolution
and their window size**. They played it and said **"still feels pretty much the same
framerate wise"**, at 69-96 fps.

Those are not reconciled, and the honest position is to say so rather than pick whichever
one is convenient. What can be said:

* **Nothing measured contradicts them.** There is no readback-arm measurement of THEIR
  route on THEIR machine from the same evening; the numbers their report is implicitly
  compared against are part 53's, from another day, which is what gotcha 51 forbids using
  as a control.
* **Both arms are well clear of 60 fps in most of the map.** 10.70 → 7.60 ms is 93 → 132
  fps. A change that moves a frame rate around within "comfortably above the refresh rate"
  is real and is not necessarily felt, and this project has no instrument for felt.
* **The routes differ.** The campaign drives the DebugJump route with AutoChuck; they play.
* **The item's shape works against being felt**, which §6 already recorded: it is a FIXED
  per-frame cost, so its percentage is largest where the frame is lightest — precisely
  where nobody was short of frames.

**The one experiment that would settle it is a chained A/B they can feel**, both arms
maximised at 1440p with `CZ_FPS_LOG`: quit one and the next starts. Ten minutes of operator
time, and it is the only thread this part leaves open.

### §10. THE OPERATOR'S SOAK A/B RETRACTS §6 AND §9's HEADLINE: the saving COLLAPSES with
### load, and every campaign in this part measured the light end

Both arms in one session, their machine, their route, uninstrumented apart from
`CZ_FPS_LOG`, god mode / no death sequence / **zombies ignore all humans** held by the pump
in both (their idea, and a better one than the script's original reasoning — a zombie grab
moves the CAMERA, which is the one thing a soak exists to hold still). Four minutes an arm,
matched on the draw count the `[fps]` line now carries:

| draws | readback | swapchain | Δ fps | Δ ms | delta |
|---|---|---|---|---|---|
| 2,250-2,499 (n=1/2) | 90.4 fps, 11.06 ms | 114.7, 8.73 | +24.2 | **−2.33** | −21.1% |
| 6,500-6,749 (n=1/13) | 69.4, 14.41 | 70.9, 14.10 | +1.5 | −0.31 | −2.2% |
| **6,750-6,999 (n=4/6)** | **67.8, 14.75** | **70.2, 14.24** | **+2.4** | **−0.51** | **−3.5%** |

Their own report before seeing any of this: *"Feels pretty much the same but run 1 felt like
it had less stutter and framerate was 3 to 6 fps higher than arm 2."* **Measured: +1.5 to
+2.4 fps.** They were right and slightly generous.

#### What is retracted, and it is the generality and not the number

§6 and §9 are correct about what they measured and wrong about what it means:

* **In milliseconds the saving is NOT constant.** §6 and §9 said it was a fixed per-frame
  cost whose PERCENTAGE falls with load while the millisecond figure holds — measured at
  −3.20 ms at 500-999 draws through −2.70 at 4,500-4,999. Their soak reads **−2.33 ms at
  ~2,400 draws and −0.51 ms at ~6,800.** The milliseconds collapse too.
* **Every campaign in this part measured the LIGHT END.** The best-populated band in both
  the small-window and maximised campaigns was 2,500-2,999 draws; the heaviest band with
  more than one window an arm was 3,500-3,999. **The operator plays at 6,700-7,300.** The
  −29.0% headline is a number for ~2,700 draws, and at the load they actually play it is
  **−3.5%**. Their light band agrees with the campaigns (−21.1% at ~2,400), so nothing was
  mis-measured — it was over-generalised.
* **The likely mechanism, stated as a hypothesis.** At high load the GPU is busy, so CPU
  time taken off the pump is absorbed by a longer fence wait rather than converted into
  frames: `submit gpu` was already rising to 14.7% at 2x in §3. That predicts the saving
  returns wherever the frame is CPU-bound again, and it is testable with `CZ_VK_PROFILE` on
  a soak — which nobody has run, because a profiled soak costs 2-4 ms a frame and would be
  a different frame.

**The transferable half is the process failure, not the physics.** The item was priced,
built, gated, A/B'd three rounds an arm at two internal resolutions and two window sizes,
and every one of those campaigns sampled the same light part of the game. The operator's
first soak found it. That is the third time in three parts that a soak in the heaviest place
has answered a question a roaming campaign could not (part 52 §§10-12, part 53 §10, this),
and the lesson is now unambiguous: **take the A/B at the load the player is at, or the
number is a fact about the load you happened to sample.**

#### The stutter is real and separate, and it is the present MODE

Their *"less stutter"* is supported by the logs. Frame-time mean against median — a mean
above the median means slow frames exist in the tail:

| | settled soak | in transit |
|---|---|---|
| swapchain (MAILBOX) | +0.3% | **+3.3%** |
| readback (SDL, compositor-paced) | +0.0% | **+5.9%** |

At the settled soak both are smooth. Where the two differ is **while moving and loading**,
which is where the compositor's pacing of `SDL_RenderPresent` bites and where MAILBOX — the
present mode the SDL path could never choose — does not. That is a quality-of-motion win
the frame-rate number does not carry, and it is the part of this item that survives the
retraction above intact.

#### Where that leaves the item

**It ships as an arm, and the honest summary is: a large win at light load, ~3.5% at the
load the operator plays, plus a real smoothness win in transit and a picture that
correlates with hardware at both resolutions.** It also removes the readback's whole
architecture — three full-frame copies and a GPU→CPU→GPU round trip — which is worth
keeping for what it makes possible later, not only for what it returns today.

## 6cl. Part 55: THE OPERATOR ASKED FOR MULTITHREADING AND THE BIGGEST THING ON THE
## CRITICAL THREAD TURNED OUT NOT TO BE PARALLELISABLE WORK AT ALL — it was
## `std::unordered_map`

**The subject was set by the operator**, closing part 54: *"For part 55 I want us to focus
on making it so the game properly use multithread and dispose of the load properly unless
you tell me it's not possible."* And a second instruction that is a design constraint
rather than a preference: *"even if we really needed the 16 core we should still leave core
empty for user background item and all. So we should do it smart and depend on amount of
core the user has instead of aiming for my machine."*

`docs/perf-plan-part55.md` is the plan that answers both. Its §0 says the ceiling is 5-6
busy threads and not 16, because the PM4 walk is serial (a command stream's meaning is
positional) and draw ORDER is semantic. Its §0b sets the thread budget. And its item C —
filed as the cheapest thing on the list and expected to return an *instrument* rather than
a saving — is what this part is actually about.

---

### 1. THE THREAD BUDGET, because it constrains everything after it

`runtime/cpu/thread_budget.{h,cpp}`. One number for the whole runtime, and every pool asks
it for a share:

```
physical  = counted from sysfs topology, INTERSECTED with the process's affinity mask
reserved  = 2      # the OS/compositor, and the user's own software
committed = 3      # the graphics pump + the two busy guest threads, measured
budget    = clamp(physical - reserved - committed, 0, 6)
```

| machine | physical | budget |
|---|---|---|
| 4-core laptop | 4 | **0** — the serial path, which is correct and not degraded |
| 6-core | 6 | 1 |
| **the operator's** | **8** | **3** |
| 12-core and up | 12+ | 6, the cap |

Three things about it are deliberate.

**Physical cores are COUNTED, not derived.** Unique `(package, core)` pairs out of
`/sys/devices/system/cpu/*/topology`, because dividing the logical count by an assumed
threads-per-core is wrong on any heterogeneous part. The count is intersected with
`sched_getaffinity`, so a run under `taskset` or in a constrained container budgets against
what it was actually given — verified, `taskset -c 0,1,8,9` reports **2 physical cores and
a budget of 0**.

**One knob.** `CZ_WORKERS=N` overrides the whole budget and `CZ_WORKERS=0` forces the
serial path everywhere. Not one variable per pool, or the arms multiply and nobody can say
afterwards what a run was configured as. Per-pool arms that already existed still win where
set.

**It prints**, at start-up and again once the grants exist:

```
[threads] machine: 8 physical cores, 16 logical cpus -> budget 3 workers (reserve 2, committed 3, cap 6)
[threads]   guard      3 of 4 wanted (clamped)
```

because a performance number taken at an unknown thread count is not comparable with
anything. That is gotcha 353's shape a third time over — a parallel measurement has a
MACHINE as well as a workload.

**The measurable consequence, stated so it can be refuted:** the guard pool asked for 4 and
gets 3 on this machine, so part 53's measured configuration is no longer the default.
`CZ_VK_GUARD_WORKERS=4` restores it exactly, and the A/B is owed.

---

### 2. ITEM C: THE SPLIT THAT COULD NOT BE TAKEN WITH A ProfScope

The plan filed `UploadStream` as unexplored: 12.84% of the pump thread in part 54's symbol
budget (14.74% when re-taken here), and in no performance plan this project had written —
because part 22 closed the stream cache on the strength of `ProfScope(streams)` reading
0.0%, and a scope is a region of code and not a subsystem (gotcha 343).

**It could not be split the way `record` and `drawOther` were.** The hot path is taken
~33,000 times in a crowd frame and a `ProfScope` costs two clock reads at ~20 ns —
**1.3 ms a frame**, larger than several of the phases it would be separating. An instrument
that big does not measure a function, it replaces it (gotcha 7), and this project has the
worked example already: part 50 found `other`'s residual WAS the profiler's own clock
reads.

So the split was taken with `perf` and the DWARF line table the RelWithDebInfo build
already carries — `tools/part55_srcline.py`, which folds a flat profile by SOURCE LINE
inside one symbol on one thread, at zero cost to the subject, and at -O2 attributes
INLINED callees to their own lines rather than to the container. It is the same move part
51 made from phases to symbols, one level finer. **Gotcha 360.**

The answer was not something reading the code would have produced:

| share of `UploadStream` | line | what it is |
|---|---|---|
| **41.24%** | `stl_function.h:378` | `std::equal_to<uint64_t>` — the key compare |
| **36.92%** | `hashtable.h:2257/2263/0` | `_M_find_before_node` — the bucket's node chain |
| **10.49%** | `hashtable_policy.h:585` | `_Mod_range_hashing` — the prime modulo, a division |
| 1.53% | `vk_renderer.cpp:6851` | ...the first line of our own function |

**Eighty-nine percent of `UploadStream` was the hash-map lookup — 13.1% of the whole pump
thread.** Not parallel work waiting for a thread. Work that should not exist.

And the same tool, run on the other hot symbols, said it was a CLASS and not one site:

| symbol | share of the pump | of which container lookup |
|---|---|---|
| `DoDraw` | 22.06% | **17.9%** — `std::less` + `_Rb_tree::find`, i.e. `R->shaders` probed twice per draw |
| `UploadTexture` | 9.50% | **~76%** — `R->textures` plus two always-on census `std::map`s |
| `UploadStream` | 14.74% | **89%** |

Roughly **a quarter of the pump thread was container lookups**, which is larger than any
single item in part 55's plan and larger than the two parallel items combined.

**WHY `std::unordered_map` IS THIS SLOW**, stated so the reasoning can be checked rather
than trusted. It is a chained hash table: every entry is a separately `malloc`ed node, so a
lookup is a bucket-array load, a dependent chase to a node allocated at an unrelated
address, and a compare of a key living in that node — two dependent cache misses whose
latency cannot be overlapped, with the compare charged for the second, which is why
`equal_to` reads as the single hottest line. The standard then mandates prime-modulo
bucketing, so every lookup performs a 64-bit division (~20-26 cycles, unpipelined), and
`std::hash` for an integer is the IDENTITY, so nothing is mixed before it. `std::map` is
worse: a red-black tree walk of ~9 dependent pointer loads.

---

### 3. WHAT REPLACED THEM, and the two details that would have been silent defects

`FlatCache<V>` in `vk_renderer.cpp`: open addressing with linear probing, keys, generation
stamps and values in three flat arrays sized to a power of two. One masked load and a probe
sequence that walks forward through memory the prefetcher can see coming. No division, no
node allocation — `_int_malloc` was 2.11% of the pump thread and is now **0.02%**.

**The key must be MIXED.** With prime bucketing a structured key is survivable; with a
power-of-two mask the low bits ARE the bucket, and the stream key is
`(va << 32) | (bytes << 2) | endian` — its low bits are an endian code and the bottom of a
byte count. The identity hash would pile a frame's streams into a handful of buckets and
turn a probe into a linear scan. A splitmix64 finalizer costs three multiplies and fixes
it. **`[vkprof] flat stream cache` prints PROBES PER LOOKUP for exactly this reason**: a
bad hash or a load factor left too high presents as "the change did nothing", which is
indistinguishable from a wrong theory. Measured: **1.37-1.62**.

**Clearing is a GENERATION BUMP, not a memset.** The per-frame cache is cleared every
frame and zeroing a 4,096-slot table would hand back part of the saving. Each slot carries
the generation it was written in and any older generation reads as empty — correct with
linear probing because every live entry was inserted after the last bump, so its own probe
sequence also treated stale slots as free and no live key's chain can pass over one. The
tombstone flag lives in the TOP BIT of the same generation word, so a probe still reads one
array, and a tombstone from an older generation reads as empty for free.

---

### 4. THE VERIFIER, because a wrong lookup is a wrong ANSWER and not a crash

A lookup returning the wrong entry hands a draw another mesh's vertex stream, or the wrong
shader. Nothing in this runtime would report it. Same shape as part 52's memo defect and
part 53's slot mix-up, which is why those items were trustworthy.

* **`CZ_VK_NO_FLAT_CACHE=1`** restores `std::unordered_map`/`std::map` for all three tables
  — the same-binary control arm, and what this renderer used for fifty-four parts. It
  announces itself in the profile with `0 lookups/frame` rather than going silent
  (gotcha 151).
* **`CZ_VK_VERIFY_FLAT_CACHE=1`** maintains both structures and compares every lookup, for
  the two tables where a shadow copy is cheap. **0 of 25.4 M, 0 of 36.7 M, 0 of 26.5 M and
  0 of 48.5 M disagreed** across four windows.
* **`CZ_VK_VERIFY_FLAT_CACHE_POISON=1`** makes the flat side look the wrong key up, so the
  check MUST fire: **81.7%** of lookups disagree, and the shader half has a *visible*
  consequence rather than only a counter — the run starts printing `no translated shader`
  for hashes that are in the cache. A verifier that has never failed has not been shown
  capable of failing (gotcha 30).

**The cross-frame store is the exception and the reason is worth recording.** Its entries
are MUTATED through the pointer a lookup returns — the guard, the ping-pong slot, the
promotion counters, the pre-hash slot — so a shadow copy would mean mirroring every
mutation: more new code than the change itself, and a defect in the mirror would present
exactly like a defect in the subject. So there the arm switches which container four
accessors use, and the `FlatCache` implementation is verified where a shadow IS cheap,
running the same probe, insert and grow code.

---

### 5. WHAT IT IS WORTH — the frame, measured

`tools/part55_item_campaign.sh 3 nofl=CZ_VK_NO_FLAT_CACHE=1`, one frozen binary, three
rounds an arm, alternated `base / nofl / null`, the shipped 500 fps cap in every arm, the
DebugJump outdoor route. **This campaign measured the first three tables** (the per-frame
stream cache, the shader table and the cross-frame store's index); the texture cache and
its two censuses landed afterwards and are measured by the second campaign in §7.

The cleanest statistic the tool prints, because it needs no binning at all — **the per-run
mean of every frame with ≥ 6,000 draws**:

| arm | run 1 | run 2 | run 3 |
|---|---|---|---|
| **base** (flat) | — | **12.18 ms** | **12.26** |
| **null** (base again) | **12.09** | **12.26** | **12.01** |
| **nofl** (`std::map`/`std::unordered_map`) | **13.06** | **14.19** | **14.19** |

**Five runs of the shipped configuration span 12.01-12.26 ms and three runs of the control
arm span 13.06-14.19. They do not overlap.** Pooled, 12.16 against 13.81 — **−1.65 ms,
−12.0%**. (`base_1` reached only 3,150 draws, so it contributes no frames to this row; its
absence is why the table has a gap rather than a number.)

Binned by draw count, with the campaign's own null beside it:

| draws | base | nofl | Δ mean | Δ median | NULL Δ mean |
|---|---|---|---|---|---|
| 3,000-3,999 | 8.51 ms | 9.22 | **+8.4%** | +12.5% | −0.4% |
| 4,000-4,999 | 9.74 | 10.02 | +2.8% | +11.1% | +0.6% |
| 5,000-5,999 | 10.92 | 11.26 | +3.2% | +0.0% | −0.2% |
| **6,000-6,999** | **12.16** | **12.84** | **+5.6%** | **+8.3%** | **−1.5%** |
| 7,000-7,999 | 13.07 | 14.63 | **+11.9%** | +7.7% | −2.7% |
| 8,000-8,999 | 13.99 | 15.64 | **+11.8%** | +7.1% | +0.5% |

**The 2,000-2,999 bin is not usable in this campaign and it is worth saying why rather than
quietly dropping it.** `base_1` never left a light area, so it put 48,091 frames into that
one bin — 38% of the whole arm — and the null reads **+8.1%** there against +0.0…−1.5%
everywhere else. A bin dominated by one run of one arm is a measurement of that run.
Gotcha 331's rule earns its place again: the arm that cannot move the statistic is what
tells you which rows to read.

**AND THE SHAPE IS THE ONE PARTS 52 AND 53 HAD, NOT PART 54'S.** The saving grows with the
draw count, because these lookups run per stream and per draw. Part 54's swapchain item was
a FIXED cost per frame and its percentage collapsed with load; this one does not. At the
operator's 6,700-7,300 draws the campaign reads **+5.6% to +11.9%** and the trend is
upward, which is the first item since part 53 that should be worth *more* on their machine
than on this route — but that is a prediction, and gotcha 355 says the only thing that
settles it is a soak at their load.

---

### 6. WHAT THE PUMP THREAD LOOKS LIKE NOW

Same route, instruments off, all five tables flat:

| symbol | part 54 | part 55 open | now |
|---|---|---|---|
| the NVIDIA driver, unsymbolised | 15.13% | 16.00% | **20.57%** |
| `DoDraw` | 24.43% | 22.36% | **19.88%** |
| `UploadTexture` | 8.69% | 8.87% | **11.87%** |
| `UploadStream` | 12.84% | 14.98% | **11.68%** |
| `ExecutePacket` | 5.77% | 5.44% | 6.06% |
| `WriteRegisterRun` | 9.10% | 6.51% | 5.78% |
| `_int_malloc` | 2.00% | 2.11% | **absent from the top sixteen** |

**Read these as shares of a thread doing less total work**, so a symbol that did not change
absolutely reads HIGHER here — the driver going 16.00% → 20.57% is not the driver getting
slower, it is everything around it getting cheaper. The absolute reduction is the frame
time in §5, and this table is for choosing what to do next.

Re-split by source line, the three converted symbols have no container machinery left in
them at all. `UploadStream`'s remaining cost is the flat probe's own cache miss
(`vk_renderer.cpp:2045`, 40.1% of the symbol) and the guard — which is what an irreducible
lookup looks like. `DoDraw`'s two hottest lines are now `vk_renderer.cpp:8330` and `:8333`
at **18.90% and 18.89%**, and they are not a container: they are **the ALU constant copy**,
2,048 dwords — 8 KB — memcpy'd into mapped memory per draw. That is **7.5% of the pump
thread** and it is the next item in this function.

`UploadTexture` was still ~72% container at that point, which is what the fourth commit
went after.

---

### 7. THE OPERATOR'S SOAK A/Bs — and a NEGATIVE result worth more than the positive one

Two chained sessions on their machine (`tools/part55_chained_ab.sh`), both arms in one
sitting, uninstrumented but for `CZ_FPS_LOG`, standing still in the heaviest place they
know. Their own framing, and it is why the roaming campaign was abandoned mid-run: *"I'll
do your campaign with soak at the spot that hit the cpu the most so we just get 2x
3minutes soak instead of hours of testing in unstable environment with autochuck."*

#### The container item, confirmed

| session | arm | draws | frame | fps |
|---|---|---|---|---|
| 1 | flat | 6,701-6,971 | 10.94 ms | 91.4 |
| 1 | maps | 7,161-7,492 | 13.20 | 75.8 |
| 2 | maps | 6,725-6,910 | 12.50 | 80.0 |
| 2 | flat | 7,004-7,253 | 11.14 | 89.8 |

**The two soaks in a session are never at the same draw count**, which is the thing to
watch: session 1's maps arm carried 7% MORE work than its flat arm and session 2's flat arm
carried 4% more than its maps arm, so the raw fps gap flatters the item once and understates
it once. Draw-matched with each arm's own within-arm slope, session 2 reads **11.18 -> 12.80
ms, −13.2%** — and the roaming campaign said −12.0% and the cross-session pair −12.5%. Three
independent measurements within a point of each other.

#### The stutter report, and how it was resolved

Their first session: arm 1 (flat) *"is stuttering a lot but since I stopped moving for the
soak no stutter and around 20fps higher"*; arm 2 (maps) *"way less stutter when moving"*.
The logs agreed — the flat arm lost ~7 seconds inside one 10-second transit window (270
frames where the maps arm never dropped below 737).

Three things were done and the order matters. **The tables were counted, not argued about**:
grow accounting says 20 grows, 31.41 ms total, worst 15.04 ms over a whole run — three
hitches a player could see, and nothing like seven seconds. **The headless campaigns were
asked and disagreed with the report**: 18 frames over 50 ms in the flat arm against 39 in
the map arm across three runs each. **And the order was reversed**, which is the control the
first session could not have: on a re-test with `maps` first, the worst mean-over-median
window in every one of the four logs except the original is 1.45-1.47x, and that peak is a
2 ms menu frame. Their verdict on the re-test: *"arm 2 has same amount of stutter as arm 1
but framerate is higher"*.

So the tables were **a** stutter source and not **the** one, and pre-sizing them to the
measured high-water mark takes the run's grow bill to 1 grow / 0.06 ms. Two things changed
between the sessions — the pre-size and a now-warm page cache — so the credit cannot be
awarded cleanly, and that is recorded rather than resolved in the fix's favour.

#### THE NEGATIVE RESULT: geometry in VRAM is ~14% SLOWER, and the reason generalises

Their question: *"shouldn't we load texture, frame buffers, geometry/meshes, shadow maps,
lighting and refraction data, shaders and cached assets from vram. Especially since it's a
old game shouldn't take much vram and then we use ram as fallback."*

Most of that list already was in VRAM — every `VkImage` allocates `DEVICE_LOCAL`. Geometry
was not: the 512 MB cross-frame store and the per-frame arena asked for
`HOST_VISIBLE | HOST_COHERENT`, which resolves to the first matching type, system RAM. Their
RTX 3070 exposes `memoryTypes[5]` — `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` over all
8 GiB, i.e. Resizable BAR — so the switch is one line.

It was built as an ARM (`CZ_VK_VRAM_STREAMS=1`) rather than a default, with the new arm run
FIRST so any first-run penalty would land on it. It lost:

| arm | draws | frame | fps |
|---|---|---|---|
| **vram** | 6,726-7,042 | 11.80-12.34 ms | 81-85 |
| **ram** (shipped) | 7,194-7,400 | 10.96-11.28 | 89-91 |

RAM is faster while carrying ~400 MORE draws. Draw-matched at ~7,300: **12.80 vs 11.18 ms,
about 14% slower in VRAM.**

**AND THE MECHANISM IS THE PART THAT TRANSFERS.** The pre-registered prediction was
`SynthRectStream` — which still reads three vertices per rect draw out of the buffer, and in
VRAM those become uncached PCIe reads. It is probably a contributor and it is not the main
term. The arithmetic is on the WRITE side: **the ALU constant copy writes 8 KB per draw**,
which at 7,000 draws and 90 fps is ~57 MB/frame, over **5 GB/s**, plus ~28 MB/frame of
first-touch geometry. In system RAM those are cached writes at 30+ GB/s; in VRAM they are
write-combined transfers across PCIe at roughly a third of that. The GPU saves one fetch;
the CPU pays several times more to put the data there.

**A recompiler is not a normal engine.** A game engine uploads a mesh once and draws it for
a hundred frames, so the GPU's fetch dominates and VRAM wins. A recompiled title re-uploads
its shader constants every draw, because the guest writes its register file every draw. That
is gotcha 363, and it comes with two corollaries: the calculus FLIPS if the per-draw upload
is removed, so re-ask after any change that stops re-uploading constants; and CPU-visible
device memory is write-combined and therefore **write-only**, so every read of such a buffer
must be audited before switching one over.

The arm stays off. `SynthRectStream` keeps its fix — it assembles the quad in local memory
and writes once, which is strictly better in both arms — and the rule is written where the
next person will edit it.
