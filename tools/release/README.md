# Dead Rising 2: Case Zero — native PC recompilation

This is a native port of the Xbox 360 XBLA title *Dead Rising 2: Case Zero*
(Capcom / Blue Castle Games, 2010), produced by static recompilation. It is not an
emulator: the game's code was translated ahead of time and runs directly on your CPU,
with the Xbox 360's GPU commands translated to Vulkan.

**This build ships no game data.** You supply your own copy of the game, and the first
run turns it into everything else it needs.

## Requirements

* x86-64 Linux with a working **Vulkan** driver (if `vulkaninfo` works, you are fine).
* Your own copy of the **Dead Rising 2: Case Zero** XBLA package (title ID `58410A8D`).
  It is the ~825 MB file your Xbox 360 downloaded; on the console's storage it lives at
  `Content/0000000000000000/58410A8D/000D0000/<a long hash, no file extension>`.
* A game controller is strongly recommended (anything SDL recognises — XInput layout).
  A keyboard fallback exists; the exact key map is printed in the terminal at startup.

## Quick start

1. Run `./cz_runtime`. The **launcher** opens: pick your display mode, resolution and
   other settings, and **drag your XBLA package file onto the window** to install the
   game (putting it in `assets/package/` by hand works too).
2. Press PLAY.
3. The first run does three things, once, with progress shown as it goes:
   * unpacks the package (825 MB in, ~832 MB out),
   * prepares the game's shaders from its own disc data (1,265 of them, ~10 s),
   * warms up as you play — entering a new area for the first time may translate a
     few more shaders on the fly (a fraction of a second each; the log says
     `first-sight translation` when it happens).
4. Subsequent launches skip all of that and start straight into the game.

Settings (resolution, display mode, shadows) are in the in-game settings menu. Your
save games and settings live **outside this folder**, so reinstalling or deleting the
game can never touch them:

* Windows: `Saved Games\Dead Rising 2 Case Zero\` (next to your Documents folder)
* Linux: `~/.local/share/Dead Rising 2 Case Zero/`

A build that finds saves in the old in-folder location (`assets/save/`) copies them
over automatically on its first launch and leaves the originals as a backup.

## If something is missing

The runtime refuses to start with a message that says what is missing, where it goes,
and what produces it — a black screen is never the intended failure. If it starts but
misbehaves, read on.

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

**A log of everything** is printed to the terminal; when reporting a problem, run from
a terminal and include the output.

## What is in this bundle

* `cz_runtime` — the game: recompiled code plus the host runtime.
* `lib/` — bundled libraries (SDL2, an LGPL ffmpeg build for the 360's XMA audio, the
  DirectX Shader Compiler used to translate shaders). Licenses are alongside, and
  `THIRD_PARTY.md` lists everything with provenance.
* `tools/extract_stfs.py` — a reference unpacker; the runtime normally unpacks your
  package itself, this is for doing it by hand (`python3 tools/extract_stfs.py -h`).
* `cz_defaults.env` — default settings applied when not set in your environment.

The Vulkan loader is deliberately *not* bundled — your GPU driver supplies it.

## Legal

This project ships no game content and cannot supply any; it loads the package you
own. The recompilation and runtime are licensed PolyForm Noncommercial 1.0.0 (see
`LICENSE`); third-party components and their licenses are listed in `THIRD_PARTY.md`.
