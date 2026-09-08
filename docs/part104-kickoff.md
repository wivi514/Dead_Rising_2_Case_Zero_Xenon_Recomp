# Part 104 kickoff — after the AMD/Windows performance board (part 103)

**Written 2026-09-08 at the end of part 103. Supersedes `part99-kickoff.md` as the live
hand-off.** Read `phase5-notes.md` §6et (part 103's record) and
`part103-amd-windows-perf-plan.md` §1b (its execution table) before anything below.

## §0 Where the port is

- **v1.0.1 shipped** (parts 98-102's fixes: async pipeline creation + pre-warm chain, the
  seed-shadow union, the semaphore limit, the AMD depth fallback, the vertex recipes, the
  golden writer). The repo is public; the operator promotes it.
- **Both test machines are stutter-free at steady state and session one is clean on both**
  (§6es: first-sight 4 boot-only, skipped draws in the hundreds, no stutter, no pop-in —
  operator-verified on the dev box; czamd the same by the counter).
- **Performance on AMD/Windows is CLOSED BY MEASUREMENT (part 103).** The RX 6600's frame
  is the title's own shading at the RX 6600 / RTX 3070 hardware ratio (2.4-2.5x at every
  load band); ours is ~1.2 ms of 20 at the crowd and 1.08 of that is the MSAA resolve.
  MSAA 2x is free on that box (single-sample is SLOWER under 6,000 draws — gotcha 518),
  the dead resolve copies are 0.14-0.39 ms, barriers 0.06 ms. The only lever a player has
  is the internal resolution, and the launcher already carries it. **Do not re-open a GPU
  item on czamd without a NEW class in the split**; the split is
  `CZ_VK_GPU_PASSES=1 CZ_VK_STATS=1500` + `tools/gpu_split_window.py` there (no exit
  dump on that box, gotcha 519), and its headless number carries a 1.12 ms readback the
  release never pays (gotcha 517).
- **The cold-driver-cache warm on czamd is priced** (§6et §6): +1.5 ms at p99 for its 55 s,
  nothing at the median, no >2x frames; the part-103 priority change recovers ~0.5 ms of
  that at one run per arm. Items 4b/4c of the part-103 plan are not warranted.
- **Performance on the 3070 stays parked** (`part91-kickoff.md` §0c-§0d) — no lead
  ≥0.5 ms on either side of the crowd.

## §1 Owed, in order

1. **Item 5 of part 103 — the golden store's boot-time file walk.** Now measured:
   **czamd 5,923-5,958 files in 1,048-1,095 ms (176-185 us a file; 765 ms / 135 us on a
   warmer run), dev box 29,932 in 1,290 ms**, on the boot
   path before the first frame, and it grows with every session (every new small texture
   adds a file). The design is one packed file (`golden.pack`: signature + length + bytes,
   rewritten by the background writer at exit from the in-memory map) with the per-file
   directory read only when the pack is absent, or a lazy load keyed by signature at the
   first decode of that signature. The pack is simpler and turns 5,679 opens into one; the
   gate is the preload line reading the same signature count in under ~50 ms, and the
   picture gate is the black-pit fix still holding (`CZ_VK_NO_GOLDEN_TEX=1` is the arm that
   brings the pit back, §6eo/§6ep). Not urgent — 0.77 s of a 90-130 s czamd boot — but it
   is the last named boot cost that is ours.
2. **The czamd black square** — `part103-amd-windows-perf-plan.md` §2 unchanged: arms
   staged on czamd (`cz_arm_fif1.bat` first, then `noclear`, then `norecord`); it vanishes
   on capture so a cross-frame race is the lead. Operator-driven.
3. **The intermittent czamd pre-frame park.** One boot in five this part sat after
   `[kbm] splice` with `KeDelayExecutionThread` spinning and never delivered `vblank
   #1000`, with and without instruments — `part99-amd-hang.md` §5.4's residual. The
   diagnostic is already in the tree (`CZ_KCALL_WHO` milestone backtraces, `CZ_APC_TRACE`,
   `CZ_KOBJ_DUMP`); arm them on the crowd script and wait for the one-in-five.
4. **Part 102's licensing call** on the vertex recipes (`part102-no-popin-plan.md` §2.1:
   978 patched instruction dwords shipped vs. reimplementing the patch routine) — the
   operator's decision, and the seed's 3 orphan vertex shaders (`vs_recipes.py` names
   them) which need a live-process recovery from an era no run has entered.
5. **Case West** inherits everything through `docs/reusability.md`; part 103 adds: ship no
   MSAA-off performance row, read a Windows test box with `CZ_VK_STATS`, and the
   `[threads]` block is the first thing to read in any small-machine report.

## §2 The czamd harness as it stands

`C:\Users\lisab\Desktop\CaseZeroRecomp\p103_run.ps1 -Tag <t> -Extra K=V,K=V` — the crowd
route headless, logs to `p103\<t>.err.log`; `p103_cold.ps1` the same with the AMD driver
cache emptied and our pipeline cache parked (XDG_CACHE_HOME to a fresh dir — the golden
store on Windows ignores it and stays warm, on purpose). The exe there is ab80b87
(`cz_runtime_part102.exe` is the previous). `crowd_run.ps1` and the `cz_*.bat` arms are
unchanged. Boot to first frame is 90-130 s; a run with no `vblank #1000` by 150 s is the
§1 item 3 hang, not a slow boot.
