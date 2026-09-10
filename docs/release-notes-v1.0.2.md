# Release notes — v1.0.2

**This is the text to paste into the GitHub Release body.** Binaries are commit
`8b67e6a` (all three artifacts rebuilt from that source on 2026-09-09; the docs commits
after it change no code). It carries parts 104-108: the golden texture
store as one pack file, the AppImage data root, Wayland-first on Linux, the window
title and icon, the log file and `--diag`, the device-local geometry mirror, the
keyboard/mouse start-up scan fix, 16:10 resolutions, MSAA as a setting, the window
following the resolution, controller vibration, and the night's picture and input
fixes — all on the first Linux artifacts built on the old base.

**The release is frozen at the tag**: if any artifact is EVER rebuilt, refresh its hash
below before attaching.

---

A large update on both platforms: the GPU cost of crowds roughly halves, keyboard/mouse
players no longer lose a CPU core for the first two and a half minutes of every session,
controller vibration arrives, 16:10 screens (the Steam Deck's) are supported, MSAA is a
setting, and a round of picture and input fixes. For Linux the glibc requirement drops,
there is an AppImage, and a one-frame-per-second defect on Wayland desktops is fixed.

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

### Both platforms: performance

- **Crowds cost the GPU about half of what they did.** The game's geometry was being
  fetched across the PCIe bus for every draw; it now lives in a mirror in video memory.
  Measured at a full crowd at 1080p with 2x MSAA on an RTX 3070, the GPU's frame went
  from 8.8 ms to 4.0 ms, and wall time fell 22-24% wherever the GPU was the limit. On a
  GTX 1060-class card this is the difference between a crowd in the mid-30s and one
  near 60. `CZ_VK_NO_STORE_MIRROR=1` turns it off, and is the first thing to try if
  you ever see a one-frame stale mesh.
- **Keyboard/mouse: a whole CPU core is back for the first minutes of play.** Since
  the native keyboard/mouse support arrived, a background scan for the game's
  prompt art ran at 100% of one core for the first 137-150 s of EVERY session. It now
  finishes in under a tenth of a second. On a 4-core CPU this was worth 1.5-1.8 ms a
  frame at the crowd; every crowd window measured under 16.7 ms afterwards.
  `CZ_KBM_SCAN_LEGACY=1` is the old behaviour.
- The game's draw thread no longer spins a core while it waits for the GPU; it sleeps
  until the GPU signals. Frame time is unchanged by this; CPU use and heat are lower.
  `CZ_FENCE_PARK=0` is the old behaviour.
- **On a GTX 1050 Ti / Steam Deck-class GPU**, the honest recommendation is **1080p with
  MSAA off** (see the new MSAA setting below): that combination ran the crowds at
  59-65 fps in our low-end testing, where 2x MSAA held 53-54.

### Both platforms: new settings

- **16:10 resolutions.** 1280x800, 1920x1200, 2560x1600 and any 16:10 mode your display
  offers are accepted, in the launcher and in the in-game options screen. The picture keeps its
  proportions and gains a little vertical view; the HUD sits at full width in the
  central 16:9 band. The Steam Deck's native 1280x800 no longer falls back to 720p.
- **MSAA is a setting** (Off / 2x / 4x) in the options screen and the launcher. It
  applies at the next launch — the row shows a star until then.
- **In windowed mode the window follows the resolution.** It opens at the resolution
  you chose (fitted to your desktop), and resizes when you apply a new one or leave
  fullscreen. A maximised window stays maximised.
- **The start-up launcher takes a controller.** D-pad or left stick to move, A to
  select, START to play from any row, B to quit. It used to read arrow keys and Enter
  and nothing else, which made it a dead end on a handheld or a couch setup with no
  keyboard attached.
- **Ultrawide resolutions are in the launcher's list**: 2560x1080, 3440x1440 and
  3840x1600. Picking a 21:9 resolution is what turns on the game's wide mode, and until
  now the only 21:9 entry you could reach there was your own desktop's size.
- **A log file for bug reports.** Every run writes `cz_runtime.log` beside the
  executable (the previous run is kept as `cz_runtime.log.1`), and
  `cz_runtime --diag` writes `cz_diag.txt` with your GPU, driver, and the Vulkan
  features the game found. Attach both to any issue.

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
- **Keyboard and mouse: telling a survivor to wait at a spot works.** Aim with the
  right mouse button and press Q, as the prompt says; it used to call the survivor to
  you instead. Q now acts as the Y button everywhere the game reads it.
- **Mouse wheel: one notch, one item.** Scrolling through the inventory needed two
  notches per step for some players; every notch counts now.
- **Fixed: closing the launcher without playing crashed instead of exiting.** Backing
  out with Escape (or the window's close button) ended the process with an abort rather
  than a clean exit. The same fault hit four other exits, including the one that
  reports missing game data — the case where the log file is exactly what we ask you to
  send.
- **A quieter log.** The audio decoder no longer writes a warning line thirty times a
  second; those lines were harmless (the decoder costs about 3% of one core) but they
  made it look like the culprit for stutter that came from elsewhere. The controller
  vibration request no longer logs a line on every poll either (it was half of a
  session's log); `CZ_RUMBLE_TRACE=1` prints the changes.
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

- **On AMD GPUs** (tested on an RX 6600): a **flickering black square** can appear in
  game. **If you hit it, alt-tab out and back, or press Win+PrintScreen** — either one
  clears it. We see this on our own AMD test machine and no player has reported it;
  it is being investigated and NVIDIA is unaffected. The zombies on the **main menu**
  may also flicker in and out on that GPU. Also on AMD, one launch after a GPU driver
  update may sit on a black screen for a minute or two while the driver recompiles its
  pipeline cache — it is not hung, and later launches are fast. Rarely, a launch on
  that machine has sat on the loading screen without ever reaching the title; quit and
  relaunch.
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
558937b1b8f2d84a6c6832d09417d11c53c8f7281aab658187127f8659ce46ab  CaseZeroRecomp-linux-x86_64.tar.zst
964a62c04c9b8eb343e1af359dcd3295057ceef95fcd9d44e048fa1b64cb448c  CaseZeroRecomp-linux-x86_64.AppImage
b500033f5928d94eb075f48bf8207b444f72720dd3a1a22d49f63efedb0fa558  CaseZeroRecomp-windows-x86_64.zip
```
