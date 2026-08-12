# Part 33 hand-off (for part 34). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part32-kickoff.md` for "where the
port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

The game boots, renders, plays, makes sound and plays its cinematics through. Part 33
took the port's largest picture defect — the white `rgb(180,180,180)` surfaces open
since part 26 — and closed it: the plateau was **NaN vertex normals laundered by the
shared tone epilogue**, and the NaN was minted by a **vertex-input type mismatch on
fmt16 packed normals** (`R32_UINT` attribute against a `float4` TEXCOORD input,
VUID-VkGraphicsPipelineCreateInfo-Input-08733). The fix is two commits (XenosRecomp
4621beb, runtime 7889e99); the plateau reads 0, the scene gains real lighting on every
fmt16 mesh, and validation is clean of 08733. Every fmt16 mesh had garbage normals
since phase 5 — the plateau was only the visible tip — so several parked picture items
should now be re-asked.

## WHAT PART 33 DID — do not rebuild any of this

Full record: `docs/phase5-notes.md` §6bg. Item 00f is closed in place.

* **The measurement chain**, one run each, scene snapshot at frame 3000 outdoors:
  baseline plateau 1,092 px; `XE_FLOOR_IS_NAN` -> plateau 0, magenta 1,187, green 0
  (every plateau pixel had a NaN at its tone-curve max; none honest, none stale);
  the floor-predicate control paints the SAME population green (flag follows the
  predicate — a NaN fails `>` and `<` alike, so the swap is its own control);
  `XE_NAN_IN_PAINT` -> 20,563 px (NaN ARRIVES in interpolants, 17x the plateau);
  `XE_NAN_VS_KILL_IN` -> 0 (it is in the declared vertex data).
* **Two nulls that pointed the right way**: `CZ_VK_RANGE_CENSUS` (new) — 786,861 draws,
  zero index overruns past any stream, zero NaN bytes in any float-format attribute,
  FP32 and FP16; so the streams were clean and the NaN was minted at the FETCH.
  `CZ_VK_ROBUST` (new) was recognised as a NO-TEST before its null was believed:
  streams share one arena VkBuffer, so robustness bounds the arena, not the stream
  (gotcha 283).
* **The naming run**: `CZ_VK_VALIDATION=1` printed ten 08733 pipelines — `R32_UINT`
  attributes at TEXCOORD-range locations against `vec4 of float32` inputs. Format census
  over the 416 sidecars: the only `R32_UINT` source is fmt16 (`k_10_11_11`), 37 vertex
  shaders, always TEXCOORD-wrapped. Fable 2 wraps packed normals as NORMAL (uint4 path,
  already correct), which is why three ports never hit it before.
* **The fix**: emitter unpacks fmt16 in-shader (`XeUnpack_10_11_11(asuint(input.x))`,
  chosen by the fetch instruction's own static format field — no spec constant); runtime
  binds fmt16 `R32_SFLOAT` (type-matched, bits intact). 35 of 416 modules changed;
  `assets/shader_spv` is rebuilt and installed. Gates: `--smoke` OK,
  `shader_dim_census` exit 0, `no translated shader` 0, byte-identity held on every
  emitter change before the fix itself.
* **Three XenosRecomp instruments** (XenosRecomp adc79c9, documented in
  `docs/xenonrecomp-upstream-bugs.md`): `XE_FLOOR_IS_NAN`, `XE_NAN_IN_PAINT`,
  `XE_NAN_VS_KILL_IN`, each with a `_FORCE` positive control. Two runtime instruments
  (23865ed): `CZ_VK_ROBUST`, `CZ_VK_RANGE_CENSUS`.
* **Gotchas 281-283** (NaN laundering / run validation first / robustness bounds the
  buffer, not the sub-allocation). Part 27's `XE_NAN_PAINT` zero-magenta reading is
  corrected in place in `xenonrecomp-upstream-bugs.md`.

## READ THIS BEFORE MEASURING ANYTHING

Everything from parts 26-32's lists stands, plus:

* **Any picture measurement recorded before part 33 on a scene containing fmt16 meshes
  has garbage normals in its baseline.** That includes every outdoor era median, the
  exposure comparison (ours 1.0 vs hardware 0.298-0.331), and both cube-map A/Bs. Do
  not treat those numbers as current baselines; re-run the arm if it matters.
* The NaN-input footprint was 2.23% of an outdoor frame, on zombies, barriers, props
  and buildings — wrong-but-unremarkable colours, not white. "The white is fixed" and
  "the picture matches hardware" are different claims; only the first is measured.

## WHERE TO START

0. **ASK THE OPERATOR TO PLAY.** Two things at once: their verdict on the white
   surfaces at the seven part-27 locations (spawn, gas station, pawnshop, register,
   slot machines, bathroom, newsboxes — the `DISABLE TIME OF DAY` night trick makes
   them unmissable if any survive), and the part-32 three-way shadow verdict
   (`CZ_VK_MSAA_WINDOW_SCALE_Y=1` vs null vs `CZ_VK_NO_ADDR_TILE_FOLD=1`) which was
   already owed and is now on a much better-lit renderer. Wire `CZ_SHADER_DUMP` to
   `~/DR2CZ-troubleshooting/ucode-dumps` as always.
1. **RE-MEASURE THE EXPOSURE DISCREPANCY** (§6ba's open question): `CZ_VK_PSBIND` for
   `pc(14).w` on the outdoor route. If our auto-exposure now settles near hardware's
   0.3 the §6ba question closes for free; if it stays at 1.0 there is a second defect
   upstream of it.
2. **Re-ask the parked picture items on this renderer**: LOD/00i (was explicitly
   parked behind 00f), NPC part meshes, mipmaps, the colour-grading LUT, and the
   cube-decline defect (the `s3`/`s4` duplicate — a SEPARATE defect, still open, 9
   enumerated cases).
3. **Part 32's item 0** — reconcile the scene tile's 4x clear and ship
   `CZ_VK_MSAA_WINDOW_SCALE_Y` as the default — is untouched by part 33 and remains
   the recommended shadow next step (`part32-kickoff.md` still describes it).
4. The rest of `docs/open-items.md`, and `docs/perf-cpu-plan.md`'s CPU/GPU overlap.

## Gates, on this binary

* `--smoke` OK. `tools/shader_dim_census.py` exit 0 (417 modules, the lost-microcode
  sidecar still the only one without `tfetchDims`). `no translated shader` = 0 on every
  run of the day.
* `CZ_VK_VALIDATION=1` on the outdoor route: **zero 08733** (was 10). The 3
  `topology-08773` lines (patch topology) predate part 33 and are untouched.
* **Not re-run and owed before any claim resting on them**: the A5 kernel-call diff,
  `truncated=0`, the two PM4 capture oracles (no CP change this part, so expected
  clean), the capture-E picture correlation (the picture CHANGED this part — by design —
  so the recorded +0.9597 is stale and should be re-measured, expecting improvement).

## The artifacts

Scratch runs live in the session scratchpad (`arm_base`, `arm_nan`, `arm_floor`,
`arm_nanin`, `arm_nankill`, `arm_robust`, `arm_range*`, `arm_valid*`, `arm_fix`) —
tmpfs, so quote the numbers from §6bg, not the files. The diagnostic caches
(`assets/shader_spv_nanpaint`, `_nanforce`) were built pre-fix and are STALE; rebuild
before reusing.
