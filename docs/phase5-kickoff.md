# Phase 5 kickoff — the renderer. Paste this into a fresh conversation.

---

> ## STATUS, 2026-08-05: THE RENDERER IS BUILT AND DRAWS THE SCENE. DO NOT RESTART IT.
>
> **`docs/phase5-notes.md` is the record and supersedes the "What is ALREADY DONE" list
> below.** Read it first; then come back here for the method sections (the gate, the
> traps, the standing constraints), which are all still current.
>
> **What exists:** the shader pipeline (336 of 336 translate, zero failures, no
> recompiler change), `runtime/gpu/vk_renderer.{h,cpp}` + `xenos.h` behind `CZ_VKDRAW=1`,
> a draw seam in `pm4.cpp`, and eighteen instruments listed in `phase5-notes.md` §9.
> The shader cache is gitignored — `CLAUDE.md`'s Commands section rebuilds it in three
> lines.
>
> **Where the picture is:** the Still Creek scene renders in its own colours, and the
> whole post-processing chain (the 640x360 -> 32x1 pyramid, the 64x64 luminance chain,
> the colour-grading LUT) is alive. A class of triangles is still wrong. The frame we
> present is the logo era and still lacks the logo.
>
> ### THE METRIC EXISTS NOW — USE IT, DO NOT A/B WITHOUT IT
>
> ```
> for a in base arm; do
>   (cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_VK_FRAME_STATS_SURFACE=06BE4000 \
>       CZ_VK_FRAME_STATS=/tmp/$a.txt timeout 85 ./cz_runtime >/dev/null 2>&1)
> done
> python3 tools/frame_compare.py /tmp/base.txt /tmp/arm.txt
> ```
>
> Median surface coverage over the era. Baseline band **1.36 pp** over five runs of one
> binary; the tool calls anything inside 1.5 pp "no detectable difference", and it has
> been shown capable of failing — `CZ_VK_PRIM_RESTART=1` reads 17 pp outside it.
>
> **Two of this phase's three "measured improvement" claims were retracted** once it
> existed: both were single runs of the coverage number at a fixed frame, and the title
> screen's 3D background is animated, so that number spreads 58.8–100.0% on ONE binary.
> Do not claim a renderer change helped without this tool. `phase5-notes.md` §6m records
> the two metric designs that failed first, including one that reported 257 of 257
> frames bit-identical while comparing 257 copies of a black image.
>
> **Now** chase the remaining geometry defect, with every change measured. What is already ELIMINATED, each with
> a measurement in `phase5-notes.md`: the fetch-slot convention (§6i, by an inverting
> arm), the vertex- and pixel-shader constant windows (§6c, §6g), the index endian
> decode (§6c), the colour mask (§6), culling (§6c — the title does not cull),
> `VGT_INDX_OFFSET` (0), the `sges` "set w = 1" idiom (§6l, by a hand-patched shader
> cache), the zero-attribute shader that draws 100k degenerate points, and primitive
> restart (`VGT_MAX_VTX_INDX` is 65535, so 0xFFFF is a legal index — restart must stay
> off).
>
> Known gaps with a location, from §7: a depth-only pass resolves the colour target;
> the 4096x1024 shadow cascades are clipped by a 1280x720 EDRAM image; one global
> sampler; no mip levels; one EDRAM format; rectangle lists reuse three corners.
>
> All gates hold with the renderer ON — re-run them, do not assume them (gotcha 86), and
> use an empty save root (gotcha 106).

---

## The task

Turn the PM4 stream our command processor already parses into pixels: translate the
Xenos shader microcode with XenosRecomp, build a Vulkan renderer behind the present
seam that already exists, and gate it against the GPU captures.

## Read before writing code

1. **`docs/xenia-capture-analysis.md`** — the numbered findings ledger, and the
   authority wherever any other doc disagrees. Finding 10 (determinism) is this
   phase's methodology.
2. `docs/xtr-decoder.md` — the GPU capture format and the determinism method.
3. `docs/phase1-notes.md` findings 38-39 — why the command processor is trustworthy
   now, and the two capture-derived oracles that prove its arithmetic.
4. `docs/phase3-notes.md` — the window and the present seam this phase draws into.
5. `Xenia logs/Xenia_Run_Content.md` — what each capture is. For this phase: **B1/B1b**
   (boot→title GPU stream + its determinism repeat), **B2** (7.95 GiB of gameplay), and
   **E** (five screenshots, the visual target).

## What is ALREADY DONE — do not rewrite these

This is the important half of the document. Phase 3's kickoff had the same section and
it was the thing that saved the most time.

**1. The command processor is live and verified, and it is phase 4's work done early.**
`runtime/gpu/pm4.cpp` parses the real stream: 21 distinct type-3 opcodes, all named,
**zero unknown opcodes**, ~6.5 M draws and ~97 M packets in a 100 s run, and
`ring: indirect buffers truncated=0`. Two capture-derived oracles prove its arithmetic
against hardware's own packet boundaries (`tools/pm4_packet_lengths.py` over 24,527,474
packets; `tools/pm4_indirect_walks.py` over all 28,726 indirect buffers). **Both must
keep passing.**

**2. There is a register file already, and the state you need is landing in it.**
`g_regs[0x8000]` in `pm4.cpp`, fed by `SET_CONSTANT` (0x2D), `SET_CONSTANT2` (0x55/0x56)
and `LOAD_ALU_CONSTANT` (0x2F), with the bank mapping already decoded — ALU constants at
0x4000, **fetch constants at 0x4800**, booleans 0x4900, loops 0x4908, registers 0x2000.
Draw packets (0x22 `DRAW_INDX`, 0x36 `DRAW_INDX_2`) are parsed and counted; what they do
not yet do is draw.

**3. The front buffer's fetch constant is already in the register file.** `VdSwap`
(`runtime/gpu/vd.cpp`) copies the guest's own six-dword texture fetch constant through as
a type-0 write to register 0x4800 — verbatim, because those dwords encode the front
buffer's address, tiling and format and re-deriving them would be asserting a surface
layout nobody has measured.

**4. The present seam exists and is wired to the guest's frame clock.**
`pm4.cpp` case `0x64` (`XE_SWAP`) decodes `'SWAP'`, front buffer, width, height out of
the packet body and calls `Host_Present()`. The window presents on that signal and on
nothing else. **The renderer replaces the `SDL_RenderClear` in
`runtime/host/window.cpp`; it does not need a new seam.** The window currently sizes
itself from the guest's own stated dimensions (1280x720) on the first present.

**5. The inputs are in hand and the tool is built.**
- **455 raw Xenos microcode blob FILES** — corrected in phase 5 to **335 distinct
  shaders**, because A1's 120 are a strict subset of A2's 335.
  `Xenia logs/A1_boot_title_fullgame/shaders/*.ucode.bin.{vert,frag}` and
  `Xenia logs/A2_gameplay_stillcreek/shaders/*`. Each has Xenia's disassembly and its
  own D3D12 translation alongside, which are reference material, not input — though the
  disassembly earned its keep by exposing a fetch-slot display convention
  (`docs/phase5-notes.md` §4).
- XenosRecomp is built at `~/GithubRepo/XenosRecomp/build/XenosRecomp/XenosRecomp` and
  carries local patches Case Zero inherits for free.
- **The disc shader banks are a dead end** — `data/shaders/*.big` are `.vo` shader-object
  containers with build metadata, not microcode (finding 6, retracted in place). Do not
  spend a session rediscovering this.

**6. The GPU capture decoder exists.** `tools/xtr.py` is the format; `xtr_walk.py`,
`xtr_pm4_census.py` and `xtr_determinism.py` are thin CLIs over it. `--verify` on the
census is the only check that can fail — always pass it.

## The gate, and the one way to get it wrong

**Gate on per-era aggregates. NEVER on absolute frame index.**

This is measured, not cautious: two *hardware* runs of the same drive (B1 vs B1b) agree
frame-exactly only **80.0%** of the time, with phase drift concentrated at lag +3. A
frame-indexed GPU gate would report ~20% divergence against a *correct* renderer
(finding 10, gotcha 38).

The noise floor over the correct comparison window is **0.42% worst aggregate, 0.19% on
draws**. Anything inside that is agreement.

Two further method traps from the same finding, both of which cost a session before:

- **Align over the fixed boot+movie prefix and ignore the idle tail.** Comparing whole
  runs produced "16% of frames agree — NOT content-deterministic", a confident claim
  about the game that was purely an artifact of including 619-vs-409 frames of a human
  deciding when to quit. Same data, correct window: 0.42% (gotcha 36).
- **Do not fold `MemoryRead` counts into a content fingerprint.** They agree to 0.37% in
  total and align on only 17.7% of frames, because Xenia's dirty-tracking decides *when*
  to record, not *whether*. Including them dropped frame agreement from 42.7% to 16.0%
  (gotcha 37).

**The E screenshots are the human visual target, not a numeric gate.** The plan text
(`docs/runtime-plan.md` §"Phase 5") says "per-pixel diff against the E-series screenshots
at the same frame" — **that is the one line of the plan to correct**: there is no
frame-exact alignment to diff at. Use E1-E5 as the eyes-on check of whether the picture
is right, and the `.xtr` aggregates as the falsifiable gate.

Secondary gates, all of which must still hold:
```
./runtime/build/cz_runtime --smoke                            # the phase 0.2 link gate
kernel_call_diff --xenia A5 --include-high-frequency          # exit 0, all permutations
kernel_call_diff --xenia A1                                   # 92-deep prefix of 93
ring: indirect buffers truncated=0
python3 tools/pm4_packet_lengths.py  "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/pm4_indirect_walks.py  "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
```
Run the A1 gate with an **empty save root** (gotcha 106) and with `CZ_FAKE_START_MS`
**unset** unless you are deliberately driving past the title screen.

## Where the boot is when you start

- Reaches the **title screen** and renders it: ~1,982 draws/frame against A1's
  title-screen ~2,540, ~31 fps, one `XE_SWAP` per frame.
- A blank window is up, presenting at the guest's swap rate. **That is phase 3's correct
  result, not a bug** — say so in the write-up or the next reader hunts a renderer bug
  that does not exist.
- A real press (or `CZ_FAKE_START_MS`, the arm) advances it through the storage-device
  selector and the save enumeration: A1 gate `PREFIX MATCH: our 92 calls are an exact
  prefix of Xenia's 93`.
- **A1 is exhausted as an oracle.** Its position 93 is not the next piece of work
  (finding 49). The kernel gates stay as regression checks; the GPU captures are this
  phase's oracle.
- Stability: 0 crashes across this session's runs; host CPU ~121%.
- 161 of 244 imports real, 83 generated honest-failure stubs.

## Traps this phase will walk into

1. **`INDIRECT_BUFFER` is recorded one dword short in the `.xtr`** — the size lives in
   the following `IndirectBufferStart` record. A replay tool that trusts either length
   feeds the command processor a malformed packet (gotcha 39). `tools/xtr.py` already
   handles this; anything new that reads the format must too.
2. **B1 uses only 225 distinct packet headers across 24.5 M packets.** That tight
   vocabulary is an extremely strong classifier and it is what located finding 39's
   desync after six truncation reports had each named an innocent dword (gotcha 89). Any
   format with a capture has one — build it before theorising.
3. **`EVENT_WRITE_ZPD` (0x5B) does not appear at all.** Asura's Wrath needed a whole
   synthetic-occlusion mechanism for it; Case Zero issues no occlusion queries in the
   captured eras, so that machinery is deliberately absent. If a `0x5B` ever shows up in
   our own stream, the unknown-opcode census is what will say so.
4. **Xenia's physical addresses carry a +0x1000 skew** (finding 24), so a physical
   address in our log is 0x1000 below the same object's in a capture. Any geometry
   argument mixing the two conventions is wrong — it manufactured a ring-buffer overrun
   that did not exist once already.
5. **An oracle for your arithmetic does not clear your inputs** (gotcha 88). Both parser
   gates passed while the command processor desynced dozens of times a minute, because
   the *bytes* were wrong, not the maths. When every check of a computation passes and
   the result is wrong, stop checking the computation.
6. **Textures are tiled.** A1's own log names the layouts it loaded, e.g. `Loaded tiled
   1024x32x1 2D k_8_8_8_8 texture with 1 unpacked mip level, base at 0x180ED000 (pitch
   1024, size 0x00020000)` — free ground truth for the untiler, and a good source of
   test cases.

## Standing constraints

- Commit proactively; end messages with
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- No copyright/license headers in new files (ask first).
- Document in `docs/` for an outside reader — **Dead Rising 2: Case West is the next
  port and will lift these documents.** Say what the idiom was, not just what changed.
- Retract in place when a finding turns out to be an artifact.
- **UnleashedRecomp is GPLv3 → structural reference only.** Guest structs come from
  XenonRecomp's `XenonUtils/xbox.h` (MIT). This matters more in this phase than any
  other, because UnleashedRecomp has a working Xenos renderer and it is the obvious
  place to look. Look at *structure*; do not copy code.
- Captures run on the operator's Windows machine and are **never self-servable**. There
  is currently no outstanding capture request, and B1/B1b/B2/E should cover this phase.
- Measurement discipline: same-binary A/B arms, a rate rather than a single run against
  anything intermittent, and the control for "did my change do this" is the old binary
  rebuilt and run **now** (gotchas 50, 51, 86, 95).
- Rebuild: `cmake --build runtime/build -j$(nproc)`; `python3 tools/gen_import_stubs.py`
  after any change to the import set. If the *function list* ever changes, the five-tool
  pipeline order in `CLAUDE.md` applies in full.
