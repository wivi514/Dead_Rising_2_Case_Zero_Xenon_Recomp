# The overnight plan — the occluder work is done, and the defect is somewhere else

Written when the operator went to sleep, closing part 69's session with:
*"Didn't do the last arm. Do a plan to fix this while I go to sleep that you do not need me."*

**Everything below needs no operator.** Where a headless run appears it is a DIAGNOSTIC
that reads an internal image the shader produces, never a verdict on the look — the
standing rule that picture claims go through the operator is untouched, and the reason for
it (a headless "it looks fine" is not evidence) is exactly why the experiments below read
counters and debug images instead.

---

## 0. What the session established, including the part that hurts

**The structural work landed.** At `CZ_VK_RT_DYN_SETTLE=0` — the configuration that before
items 1 and 2 would climb to the 1 GB cap and flush the lot — a real play session holds:

```
tlasInst=5001   blas=8029 (148.6 MB)  built=8058  flushes=0   outOfRange=0
```

`built` barely above `blas` is refit doing its job. That is items 0, 1 and 2 proven on the
operator's own machine, and it is the first time this configuration has been survivable.

**The picture defect is untouched by any of it.** The operator's verdict on the shipped
pair was *"the shadows was under them not placed in the right way and passing through
thing it shouldn't"*, and the settle-0 arm — which put the ACTORS into the structure for
the first time — doubled the shadowed share (18.5% -> 39.7%) and produced a **flat slab
with a hard straight boundary** crossing a shipping container, tyres, cars, a chain-link
fence and the ground without bending at any of them, plus a horizontal cut through Chuck's
chest.

**That is the same signature part 68 recorded**, and it has now survived, in order:

| part | what was fixed | signature after |
|---|---|---|
| 67 | the streams are object-space; every instance gets its world matrix | still a straight line |
| 68 | (diagnosis: the population) | still a straight line |
| 69 | the palette blend baked per vertex; identity, refit, direct buffers | still a straight line |
| 69 | the ACTORS admitted (`DYN_SETTLE=0`), `tlasInst` 2866 -> 5001 | **still a straight line** |

**Four rounds of occluder work have not moved it.** The honest inference is that the
defect is not in the occluder set at all. A boundary that is straight across surfaces at
different depths means neighbouring pixels are being given receiver positions that lie on
one plane — which is a statement about where the ray STARTS, not about what it hits.

## 0b. And the pre-registered gate failed, in a way that was my error

§6da §7 predicted the crowd-region edge density would FALL from ~100 toward the open-road
~10. Measured on the pair: crowd **87.0 -> 121.2** per 1000 px and open road **4.4 ->
10.2**. It rose.

The prediction's SIGN was wrong, not merely its value: several separate actor shadows
legitimately produce more boundary than one smeared blob, so that statistic could never
have distinguished "fixed" from "worse". That is gotcha 403 — an oracle that does not
resolve the question asked — committed one part after writing it down. **Retract the
edge-density gate; it is not a test of this fix.**

---

## 1. THE EXPERIMENT THAT SPLITS THE REMAINING SEARCH IN HALF

`rt_factor.hlsl` already carries the instrument, and it needs no operator and no capture:

* **`CZ_VK_RT_FACTOR_DEBUG=18`** — the primary ray's hit DISTANCE, `saturate(t / 500)`.
  A depth image made entirely of rays. Its own comment says why it exists: *"it separates
  'the rays hit something' from 'the rays hit the right thing': a TLAS full of junk
  geometry at the origin reads uniformly black here while mode 17 still passes."*
* **`CZ_VK_RT_FACTOR_DEBUG=17`** — `hit ? shadow : lit`, and the shipped GATE: no build
  goes to the operator until it lands near the all-shadow calibration (90.2) rather than
  the all-lit one (99.9).

**Run both at `CZ_VK_RT_DYN_SETTLE=0` on the headless outdoor route and LOOK at mode 18.**

* If it is a **plane or a near-uniform field** — the primary ray is resolving to one
  surface, and the receiver is the defect. Four parts of occluder work were aimed at the
  wrong subsystem, and §2 is the follow-up.
* If it is a **recognisable depth image of the junkyard** — fence in front, container to
  the left, buildings behind — the receiver is fine and the defect is downstream of it:
  the sun matrix, the ray bias, or the screen mapping of the factor into the 126 shaders.
  §3 is the follow-up.

Mode 18 at `DYN_SETTLE=120` is run beside it as the control, because the two differ by
exactly the population that was just added.

## 2. IF THE RECEIVER IS A PLANE — find the mesh that is standing in front of the camera

The likeliest mechanism is ONE ENORMOUS ADMITTED MESH. The bounds gate rejects a stream
whose extent exceeds `CZ_VK_RT_BOUNDS_CAP` (default 50,000 units) against a town that fits
in ~1,100 — so a 5,000-unit mesh passes the gate, is a hundred times the town's height,
and the primary ray hits it from every pixel. Admitting `dyn` at settle 0 adds exactly the
class §6cu named as the risk: *"junk-coordinate effect buffers, not world geometry"*.

1. **Sweep the cap** — `CZ_VK_RT_BOUNDS_CAP=5000`, `1000`, `200` — and watch mode 18 and
   `tlasInst` together. A cap at which the plane disappears and `tlasInst` barely moves
   names the population without any new code.
2. **Then name the members rather than the threshold.** Add a per-BLAS extent census to
   `PrintCollectorCensus` — the largest few meshes in the structure by world-box extent,
   with their stream keys. A threshold that works is a workaround; the named streams are
   the finding, and they are what a Case West port would need.
3. `tools/rt_tlas_census.py` can ask the same question offline over the twenty `.xtr`
   traces, but only for streams the traces carry — it cannot see the dynamic ones, which
   is precisely the population under suspicion. Prefer the runtime census.

## 3. IF THE RECEIVER IS THE REAL SCENE — the defect is downstream, and §6cy's exonerations are void

Gotcha 172: a retirement is only as good as the oracle it was measured on. The sun
direction, the ray length and the origin bias were all cleared **against a pile at the
world origin**, which is no test. They are open questions again and the arms exist:

* **`CZ_VK_RT_FACTOR_DEBUG=20`** — the hemisphere probe. Part 66 read 0.987 (nothing is
  occluded), part 67 read 0.650 after placement. Read it again at settle 0: a further fall
  says the structure keeps improving while the picture does not, which localises the
  defect past the trace entirely.
* **`CZ_VK_RT_SUN_FLIP=1`** — the sign arm. A straight boundary is what a sun pointing
  nearly along the view direction would give.
* **`CZ_VK_RT_FACTOR_BIAS` / `CZ_VK_RT_FACTOR_CAMBIAS` / `CZ_VK_RT_RAY_LEN`** — the origin
  offsets and the ray length, all derived from a cascade volume that was measured on the
  old structure.

## 4. What is owed regardless of how §1 comes out

* **Retract the edge-density gate in `phase5-notes.md` §6da §7** and record the operator's
  verdict, the settle-0 arm and its counters. The measurements stand; the prediction does
  not.
* **Retract the session script's note that `tlasInst` should be close between the arms.**
  It is 4828 against 2866 and that is BY DESIGN — the occurrence-ordinal identity creates
  more instances. The note was written before the measurement and never revisited.
* **`tools/part69_menu_flicker.sh` shipped an arm that never engaged**: `CZ_VK_RT=0` was
  written into the arm's DESCRIPTION and never passed to `env`, so the control ran with
  the RT device fully enabled. Fixed, and the script now REFUSES to report an arm whose
  log does not carry the line that proves it engaged. The lesson is gotcha 408.
* **The frame-rate reading has no denominator.** `18.0 fps median at 8,578 draws` was
  measured at settle 0 with RT on, and the two arms before it logged no frame rate at all.
  A headless RT-on/RT-off pair at a matched draw band bounds it; the operator's own
  machine is the number that counts, but a bound costs nothing.
* **The menu zombie flicker is unattributed.** The persist store's extra usage flag was the
  only part-69 change touching the raster path, and it is weakened: the store reports
  `memory type 3, heap 1` in both the part-68 and part-69 binaries, so the flag did not
  move it. The fixed two-arm script is ready and takes forty seconds.

## 5. What NOT to do

* **Do not add more occluders.** Four rounds of that have not moved the signature, and §1
  is designed to say whether the fifth would either.
* **Do not tune the bounds cap into a fix.** Use it to FIND the population, then name the
  members (§2.2).
* **Do not trust the edge-density statistic** (§0b) or the frustum test (gotcha 403) on
  this class of defect.
* **Do not re-buy** the placement, the transform table, the blend descriptor, or "a filter
  is eating the buildings" — all measured, all in `open-items.md` 0v.
