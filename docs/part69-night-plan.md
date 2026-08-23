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
* If it is a **recognisable depth image of the world** — the receiver is fine and the
  defect is downstream of it: the sun matrix, the ray bias, or the screen mapping of the
  factor into the 126 shaders. §3 is the follow-up.

Mode 18 at `DYN_SETTLE=120` is run beside it as the control, because the two differ by
exactly the population that was just added.

---

### THIS WAS RUN, AND THE ANSWER IS THE SECOND BRANCH. §3 IS LIVE; §2 IS NOT.

Headless outdoor route, `CZ_VK_RT_FACTOR_DEBUG=18 CZ_VK_RT_DYN_SETTLE=0`, with
`tlasInst=3366 blas=4503 (78.6 MB, built=4519, flushes=0)`. The image
(`~/DR2CZ-troubleshooting/part69-rt-geometry/mode18_primary_ray_settle0.png`) is **a
recognisable depth image of the world**: Chuck's silhouette in the foreground, both lamp
posts with their arms, the power lines strung between them, the gantry, the fence on the
right, the hills behind. Near is dark, far is light, and the octiles spread across the
range (59.0 / 2.9 / 0.2 / 1.2 / 0.4 / 2.4 / 2.5 / 31.4) instead of collapsing to two bins.

Part 68 read the same instrument class as *"a flat plain with distant buildings — no vans,
no wrecked cars, no fence, no Chuck"*. **The population work fixed exactly that.** The
structure is right and the receiver is right — which means the straight-line shadow
signature is not, and has not been for some time, an occluder problem.

**So do not spend the night on §2.** What is left between a correct receiver and a wrong
shadow is the SHADOW RAY, and §3 is where the work is.

## 2. NOT THE LIVE PATH — kept because it is what §1 would have needed, and because the bounds cap is still a real risk

**§1 came out the other way, so this section is not the work.** It stays written down for
two reasons: if a later reading of mode 18 in a DIFFERENT place shows a plane, this is the
follow-up ready to go; and §2.2's per-BLAS extent census is worth having regardless, since
nothing in this runtime can currently name the largest mesh in the structure.

### IF THE RECEIVER IS A PLANE — find the mesh that is standing in front of the camera

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

## 3. THE LIVE PATH — the receiver IS the real scene, so the defect is downstream and §6cy's exonerations are void

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

**Order them by what a straight boundary can and cannot be.** A boundary that stays
straight across surfaces at different depths cannot come from an occluder — occluders cast
shapes. It CAN come from:

1. **THE SUN'S Z FLIPS SIGN BETWEEN HEADLESS AND WINDOWED, and this is the first thing to
   run.** Censused over every run of this session, not argued:

   ```
   windowed (operator: bake, nobake, dyn0)   sun=(-0.364  0.546  -0.755)
   headless (v_final, seq, m18_settle0)      sun=(-0.371  0.557  +0.743)
   ```

   X and Y agree to about 2% and **Z is negated**. The runs are in different places and at
   different in-game times, so a moving sun is the alternative explanation — but a sun that
   moved would drift ALL THREE components, and two agreeing to 2% while the third flips
   sign is not that. (Both also carry `(-0.381 0.812 -0.443)`, which §6cw already names as
   the menu-era latch; that one is identical in both and is not the anomaly.)

   Why it matters more than its size suggests: **every headless RT measurement this feature
   has ever made was taken with the mirrored sun**, including part 66's 0.987 hemisphere
   reading, part 67's 0.650, and the whole of §6da. If the windowed value is the correct
   one, the offline work has been optimising against a light in the wrong half of the sky.

   `CZ_VK_RT_SUN_FLIP=1` is the arm. First establish WHICH is right, and the cheap way is
   not an arm at all: the cascade atlas is rendered by the title itself, so the shadow
   directions in the title's OWN raster shadows are the oracle. Compare the direction of a
   raster shadow in an operator F9 against the latched vector.

   The likeliest mechanism to check first is the per-frame slice VOTE (§6cw): if the window
   changes which cascade draws pass `IsShadowSurface`, a different slice can win, and the
   census above is one distinct vector per run rather than a drift — which is what a vote
   flipping looks like, not what a decomposition sign error looks like.
2. **An origin bias large enough to lift the ray clear of everything nearby**, which §6cw
   already recorded happening once: a bias derived from a 3587.7-unit volume put the origin
   5.4 world units off the surface. Re-read `bias=` in the `[rtb]` line at settle 0 — the
   operator session shows `len=88.5 bias=0.133/0.044`, which is small, so this is second.
3. **A ray length that stops short**, leaving everything past it lit — the opposite
   signature, so third.

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
