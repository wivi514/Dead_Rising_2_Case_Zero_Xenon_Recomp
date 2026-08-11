# D3D phase C, part 28 hand-off (for part 29). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c27-kickoff.md`, which
was written HALFWAY through part 27 and is out of date on almost everything below.
**Check the git log against this file before working an item** — gotcha 13, and it has
cost this project a session three times.

## The one-paragraph state of the port

The game boots, renders and plays at the title's own ~31 fps pacing floor. Part 27 spent
itself almost entirely on **one defect — the flat white patches on world surfaces** — and
took it from "seven refuted hypotheses and no mechanism" to a value, a population and a
named instruction. It is not fixed. What it now has is a chain of measurements that a next
session can extend in one run rather than re-derive.

## READ THIS BEFORE MEASURING ANYTHING

Everything from part 26's list still stands (null arm first; no matched-frame picture
comparison outdoors; the frame time is pinned at two vblanks; profile phases are
exclusive; don't pin the GPU clock; three runs an arm on crowd timing; validation on once
a session; serialise background runs). Part 27 adds four:

* **A REPLAY THAT IGNORES A PACKET REPORTS ABSENCE** (gotcha 262). Census the opcodes
  before trusting any `.xtr` tool. All three of ours were dropping `LOAD_ALU_CONSTANT`,
  which this title uses 620 times to `SET_CONSTANT`'s 36.
* **AN UNRECONSTRUCTIBLE VALUE MUST SAY SO** (263). 81 of those 620 loads read memory the
  capture never recorded. What exposed it was an IMPOSSIBLE value, not a suspicious one —
  ask of every oracle reading *could the guest have run with this?*
* **A FILTER THAT SELECTS ON THE PROPERTY UNDER TEST CANNOT FIND A VIOLATION** (264). This
  bit three times in one day: the 414-of-414 cube census, the binding diff's slot lists,
  and the first read of the floor paint. Write down what the disagreeing case would look
  like and check your filter admits it.
* **A LAST-WRITE PROBE AND AN ACCUMULATING FLAG ARE NOT INTERCHANGEABLE.** The translated
  shaders are a `switch` over exec blocks inside a loop, so a block can run more than once
  per pixel. `f = f || ...` is safe; `x = <value>` records whichever iteration happened to
  be last. This is what made two of part 27's own measurements contradict each other.

## The white-patch chain as it stands, in order of certainty

Each line was measured, and each has its own control. `docs/open-items.md` 00f/00g.

1. **The patches are exactly `rgb(180,180,180)` in the scene buffer**, at all seven
   captured locations, 1.81%-15.36% of the frame, and in five of seven the whole 1280x720
   buffer never exceeds 180. The next most common colour above luma 150 has TWO pixels.
2. **They are not modulated by scene lighting.** With `DISABLE TIME OF DAY` the world is at
   night — 90% of the slot-machine frame below luma 40 — and the cabinets are still 255.
3. **The tone map is the amplifier, not the cause.** 96.1% of the presented white was
   already >= 150 in the scene buffer, mean 173, max exactly 180.
4. **The material pixel shaders write it themselves** — `XE_VALUE_PAINT`, 3,242 plateau
   pixels to 0 with 3,982 painted, positive control 99.73%. Resolve and EDRAM eliminated.
5. **48 shaders emit it**, named by `XE_SHADER_TAG` over the operator's 59 captures.
   `ps_7d2f8f33deec1b65` alone is 703,376 px, 47%.
6. **They share ONE epilogue**, differing only in which constant slots the compiler
   allocated: `sqrt(abs((max(c', K1) - r1*r1) * K2))`.
7. **180 is that operator's KNEE**, `sqrt(K1*K2) = sqrt(0.5)`, and the constants keep the
   curve continuous there — so the knee value is the same for ANY exposure. That is why
   one number appears across materials, locations and times of day.
8. **No max takes its floor on those pixels** — `XE_FLOOR_PAINT`, 0 magenta / 1,157 green
   with only `ps_7d2f8f33deec1b65` instrumented. Therefore `c' >= K1`, therefore
   **`c = 1/pc(14).w`**: the colour arriving at the epilogue is pinned at the reciprocal of
   the exposure constant.

**Refuted along the way, each by measurement, none to be re-bought:** the cube dummy (the
part-27 fix engaged and the surfaces stayed white), a NaN (positive control at 99.85%,
zero painted), XenosRecomp's `rcp` clamp (`FLT_MIN` is -FLT_MAX, and +3.4e38 gives 255 not
180), and an emitter defect in the epilogue (the HLSL is one-to-one with the microcode, so
**nothing for Fable 2 to inherit**).

## LATE IN PART 27: THE ASSET LAYER, AND THE GRADING CHAIN END TO END

Written after the section above, from an operator question — *did we extract all the
`.big` files, and is that why things are white?* The answer is no, and chasing it properly
produced four things worth keeping.

**1. The extraction is complete and the file layer is clean.** 256 of 256, zero missing,
zero wrong size, 146 of 146 archives, zero case mismatches. The 304 not-founds a run
produces are the title probing for Dead Rising 2 content this cut-down package never
shipped — and Xenia launches the SAME package, so a file absent here is absent there.

**2. Not-founds are now CLASSIFIED**, in `runtime/kernel/file_imports.cpp`, because 304
expected misses hid any real one: PROBE (parent empty or absent) 300, SIBLING (parent
holds files — where a missed extraction would land) 4, REGRESSED (opened OK earlier in
this run) 0. SIBLING and REGRESSED print immediately, not at exit — most runs here die to
`timeout`.

**3. `tools/big_list.py` + `tools/big_decompress.cpp` read the archives**, and corrected
`docs/big-archive-format.md` twice: the name table is NOT fixed-width outside the shader
banks (95 of 146 archives failed that check), and **1,671 of 12,481 entries are
COMPRESSED** — `size` is stored, `size2` uncompressed, the stream is BE-framed chunked LZX
inside a LE container. Both retractions are the same shape as the 40-byte-stride error the
doc already recorded: **a structural constant derived from one family of files is a
property of that family until a second family says otherwise.**

**4. The colour-grading chain works, and item 6 has a mechanism for the first time.** The
15 grades ship LZX-compressed as 32-cubed LUTs unrolled into 1024x32 tiled strips (about
five distinct looks — `cc_01 == cc_04`, `cc_07..cc_13` are one). The runtime binds one to
`ps_114c4965eaabd54c`, one draw a frame, and the content is an **exact byte match** to a
disc LUT (mean |delta| 0.00 against a field 16 to 110 apart). The index **tracks the
clock**: frozen gives `cc_03`, running gives `cc_01`.
**What is NOT established** is whether that is the index hardware picks — the capture
cannot say, because hardware's LUT address is a resolve destination there too and the
bytes the trace carries are monotone under no layout, i.e. not a LUT. And **we snap to one
grade exactly while the title keeps THREE LUT surfaces**, which is the shape of a
cross-fade; if hardware blends and we snap, colour is right at each band's endpoints and
wrong between — which is what "colour is flat" describes. No evidence either way yet.

## Where part 28 starts

0. **WHAT PINS `c` AT `1/pc(14).w`.** One value, one shader, one instruction to find. The
   probe must ACCUMULATE (min/max across iterations), not record the last write — that
   mistake is why part 27 ended with two disagreeing numbers. `c` is the output of a fog
   LERP, `c = (r2 - pc19) * r0.x + pc19`, so the candidates are `r2` (the lit colour) and
   `r0.x` (the fog factor, which read 0.90 on the unreliable probe — plausible, not
   established).
0b. **ALSO STILL OPEN AND CHEAPER: why our slot 4 holds a copy of slot 3.** Hardware binds
   a real 128x128 DXT1 cube map there for the same shader pairs. Both candidates are
   GUEST-side: the env map was never created in our runtime, or the engine's own bind was
   skipped. `01330000` (4x4, "uploaded BLACK, guest memory NON-ZERO NOW") is probably the
   same defect, so `CZ_VK_TEX_REFRESH=01330000` is one arm of it.
1. **SOUND.** Unchanged and untouched by part 27: the guest hands us real buffers full of
   zeros, so the next step is XMA DECODE, not an output device. Fable 2's `audio/xma_hw.cpp`
   and `audio/xma_decoder.cpp` are directly liftable. `docs/open-items.md` 00e.
2. **The two remaining validation defects**, and measure before changing either.
3. **Re-test the parked picture defects** — shadow cascade, mipmaps, colour grading — with
   `tools/frame_era_medians.py` and a measured null. **And note the binding diff found a
   new one**: `vs_fa161b0fde7aa4d5` consistently binds SMALLER textures than hardware
   (256x256 where hardware has 512x512), which may be the mipmap item from a new angle.
4. **The binned frame-time A/B still owed for `CZ_VK_FRAMES_IN_FLIGHT=2`** (part 23).
5. The rest of `docs/open-items.md`.

## What part 27 built, and what it is good for beyond this defect

* **`CZ_CAPTURE_KEY=<dir>`** — one F9, one frame, the picture + the per-draw census + every
  resolve snapshot, all named by frame. The census carries `dim`/`depth` so its columns
  match `tools/xtr_draw_bindings.py`'s.
* **`CZ_DEBUG_FLAGS="..."`** — the title's own gameplay debug bools by menu label, held on
  by the pump. `ZOMBIES IGNORE ALL HUMANS` and `CHUCK GOD MODE` make an operator session
  a comparison instead of a fight; **`DISABLE TIME OF DAY` removes a lighting confound
  that standing in the same spot does not**, and it is what made the plateau visible as a
  constant. The title clears it — 9 re-asserts in one run — so the pump is load-bearing.
* **`CZ_DXC_DEFINES` + `CZ_SHADER_SPV`** — a shader change as a same-binary A/B, built into
  its own cache directory. The default cache rebuilds 410 of 410 byte for byte, which is
  the gate on every emitter change here.
* **`XE_NAN_PAINT` / `XE_VALUE_PAINT` / `XE_SHADER_TAG` / `XE_FLOOR_PAINT`** in XenosRecomp,
  all documented in `docs/xenonrecomp-upstream-bugs.md`, each with a positive control.
* **`tools/xtr_cube_agreement.py`**, **`tools/xtr_draw_vertices.py`**,
  **`tools/draw_binding_diff.py`**, **`tools/shader_tag_decode.py`**.
* **A cache gap closed**: `ps_7d6044e7dcaea1f2` was in the ucode dumps and never built.
  The count could not show it (410 dumps, 410 modules, different sets); the gate is to
  rebuild and diff the NAMES, and it is in `CLAUDE.md`.

## Gates, on the part-27 binary

`--smoke` OK. `tools/shader_dim_census.py` exit 0 across all **411**. `no translated
shader` = 0 on every run of the day.

**Not re-run and owed before any claim resting on them**: the A5 kernel-call diff,
`truncated=0`, the two PM4 capture oracles, the capture-E picture correlation, and the
Vulkan validation tally. The runtime changes this part are a diagnostic, two counters and
one behaviour change (the cube replicate), so the PM4 oracles are almost certainly clean —
but that is an argument, not a gate.

## The method notes worth carrying

* **Populations, three times in one day.** Every one of part 27's false results came from
  comparing two sets built by different membership rules. It is worth one sentence of
  pre-registration every single time.
* **An oracle needs its own gates.** All of this project's discipline points at the
  runtime; the `.xtr` tools had none and had been silently reporting hardware's shader
  constants as zeros.
* **Impossible beats suspicious.** A stale value that lands on a plausible number is
  invisible. `c255.w = 0` where the shader uses it as its literal 1.0 is what unravelled it.
* **Build the positive control before reading the result.** `XE_NAN_PAINT` returned zero
  magenta, which is only a finding because the forced arm returned 99.85%.
* **The operator's route reaches defects the headless one does not** — the cube
  disagreement is 2.06% of cube fetches on their route and 0.05% on mine, a 40x difference
  on the same binary. When a share matters, ask whose population it is a share of.
