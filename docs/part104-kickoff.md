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

## §1 Part 104 — the work that needs no operator (operator instruction, 2026-09-08: *"remove what you cannot do alone and put them for part 105 and add the work for appimage for linux to part 104"*)

1. **The Linux AppImage, and the glibc floor with it** (release-plan E.2, owed since part
   82; `release-plan.md` §9.8 items 3-4). The shipped `.tar.zst` inherits this machine's
   glibc 2.43 and refuses to start on anything older; an AppImage RUNTIME alone does not fix
   that — the binary still links the build host's glibc — so the work is two halves: (a)
   build the release in an OLD-BASE container (podman is already the clean-container gate's
   tool; a Debian-oldstable or Ubuntu 20.04 image gives a ~2.31 floor) with our own SDL2 and
   LGPL ffmpeg built in the same container, `tools/release_text_identity.sh` still passing
   against the dev build; (b) wrap `dist/CaseZeroRecomp` as
   `CaseZeroRecomp-linux-x86_64.AppImage` with the Vulkan loader left to the host
   (`release-plan.md` §2.1's table says so), `libdxcompiler.so` bundled, `AppRun` deriving
   the root the way `host_paths.cpp` does (from the executable, never the CWD — and an
   AppImage's executable is a mount, so the game-data root must resolve BESIDE the
   AppImage, which is a host_paths case that does not exist yet). Gates: the existing
   `release_gate_clean_container.sh` run against the AppImage in a container OLDER than the
   build base, both artifacts' SHA-256 in the release notes, and `ldd`'s glibc floor
   printed by the packaging script. Ship as v1.0.2 alongside the Windows zip rebuilt at the
   same head.
2. **The golden store's boot-time file walk** (part 103 item 5). Measured: czamd
   5,923-5,958 files in 1,048-1,095 ms (176-185 us a file), dev box 29,932 in 1,290 ms, on
   the boot path before the first frame, growing with every session. One packed file
   (`golden.pack`: signature + length + bytes, rewritten by the background writer at exit
   from the in-memory map) with the per-file directory read only when the pack is absent.
   Gates: the preload line reads the same signature count in under ~50 ms; the black-pit
   fix still holds (`CZ_VK_NO_GOLDEN_TEX=1` brings the pit back, §6eo/§6ep); a session that
   captures new textures grows the pack on the next boot.
3. **The one-in-five czamd pre-frame park.** One boot in five this part sat after `[kbm]
   splice` with `KeDelayExecutionThread` spinning and never delivered `vblank #1000`, with
   and without instruments — `part99-amd-hang.md` §5.4's residual. Arm `CZ_KCALL_WHO`
   milestone backtraces, `CZ_APC_TRACE` and `CZ_KOBJ_DUMP` on `p103_run.ps1` and loop boots
   (a loop of ten is an hour) until it recurs; a boot with no `vblank #1000` by 150 s is the
   hang. The diagnostic is the deliverable if a fix does not follow in the part.
4. **Case West carry-over notes** into `docs/reusability.md`: ship no MSAA-off performance
   row (gotcha 518), read a Windows test box with `CZ_VK_STATS` (gotcha 519), the headless
   readback caveat (gotcha 517), and the `[threads]` block as the first read on a
   small-machine report.

## §1b Part 105 — the work that needs the operator (moved out of part 104 on the same instruction)

1. **The czamd black square.** It vanishes on any capture, so no instrument can see it; the
   three arms are staged on czamd's desktop and are run in order at a visible session:
   `cz_arm_fif1.bat` (one frame in flight), then `cz_arm_noclear.bat`, then
   `cz_arm_norecord.bat`. The arm under which it stops appearing names the mechanism
   (`part103-amd-windows-perf-plan.md` §2).
2. **The vertex-recipe licensing call** (`part102-no-popin-plan.md` §2.1): the shipped
   `vs_recipes.bin` carries 978 patched instruction dwords derived from the title's own
   bind; option (B) ships captured declarations and reimplements the ~85-instruction patch
   routine. The operator decides which line to ship.
3. **The seed's three orphan vertex shaders** (`vs_493c66172ad88e3d`, `vs_69b6efbe227d40ce`,
   `vs_8588a559f396b81d`, named by `tools/vs_recipes.py`): no dump holds their microcode and
   no autonomous route has entered the era that binds them. An operator playthrough with
   `CZ_SHADER_DUMP` armed, or the live `process_vm_readv` recovery when a run reports them.

## §2 The czamd harness as it stands

`C:\Users\lisab\Desktop\CaseZeroRecomp\p103_run.ps1 -Tag <t> -Extra K=V,K=V` — the crowd
route headless, logs to `p103\<t>.err.log`; `p103_cold.ps1` the same with the AMD driver
cache emptied and our pipeline cache parked (XDG_CACHE_HOME to a fresh dir — the golden
store on Windows ignores it and stays warm, on purpose). The exe there is ab80b87
(`cz_runtime_part102.exe` is the previous). `crowd_run.ps1` and the `cz_*.bat` arms are
unchanged. Boot to first frame is 90-130 s; a run with no `vblank #1000` by 150 s is the
§1 item 3 hang, not a slow boot.
