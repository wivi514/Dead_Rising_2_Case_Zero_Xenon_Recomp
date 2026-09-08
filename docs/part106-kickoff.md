# Part 106 kickoff — after part 105 (the Steam Deck plan executed on the dev box)

**Written 2026-09-08 at the end of part 105. Supersedes `part105-kickoff.md` as the live
hand-off.** Read `docs/steam-deck-plan.md` §6 (the item-by-item record) and
`phase5-notes.md` §6ev before anything below.

## §0 Where the port is

- **Every build now writes `cz_runtime.log` beside its data root and has `cz_runtime
  --diag`** (`host/log_file.{h,cpp}`; `docs/instruments.md`'s last section). The
  renderer's required Vulkan features are ONE table read by bring-up and `--diag`; a
  missing one is named, with the driver. gamescope is detected and the log states the
  video path. Gated on the dev box AND on czwin (Windows: file == console by hash for
  both `--diag` and a renderer boot; Wine 11 names itself in `--diag`).
- **The three v1.0.2 artifacts in `dist/` are at 482b47f and carry NONE of part 105.**
  `docs/release-notes-v1.0.2.md` is still paste-ready for those. Decision for the
  operator (§1b item 0): rebuild all three at the part-105 head and refresh the hashes
  (the useful option — a Deck tester needs a build WITH the log file), or publish v1.0.2
  as is and make part 105 v1.0.3.
- **The Steam Deck hypotheses (`steam-deck-plan.md` §2):** H1 fixed (v1.0.2), H2 mostly
  refuted (Wine 11 runs the zip end to end), H5 closed on this box (gamescope strips the
  variables; x11 path presents at ~165 fps here), H4 priced (shader build 12.5 s on a
  4c/8t mask at desktop clocks). **H3 — RADV — is the one that matters and nothing here
  can test it**; the live-USB run (§1b item 1) and the first Deck report are the tests.
- Performance stays parked on both boxes; the czamd park hunt stands as part 104 left it.

**Item 0's Windows half — DONE the same evening, once czwin was on.** Built with
clang-cl, no warnings; `cz_runtime.exe --diag` on czwin (NVIDIA RTX 3070 Ti Laptop,
610.62) writes `cz_diag.txt` **hash-identical to a `cmd`-redirected console copy**
(5,729 bytes both) — after one fix: the first Windows run's file was 64 bytes short of a
64-line console, because the CRT's original fd 2 is in TEXT mode (CRLF) and the log was
opened binary (31e035c opens it text-mode; gotcha 529). A 45 s headless renderer boot on
czwin: log 499,289 bytes == console 499,289 bytes, with `[vk] driver: NVIDIA - 610.62`
and `vblank #10000`. The same exe under the dev box's Wine 11 prints `[diag] os: Windows
10.0 build 19045 under Wine 11.0` — the line a Proton report will carry. **czamd's address had
moved** (192.168.0.60 → .27 by DHCP; `~/.ssh/config` and `part103-amd-windows-perf-plan.md`
§4 updated). Once reached: the part-105 exe deployed (**the previous exe, part 103's
ab80b87, was overwritten** — the backup step failed on PowerShell syntax before the
copy; `cz_runtime_part101/102/golden_sync.exe` remain and ab80b87 rebuilds from git),
and **`--diag` on the RX 6600 under AMD's proprietary driver 26.8.1**: every REQUIRED
feature present, `D24_UNORM_S8_UINT sampleable: no -> D32_SFLOAT_S8_UINT` (part 100's
hand derivation, now printed by the tool), sample counts 0xf/0xf so 2x is available,
timestamp period 10 ns, file == console by hash. That is the AMD-proprietary half of H3;
RADV on the same GPU is still the operator's live-USB run.

## §1 Part 106 — autonomous, in order

0. ~~**Compile on Windows the moment czwin answers**~~ — DONE, and the czamd `--diag`
   too (above). Original text:: `git pull --ff-only`, build, run
   `cz_runtime.exe --diag` and a renderer boot, and `cmp` the log against a captured
   stderr as part 105 did on Linux. The Windows-only code is `log_file.cpp`'s
   `_pipe`/`_dup2`/`SetStdHandle` block and `RunDiag`'s `RtlGetVersion` +
   `wine_get_version` block. Also run the shipped zip under the dev box's Wine with
   `--diag` — it should print `under Wine 11.x`.
1. **If the operator chose the rebuild (§1b item 0): `tools/release_build_oldbase.sh`
   (Linux tar + AppImage), the Windows zip on czwin, both gates, hashes into the notes** —
   exactly part 104's §6eu §1 procedure. Add "Changed" lines to the notes: the log
   file, `--diag`, the named-feature refusal, the gamescope line.
2. **Part 105's §1 items 2-3 are unchanged**: the crowd-route confirmation on the
   old-base binary (three runs an arm), the park if it recurs. `part105-kickoff.md` §1
   has the detail. ~~Item 1, the golden pack's czamd number~~ — **DONE, same evening**:
   migration boot 24.9 s once (9,819 loose files folded into a 135 MB pack), then
   **94.0 ms** from the pack on the next boot — the dev box's per-MB rate, so the
   "under ~50 ms" gate was sized for a store half the real one (`phase5-notes.md`
   §6eu §2, retraction in place).
3. **Watch for the first Deck issue** (`.github/ISSUE_TEMPLATE/steam-deck-report.md`).
   Read its `--diag` block against `steam-deck-plan.md` §2 in this order: the `[vk]
   driver:` line (RADV vs AMDVLK, Mesa version), the requirements table (any ABSENT
   REQUIRED row is the whole report), the depth-format line, the `[host] gamescope`
   line, the settings line (a carried-over resolution). If the renderer was refused,
   the fix is the named feature; if it was not, the log's last lines are the lead.

## §1b Part 106 — the operator's

0. **Rebuild v1.0.2 at the part-105 head, or publish as is and call this v1.0.3.**
1. **The RADV test on czamd from a Fedora live USB** (`steam-deck-plan.md` §3 item 5):
   run the AppImage's `--diag` first (that alone settles H3's feature half), then the
   game; bring back `cz_diag.txt` and `cz_runtime.log`. This is the only RDNA2+RADV
   in reach and it is the Deck's GPU generation under the Deck's driver.
2. **Post the Deck test request** — `docs/steam-deck-testing.md` as a pinned discussion
   or in the release notes, once a build with the log file is published.
3. Part 105's §1b items (the czamd black square, the vertex-recipe licensing call, the
   seed's orphan vertex shaders) stand unchanged.

## §2 What already exists and must not be rewritten

- `runtime/host/log_file.{h,cpp}` — the tee; `LogFile::Begin/Flush/End/Path`. Every
  `_Exit`/`_exit` path already calls `Flush`; a NEW exit path must too.
- `runtime/main.cpp` `RunDiag`, `runtime/host/window.cpp` `Host_DiagVideo` +
  `UnderGamescope`, `runtime/gpu/vk_renderer.cpp` `kFeatureReqs`/`QueryDeviceCaps`/
  `EvaluateRequirements`/`PrintDriverLine`/`PickEdramDepthFormat`/`VkRenderer_Diag`.
  A new device feature goes in the TABLE, nowhere else.
- `docs/steam-deck-testing.md`, `.github/ISSUE_TEMPLATE/{steam-deck-report,bug-report}.md`.
- The packaging scripts' log-file exclusions (all three).
- Everything in `part105-kickoff.md` §2 (the old-base build, the AppImage, the gates,
  the czamd harness).
