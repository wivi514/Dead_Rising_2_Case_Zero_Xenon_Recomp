# Part 45 kickoff — item 00i is CLOSED (faithful); the freeze is the top item

Written at the end of part 44 (2026-08-15). **This is the LIVE hand-off**,
superseding `part44-kickoff.md`. Read `docs/phase5-notes.md` §6bx (and its
addendum) first — part 44 ran the part-44 hunt's control and the premise
inverted twice.

## What part 44 established (do not re-derive)

* **Item 00i is CLOSED AS FAITHFUL.** Two censuses:
  * The MENU premise of the part-43 reframe failed its control: our menu F9
    census equals B1's title era **bind for bind** (148/41/37/31/27/20/20/17/
    12 …), including the 31 tiny-on-big draws — `flat_color_gray_cm.bct`,
    346 bytes, flat by design. The "56 flat menu draws" were always
    hardware's own numbers.
  * B2 — a FRESH hardware session walking into Still Creek, on disk since
    2026-08-04 — carries the operator's flat-at-range class through the whole
    town era: **2–3% tiny-on-big per bucket, 1,300–3,900 world-shader draws
    on an 8×8**, persisting to the end of the session (no promotion wave).
    Ours reads 0.2–4.9% on the same census. Fresh-vs-fresh, we match; the R4
    zero-tiny street was a warm loaded-save session.
* **The texture level machine is fully named and healthy on our runtime**
  (§6bx): name hash (extensionless basename, init 0x20225, poly 0xEDB88320),
  texture DB (entries 0x4C stride: +0x38 asset slot, +0x3C/+0x40 per-level
  refcounts, +0x44 current level, level 0 = full set payload, level 1 =
  thumbnail), the promote walk `sub_82268840`, the bind `sub_82268238`
  (vtable-dispatched), registration `sub_8222CC80`, the catch-up queue
  (`sub_82206910` → type-8 → `sub_82268A10`), set completion `sub_82269388`
  binding at the zone's LOD verdict (`[volObj+0x22C]`). 39 walk-scheduled
  payload ops verified against their archive reads; shared atlases observed
  promoting live (`f44=1 → 0`) when a full zone references them.
* **`CZ_SET_APPLY_PROBE=1`** (guest_probe.cpp) prints every walk, lookup,
  registration, bind and payload op. The 35 menu-set hashes are compiled in
  and print from any caller.
* **Big-trace censuses need the lean tool**: `xtr_draw_bindings.py` OOMs
  three different ways on the 8.5 GiB B2 (§6bx addendum). The validated lean
  variant (rolling memory window + streamed CSV rows, byte-identical on B1)
  is in `~/DR2CZ-troubleshooting/part44/` with all part-44 artifacts. Never
  point a multi-GB CSV at /tmp — the tmpfs filled mid-run and the shell's
  every command "died" until space was freed (the known quota signature).
* **Operator guidance**: compare like states only — fresh vs fresh, or same
  save vs same save. Their two flat-building reports (menu gas sign, Big Buck
  approach) both reproduce on hardware in the matching state.

## SAME-DAY REWRITE — 00i REOPENED, and the mechanism is cornered to a MIP-SELECTION OVERSHOOT

The operator refused the closure, recreated the R4 walk on our renderer, and
was right (§6bx second/third addenda — read both): the flat buildings bind
FULL-SIZE textures (8 tiny draws of 1,760 at the matched view), so the
thumbnail story was the wrong mechanism. The chase then established, same day:
mips are the mechanism (`CZ_VK_NO_MIPS=1` restores the buildings, tail arm
does not, scene surface carries the flatness); the mip DATA is correct (guest
chains AND uploaded staging bytes verified level by level); every LOD input
matches hardware (tfetch bias 0×1,846; fetch-constant bias 0×56,111; filters
and aniso equal and ENGAGED); and the new `CZ_VK_MIP_TINT=1` instrument shows
a ~2-octave global overshoot (L1 at two meters, L2 at ten, fence anchor: L2
where texel math says 0.5). E5 proves hardware-fresh is crisp. The defect is
ours, in the scene pass's LOD/derivative environment or in the still-unnamed
wall-texture fetch state.

## The plan

0. **Item 00i, reopened — the selection-overshoot hunt:**
   * `CZ_VK_DRAW_ID` at the operator's matched Big Buck view (pose in
     `part44-operator/capture_008693.pose`): NAME the wall draw and its
     texture (every candidate so far was picked by inference — gotcha 302).
   * Compute hardware's implied LOD for that draw from the R4_04 trace's
     vertex streams (UVs + screen extent) and diff against the tint reading —
     the octave delta becomes a hard number.
   * Audit the scene pass's derivative environment: the MSAA window-scale
     interplay (§6bf/§6bh) is the one global term that could scale every
     gradient; check what pixel grid the scene REALLY rasterizes at vs the
     guest's window-transform expectations.
   * The mip-tint contact points: tint + `CZ_VK_ANISO=0` and tint +
     `CZ_VK_NO_FETCH_SAMPLERS=1` at one fixed view quantify each sampler
     term's share of the overshoot.

1. **The sledgehammer FREEZE is the operator-session item** (part-43 operator session:
   signal-15, not a fault — input/audio still pumping, right after grabbing
   a sledgehammer outside the safehouse in natural play). Next operator
   session carries `CZ_WAIT_TRACE=1` so the freeze names its wait; log at
   `~/DR2CZ-troubleshooting/part43-operator-zone-session.log`.
2. **The 0u residues** (parked since part 42's kickoff):
   * serve the DoF gather's fmt6 depth-as-8888-bytes fetch a packed byte view
     (D32F → RGBA8 24_8 at snapshot time, own arm + counter). Edge-weight
     correctness; predicted visible effect small.
   * the gather's pc255.x (taps' depth threshold): ours 0, hardware
     unrecoverable (CPU-written `032B6000`). If the byte view lands,
     re-derive what the shader expects.
3. Parked from part 41: A2M dither at distance (item 4), clamp modes / cyan
   fringes (item 5).
4. **Optional curiosity, operator-dependent**: load their TANKER save (the
   R4 session's) on our runtime with `CZ_SET_APPLY_PROBE=1` +
   `CZ_ZONE_TEX_PROBE=1` to name what a save carries that makes a warm
   session all-full (skip bits remain the candidate). Not a defect either
   way; do not spend headless time on it.
5. The AO-only-up-close observation: re-check like-for-like before treating
   it as an item (plausibly the same far-LOD design).

## Standing state

* Cache 435, dim gate clean. Part-44 artifacts in
  `~/DR2CZ-troubleshooting/part44/` (probe logs, menu/outdoor censuses,
  B1/B2 binding CSVs, the lean census tool, timeline analysis). The B2 clean
  census may still be finishing when part 45 starts — its CSV lands at
  `~/DR2CZ-troubleshooting/part44/b2_bindings.csv`; the truncated run's
  numbers in §6bx are from the first 90% of the trace and 23 full buckets.
* New instruments: `CZ_SET_APPLY_PROBE=1` (see `docs/instruments.md`).
* Two commits this part: the probe + level machine + menu retraction
  (857cbe6), then the B2 verdict + closure (this one).
