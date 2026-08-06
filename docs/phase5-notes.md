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

## 7. What is NOT right yet, with the measurement for each

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
