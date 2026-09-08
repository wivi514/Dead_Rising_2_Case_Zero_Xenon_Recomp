# Release notes — v1.0.2

**This is the text to paste into the GitHub Release body.** Binaries are commit
`c7ee332` (all three artifacts were built from that source; the tooling and docs commits after it change no code). It carries
part 104's two runtime changes (the golden texture store as one pack file, and the
AppImage data root) and the first Linux artifacts built on the old base.

**The release is frozen at the tag**: if any artifact is EVER rebuilt, refresh its hash
below before attaching.

---

A maintenance release for Linux, plus one boot-time improvement on both platforms.

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
- The bundled XMA audio decoder now carries its hand-written x86 assembly (the earlier
  Linux builds shipped the plain-C fallback).

### Both platforms

- **Faster start-up after many sessions.** The runtime remembers small streamed textures
  it has seen (this is what keeps a certain gravel floor from rendering black), one file
  each — and that directory was being re-read file by file at every launch, one second
  and growing. It is now a single pack file; the old per-file store is folded into it on
  the first launch and removed.

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
9bf180c74322647dc9ebdb872954a59293ba7b4bba21d04978c4249c8909f58f  CaseZeroRecomp-linux-x86_64.tar.zst
ec5f263b45e15cd67c727d3677cfa825cbd70b44366e44fd4ad2a057c33c0a45  CaseZeroRecomp-linux-x86_64.AppImage
ab69a837c519af71344686167c13f798876a48f4f51d61666843835f23cf5975  CaseZeroRecomp-windows-x86_64.zip
```
