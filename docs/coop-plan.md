# Adding co-op to Case Zero

Operator ask (2026-09-11): *"add co-op that was not present in the main game … we'll
test it ourselves … if it's impossible in the end we'll just revert back to solo only
with leaderboard working."* So this is a build-and-try, with a clean fallback (solo +
XenonLive leaderboards, which already ship).

Case Zero shipped **single-player only**. But it is the same Blue Castle engine as the
full Dead Rising 2 and Case West, and **the entire co-op layer is linked into the image
and runs at boot** — it was disabled at the frontend, not compiled out. So the work is
not "write netcode"; it is "find the one place the frontend would have kicked the
session, and fire it ourselves", then content-test. This doc is the map.

## What is already done (the kernel/Live half)

The `xlive-integration` branch mirrors Case West's HEAD (commit 7e9662b). Sessions,
sockets, QoS, invites, friends, XNADDR — all transplanted and self-tested against guest
memory. Switches: `CZ_XLIVE_COOP=1` (handle the XGI session messages),
`CZ_XLIVE_ONLINE=1` (be told we are signed in), `CZ_NET_LOG=1`. Signed in to a local
`xlived` (needs `XLIVE_ALLOW_INSECURE=1`) the title reads
`netconfig status: |online|dhcp|ethernet` and opens its UDP socket.

**This is exactly the state Case West was in before its own first two-machine session.**

## The instrument: CZ_ONLINE_LOG (part 1, DONE)

`runtime/kernel/online_log.cpp` taps the title's own online logger — the routine every
co-op subsystem reports through, dropped by a retail build. Without it a refused join is
silence.

- `sub_8255B968(this, level, fmt, ...)` — level 1 error .. 4 chatter. `[title:N]`
- `sub_8255B910(this, level0, fmt, ...)` — same, zero-based. `[title:N*]`
- `sub_82567080(ok, file, msg, line)` — the DESYNC check. `[title:desync]`

`CZ_ONLINE_LOG=N` prints levels up to N (default 3 when set to 1). It is the twin of
Case West's `runtime/kernel/guest_log.cpp` (`sub_8252A030`/`8252A098`), found from the
shared format string `"HW MM session state transition to %s"` (passed to `sub_8255B968`
at 0x825CB3D4). Confirmed working: a signed-in boot prints the full
`cReliableLayer::CreateNBindSocket … 0.0.0.0:5679`, `cP2PConnMesh Constructed`,
`HW MM session: entering LIVE_STATE_IDLE`, `UpdateRecv/UpdateSend` pump.

## The session state machine (the map)

Session object vtable: **`0x8208e474`** (40 entries). The state setter is
**`sub_825CB2A8(this, newState)`** — it logs "HW MM session state transition to %s".

LIVE_STATE enum (name table `0x829DFD40`), the create path in order:

| # | state |
|---|---|
| 0 | IDLE |
| 1 | SETTING_SESSION_CONTEXTS |
| 2 | SETTING_SESSION_PROPERTIES |
| 3 | CREATING_SESSION |
| 4 | QOS_SESSION |
| 5 | SEARCHING_FOR_HOST_SESSION |
| 6 | SEARCHING_FOR_HOST_SESSION_BY_ID |
| 10 | ADDING_LOCAL_GAMER_TO_SESSION |
| 11 | ADDING_REMOTE_GAMER_TO_SESSION |

A **host** walks 1 → 2 → 3 (Case West's log). A **joiner** searches (5/6) then adds
gamers (10/11). This is what we have to make Case Zero's game flow do.

Command dispatcher / session object ctor: `sub_825CD3C0` region; the 36 callers of the
setter are the command handlers (state 1 pushed at 825CD4E4, 825CD65C, 825CDA28,
825CDAC0).

## The lever: the create-coop-session method

`sub_824C0668` — its body holds the assert
`mm_info.GetGameType() == DR2Online::GAME_TYPE_COOP && "can only recreate a coop
session."` (assert string 0x82072090). It is in the game-session **Update**
(`sub_824C23C0` calls it at 824C2484), gated by session state. It reads the online
tunables and the `coop_privacy` profile setting (reader at **824D6E30**,
string 0x82076074). **This is the routine that, driven, makes this build host.**

Online event-type table: `0x829DE580` (triples of filter/enum/display). Relevant:
`EVENT_TYPE_MATCH_MAKING_HOST` (idx 6), `_CLIENT` (7), `_HOST_MIGRATION_RESULT` (9),
`P2P_*`, `SYSTEM_LINK_SEEK_HOST` (21), `HOST_DECLINE_JOIN` (22).

## Online tunables (dataflow-bound, gotcha 241)

Loader `sub_824A2470`; bank based at 0x82A57xxx. The knobs we will want:

| addr | name |
|---|---|
| 0x82A57D00 | disable_online |
| 0x82A57D01 | online_is_enable_systemlink (readers 824C3530, 825C93B8) |
| 0x82A57D02 | online_is_systemlink_host |
| 0x82A57D23 | online_disable_coop_triggers |
| 0x82A57B94 | online_num_players |
| 0x82A57B98.. | online_net_sim_latency / lost_rate / lost_type |
| 0x82A57BB0 | online_log.verbose_level |

These are the debug knobs — `online_net_sim_*` for shaking out desyncs, and the
systemlink pair is the LAN path Case West dropped but Case Zero kept.

## Plan, in order

1. **DONE** — `CZ_ONLINE_LOG`.
2. **Host kick.** Drive `sub_824C0668` (or the command that precedes it) from a new
   `CZ_XLIVE_HOST=1` switch and/or an F4 debug-menu row, so a signed-in boot becomes a
   host. Watch for the 1→2→3 transitions and `hosting session …` from our kernel side.
   The open question is what state the game session must be in first (GetGameType must
   read COOP) — that is the next investigation, driven by the log.
3. **Joiner.** Reuse the invite path already transplanted (`cFESynchronizer::UpdateInvite`
   exists here) or the search path (states 5/6). Two headless instances on this box:
   default `~/.config/XenonLive` (xuid …19) and `XLIVE_DATA_DIR=~/.config/XenonLive-host`
   (xuid …30). Gate on `in session … 2 member(s)` + "Accepted client from".
4. **Content test — the real unknown.** Whether Still Creek's data has P2 spawns and
   whether its missions/cinematics tolerate a second Chuck. Nobody has tried it on this
   map. `online_disable_coop_triggers` + `online_net_sim_*` are the debug knobs.
5. **If impossible:** revert the host/joiner wiring, keep `CZ_ONLINE_LOG` and the whole
   xlive stack for solo + leaderboards.

For Case West this whole doc's shape transfers — it already got past step 3.
