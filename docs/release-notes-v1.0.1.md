# Release notes — v1.0.1

**This is the text to paste into the GitHub Release body.** Binaries are commit
`052275c` (both legs; the last source change is `95611b9`). It carries part 98's
stutter fix (async pipeline creation + the pre-warm chain), part 99's four
changes (subtitle-language selection, the skip-intro-logos toggle, the
aim-trigger fix, Exit Game quitting to the desktop), part 100's AMD boot-hang
fix + EDRAM depth auto-negotiation + the worker floor, and part 101's pre-warm
seed union.

**The release is frozen at the tag**: if either artifact is EVER rebuilt,
refresh its hash below before attaching.

---

A maintenance release: it fixes the first-session stuttering several players
reported, adds subtitle-language selection and an intro-logo skip to the
launcher, fixes firearms firing by themselves while aiming with the mouse, and
makes Exit Game actually exit.

**Upgrading from v1.0.0:** unpack over your existing folder, or anywhere —
saves and settings live outside the game folder and are untouched. Your
unpacked game data and shader cache are reused; the menu and prompt assets
regenerate once on the first launch.

### Fixed

- **First-session stuttering.** Reported after v1.0.0 by players on their first
  run: long hitches, occasionally a multi-second freeze, easing off in later
  sessions. Graphics pipelines were being compiled on the frame thread the
  moment something new came into view, and the pre-warm meant to prevent that
  could not help a brand-new install — it can only rebuild what already
  existed. Pipelines are now built on a background thread, and the pre-warm
  chains onto shader translation so it works from the very first session.
  Measured on a simulated fresh install: zero frame-thread compiles, no outdoor
  frame over 100 ms, and a second session that starts fully warm.
- **Firearms firing by themselves while aiming** with the mouse. Holding right
  mouse to aim was also holding the controller's fire trigger, so automatic
  weapons emptied themselves; the handgun and shotgun were unaffected, which is
  why this survived to release. Right mouse now aims and nothing else.
- **Exit Game → Yes now quits to the desktop** instead of doing nothing.
- **Permanent freeze on the loading screen on some machines** (seen on an AMD
  Ryzen 5 5500 / RX 6600 system). A kernel semaphore our runtime implemented
  without its maximum let a work counter run away and starve the streaming
  loader. Fixed; the same machine now reaches the title and gameplay.
- **AMD GPUs: the renderer now negotiates its depth format.** The depth format
  the Xbox 360 uses isn't sampleable on AMD cards; the renderer now falls back
  to an equivalent one automatically. Required for the game to render at all on
  Radeon.
- **The smoothing seed no longer gets lost.** The shipped list of ~1,400
  graphics pipelines to pre-build was silently replaced by a much smaller
  per-machine list after the first session (or after any session that didn't
  get past loading), bringing back first-encounter pop-in it was meant to
  prevent. The two lists are now merged.
- **Low-core-count machines get a fairer thread budget** (6-core CPUs now get 3
  background workers instead of fewer).

### Added

- **Subtitle language, in the launcher.** All six languages on the disc —
  English, French, Italian, Spanish, Japanese, Korean. Menus and subtitles
  follow your choice; it applies on the next launch.
- **Skip intro logos**, a launcher toggle (off by default). Skips the Capcom,
  Blue Castle and Dolby sequence and goes to the title screen.

### Everything from v1.0.0 still applies

The whole game start to finish, 60 fps, native keyboard/mouse with real key
icons, the restored PC options screen, MSAA 2x, adjustable FOV, real Xbox 360
audio, and a first run that builds what it needs from your own copy of the
game.

**You must own the game.** No Capcom content ships in this repository or in
these downloads.

### How to install

1. Download the build for your system below and unpack it anywhere.
2. Copy your own XBLA package file (~825 MB, no file extension — on the
   console it lives at
   `Content/0000000000000000/58410A8D/000D0000/<long name>`) into the
   unpacked folder's `assets/package/`, or just drag it onto the launcher.
3. Run `cz_runtime.exe` (Windows) or `./cz_runtime` (Linux). The first run
   sets everything up by itself under a progress bar. Later launches start
   straight into the game.

### Requirements

- GPU + driver with **Vulkan 1.3**.
- **Windows** 10+ x86-64, or **Linux** x86-64 with **glibc 2.43 or newer**.
- Your own copy of the Dead Rising 2: Case Zero XBLA package (~825 MB).
- ~2 GB free disk after first-run unpacking.

### Known issues (minor)

- **On AMD GPUs** (tested on an RX 6600): the zombies on the **main menu** may
  flicker in and out — gameplay itself was unaffected in our testing — and a
  **flickering black square** has been seen in-game. Both are actively being
  investigated; NVIDIA is unaffected. Also on AMD, one launch after a GPU
  driver update may sit on a black screen for a minute or two while the driver
  recompiles its pipeline cache — it is not hung, and later launches are fast.
- A subtle **shading flicker on Chuck's hair** in motion; real hardware does
  not show it and it is being tracked.
- The occasional spot may shade slightly differently than the console.
- **Linux glibc floor** (2.43): older distributions refuse to start with a
  `GLIBC_x.yz not found` message. An AppImage-style build is planned.
- **No macOS build yet** — awaits test hardware, nothing structural.

### Legal

This project is not affiliated with, or endorsed by, Capcom or Microsoft.
Dead Rising 2: Case Zero is © Capcom Co., Ltd. The downloads contain the
recompiled program and this project's own runtime/art only; all game content
is read from, or generated at first run from, the player's own copy.
Project code: PolyForm Noncommercial 1.0.0. Third-party licences:
`THIRD_PARTY.md` inside each bundle. Built on hedge-dev's XenonRecomp and
XenosRecomp.

### Checksums (SHA-256)

```
7335141429d331c7b3b81da87beaf5ad9a859531f403a62f87a22a8a79c77fee  CaseZeroRecomp-linux-x86_64.tar.zst
b5a5de41eeb7204b5b008f2acfbf216fb4cd95ce367c514841bc73c5378cc4e5  CaseZeroRecomp-windows-x86_64.zip
```
