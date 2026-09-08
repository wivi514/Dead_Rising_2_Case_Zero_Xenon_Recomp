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
