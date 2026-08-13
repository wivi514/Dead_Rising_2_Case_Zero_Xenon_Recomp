# Part 39 hand-off (for part 39). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part38-kickoff.md` for "where
the port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

Parts 37-38 (one day) closed three renderer defect classes: the lightmap-UV
transposition (the striped blotches — mask now zero), the stale texture cache (the
random-texture class — revalidate now default), and the never-driven RB alpha test
(now wired). Operator-confirmed across a full evening. Two picture items remain
cornered with hardware ground truth ON DISK: the flat-panel LOD look (item 00i, now
top — eight paired R4 traces) and the shard trees (item 0t — alpha-to-mask suspected,
R4 carries the register state to prove it).

## WHAT PARTS 37-38 DID — do not rebuild any of this

Records: `phase5-notes.md` §6bo (part 37) and §6bp (part 38); gotchas 291-294;
`port-history.md`. Evidence: `~/DR2CZ-troubleshooting/part37-headless/` and
`part38-operator/` (both indexed); hardware: `Xenia logs/R4_world/` (eight
frame-locked Big Buck traces + PNGs + 261-shader dump), R3_world unchanged.

* `g_SwappedTexcoords` = 0 default (`CZ_VK_TEXCOORD_SWAP=1` repaints the blotches).
* Texture guard+revalidate = default (`CZ_VK_NO_TEX_REVALIDATE=1` brings back the
  random textures). Headless frame-time A/B of the guard's cost is OWED.
* RB alpha test = default (`CZ_VK_NO_ALPHA_TEST=1` arm); unknown compare funcs are
  counted by name. Foliage does NOT use it.
* Window-close now dumps renderer counters; a long operator session finally reports.
* The headless defect loop (DebugJump + F9 in the press sequence + live_texdump) and
  the R4 capture round-trip are the working method — the whole of part 38 ran on them.

## WHERE TO START

1. **Item 00i — the flat-panel LOD look, top picture item, oracle on disk.** Pair one
   far building between our F9 series (`part38-operator/arm1b_revalidate/` frames
   9965-10986, censuses + live texdumps) and the R4 trace of the matching viewpoint.
   Pair by texture CONTENT (gotcha 291 — never by vertex count). The question: at the
   range where we draw flat color, what does hardware bind — a texture level we never
   uploaded, a fetch slot we read as unset, or constants? `xtr_draw_bindings.py` on
   `Xenia logs/R4_world/Big_buck_hardware_store_0N/…xtr`.
2. **Item 0t — the shard trees.** First read RB_COLORCONTROL at hardware's foliage
   draws from an R4 trace (small xtr-tool extension: track register 0x2205 per draw
   and print it beside the bindings; the foliage draws bind DXT5+DXN pairs, e.g. our
   ps_c9ca4f73ba93d023 family). If it says ALPHA_TO_MASK (bit 4), emulate as a
   threshold or dithered discard (no real MSAA on our side) behind a new pipeline-key
   bit, same shape as the part-38 alpha-test wiring. Target look: R4's PNGs.
3. **The guard-cost frame-time A/B** (three runs an arm, alternated, null first —
   `docs/measurement.md`): the revalidate default was shipped on correctness; the
   number is owed. Quote medians and the pinned-share, not means (gotcha 237).
4. Item 0s residue (16 small colour resolves / resolve write-back; 231 depth-fed
   fetches) and the parked teleport (part38-kickoff item 4 has the 0x6A state).
5. `docs/perf-cpu-plan.md`'s CPU/GPU overlap — still the largest performance item.

## READ THIS BEFORE MEASURING ANYTHING

* Everything from parts 26-36 stands, plus gotchas 291-294.
* **Pair draws across platforms by texture CONTENT, never by vertex count** (291) —
  it produced a street chunk and a severed head before it produced the tanker.
* **A localized surface defect is invisible to whole-frame statistics** — crop the
  surface at a matched F9 index; era medians need matched camera paths (254).
* **A staleness rate is a fact about its workload** (293): anything that scales with
  session length or area coverage must be censused on an operator-length session.
* The part-38 operator logs (`arm1.log`, `arm1b.log`) predate the stats-on-close fix
  — their counter dumps are MISSING, not zero. Do not read absence as absence of
  engagement.

## Gates, on this binary (part-38 final: commit 91f5bca)

* `--smoke` OK after every change. Part-37 gates (A5 exit 0 / 3 permutation / 0 real;
  E2 identity +0.9594) were taken two commits earlier on the same day; re-run before
  any claim resting on them.
* The alpha-test and revalidate defaults have NO headless picture gate yet beyond the
  operator's session — the part-39 A/Bs above are that gate.
