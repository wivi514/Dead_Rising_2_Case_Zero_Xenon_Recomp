# Part 67 kickoff — RT shadows: the TLAS is a ground plane. Census it, do not build.

> **THIS IS THE LIVE HAND-OFF**, superseding `part66-kickoff.md`. Part 66 ran five
> operator sessions and ended with the defect LOCATED and everything downstream of
> it exonerated by measurement. **Part 67's first move is an offline census. Build
> nothing before it returns a list** — that instruction is what made part 65's
> shader census worth having, and part 66 wasted two sessions by not obeying it.
>
> The record is `phase5-notes.md` **§6cx** (§7 is the session log, §8 is the
> answer, §9 the lessons); the backlog entry is `open-items.md` 0v; the arms are
> in `instruments.md`; the lessons are gotchas **391-397**.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (standing instruction),
> and the Fable 2 port is NOT a renderer reference (operator instruction, part 59).

---

## 0. THE ANSWER, so it is not re-derived

**No direction above a receiver is occluded, so no sun vector could ever have
produced a shadow. The ray-tracing structure is effectively a ground plane.**

`CZ_VK_RT_FACTOR_DEBUG=20` fires eight FIXED directions over the upper hemisphere
with the sun deliberately not involved, and outdoors it reads **97.3% fully open,
mean 0.987 — 1.3% average occlusion.** Everything else is exonerated:

| link | evidence | status |
|---|---|---|
| the injection into 126 shaders | poison reads **100.0% shadowed** | ✓ |
| screen-space alignment, X and Y | stripe pair, a clean transpose, 8 bands each, flat on the other axis | ✓ |
| the receiver's world position | mode 2's checker is **perspective-correct and world-locked** | ✓ |
| the primary ray | **85.2%** hit, mask matches the captured frame's skyline | ✓ |
| the ray mechanism | rays DOWN hit, rays UP escape — correct for a world below a sky | ✓ |
| ray length | 2000 units vs 116.5: **0.9% either way** | ✓ not the fault |
| the sun direction | mode 20 says no direction is occluded | ✓ not the fault |
| **what is IN the structure** | ~700 static opaque meshes for a whole town | **✗ THE DEFECT** |

## 1. STEP 1 — THE CENSUS. Two questions, both offline, no session

The collector's own counters, at depth on the operator's route:

```
collected=10.9M   skips: dyn=19.0M  alpha=3.2M  prim=14.6k  bounds=5026
                         new=1300   endian=0    range=0     collide=0
tlasInst=216..722   blas=1920 (42.3 MB)   prevKeys == tlasInst (nothing throttled)
```

**(a) What ARE the ~700 accepted instances?** A histogram of their world-space
vertical extents answers "is this a flat sheet?" in one pass. The BLAS bounds are
already computed by the bounds gate part 64 added, so this is a print, not a
measurement. Add the aggregate too: the TLAS's own min/max Y against the scene's.

**(b) Which filter eats the buildings?** `dyn` is 57% of every draw the collector
sees and `alpha` is 10%. `dyn` comes from the persist store's `dynamic` flag — the
raster path's change detector — so a static building whose vertex buffer the title
re-uploads is dropped as if it were a skinned zombie. That is the first hypothesis
to test and it is testable offline against the `.xtr` traces, which say what
hardware draws for the scene pass and how big each draw is.

`tools/rt_depth_order_census.py` is the model for both: a `.xtr` carries the whole
frame's draw stream with the register file at every draw, and our own counters say
what we did with each class (gotcha 387).

**Arms that already exist for (b)**: `CZ_VK_RT_CASTERS=cascade` uses the title's
OWN shadow casters — the meshes the game itself believes cast shadows, which is
exactly the occluder set — and `CZ_VK_RT_BOUNDS_CAP=<f>` relaxes the bounds gate.
Neither has been tried on route (b) since the primary ray landed.

## 2. Do not re-buy any of these

Killed by their own counters in part 66, most of them by measurement the same hour:

* the pass's texture bindings (part 66 kickoff's headline suspect — refuted; the
  depth buffer was genuinely at its clear value because **this title has no scene
  Z prepass**, proven against 20 `.xtr` traces offline);
* ladder modes 12 and 13 (`g_colour` was never bound — a 3-element write array
  passed with a count of 2);
* the 427 px vertical misalignment (real, found by the operator, FIXED);
* the sun's SIGN (`CZ_VK_RT_SUN_FLIP=1` gives a uniform blanket — the ray points
  at the ground and every receiver self-hits, column profile flat at range 0.11);
* the ray LENGTH (2000 units reads 0.9%, same as 116.5);
* the origin BIAS;
* the world reconstruction;
* and route (a) entirely (§6cv §7j).

**Two of part 66's own mid-session claims are retracted** and are worth reading
before trusting a partial number again: "the ray length is the biggest effect yet"
(read off frame 1228 of a file still being written; complete it is 0.9%), and "the
skyline silhouette proves the TLAS contains the world" (a bare ground plane
produces the identical silhouette — gotcha 395).

## 3. The instruments part 66 leaves behind, and how to use them

* **`CZ_VK_RT_FACTOR_READBACK=N`** — the factor image's own histogram, printed as
  `[rtb] FACTOR IMAGE`. **Start every future RT investigation here.** Its positive
  control is `CZ_VK_RT_FACTOR_POISON=1`, which must read ~100% shadowed; run it
  first and believe nothing else until it does. `CZ_VK_RT_FACTOR_PGM=<dir>` also
  writes the images, and `tools/rt_factor_pgm_read.py` reads them for STRUCTURE
  (row/column profiles, band count and pitch from midpoint crossings) because a
  mean cannot see a displacement.
* **The ladder**, now 20 modes. The ones that earned their keep: **2** the world
  checker (revived — it had never validly run), **14 + 19** the alignment pair
  (run BOTH, agreeing is the result), **17** primary-ray hit, **20** hemisphere
  occlusion with the sun excluded.
* **`tools/rt_depth_order_census.py`** — ordering inside a hardware frame.
* **The collector census now prints on route (b)** (`[rt] collector: ...`). It
  printed only from route (a)'s `TraceSlice` for three parts. It is the first line
  to read for step 1.
* Session scripts: `part66_factor_session.sh` (five arms, self-reading),
  `part66_ray_session.sh`, `part66_world_session.sh`. All three refuse to continue
  when an arm's log comes back under 4 KB.

## 4. Carry-overs (non-blocking, ask when convenient)

* **Turn-stutter under the wide-culling over-widen — operator-deferred**
  (`perf-state-parked.md`, part 62).
* Small open verdicts from parts 61-62: a gore cut with the fov slider off zero;
  aiming behaviour under the slider; the cutscene 21:9 CROP look.
* **Owed since part 60**: the shadow Low-vs-High LOOK verdict.
* Suspected input leak at panel open; decal flicker (needs a sighting);
  doubled-slab watch (00q); the point-list PointSize VUID class.
* Shader cache 449; any run reaching new ground carries `CZ_SHADER_DUMP`
  (`~/DR2CZ-troubleshooting/ucode-dumps`, never `/tmp`).

## 5. Measurement discipline, from a part that broke four of its own rules

* **A partial read measures a different PLACE** (gotcha 384). Part 66 quoted one
  anyway, in the same session in which it had quoted the gotcha at the operator.
  Gate every reading on the arm having ENDED.
* **Spatial controls come in pairs** (394) and **a silhouette proves occupancy,
  not content** (395).
* **Read the artifact, not its effect through a lossy channel** (397).
* **A counter added in the same commit as its subject is never read** (391) — part
  65 shipped the exact instrument that named part 66's first finding, and every run
  on disc predated it.
* **An arm that never ran must not print "done"** (396).

## 6. Gates at part 66's close

RT is OFF by default and nothing outside the RT pass changed.

* `--smoke` OK.
* **A5 exit 0** — 4 permutation windows, 0 real (run as arm 0 of session 1).
* `tools/rt_depth_order_census.py` runs over 20 traces and prints its own verdict.
* `shader_dim_census.py` clean on all sixteen caches; the two play caches' NAME
  diff against stock is empty (gotcha 390).
* The generated `rt_factor_spv.h` PS module carries the ray queries and image
  queries the new modes need.
