# Part 70 kickoff — the RT geometry work: items 0-3 are built, item 4 and the session are owed

> **THIS IS THE LIVE HAND-OFF**, superseding `part69-kickoff.md`. The record is
> `phase5-notes.md` **§6da**; the backlog entry is `open-items.md` 0v; the lessons
> are gotchas **403-407**. The plan being executed is still
> `docs/rt-remix-plan.md`, and its items 0, 1, 2 and 3 are done.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

---

## 0. Where the feature is, in five lines

* **Items 0-3 of the Remix plan are in the binary**, each with a same-binary
  control arm: a live vertex source (`CZ_VK_RT_NO_DIRECT_BUFFERS=1`), an identity
  that is not a content hash (`CZ_VK_RT_STABLE_KEY=0`), BLAS refit
  (`CZ_VK_RT_NO_REFIT=1`) and the palette blend baked into the BLAS vertices
  (`CZ_VK_RT_NO_BAKE=1`).
* **Item 2's own gate is met headlessly**: at `CZ_VK_RT_DYN_SETTLE=0` — the
  configuration that used to climb to the 1 GB cap — the structure holds at
  `blas=4493 (78.6 MB, built=4509, flushes=0)` over the outdoor route.
* **The population went up 4.9x**: `tlasInst` **682 -> 3,356** at a matched 4,097
  RT passes, once a baked mesh's identity included its occurrence ordinal within
  the frame. Half of every palette draw is the same vertex buffer redrawn under a
  different palette, and without that the occurrences collapsed into one another.
* **Entry 0 was never an approximation of this geometry**: zero of 2,786 palette
  draws reference a single matrix, and entry 0 collapses the median draw's extent
  to 1.51 units where the blend assembles it at 8.75.
* **What is owed: one operator session** — `tools/part69_rt_geometry_session.sh`,
  six arms, **two of which need no eye at all**. A5 is arm 0 and has been owed
  since part 67.

## 1. Start here

Run the session. Its arms `dyn0` and `old` are a log-only PAIR and the second is
the positive control: `old` backs items 1 and 2 out under the same load and is
EXPECTED to flush. If it does not flush, the growth those items exist to stop was
never happening on the operator's route and they are unpriced (gotcha 30).

The picture pair is `bake` against `nobake`. The pre-registered prediction is in
`phase5-notes.md` §6da §7: the factor image's crowd-region edge density falls from
part 68's 100.5 per 1000 toward the open-road 10.6 with no change on open road,
and a shadow boundary BENDS over a van's roof and down its side instead of running
as one straight line across wall, van and ground.

## 2. Then item 4, which is the rest of the plan

* **`CZ_VK_RT_DYN_SETTLE`'s default is unchanged** and moving it is a measurement,
  not an edit. With refit in place it may want to be 0 — that is what arm `dyn0`
  is for.
* **Re-ask part 67's exonerations against the new structure.** The sun direction,
  the ray length and the origin bias were all cleared at 0.9% shadowed **against a
  pile at the world origin**, which is no test (gotcha 172). Mode 20 said "no
  direction is occluded" because the structure was empty.
* `CZ_VK_RT_NO_PALETTE` can retire, or stay as a tier knob if the blend costs too
  much on the operator's machine.

## 3. Do not re-buy any of these

* the placement, the transform table, `config/rt_world_xform.json` — verified
  against hardware's own frames twice, and the table now also carries the blend
  descriptor for all 18 palette shaders;
* "the palette approximation is fine for the static world" — refuted, 0 of 2,786;
* comparing palette entry 0 with entry 1 to find the draws that do not blend —
  measured, separates nothing;
* **the frustum test as an oracle for the blend** — it saturates at 96.55% vs
  96.38% (gotcha 403). Use the extent/displacement statistics
  (`tools/rt_bake_check.py`) or the visual splat
  (`tools/rt_placement_render.py --blend`);
* route (a) entirely (§6cv §7j).

## 4. Known-open, named so the session is read against a list

1. **Cost is entirely unpriced.** The blend is CPU-side on the pump thread —
   the thread part 55 showed IS the frame rate — and instances went from a few
   hundred to ~3,400 a frame. Only paid with RT armed. Price it with
   `CZ_VK_PROFILE` and a soak, never from a run carrying `CZ_VK_FRAME_STATS`.
2. **RT HIGH's four rays buy no penumbra** (0.3% of pixels partially shadowed).
3. **Self-shadowing** has never been tested against correctly-placed geometry.
4. `alpha` is 2.0-3.0M draws and stays raster-only by a stated trade.
5. `palConflict` is down from 2.36M to 2,227 but is not zero — the residual is
   draw-order shifts between frames, and it is bounded and self-correcting by
   design. Watch it rather than chase it.

## 5. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**
  (`perf-state-parked.md`, part 62).
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict.
* Suspected input leak at panel open; decal flicker; the point-list PointSize VUID
  class (6 per run, unchanged from part 68).
* **PERFORMANCE IS PARKED** — `docs/perf-state-parked.md` resumes it.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 6. Gates at part 69's close

* **`CZ_VK_VALIDATION=1 CZ_VK_RT_SHADOWS=1 CZ_VK_RT_DYN_SETTLE=0` on the headless
  outdoor route to `passes=4097`: no VUID but the known point-list `PointSize`
  one.** Four synchronisation VUIDs were found during this part and fixed
  (`phase5-notes.md` §6da §9); this run is the confirmation, taken at the exact
  pass count where the failing run had twenty of each. **Re-run it after any
  change to the RT structure code — it is the cheapest gate this feature has.**
* `--smoke` OK.
* `rt_world_xform_census.py`: 104 of 104 covered, exit 0, and the tool now
  self-checks its own JSON output.
* `rt_palette_census.py` and `rt_bake_check.py` run over the world traces.
* `shader_dim_census.py` and the play cache's NAME diff — **owed, unchanged since
  part 68 and no shader-cache change was made here**.
* **A5 is owed** and is arm 0 of the session. No kernel path changed in parts
  67, 68 or 69.
