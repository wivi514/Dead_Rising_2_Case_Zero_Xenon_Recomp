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

Census a capture's RESOLVES by source and destination. **18.4% of this title's resolves
copy the DEPTH buffer, not a colour target** — its three shadow cascades and its scene
depth — and our command processor read that field nowhere until part 14, which is what
made the whole frame uniformly out of focus. It is also the tool that named the scene's
real colour address:
```
python3 tools/xtr_resolve_census.py "Xenia logs/gpu_B1_boot/58410A8D_stream.xtr"
```

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

