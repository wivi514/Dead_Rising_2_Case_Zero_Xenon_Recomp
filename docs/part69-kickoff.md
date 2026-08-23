# Part 69 kickoff — RT geometry: execute `docs/rt-remix-plan.md`, in its order

> **THIS IS THE LIVE HAND-OFF**, superseding `part68-kickoff.md`… which does not
> exist: part 68 ran inside part 67's session and its record is
> `phase5-notes.md` **§6cz**. The backlog entry is `open-items.md` 0v; the
> lessons are gotchas **398-402**.
>
> **THE PLAN IS `docs/rt-remix-plan.md` AND IT IS THE DOCUMENT TO EXECUTE.** This
> file exists to say where the port is and what is already settled, so the plan
> is not re-derived. Read, in order: `docs/rtx-remix-prior-art.md`, then
> `phase5-notes.md` §6cy and §6cz, then the plan.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

---

## 0. Where the feature is, in five lines

* **The occluders are placed and the placement is verified against hardware.**
  Part 67 found the position streams object-space and gave every TLAS instance
  its draw's own world matrix; part 68's operator session met every
  pre-registered prediction, and `tools/rt_placement_render.py` lands 2.9M placed
  vertices on Chuck, the zombies and the lamp posts of hardware's own frame.
* **What is left is the POPULATION, both halves of it**: geometry that never
  reaches the structure (`dyn`, 17-41% by location) and geometry that reaches it
  in the wrong shape (the palette blend collapsed to entry 0).
* **Exclusion cannot ship**: declining the palette draws costs 60% of the
  occluders, because that shader shape is the engine's main world shader.
* **RTX Remix has industrialised this exact problem** and its answers are the
  plan: refit rather than rebuild, an identity that is not a content hash, and
  skin rather than exclude.
* **A5 is owed**, carried from part 67. No kernel path changed in either part.

## 1. Start here, and do not start with code

`docs/rt-remix-plan.md` item 0 is a **one-flag check** and it changes the cost of
everything after it: `R->persist` is already created with `deviceAddress = true`,
already holds the guest vertex bytes dword-swapped exactly as the BLAS staging
swaps them, and is re-uploaded by the raster path on every guest rewrite. It is
missing only
`VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`.

Item 3 starts with a **one-draw offline experiment**, not a build: read the
dependent fetch's per-vertex palette indices for a known palette draw and count
how many distinct entries it references. That single number decides whether the
fix is "skin the actors" or "bake world positions for two thirds of the world",
and they are different jobs. **Census first, build nothing** is the instruction
that made parts 65 and 67 cheap and whose absence made part 66 expensive.

## 2. Do not re-buy any of these

* the placement, the transform table, and `config/rt_world_xform.json` — verified
  against hardware's own frames, twice;
* the injection into the 126 shaders, the screen-space alignment on both axes,
  the world reconstruction, the primary ray (all part 66);
* "a filter is eating the buildings" — the census says the buckets are ordinary;
* `CZ_VK_RT_NO_PALETTE` as a fix — it is a diagnostic, priced at 60%;
* comparing palette entry 0 with entry 1 to find the draws that do not really
  blend — measured, separates nothing (63.4% and 69.8% over two traces);
* route (a) entirely (§6cv §7j).

**And re-ask, do not re-buy, part 67's exonerations.** The sun, the ray length and
the origin bias were all cleared at 0.9% shadowed **against a pile at the world
origin**, which is no test at all (gotcha 172). Mode 20 said "no direction is
occluded" because the structure was empty. They are open questions again.

## 3. Known-open, named now so the session is read against a list

1. **RT HIGH's four rays buy no penumbra** — its octiles read 0.3% of pixels
   partially shadowed. Either `CZ_VK_RT_CONE` is too small or the rays are not
   spreading. Cheap to check.
2. **Self-shadowing** has never been tested against correctly-placed geometry.
3. `alpha` is 2.0-3.0M draws and stays raster-only by a stated trade; the
   chain-link fences are in it.
4. **Cost**: instances went 500 -> ~3,000 a frame, which is hash inserts on the
   PUMP thread — the thread part 55 showed is the frame rate. Only paid with RT
   armed. Price it with `CZ_VK_PROFILE` and a soak, never from a run carrying
   `CZ_VK_FRAME_STATS`.

## 4. Instruments this part inherits

* **`CZ_VK_RT_FACTOR_READBACK=N`** — start every RT investigation here, and read
  its positive control (`CZ_VK_RT_FACTOR_POISON=1`, must be ~100%) first.
* **The edge-density statistic** — the thing that turned "the shadows look wrong
  near zombies" into 100.5 against 10.6 per 1000 px. It is a few lines of numpy
  over the factor PGM and it is the gate for the palette work.
* `tools/rt_placement_render.py` — placement against hardware's own frame.
* `tools/rt_tlas_census.py` — Collect's whole chain re-run over the traces.
* `tools/rt_world_xform_census.py` — the per-shader transform table, **and a
  coverage gate**: re-run it after ANY shader-cache change.
* Arms: `CZ_VK_RT_OBJ_XFORM=0`, `CZ_VK_RT_DYN_SETTLE=N`, `CZ_VK_RT_NO_PALETTE=1`.
* Sessions: `tools/part67_placement_session.sh`, `part68_dyn_session.sh`,
  `part68_palette_session.sh` — all self-reading, all refusing to continue on a
  log under 4 KB.

## 5. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**
  (`perf-state-parked.md`, part 62).
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict.
* Suspected input leak at panel open; decal flicker (needs a sighting);
  doubled-slab watch (00q); the point-list PointSize VUID class.
* **PERFORMANCE IS PARKED** — `docs/perf-state-parked.md` resumes it.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 6. Gates at part 68's close

* `--smoke` OK.
* `shader_dim_census.py` clean on all sixteen caches; the play cache's NAME diff
  against stock empty.
* `rt_world_xform_census.py`: 104 of 104 covered, exit 1 on a planted gap.
* `rt_tlas_census.py` runs over all twenty traces and prints its verdict.
* **A5 is owed.**
