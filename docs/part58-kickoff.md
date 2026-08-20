# Part 58 kickoff — the clip-plane SPACE, the R6 trace, and the flicker arm in waiting

> **THIS IS THE LIVE HAND-OFF**, superseding `part57-kickoff.md`. Part 57 was two operator
> sessions and the analysis between them: user clip planes went in (the slicing doubling is
> FIXED), the GAS sign was attributed and its shader exonerated, and the decal "drop"
> verdict was reversed by one census field. The full record is `phase5-notes.md` §6cm; the
> per-defect state is `open-items.md` 00p.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** — they said so in part 57 ("do not
> run it headless, let me do it and tell me what you need for the run") and the session
> scripts are built for it: `tools/part57_operator_session.sh` (four chained arms) and
> `tools/part57_followup_session.sh` (one arm, self-firing texture dump on every F9).
>
> Performance stays PARKED in `docs/perf-state-parked.md`.

---

## 0. THE STATE, one line each

| item | state | next action |
|---|---|---|
| GAS sign / distance class | shader faithful; guest-memory CONTENT of far-LOD textures is the suspect | **R6 hardware trace** (filed, blocking) |
| decal flicker | draws issued every frame; title triple-buffers; defect did not fire in part 57 | when it next shows: same spot, `CZ_VK_NO_PARALLEL_GUARD=1` A/B |
| zombie slicing | halves separate; cut see-through + thin doubled slab | derive the dot SPACE from captured planes + poses (NOT an epsilon) |

## 1. THE CLIP-PLANE SPACE — an OFFLINE derivation, the data is already on disk

The plug seals the cut on hardware and is clipped away by us; CZ_VK_CLIP_BIAS=0.01
over-corrects into no clipping at all, so the whole body spans <0.01 of a plane's
magnitude in clip distance — a razor margin where a systematic space error lives.

**The data**: `~/DR2CZ-troubleshooting/part57-operator/clip_slice/` — ten F9 captures,
each with a census (per-draw `ucp=` plane 0 + `v0=`/`va=` vertex anchors) and a pose
(the view-projection windows, `tools/pose_read.py`). Plus `clipdraw_f009266.regs`, a
whole register file at a clip-enabled draw.

**The method**: the game draws each sliced body twice with near-complementary planes
(part 56 measured the pairing NOT exact in x/y — which, since negation survives any
linear transform, means the planes genuinely differ, probably an intentional overlap).
Candidate spaces for the hardware dot: raw clip (what we do now), post-divide NDC,
view space via the pose's V matrix, world. For each candidate, transform the captured
planes and ask which one makes the two copies' kept half-spaces PARTITION a body-sized
interval with a plug-sized overlap — the coherence method that located the stencil
layout and the plane registers themselves (gotcha: dump and look for the SHAPE).

Wrong-space signatures the operator can also check in one launch: if the cut edge
SHIFTS as the camera orbits a fallen piece, the space is view/clip-coupled when it
should not be.

## 2. R6 — the far-sign hardware trace (filed in xenia-capture-requests.md)

One single-frame .xtr at the part-56 far viewpoint. It decides content-vs-pipeline for
the whole distance class (sign, canopy, bunting — all sample tiny sub-tile tiled DXT1s).
Until it lands, do NOT buy any theory about the tiny textures' bytes: our live dump
shows garbage, the decoder is validated, and the asset-file comparison was null in every
endianness (per-record compression inside .tex could hide a match — cracking the .tex
container format is the self-serviceable fallback if R6 is slow to arrive).

Read it with `tools/xtr_draw_bindings.py` against `ps=86ac6569ea0d700d` (letters) and
`ps=57d441f53fc93ad7` (disc/batch): compare hardware's s0 BYTES at the letters draw with
our dump `followup_2016/texdump_f9073/109EF000_32x16_fmt18.bin`.

## 3. THE FLICKER ARM IN WAITING

Nothing to build. The burst census now carries va= and burst_read.py folds the
triple-buffer ring out before concluding. The moment the operator sees a decal flicker
again: F8 burst at it (camera moving), then the same spot with
`CZ_VK_NO_PARALLEL_GUARD=1` — if the flicker stops, the frame-ahead guard against the
3-frame ring is convicted; if not, the depth/stencil/alpha state of the decal draws is
next, and the census from the same burst carries all of it.

## 4. WHAT PART 57 ADDED (tools and arms)

* **Burst per-draw census** (`CZ_BURST_CENSUS`, `CZ_BURST_CENSUS_EVERY`), census fields
  `v0=` and `va=`, ring-aware verdicts in `burst_read.py`.
* **User clip planes**: `XE_USER_CLIP_PLANES` cache arm (`assets/shader_spv_clip`,
  `assets/shader_spv_clip_a2m`), `CZ_VK_NO_CLIP_PLANES`, `CZ_VK_CLIP_POISON`,
  `CZ_VK_CLIP_BIAS`, per-plane publish counters, shaderClipDistance device feature.
  XenosRecomp local patch (committed there) — a SHARED gap: Fable 2 lacks it too.
* **Session scripts**: `part57_operator_session.sh`, `part57_followup_session.sh`
  (the self-firing live_texdump watcher pattern — keep it in every capture session).
* The null-cache rebuild diff caught `vs_c17bbebf65383249` missing from
  `assets/shader_spv` — added; **the cache is 440**.

## 5. GATES

Part 57's runtime verification ran WINDOWED through the operator: Vulkan validation on
the clip cache = **zero non-topology lines** (arm 1, 3.5 M poisoned draws), cache misses
0 in every arm, clip publish counters engaged (750,632 draws in the slicing arm). The
smoke gate passes. The headless gate sweep (E3, A5, PM4 oracles, dimension census) was
NOT re-run this part — the renderer changes are inert without the clip cache selected,
and the operator's instruction stands; run the sweep when a part next changes the
default path, or ask them for one gate session.
