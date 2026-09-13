# Release notes — v1.1.0

**This is the text to paste into the GitHub Release body.** Binaries are the tag
`v1.1.0` = `fede8e1` + this notes commit (all FOUR artifacts built from it on 2026-09-13 — the co-op
fixes of the night of the 12th/13th: the five-minute session end, the save-less
joiner, the voice endpoint, the sign-in grace, the F8/F9 bug reports; the hashes below
are the current ones). It carries co-op plan parts 1-6: two-player online co-op over XenonLive, the XenonLive account, achievements
and friends underneath it, the ringing walkie-talkie, part 118's thread placement and
the Windows timer fix — and the military-arrival limitation stated under Co-op.

**The release is frozen at the tag**: if any artifact is EVER rebuilt, refresh its hash
below before attaching.

---

> **To play online — gamertag, achievements, friends, co-op — start the game from the
> [XenonLive launcher](https://github.com/wivi514/XenonLive_Launcher).** It signs you
> in, installs this build for you and starts it. Started any other way, the game is
> offline: the full single-player game, nothing online.
>
> **Co-op invites are not working properly yet in this game.** To join someone, use
> **JOIN CO-OP GAME** on the main menu (JOIN XBOX LIVE GAME, or JOIN FRIENDS and X on
> the friend) — not an invite from the launcher.

**Two-player online co-op**, the feature the Xbox build carried in its code and never
showed in its menus. It works the way Dead Rising 2's does: one player hosts by
playing, the other joins from the main menu, and the host is asked before anyone comes
in. It runs over **[XenonLive](https://github.com/wivi514/XenonLive)** — sign in
through the [XenonLive launcher](https://github.com/wivi514/XenonLive_Launcher), which
installs this build for you and starts it signed in.

**Upgrading from v1.0.2:** unpack over your existing folder, or let the launcher
install it — settings live outside the game folder and are untouched. Your unpacked
game data and shader cache are reused; the first launch spends a few seconds
regenerating the menu files (the main menu gained a row). **Saves are now per
profile** — read the XenonLive section before you look for yours.

### Co-op

- **JOIN CO-OP GAME** is on the main menu, under START GAME — and it is the way to
  join in this release, because invites do not work properly yet. It opens the game's
  own Join Game screen:
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
  row for the co-op partner's torso; one was added) — including a brand-new account
  with no save yet, which the first v1.1.0 build showed as an invisible Chuck.
- **A session no longer ends by itself five minutes in.** The game flushes its
  stats five minutes after a session starts; the first v1.1.0 build answered that with
  an error and the host's session closed, dropping the second player every time.
  Tested past ten minutes on two machines.
- **In-game voice chat is a silent channel**: the game registers you in chat (it used
  to fail and log about it every frame) but no audio is carried — use Discord or
  party chat.
- **F9 (or F8) captures a bug report** — the frame, the last 60 s of log and 15 s
  after, and your machine — into the XenonLive launcher's Issues tab, where you write
  what happened and Send (or Delete). Nothing leaves your machine until you press Send.
- **When someone asks to join, the walkie-talkie rings** — a notice says to press
  **RIGHT on the D-pad** (**Right Arrow** on the keyboard) to answer, as Dead Rising 2
  does; the question follows.
- What has been played through in co-op so far: cinematics, saving on the host,
  picking up and dropping items, one side quitting (the host's game continues), and
  the mission transition. Not yet exercised with two players: a big crowd — if it
  misbehaves, report it with both players' `cz_runtime.log`.
- **THE MILITARY ARRIVAL (the end of the game) IS SINGLE-PLAYER FOR NOW.** When the
  story reaches the part where Chuck is put on the motorcycle to flee the military,
  the second player is dropped from the session and the host carries on alone. In
  our tests the host crashed there with two players — that scene puts the partner
  somewhere the game then removes — so for this release we chose to disconnect the
  second player at that point rather than crash the host. A fix that keeps both
  players in for the ending is being worked on.
- **Steam Deck:** take `CaseZeroRecomp-steamdeck-x86_64.tar.gz`. It starts without the
  settings window (the launcher that crashed on SteamOS in v1.0.2) and comes up at the
  Deck's 1280x800 on the first run; from then on the in-game settings menu's
  RESOLUTION row is yours (1920x1080 when docked to a 1080p screen, lower for frame
  rate) — v1.0.2's Deck build pinned it, this one does not. Its README says how to
  put the package in and how to go online through the XenonLive launcher from Desktop
  Mode.
- Playing **solo** is exactly as before. Outside the launcher there is no session, no
  hosting and no prompt; every co-op path is inert until a session exists. To be
  signed in but never host, set `CZ_XLIVE_HOST=0`.

### XenonLive underneath

- **Online is through the launcher, only.** Start the game from the XenonLive launcher
  and you are signed in: your gamertag, achievements, friends, co-op. Start it any
  other way and it is the **default profile, offline** — no account, nothing online,
  the game exactly as v1.0.2 played it.
- **Saves are per profile.** Each XenonLive account has its own save folder, named by
  its gamertag, under the saved-games location (`~/.local/share/Dead Rising 2 Case
  Zero/<gamertag>/` on Linux, `Saved Games\Dead Rising 2 Case Zero\<gamertag>\` on
  Windows); the offline default profile keeps `default/`. **Your existing saves are
  the default profile's**: to carry them onto your account, copy the save folders from
  `default/` into your gamertag's folder once. `CZ_SAVE_DIR` still overrides the lot.
- **The in-game overlay: Shift+Tab.** The launcher's friends, invites and notifications
  over the game, on both platforms — and you can type in it (a friend's name, a
  message); that did not work in the first v1.1.0 build.
- Your **gamertag and achievements** are real: unlocking an achievement in game records
  it on your XenonLive account, and the launcher shows it.
- **Friends and presence**: the friends list in game is your XenonLive friends list,
  and it shows what they are playing.
- **Invites do not work yet** in this release — sending one from the launcher and
  joining by it was tried and does not connect. Join from the menus (JOIN CO-OP GAME,
  or JOIN FRIENDS + X on the friend); that is the road this release was tested on.

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

### Known issues

Reported by players on the v1.1.0 release thread — noted, and on the list for a
future release unless said otherwise:

- **Steam controller (Steam Input) does not work.** The game reads controllers
  through SDL, which does support it; something in the Steam Input path is not
  reaching the game. Until it is fixed, use the controller directly (Steam Input off
  for the game) or a keyboard.
- **Black textures and boxes** when a large amount of fading gore is on screen, and
  when an NPC clips the camera on the right side (seen on an NVIDIA machine).
- **Items spawned floating in the air** (the survivor's money case above her head in
  the gas station, for one). This is Dead Rising 2's own physics misbehaving above
  ~90 fps — the original game does it too at high frame rates — so it is probably
  not fixable here; capping the frame rate in the options avoids it.
- **Mouse scroll is inconsistent** in menus and item cycling; it registers more
  reliably at higher frame rates (~120 fps).
- Unchanged from v1.0.2: on AMD GPUs a flickering black square can appear in game
  (alt-tab out and back clears it); the main-menu zombies may flicker on that GPU.
- Co-op: the session privacy setting is not in the options yet — the only way to go
  private is the prompt's "Set to private" when someone asks to join. A friend's
  private game cannot be joined from the friends list (it is not searchable); an
  invite would be the way in, and invites are not working yet (above).
- The Linux builds link a static libcurl/OpenSSL for XenonLive; the launcher hands
  the game the system's CA bundle (`XLIVE_CA_FILE`). A bare launch on an unusual
  distribution that keeps its CA bundle somewhere non-standard can fail to sign in
  while the launcher succeeds — start the game from the launcher.

### Downloads

| file | sha256 |
|---|---|
| `CaseZeroRecomp-linux-x86_64.tar.zst` | `e1b5a176e707462dd189a9d1a25a7d17eeb726cd5dda19e094cb8f3e762d2462` |
| `CaseZeroRecomp-linux-x86_64.AppImage` | `9c0066c3c2ef49750bfd96d5d3849d52972f6e745969b8072649dd455a19d740` |
| `CaseZeroRecomp-windows-x86_64.zip` | `7a97e90fc287990979769c240b0439cb621063c522f1dc7ffa0c9815e6575b2d` |
| `CaseZeroRecomp-steamdeck-x86_64.tar.gz` | `7c81736367c983df4aacfbb6bd5e1776db0c50141f83aa181a8a535f215477a0` |
