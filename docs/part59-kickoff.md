# Part 59 kickoff — the owed gate sweep, R6, and a quiet watch list

> **THIS IS THE LIVE HAND-OFF**, superseding `part58-kickoff.md`. Part 58 was one offline
> derivation, one operator ladder, and one one-arm A/B, and it FIXED the zombie-slicing
> see-through: the kickoff's clip-space hypothesis was refuted (88/88 captured planes are
> unit view-space normals — `tools/clip_plane_space.py`), the shift ladder closed the
> whole clip branch, and the captures found the real mechanism — the gore cap is a
> stencil-masked quad, the mask is written by TWO-SIDED stencil, and our hardcoded
> CCW front was inverted relative to hardware. FRONT=CW is now the default
> (`CZ_VK_STENCIL_CCW_FRONT=1` is the control arm). Operator: *"Yes it is perfect now."*
> Full record: `phase5-notes.md` §6cn; per-defect state: `open-items.md` 00q;
> gotchas 370–372.
>
> **ALL RUNTIME VERIFICATION GOES THROUGH THE OPERATOR** (their part-57 instruction,
> reaffirmed by practice in part 58 — three windowed sessions, zero headless game runs).
>
> Performance stays PARKED in `docs/perf-state-parked.md`.

---

## 0. THE STATE, one line each

| item | state | next action |
|---|---|---|
| headless gate sweep | **OWED** — part 58 changed a DEFAULT (front=CW), the named re-run condition | ask the operator; 2 commands, ~10 min (see §1) |
| GAS sign / distance class | unchanged from part 57: shader faithful, far-LOD texture CONTENT suspect | **R6 hardware trace** (filed, blocking) |
| decal flicker | unchanged: issued-and-lost, title triple-buffers, did not fire in parts 57–58 | when it shows: F8 burst + `CZ_VK_NO_PARALLEL_GUARD=1` A/B |
| doubled slab (slicing) | NOT re-observed in any part-58 session | watch only; re-open on a sighting (F9 + immediate F8) |

## 1. THE GATE SWEEP (first ask)

Part 57 skipped the sweep because its changes were inert without the clip cache; part 58
is the opposite case — `frontFace` changed for EVERY pipeline. The expected blast radius
is nil (facing's only consumer is two-sided stencil, and the fix is hardware-correct by
experiment), but "can't affect" claims are how defects hide. The sweep, when the operator
green-lights it (it is headless game runs, their call):

```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 ... )   # E3 correlation — CLAUDE.md Commands
python3 tools/kernel_call_diff.py --xenia "Xenia logs/A5_highfreq_boot/cz_run5.log" \
    --ours /tmp/run.log --include-high-frequency        # A5, exit 0 expected
# both PM4 capture oracles, and:
python3 tools/shader_dim_census.py                      # 0 disagreements expected
```

Also owed from part 56 (never run): the sweep on the final stencil build — this one
covers it, same binary lineage.

## 2. WHAT PART 58 ESTABLISHED THAT A FUTURE SESSION WILL WANT

* **`tools/clip_plane_space.py`** — the space derivation. The reusable trick: a clip
  plane moves to view space through Projᵀ ALONE (no view matrix), and view space is
  rigid, so "is the normal unit?" is a space test and plane-boundary shifts are in true
  meters. It also fitted the actual scene camera: **fov 42.98°, 16:9, zn 0.1, zf 1000**
  (the pose first-draw 45° matrix is some other camera).
* **The slicing technique, fully decoded** (§6cn §4/§6): per piece and per tile —
  two-sided stencil+depth write (body plane, ref 0xB0+i) → visible gore paint (a SECOND
  plane ~40° off, ref 0xAC+i, same ps/textures as the body) → body prepass → body color
  at EQUAL → later, a 6-vert cap QUAD stencil-EQUAL against the written ref. Nothing
  else in this game consumes facing (su=00080008 on every draw; culling NONE).
* **Arms**: `CZ_VK_CLIP_SHIFT=<m>` (meters-true plane translation; the ladder
  0/+0.05/+0.02/−0.02 read "no change" everywhere → clip branch closed),
  `CZ_VK_STENCIL_CCW_FRONT=1` (pre-part-58 facing, the control arm). RETIRED:
  `CZ_VK_CLIP_BIAS` as a probe (it rotates — gotcha 370), `CZ_VK_STENCIL_FLIP_FACES`
  (was the experiment arm, now the default, unread).
* **Session practicalities**: severed pieces despawn in ~3 s and there is no
  corpse-persistence tunable among the 393 — F9 liberally plus an immediate F8 burst is
  the analysis route for anything on a corpse. And a "visible positive control"
  prediction can be wrong in PRESENTATION while the arm works (the ±0.05 shift arms:
  ragdoll halves separate, so 5 cm more per piece reads as "the same") — the counters
  were the engagement evidence, which is why every arm carries one.

## 3. CANDIDATE AGENDA BEYOND THE TABLE

The three big reported picture defects are now: sign (blocked on R6), decal flicker
(waiting for a sighting), and whatever the operator reports next — part 58 closed the
slicing item, so ASK THE OPERATOR what they want next before picking from
`open-items.md`'s older backlog (shadow cascade, NPC part meshes, colour-grading LUT are
the standing candidates there, all pre-dating the post-chain fix and worth a re-look on
current builds).
