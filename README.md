# Dead Rising 2: Case Zero — Native PC Port

Play **Dead Rising 2: Case Zero** (Capcom / Blue Castle Games, 2010 — the Xbox
360 exclusive prologue to Dead Rising 2) natively on your Windows or Linux PC.

This is not an emulator: the game's Xbox 360 code was translated ahead of time
into a native program ([XenonRecomp](https://github.com/hedge-dev/XenonRecomp) /
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp)), running on a
purpose-built engine with a Vulkan renderer, real XMA audio, and native
keyboard/mouse support.

> **Status: essentially complete.** The game is **100% playable start to
> finish** — it has been completed end to end on both Windows and Linux — and
> should look right in nearly all places. A few minor issues remain (listed
> below); none block progress.

**No game data is included** in this repository or the downloads. You must own
Dead Rising 2: Case Zero and supply your own copy of the game package. This
project is not affiliated with, or endorsed by, Capcom or Microsoft.

## How to install

1. **Download** the release for your system from the
   [Releases](../../releases) page:
   - Windows: `CaseZeroRecomp-windows-x86_64.zip`
   - Linux: `CaseZeroRecomp-linux-x86_64.tar.zst`
2. **Unpack it anywhere** (Windows: right-click → Extract All; Linux:
   `tar --zstd -xf CaseZeroRecomp-linux-x86_64.tar.zst`).
3. **Add your copy of the game.** You need the XBLA package file your Xbox 360
   downloaded — about **825 MB**, no file extension. On the console's storage
   it is at:
   ```
   Content/0000000000000000/58410A8D/000D0000/<a long string of letters and numbers>
   ```
   Copy that file into the `assets/package/` folder inside the game folder you
   just unpacked (copying the whole `58410A8D` folder in also works — or just
   drag the file onto the launcher window in step 4).
4. **Run the game** — `cz_runtime.exe` on Windows, `./cz_runtime` on Linux.
   The first run sets everything up by itself under a progress bar: it unpacks
   your package, prepares the game's 1,367 shaders (~10 seconds), and generates
   the menu and prompt assets from your data. Later launches go straight into
   the game.

If anything needed is missing, the game tells you exactly what and where — it
never fails with a blank screen on purpose. A `README.md` inside the bundle has
a troubleshooting section. **Every run writes `cz_runtime.log`** next to the
game folder's `assets/` (for the AppImage, next to the `.AppImage`), and
`cz_runtime --diag` prints your OS, GPU, driver and display facts and exits —
attach both to any bug report.

Your **saves and settings live outside the game folder** (Windows:
`Saved Games\Dead Rising 2 Case Zero\`; Linux:
`~/.local/share/Dead Rising 2 Case Zero/`), so you can delete or replace the
game folder at any time without losing progress.

## Controls

- **Keyboard/mouse** works out of the box with the Dead Rising 2 PC control
  scheme: WASD to move, mouse to look, left-click attack, right-click aim,
  Space jump, E to use/pick up, Tab for the map, 1/3 to cycle items, arrow
  keys for the d-pad. Every on-screen prompt shows real key icons, and the exact map
  is printed in the terminal at startup. Rebindable via a `kbmap.txt` file next
  to the executable.
- **Any controller SDL recognizes** (Xbox layout) works, with vibration, and the
  prompts switch between keyboard and controller art automatically depending on
  which one you touched last.

## Features

- The **whole game**: Still Creek, combo weapons, cinematics, save/load —
  completable start to finish.
- **Level cap raised to 50** (the original XBLA release stopped Chuck at
  level 5) with all fifteen skills unlockable — Case Zero as a full game,
  not a demo-sized one.
- **60 fps** (the game's own hidden mode, surfaced) — the original 30 fps
  pacing remains available as a setting, along with higher caps.
- **The restored PC options screen**: the Xbox build ships a dormant PC
  graphics menu; this port revives it in-game — resolution (720p up to 5K,
  applies live without a restart), display mode, vsync, shadow quality.
- **21:9 ultrawide support** — pick an ultrawide resolution (e.g. 3440×1440)
  and the game renders true widescreen. **Tip:** raise Field of View to at
  least **+10** in the options when playing ultrawide — at the stock FOV the
  wider frame stretches Chuck; +10 or more makes it look great.
- **MSAA 2x anti-aliasing** by default, adjustable field of view, a settings
  launcher, and a pipeline pre-warm so even your first session plays smoothly.
- **Real Xbox 360 audio** (XMA) through ffmpeg — music, speech, effects,
  looping ambience.
- **Built for long sessions**: texture memory recycles over a full
  playthrough — no slow degradation on marathon runs.

## Requirements

- A GPU and driver with **Vulkan 1.3** support.
- **Windows**: Windows 10 or later, x86-64.
- **Linux**: x86-64 with glibc **2.35 or newer** from v1.0.2 (v1.0.1 needed
  2.43 — see known issues). An AppImage is provided from v1.0.2.
- **Steam Deck**: not yet verified. v1.0.1 cannot start there (glibc); v1.0.2's
  Linux build removes that cause but has not been run on a Deck by anyone on the
  project. **If you try it, use the `.tar.zst` rather than the AppImage** — you have
  to keep an 825 MB package and ~2 GB of unpacked data in the folder either way, so
  the single file buys nothing and the AppImage adds a FUSE dependency. Do the first
  run in Desktop Mode: it takes about a minute and Game Mode shows no progress
  console. `docs/steam-deck-testing.md` says exactly what to try and what to send
  back.
- **~2 GB free disk space** after first-run unpacking.
- **Your own copy of the game** (see above).

## Known issues (minor — none affect playability)

- A subtle **shading flicker on Chuck's hair** in motion
  (`docs/hair-flicker-part92.md` tracks it).
- The occasional spot may shade slightly differently than original hardware;
  everything is being tracked and refined.
- **Linux glibc floor**: v1.0.1 refuses to start on distributions below glibc
  2.43 with a `GLIBC_x.yz not found` message (that includes every SteamOS).
  v1.0.2 is built on an older base (floor 2.35) and also ships as an AppImage.
- **Steam Deck**: untested by the project — see Requirements for which download to
  use. The Windows build under Proton is likewise untested and is the fallback, not
  the path to try first; on the dev box the same zip runs end to end under Wine 11,
  so a Deck failure there would be new information — please report it with
  `PROTON_LOG=1`.
- **No macOS build yet** — nothing blocks it in principle; it awaits test
  hardware.

## Building from source

The repository contains no game data, so a build needs your own package plus
sibling checkouts of the (patched) recompilers — see
`docs/xenonrecomp-upstream-bugs.md` for the local patches and
`docs/windows-build-setup.md` for the Windows toolchain. The short form
(Linux, after unpacking the game and regenerating `ppc/` per `CLAUDE.md`):

```
python3 tools/gen_import_stubs.py
cmake -S runtime -B runtime/build -G Ninja
cmake --build runtime/build -j$(nproc)
./runtime/build/cz_runtime --smoke
```

CI (`.github/workflows/build.yml`) builds the host runtime on both platforms
on every push — it proves the host code compiles; it cannot run the game.

## For developers and other porters

`docs/` is this project's full working memory, written for an outside reader
porting a *different* Xbox 360 title with the same pipeline: the findings
ledger, the 500-entry gotcha list, the `.big`/STFS/XEX format notes, the
renderer and audio build-out records, and the measurement discipline that kept
it honest. Start with `docs/xenia-capture-analysis.md` and `docs/gotchas.md`.
The original day-1 dev README is preserved at `docs/dev-readme-day1.md`.

## Support the project

If this port made your day and you'd like to support the work,
[**sponsor me on GitHub**](https://github.com/sponsors/wivi514) — it helps
keep improvements coming (and future ports: Dead Rising 2: Case West is
next). Bug reports and issues are just as valuable.

## Credits and licensing

- **[hedge-dev](https://github.com/hedge-dev)** — XenonRecomp and XenosRecomp,
  the recompiler pair this port is built on, and UnleashedRecomp for proving
  the shape (used as a structural reference only; no GPL code is copied).
- Third-party components and their licences are enumerated in
  `THIRD_PARTY.md`, generated into every release bundle.
- This repository's own code is licensed under **PolyForm Noncommercial
  1.0.0** (see `LICENSE`).
- Dead Rising 2: Case Zero is © Capcom Co., Ltd. This project ships none of
  its content; everything the game needs is read from, or generated at first
  run from, the player's own copy.
