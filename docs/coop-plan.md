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

## The lever, as it turned out (part 2, DONE)

Part 1's guess — `sub_824C0668`, the create-coop-session method — was right about
the *routine* and wrong about what was missing. Driven by `CZ_ONLINE_LOG` and a
predicate-by-predicate instrument on it (`runtime/kernel/coop_host.cpp`), the
recreate path turned out to be **fully armed by the title itself**: the session's
IS-COOP byte (`+0x98`, read by `sub_82547920`) is already 1, `GameplayFlow::Enter`
(`sub_82537FA0`, flow table 0x8207A59C) writes `mm_info{host=1, COOP}` and sets the
game session's request byte, and `sub_824C0668` runs every frame from the game
session's Update (`sub_824C2268` at 0x824C2484). A signed-in solo Case Zero game
**already creates a Live session on entering gameplay** — three things stood between
that and a joinable host, and none of them was the frontend:

1. **`XamUserGetSigninState(1)`.** The matchmaking object the create path consults is
   the one for the ACTIVE USER (`online+0x5C`, set by `sub_825C2E20` from the
   frontend's `ProfileChange` event, which the title-screen START handler
   `sub_824D83F0` raises for the pad that pressed START). Headless, the synthetic input
   arm answered every pad index, so one START was four pads, `ProfileChange` fired for
   0 then 1, user 1 became active, and its `XamUserGetSigninState` is 0. Fixed at the
   source: synthetic input is pad 0 only (`kernel/imports.cpp`). A real player could
   never have hit it; a headless gate could never have shown it any other way.
2. **The host's XSession flags — THE one place this build disabled co-op.**
   `sub_825C61B0` (online vt[33], the session-description builder) gives a host
   `0x706` here — PRESENCE|STATS|INVITES_DISABLED|JOIN_VIA_PRESENCE_DISABLED|
   JOIN_IN_PROGRESS_DISABLED, no HOST bit, no PEER_NETWORK — for every privacy value,
   where Case West's (0x82597A0C) switches on privacy: 0 → `0x42F`, 1 → `0x827`,
   2 → `0x227`. The joiner's `0x42E` and system link's `0x21` are untouched, and so is
   everything else on the path. `CZ_XLIVE_HOST=1` rewrites the word after the title's
   own builder runs. The kernel then answers `hosting session <XNKID> (2 public
   slot(s))`.
3. **`XamGetSystemVersion` below 0x200CE900.** The `XSessionGetDetails` wrapper
   (`sub_825F2078`) and `XSessionMigrateHost` (`sub_825F21F0`) refuse with 1627 before
   sending their message; the state machine asks for details right after creating
   (LIVE_STATE_GETTING_SESSION_DETAILS) and deletes the session on the 1627. Case
   West's fix (its 5e14cdb) was address-specific and had not been mirrored; it is now.

With all three, a headless DebugJump run walks **IDLE → SETTING_SESSION_CONTEXTS →
SETTING_SESSION_PROPERTIES → CREATING_SESSION (flags 1071 = 0x42F) →
GETTING_SESSION_DETAILS (1 member) → ADDING_LOCAL_GAMER_TO_SESSION →
`LOGIN_STATE_CONNECTED`**, and stays there (`session already live`). The recipe:

```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_DEBUG_MENU=1 CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 \
  CZ_XLIVE_HOST=1 CZ_ONLINE_LOG=3 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE,NONE \
  timeout 160 ./cz_runtime > /tmp/host.log 2>&1)
grep -E "host session flags|hosting session|LOGIN_STATE_CONNECTED" /tmp/host.log   # three lines
```
The DebugJump route reaches gameplay on roughly half its attempts (the A at 48 s
sometimes lands on the main menu's leaderboard row instead — a stats enumerator in the
log is the tell); retry rather than tune. Known chatter: `User 0 cannot be added to
the chat` every frame — `XamVoiceCreate` is an honest-failure stub and voice is out of
scope; the online log now collapses consecutive repeats.

Retained from part 1, still true: `sub_824C0668` holds the asserts
`mm_info.IsHost() && "only host can recreate session."` (0x820720EC) and
`mm_info.GetGameType() == DR2Online::GAME_TYPE_COOP && "can only recreate a coop
session."` (0x82072090); GAME_TYPE_COOP = 1. `mm_info` is 24 bytes (ctor
`sub_82547008`): +0 u8 IsHost, +4 GameType, +8, +C, +10 u8 systemlink, +14. The six
frontend callers of `sub_824BD960` (the start-session routine) pass TIR matchmaking
(`FindMatch`, gametype 0) or the co-op JOIN (`sub_825026B0`, isHost 0, COOP) — no
host+COOP caller exists in the frontend, which is consistent with the flags finding:
DR2 hosts implicitly on entering gameplay, and Case Zero left that path in with the
joins switched off in one word.

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
2. **DONE — Host kick.** `CZ_XLIVE_HOST=1`; see "The lever, as it turned out". The
   host reaches `LOGIN_STATE_CONNECTED` headlessly with 1 member. No F4 row yet — the
   switch is enough for the two-instance test, and a row belongs with the privacy
   setting once part 4 says co-op is worth shipping.
3. **Joiner.** The join path is intact in this build (`sub_825026B0` passes
   `mm_info{host=0, COOP}`; the joiner's flags 0x42E are untouched). Reuse the invite path already transplanted (`cFESynchronizer::UpdateInvite`
   exists here) or the search path (states 5/6). Two headless instances on this box:
   default `~/.config/XenonLive` (xuid …19) and `XLIVE_DATA_DIR=~/.config/XenonLive-host`
   (xuid …30). Gate on `in session … 2 member(s)` + "Accepted client from".
4. **Content test — the real unknown.** Whether Still Creek's data has P2 spawns and
   whether its missions/cinematics tolerate a second Chuck. Nobody has tried it on this
   map. `online_disable_coop_triggers` + `online_net_sim_*` are the debug knobs.
5. **If impossible:** revert the host/joiner wiring, keep `CZ_ONLINE_LOG` and the whole
   xlive stack for solo + leaderboards.

For Case West this whole doc's shape transfers — it already got past step 3.
