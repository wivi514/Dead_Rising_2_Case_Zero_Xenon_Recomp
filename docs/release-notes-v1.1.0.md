# Release notes — v1.1.0

**This is the text to paste into the GitHub Release body.** Binaries are commit
`aaf3d1f` (all three artifacts built from that source on 2026-09-11; the notes commit
after it changes no code). It carries co-op plan parts 1-5: two-player online co-op
over XenonLive, and the XenonLive account, achievements and friends underneath it.

**The release is frozen at the tag**: if any artifact is EVER rebuilt, refresh its hash
below before attaching.

---

**Two-player online co-op**, the feature the Xbox build carried in its code and never
showed in its menus. It works the way Dead Rising 2's does: one player hosts by
playing, the other joins from the main menu, and the host is asked before anyone comes
in. It runs over **[XenonLive](https://github.com/wivi514/XenonLive)** — sign in
through the [XenonLive launcher](https://github.com/wivi514/XenonLive_Launcher), which
installs this build for you and starts it signed in.

**Upgrading from v1.0.2:** unpack over your existing folder, or let the launcher
install it — saves and settings live outside the game folder and are untouched. Your
unpacked game data and shader cache are reused; the first launch spends a few seconds
regenerating the menu files (the main menu gained a row).

### Co-op

- **JOIN CO-OP GAME** is on the main menu, under START GAME. It opens the game's own
  Join Game screen:
  - **JOIN XBOX LIVE GAME** — finds any joinable public game and joins it.
  - **JOIN FRIENDS** — opens the game's friends list (the same one Dead Rising 2 has),
    with each friend's presence. Move to a friend and press **X** to join their game.
    A friend who is not in a joinable game gets "Join Session Failed!".
- **Hosting is automatic.** When you are signed in and playing, your game is joinable
  by other players. When someone asks to join you see *"<name> wants to join your game.
  Let the player join?"* with **Yes**, **No** and **Set to private** — the last one
  declines and makes your session private for the rest of it. Nobody enters without
  your Yes.
- **The second player spawns dressed** in Chuck's starting outfit (the Xbox data has no
  row for the co-op partner's torso; one was added).
- What has been played through in co-op so far: cinematics, saving on the host,
  picking up and dropping items, one side quitting (the host's game continues). Not
  yet exercised with two players: a mission transition and a big crowd — if either
  misbehaves, report it with both players' `cz_runtime.log`.
- Playing **solo** is exactly as before. If you start the game outside the launcher,
  or signed out, there is no session, no hosting and no prompt; every co-op path is
  inert until a session exists. To be signed in but never host, set `CZ_XLIVE_HOST=0`.

### XenonLive underneath

- Your **gamertag and achievements** are real: unlocking an achievement in game records
  it on your XenonLive account, and the launcher shows it.
- **Friends and presence**: the friends list in game is your XenonLive friends list,
  and it shows what they are playing.
- **Invites** from the launcher reach the game (the plumbing Case West established),
  but that road is untested on this title; joining from the menus is the one this
  release was tested on.

### Requirements

- GPU + driver with **Vulkan 1.3**.
- **Windows** 10+ x86-64, or **Linux** x86-64 with **glibc 2.35 or newer** (the AppImage
  additionally needs FUSE, as every AppImage does; without it run it with
  `--appimage-extract-and-run`).
- Your own copy of the Dead Rising 2: Case Zero XBLA package (~825 MB).
- ~2 GB free disk after first-run unpacking.
- For co-op: a XenonLive account (free, in the launcher), and both players on this
  version.

### How to install

1. Download the build for your system below and unpack it anywhere (the AppImage needs
   no unpacking: `chmod +x` it) — or install it from the XenonLive launcher, which
   also signs you in.
2. Copy your own XBLA package file (~825 MB, no file extension — on the console it
   lives at `Content/0000000000000000/58410A8D/000D0000/<long name>`) into
   `assets/package/` (beside the executable, or beside the AppImage after its first
   launch), or just drag it onto the launcher.
3. Run `cz_runtime.exe` (Windows), `./cz_runtime` (Linux tarball) or the AppImage. The
   first run sets everything up by itself under a progress bar. Later launches start
   straight into the game.

### Everything from v1.0.2 still applies

The whole game start to finish, 60 fps, native keyboard/mouse with real key icons, the
restored PC options screen, MSAA 2x, adjustable FOV, real Xbox 360 audio, the glibc
2.35 floor and the AppImage on Linux, and a first run that builds what it needs from
your own copy of the game.

**You must own the game.** No Capcom content ships in this repository or in these
downloads.

### Known issues (minor)

- Unchanged from v1.0.2: on AMD GPUs a flickering black square can appear in game
  (alt-tab out and back clears it); the main-menu zombies may flicker on that GPU.
- Co-op: the session privacy setting is not in the options yet — the only way to go
  private is the prompt's "Set to private" when someone asks to join. A friend's
  private game cannot be joined from the friends list (it is not searchable); an
  invite is the way in, and invites from the launcher are new ground.
- The Linux builds link a static libcurl/OpenSSL for XenonLive; the launcher hands
  the game the system's CA bundle (`XLIVE_CA_FILE`). A bare launch on an unusual
  distribution that keeps its CA bundle somewhere non-standard can fail to sign in
  while the launcher succeeds — start the game from the launcher.

### Downloads

| file | sha256 |
|---|---|
| `CaseZeroRecomp-linux-x86_64.tar.zst` | `513a4346bb32534e94182dce2fe7cd2b5e9be18e86d79ebad4d06b26dae20ab8` |
| `CaseZeroRecomp-linux-x86_64.AppImage` | `ce5314abc78df137372ebc84dc2292f29897c9e24d31b13c8c47ca7369b6860e` |
| `CaseZeroRecomp-windows-x86_64.zip` | `d152cbcdc7438678ebef3f2fd4795077cefed27e37b0aab9e80d76f6a8142248` |
