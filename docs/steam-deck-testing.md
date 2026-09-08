# Testing on the Steam Deck — what to try, and what to send back

Nobody on this project owns a Steam Deck. v1.0.1 did not run there on either build, and
all we know for certain is why the Linux one did not (its glibc floor was 2.43; SteamOS
is lower). v1.0.2 removes that cause. Whether something else is behind it is what a
Deck owner can tell us in about fifteen minutes, if the run leaves the two files below.

## What you need

- A Steam Deck in **Desktop Mode** for the first run (a terminal shows what is happening;
  Game Mode has no console and a first run that takes a minute looks hung there).
- The XBLA package file from your Xbox 360 (see the README; ~825 MB, no extension).
- **v1.0.2 or later** — v1.0.1's Linux build cannot start on SteamOS, and that is
  already known; testing it again tells us nothing.

## The Linux build (preferred — it is the native path)

1. In Desktop Mode, make a folder, e.g. `~/Games/CaseZeroRecomp/`.
2. Put **`CaseZeroRecomp-linux-x86_64.AppImage`** in it (or unpack the `.tar.zst` there
   instead — both are fine; the AppImage needs no unpacking).
3. Make it executable: right-click → Properties → Permissions → "Is executable", or in a
   terminal `chmod +x CaseZeroRecomp-linux-x86_64.AppImage`.
4. Put your package file in **`assets/package/`** beside it (`~/Games/CaseZeroRecomp/assets/package/<the file>`).
   The AppImage creates the folder on first launch if you would rather drag the file onto
   the launcher window.
5. Open a terminal in that folder (Konsole; right-click the folder → "Open Terminal Here")
   and run:
   ```
   ./CaseZeroRecomp-linux-x86_64.AppImage --diag
   ```
   That prints one line per fact about the Deck and exits. It also writes `cz_diag.txt`
   in the folder. **This alone answers half our questions** (which Vulkan driver, which
   features, which display driver).
6. Then run the game from the same terminal:
   ```
   ./CaseZeroRecomp-linux-x86_64.AppImage
   ```
   The first run unpacks the package and translates ~1,400 shaders under a progress
   window. On a Deck expect **up to a minute** for the shader step (12.5 s on a desktop
   Ryzen restricted to four cores; the Deck's cores are slower). Wait for it.
7. Whatever happens, **`cz_runtime.log`** is in the folder (and `cz_runtime.log.1` is
   the run before). Attach it.
8. If it reaches the title screen: play a few minutes, note the frame rate the window
   title shows, then quit with the launcher/Escape so the log ends cleanly.
9. Only THEN add it to Steam (Games → Add a Non-Steam Game → browse to the AppImage) and
   try Game Mode. Report Game Mode separately from Desktop Mode — they are different
   display paths (Game Mode is gamescope; the log's `[host] gamescope session detected`
   line says which path it took).

## The Windows build under Proton (second — only if you also want to)

1. Unzip `CaseZeroRecomp-windows-x86_64.zip` into `~/Games/CaseZeroRecomp-win/`, put the
   package in its `assets/package/`.
2. Add `cz_runtime.exe` as a non-Steam game, set Properties → Compatibility → force
   Proton (say which version — Proton 9, 10, Experimental, GE).
3. In Properties → Launch Options put `PROTON_LOG=1 %command%`. Proton then writes
   `~/steam-<appid>.log`.
4. Run it once from Desktop Mode. Attach **`cz_runtime.log`** from the folder AND the
   `steam-*.log`.

Known so far: the same zip runs end to end under Wine 11 on the dev box, including the
shader translation, so a Proton failure would be new information, not a repeat.

## What to write in the issue

Open an issue with the **Steam Deck report** template and attach:

- `cz_diag.txt` (or paste the `--diag` output)
- `cz_runtime.log` (and `.log.1` if the run before was different)
- for Proton: the `steam-<appid>.log`
- SteamOS version (Settings → System), and whether it was Desktop Mode or Game Mode

And say **what you saw**, in these words if they fit — the log usually says the rest:

| you saw | say |
|---|---|
| nothing at all, or a window that vanished | "closed" + how long after launch |
| the launcher, then nothing | "closed after the launcher" |
| the progress bar, then nothing | "closed after the progress bar" + which step |
| a black or blank window that stays | "black window" |
| the title screen but unplayable frame rate | "slow" + the fps in the window title |
| it plays | "plays" + the fps and where you went |

"Didn't work" is the one phrase that cannot be acted on.

## What the project will do with it

The `--diag` block settles, in order, the hypotheses in `docs/steam-deck-plan.md` §2:
the driver and its version (H3), which display driver took (H5), the display mode and
any carried-over resolution (H6), the thread budget on four cores (H8). The log's last
lines say where a "closed" run stopped. If the renderer refuses the device, the log
names the missing feature — that line is the whole bug report.
