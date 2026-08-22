# Measurement and analysis recipes

**Split out of `CLAUDE.md` on 2026-08-08.** The commands for judging a change rather than
making one: the renderer A/B, the picture checks against capture E, the capture oracles
for the command processor, the guest disassembler and the `.xtr` tools.

The rule they all serve is in `CLAUDE.md`'s evidence section and in `docs/gotchas.md`:
**a single run of an unvalidated metric is not a measurement.** Two of phase 5's three
"measured improvement" claims were noise from an animated title screen.

A/B the renderer. **This is the only sound way to claim a renderer change helped** —
two of this phase's three "measured improvement" claims turned out to be noise from the
title screen's ANIMATED 3D background, because a single run of an unvalidated metric is
not a measurement (gotchas 50/51/86). Aggregate over the era; never compare by frame
index (gotcha 38):
```
for a in base arm; do
  (cd runtime/build && CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_VK_FRAME_STATS_SURFACE=0684B000 \
      CZ_VK_FRAME_STATS=/tmp/$a.txt timeout 85 ./cz_runtime >/dev/null 2>&1)
done
python3 tools/frame_compare.py /tmp/base.txt /tmp/arm.txt
```
Baseline band is **1.36 pp** of median surface coverage over five runs of one binary;
the tool calls anything inside 1.5 pp "no detectable difference". It has been shown
capable of failing (gotcha 30): `CZ_VK_PRIM_RESTART=1` reads 17 pp outside the band.

A/B the FRAME TIME, which is a different question and needs a different tool. The
recipe that reaches a crowd is 57 fixed 8-second steps against a boot whose depth in
wall time is a distribution (gotcha 75), so two runs of one binary spend different
amounts of time in each place and a whole-run mean is dominated by the ~1,900-draw
safehouse era — where the title's own two-vblank pacing pins the frame at 31 fps and a
CPU saving measures as exactly zero. Compare frames BINNED BY DRAW COUNT instead: a
6,000-draw frame is the same workload whenever in the run it arrived.
```
python3 tools/frame_perf_bins.py /tmp/armA_stats.txt /tmp/armB_stats.txt
python3 tools/frame_perf_bins.py /tmp/armA_stats.txt          # one arm, just the profile
```
**The floor is 10-13% per crowd bin at one run a side, and that is measured rather than
assumed** (gotcha 229). Run against a genuine null arm — part 20's profiler fix, which
changes only arithmetic inside the instrument — individual crowd bins moved 10-13%, with
the tool's own standard-error column reading up to 22 sigma. Frames inside a bin are not
independent samples: consecutive frames share a camera, a location and a thermal state,
so the effective N is a small fraction of the frame count. **Run the null comparison
before believing a frame-time result**, and use three runs an arm, alternated
a/b/a/b/a/b, pooled with `--a`/`--b`. Part 20's worked example: arm A 55.30/53.00/53.17
against arm B 48.35/48.51/46.89, −11.0% with no overlap between the arms.

The pre-part-20 binary, for reference. **Sample the GPU clock rather than pinning it**
— `tools/gpu_clock_sample.py --csv /tmp/clk.csv -- ./cz_runtime ...` — and quote what it
was; this workload governs itself to P5/~524 MHz at 32% utilisation, and the P8/210 MHz
this project used to quote was a blanked monitor, not a defect (gotcha 219's retraction,
gotcha 231):

| draws | 0-999 | 1000-1999 | 2000-2999 | 4000-4999 | 5000-5999 | 6000-6999 |
|---|---|---|---|---|---|---|
| ms/frame | 32.22 | 32.67 | 35.51 | 46.03 | 49.70 | 52.84 |

The flat pair at the top is the title's pacing cap and the climb below it is the
workload, which is why the high bins are the only admissible place to measure a CPU
change.

Check a frame against capture E, and NAME the transform if it is one. This is the
instrument the phase-5 blind spot needed: the frame was rendered vertically mirrored for
a whole phase and no aggregate could see it (gotcha 135). Exit 1 = the frame is
transformed:
```
python3 tools/frame_signature.py \
    --ref "Xenia logs/E_screenshots/E2_title_screen_logo.png" /tmp/frames/frame_000448.ppm
```
And the pixel A/B against a noise floor measured from the same runs — imported from
Fable 2, and preferred over `frame_compare.py`'s quoted band whenever there are >= 2
runs per arm:
```
python3 tools/frame_matched_diff.py --a runA1 runA2 --b runB1 runB2
```
**READ ITS PER-PAIR LINES, NOT ITS HEADLINE, and never quote a frame COUNT as an effect
size.** Part 25 learned both the hard way on one experiment (gotchas 247, 249):

* Its pooled verdict said `cross 4.02 vs floor 1.18 -> ARMS DIFFER (3.41x)`. Three of the
  four cross pairs underneath were AT OR BELOW both within-arm floors; the fourth was the
  only pairing of two 620 s runs — which drift furthest — and carried twice the sample
  count, so the pooled median inherited it. **A median over pooled pairs of unequal n is
  not a summary of those pairs.**
* "82 of 109 frames differ" sounds like a result and is not one: **two runs of the SAME
  configuration differ on 82 of 109 frames.** Only the magnitude against that floor means
  anything — 0.069 median drift, 0.401 for a positive control (6x), 0.038-0.085 for the
  change under test (i.e. invisible).

So the picture protocol is the frame-time protocol: **run the null arm FIRST, in the same
serial block on an idle GPU, and quote every effect as a multiple of it.** And enforce
admissibility with the per-frame fingerprints rather than by matched index alone — two arms
are comparable only where `drawFingerprint` AND `cameraFingerprint` agree:
```
# of 301 matched dumped frames: 70 share a camera, 44 share camera AND draw set
awk 'NR>1 {print $1, $4, $5}' /tmp/armA.txt   # frame, drawFingerprint, cameraFingerprint
```
**Quote how many frames survived that filter.** On this title's synthetic-input recipes it
is 13-44, and every one of them is under 1,800 draws — so a filter that is honest about
drift discards the entire outdoor era.

**PART 26 SETTLED WHY, AND IT IS NOT THE RECIPE.** The DebugJump route lands in a crowd at
7,300 draws and spends 93% of its frames there, so it was built to fix exactly this. Run
the filter on two runs of ONE configuration — the null, which is the only way to tell "the
arms disagree" from "the filter cannot be satisfied":
```
python3 tools/frame_determinism.py /tmp/det1.txt /tmp/det2.txt
```
It reports **422 of 13,056 frames matched, none above 141 draws, and 0 of the 12,174
outdoor frames matched** — by index or by content. A crowd of animated actors never renders
the same draw list twice, so exact equality selects for the frames where nothing is
happening (gotcha 254; `frame_compare.py`'s docstring records the same failure from the
other end, where 257 "perfectly aligned" frames were 257 copies of an empty scene).

**So outdoors, do not align — AGGREGATE, and take the noise floor from baseline runs made
in the same block.** Over the 12,000+ frames above 1,800 draws, THREE runs of one
configuration give:

| era median, frames >= 1,800 draws | 3 baseline runs | spread | usable? |
|---|---|---|---|
| **mean luma** | 56.593, 56.907, 56.738 | **0.55%** | **yes** |
| distinct colours | 98,566, 98,448, 103,777 | **5.4%** | **no** |
| coverage % | 99.669, 99.669, 99.672 | 0.003% | no — saturated outdoors |

That is the outdoor instrument: quote an arm's era median against the BASELINE BAND, and
say how many frames each median is over.

**Three baseline runs, not two, and the table above is why.** On two baselines the
distinct-colour null read 0.12% and an arm read *12.0x* it; the third baseline came in 5.4%
away and the result vanished (gotcha 258). Two independent pairs had already put that
statistic's null at 0.76% and 0.12% — a 6x disagreement, which is the null saying it is not
one yet. Mean luma reproduced across all three and is the statistic to use.

**Calibrate with an arm that changes the subsystem WHOLESALE.** `CZ_VK_NO_CUBE=1` (no cube
map bound at all) moves the median luma to 59.469 against the 56.59-56.91 band — 8x its
width, no overlap — which is what says the instrument is sensitive outdoors. Without such
an arm, a small measured effect and a blind instrument look identical, and this project has
twice mistaken the second for the first.
And how SHARP the frame is, which is the one thing no aggregate over pixel VALUES can
report. A blur preserves coverage, mean luminance, distinct colours and the whole
histogram exactly as a vertical flip does (gotcha 135), so `frame_compare.py` scored
part 14's blurred and sharp arms 0.01 pp apart — inside its own band — while the
operator could see the difference instantly. Measure the spatial DERIVATIVE instead;
it separated those arms 6.47x with no overlap:
```
python3 tools/frame_sharpness.py /tmp/dump_base1 /tmp/dump_base2 /tmp/dump_arm1 \
    /tmp/dump_arm2 --stats /tmp/base1.txt /tmp/base2.txt /tmp/arm1.txt /tmp/arm2.txt
```

Summarise a `CZ_VK_SNAP_DUMP` directory — one line per resolve snapshot, grouped by
frame and ordered by address. A single frame of this title dumps 61 surfaces, so "open
them and look" is not an analysis anyone repeats, and it is gotcha 133 applied to a
directory: whichever three you happen to open become the conclusion. The `lit%`, `mean`
and `max` columns down an address ladder are what say which LINK of a reduction chain
broke, and reading them that way is how §6ao's pitch-vs-width defect was found and how
§6ap separated "the scene is dark" from "the post chain did not run":
```
tools/snap_dump_stats.py <dir> [--frame N] [--min-lit PCT] [--sort addr|lit|size]
```

Census a capture's RESOLVES by source and destination. **18.4% of this title's resolves
copy the DEPTH buffer, not a colour target** — its three shadow cascades and its scene
depth — and our command processor read that field nowhere until part 14, which is what
made the whole frame uniformly out of focus. It is also the tool that named the scene's
real colour address:
```
python3 tools/xtr_resolve_census.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
```

Ask a capture what HARDWARE bound to a draw, and compare it with what we bound. Three
tools, all reading a single-frame `.xtr` (which is self-contained — gotcha 259 — so any
of them works on any of `Xenia logs/R2_world/`'s seven):
```
python3 tools/xtr_draw_bindings.py <t.xtr> [--min-verts N] [--csv o.csv]
                                   [--dump-texture ADDR --out DIR]
python3 tools/xtr_cube_agreement.py <t.xtr>          # per DECLARED fetch slot
python3 tools/xtr_draw_vertices.py  <t.xtr> --vs <hash> [--ps <hash>] [--min-verts N]
```
`xtr_draw_bindings` is the per-draw shader pair, every fetch constant it could sample
(address, extent, format, tiling, dimension, stack depth) and — uniquely — THE BYTES, so
a texture hardware really read can be written out and compared with our upload.
`xtr_cube_agreement` asks the shader-versus-constant dimension question the way
`bindTextures` asks it, per fetch slot the sidecar declares, which is the form that can
actually report a disagreement (gotcha 264). `xtr_draw_vertices` prints hardware's vertex
STREAMS and ALU CONSTANTS for a named draw in the same shape `CZ_VK_DRAW_PROBE` prints
ours, so the two transcripts read side by side.

**All three replay `LOAD_ALU_CONSTANT`, and they must**: this title issues 620 of those
against 36 `SET_CONSTANT`s in one frame, so a replay without it reconstructs almost none
of the ALU constant file (gotcha 262). 81 of the 620 read memory the capture does not
carry, and `xtr_draw_vertices` prints `UNRECOVERABLE` for those registers rather than the
stale value (gotcha 263) — **which is the difference between "hardware's answer" and
"some earlier draw's leftover".**

Disassemble the guest image. **Reach for this before reading `ppc/`** — a recompiled
function is a translation, and most questions ("what writes this field", "which branch
does this predicate take", "how many arguments does this call site really pass") are
about the original. The host toolchain cannot do it: no PowerPC target in `objdump`, no
`-b binary` in `llvm-objdump`, and `llvm-mc` silently loses instruction alignment on
the first VMX128 encoding it does not know:
```
python3 tools/gdis.py 8284B568 --count 120        # a function
python3 tools/gdis.py 8284B6C0 --to 8284B710      # a window around a faulting insn
python3 tools/gdis.py --find-uses 0x7FEA1800      # every lis/addi pair building a
                                                  # constant, with context — a 32-bit
                                                  # constant is never one instruction,
                                                  # so grepping the image misses them
```

Re-derive the save/restore helper addresses:
```
python3 tools/find_save_restore.py assets/game/default_image.bin
```

Read a GPU capture (`tools/xtr.py` is the format; the rest are thin CLIs over it —
see `docs/xtr-decoder.md`):
```
python3 tools/xtr_walk.py stats  "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/xtr_walk.py limits "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/xtr_pm4_census.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" --verify
python3 tools/xtr_determinism.py \
    "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" \
    "Xenia logs/gpu_B1b_boot_repeat/58410A8D_stream.xtr" --labels B1 B1b
```
`--verify` is the only check in the census that *can* fail — always pass it.

Replay the ME's bin-predication rule over a capture. **This is the oracle for any claim
about what our command processor discards**, and it needs no emulator: Xenia records a
`PacketStart` for every packet BEFORE evaluating predication, so the capture contains
the packets hardware skipped. Compare its pair table against `CZ_PM4_BIN_CENSUS=1`'s —
hardware discards **0.3%** of this title's draw packets and, since part 11's screen
extent, so do we (0.28%; it was 33% — gotcha 178, and gotcha 185 for the cause):
```
python3 tools/xtr_bin_predication.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr" --per-select
```
`--trace-window N --trace-arm-mask 8000000F` prints the capture's own stream-order
window — the exact twin of `CZ_PM4_BIN_TRACE` + `CZ_PM4_BIN_TRACE_ARMMASK`, and the
comparison that named part 11's defect in four lines (gotcha 187). It stops at the
budget, so its census is over a PREFIX and says so.

Check the command processor against the boundaries hardware itself used. The first
covers each packet's LENGTH (24.5 M packets); the second covers a WALK — every indirect
buffer's start address and every internal boundary, chained from its first dword (28,726
buffers), which is the half a per-packet check structurally cannot see. **Exit 1 = our
parser would desync on a real stream:**
```
python3 tools/pm4_packet_lengths.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
python3 tools/pm4_indirect_walks.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
```
Both passing means our *arithmetic* is right. It says nothing about whether the bytes
we walk are the bytes hardware walked — that was finding 39's whole lesson (gotcha 88),
and the live counter for it is `ring: indirect buffers truncated=` in `CZ_RING_TRACE`,
which must be **0**.

## Is the depth buffer full yet? — the ORDER inside a hardware frame

`tools/rt_depth_order_census.py` walks the twenty `.xtr` world traces in stream order
and, for the first draw that samples a named surface, reports how many earlier draws
wrote the depth surface that draw renders into. It exists because a design decision
rested on "by then the Z prepass has finished" and the only evidence for it was a COUNT
over a whole boot, which cannot speak to order (gotcha 392). It printed the opposite:
this title has no scene Z prepass, and the first draw of the scene pass already samples
the shadow atlas.

```
tools/rt_depth_order_census.py                    # every world trace, with a verdict
tools/rt_depth_order_census.py --trace <one.xtr> --verbose   # the per-draw timeline
```

Two filters are load-bearing and are the reusable part. The cascade atlas is identified
by SHAPE (the widest depth-format fetch whose width is a multiple of its height), not
by an address, because hardware's is `1812F000` and ours is `1439B000`. And a draw
counts as sampling it only if the atlas sits in a slot the bound shader's own sidecar
DECLARES — a register file carries all 32 fetch constants whether the shader reads them
or not, so raw address matching over-reports about 6x.
