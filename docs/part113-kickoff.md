# Part 113 kick-off — co-op part 4, where part 3 left it

**READ `docs/coop-plan.md` "The joiner, and the first two-machine sessions" FIRST.** It
is the execution record of 2026-09-11 and the whole state. This file says only what to
do next and what already exists so it is not rewritten. The work is on branch
**`xlive-integration`** (25 commits ahead of master); master has none of it.

## §0. CO-OP WORKS. Two machines, two Chucks in Still Creek, operator-played.

Host on the Linux box (`CZ_XLIVE_HOST=1`), joiner on the Windows laptop czwin
(`CZ_XLIVE_JOIN=1`), both on the operator's own XenonLive. Three rounds; the last two
joined on the first attempt in ~10 s and played until the operator quit. The operator's
words: *"coop works I am in the game"*. Part 3's plan item 4 (the content test) is
therefore STARTED, not owed — Still Creek takes a second Chuck.

## §1. What already exists — do not rewrite any of it

| piece | file | switch / control |
|---|---|---|
| the title's online logger, tapped | `kernel/online_log.cpp` | `CZ_ONLINE_LOG=N` |
| host lever (IS-COOP byte + the flags word 0x706 → 0x42F) | `kernel/coop_host.cpp` | `CZ_XLIVE_HOST=1` |
| the object resolver both arms share | `kernel/coop_objects.h` | — |
| joiner: opens GameSelect mode 1 through the frontend manager, main menu only, retries when the walk returns to IDLE | `kernel/coop_join.cpp` | `CZ_XLIVE_JOIN=1` (`=call` for the bare call), `_AFTER_MS`, `_RETRY_MS`, `_TRIES` |
| the connection listener in Case West's form (Case Zero's release byte NULLs the endpoint list; the host crashed at guest 0x80 on its first client) | `kernel/coop_transport.cpp` | `CZ_COOP_LISTENER_STOCK=1` runs the crash |
| a gateway drop is held before the title hears "signed out" (the hourly token refresh ended every session at 60-120 s) | `kernel/xlive_glue.cpp` | `CZ_XLIVE_SIGNIN_GRACE_MS` (30000; 0 = old) |
| `DebugTunables_FrontendManager()` — the transition manager the title's first screen change captured | `cpu/debug_tunables.cpp` | — |
| outfits.csv `OUTFIT_COOP_DEFAULT` chest → `default`, both archives, in the patched overlay | `tools/patch_coop_outfit.py` | a MEASURED NULL — see §2.1; leave it or revert it, but do not re-run it as a fix |
| the laptop's launchers | `C:\cz\play.bat` (JOINER, guest diag ON), `C:\cz\play_solo.bat` (the old one), `C:\cz\build_cz_xlive.ps1` (configure+build with XenonLive/curl), `schtasks /run /tn cz_play` | the exe needs `libcurl-x64.dll` beside it |
| the screenshots | `~/DR2CZ-troubleshooting/coop/` (+ INDEX.md) | — |

The DebugJump host route for headless work: `NONE,START,NONE,F2,WAITJUMP,DOWN,A,…` (F2
after the menu is up); the CLAUDE.md order flaked 3 of 3 on 2026-09-11.

## §2. What to do next, in order

### 1. The joining Chuck's chest piece (the operator's first complaint)

Facts, all measured on 2026-09-11: missing on BOTH machines; only the chest — head and
hands are placed correctly (the operator corrected a misreading of the screenshot
twice: look at `~/DR2CZ-troubleshooting/coop/*.png` knowing that); changing clothes on
the client makes it appear; the client sends seven `tEventOutfit` messages and the host
logs `FLOW_COMMAND_DONE_OUTFIT_TRANSFER - RESULT_SUCCESS`; the `OUTFIT_COOP_DEFAULT` row
of outfits.csv names `chest_champions_jacket2`, which no archive holds, BUT patching it
to `chest_default` changed nothing, and the jeans the client wears are not `leg_default`
(no such file) — so the client's spawn pieces are not that row as read.

**Instrument, do not guess again.** Hook `sub_82167428(db, idx)` (returns the outfit
entry: 0x11C bytes each from db+0x88, 51 max) and `sub_82167480(name)` (find by name),
print index/name and the caller on the joiner; and decode the seven outfit events (the
sender is `POUT: Client:tEventOutfit` in the guest log; `CZ_NET_LOG=1` shows the packets
but not the fields). The clothing database rows are `clothingdatabase.csv`
(`tools/big_list.py assets/game/data/datafile.big --extract …` + `tools/big_decompress`);
`chest_*` models live in `data/models/npcs.big`. The question is which chest piece the
client asks for at spawn and why the renderer/loader produces nothing for it.

### 2. The host's missing "incoming co-op call" notice

DR2 shows *"Incoming co-op call…"* (str 11546) and answers on D-pad RIGHT; here the host
sees nothing and the join completes without any answer (the operator found the call in
the pause menu by accident). `sub_824BDBE8` formats str 11533 (*"%s wants to join your
game. Let the player join?"*) and posts it as event 0x82A690F4; `sub_824C24A0` raises
`ON_HOST_CONFIRM_COOP_JOIN` / consumes `ConfirmToAcceptClient` only in game states 5/7.
Whether Case Zero's HUD lacks the element or a release-byte site kills the trigger is
unmeasured. Low priority: the session works without it.

### 3. The content test proper

With both in: a mission transition, a cinematic, a save on the host, a crowd
(`[title:desync]` lines are the DESYNC check, `CZ_ONLINE_LOG=1` prints them), the client
picking up / dropping items, one side quitting (the host's leave handling was seen once:
remote gamer removed, session re-hosted, game continued). `online_disable_coop_triggers`
(0x82A57D23) and `online_net_sim_*` (0x82A57B98..) are the title's own knobs.

### 4. Only if co-op is to SHIP (the operator's decision, plan item 5 is the fallback)

Panel rows for host/join and the privacy setting (`sub_825C61B0` privacy 0/1/2 → 0x42F /
0x827 / 0x227); the joiner's screen from a menu row rather than an env var; the
`overlay_gen.cpp` port of any data patch that turns out to be real; a Windows bundle
that carries `libcurl-x64.dll`; and the libxlive side of the token refresh (refresh
BEFORE expiry so the gateway never sees a 401 — the runtime grace is a workaround).

## §3. Gates

Run on the shipped binary at part 3's close: `--smoke` OK. Owed before merging the
branch: A5 (`tools/kernel_call_diff.py … --include-high-frequency`, must stay exit 0
without the co-op env vars set — every new hook is inert without them, but say so with a
number), `find_unlowered_switches.py`, and a solo play session to show nothing changed
for a player who never joins anything.
