# Part 105 kickoff — after part 104 (the AppImage, the golden pack, the park hunt)

**Written 2026-09-08 at the end of part 104. Supersedes `part104-kickoff.md` as the live
hand-off.** Read `phase5-notes.md` §6eu (part 104's record) and `release-plan.md` §9.9
before anything below.

## §0 Where the port is

- **v1.0.2 is BUILT AND GATED ON LINUX, NOT PUBLISHED — the Windows zip needs one rebuild first (§1 item 0).** `docs/release-notes-v1.0.2.md`
  carries the three SHA-256s; the artifacts are in `dist/` (Linux `.tar.zst` 26 MB and
  `.AppImage` 25 MB built on Ubuntu 22.04 with glibc floor **2.35**, Windows zip 21 MB
  built on czwin), all from source `482b47f`, which also carries the Wayland-first fix and the title/icon change (§6eu §6) (§6eu §5: every
  shipped Linux build presented at 1.0 fps on a Wayland+NVIDIA desktop; gotcha 524). Tagging and attaching are the operator's,
  as for v1.0.1. **If any artifact is rebuilt, its hash in the notes must be refreshed.**
- **The Linux glibc floor is 2.35, measured off the artifact per file** and gated in two
  images (pass at 2.35, refuse with the named symbol at 2.34). It cannot go lower than
  2.34 while the bundle dlopens the DXC prebuilt (gotcha 520).
- **The golden store is one pack file** (`golden.pack`, append-only, torn tail cut at
  load, loose files folded in and removed). Dev box: 1,301-1,331 ms → 475-509 ms for
  29,932 signatures / 345 MB. **czamd's number is owed** — see §1 item 1.
- **The czamd pre-frame park did not recur under its instruments: 122 armed boots (112
  rapid, 6 full-length, 4 cold), 0 parked** (§6eu §3). The untested candidate is the first
  boot after a fresh exe deploy — every recorded hang followed one — so §1 item 1's deploy
  of the v1.0.2 exe should itself be run through `p104_full.ps1` (armed), twice. The instruments are staged (`tools/czamd/p104_*.ps1`); a boot with no
  `vblank #1000` by 150 s is still the hang, and `vblank #1000` arrives at 5 s in a
  healthy boot (gotcha 521), not at the first frame.
- Performance stays parked on both boxes (`part104-kickoff.md` §0 still describes it).

## §1 Part 105 — autonomous, in order

0. **Rebuild the Windows zip at the source head** (`482b47f` or later) — czwin was off the
   network at the end of part 104, so `dist/CaseZeroRecomp-windows-x86_64.zip` is the
   `79ef1b7` build without the title/icon change. `ssh czwin`, pull, `cmake -S runtime -B
   runtime\build` (a new source file), build, `release_package_windows.ps1`, scp the zip,
   refresh its hash in `docs/release-notes-v1.0.2.md`. Until then v1.0.2 is not ready to tag.
1. **Deploy the v1.0.2 Windows bundle to czamd and read the golden pack's number there**
   (two boots: the migration, then the pack; `tools/gpu_split_window.py` is not needed —
   the preload line prints as it goes). The kickoff's "under ~50 ms" gate was written for
   czamd's ~6,000-file / ~70 MB store; the dev box's 475 ms is for 345 MB. Deploying
   swaps the exe the hang campaign ran under; keep `cz_runtime_part103.exe` beside it.
2. **The crowd-route confirmation on the old-base binary.** Part 104's first attempt found
   the 1 fps defect instead (§6eu §5); the rebuilt bundle's run is in §6eu §5 if it landed,
   and one run a side is a coin flip on this workload (gotcha 159). Three runs an arm,
   `tools/part80_trace_band.py`, before any claim that the compiler change is a null.
3. **The park, if it recurs on the operator's desktop:** `tools/czamd/p104_full.ps1` is
   the armed form of their boot; the log of a parked boot has the milestone backtraces,
   the KOBJ dumps every 20 s and the APC balance. Read the last `[kcall+]` backtrace and
   the last `streamedassets` read before anything else (`part99-amd-hang.md` §5.2 is the
   shape).
4. **`docs/reusability.md`'s old-base rule needs Case West's own Containerfile** the day
   that port ships on Linux; `tools/release/oldbase/Containerfile` is copyable as is.

## §1b Part 105 — the operator's (unchanged from part 104's §1b)

1. **The czamd black square** — `cz_arm_fif1.bat`, then `cz_arm_noclear.bat`, then
   `cz_arm_norecord.bat`, at a visible session; the arm it stops under names the
   mechanism (`part103-amd-windows-perf-plan.md` §2).
2. **The vertex-recipe licensing call** (`part102-no-popin-plan.md` §2.1).
3. **The seed's three orphan vertex shaders** — an operator playthrough with
   `CZ_SHADER_DUMP` armed, or the live `process_vm_readv` recovery.
4. **Publish v1.0.2**: tag at the docs head, attach the three artifacts from `dist/`,
   paste `docs/release-notes-v1.0.2.md`, pull the downloads back and hash them.

## §2 What already exists and must not be rewritten

- `tools/release_build_oldbase.sh` + `tools/release/oldbase/Containerfile` — the whole
  Linux release path (image, SDL2, ffmpeg with nasm, XenonRecomp libs, both runtime
  builds, the identity gate, the packaging), all inside podman with the repo mounted at
  its own absolute path. `CZ_OLDBASE_SKIP_DEPS=1` reuses the built deps; `--shell` for
  a look inside. The XenosRecomp and XenonRecomp checkouts are mounted read-only.
- `tools/release_package_appimage.sh` — stage → AppDir → squashfs + type-2 runtime, with
  the three host self-checks. The runtime is cached under `thirdparty/appimage/`.
- `tools/release_gate_clean_container.sh` — takes a stage dir OR a `.AppImage`; apt or
  microdnf; run it at the floor (ubuntu:22.04) AND below it (Rocky 9). Do not edit it
  while a gate is running (bash reads scripts lazily; §6eu §1).
- `tools/release_package_linux.sh` — prints the floor per file; `CZ_FFMPEG_WORK`,
  `CZ_SDL2_PREFIX`, `CZ_DXC_LIB` are its container-side knobs.
- `runtime/host/png_icon.{h,cpp}` — the PNG decoder behind the window icon (PIL-identical on
  the five game PNGs); `window.cpp` `ApplyGameIcon` at the three window sites.
- `runtime/host/host_paths.cpp` step 1b (`$APPIMAGE` + the `$APPDIR` containment guard);
  `runtime/gpu/vk_renderer.cpp` `GoldenLoad`/`goldenwriter` (the pack, its arm
  `CZ_VK_NO_GOLDEN_PACK=1`).
- `tools/czamd/p104_loop.ps1`, `p104_full.ps1`, `p104_cold.ps1` and their `_all`
  wrappers — the campaign, run as scheduled tasks (`schtasks /run /tn cz_p104all`).
- `docs/release-notes-v1.0.2.md`, `dist/` with all three artifacts and `.sha256` files.

## §3 The czamd harness as it stands

`p103_run.ps1` / `p103_cold.ps1` / `crowd_run.ps1` and the `cz_*.bat` arms are unchanged.
The exe on the desktop is still **ab80b87** (part 103) unless §1 item 1 has run; the
part-104 logs are in `p104\` with `summary.txt` one line per boot.
