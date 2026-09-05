# Release notes — v1.0.0 (draft, release-github-plan §5.2)

This is the text to paste into the GitHub Release body when the operator creates
it. The SHA-256 lines are current as of the post-§3-fix rebuild (2026-09-05, commit
`74ab694` binaries — the MASH-on-pad string-follow fix); **if either artifact is rebuilt after the §3 operator
sitting, refresh its hash here first** (`sha256sum dist/*.tar.zst` /
`Get-FileHash` on czwin).

---

## Dead Rising 2: Case Zero — Native PC Port v1.0.0

The first public release. The game is **completable start to finish** on both
platforms — this build has been played through end to end.

**You must own the game.** No Capcom content ships in this repository or in
these downloads; the runtime reads everything from your own XBLA package
(quickstart in the README — drop the package in `assets/package/` and run).

### What's in the port, by era

- **Boot → title → gameplay**: the whole XBLA title statically recompiled
  (57,822 PowerPC functions → C++), kernel HLE written against hardware
  captures, honest-failure discipline throughout.
- **Renderer**: Vulkan 1.3, translated Xenos shaders (the disc's own 1,265
  pixel shaders built at first run), EDRAM tiling semantics, cube-map
  snapshots, deferred scoped clears, parallel command recording, and a frame
  that holds 60 fps at 1440p through the heaviest crowds on the dev machine.
- **Audio**: real XMA decoding through ffmpeg — music, speech, effects, and
  the cinematics that gate on them.
- **Save/load**: full round trip, relocated to the per-user directory.
- **60 fps**: the title's own present-interval configuration, surfaced as a
  setting (30/60/90/120/240/480 or off).
- **Keyboard/mouse**: native DR2-PC-style bindings fed to the title's own
  PC input layer (shipped dormant in the 360 build), raw mouse camera,
  our-own-art key-cap prompt icons with live device-follow, and a
  player-editable `kbmap.txt`.
- **The PC options screen**: revived from the dormant layout the 360 build
  ships — resolution (720p–5K, applies live), display mode, vsync, shadow
  quality. **MSAA 2x** is the default; `CZ_VK_MSAA=0` restores single-sample.
- **First run**: fully self-contained — in-process package extract, disc
  shader build, and generation of the patched menu/prompt assets from your
  data, under one progress window. A pipeline pre-warm seed makes even the
  first session smooth.

### Requirements

- GPU + driver with **Vulkan 1.3** (dynamic rendering).
- **Windows** 10+ x86-64, or **Linux** x86-64 with **glibc ≥ 2.43**.
- Your own copy of the Dead Rising 2: Case Zero XBLA package (~825 MB).
- ~2 GB free disk after first-run unpacking.

### Known limitations

- **Linux glibc floor** (2.43): older distributions refuse to start with a
  `GLIBC_x.yz not found` message. An AppImage/old-base build is planned.
- **No macOS** yet (test hardware, not architecture — an ARM64 path exists).
- A subtle **hair-shading flicker** on Chuck in motion
  (`docs/hair-flicker-part92.md`) — real hardware does not show it; open.

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
59b8994d699e488e31cd770b9952a256d9bd5ee864fe4dcf76a762175077fd18  CaseZeroRecomp-linux-x86_64.tar.zst
f0f97e56f29854013f5808eec5d839c5c7fc606b607503b591730597c730a919  CaseZeroRecomp-windows-x86_64.zip
```
