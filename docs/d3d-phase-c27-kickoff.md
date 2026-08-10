# D3D phase C, part 27 hand-off (for part 28). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `d3d-phase-c26-kickoff.md`.
**Check the git log against this file before working an item** — that is gotcha 13, and
it has cost this project a session three times now.

## The one-paragraph state of the port

The game boots, renders and plays. Ordinary gameplay is ~30-31 fps and that is the title's
own two-vblank pacing floor. The CPU and GPU overlap (part 23), the HUD defect is fixed
(24), cube maps are bound (25) and the one the title renders itself is assembled from its
six resolve snapshots (26). **Part 27 closed the last uncompared INPUT to the white ground
and the answer is that every one of them matches hardware, so that defect is in the
SHADING** — and it found that this project's `.xtr` replay tools had been dropping the
packet this title uses for almost all of its shader constants.

## READ THIS BEFORE MEASURING ANYTHING

Unchanged from part 26 and still the whole of it:

* **RUN THE NULL ARM FIRST, IN THE SAME SERIAL BLOCK, AND QUOTE EVERY EFFECT AS A MULTIPLE
  OF IT** (gotchas 246, 248, 249; `tools/frame_era_medians.py` takes the null as a
  required argument).
* **DO NOT use matched-frame picture comparison outdoors.** Its n is zero and always was
  (254). `tools/frame_determinism.py` is the check and it takes seconds.
* **The frame time is PINNED at two vblanks (~32 ms)** for everything reachable, so a CPU
  saving does not show as frame rate and neither does a CPU cost (237, 243). Quote
  `outside`.
* **Every phase in `CZ_VK_PROFILE` is EXCLUSIVE of nested ones** as of part 20 (228).
* **Do not pin the GPU clock**; sample it with `tools/gpu_clock_sample.py` (219).
* **Three runs an arm on any crowd frame-time claim**; the floor is 10-13% (229).
* **`CZ_VK_VALIDATION=1` on at least one run per session, and quote the tally.** Standing
  gate: 20 `VkGraphicsPipelineCreateInfo-Input-08733` + 6 `...-topology-08773`, nothing
  else. Name every new object type you add (255).
* **SERIALISE BACKGROUND RUNS THROUGH ONE JOB**, and remember an operator can open the
  game at any moment.

**And one new one, which is about the ORACLE rather than about our own measurements:**

* **A capture replay is a decoder, and a decoder that ignores a packet reports absence.**
  Census the opcodes before trusting a replay (gotcha 262). And when the replay cannot
  reconstruct a value, it must SAY so rather than keep the stale one (263) — an
  unrecoverable register that prints a plausible number is the worst possible oracle
  output, and what exposed it here was an IMPOSSIBLE value, not a suspicious one.

## What part 27 established

**1. THE GROUND DRAW'S VERTEX DATA MATCHES HARDWARE EXACTLY, and so do its constants.**
`tools/xtr_draw_vertices.py` (new) reads hardware's streams out of `w1_spawn.xtr` in the
same shape `CZ_VK_DRAW_PROBE` prints ours. For the 25,234-vertex ground draw
(`vs_36eef2c94b4a065c` / `ps_ad65b98593f95926`), all five attributes agree to the printed
digit over six vertices. **The recorded anomaly — two float2 attributes at different slots
and offsets decoding identically — is what hardware does too**; the guest duplicates that
texture coordinate into two streams. Constants: `pc(1)`, `pc(22)`, `pc(45)`, `pc(46)`
identical, `pc(14)` a world position that must differ with the camera, `pc(253..255)`
unrecoverable from this capture.

**2. AND ITEM 00f's LAST LEAD WAS TILING.** The "same mesh drawn twice with `mask=F`"
draws carry `scissor 0,0 640x720` and `scissor 640,0 640x720` — this title's two
640-wide halves. Gotcha 265.

**3. THE CUBE DISAGREEMENT: conclusion kept, measurement replaced, magnitude off by 10x.**
Part 26's "414 of 414" counted only slots that already read cube and so could not have
found a disagreement (264). `tools/xtr_cube_agreement.py` asks it per declared fetch slot:
**0 of 13,203**. `CZ_VK_DIM_DISAGREE` enumerates our side: **9 (shader, slot, texture)
cases**, two textures, and at those draws **our slot 4 holds an exact duplicate of slot 3**
where the captures show hardware holding a real 128x128 DXT1 cube map (`0E751000`, dim 3,
depth 6) for the *same shader pairs*. Not a decode error — the same dump reads
`s6 06805000` as dim 3 depth 6 correctly.

**4. THE DECLINE HAD A SECOND, LARGER CAUSE.** `cube fetch got the dummy` = **3,210 of
1,903,592** cube fetches, splitting exactly into **2,182 "the constant at that slot is not
a texture at all"** and **1,028 "the dimension disagrees"**. The first shared a counter
with every 2D fetch and was therefore invisible (171).

**5. `LOAD_ALU_CONSTANT` WAS MISSING FROM ALL THREE `.xtr` TOOLS** — 620 packets against
36 `SET_CONSTANT`s in `w1_spawn` — and 81 of those loads read memory the trace does not
carry. Both fixed; the second is reported rather than papered over.

## Where part 28 starts, in order

0. **READ OUR TRANSLATED `ps_ad65b98593f95926` AGAINST THE CAPTURE'S DISASSEMBLY OF IT.**
   Every comparable input to the white ground now matches, so the defect is in the shading
   and this is the only place left to look. The capture's disassembly is
   `~/DR2CZ-troubleshooting/r2-shaders/shader_D007C18389DF0E55.ucode.frag` (187 lines;
   located by hashing the dword-swapped `.ucode.bin`, gotcha 261), and ours is
   `assets/shader_spv/ps_ad65b98593f95926.spv` plus whatever HLSL XenosRecomp emitted.
   The second ground pixel shader is `57d441f53fc93ad7` =
   `shader_5C87A47E50F72D6E.ucode.bin.frag`, and the vertex shader is
   `shader_9AAEF736E3DA38ED.ucode.bin.vert`.
   **Note it reads `c253`, `c254` and `c255`** — the three constants the capture cannot
   reconstruct — so if the disassembly reading dead-ends, that is the next capture request
   (see item 6).
0b. **WHY OUR SLOT 4 HOLDS A COPY OF SLOT 3.** Both remaining candidates are GUEST-side,
   not renderer-side: the environment map that material wants was never created in our
   runtime, or it was created and the engine's own bind was skipped. `01330000` (the other
   disagreeing texture) is already on file as "uploaded BLACK, guest memory NON-ZERO NOW",
   so the two are plausibly one defect in this title's texture creation. Start from
   `CZ_FILE_TRACE` / `import_call_sites.py` rather than from the renderer — and note the
   whole thing is 0.17% of cube fetches, so it is a correctness item and not a picture
   emergency.
1. **The other five cube maps are LOADED and one uploads BLACK.** `01330000` (4x4):
   `uploaded BLACK, guest memory is NON-ZERO NOW` — the texture arrived after our one and
   only upload and the fetch-constant cache froze it black. `CZ_VK_TEX_REFRESH=01330000`
   is the arm that says whether re-reading fixes it. **This is now known to be the same
   texture as one of the two disagreement cases**, so 0b and this are probably one item.
2. **SOUND — the game is silent and the operator has asked for it.** The guest hands us
   REAL BUFFERS FULL OF ZEROS (`null=0`, `non-silent=0`, `maxpeak=0.000000`, scanner
   self-testing at 0.5000), so the silence is upstream of the mixer and **the next step is
   XMA DECODE, not an output device**. Fable 2's `audio/xma_hw.cpp` (430 lines) and
   `audio/xma_decoder.cpp` (192, ffmpeg) are directly liftable and its `docs/audio-xma.md`
   is titled "why nothing the game mixed was audible". Output last; its timer trap applies
   to us verbatim (our pump is `sleep_for(5333us)` = ~184 frames/s against the 187.5 that
   48 kHz needs). Full item: `docs/open-items.md` 00e.
3. **The two remaining validation defects, and MEASURE before changing either.**
   `Input-08733` (20) is very likely the deliberate `USCALED`/`SSCALED` decision seen from
   the layer's side — read it against gotcha 122 first. `topology-08773` (6) is a
   `POINT_LIST` pipeline whose VS never writes `PointSize`; the question is what those six
   pipelines draw.
4. **THE OPERATOR'S VERDICT ON THE CUBE MAPS, on SURFACES rather than the frame.** Still
   owed and still cheap: same spot outdoors, reflective surfaces, three configs on one
   binary (default / `CZ_VK_NO_CUBE_SNAPSHOT=1` / `CZ_VK_NO_CUBE=1`). The headless answer
   is known — removing every cube map is 8x the baseline band, removing only the rendered
   one does not separate — so ask about the car bonnet and the shop window, not the median
   (gotcha 257). Put `CZ_SHADER_DUMP=~/DR2CZ-troubleshooting/ucode-dumps` on it.
5. **Re-test the remaining picture defects with the outdoor instrument that now exists** —
   shadow cascade (item 3), mipmaps (4), colour grading (6) — using
   `tools/frame_era_medians.py` with a measured null. And **the binned frame-time A/B still
   owed for `CZ_VK_FRAMES_IN_FLIGHT=2`** (part 23); read the median and the vblank-pinned
   share, not the mean.
6. **THE NEXT CAPTURE REQUEST, if the shader reading needs it.** One line: a single-frame
   trace whose `MemoryRead` records cover the **constant-buffer memory every
   `LOAD_ALU_CONSTANT` in the frame reads**. 81 of 620 are missing in the current set and
   `pc(253..255)` on the ground draw are exactly the casualties. Everything else about the
   round-2 method was right and should be repeated verbatim (gotcha 259).
7. The rest of `docs/open-items.md`, including item 12.

**Still deliberately NOT planned: giving `CZ_FAKE_PRESS_SEQ` a trigger.** A recipe would
have to ACQUIRE a gun and ammo along a long scripted path. Propose the acquisition first.

## Gates, on the part-27 binary

`--smoke` OK. `tools/shader_dim_census.py` exit 0 across all **410** shaders, the ucode
parse and the translated SPIR-V agreeing on every one. `grep -c "no translated shader"` =
**0**.

**Not re-run this part and owed before any claim that rests on them**: the A5 kernel-call
diff, `truncated=0`, the two PM4 capture oracles, the capture-E picture correlation, and
the Vulkan validation tally. No renderer BEHAVIOUR changed this part — both runtime edits
are a diagnostic and a counter split — but that is an argument, not a gate.

## The method notes worth carrying

* **An oracle needs its own gates.** Every discipline in this project points at our
  runtime; the capture tools had none, and they had been reporting hardware's shader
  constants as zeros for a whole part. The thing that caught it was asking *could the guest
  have run with this value?* — `c255.w = 0` where the shader uses it as its literal 1.0.
  **Impossible beats suspicious**: a stale-value bug that lands on a plausible number is
  invisible, and only the impossible one announces the class.
* **Write down what the disagreeing case would look like before quoting an N-of-N.** "414
  of 414 agree" was a filter that selected on the property under test (264). One sentence
  of pre-registration would have caught it, and the fix cost 200 lines.
* **A per-draw census that does not print the SCISSOR makes every tiled title look like it
  double-draws** (265). That duplicate was item 00f's most inviting lead for two parts.
* **An early return that shares a counter with the common case is not counted.** Two thirds
  of the cube declines had no name because "not a texture" was pooled with 216,866
  ordinary 2D fetches. Gotcha 171, and this is the third instance.
* **The oracle can close a lead by AGREEING with it.** The duplicated texcoord was on file
  as an anomaly; hardware does the same thing, so the branch closes properly rather than
  staying open as "we looked and saw nothing". That is refutation by compensation's cousin
  and it is worth the same as a finding.
