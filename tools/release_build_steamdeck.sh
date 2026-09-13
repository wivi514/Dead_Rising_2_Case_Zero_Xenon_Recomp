#!/usr/bin/env bash
# Build the STEAM DECK variant of the Linux release artifact.
#
# WHY THIS EXISTS. A player on SteamOS 3.8 reported the v1.0.2 Linux build (tar and
# AppImage alike) dying with SIGSEGV right after the "[settings]" line, with
# CZ_VKDRAW=0 and with SDL_VIDEODRIVER=x11 making no difference. Reading the boot order
# places that exactly at the LAUNCHER's window: main.cpp runs Settings_Load and then
# Host_RunLauncher before any boot machinery, and the launcher's SDL_CreateRenderer
# (host/window.cpp, right after the ApplyGameIcon call whose "[icon]" line they quoted)
# is the next thing that runs. That call is also the ONE code path no gate here has ever
# exercised: release_gate_clean_container.sh runs CZ_NO_WINDOW=1 CZ_LAUNCHER=0, the
# launcher early-returns on the dummy driver anyway, and this project's dev box is
# NVIDIA — whose GL driver, unlike Mesa's, does not link libstdc++ at all.
#
# So this variant removes the launcher from the player's path entirely rather than
# guessing at what inside it faults. The game window itself creates no SDL_Renderer (it
# carries SDL_WINDOW_VULKAN and the renderer presents its own image), so with the
# launcher gone a default run never asks SDL for a GL context at all.
#
# THREE CHANGES, each revertible on its own, so the report that comes back can name
# which one mattered:
#   1. cz_defaults.env says CZ_LAUNCHER=0                        (CZ_PKG_NO_LAUNCHER)
#   2. settings defaults are 1280x800 fullscreen-desktop, the    (CZ_DECK -> CZ_DECK_DEFAULTS)
#      Deck's native panel, instead of 1280x720 windowed — a
#      DEFAULT for the first run; the in-game settings menu
#      owns it from then on (the pin the first builds carried
#      is retired below)
#   3. libstdc++/libgcc_s are NOT bundled, so Mesa gets SteamOS's  (CZ_PKG_SYSTEM_CXX)
#      newer copy instead of our GLIBCXX_3.4.30 one shadowing it
#
# Change 3 is the only one that could make things WORSE, and only on a distribution
# whose libstdc++ is older than the old base's — which SteamOS 3.8's is not. It is in
# here because change 1 alone would not help if the same shadowing also reaches RADV,
# and a delivery that gets past the launcher only to fault at Vulkan init is no use.
#
# The archive is .tar.gz, not the desktop build's .tar.zst: Dolphin on the Deck extracts
# a .gz by double-click.
#
# Everything else — the container, the old base, the identity gate, the packaging — is
# the normal release path with the switches above set. It writes into dist-steamdeck/,
# so it cannot touch the desktop artifact or its staged tree.
#
# Usage:  tools/release_build_steamdeck.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

export CZ_DECK=ON
export CZ_BUILD_TAG=-deck
export CZ_PKG_SUFFIX=steamdeck-x86_64
export CZ_PKG_TARGZ=1
export CZ_PKG_NO_LAUNCHER=1
export CZ_PKG_SYSTEM_CXX=1
export CZ_PKG_README=$ROOT/tools/release/README.steamdeck.md
export CZ_PKG_OUT=dist-steamdeck
# THE RESOLUTION IS A DEFAULT, NOT A PIN (operator instruction, v1.1.0: "launch the
# first time in 1280x800 but the player can then change resolution in game — a user is
# asking for 1080p"). The v1.0.2 Deck build and the first v1.1.0 Deck build appended
# CZ_VK_RES=1280x800 to cz_defaults.env, which wins over the settings file and so made
# the in-game RESOLUTION row do nothing; a docked Deck could never pick its 1080p
# screen. CZ_DECK's compiled defaults (1280x800 borderless, when there is no settings
# file yet) give the first run its native panel, and after that the settings file is the
# player's. The one case the pin covered — a settings file carried over from a PC
# install — is now one trip to the settings menu, and the README says so.
# (CZ_PKG_EXTRA_DEFAULTS is still honoured for anyone who wants the pin back.)
# The SDL2/ffmpeg/XenonUtils prefixes under thirdparty/oldbase are the desktop build's
# and are variant-independent — rebuilding them would cost 20 minutes and change nothing.
export CZ_OLDBASE_SKIP_DEPS=${CZ_OLDBASE_SKIP_DEPS:-1}

exec "$ROOT/tools/release_build_oldbase.sh" "$@"
