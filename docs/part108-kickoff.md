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

## §0c The operator's low-end sessions (2026-09-09, after the part closed)

GPU core locked at 210 MHz (the 1050 Ti compute stand-in), CPU at the 3.2 GHz cap,
`taskset -c 0-3,8-11`, 2x MSAA, the operator playing through crowds
(`~/DR2CZ-troubleshooting/play/p107_lowend*.{log,trace}`; `CZ_VK_GPU_PASSES=1`):

| session | crowd `[fps]` | wall / GPU / fence at 8,000-10,000 draws | note |
|---|---|---|---|
| 3440x1440 | **30-33 fps** | 30.0 / 29.8 / 14.1 ms | memory clock sat at 405 MHz (P8 idle, ~26 GB/s — a quarter of a 1050 Ti's); flat 30-33 ms from 2,400 draws up: pure GPU |
| 1920x1080 | **53-54 fps** | 19.1 / 18.9 / 3.3 ms | memory 5,001 MHz; still GPU-bound with ~3 ms of CPU slack |
| 1600x900 (live switch) | **62-64 fps** | 16.4 / 15.8 / 0.7 ms | GPU-bound with under 1 ms of slack |

The operator's words: 1440p *"33 fps which is not acceptable performance for a xbox 360
title"*; 1080p *"51 to 55 fps which is much closer to what I want"*; 900p *"60 fps up to
65"*. **Every one of these is the GPU at a tenth of its clock, not the four cores**: wall
equals GPU in every band and the CPU sits in the fence. At 1080p the whole-run GPU split
puts the **MSAA resolve copies first (3.03 ms, 27.6%)**, then the scene passes (2.83),
the one-draw post passes (1.76), the shadow cascade (1.25): for a 1050 Ti-class card the
picture decisions (2x MSAA, the post chain at full resolution) are the levers, as
perf-plan-part106 §4.2 said. **The 1080p `CZ_VK_MSAA=0` session then read 59-65 fps
through the crowds** (window medians 16.9 → 15.4 ms; trace at ≥6,000 draws: wall 15.8 /
GPU 15.4 / fence 2.3 ms — still GPU-bound, at the target). For a 1050 Ti-class card the
honest recommendation is therefore **1080p without MSAA**, and the settings panel should
be able to say so (MSAA is an env arm today, not a setting — a part 108 item).

**SUBJECT CHANGE, 2026-09-09, the operator closing the session:** *"Good enough for now
we'll do other performance improvement later but for v1.0.2 it's huge improvement."*
**PERFORMANCE IS PARKED AGAIN; THE LIVE WORK IS THE v1.0.2 REBUILD** (part106-kickoff §1b
item 0, now carrying part 105's log file and `--diag`, part 106's store mirror, and part
107's fence park and glyph-scan fix — every one with a control arm named in
`docs/instruments.md`). The resume list, in order, is §1 below; the stand-in recipe and
its calibration are perf-plan-part107 §1, and nothing in it needs re-deriving.

**A defect found by the second launch: `CZ_VK_RES=1280x800` — the Steam Deck's native
panel — is REFUSED** (`[vk] ... not a resolution this renderer can produce (even width,
height 720..2880, at least 16:9) — IGNORED, rendering at 1280x720`). A Deck therefore
renders 720p and presents it on an 800-line panel. Part 108 item: accept 16:10 (and
document what the EDRAM stand-in does with it).

## §1 Part 108 — autonomous, in order

00. ~~**16:10 resolutions** (the Deck's 1280x800; §0c) — refused today.~~ **DONE
    2026-09-09 (narrow mode, `phase5-notes.md` §6ey): the aspect floor is 16:10, the
    launcher ladder carries 1280x800 / 1920x1200 / 2560x1600; OPERATOR-VERIFIED the same
    evening — the panel offered 1280x800 / 1440x900 / 1680x1050 / 1920x1200 on the 3440x1440
    display and the selection applied live: "Perfect".**
0a. ~~**MSAA as a SETTING** (§0c): the 1050 Ti-class answer is 1080p without MSAA, and a
    player cannot choose that from the panel.~~ **DONE 2026-09-09**: `msaa=0|2|4` in
    cz_settings.txt, an MSAA row in the panel (row 5, starred until relaunch, footer
    "MSAA APPLIES AT THE NEXT LAUNCH") and in the launcher; `CZ_VK_MSAA` wins over it.
    Headless gate: file 0 → single-sample, file 0 + env 2 → 2x, file 4 → 4x, file 7 →
    refused to 2x, each announced with its source. Operator-verified the same evening: "it is there perfect".

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

## §1c Part 108 — done after the kickoff was written (2026-09-09)

* **16:10 narrow mode** and **MSAA as a setting** — `phase5-notes.md` §6ey and its
  first addendum; both operator-verified.
* **The window follows the resolution** (operator instruction, *"when we are in
  windowed mode when changing resolution it also resize the window"*): a windowed
  window opens at the persisted internal resolution (clamped to the desktop's usable
  bounds, aspect kept) and resizes on every live apply and on leaving fullscreen;
  pinned, fullscreen and maximised windows decline with a log line. §6ey addendum 2;
  operator-verified in the gate run itself (*"it works"*). The Windows compile of
  `host/window.cpp` is owed along with the rest of §1's item.
* **Controller vibration** (the first of the public player reports the operator queued
  as `open-items.md` 0aa; instruction: *"Implement controller vibration"*):
  `XamInputSetState` now reaches the pad — `Host_PadRumble` → the window thread →
  `SDL_GameControllerRumble`, a change at once and a held level refreshed every
  250 ms. `CZ_NO_RUMBLE=1` control, `CZ_RUMBLE_TEST=1` positive control,
  `CZ_RUMBLE_TRACE=1` witness. §6ey addendum 3. Operator-verified (*"it works"*, Xbox
  Series X pad, 94 level changes, every issue rc 0) — and then RE-TIMED: the title's
  effects are counts of 30 fps frames ticked per frame, so `cpu/rumble_guest.cpp` runs
  the title's rumble tick at 30 Hz of real time (`CZ_RUMBLE_TICK_HZ`, `=0` the
  control); operator: *"Pretty good now"*. **OWED: the Windows leg,**
* **The mouse wheel's two-notches-per-item** (0aa item 2): press+release in one
  controller tick is no press for a level-sampling title; the feed carries the release
  to the next tick (`CZ_KBM_NO_TAP_SPLIT=1` control, `CZ_KBM_WHEEL_TRACE=1` witness).
  §6ey addendum 4, gotcha 538; 60 of 60 notches, *"Yeah you fixed it."*
  **OWED: the Windows leg** (SDL's wheel event granularity on Windows — the trace's
  `steps=` per notch is the number to read),
* **The grab QTE's "refused" final key** (0aa item 3): our map is DR2 PC's, whose
  minigame face buttons are WASD; our chips draw Y as Q, so the prompt lied. The map
  follows the art now (`cpu/kbm_default_map.h`); LMB/E/SPACE prompts *"all worked"*.
  §6ey addendum 5, gotcha 539.
* **The "audio decoder stutter" report** (0aa item 6): priced, not a stutter source
  (2.8% of a core at the crowd; libavcodec 0.31% of cycles); the per-packet
  `Could not update timestamps` log line fixed by giving packets a timestamp; the
  audio threads named. §6ey addendum 6.
* **NEW, not taken: near actors black from the screen's centre rightward** — the
  operator's long-standing report with an F9; the seam is the tile boundary.
  `open-items.md` item 0ab has the evidence and the three instruments in order. where SDL's XInput/WGI backend does the rumble
  (`windows-test-list` item 9). `docs/release-notes-v1.0.2.md` carries it; the notes
  do NOT yet carry parts 107-108's other items (the CPU work, 16:10, MSAA, the window
  follow) — write those bullets when the rebuild is packaged.

## §2 Instruments and arms this part added

`CZ_FENCE_PARK`, `CZ_FENCE_PARK_SPIN_US`, `CZ_FENCE_PARK_TRACE`, `CZ_KBM_SCAN_LEGACY`,
`tools/part107_standin_probe.sh` (`docs/instruments.md` "Part 107"). Campaign scripts,
traces, logs and both perf profiles are in `~/DR2CZ-troubleshooting/part107/`
(`ab_fencepark.sh`, `ab_kbmscan.sh`, `confirm_gates.sh`, `final.sh`, `probe_{spin,park}.*`).
