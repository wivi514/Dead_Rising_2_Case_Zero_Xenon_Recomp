# Part 66 kickoff — RT shadows, ROUTE (B): the factor computes LIT

> **THIS IS THE LIVE HAND-OFF**, superseding `part65-kickoff.md`. Part 65
> (2026-08-22) executed that kickoff's whole spec — the census, the shader
> substitution, the factor pass, the tier ladder — and then spent three operator
> sessions and a long headless ladder on the one thing left:
>
> **The injection is PROVEN and the factor computes 1.0 (lit) everywhere.** Our
> image is read by all 126 patched shaders and drives the title's shadow term;
> what it contains is wrong. §0 has the ladder, the calibrated readings, the live
> suspect, and FIVE dead causes not to re-buy.
>
> **Part 66's first action is NOT an operator session.** It is meeting the gate at
> the end of §0 — the depth arm must turn the world black — because part 65 handed
> over three builds without it and burned three of the operator's sessions
> learning it was not met.
>
> The record is `phase5-notes.md` **§6cw**; the backlog entry is
> `open-items.md` 0v; the arms are in `instruments.md`; the transferable lessons
> are gotchas **387-390**. Route (a)'s record (§6cv) is history now — read §7j
> only if someone proposes re-opening it.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part
> 59). Part 64's session is why part 65 exists at all, and part 65's sessions
> earned their keep the same way: the operator's eleven words named the failure
> class in one sentence where the headless statistics had three wrong answers.

---

## 0. WHERE IT ACTUALLY IS — the injection is PROVEN, the factor computes LIT

Part 65 did not stop at building route (b). It went through **three operator
sessions**, and they moved the problem a long way. Read this before touching
anything; almost every plausible next step has already been tried and killed.

### The operator's verdict, which is the ground truth here

> *"There is just 0 shadow except the power line in main menu, and they are not
> ray traced, they are just a leftover of normal shadow."* — and, of the poison
> arm, *"only run 1 show darkened (maybe shadow on floor everywhere but not from
> object zombie or building)"*. All captures were at **RT LOW**.

Both halves are findings:

* **THE INJECTION IS PROVEN.** Poison darkening the world is the whole purpose of
  that arm — it says our factor image is written by the pass, read by all 126
  patched shaders, and drives the title's own shadow term. That is the one result
  that could have killed route (b) outright, and it did not.
* **THE FACTOR COMPUTES 1.0 (LIT) EVERYWHERE.** So no RT shadow reaches the
  screen, and the fault is inside the pass that computes the factor.

### The ladder, and how to read it

`CZ_VK_RT_FACTOR_DEBUG=N` turns each link of the pass into a picture, and
`tools/part65_luma_read.py` reads the arms off `CZ_VK_FRAME_STATS` medians. **It
REFUSES any file with under 85% of the control's frame count** — see §5.

Two calibration points make the whole table readable: **all-lit = 99.86**
(control) and **all-shadow = 90.16** (poison). A factor covering half the screen
must land halfway, at ~95.0.

| mode | what it writes | median | reading |
|---|---|---|---|
| — | RT off | **99.86** | the lit end of the scale |
| — | RT on, real factor | 100.49 | **nothing happens** |
| — | poison (uniform 0) | **90.16** | the shadow end of the scale |
| 4 | uniform 0 via the debug selector | 90.09 | the selector reaches the shader ✓ |
| **14** | **screen-space stripes, no texture** | **95.50** | **the round trip carries SPATIAL DETAIL ✓** (a 50/50 pattern landing on the midpoint) |
| 1 | `z >= 0.999999 ? lit : shadow` | 100.89 | the sampled depth is FAR everywhere |
| 8 | `frac(z*1024) < 0.5` | 90.64 | uniformly shadowed, i.e. **z is exactly 1.0** |
| 9 | `z` returned as the factor | 100.29 | mean depth ≈ 1.0 |
| 12 | the COLOUR buffer's luminance | 90.34 | ambiguous alone — a working sample returning a realistic ~0.3 would land here too |
| **13** | **"is there ANY colour here", binary** | **90.81** | **the colour sample returns BLACK.** This is the unambiguous form of 12 |
| 5, 6 | depth bypass, fixed z = 0.99 / 0.999 | 99.01 / 100.59 | nothing |
| 3, 7 | unbiased rays / rays straight down | 100.56 / 100.79 | nothing |

**The shape of the table is the finding.** Everything the shader computes ITSELF
works — a uniform constant lands exactly, a stripe pattern lands exactly halfway.
Everything it SAMPLES comes back empty: the depth reads exactly 1.0 and the colour
buffer reads ~0, through the same descriptor set, sampler, uv and barrier.

**So the live suspect is the pass's texture bindings, not the depth, the sun, the
matrix or the ray.** Two different images, two different bindings, one sampler:
the depth reads exactly 1.0 and the colour reads exactly black, while a value the
shader computes for itself lands to within 0.5 luma of where it should. Mode 12 exists because three depth probes agreed and all
three shared one untested assumption — that this pass can sample an EDRAM
attachment at all. It cannot, apparently, sample either one.

### The next probe, and why it is the right one

Sample nothing and ask the descriptor a question only a correctly bound image can
answer: **`GetDimensions()`**. The extent comes from the descriptor, not from
memory contents, so it separates "the descriptor references a real image whose
contents are genuinely far/zero" from "the descriptor references nothing". Add it
as mode 15 (depth) and 16 (colour); a real 3440-wide image returns a value the
luma scale can show, a bogus descriptor returns 0.

### FIVE CAUSES ARE DEAD — do not re-buy any of them

Each was proposed, instrumented, and killed **by its own counter**, which is the
one thing that went right all session:

1. **The sun direction.** It WAS wrong — a top-down ortho at a 3587-unit volume
   was being captured instead of the sun, and the bias derived from it lifted every
   ray origin 5.4 world units clear of its caster. Fixed properly (§1), and the
   picture did not move. A real bug, not this one.
2. **Z-prepass timing, on the colour mask.** Gate added; it declined **0** draws.
3. **Z-prepass timing, on `RB_MODECONTROL` edram_mode.** Also **0**. The
   atlas-sampling draws are all genuine `kColorDepth` shading draws.
4. **The world-position reconstruction.** Argued from mode 6 reading 13.49 — which
   was a partial read; complete it reads 100.59.
5. **Barrier access/stage masks.** `Barrier()` is already a full
   `ALL_COMMANDS -> ALL_COMMANDS` with `MEMORY_READ|MEMORY_WRITE`. Nothing to fix.

### The gate to insist on before the operator sees another build

**The depth arm (mode 1) must turn the world black and leave the sky lit** — i.e.
land near 90, not near 100. Part 65 handed over three builds without that gate and
spent three of the operator's sessions learning it was not met. Do not repeat it.

---

## 1. What part 65 built, so it is not rebuilt

* **The sun binding, fixed three times over** (§6cw §9) — latch at a cascade
  resolve, then bind the atlas by dataflow, then choose by **per-frame MAJORITY of
  cascade slices**, because the atlas holds three sun cascades plus something else
  sharing the same destination and only the DIRECTION distinguishes them. Reads
  `sun=(-0.363 0.545 -0.756) won 3/2 slice votes` in gameplay, 3 switches in
  40,435 passes, bias 5.382 -> 0.159. Correct, and it did not move the picture.
* **`tools/shadow_shader_census.py`** — which pixel shaders sample the cascade
  atlas and at which fetch slot, out of the `.xtr` world traces. **126 shaders,
  140 (shader, slot) pairs**, 42,620 draws across twenty traces. It also
  classifies what each use FEEDS: `pcf4` (116 uses, four ±0.5 taps compared
  `> receiverDepth`) and `tap1` (24 uses, one centre tap feeding
  `saturate((receiver − sampled) * k − bias)`). Both monotonic and saturating,
  which is why ONE substitution serves all 140. Output:
  `config/rt_shadow_slots.json`.
* **`tools/part65_operator_session.sh`** (three arms: poison / live panel / cone)
  and **`tools/part65_ladder_session.sh`** (the link-splitting ladder). The first
  established what §0 records; the second is what to re-run once the gate is met.
  **`tools/part65_luma_read.py`** reads any set of `CZ_VK_FRAME_STATS` files as
  medians and refuses partial ones.
* **`tools/patch_rt_shadow_hlsl.py`** + `build_shader_spv.sh`'s `CZ_HLSL_PATCH`
  hook — the substitution, on XenosRecomp's HLSL before DXC. Taps become a
  lookup of our factor at the shader's own `SV_Position`; `getWeights2D` on an
  atlas slot returns 0.5, which makes every 2x2 weight product 0.25 whatever the
  swizzle and turns the title's own filter into a five-level quantiser.
* **`runtime/gpu/rt_factor.hlsl`** + `rt_factor_spv.h` — the factor pass. World
  position from the scene depth and composite, origin pushed off the surface
  along the sun and toward the camera, ray query against route (a)'s TLAS.
* **The renderer wiring** — a variant module per shader in `ShaderMeta`,
  `PipelineKey::passFlags` bit 1, the pass triggered by the title's own first
  atlas-sampling draw and invalidated at every resolve, the scene composite
  captured with a distinct-value check, and `VkRenderer_RtAvailable()` now
  requiring the variant cache so a dead rung says why.

**The caches that exist**, all 449 modules: `shader_spv`, `_a2m`, `_clip`,
`_clip_a2m` (what a play session selects), `_rt`, `_clip_a2m_rt`. The runtime
finds the RT sibling by appending `_rt` to whatever `CZ_SHADER_SPV` names, so
turning RT on does not also change the foliage or the slicing.

## 2. What route (b) can still buy, if the verdict is good — priced, not promised

* **MED/HIGH could add the missing casters.** Nothing outside the TLAS casts an
  RT shadow: skinned actors (zombies, Chuck — structurally 3.0% of scene draws,
  §6cu) and alpha-tested foliage. On route (a) that hole was permanent because
  the raster cascade was unioned underneath; on route (b) the raster shadow is
  REPLACED, so the hole is visible and buying it back is a real tier feature.
  Per-frame BLAS rebuilds for skinned meshes is the price and it has never been
  measured on this title.
* **The five-level ceiling.** The `pcf4` family quantises our factor to five
  levels and the `tap1` family (24 uses) stays binary. Going past that means
  patching the compare itself, which is per-shader work at a population of 126 —
  do not start it without a reason from the picture.
* **Cost.** `CZ_VK_PROFILE`'s `rt` phase carries the factor pass. Nothing has
  been measured on the operator's machine; performance is PARKED
  (`perf-state-parked.md`) and an RT tier is opt-in, so this is a number to
  quote in the panel, not a regression to chase.

## 3. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**, filed
  in `perf-state-parked.md` (part 62). If the operator asks for it, THAT is the
  part's work instead.
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict — arm 2 of the
  part-65 session asks for it, so it should come back with everything else.
* Suspected input leak at panel open (one Resolution-row write per open).
* Decal flicker: waiting on a sighting; F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1`.
* Doubled-slab watch (00q): F9 + immediate F8 on any sighting.
* Live-resolution switch parked; the point-list PointSize VUID class is the only
  validation class the RT work leaves behind.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 4. Measurement discipline this part must not repeat

Part 65 retracted **three** conclusions, all the same way: a `CZ_VK_FRAME_STATS`
file is APPENDED TO while the run continues, so a read taken early measures a
different PLACE in the boot, not a noisier version of the same one (gotcha 384).
The worst was `CZ_VK_RT_FACTOR_DEBUG=6`, which read **13.49 at n=383** and
**100.59 at n=2,924** — the difference between "the whole ray chain works" and
"nothing works at all", from the same arm. It was quoted as evidence.

`tools/part65_luma_read.py` now REFUSES to print a median below 85% of the
control's frame count and says so in place of the number. Use it rather than
reading these files by hand, and note that **turning `CZ_VK_VALIDATION=1` on slows
a run below that bar** — validate and measure in separate runs.

The second repeat to avoid: **a control that bypasses the computation cannot
validate the computation.** Part 64 called route (a)'s ray path "proven end to
end" on `CZ_VK_RT_POISON`, which returns a constant before any reconstruction
happens — so it proved the plumbing and never touched the arithmetic, and route
(b) inherited the assumption unexamined. The same blind spot recurred inside the
ladder itself: every arm that worked wrote a UNIFORM factor and every arm that
failed wrote a VARYING one, and no mode tested that difference until mode 14 was
added specifically to.

## 5. Practical notes

* **Every variant cache needs the membership gate, not just the stock one**
  (gotcha 390). Part 65 found the operator's play cache ten shaders short since
  2026-08-19 — absent, not stale, so their draws were SKIPPED in every session
  for three parts. All six caches are now 449 and rebuilt.
* `/tmp` is a tmpfs; a 3440x1440 frame dump is ~15 MB. Dump to
  `~/DR2CZ-troubleshooting/`.
* Gates at part 65's close, RT off (the shipped default): `--smoke` OK; **A5 exit
  0** (4 permutation windows, 0 real); `shader_dim_census.py` clean on every
  cache; each RT cache differs from its plain rebuild in exactly 126 modules.
