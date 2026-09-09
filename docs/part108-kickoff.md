# Part 108 kickoff — the hand-off from part 107 (2026-09-09)

**THE LIVE HAND-OFF.** It supersedes `part107-kickoff.md` on "where the port is"; that
file's §0b (the CPU plan, the clock cap, the worker floor) has been executed and its
§1 item 1 (the CPU half, re-baselined) is this part. `docs/perf-plan-part107.md` is the
plan and carries the record in place (§1.1, §1.2, §2b); `phase5-notes.md` §6ex is the
narrative; gotchas 533-535.

## §0 What part 107 established — read this before anything else

0. **THE OPERATOR'S CPU MAY STILL BE CAPPED AT 3200 MHz** (`sudo cpupower frequency-set
   -u 3200MHz`, set for the stand-in). Part 107's closing message asked for
   `sudo cpupower frequency-set -u 4654MHz`; **check `scaling_max_freq` before believing
   any number taken on this box**, and if it still reads 3200000 the box is still the
   stand-in, not the operator's machine.
1. **The stand-in is calibrated** (plan §1.1-§1.2): `taskset -c 0-3,8-11` + the 3.2 GHz
   cap. Its crowd sat at 16.6-17.4 ms (57-60 fps) before the part; four cores against
   eight at the same clock read +32% — a number taken WITH item 3 below running on both
   sides, so re-attribute it before calling it contention (gotcha 535). The null under
   the mask is ±1% frame-weighted, ±4% in any one band.
2. **Item 1 (the third worker on 4c/8t) is a null** — floor stays at three. **Item 2
   (the Draw Thread's wait, parked) is BUILT, ON, and a null on frame time** — the word
   is the FENCE counter, not the read pointer (retracted in place; gotcha 533);
   `cpu/fence_wait.cpp`, `CZ_FENCE_PARK=0` the control, its `[fencewait]` counters the
   standing gate (parks > 0, MISSED 0). Gates run: A5 exit 0 (4 permutation, 0 real),
   ring `truncated=0`, sync validation 0 hazards (route reached 3,930 draws under the
   cap; its own gate refused the run for that), poison control 30. A1 shows the
   documented position-71 interleave (bimodal, gotcha 221) — quote A5.
3. **THE FINDING: our native-KB/M glyph scan was the busiest thread in the process —
   99.4% of a core for 137-150 s from the first input poll, in every run since part 92**
   (gotcha 534; `phase5-notes.md` §6ex §2). Fixed in `cpu/native_kbm.cpp`: a 64-aligned
   multi-probe pass (26 of 26 glyphs in ~96 ms), a rarest-byte memchr anchor for the
   fallback and the bank, the physical arena first, the worker at low priority, and
   START/END lines with the scan's length. `CZ_KBM_SCAN_LEGACY=1` is the control (the
   whole original algorithm). Measured on the stand-in, legacy vs the aligned build:
   **−5.7% frame-weighted, monotone, −1.49 ms at 8,000-9,000 and −1.83 at 9,000-10,000;
   crowd windows under 16.7 ms 67% → 100%, mean of window medians 16.41 → 15.33 ms,
   mean p99 20.59 → 19.41** (the final finder; the scan is 0.122 s end to end); on 16 cpus
   −1.41 ms at the crowd (one run of the interim build). §6ex's closing addendum.
4. **Where the stand-in stands after the part:** 8,000-9,000 at 15.0 ms (≈67 fps),
   9,000-10,000 at 16.0 (≈62), p99 19.4 ms. The median target is met on the stand-in
   in both crowd bands; the p99 is the next number.
5. **Every crowd baseline from part 92 to part 106 carried the scan for most of its
   soak** — on this box and on the operator's. Numbers stand as measured; any
   comparison across a part boundary that straddles this fix needs the `[kbm] ... END`
   line placed against the `[fps]` windows first.
6. **Item 3(a) already existed** (`WriteRegisterRun`'s bulk path). The pump's split under
   the stand-in is plan §2b's table: `DoDraw` 26%, the walk ≈23%, `UploadStream` +
   `UploadTexture` ≈14%, no symbol above a quarter.

## §1 Part 108 — autonomous, in order

0. **Re-measure the 4-vs-8-core gap WITHOUT the scan** (two runs each, capped): what
   remains is the real contention number for §0.4's item, and it decides whether the
   remaining items are worth building on a 4-core box at all.
1. **The p99** (plan item 7): `CZ_FPS_LOG` p99 19.6-21 ms at the crowd; classify the
   slow frames by where the time went (`CZ_VK_FRAME_TRACE`'s columns; texture-upload
   frames are counted separately by the band reader). A locked 60 is a p99 claim.
2. **Plan items 4-6 in order** (`UploadStream`'s resolve half, `UploadTexture` as pure
   lookup, the constants) — each priced under the stand-in first, kill rule 0.4 ms.
3. **The Windows compile of `cpu/fence_wait.cpp` and `cpu/native_kbm.cpp`** (czwin was
   unreachable during part 107; `WaitOnAddress`/`WakeByAddressSingle` link through
   `synchronization`, added to the link line), then the artifact rebuild that now
   carries the log file, the mirror, the park and the scan fix.
4. **The operator's play with the scan fix**: the first two and a half minutes of every
   session used to run beside a memory sweep; ask whether the early game feels
   different, and hand them `CZ_KBM_SCAN_LEGACY=1` as the A/B.

## §1b Part 108 — the operator's

0. `sudo cpupower frequency-set -u 4654MHz` if it has not been done.
1. The rebuild-or-v1.0.3 decision (part106-kickoff §1b item 0), now with the mirror,
   the park and the scan fix in.
2. A GTX 1060 / Ryzen 3 3100 owner's `--diag` and crowd run, if one can be found.

## §2 Instruments and arms this part added

`CZ_FENCE_PARK`, `CZ_FENCE_PARK_SPIN_US`, `CZ_FENCE_PARK_TRACE`, `CZ_KBM_SCAN_LEGACY`,
`tools/part107_standin_probe.sh` (`docs/instruments.md` "Part 107"). Campaign scripts,
traces, logs and both perf profiles are in `~/DR2CZ-troubleshooting/part107/`
(`ab_fencepark.sh`, `ab_kbmscan.sh`, `confirm_gates.sh`, `final.sh`, `probe_{spin,park}.*`).
