# Release notes — v1.1.1

**DRAFT ON GITHUB (2026-09-17): tag `v1.1.1` pushed, the draft release carries the four
artifacts + SHA256SUMS and this text below the rule as its body; the operator reads and
publishes it on GitHub. The Windows zip downloaded from the draft hashes OK.**

**This is the text to paste into the GitHub Release body.** Binaries are the tag
`v1.1.1` (8efd9d1 + this hashes commit — re-tagged after the night/interior exposure fix
landed; the 2026-09-15 staging build at 626e438 is superseded); the hashes below are
the artifacts built at that tree on 2026-09-16, all four legs: Linux tarball + AppImage
(clean-container gate PASSED at the glibc 2.35 floor), Steam Deck (gate PASSED with the
system C++ runtime permitted, as that build is shaped), Windows (staged-exe gate OK,
XenonLive 1034bcc / launcher overlay 761de27 on both platforms).

**The release is frozen at the tag**: if any artifact is EVER rebuilt, refresh its hash
below before attaching.

---

A fix release for the reports players sent through the launcher's Issues tab after
v1.1.0 (thank you — F9 reports are exactly what found these). Co-op, XenonLive, the
launcher-only online rule, per-profile saves: all as in v1.1.0, unchanged.

> **To play online — gamertag, achievements, friends, co-op — start the game from the
> [XenonLive launcher](https://github.com/wivi514/XenonLive_Launcher).** Started any
> other way, the game is offline: the full single-player game, nothing online.

**Upgrading from v1.1.0:** unpack over your existing folder, or let the launcher
install it — settings and saves live outside the game folder and are untouched. Your
unpacked game data and shader cache are reused.

### Fixed

- **Night and interiors were far too dark** — the safehouse and shops at night were
  black where an Xbox shows a dim, readable room, and interiors by day were dimmer than
  they should be. The game's auto-exposure reads its scene-brightness measurement back
  from the GPU, and this port never delivered it, so the exposure sat at its minimum
  all day long. It adapts now, as it does on a console — interiors are brighter by day
  as well as by night. (Xenia has the same defect with its default settings, which is
  why every comparison against it agreed.)
- **Zombies opaque on the right half of the screen** — a zombie close to the camera
  at screen centre drew translucent-dark on the left half and solid dark on the right,
  with a hard vertical edge between. Two things in how the Xbox's two rendering tiles
  are replayed were wrong (the second tile inherited the first tile's shader for the
  actor pre-pass, and the depth clear that should follow it was landing on the left
  tile both times); both fixed.
- **The gas-station roof turned black** — again, from the safehouse side of the map,
  after v1.0.2's fix. The fix keyed on an address the game reuses; it now keys on the
  shader and the texture, which is what the roof actually is.
- **PP earned were not written to the leaderboard for six minutes** — the game's own
  stats flush ran on a 360-second timer; it now runs within two seconds of a save.
- **The "MASH" prompt in the struggle read English on a pad whatever the language**
  — the string follow read the English bank's offsets even when the game had loaded
  French, Italian, Spanish, Japanese or Korean.
- **Co-op: the partner falling through the map after the host loads a level** — a
  client that spawns before its floor exists is now held at the spawn point instead of
  dropping through it.
- **Co-op: an invisible second player when the guest loads slowly** — a guest with no
  save is dressed by the host in Chuck's default outfit; that used to happen ten
  seconds after the guest joined, and a guest still loading at that moment got
  nothing. The host now dresses the guest one second after the guest reports its
  level is up, however long the load takes.
- **The launcher window and the in-game PC settings panel follow the SUBTITLES
  language** (English, French, Italian, Spanish; Japanese and Korean show those two
  menus in English — the game's own text is localized either way). The launcher
  re-labels itself as you step the row.

### Known issues

- **Chuck's hair flickers** in some lights (reported after v1.1.0). Reproduced; not
  fixed in this release — being worked on.
- **Loud audio** reported once, ~40 minutes into a session — not reproduced yet. If it
  happens to you, an F9 right then is what would find it.
- **JOIN FRIENDS looks like a random search.** Pressing X on a friend joins THAT
  friend's game — the search is kept to their session — but the screens it goes
  through are the JOIN XBOX LIVE GAME ones (the save-slot pick, "searching for host
  session"). If X on a friend does nothing, the friend is not in a joinable game;
  backing out to JOIN XBOX LIVE GAME is a random search.
- Everything listed under v1.1.0's known issues still applies: Steam Input, black
  boxes with a lot of gore on screen, items floating above ~90 fps (cap the frame
  rate), inconsistent mouse scroll in menus, the AMD flickering square, the co-op
  privacy setting, the military arrival being single-player.

### Requirements

- **Windows** 10+ x86-64, or **Linux** x86-64 with **glibc 2.35 or newer** (the AppImage
  additionally needs FUSE, as every AppImage does; without it run it with
  `--appimage-extract-and-run`). **Steam Deck:** `CaseZeroRecomp-steamdeck-x86_64.tar.gz`.
- A **Vulkan 1.2** GPU and driver.
- **Your own copy of the game**: the Xbox 360 XBLA package of Dead Rising 2: Case Zero
  (title ID `58410A8D`), which you put beside the executable in `assets/package/` on the
  first run — or drop onto the launcher window. No Capcom content ships here.

### Downloads

| file | sha256 |
|---|---|
| `CaseZeroRecomp-linux-x86_64.tar.zst` | `af35e7fb10a1456cfd144a31ada8eb446b5ae4ac3c0067a2ac92484faee5513e` |
| `CaseZeroRecomp-linux-x86_64.AppImage` | `9b65f0464d4a2266f451afdd13dbbf7659c79fa28b1a0c4a3d966e69822de0dd` |
| `CaseZeroRecomp-steamdeck-x86_64.tar.gz` | `496d21d01cb11cf7b9078e8cbb2981801128326d55ff49ce347d8091ec879977` |
| `CaseZeroRecomp-windows-x86_64.zip` | `2aaff352a5f3aa252d6df0e721808b4fd2d26b3678625a5e9286b4f6bae6da44` |
