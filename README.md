# Dead Rising 2: Case Zero — Native PC Port (Xenon Recompilation)

A native Windows and Linux port of the Xbox 360 XBLA title **Dead Rising 2:
Case Zero** (Capcom / Blue Castle Games, 2010), produced by statically
recompiling the game's PowerPC executable to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and translating its
Xenos shaders with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp),
running against a purpose-built host runtime (Vulkan renderer, XAudio-style
mixer with real XMA decoding, SDL window/input). **The game is completable
start to finish** — it has been played through on both platforms.

**No game data is included in this repository or in the release downloads.**
You must own Dead Rising 2: Case Zero and supply your own copy of the XBLA
package. This project is not affiliated with, or endorsed by, Capcom or
Microsoft.

## Player quickstart

1. Download the release for your platform and unpack it anywhere:
   - `CaseZeroRecomp-windows-x86_64.zip`
   - `CaseZeroRecomp-linux-x86_64.tar.zst`
2. Put your own copy of the XBLA package into `assets/package/`.
   It is the file your Xbox 360 downloaded, normally at
   `Content/0000000000000000/58410A8D/000D0000/<long hash, no extension>`,
   about 825 MB. Copying the whole `58410A8D` folder in also works — the
   runtime looks recursively.
3. Run `cz_runtime` (`cz_runtime.exe` on Windows). The first run does
   everything itself under a progress window: unpacks the package, builds the
   shader cache from the disc's own shader banks, and generates the patched
   menu/prompt assets from your data. Later runs start straight into the game.

If anything required is missing, the game refuses to start with a message
naming exactly what and where — it never half-starts.

## Features

- **The whole game**, playable start to finish: Still Creek, combo weapons,
  cinematics, save/load, the works.
- **60 fps** (the title's own hidden mode, surfaced) — with the shipped 30 fps
  pacing available as a setting.
- **Keyboard/mouse** with native key-cap prompt icons (our own art), DR2-PC
  default bindings, raw mouse camera, and live prompt switching when you swap
  between keyboard and pad. Player-editable `kbmap.txt` beside the executable.
- **Gamepad** via SDL — anything SDL recognizes.
- **The resurrected PC options screen**: the 360 build ships a dormant PC
  graphics menu; this port revives it in-game for resolution, display mode,
  vsync and shadow quality — resolution applies live, no restart.
- **Internal resolution scaling** (720p up to 5K), **MSAA 2x** by default,
  FOV adjustment, a settings launcher, and a pipeline pre-warm seed so even
  the first session plays smoothly.
- **Real XMA audio** through ffmpeg — music, speech and effects.

## Requirements

- A GPU + driver with **Vulkan 1.3** (dynamic rendering is required).
- **Windows**: Windows 10 or later, x86-64.
- **Linux**: x86-64 with glibc **2.43 or newer** (see known limitations).
- ~2 GB of free disk space after first-run unpacking.
- Your own copy of the game (see above).

## Known limitations

- **Linux glibc floor**: the artifact is built on a current distribution and
  refuses to start on older ones with a `GLIBC_x.yz not found` message. An
  AppImage/old-base build is planned.
- **No macOS build yet** (nothing in principle blocks it — the recompiled
  code is portable and an ARM64 path exists — it is hardware for testing that
  is missing).
- A subtle **hair-shading flicker** on Chuck in motion is a known open issue
  (`docs/hair-flicker-part92.md`); real hardware does not show it.
- The runtime expects the **full game package** and you must own the game.

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

## For other porters

`docs/` is this project's full memory, written for an outside reader porting
a *different* Xbox 360 title with the same pipeline: the findings ledger, the
500-entry gotcha list, the `.big`/STFS/XEX format notes, the renderer and
audio build-out records, and the measurement discipline that kept it honest.
Start with `docs/xenia-capture-analysis.md` and `docs/gotchas.md`. The day-1
README this file replaced is preserved at `docs/dev-readme-day1.md`.

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
