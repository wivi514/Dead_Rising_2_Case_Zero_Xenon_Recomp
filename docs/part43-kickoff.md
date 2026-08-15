# Part 43 kickoff — the zone texture-set DECISION: name its inputs, fix it, A/B it

Written at the end of part 42 (2026-08-14). **This is the LIVE hand-off**,
superseding `part42-kickoff.md`. Read `docs/phase5-notes.md` §6bv (+ addendum)
first — part 42 collapsed both of part 42's kickoff items into ONE mechanism
with the engine's own name on it.

## What part 42 established (do not re-derive)

* **Item 00i is the per-zone `COMMON_TEXTURE.tex` vs `COMMON_TEXTURE_LOD.tex`
  choice.** At the DebugJump spawn our runtime loads zones 1/2/3/7 as LOD
  (thumbnail atlases — the patternless buildings) and 0/5/8 as full. Hardware
  shows the same street fully textured in all eight R4 traces (0 tiny-on-big
  bindings vs our 912 across the walk). The complaint is verified PRE-post-chain
  (the 003053 building is flat in the scene surface itself).
* **The decision is a load-time threshold, not a rate**: 20 stationary censuses
  over 3.6 minutes promote NOTHING (gotcha 312). `KeSetBasePriorityThread` and
  file-IO latency are REFUTED as this item's mechanism — do not build them for it.
* **`ForceLODTexForStreamingWorld` is 0 on our side** (name-resolved config flag,
  byte `0x82A57BD7`, read live from the running process). Not the mechanism.
* **Item 0u is DOWNGRADED** — our DoF constants match hardware's to the digit
  (`CZ_VK_PSBIND_PC=48,49,81,82,...` on any census run prints them), the alpha
  math gives ~95% blur at 50 m on BOTH platforms, and hardware's own R4 PNGs are
  soft at range. Part 41's "hardware contradiction" was 00i's texture damage read
  as blur (gotcha 313). Only two residues remain (below).

## The plan

1. **Name the decision's inputs.** `gdis.py --find-uses` on the format string
   `"<> LoadZoneCommonTextureSet : zone = %d..."` (find its address with the
   §6bv method) → the caller → the branch that picks the filename. Read the
   branch's inputs (zone distance? memory budget? `cZone::UpdatePriorities`'s
   `mForceLowLOD`? `mNumVolumes`?). Each candidate input is ONE live
   `process_vm_readv` against a running stand-still process — the part-42 method,
   minutes per theory.
2. **Find the divergent input.** Whatever the branch reads, ask why it differs
   from hardware: an HLE return (grep `kernel/imports.cpp` for the producer), a
   config default, or a computed distance. `import_call_sites.py` if it smells
   like a kernel input (finding 29: the caller is the spec).
3. **Fix ONLY the input, never the branch** (gotcha 5 — no faking the decision),
   with a same-binary arm (`CZ_ZONE_LOD_*` or whatever fits) and a counter that
   proves engagement.
4. **Gate**: the part-42 statistic is the yardstick — tiny-on-big bindings for
   `ps_34524bb64374d20e` on the stand-still recipe (currently 6 from 3 addresses,
   20/20 censuses) must go to ~0 with the zone narration flipping 1/2/3/7 to
   full; then the walk-era census (44/82 frames currently) and one operator look.
   Watch the heaps while doing it: the full sets cost real memory, and "Out of
   memory in the load & decomp heap!" is the assert to watch for (it has NEVER
   fired — keep it that way).
5. **The 0u residues, when 00i is done**:
   * serve the DoF gather's fmt6 depth-as-8888-bytes fetch a packed byte view
     (part-42 kickoff step 3's design: D32F → RGBA8 24_8 at snapshot time, own
     arm + counter). Edge-weight correctness; predicted visible effect small.
   * the gather's pc255.x (taps' depth threshold): ours is 0. If the byte view
     lands, re-derive what the shader expects and whether 0 is even wrong.

## Standing state

* Cache **435**, dim gate clean; four never-reported shaders recovered by the
  name-diff gate plus `vs_c8e86dffb37149dd` via a XenosRecomp emitter fix
  (empty-dest-mask vfetch — `docs/xenonrecomp-upstream-bugs.md`, XenosRecomp
  commit 597855c). The batch's per-shader failure list is worth reading EVERY
  rebuild.
* `xtr_draw_constants.py` prints load provenance on UNRECOVERABLE registers
  (`UNRECOVERABLE@addr`); the DoF block loads from CPU-written `032B6000`.
* Part-42 capture sets under `~/DR2CZ-troubleshooting/part42/`: `fan/` (8-yaw
  F9 fan at spawn), `standstill/` (20 fixed-camera censuses), `dofconst/` (the
  full DoF register census), `diag/` + `/tmp/part42_diag.log` (the narrated run
  — the log is in tmpfs, copy it out if it matters).
* Items still open from part 41's plan, parked: A2M dither at distance (item 4),
  clamp modes / cyan fringes (item 5). The 81-capture walk and 20-capture
  part-41 sets remain the picture dataset for any A/B.
