# Release notes — v1.0.2

**This is the text to paste into the GitHub Release body.** Binaries are commit
`482b47f` (all three artifacts were built from that source; the docs commits after it
change no code). It carries part 104's runtime changes (the golden texture store as one
pack file, the AppImage data root, Wayland-first on Linux, the window title and icon) and
the first Linux artifacts built on the old base.

**The release is frozen at the tag**: if any artifact is EVER rebuilt, refresh its hash
below before attaching.

---

A maintenance release for Linux — the glibc requirement drops, there is an AppImage, and
a one-frame-per-second defect on Wayland desktops is fixed — plus one boot-time
improvement on both platforms.

**Upgrading from v1.0.1:** unpack over your existing folder, or anywhere — saves and
settings live outside the game folder and are untouched. Your unpacked game data and
shader cache are reused.

### Linux: the glibc requirement drops from 2.43 to 2.35, and there is an AppImage

- **Older distributions now start.** v1.0.0 and v1.0.1 were linked on a Fedora 44
  machine and refused to run anywhere with a glibc older than 2.43
  (`GLIBC_2.43 not found`). This build is linked on Ubuntu 22.04, so the requirement is
  **glibc 2.35** — Ubuntu 22.04, Debian 12, Fedora 36 and anything newer. The shader
  translator we bundle needs 2.34 on its own, so this is as far down as it goes.
- **`CaseZeroRecomp-linux-x86_64.AppImage`**, a single file. Put it anywhere, make it
  executable, run it. It creates `assets/package/` NEXT TO ITSELF on the first launch —
  that is where your game package goes (or drag the package onto the launcher). The
  `.tar.zst` is still provided and is the same build.
- **Fixed: one frame per second on Wayland desktops.** The Linux builds preferred X11
  (through XWayland) over Wayland, and on at least NVIDIA + XWayland that path presented
  exactly one frame a second — v1.0.0 and v1.0.1 both did this. The game now uses Wayland
  when the session offers it and falls back to X11 otherwise; setting `SDL_VIDEODRIVER`
  yourself still wins.
- The bundled XMA audio decoder now carries its hand-written x86 assembly (the earlier
  Linux builds shipped the plain-C fallback).

### Both platforms

- **Controller vibration.** The game's rumble now reaches your pad — hits, weapons,
  everything the Xbox 360 version shook the controller for, at the length the console
  gave them whatever your frame rate. Any pad SDL drives with
  rumble support; the game's own vibration option still applies. Set `CZ_NO_RUMBLE=1`
  to switch it off.
- **Lights and glows no longer wrap to the opposite edge of the screen**, and the
  title screen's zombies no longer show in the far corner: textures now clamp at
  their edges the way the game asks, where they used to wrap.
- **Door transitions at 21:9 keep their proportions.** Walking through a door used
  to stretch the picture until you moved.
- **Mouse wheel: one notch, one item.** Scrolling through the inventory needed two
  notches per step for some players; every notch counts now.
- **A quieter log.** The audio decoder no longer writes a warning line thirty times a
  second; those lines were harmless (the decoder costs about 3% of one core) but they
  made it look like the culprit for stutter that came from elsewhere.
- **Faster start-up after many sessions.** The runtime remembers small streamed textures
  it has seen (this is what keeps a certain gravel floor from rendering black), one file
  each — and that directory was being re-read file by file at every launch, one second
  and growing. It is now a single pack file; the old per-file store is folded into it on
  the first launch and removed.

### Changed

- **The window title is just the game's name and the frame rate.** The developer note
  about enabling the renderer is gone from the title bar.
- **The window wears the game's own icon** — the 64x64 tile from your unpacked game
  (`assets/game/X_IMAGEID_GAME.PNG`), so it is read from your copy and never shipped. It
  appears once the first run has unpacked the game. On Linux under Wayland the title bar
  keeps the desktop's icon (SDL2 cannot set one there); Windows and X11 show it.

### Everything from v1.0.1 still applies

The whole game start to finish, 60 fps, native keyboard/mouse with real key icons, the
restored PC options screen, MSAA 2x, adjustable FOV, real Xbox 360 audio, and a first
run that builds what it needs from your own copy of the game.

**You must own the game.** No Capcom content ships in this repository or in these
downloads.

### How to install

1. Download the build for your system below and unpack it anywhere (the AppImage needs
   no unpacking: `chmod +x` it).
2. Copy your own XBLA package file (~825 MB, no file extension — on the console it
   lives at `Content/0000000000000000/58410A8D/000D0000/<long name>`) into
   `assets/package/` (beside the executable, or beside the AppImage after its first
   launch), or just drag it onto the launcher.
3. Run `cz_runtime.exe` (Windows), `./cz_runtime` (Linux tarball) or the AppImage. The
   first run sets everything up by itself under a progress bar. Later launches start
   straight into the game.

### Requirements

- GPU + driver with **Vulkan 1.3**.
- **Windows** 10+ x86-64, or **Linux** x86-64 with **glibc 2.35 or newer** (the AppImage
  additionally needs FUSE, as every AppImage does; without it run it with
  `--appimage-extract-and-run`).
- Your own copy of the Dead Rising 2: Case Zero XBLA package (~825 MB).
- ~2 GB free disk after first-run unpacking.

### Known issues (minor)

- **On AMD GPUs** (tested on an RX 6600): the zombies on the **main menu** may flicker
  in and out — gameplay itself was unaffected in our testing — and a **flickering black
  square** has been seen in-game. Both are being investigated; NVIDIA is unaffected.
  Also on AMD, one launch after a GPU driver update may sit on a black screen for a
  minute or two while the driver recompiles its pipeline cache — it is not hung, and
  later launches are fast. Rarely, a launch on that machine has sat on the loading
  screen without ever reaching the title; quit and relaunch.
- A subtle **shading flicker on Chuck's hair** in motion; real hardware does not show
  it and it is being tracked.
- The occasional spot may shade slightly differently than the console.
- One player reported the **sound cutting out during the last few cutscenes** of the
  game. We have not been able to reproduce it. If it happens to you, please open an
  issue and attach `cz_runtime.log` from the game's folder (the file from that
  session) — it records what the audio decoder was doing, and that is what we need.
- **No macOS build yet** — awaits test hardware, nothing structural.

### Legal

This project is not affiliated with, or endorsed by, Capcom or Microsoft. Dead Rising
2: Case Zero is © Capcom Co., Ltd. The downloads contain the recompiled program and
this project's own runtime/art only; all game content is read from, or generated at
first run from, the player's own copy. Project code: PolyForm Noncommercial 1.0.0.
Third-party licences: `THIRD_PARTY.md` inside each bundle. Built on hedge-dev's
XenonRecomp and XenosRecomp.

### Checksums (SHA-256)

```
3e704f8869d5d5bb61ad353960da75628a5030cc10fd62f8b4d033d29d60f226  CaseZeroRecomp-linux-x86_64.tar.zst
73b054185b68125d142df5deffe954dc7226de00ecef8fd8045f1b055e05f1ac  CaseZeroRecomp-linux-x86_64.AppImage
56b5cb890884de1373eeace4abefda62013de23b73934d68f838ce59bf2f74ed  CaseZeroRecomp-windows-x86_64.zip
```
