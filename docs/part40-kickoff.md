# Part 40 hand-off (for part 40). Paste this into a fresh conversation.

`CLAUDE.md` loads automatically. This file supersedes `part39-kickoff.md` for "where
the port is".

**Check the git log against this file before working an item** — gotcha 13.

## The one-paragraph state of the port

Part 39 found and implemented a whole input the renderer had been discarding since
phase 5: **the guest's mip chain**. A Xenos fetch constant names a second address for
mip levels 1..n; we uploaded one level and sampled it at every distance. Hardware
declares chains up to nine levels deep on the majority of its fetches. The chain is
built, its layout verified against hardware's own bytes, the packed tail declined and
counted, `CZ_VK_NO_MIPS=1` the control arm. Item 00i's level-0 input was exonerated by
an md5-identical content pairing against R4 first, which is what pointed here. Item 0t's
suspect (ALPHA-TO-MASK) is **refuted** across 40,703 hardware draws and the item now
needs one capture, not an investigation.

## WHAT PART 39 DID — do not rebuild any of this

Records: `phase5-notes.md` §6bq; gotchas 295-297; `port-history.md`. Commits 40deac8,
b8b856f, 6a24b9e and the wrap-up.

* **The mip chain uploads by default.** `CZ_VK_NO_MIPS=1` restores the pre-part-39
  renderer. Counters: `mip: chain uploaded` (1,815 textures on the outdoor route),
  `mip: PACKED TAIL DECLINED`, `mip: CUBE chain not uploaded` (6).
* `xenos::TextureFetch` now carries `mipAddress` and `packedMips`; both censuses (ours
  and `tools/xtr_draw_bindings.py`) print `mip=lo..hi mipAddr=…`, so the two are
  diffable in one vocabulary as they were designed to be.
* `xtr_draw_bindings.py` also carries **RB_COLORCONTROL, RB_ALPHA_REF,
  RB_BLENDCONTROL0 and RB_DEPTHCONTROL** per draw, decoded in words.
* **SIGTERM/SIGINT dump the renderer counters** — every headless recipe here ends with
  `timeout`, and that path printed nothing (gotcha 297).

## WHERE TO START

1. **FINISH THE MIP A/B AND SAY WHAT IT MEANS.** Part 39 registered the prediction
   (distant surfaces gain filtered detail; era-median distinct colours move beyond the
   null) and ran an alternated three-runs-an-arm block; read `docs/measurement.md` and
   `tools/frame_era_medians.py --null B1 B2 --arm A`. **If it is unmoved, say so and
   retract the 00i link in `open-items.md` 00i** — the change is still correct (the
   guest declared data we discarded), but it would not be that item's mechanism.
2. **THE PACKED MIP TAIL**, which is where the deepest minification lives and therefore
   where a distant building most needs a level. Levels below one tile share a tile at
   sub-tile offsets this code declines. The oracle method is worked out and cheap:
   pull the chain out of an R4 trace (`xtr_mem`-style raw read + `tools/tex_decode.py`)
   and accept an offset only when the mean holds while distinct colours fall. Verified
   data points to fit against are in §6bq's table.
3. **Item 0t is now a CAPTURE REQUEST**: one single-frame F4 trace **standing at a
   shard tree** (the operator's `capture_f28446` location). `ps_c9ca4f73ba93d023` is
   absent from R4's 261-shader bank, so R4 cannot answer it. Add it to
   `docs/xenia-capture-requests.md` and ask.
4. **The guard-cost frame-time A/B** for the part-38 revalidate default — still owed,
   still three runs an arm, medians and pinned-share not means (gotcha 237).
5. `docs/perf-cpu-plan.md`'s CPU/GPU overlap — still the largest performance item.

## READ THIS BEFORE MEASURING ANYTHING

* Everything from parts 26-38 stands, plus gotchas 295-297.
* **For every field a decoder parses, grep for a READER** (295). The mip fields sat
  parsed and unread for thirty-four parts and presented as "hardware must not use this".
* **A count that saturates is a question about its emitter** (296). 324 of 324 of our
  pixel shaders "contain a discard"; hardware's own microcode says 1 of 208.
* **Both texture dumpers size themselves `w*h*bpp`**, which is short of a TILED
  surface's pitch-and-rows-rounded footprint — the part-39 md5 pairing compared 8 KB of
  a 16 KB footprint on both sides. It holds because both truncate identically, but
  neither `--dump-texture` nor `live_texdump.py` writes a whole tiled texture. Worth
  fixing before the next content pairing.
* A matched-frame outdoor picture A/B is still unsatisfiable (gotcha 254); era medians
  with a null measured from the same block are the instrument.

## Gates, on this binary (part-39 final)

* `--smoke` OK after every change (run, green).
* `CZ_VK_VALIDATION=1` on the outdoor route after the mip change: **no new messages** —
  the only ones are the pre-existing `topology-08773` point-size trio.
* **OWED and NOT re-run in part 39**: A5 kernel-call gate, both PM4 capture oracles,
  `truncated=0`, the capture-E identity correlation, and the shader-cache name diff.
  None of part 39's changes touch the CP, the kernel or the shader cache, but the
  standing gates should be re-run before any claim rests on them.
