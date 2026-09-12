# Part 115 kick-off — co-op is on master; the release that carries it

**READ `docs/coop-plan.md` "Part 5: the menus, the host's prompt, and a friend's game"
FIRST.** It is the execution record of the late evening of 2026-09-11. This file says
where the release stands and what already exists so it is not rewritten. The work is on
**`master`** now: `xlive-integration` was fast-forwarded into it at `3646327` after the
gates (`--smoke` OK, unlowered switches 0, A5 exit 0 with no co-op variables).

## §0. Where co-op is

Playable end to end by a player with no environment variables: **JOIN CO-OP GAME** on
the main menu → **JOIN XBOX LIVE GAME** (any public game) or **JOIN FRIENDS** (the
game's own friends list; X on a friend joins their game) → the host sees *"<name>
wants to join your game. Let the player join?"* and answers. Hosting rides
`CZ_XLIVE_COOP=1`, which the XenonLive launcher sets (its catalogue already carries
Case Zero as `case_zero`, env prefix `CZ`, artifact names unchanged). The joining
Chuck is dressed (part 4). Content seen normal across parts 4-5: cinematics, a host
save, items, one side quitting, six joins; 0 desyncs. **Still owed by the operator:
a mission transition and a crowd.** Not exposed: the privacy setting (the prompt's
SetToPrivate button is the only way to go private mid-session).

## §1. What already exists — do not rewrite any of it

Everything in `part113-kickoff.md` §1 and `part114-kickoff.md` §1, plus:

| piece | file | switch / control |
|---|---|---|
| the main-menu row + the TitleScreen→JoinGame edge (data) | `tools/patch_coop_menu.py`, ported in `runtime/host/overlay_gen.cpp` (v5) | idempotent; the C++ is byte-identical (`cz_runtime --gen-overlays` + `diff -r`) |
| the co-op outfit rows (data) | `tools/patch_coop_outfit.py`, ported in `overlay_gen.cpp` | same gate |
| the host's join prompt | `runtime/kernel/coop_call.cpp` | `CZ_COOP_JOIN_PROMPT=0` control, `CZ_COOP_CALL_TRACE=1` instrument |
| JOIN FRIENDS → the title's friends screen; X joins the picked friend | `runtime/kernel/coop_friends.cpp` (+ `XliveSession_SetSearchHostFilter`, `CoopJoin_RequestJoinScreen`) | — |
| hosting implied by co-op | `runtime/kernel/coop_host.cpp` | `CZ_XLIVE_HOST=0` opts out |
| the same-box headless pair | host: DebugJump route with `CZ_XLIVE_HOST=1`; joiner: `XLIVE_DATA_DIR=~/.config/XenonLive-host XLIVE_ALLOW_INSECURE=1 CZ_XLIVE_PEER_PORT=3075 … CZ_FAKE_PRESS_SEQ=START,NONE,DOWN,A,NONE,A,NONE,A,NONE,A` | reproduces the join AND the prompt without an operator |
| the release legs with XenonLive | `tools/release_build_oldbase.sh` (static curl+OpenSSL, the launcher's recipe; `-DXLIVE_ROOT`), `tools/release_package_linux.sh` (refuses a dynamic curl), `tools/release_package_windows.ps1` (`libcurl-x64.dll` + `LICENSE.CURL`) | — |

The two test accounts: the host for the friends tests ran as **Chuck Greene**
(`~/.config/XenonLive-host`), who is the laptop's **Frank West**'s only friend; the
default account here (**wivi514**) has no friends, so a friends-only search from the
laptop against a wivi514 host finds nothing — that is the feature, not a defect.

## §2. The release (the operator's instruction: *"push this to master and we'll do the release build"*)

1. Linux old base: `CZ_OLDBASE_SKIP_DEPS=1 tools/release_build_oldbase.sh` (the image is
   rebuilt with `libssl-dev zlib1g-dev`; the static curl lands in
   `thirdparty/oldbase/curl`), then `tools/release_package_appimage.sh`, then both
   clean-container gates at the floor (ubuntu:22.04) — **GATE PASSED** is the pass.
2. Windows: on czwin, `C:\cz\build_cz_xlive.ps1` at the release commit, then
   `tools/release_package_windows.ps1` through `vc.bat`; its gate runs the staged exe.
3. `docs/release-notes-v1.1.0.md` with the three hashes; tag; attach; verify the
   downloads byte-identical from the outside (the v1.0.0 discipline).
4. The XenonLive launcher installs the artifacts by name from the GitHub release —
   nothing to change there.

## §3. Gates still owed

The operator's solo play session on the shipped binary (nothing changed for a player
who never joins anything — every hook is per-event and inert without a session), the
mission transition and the crowd with both in.
