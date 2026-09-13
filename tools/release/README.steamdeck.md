# Dead Rising 2: Case Zero — native PC recompilation (Steam Deck build)

This is a native port of the Xbox 360 XBLA title *Dead Rising 2: Case Zero*
(Capcom / Blue Castle Games, 2010), produced by static recompilation. It is not an
emulator: the game's code was translated ahead of time and runs directly on your CPU,
with the Xbox 360's GPU commands translated to Vulkan.

**This build ships no game data.** You supply your own copy of the game, and the first
run turns it into everything else it needs.

## What is different about this build

It is v1.1.0 with three changes, all of them for the Deck (the same three the v1.0.2
Steam Deck build made):

* **No launcher.** The desktop build opens a small settings/install window first. On
  SteamOS that window is where the v1.0.2 crash a player reported happened, so this build skips it and
  goes straight to the game. Everything the launcher offered — resolution, display
  mode, shadows, FOV, language — is in the in-game settings menu instead.
* **Starts at 1280x800**, the Deck's panel on every model, borderless — instead of
  the desktop build's 1280x720 windowed. This is a **default, not a pin**: it applies
  when there is no settings file yet (the first run), and from then on the RESOLUTION
  and DISPLAY MODE rows in the in-game settings menu are yours — drop below native for
  frame rate, or pick 1920x1080 when the Deck is docked to a 1080p screen (the row
  lists the modes of the display the game is on). The v1.1.0 build before this one
  pinned 1280x800 and the row did nothing; that pin is gone.

  If you carried a settings file over from a PC install and the game comes up at your
  monitor's resolution, set it once in the settings menu; or set `CZ_VK_RES=1280x800`
  in `cz_defaults.env` to pin it again (the startup log then says
  `internal resolution 1280x800 from CZ_VK_RES (env wins over ...)`).
* **It uses SteamOS's own C++ runtime** instead of carrying its own copy. The bundled
  copy is older than the one Mesa expects and could be reaching the AMD graphics driver
  ahead of it — a suspected cause of the crash, and the reason this build does not
  bundle it. This is safe on SteamOS specifically, whose copy is newer than ours.

If this build works and the desktop one does not, please say so — it tells us which of
the three mattered, and the fix goes into the next release for everyone.

## Online and co-op on the Deck

The game goes online — gamertag, achievements, friends, two-player co-op — only when
it is started by the [XenonLive launcher](https://github.com/wivi514/XenonLive_Launcher),
which is a separate program (not the small settings window this build removes).
Started from Steam or a terminal as described below, it is the offline default
profile: the full single-player game, nothing online. Saves are per profile, under
`~/.local/share/Dead Rising 2 Case Zero/`. In co-op, the military arrival at the end
of the game is single-player for now: the second player is dropped there rather than
crash the host (a fix is in progress).

## Requirements

* A Steam Deck (LCD or OLED) on SteamOS 3.x, or any x86-64 Linux with a working
  **Vulkan** driver and a reasonably current distribution.
* Your own copy of the **Dead Rising 2: Case Zero** XBLA package (title ID `58410A8D`).
  It is the ~825 MB file your Xbox 360 downloaded; on the console's storage it lives at
  `Content/0000000000000000/58410A8D/000D0000/<a long hash, no file extension>`.
* A controller (the Deck's own counts) or keyboard and mouse. Both are first-class, and
  on-screen prompts switch between key and button art depending on which you touched
  last.

## Quick start

Do this in **Desktop Mode** the first time — the first run has to unpack 825 MB and
prepare shaders, and it is easier to watch in a terminal.

1. Extract this archive somewhere with ~3 GB free. The Deck's internal drive or a
   microSD both work.
2. **Copy your XBLA package file into `assets/package/`** inside the extracted folder.
   The file has no extension; the name does not matter. (The desktop build lets you
   drag it onto the launcher window — with no launcher here, this is the way.)
3. Run `./cz_runtime` — from a terminal, or by double-clicking it in Dolphin.
4. The first run sets everything up, once, with progress shown as it goes:
   * unpacks the package (~825 MB in, ~825 MB out),
   * prepares the game's shaders from its own disc data (pixel shaders, and the
     vertex shaders through `vs_recipes.bin`),
   * generates the patched menu and key-prompt assets from your own game data,
   * builds the graphics pipelines in the background while the game starts, so the
     first session plays like the second.
5. Subsequent launches skip all of that and start straight into the game.

### Adding it to Game Mode

Once it runs in Desktop Mode: right-click `cz_runtime` -> *Add to Steam*, then switch
to Game Mode and launch it from your library. Steam Input will treat it as a controller
game. Do not set a launch resolution or a Steam per-game scaling option — this build
starts at the Deck's native 1280x800, and the in-game settings menu is where to change it.

## If it still crashes

That is the thing we most want to hear about, and there are three files that answer it
without a debugger. Run from a terminal in the game's folder:

```
./cz_runtime --diag                      # writes cz_diag.txt, prints your GPU/driver
./cz_runtime                             # let it crash
LD_DEBUG=libs ./cz_runtime 2> ld.log     # the tail names the library it died loading
```

Then send **`cz_runtime.log`** (written next to the executable; the previous run is
kept as `cz_runtime.log.1`), **`cz_diag.txt`** and **`ld.log`**. Say what you saw —
"closed after the progress bar", "black screen", "never opened a window" — rather than
"didn't work"; the logs usually supply the rest.

Two quick things worth trying first, and worth reporting either way:

| try this | what it tells us |
|---|---|
| `CZ_LAUNCHER=1 ./cz_runtime` | puts the launcher back. If this crashes and the plain run does not, the launcher was the whole problem. |
| `CZ_VKDRAW=0 ./cz_runtime` | starts with no renderer at all — a blank window, on purpose. If even this crashes, the fault is before the graphics. |

`coredumpctl` will not have anything: SteamOS does not route core dumps to it by
default. Do not spend time on that.

## Troubleshooting

All of these are environment variables — run e.g. `CZ_FPS_CAP=30 ./cz_runtime`.
Defaults for a shipped build come from `cz_defaults.env` next to the executable (a
plain text file you can edit); anything you set in the environment overrides it.

**Wrong-looking or missing graphics** — try these one at a time, and if one fixes it,
please report that along with your GPU and driver:

| variable | what it bisects |
|---|---|
| `CZ_VK_NO_BIND_BATCH=1` | turns off batched vertex-buffer binding |
| `CZ_VK_NO_DEVICE_PFN=1` | turns off the direct driver-function table |
| `CZ_VK_VALIDATION=1` | runs the Vulkan validation layer and prints what it finds |

**Stutter in the first minutes of a session** is mostly shader/pipeline warm-up and
fades as the caches fill. It should be far milder from the second launch on. If it
never fades: `CZ_VK_NO_PREWARM=1` disables the pipeline pre-warm as a test.

**Performance**: the game's own frame pacing targets 30 fps on the 360;
`CZ_FPS_CAP=60` runs the mode the game itself ships for higher refresh. Lowering the
resolution in the settings menu helps most in crowds.

**Sound**: `CZ_NO_AUDIO_OUT=1` disables audio output entirely, `CZ_NO_XMA_DECODE=1`
disables the decoder — useful to tell a sound problem from a game problem when
reporting an issue.

**Starting over**: delete `assets/game/` and/or `assets/shader_spv/` and the first-run
steps run again — your saves are unaffected, they live in the saved-games location
above. Deleting THAT folder removes your saves and settings; the game never does this
itself.

**Reporting a bug from inside the game: press F9.** The game writes a capture — the
frame on screen, the log for the 60 s before and 15 s after, and your machine (OS, CPU,
GPU, driver, settings) — that the XenonLive launcher's **Issues** tab lists. Open the
launcher, write what happened, and Send; or Delete it. Nothing leaves your machine
until you press Send. F8 does the same with three frames a half-second apart, for
something that moves. The folder is bounded (the oldest captures are deleted when it
passes 256 MB), and `CZ_BUG_REPORTS=0` turns the keys off.

**A log of everything** is written to `cz_runtime.log` next to the game folder's
`assets/`, and the previous run's log
is kept as `cz_runtime.log.1`. When reporting a problem, attach that file. To describe
your machine — OS, GPU, driver, which Vulkan features it has, which display driver the
window uses — run `cz_runtime --diag` (Windows: `cz_runtime.exe --diag` from a command
prompt): it prints one line per fact, writes the same to `cz_diag.txt`, and exits.
Paste both into the issue. Say what you SAW — "closed after the progress bar", "black
window", "slow" — rather than "didn't work"; the log usually says the rest.

## What is in this bundle

* `cz_runtime` / `cz_runtime.exe` — the game: recompiled code plus the host runtime.
* `lib/` — bundled libraries (SDL2, an LGPL ffmpeg build for the 360's XMA audio, the
  DirectX Shader Compiler used to translate shaders). Licenses are alongside, and
  `THIRD_PARTY.md` lists everything with provenance. Unlike the desktop build this
  one does not bundle libstdc++/libgcc — SteamOS supplies them.
* `tools/extract_stfs.py` — a reference unpacker; the runtime normally unpacks your
  package itself, this is for doing it by hand (`python3 tools/extract_stfs.py -h`).
* `cz_defaults.env` — default settings applied when not set in your environment.
  In this build it sets `CZ_LAUNCHER=0` (no launcher). Set it back to `1` for the
  launcher. Add `CZ_VK_RES=WxH` to pin the internal resolution (the in-game row then
  does nothing while the line is there).

The Vulkan loader is deliberately *not* bundled — your GPU driver supplies it.

## Legal

This project ships no game content and cannot supply any; it loads the package you
own. The recompilation and runtime are licensed PolyForm Noncommercial 1.0.0 (see
`LICENSE`); third-party components and their licenses are listed in `THIRD_PARTY.md`.
