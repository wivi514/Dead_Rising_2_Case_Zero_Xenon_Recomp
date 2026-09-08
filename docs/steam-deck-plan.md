# Steam Deck — why v1.0.1 did not run there, and the plan to make v1.0.2 (or v1.0.3) run

**Written 2026-09-08 at the end of part 104, for the next session.** Operator's report:
*"People said the game didn't work on Steam Deck when trying v1.0.1 with windows and linux
build … someone said glibc is not compatible on steam deck but i think it's not just that
since windows build also don't work."* Nobody on this project has a Deck, so this plan is
built the way part 99's hang plan was: name what is CERTAIN, rank the hypotheses for the
rest by what each would cost to test, and make the first deliverable the one thing that
turns a player's "didn't work" into evidence we can read.

## §0 What the Deck is, as far as this runtime cares

- **GPU: AMD "Van Gogh" APU, RDNA2 (gfx1033), 8 CUs**, driven by **RADV** (Mesa's open
  Vulkan driver) — under BOTH our Linux build and our Windows build through Proton. That is
  the one component the two failing builds share and the one this project has never run
  on: the dev box is NVIDIA, czamd is RDNA2 under AMD's *Windows* driver. Every RDNA2 fact
  from part 100-103 (no sampleable D24S8 → the depth negotiation; DCC on single-sample
  colour; MSAA 2x free) is hardware and should hold; every *driver* fact may not.
- **CPU: Zen 2, 4 cores / 8 threads, 2.4-3.5 GHz.** The thread budget was floored for
  6-core machines in part 100 (`[threads]` block); 4 physical cores is a rung below
  anything measured. Performance will be poor before it is a bug — "slow" and "did not
  work" must be kept apart in the reports.
- **OS: SteamOS 3.x, Arch-based, glibc in the 2.38-2.41 range for the 3.6/3.7 releases
  players run in 2026** (the tester's `ldd --version` is the number to record; nothing
  shipped by Valve carries 2.43). Game Mode runs everything under **gamescope** (a
  Wayland compositor that presents X11 clients through XWayland); Desktop Mode is KDE
  Plasma on Wayland. The root filesystem is read-only; `~` is writable; FUSE is present
  (AppImages are common there).
- **Display 1280x800**, one screen, no external monitor by default. Steam Input presents
  the Deck's controls as an XInput-style pad to SDL.
- **Windows builds run under Proton** (Wine + DXVK/vkd3d; a plain Vulkan app like ours
  goes Wine → the host's `libvulkan` → RADV, no DXVK involved).

## §1 What is CERTAIN, and what it already tells us

1. **v1.0.1's Linux build cannot start on any SteamOS.** Its glibc floor was 2.43
   (part 104 §6eu §1 measured it off the artifact); SteamOS is ≤2.41. The player who
   said "glibc is not compatible" was right about the Linux build, and the message they
   saw was `GLIBC_2.43 not found`. **v1.0.2's floor is 2.35** (built on Ubuntu 22.04,
   gated at 2.35 and refused at 2.34 — `release-plan.md` §9.9), which is below every
   SteamOS. So for the Linux build, v1.0.2 removes the certain cause; whether a second
   cause hides behind it is §2.
2. **The Windows build's failure is NOT glibc** — Wine supplies its own C runtime to the
   PE — so the operator's instinct is right: there is a second cause, and it is either in
   the Proton layer (§2 H2) or in RADV (§2 H3), and H3 would also stop the Linux build
   the moment glibc lets it start.
3. **No shipped build writes a log file.** stderr goes to the console; a player who
   double-clicks an exe or an AppImage has NO console, and Steam's Game Mode has none
   either. Every report so far is "didn't work" because that is all a player CAN say.
   This is why §3's first item is a log file and a `--diag` mode, before any fix.

## §2 Hypotheses, ranked by (likelihood × cost to test), each with its test

| # | hypothesis | affects | how to test WITHOUT a Deck | how a Deck tester settles it |
|---|---|---|---|---|
| H1 | glibc 2.43 floor (v1.0.1 Linux) | Linux | done: measured, fixed in v1.0.2 | runs v1.0.2's AppImage; the refusal line is gone |
| H2 | Proton/Wine cannot run the PE: `VirtualAlloc2` with `MEM_RESERVE_PLACEHOLDER` for the 4 GB guest map (memory.cpp:95 — Wine implemented placeholders late and Proton pins Wine versions), `dxcompiler.dll` under Wine, `SHGetKnownFolderPath(SavedGames)`, the SDL progress window | Windows | **the dev box has wine64** — run the shipped zip: `--smoke`, then a renderer-off headless boot, then renderer-on (Wine's Vulkan → NVIDIA); part 104 started this, §4 | Proton log (`PROTON_LOG=1`) from the Deck |
| H3 | RADV lacks or differs on something the renderer REQUIRES: `shaderInt64`, `textureCompressionBC`, `descriptorIndexing`/`runtimeDescriptorArray`, `dynamicRendering`, `independentBlend`, `fillModeNonSolid`, `depthClamp` (vk_renderer.cpp ~6930-7040); the EDRAM depth format negotiation choosing something RADV cannot sample; a format/feature the AMD Windows driver has and RADV does not | both | **Mesa's lavapipe is installed here** (`/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`) — a software Vulkan sharing Mesa's common layers; boot the dev binary headless on it (part 104 started this, §4). **The real test is RADV on RDNA2: boot czamd from a Fedora live USB** and run the v1.0.2 AppImage — same GPU class as the Deck, same driver, the operator's own hardware | `vulkaninfo --summary` + our log's `[vk] device` block |
| H4 | The first-run flow on a Deck: the 825 MB package copy, ~2 GB free, the disc shader build (9 s on 16 cores → ~40-60 s on 4), the pre-warm's 1,365 pipelines on RADV's compiler — a boot that LOOKS hung for minutes in Game Mode with no console | both | time the first run with `taskset -c 0-3` on the dev box; read the progress window's coverage of each step | the tester waits 5 minutes before calling it hung; the log's timestamps |
| H5 | gamescope: SDL's window under Game Mode. Part 104's `wayland,x11` hint (79ef1b7) prefers Wayland when `WAYLAND_DISPLAY` is set — under gamescope the game is expected on XWayland and a native-Wayland SDL2 client may not present or be given focus/input | Linux (Game Mode) | none honest here (no gamescope on this box); `gamescope -- ./cz_runtime` can be installed from Fedora's repo and is worth one run | `SDL_VIDEODRIVER=x11` as an override test; the log's "up on SDL video driver" line |
| H6 | 1280x800 and the fullscreen mode: `display_mode=2` (borderless) + the settings' persisted resolution from another machine; the launcher window (720x460) under gamescope | Linux | force `CZ_VK_RES=1280x800` and a fresh settings file | screenshot |
| H7 | Steam Input / the Deck's pad through SDL: the launcher needs a pad or keyboard to get past its screen; in Game Mode there is no keyboard | both | none | can they reach the game from the launcher with the pad alone? |
| H8 | 4 physical cores: the thread budget hands out fewer workers than the pipeline warm and the golden writer assume; a starvation that reads as a hang | both | `CZ_WORKERS=` and `taskset -c 0-3` on the dev box, read the `[threads]` block | the `[threads]` block in the log |

H2 and H3 are the ones that explain "both builds fail"; H1 alone cannot. H4-H8 are the
ones a first-time Deck player would report as "didn't work" even when H1-H3 are fixed.

## §3 The next session's deliverables, in order

1. **A log file, on every platform, always.** `cz_runtime.log` beside the executable
   (or in the AppImage's beside-the-image root; on Windows beside the exe), stderr tee'd
   from the first line, rotated once (`.log.1`). Plus **`cz_runtime --diag`**: print and
   exit — OS and glibc (`gnu_get_libc_version`), CPU count, the `[threads]` budget, the
   Vulkan instance/devices with driver name and version, each REQUIRED feature and
   extension as present/absent, the chosen depth format, the SDL video driver, the root
   and its source. One line per fact, so a player can paste it into an issue. **This is
   the deliverable that turns the next report into a diagnosis**; without it §2 stays a
   table of guesses.
2. **Run §4's two probes to their conclusion** (Wine and lavapipe) if part 104's runs did
   not finish, and fix whatever they name. If `VirtualAlloc2`+placeholders is the Wine
   failure: the Windows memory map falls back to `VirtualAlloc(MEM_RESERVE)` of 4 GB plus
   `VirtualAlloc(MEM_COMMIT)` of the physical views inside it — the same shape the Linux
   `mmap` path already has (memory.cpp:162) — behind a runtime check for
   `VirtualAlloc2` failing, so real Windows keeps the placeholder path.
3. **The renderer's REQUIRED-feature list becomes a checked list with a fallback or a
   message per item**: today a missing feature is a `vkCreateDevice` failure and one
   line; on RADV the most likely candidates are the ones AMD's Windows driver has and
   Mesa's may report differently. Each required feature is queried, missing ones are
   listed by name in the log and in `--diag`, and the ones the renderer can live without
   (`fillModeNonSolid`, `occlusionQueryPrecise`, `shaderClipDistance` already are
   optional) are made optional.
4. **gamescope-awareness for the video-driver hint**: when `XDG_CURRENT_DESKTOP` or
   `XDG_SESSION_DESKTOP` names gamescope, or `GAMESCOPE_WAYLAND_DISPLAY` is set, do NOT
   prefer Wayland — the 1 fps defect the hint fixes (§6eu §5) is an NVIDIA+XWayland
   desktop case, and gamescope's X11 path is the tested one for games. Test it here with
   Fedora's `gamescope` package (`gamescope -W 1280 -H 800 -- ./cz_runtime`).
5. **The operator's RADV test: boot czamd from a Fedora Workstation live USB** and run the
   v1.0.2 AppImage from a USB stick with the package beside it. That machine's RX 6600 is
   the Deck's GPU generation under the Deck's driver, and it is the only RDNA2+RADV in
   reach. What to bring back: `cz_runtime.log`, `--diag`, and whether it reaches the title
   at 1280x800. **This single test settles H3 for the Linux build and, through
   Proton-on-Linux later, most of H2.**
6. **A Deck test request**, written for a player (a GitHub issue template or a pinned
   discussion): install the AppImage from Desktop Mode into `~/Games/CaseZeroRecomp/`,
   put the package beside it, run it once from Desktop Mode (a terminal shows the
   first-run progress), then add it as a non-Steam game for Game Mode; attach
   `cz_runtime.log` and `--diag`'s output; say "slow" or "black" or "closed" rather than
   "didn't work". For the Windows build under Proton: `PROTON_LOG=1` and the
   `steam-<appid>.log` it writes.
7. **A Deck row in the README's requirements and known issues**, honest about what has
   and has not been run.

## §4 What part 104 already ran (results filled in below when the probes finished)

Two probes were started on 2026-09-08 with the shipped v1.0.2 artifacts and the dev
binary, to give this plan a starting point rather than a blank table:

- **Wine on the dev box** (`wine64`, Fedora's Wine, NVIDIA): the shipped
  `CaseZeroRecomp-windows-x86_64.zip` unpacked under a fresh prefix — `--smoke`, then a
  headless renderer-off boot against the repo's unpacked game, then a headless renderer-on
  boot (Wine's Vulkan → NVIDIA). Log: `~/DR2CZ-troubleshooting/part104/wine/`.
- **lavapipe on the dev box** (`VK_ICD_FILENAMES=…/lvp_icd.x86_64.json`): the dev binary
  headless with the renderer on. Log: `~/DR2CZ-troubleshooting/part104/lavapipe.log`.

**Results (2026-09-08, same evening):**

| probe | result | what it settles |
|---|---|---|
| Wine 11.0 (staging), `--smoke` on the shipped exe | OK | the PE loads and links under Wine |
| Wine, headless boot, renderer off, 90 s | boots; 653 `.big` references; `runtime: guest memory at 0000000142760000, heaps ready` | **`VirtualAlloc2` + `MEM_RESERVE_PLACEHOLDER` works under Wine 11** — H2's first candidate is refuted for this Wine; Proton's Wine version is the remaining question (Proton 9/10 are Wine 9/10-based; placeholders arrived in Wine 8.x) |
| Wine, headless boot, renderer on (Wine's Vulkan → NVIDIA), 120 s | device created, `vblank #1000`, the title renders: 70-118 fps median at 3440x1440 internal | the renderer runs through Wine's Vulkan layer |
| Wine, `--build-shader-cache` on the disc bank | **1,367 translated, 0 failed, 15 s** through `dxcompiler.dll` | **DXC works under Wine** — the first-run flow's only DLL-shaped risk is gone |
| lavapipe (Mesa software Vulkan 1.4.354), dev binary headless | device created, EDRAM depth D24S8 chosen, `vblank #1000`, title at 5 fps (software) | every REQUIRED feature is satisfied by a Mesa driver; the Mesa common layers accept our device creation |

So H2 (Proton) is mostly refuted from this box, and H3 (RADV) is narrowed to the
RADV-specific half — device features Mesa shares are fine. Two things the probes also
showed: (a) **the MSAA sample-count check only walks DOWN**: lavapipe reports counts
`0xd` (1x, 4x, 8x, no 2x) and the renderer refused MSAA with a message saying "neither 4x
nor 2x", which is false for 4x — the loop at vk_renderer.cpp ~7150 halves from the
request and never tries 4x; RDNA2 has 2x so the Deck is unaffected, but the message lies
and belongs on §3 item 3's list; (b) the Wine boot logged three `SIBLING MISS` lines
(`cl.txt`, `serial.bin`) that the Linux boot does not — a path-case or extraction
difference to read before calling it noise. **The strongest remaining suspect for "both
builds fail" is therefore RADV itself, and §3 item 5 (czamd on a live USB) is the test.**

## §5 What NOT to do

- Do not buy a fix for a hypothesis before a log names it. Part 99 spent three parts on a
  hang whose real cause (a semaphore limit) was one line, and it was found by an
  instrument, not by a theory.
- Do not treat "the Windows build fails on the Deck" as one report: Proton version,
  install path and whether the first run ever finished are three different reports.
- Do not lower the Linux floor further for the Deck's sake; 2.35 is below every SteamOS,
  and 2.34 is the DXC prebuilt's hard limit (gotcha 520).

## §6 Part 105 — what was built, what was measured, and what each of §3's items came to (2026-09-08)

The operator's instruction opening the part: *"Do the steam deck plan."* Everything below
was done on the dev box (NVIDIA, KDE Wayland, Fedora 44); nothing has touched a Deck or
RADV. §3's numbering is kept.

**Item 1 — the log file and `--diag`: BUILT.** `runtime/host/log_file.{h,cpp}`: fd 2 is
redirected onto a pipe and one thread copies it to the original stderr and to
`cz_runtime.log` beside the data root (the bundle directory; beside the `.AppImage`; the
repo root for a dev tree), rotated once. It is a descriptor-level tee so it catches every
writer — stdio, the crash reporter's raw `write(2)`, SDL's and DXC's own messages. The
three `_Exit` quit paths, the SIGTERM/SIGINT handler and the crash reporter drain the pipe
first (`LogFile::Flush`). Gated three ways, each of which could have failed: (a) a 45 s
headless renderer boot ending in SIGTERM — the file is byte-identical to the console copy
over 15,147 lines; (b) `kill -SEGV` at 20 s — both copies end with the crash report; (c)
`CZ_NO_LOG_FILE=1` — no file, one line saying so. The first `--diag` run FAILED (b)'s
sibling: the console copy was 5,828 bytes short of the file because `End()` closed the
original stderr descriptor under the thread still writing the pipe's tail to it (fixed:
restore fd 2, join, then close). `cz_runtime --diag` prints the OS and glibc (under Wine,
the Wine version from ntdll's `wine_get_version`), the session variables that choose a
video driver and name a Deck, the thread budget, the root and first-run state, the
carried-over settings, SDL's drivers and displays, and every Vulkan device with its
DRIVER NAME AND VERSION plus the requirements table verdict; it writes `cz_diag.txt`
through the same tee and exits 0 when the pick can run the renderer. The dev box's own
block is in `docs/instruments.md`'s new section.

**Item 2 — §4's probes: they had finished; their two loose ends are closed.** (a) The
MSAA sample-count walk now tries the request and then the other count (2x ↔ 4x) and the
refusal message is true when it prints. (b) **The `SIBLING MISS` lines are NOT a Wine
difference — retracted.** `cl.txt`, `serial.bin` and `capcom.txt` print on the Linux boot
too (`part104/title_check.log` and `lavapipe.log`, six lines each); part 104 grepped the
wrong log. They are the title probing files the package never carried (file_imports.cpp
already documents `capcom.txt` and `serial.bin`); noise on every platform. The
`VirtualAlloc2` fallback was NOT built — §5's rule: no log has named it.

**Item 3 — the required-feature list is a table: BUILT.** `kFeatureReqs` in
`gpu/vk_renderer.cpp`, one row per feature with where it lives, REQUIRED/optional, and
the reason. Bring-up queries the device first, requests only what is present, and a
missing REQUIRED feature ends bring-up with the feature named and the driver named
(`THIS DEVICE CANNOT RUN THE RENDERER — missing REQUIRED Vulkan feature: …`) where it
was `vkCreateDevice failed: VkResult -7`. A device below Vulkan 1.3 is refused by name
too. `fillModeNonSolid` and `depthClamp` became OPTIONAL — a grep found no consumer
(every `polygonMode` is FILL, no pipeline enables depth clamp). `shaderInt64` stayed
REQUIRED on evidence: a capability census of the built cache finds Int64 in **450 of
450** translated shaders. **That census first read 0 of 450** — the scanner tested for
capability 22 (Int16) instead of 11 (Int64), and only printing the whole capability
distribution (1, 11, 43, 50, 5302, 5347) exposed it (gotcha 528). `[vk] driver: <name>
— <info>` now prints on every bring-up.

**Item 4 — gamescope awareness: BUILT, and H5 is ANSWERED on this box.** Fedora's
`gamescope` 3.16.23 was already installed. `gamescope -W 1280 -H 800 -- ./cz_runtime`
with the renderer on: the detection line printed, the window came up on the **x11**
driver, the swapchain took 1280x800 MAILBOX, and **6,645 frames presented in ~40 s**
(~165 fps at the title) — not the desktop-XWayland 1 fps of §6eu §5. The control with
`SDL_VIDEODRIVER=wayland` set outside gamescope read the SAME (5,463 frames in ~30 s,
x11), which is how the second fact surfaced: **gamescope REMOVES `WAYLAND_DISPLAY` and
`SDL_VIDEODRIVER` from the child's environment** (`gamescope -- env` shows neither; it
sets `XDG_CURRENT_DESKTOP=gamescope`, `GAMESCOPE_WAYLAND_DISPLAY=gamescope-0`,
`DISPLAY=:1`). So the part-104 hint could never have fired under gamescope and H5 was a
non-issue from the start; the new check changes no behaviour there and exists so the log
STATES the path. The log line's first wording promised "SDL_VIDEODRIVER=wayland
overrides" — false under gamescope; corrected before commit (gotcha 526).

**Item 5 — the operator's live-USB RADV test: NOT RUN (theirs).** Unchanged, and now
better instrumented: bring `cz_diag.txt` and `cz_runtime.log` back rather than a
description. `part106-kickoff.md` §1b carries it.

**Item 6 — the Deck test request: WRITTEN.** `docs/steam-deck-testing.md` (the player's
walk-through, Desktop Mode first, the "say what you saw" table) and
`.github/ISSUE_TEMPLATE/steam-deck-report.md` (plus a general `bug-report.md` that asks
for the log). Posting a pinned discussion is the operator's.

**Item 7 — README rows: WRITTEN.** Requirements (glibc 2.35 from v1.0.2, a Steam Deck
row that says "not yet verified"), Known issues (the Deck, Proton untested, Wine 11
runs it), and the log/`--diag` sentence in the install section; the bundle README's
"run from a terminal" paragraph became "attach `cz_runtime.log`, run `--diag`".

**H4, priced on this box.** `--build-shader-cache` on the disc bank under `taskset
-c 0-3,8-11` (four cores and their siblings — the Deck's shape, at a Ryzen 7 5700's
clocks): **12.52 s wall** (98.6 s user) against **7.03 s** unconstrained the same minute.
The Deck's Zen 2 at 2.4-3.5 GHz is slower per core, so the honest estimate is tens of
seconds, under a minute; `steam-deck-testing.md` says "up to a minute". The shader step
is the largest single first-run wait after the 825 MB unpack.

**Packaging.** The runtime now writes beside its root, and for the release stage that IS
the stage: `release_package_linux.sh`, `release_package_appimage.sh` and
`release_package_windows.ps1` each drop `cz_runtime.log*` / `cz_diag.txt*` before
archiving. The clean-container gate mounts the stage `:ro`, so there the log falls back
to the temp directory and says so — the designed path, not a gate failure.

**What is owed after this part.** (1) A Windows compile of the new code — czwin was
unreachable (SSH banner timeout) all session; `log_file.cpp`'s `_pipe`/`_dup2`/
`SetStdHandle` path and `RunDiag`'s `RtlGetVersion`/`wine_get_version` block have not
been compiled by MSVC/clang-cl. (2) The three artifacts carry NONE of this: v1.0.2's
`dist/` is at 482b47f. Either rebuild all three and refresh the notes' hashes, or ship
v1.0.2 as gated and make this v1.0.3 — the operator's call; a Deck tester needs a build
WITH the log file, so the rebuild is the useful one. (3) Item 5. (4) A Deck report.
