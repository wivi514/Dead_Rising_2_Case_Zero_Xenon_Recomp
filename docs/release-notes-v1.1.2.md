# Release notes — v1.1.2

**PUBLISHED 2026-09-30T06:50:46Z and `Latest`. The four downloads were pulled back over
the public internet after publication and are BYTE-IDENTICAL to the gated originals in
`~/Release/Case Zero/1.1.2/`. Operator-QA'd on two machines before publication.**

**This is the text to paste into the GitHub Release body.** Binaries are the tag
`v1.1.2` (efe50fb + this hashes commit); the hashes below are the artifacts built at
that tree on 2026-09-30, all four legs: Linux tarball + AppImage (clean-container gate
PASSED at the glibc 2.35 floor), Steam Deck (gate PASSED with the system C++ runtime
permitted, as that build is shaped), Windows (staged-exe `--smoke` OK, and the zip's
hash verified identical after the copy from the build box). XenonLive 134d49b / launcher overlay
e59d3ed on BOTH platforms (czwin's checkouts were a release behind and were synced;
the Windows leg was rebuilt and rehashed after).

**The release is frozen at the tag**: if any artifact is EVER rebuilt, refresh its hash
below before attaching.

---

# ⚠️ ONLINE PLAY — READ THIS FIRST

> [!WARNING]
> **DO NOT launch the game executable directly if you want online features.**
>
> Launch the game through the **[XenonLive Launcher](https://github.com/wivi514/XenonLive_Launcher)** for:
>
> **🎮 Online / Co-op · 🏆 Achievements · 👤 Gamertag · 👥 Friends**
>
> Starting the game any other way = **OFFLINE MODE ONLY**.

A fix release. Everything players reported through the launcher's Issues tab since
v1.1.1 is in it, plus the first release that a Case Zero speedrunner's load remover can
actually talk to. Co-op, XenonLive, the launcher-only online rule and per-profile saves
are as in v1.1.0/v1.1.1, unchanged.

**Upgrading from v1.1.1:** unpack over your existing folder, or let the launcher install
it — settings and saves live outside the game folder and are untouched. Your unpacked
game data is reused.

> **First launch after this update does two one-off preparation passes** and then never
> again: **PREPARING SHADERS** (the shader cache's vertex half is rebuilt — the recipe
> changed) and **PREPARING KEY PROMPTS** (the keyboard/mouse prompt art is regenerated).
> Both are the update working, not a hang. Give it a minute.

### Fixed

- **Sliced zombies were two whole bodies.** Cut a zombie in half with a broadsword or a
  paddlesaw and each half rendered as a complete zombie instead of a half. The game
  slices bodies with user clip planes, and the shipped build had never actually had them
  switched on — the fix existed but only ever ran in developer configurations, so no
  player had it. It is the default now.
- **Co-op: a bike part the second player picked up did not count.** Only the host's
  inventory was ever checked, so a part in the guest's hands stayed "not found" on both
  screens and placing it did nothing. Both players count now. The same repair covers the
  other items that are handed in this way, not just bike parts.
- **Co-op: the host dropped whatever he was holding when the guest placed a bike part.**
  The place animation ran on the host's Chuck instead of the guest's.
- **Co-op: the second player could arrive invisible, or without a torso.** The guest's
  clothing is now checked after loading and re-requested if pieces are missing.
- **The picture was washed out when standing in shade.** Standing in a shadow outdoors
  under a bright sky came out overexposed. The game measures scene brightness by reducing
  the frame to a single pixel; above the game's native resolution we were handing the
  exposure controller one corner of that measurement instead of the whole of it, so a
  dark quadrant under a bright sky metered wrong.
- **On ultrawide screens the main menu showed down both sides of the intro logos,** the
  loading card and the area-transition fades. The game draws its full-screen fills with a
  1.2x margin — 360-era TV overscan — and the widescreen patch was shrinking that margin
  along with everything else. Only affects panels wider than 21:9 proper (2.1333:1);
  16:9 and 16:10 were never affected.
- **The How to Play screen showed the right trigger for "Attack"** where an Xbox shows
  the X button. Two prompt icons whose keyboard art is identical (both are left mouse
  button on a PC layout) had become indistinguishable to the code that swaps prompt art
  between pad and keyboard, and one overwrote the other.

### New

- **A status block for speedrunners.** Load removers and auto-splitters can now read
  "am I loading", "am I in a cutscene" and which cutscene played last from a fixed
  address, without pattern-scanning the process. It is on by default and documented at
  [docs/speedrunning.md](https://github.com/wivi514/Dead_Rising_2_Case_Zero_Xenon_Recomp/blob/master/docs/speedrunning.md).
  That document is the contract, and the v1 field layout will not move.
- **An EXPOSURE row in the in-game Visuals panel** (1.0–5.0, default 2.5). This is a
  taste control, not a fix: no single value satisfies both bright daylight and the night
  interiors, so it is yours to set. Highlight brightness is still not exactly right and
  is still being worked on.
- **A TIME OF DAY submenu in the debug menu** (F4, with the debug menu enabled): lock the
  clock, jump to 08:00 / 12:00 / 19:00, or advance it an hour at a time — including the
  mission clock, so the world actually moves with it. For testing and for anyone
  reporting a lighting problem at a particular hour.

### Known limitations

Unchanged from v1.1.1: macOS is not built; the hair on some characters can flicker; and
the Linux artifacts need glibc 2.35 or newer (Ubuntu 22.04 and later, or the AppImage).

### Artifacts

| file | sha256 |
|---|---|
| `CaseZeroRecomp-linux-x86_64.tar.zst` | `8e572a18e55db27ac8923315a30ba7f329d942042db063b12db4329716f306b9` |
| `CaseZeroRecomp-linux-x86_64.AppImage` | `b449f50e2e3e6c666d373735a94b145644ede67248d4739bad603f377510deb5` |
| `CaseZeroRecomp-steamdeck-x86_64.tar.gz` | `d05aac5cafda36e62647f6df1350686d5ca86cf9e62d2dcd5853b556cade5de1` |
| `CaseZeroRecomp-windows-x86_64.zip` | `1c8d5a153317d406a1a8ad4f22ab3069906d5a32228cf7a333b93146976aa10d` |
